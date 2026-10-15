// SPDX-License-Identifier: GPL-3.0-or-later
// Part of RXDK-360 - see LICENSE.md for the full GNU GPL v3.

using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;
using System.Threading;
using Rxdk.Xbox360.Dwarf;
using Rxdk.Xbox360.Xbdm;

namespace Rxdk.Xbox360.DebugAdapter
{
    /// <summary>
    /// The RXDK-360 devkit debug adapter: it answers Debug Adapter Protocol requests
    /// from the editor by resolving symbols with <see cref="Symbols"/> (the title's
    /// DWARF) and driving the console with <see cref="XbdmClient"/> (xbdm.dll). This
    /// is the glue that makes F5-to-devkit step source: reboot the title stopped,
    /// set breakpoints at source lines, and on a break map the PC back to source and
    /// read locals from the stopped frame.
    /// </summary>
    public sealed class DebugSession
    {
        private readonly DapConnection _dap;
        private Symbols? _sym;
        private XbdmClient? _kit;
        private string _remoteImage = "";
        private string _titleOutputFile = "";
        private uint _stoppedThread;
        private XContext _ctx;
        private bool _haveCtx;
        private bool _holdingInitialBreak;
        private int _paused; // 1 = already reported stopped; ignore extra kit breaks
        private bool _diagDumped; // one fault register/stack dump per stop
        private uint _titleBase, _titleSize;
        private const int LocalsRef = 1000;
        private int _nextChildRef = 1001;
        private readonly Dictionary<int, string> _expand = new();
        private readonly List<(string path, uint addr, int line)> _breakpoints = new();

        // ---- source-line stepping engine ----
        // 0 = idle, 1 = step-over (next), 2 = step-in, 3 = step-out. Driven from the
        // break notification: each break either satisfies the step (surface "stopped")
        // or re-arms and continues. Breakpoints are code traps (proven), never the LR
        // register alone -- LR is clobbered mid-function, which is what sent a step-out
        // running wild into unrelated code.
        private int _stepMode;
        private uint _stepTid;
        private uint _stepStartSp;      // r1 of the frame the step began in
        private int _stepStartLine;
        private string _stepStartFile = "";
        private bool _stepReturning;    // phase 2: a return address is planted, running to the caller
        private bool _stepStopping;     // the pending break should surface as reason "step"
        private readonly HashSet<uint> _stepBps = new();  // kit VAs we planted for the step

        public bool Terminated { get; private set; }

        public DebugSession(DapConnection dap)
        {
            _dap = dap;
            _dap.ProtocolTrace = (dir, json) =>
                _dap.SendEvent("output", new { category = "console", output = dir + " " + json + "\n" });
        }

        public void Handle(DapConnection.Message req)
        {
            try
            {
                switch (req.Command)
                {
                    case "initialize": Initialize(req); break;
                    case "launch": Launch(req); break;
                    case "setBreakpoints": SetBreakpoints(req); break;
                    case "configurationDone": ConfigurationDone(req); break;
                    case "threads": Threads(req); break;
                    case "stackTrace": StackTrace(req); break;
                    case "scopes": Scopes(req); break;
                    case "variables": Variables(req); break;
                    case "evaluate": Evaluate(req); break;
                    case "continue": Continue(req); break;
                    case "next": case "stepIn": case "stepOut": Step(req); break;
                    case "pause": Pause(req); break;
                    case "disconnect": Disconnect(req, terminateDefault: true); break;
                    case "terminate": Disconnect(req, terminateDefault: true); break;
                    default: _dap.SendResponse(req, true); break;      // politely accept unknowns
                }
            }
            catch (Exception e)
            {
                _dap.SendResponse(req, false, message: e.Message);
            }
        }

        private void Initialize(DapConnection.Message req)
        {
            _dap.SendResponse(req, true, new Dictionary<string, object?>
            {
                ["supportsConfigurationDoneRequest"] = true,
                ["supportsTerminateRequest"] = true,
                ["supportsFunctionBreakpoints"] = false,
                ["supportsEvaluateForHovers"] = true,
                ["supportsStepBack"] = false,
            });
            _dap.SendEvent("initialized");
        }

        private void Launch(DapConnection.Message req)
        {
            var a = req.Arguments;
            string program = Str(a, "program");                        // the .xex
            string symbols = Str(a, "symbols");                        // the .elf (default: program.elf)
            if (symbols.Length == 0 && program.Length != 0)
                symbols = Path.ChangeExtension(program, ".elf");
            string console = Str(a, "console");
            if (console.Length == 0)
                console = XbdmClient.DefaultConsole() ?? "";
            bool stopAtEntry = Bool(a, "stopAtEntry", true) && !Bool(a, "noDebug", false);
            string remoteDir = Str(a, "remoteDir");
            if (remoteDir.Length == 0)
                remoteDir = "devkit:\\" + Path.GetFileNameWithoutExtension(program);
            string? xbdm = XbdmClient.LocateXbdm();
            _titleOutputFile = Str(a, "__titleOutputFile");
            _dap.SendEvent("output", new { category = "console", output =
                (xbdm != null ? $"xbdm {xbdm}\n" : "xbdm.dll NOT found (install RXDK-360)\n") });
            if (!stopAtEntry)
            {
                _kit = new XbdmClient { ResumeOnDispose = false };
                _kit.Connect(console);
                string remote = _kit.DeployTitle(program, remoteDir);
                _kit.SetTitle(remoteDir.EndsWith("\\") ? remoteDir : remoteDir + "\\", Path.GetFileName(program));
                _kit.LaunchTitle(remote, stopAtLoad: false);
                _remoteImage = remote;
                _dap.SendEvent("output", new { category = "console", output = $"running {remote}\n" });
                _dap.SendResponse(req, true);
                return;
            }

            _sym = Symbols.Open(program, symbols.Length != 0 ? symbols : null);
            if (_sym != null)
                _dap.SendEvent("output", new { category = "console", output = $"symbols {_sym.Kind} {_sym.Path}\n" });
            else if (symbols.Length != 0)
                _dap.SendEvent("output", new { category = "console", output = $"no symbols next to {program}\n" });

            if (console.Length == 0)
                throw new InvalidOperationException("no console: set launch.console or XenonSDK\\XboxName ($(DefaultConsole)).");

            _kit = new XbdmClient { ResumeOnDispose = false };
            _kit.Notification += OnKitEvent;
            _kit.Connect(console);
            _holdingInitialBreak = true;
            var stop = _kit.LaunchStopped(program, remoteDir: remoteDir);
            _dap.SendEvent("output", new { category = "console",
                output = $"deployed {Path.GetFileName(program)} + {_kit.LastMediaFilesDeployed} media file(s) -> {remoteDir}\n" });
            _remoteImage = "devkit:\\" + Path.GetFileNameWithoutExtension(program) + "\\" + Path.GetFileName(program);
            string titleHint = Path.GetFileNameWithoutExtension(program);
            _kit.TryFindModule(titleHint, out _titleBase, out _titleSize, out _);
            _dap.SendEvent("output", new { category = "console", output = $"launched {_remoteImage} initial={stop} base=0x{_titleBase:X8}\n" });
            _dap.SendResponse(req, true);
        }

        /// <summary>Official VS: after the initial wait, setBreakpoints has run; auto-Go.
        /// The next break (a source BP) is what the user sees. Thread-create START/STOP
        /// while sitting on that BP is not Continue.</summary>
        private void ConfigurationDone(DapConnection.Message req)
        {
            _dap.SendResponse(req, true);
            _holdingInitialBreak = false;
            Interlocked.Exchange(ref _paused, 0);
            try { _kit?.ContinueAll(); } catch { }
        }

        private void SetBreakpoints(DapConnection.Message req)
        {
            var a = req.Arguments;
            string path = a.TryGetProperty("source", out var src) && src.TryGetProperty("path", out var p)
                ? p.GetString() ?? "" : "";
            var results = new List<object>();
            var wanted = new HashSet<uint>();
            var wantedLines = new Dictionary<uint, int>();
            if (a.TryGetProperty("breakpoints", out var bps) && bps.ValueKind == JsonValueKind.Array)
            {
                foreach (var bp in bps.EnumerateArray())
                {
                    int line = bp.TryGetProperty("line", out var l) ? l.GetInt32() : 0;
                    var r = _sym?.ResolveBreakpoint(Path.GetFileName(path), line);
                    if (r != null)
                    {
                        uint addr = TitleAddress.ExeToXex((uint)r.Value.address,
                            _titleBase != 0 ? _titleBase : TitleAddress.DefaultBase);
                        wanted.Add(addr);
                        wantedLines[addr] = r.Value.line;
                        results.Add(new { verified = true, line = r.Value.line, message = $"0x{addr:X8}" });
                    }
                    else results.Add(new { verified = false, line });
                }
            }

            // DAP setBreakpoints replaces every breakpoint for this source. An empty
            // list is how VS clears the file; without RemoveBreakpoint the kit keeps trapping.
            for (int i = _breakpoints.Count - 1; i >= 0; i--)
            {
                var e = _breakpoints[i];
                if (!string.Equals(e.path, path, StringComparison.OrdinalIgnoreCase))
                    continue;
                if (wanted.Contains(e.addr))
                    continue;
                try { _kit?.RemoveBreakpoint(e.addr); } catch { }
                _breakpoints.RemoveAt(i);
            }

            foreach (uint addr in wanted)
            {
                if (_breakpoints.Exists(e => e.addr == addr &&
                    string.Equals(e.path, path, StringComparison.OrdinalIgnoreCase)))
                    continue;
                try { _kit?.SetBreakpoint(addr); } catch { }
                _breakpoints.Add((path, addr, wantedLines[addr]));
            }

            _dap.SendResponse(req, true, new { breakpoints = results });
        }

        private void Threads(DapConnection.Message req)
        {
            EnsureContext();
            var list = new List<object>();
            var seen = new HashSet<int>();
            void Add(uint t)
            {
                int id = DapTid(t);
                if (id <= 0 || !seen.Add(id)) return;
                list.Add(new Dictionary<string, object?> { ["id"] = id, ["name"] = $"thread 0x{t:X8}" });
            }
            if (_stoppedThread != 0) Add(_stoppedThread);
            try
            {
                if (_kit != null)
                    foreach (var t in _kit.GetThreads()) Add(t);
            }
            catch { }
            if (list.Count == 0) list.Add(new Dictionary<string, object?> { ["id"] = 1, ["name"] = "main" });
            _dap.SendResponse(req, true, new { threads = list });
        }

        private void StackTrace(DapConnection.Message req)
        {
            EnsureContext();
            var frames = new List<object>();
            if (_haveCtx)
            {
                DumpFaultDiag();
                var pcs = UnwindPcs();
                for (int i = 0; i < pcs.Count; i++)
                {
                    uint pc = pcs[i];
                    // Frames above 0 report the *return* address; the call that got us
                    // there is the instruction before it, so resolve pc-4 for line info.
                    uint lookup = i == 0 ? pc : pc - 4;
                    var loc = _sym?.LineAtKit(lookup, _titleBase);
                    // Normalize to Windows separators: clang DWARF paths mix '/' into an
                    // otherwise '\' path (e.g. ...\AdvancedLighting/AdvancedLighting.cpp), and
                    // VS then fails to match the open document, so it won't focus the editor on
                    // the stop line (you have to scroll to the breakpoint yourself).
                    string file = (loc?.File ?? "").Replace('/', '\\');
                    string fn = loc is { Function: { Length: > 0 } } ? loc.Value.Function : $"0x{pc:X8}";
                    string srcName = file.Length == 0 ? "unknown" : Path.GetFileName(file);
                    var frame = new Dictionary<string, object?>
                    {
                        ["id"] = i,
                        ["name"] = fn,
                        ["line"] = loc?.Line ?? 0,
                        ["column"] = 1,
                        ["source"] = new Dictionary<string, object?>
                        {
                            ["name"] = srcName,
                            ["path"] = file,
                        },
                    };
                    if (file.Length == 0)
                        ((Dictionary<string, object?>)frame["source"]!)["presentationHint"] = "deemphasize";
                    frames.Add(frame);
                }
            }
            _dap.SendResponse(req, true, new { stackFrames = frames, totalFrames = frames.Count });
        }

        // One-shot fault dump: the raw GPRs at the stop plus a window of the stack, so a
        // crash inside a leaf like memcpy can be traced back to the pointer/length its
        // caller handed it. Emitted to the VS console and the title log on the first
        // stackTrace after a stop.
        private void DumpFaultDiag()
        {
            if (_diagDumped || !_haveCtx || _ctx.Gpr == null || _kit == null) return;
            _diagDumped = true;
            var sb = new System.Text.StringBuilder();
            sb.Append($"FAULTDIAG IAR=0x{_ctx.Iar:X8} LR=0x{_ctx.Lr:X8}\n");
            for (int i = 0; i < 32; i += 4)
                sb.Append($"  r{i,-2}=0x{(uint)_ctx.Gpr[i]:X8} r{i + 1,-2}=0x{(uint)_ctx.Gpr[i + 1]:X8} " +
                          $"r{i + 2,-2}=0x{(uint)_ctx.Gpr[i + 2]:X8} r{i + 3,-2}=0x{(uint)_ctx.Gpr[i + 3]:X8}\n");
            var mem = new KitMemory(_kit);
            // Walk frames, dumping each frame's SP + a stack window. push_back stashes
            // the vector `this` at [SP+96]; the vector layout is {__begin_,__end_,__cap_}
            // at this+0/+4/+8 -- enough to see which pointer is corrupt and where the
            // real buffer is.
            uint sp = (uint)_ctx.Gpr[1];
            uint prevSp = 0;
            for (int frame = 0; frame < 12 && sp != 0 && InStack(sp); frame++)
            {
                if (mem.ReadDword(sp) is not uint back || back <= sp || back - sp > 0x100000)
                { sb.Append($"  frame{frame} sp=0x{sp:X8}: <bad back chain>\n"); break; }
                sb.Append($"  frame{frame} sp=0x{sp:X8} size=0x{back - sp:X}: ");
                for (int i = 0; i < 12; i++)
                    sb.Append($"0x{(mem.ReadDword(sp + (uint)(i * 4)) ?? 0xEEEEEEEE):X8} ");
                sb.Append('\n');
                // push_back stashes the vector `this` at [sp+96]; its caller CopyAttributes
                // has a big frame (712-byte local). When this frame calls into a >=0x300
                // frame, treat it as push_back and dump the vector layout {begin,end,cap}.
                prevSp = sp;
                sp = back;
            }
            // d3d9d caches its valid physical-memory range in these globals; the
            // AssertPhysical path (AddRTListData) rejects any resource address outside
            // it. Dumping them tells us the window d3d expects vs where our GPU heap is.
            foreach (uint g in new uint[] { 0x826061C4, 0x826061C8, 0x826061CC, 0x826061D0 })
                sb.Append($"  D3DGLOBAL[0x{g:X8}] = 0x{(mem.ReadDword(g) ?? 0xEEEEEEEE):X8}\n");
            // Deref any register that points into title data or the GPU/physical heap,
            // so an object (e.g. a D3D texture header, whose GPU base address the
            // resolve/AssertPhysical path reads) can be inspected without a live probe.
            for (int i = 0; i < 32; i++)
            {
                uint v = (uint)_ctx.Gpr[i];
                bool titleData = InTitle(v);
                bool gpuHeap = v >= 0x40000000 && v < 0x40200000;
                if (!titleData && !gpuHeap) continue;
                sb.Append($"  r{i}->0x{v:X8}: ");
                for (int k = 0; k < 8; k++) sb.Append($"0x{(mem.ReadDword(v + (uint)(k * 4)) ?? 0xEEEEEEEE):X8} ");
                sb.Append('\n');
            }
            string s = sb.ToString();
            _dap.SendEvent("output", new { category = "console", output = s });
            WriteTitleOutput(s);
        }

        private bool InTitle(uint a) =>
            _titleBase != 0 && _titleSize != 0 && a >= _titleBase && a < _titleBase + _titleSize;

        // The Xbox 360 title stack lives in the 0x70000000 range.
        private static bool InStack(uint a) => a >= 0x70000000 && a < 0x80000000;

        // A real return address lands just past a call: the preceding word must be a
        // linking branch (bl, or bclrl/bcctrl). Used to tell a leaf frame (LR still
        // holds the caller) from a non-leaf (LR was spilled to the stack) and to reject
        // garbage while walking the back chain.
        private bool IsCallSiteReturn(uint ret)
        {
            if (!InTitle(ret) || (ret & 3) != 0) return false;
            if (ReadInsn(ret - 4) is not uint w) return false;
            uint op = w >> 26;
            if (op == 18) return (w & 1) != 0;                              // bl (I-form, LK set)
            if (op == 19) { uint xo = (w >> 1) & 0x3FF; return (w & 1) != 0 && (xo == 16 || xo == 528); } // bclrl/bcctrl
            return false;
        }

        // Walk the PowerPC stack. Our clang saves the return address with
        // "stw r0,-8(r1)" before "stwu r1,-N(r1)", so a frame whose SP is P stored its
        // caller's PC at (backchain(P) - 8), where backchain(P) = *(P). The innermost
        // frame may be a leaf (memcpy et al.) that never spilled LR, so its caller comes
        // from the LR register instead -- disambiguated by IsCallSiteReturn.
        private List<uint> UnwindPcs()
        {
            var pcs = new List<uint>();
            if (!_haveCtx || _kit == null || _ctx.Gpr == null) return pcs;
            var mem = new KitMemory(_kit);
            uint sp = (uint)_ctx.Gpr[1];
            pcs.Add(_ctx.Iar);

            uint? entry0 = mem.ReadDword(sp);                       // frame0 back chain = caller SP
            uint lr = _ctx.Lr;
            uint? stk = entry0 is uint e0 ? mem.ReadDword(e0 - 8) : null;   // frame0's spilled LR (if non-leaf)
            uint cur;
            if (IsCallSiteReturn(lr)) cur = lr;                     // leaf: LR is the caller
            else if (stk is uint s && IsCallSiteReturn(s)) cur = s; // non-leaf: use spilled LR
            else if (InTitle(lr)) cur = lr;                        // last resort
            else return pcs;
            sp = entry0 ?? 0;

            for (int guard = 0; cur != 0 && InTitle(cur) && sp != 0 && guard < 64; guard++)
            {
                pcs.Add(cur);
                if (mem.ReadDword(sp) is not uint entry || entry <= sp || entry - sp > 0x100000) break;
                if (mem.ReadDword(entry - 8) is not uint ret || !IsCallSiteReturn(ret)) break;
                cur = ret; sp = entry;
            }
            return pcs;
        }

        private void Scopes(DapConnection.Message req)
        {
            _dap.SendResponse(req, true, new
            {
                scopes = new[] { new { name = "Locals", variablesReference = LocalsRef, expensive = false } }
            });
        }

        private void Variables(DapConnection.Message req)
        {
            EnsureContext();
            var vars = new List<object>();
            int varRef = Int(req.Arguments, "variablesReference", LocalsRef);
            var list = new ValueList();
            if (varRef != LocalsRef && _expand.TryGetValue(varRef, out var expandKey))
            {
                if (TryPdbValues(out var pdbVals, out var ctx, out var memory))
                    pdbVals.TryEmitMembers(expandKey, ref ctx, list, memory);
                else if (TryDwarfValues(out var dwarfVals, out var dwarfMem))
                    dwarfVals.TryEmitMembers(expandKey, list, dwarfMem);
            }
            else if (TryPdbValues(out var pdbLocals, out var locCtx, out var locMem))
            {
                pdbLocals.EmitLocals(ref locCtx, list, locMem);
            }
            else if (TryDwarfValues(out var dwarfLocals, out var dwarfLocMem))
            {
                dwarfLocals.EmitLocals(list, dwarfLocMem);
            }

            foreach (var row in list.Rows)
                vars.Add(DapVar(row));
            _dap.SendResponse(req, true, new { variables = vars });
        }

        /// <summary>
        /// Autos / Watch / hover all send <c>evaluate</c>. Hover fails quietly when the
        /// token is not a live value (same as RXDK-VS20XX) so VS does not show an error
        /// tip. A successful hover/Autos result includes the formatted value and, for
        /// structs, a variablesReference so the data tip can expand.
        /// </summary>
        private void Evaluate(DapConnection.Message req)
        {
            string context = Str(req.Arguments, "context");
            string expression = Str(req.Arguments, "expression");
            bool isHover = string.Equals(context, "hover", StringComparison.OrdinalIgnoreCase);
            string? error = null;

            EnsureContext();
            if (expression.Length > 0 && TryPdbValues(out var pdb, out var ctx, out var memory)
                && pdb.TryEvaluate(expression, ref ctx, memory, out var value, out error, out var expandable))
            {
                int child = 0;
                if (expandable)
                {
                    child = _nextChildRef++;
                    _expand[child] = expression;
                }
                _dap.SendResponse(req, true, new Dictionary<string, object?>
                {
                    ["result"] = value,
                    ["variablesReference"] = child,
                });
                return;
            }

            if (expression.Length > 0 && TryDwarfValues(out var dwarf, out var dwarfMem)
                && dwarf.TryEvaluate(expression, dwarfMem, out var dwarfValue, out var dwarfExpand, out var dwarfKey))
            {
                int child = 0;
                if (dwarfExpand && dwarfKey.Length > 0)
                {
                    child = _nextChildRef++;
                    _expand[child] = dwarfKey;
                }
                _dap.SendResponse(req, true, new Dictionary<string, object?>
                {
                    ["result"] = dwarfValue,
                    ["variablesReference"] = child,
                });
                return;
            }

            if (isHover)
            {
                _dap.SendResponse(req, false, message: error ?? "not available");
                return;
            }

            _dap.SendResponse(req, true, new Dictionary<string, object?>
            {
                ["result"] = error is { Length: > 0 } ? error : "<not available>",
                ["variablesReference"] = 0,
            });
        }

        private bool TryPdbValues(out ManagedValues values, out XContext ctx, out KitMemory memory)
        {
            values = null!;
            ctx = _ctx;
            memory = null!;
            var pdb = _sym?.Pdb;
            if (!_haveCtx || _kit == null || pdb == null || _ctx.Gpr == null)
                return false;
            uint baseAddr = _titleBase != 0 ? _titleBase : TitleAddress.DefaultBase;
            values = new ManagedValues(pdb, baseAddr);
            memory = new KitMemory(_kit);
            return true;
        }

        private bool TryDwarfValues(out DwarfValues values, out KitMemory memory)
        {
            values = null!;
            memory = null!;
            if (!_haveCtx || _kit == null || _sym?.Info == null || _ctx.Gpr == null)
                return false;
            values = new DwarfValues(_sym, _ctx);
            memory = new KitMemory(_kit);
            return true;
        }

        private object DapVar(ValueRow row)
        {
            int child = 0;
            if (row.Expandable && row.ExpandKey.Length > 0)
            {
                child = _nextChildRef++;
                _expand[child] = row.ExpandKey;
            }
            return new Dictionary<string, object?>
            {
                ["name"] = row.Name,
                ["value"] = row.Value,
                ["type"] = row.Type,
                ["variablesReference"] = child,
            };
        }

        private void Continue(DapConnection.Message req)
        {
            // Reply first. ContinueAll can hit the next source BP on the notify thread
            // before this returns; VS then treats a later continue-success as "running"
            // and the Stop/break UI dies (seen as stopped seq after continue seq).
            EndStep();   // drop any half-finished step so its traps don't linger
            _haveCtx = false;
            _dap.SendResponse(req, true, new { allThreadsContinued = true });
            Interlocked.Exchange(ref _paused, 0);
            try { _kit?.ContinueAll(); } catch { }
        }

        private uint TitleBase => _titleBase != 0 ? _titleBase : TitleAddress.DefaultBase;
        private uint KitToExe(uint kit) => TitleAddress.XexToExe(kit, TitleBase);
        private uint ExeToKit(ulong exe) => TitleAddress.ExeToXex((uint)exe, TitleBase);

        // PPC branch decode (instruction words are big-endian, so opcode == w >> 26).
        private static bool IsReturn(uint w) => (w >> 26) == 19 && ((w >> 1) & 0x3FF) == 16;   // bclr/blr family
        private static bool IsDirectCall(uint w) => (w >> 26) == 18 && (w & 1) == 1;            // bl / bla (LK=1)
        private static uint DirectBranchTarget(uint pc, uint w)
        {
            int li = (int)(w & 0x03FFFFFC);
            li = (li << 6) >> 6;                 // sign-extend the 26-bit displacement
            return (w & 2) != 0 ? (uint)li : (uint)(pc + (uint)li);   // AA=1 => absolute
        }

        private void PlantStepBp(uint kitAddr)
        {
            if (_kit == null || _stepBps.Contains(kitAddr)) return;
            // a user breakpoint already traps here; leave it (removing it later would
            // silently drop the user's breakpoint). It still stops the step.
            if (_breakpoints.Exists(e => e.addr == kitAddr)) return;
            try { _kit.SetBreakpoint(kitAddr); _stepBps.Add(kitAddr); } catch { }
        }

        private void ClearStepBps()
        {
            if (_kit != null)
                foreach (var a in _stepBps) { try { _kit.RemoveBreakpoint(a); } catch { } }
            _stepBps.Clear();
        }

        private void EndStep() { ClearStepBps(); _stepMode = 0; _stepReturning = false; }

        private uint? ReadInsn(uint kitAddr) => new KitMemory(_kit!).ReadDword(kitAddr);

        // Plant a trap at every return instruction in the function so a step that leaves
        // the frame is caught at the actual epilogue -- where LR truly holds the caller.
        private void ArmReturns(DwarfFunction? fn)
        {
            if (fn == null) return;
            for (uint a = (uint)fn.LowPc; a < (uint)fn.HighPc; a += 4)
                if (ReadInsn(ExeToKit(a)) is uint w && IsReturn(w))
                    PlantStepBp(ExeToKit(a));
        }

        // The callee's first statement past the prologue (so step-in lands on real code,
        // not the opening brace); fall back to the entry.
        private uint FirstBodyLine(DwarfFunction fn)
        {
            uint best = uint.MaxValue;
            foreach (var u in _sym!.Info!.Units)
                foreach (var r in u.Lines)
                    if (!r.EndSequence && r.IsStmt && r.Address > fn.LowPc && r.Address < fn.HighPc && r.Address < best)
                        best = (uint)r.Address;
            return best == uint.MaxValue ? (uint)fn.LowPc : best;
        }

        // Plant the candidate traps for over/in in the current frame: every other
        // statement line of the function, its returns, and (step-in only) the entry of
        // any direct call made on the current source line.
        private void ArmOverIn(uint exePc, DwarfFunction? fn)
        {
            if (fn == null) { ArmReturns(fn); return; }
            uint nextStmt = (uint)fn.HighPc;
            foreach (var u in _sym!.Info!.Units)
                foreach (var r in u.Lines)
                {
                    if (r.EndSequence || r.Address < fn.LowPc || r.Address >= fn.HighPc) continue;
                    if (r.IsStmt && r.Address > exePc && r.Address < nextStmt) nextStmt = (uint)r.Address;
                    if (r.IsStmt && (r.Line != _stepStartLine || r.File != _stepStartFile))
                        PlantStepBp(ExeToKit(r.Address));
                }
            ArmReturns(fn);
            if (_stepMode == 2)   // step-in: follow direct calls that have source
                for (uint a = exePc; a < nextStmt; a += 4)
                {
                    if (ReadInsn(ExeToKit(a)) is not uint w || !IsDirectCall(w)) continue;
                    var tfn = _sym.FunctionAt(DirectBranchTarget(a, w));
                    if (tfn != null) PlantStepBp(ExeToKit(FirstBodyLine(tfn)));
                }
        }

        private void ArmForPc(uint exePc, DwarfFunction? fn)
        {
            if (_stepMode == 3) ArmReturns(fn);
            else ArmOverIn(exePc, fn);
        }

        private void ResumeStep()
        {
            _haveCtx = false;
            Interlocked.Exchange(ref _paused, 0);
            try { _kit?.ContinueAll(); } catch { }
        }

        // At a return instruction LR holds the caller PC; run there and stop in the caller.
        private void RunToReturn(uint lr)
        {
            ClearStepBps();
            _stepReturning = true;
            if (lr != 0) PlantStepBp(lr);
            ResumeStep();
        }

        private void Step(DapConnection.Message req)
        {
            EnsureContext();
            int mode = req.Command == "stepIn" ? 2 : req.Command == "stepOut" ? 3 : 1;
            _dap.SendResponse(req, true);

            // No context/symbols: nothing to line-step against -- just run.
            if (!_haveCtx || _sym?.Info == null || _kit == null || _ctx.Gpr == null)
            {
                EndStep();
                _haveCtx = false;
                Interlocked.Exchange(ref _paused, 0);
                try { _kit?.ContinueAll(); } catch { }
                return;
            }

            uint iar = _ctx.Iar;
            uint exePc = KitToExe(iar);
            var fn = _sym.FunctionAt(exePc);
            var ln = _sym.LineAt(exePc);

            ClearStepBps();
            _stepMode = mode;
            _stepTid = _stoppedThread;
            _stepStartSp = (uint)_ctx.Gpr[1];
            _stepStartLine = ln?.Line ?? -1;
            _stepStartFile = ln?.File ?? "";
            _stepReturning = false;

            // Step-out, or a step from a frame with no known return instruction, runs to
            // the caller via the epilogue; if we are already sitting on a return, use LR now.
            if (mode == 3 && ReadInsn(iar) is uint w0 && IsReturn(w0)) RunToReturn(_ctx.Lr);
            else { ArmForPc(exePc, fn); ResumeStep(); }
        }

        /// <summary>
        /// A break arrived while a step is in flight. Returns true if the step is
        /// satisfied (let the caller surface "stopped"), false if it re-armed and
        /// continued the title.
        /// </summary>
        private bool HandleStepBreak(XbdmNotification n)
        {
            if (_stepMode == 0 || _kit == null || _sym?.Info == null) { EndStep(); return true; }

            // A user breakpoint always wins: honour it and drop the step.
            if (n.Addr is uint ua && _breakpoints.Exists(e => e.addr == ua)) { EndStep(); return true; }

            uint tid = n.Thread ?? _stepTid; if (tid == 0) tid = _stepTid;
            XContext ctx;
            try { ctx = _kit.GetContext(tid, XContextFlags.Control | XContextFlags.Integer); }
            catch { EndStep(); return true; }
            if (ctx.Gpr == null) { EndStep(); return true; }

            uint iar = ctx.Iar;
            uint sp = (uint)ctx.Gpr[1];
            uint exePc = KitToExe(iar);
            var fn = _sym.FunctionAt(exePc);
            var ln = _sym.LineAt(exePc);

            // Phase 2: we planted the caller's return address and ran to it. Only stop
            // once we are back in the starting frame or shallower -- unwinding a recursive
            // call passes through the same code at deeper SPs, which must not stop the step.
            if (_stepReturning)
            {
                if (ln != null && sp >= _stepStartSp) { _stepStopping = true; EndStep(); return true; }
                // still in a deeper frame, or returned into code with no source: keep unwinding.
                _stepReturning = false;
                ClearStepBps(); ArmReturns(fn); ResumeStep(); return false;
            }

            // Sitting on a return instruction: capture the caller from LR and run out.
            if (ReadInsn(iar) is uint w && IsReturn(w)) { RunToReturn(ctx.Lr); return false; }

            bool deeper = sp < _stepStartSp;      // entered a callee
            bool returned = sp > _stepStartSp;    // popped back to a caller
            bool sameFrame = sp == _stepStartSp;

            if (_stepMode == 3)   // step-out: only a shallower frame counts
            {
                if (returned && ln != null) { _stepStopping = true; EndStep(); return true; }
                ClearStepBps(); ArmReturns(fn); ResumeStep(); return false;
            }

            // step-over / step-in
            if (deeper)
            {
                // step-in stops at a callee that has source; otherwise (and always for
                // step-over) run the callee to completion -- never wander into libc.
                if (_stepMode == 2 && ln != null) { _stepStopping = true; EndStep(); return true; }
                ClearStepBps(); ArmReturns(fn); ResumeStep(); return false;
            }
            if (returned && ln != null) { _stepStopping = true; EndStep(); return true; }
            if (sameFrame && ln != null && (ln.Line != _stepStartLine || ln.File != _stepStartFile))
            { _stepStopping = true; EndStep(); return true; }

            // still on the start line (or a wrong-frame recursion hit): re-arm and run on.
            ClearStepBps(); ArmForPc(exePc, fn); ResumeStep(); return false;
        }

        private void Pause(DapConnection.Message req)
        {
            EndStep();   // an explicit break wins over any in-flight step
            try { _kit?.Stop(); } catch { }
            _dap.SendResponse(req, true);
            if (Interlocked.CompareExchange(ref _paused, 1, 0) == 0)
            {
                _haveCtx = false;
                _dap.SendEvent("stopped", new Dictionary<string, object?>
                {
                    ["reason"] = "pause",
                    ["threadId"] = DapTid(_stoppedThread != 0 ? _stoppedThread : 1),
                    ["allThreadsStopped"] = true,
                });
            }
        }

        /// <summary>
        /// XBDM fires this on its notify thread. Do not call back into xbdm.dll here
        /// (GetContext / GetThreads) — that races the DAP thread and Visual Studio then
        /// surfaces a delayed NullReferenceException. Context is fetched on the next
        /// threads / stackTrace request instead. Extra break notifications while already
        /// stopped are ignored so VS is not sent a second stopped event with a null source.
        /// </summary>
        private void OnKitEvent(XbdmNotification n)
        {
            try
            {
                if (n.Kind is "debugstr" or "assert")
                {
                    string text = n.Fields.TryGetValue("string", out var s) ? s : n.Raw;
                    if (!string.IsNullOrEmpty(text))
                    {
                        string line = text.EndsWith("\n", StringComparison.Ordinal) ? text : text + "\n";
                        WriteTitleOutput(line);
                        // Surface the title's own debug output (OutputDebugString, ATG/D3D
                        // WRN/ERR, asserts) in the VS Output window -- these carry the real
                        // diagnosis (e.g. "ERR[D3D]: BOOL value for constant 3 must be 0 or 1")
                        // that a bare break address never shows.
                        _dap.SendEvent("output", new { category = "stdout", output = line });
                    }
                    return;
                }
                if (_holdingInitialBreak) return;
                if (n.Kind is not ("break" or "singlestep" or "data" or "exception")) return;

                // Auto-continue ONLY the loader's pre-main break: a 'break' that is not a
                // breakpoint we planted AND lies outside the loaded title (e.g. 0x800Axxxx).
                // It is a resumable loader sync-stop with no diagnostic value, and surfacing
                // it just yields a frame VS can't map. An IN-title trap is left to surface:
                // the XDK debug runtime's break-on-level (RtlDebugError) marks a REAL error
                // (a failed D3D validation, an assert), whose message we now forward to the
                // Output window -- continuing past it would hide the actual bug. If the PC
                // sits on an embedded trap instruction (twi/tw), advance NIA past it first
                // or the kit would re-execute it and re-trap.
                if (n.Kind == "break" && n.Addr is uint bpc
                    && !_breakpoints.Exists(e => e.addr == bpc)
                    && !(_titleSize != 0 && bpc >= _titleBase && bpc < _titleBase + _titleSize))
                {
                    try
                    {
                        uint tid0 = n.Thread ?? _stoppedThread; if (tid0 == 0) tid0 = 1;
                        var word = new KitMemory(_kit!).ReadDword(bpc);
                        if (word is uint w && IsTrapInstruction(w))
                        {
                            var ctx = _kit!.GetContext(tid0, XContextFlags.Control);
                            if ((uint)ctx.Iar == bpc) { ctx.Iar = bpc + 4; _kit.SetContext(tid0, ref ctx); }
                        }
                    }
                    catch { /* best-effort: fall through to a plain continue */ }
                    _dap.SendEvent("output", new { category = "console",
                        output = $"KIT auto-continue non-breakpoint trap addr=0x{bpc:X8}\n" });
                    try { _kit?.ContinueAll(); } catch { }
                    return;
                }

                // A step is in flight: the engine decides whether this break satisfies it
                // (fall through and surface "stopped") or re-arms and keeps the title running.
                if (_stepMode != 0 && n.Kind is "break" or "singlestep")
                    if (!HandleStepBreak(n)) return;

                if (Interlocked.CompareExchange(ref _paused, 1, 0) != 0) return;

                EndStep();   // stopping for any reason ends stepping; drop leftover step traps

                uint tid = n.Thread ?? 1;
                if (tid == 0) tid = 1;
                _stoppedThread = tid;
                _haveCtx = false;
                _diagDumped = false;
                _expand.Clear();
                _nextChildRef = LocalsRef + 1;
                _dap.SendEvent("output", new { category = "console", output = "KIT " + n + "\n" });
                string reason = _stepStopping ? "step" : n.Kind switch
                {
                    "break" or "data" => "breakpoint",
                    "exception" or "assert" => "exception",
                    _ => "step",
                };
                _stepStopping = false;
                _dap.SendEvent("stopped", new Dictionary<string, object?>
                {
                    ["reason"] = reason,
                    ["threadId"] = DapTid(tid),
                    ["allThreadsStopped"] = true,
                });
            }
            catch { }
        }

        // PowerPC trap instructions the XDK debug runtime and __debugbreak plant inline:
        //   twi TO,RA,SI  -> primary opcode 3
        //   tw  TO,RA,RB  -> primary opcode 31, extended opcode 4
        private static bool IsTrapInstruction(uint word)
        {
            uint op = word >> 26;
            if (op == 3) return true;
            return op == 31 && ((word >> 1) & 0x3FF) == 4;
        }

        private void WriteTitleOutput(string text)
        {
            if (string.IsNullOrEmpty(_titleOutputFile)) return;
            try { File.AppendAllText(_titleOutputFile, text); } catch { }
        }

        private void Disconnect(DapConnection.Message req, bool terminateDefault)
        {
            bool terminate = !req.HasArguments || Bool(req.Arguments, "terminateDebuggee", terminateDefault);
            TearDownKit(terminate);
            Terminated = true;
            _dap.SendResponse(req, true);
            _dap.SendEvent("terminated");
        }

        private void TearDownKit(bool terminate)
        {
            var kit = _kit;
            if (kit == null) { _kit = null; return; }
            try { kit.Notification -= OnKitEvent; } catch { }
            // Leave the kit in a clean, RUNNING state before we let go. If we detach or
            // reboot while the title is stopped at a break, its threads stay suspended and
            // our step traps stay planted -- xbdm/kernel state is then inconsistent and the
            // NEXT cold boot hangs at the logo (had to power-cycle to recover). So first
            // pull every step breakpoint and continue all threads, regardless of whether
            // we terminate or detach. (_kit is still live here so ClearStepBps works.)
            try { EndStep(); } catch { }
            try { foreach (var e in _breakpoints) kit.RemoveBreakpoint(e.addr); } catch { }
            try { kit.ContinueAll(); } catch { }
            _kit = null;
            try
            {
                if (terminate) kit.RebootToDashboard();
                else kit.Resume(detach: true);
            }
            catch { }
            try { kit.Dispose(); } catch { }
        }

        private void EnsureContext()
        {
            if (_haveCtx || _kit == null) return;
            try
            {
                uint tid = _stoppedThread;
                if (tid == 0)
                {
                    var ts = _kit.GetThreads();
                    tid = ts.Length > 0 ? ts[0] : 0;
                }
                if (tid == 0) return;
                _stoppedThread = tid;
                _ctx = _kit.GetContext(tid, XContextFlags.Control | XContextFlags.Integer);
                _haveCtx = _ctx.Gpr != null;
            }
            catch { _haveCtx = false; }
        }

        private static int DapTid(uint tid)
        {
            if (tid == 0) return 1;
            return tid <= int.MaxValue ? (int)tid : unchecked((int)(tid & 0x7FFFFFFF));
        }

        // ---- small JSON helpers ----
        private static string Str(JsonElement e, string name) =>
            e.ValueKind == JsonValueKind.Object && e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String
                ? v.GetString() ?? "" : "";
        private static bool Bool(JsonElement e, string name, bool dflt) =>
            e.ValueKind == JsonValueKind.Object && e.TryGetProperty(name, out var v) &&
            (v.ValueKind == JsonValueKind.True || v.ValueKind == JsonValueKind.False) ? v.GetBoolean() : dflt;
        private static int Int(JsonElement e, string name, int dflt)
        {
            if (e.ValueKind != JsonValueKind.Object || !e.TryGetProperty(name, out var v))
                return dflt;
            if (v.ValueKind == JsonValueKind.Number && v.TryGetInt32(out var n))
                return n;
            return dflt;
        }
    }
}
