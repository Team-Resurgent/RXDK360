// SPDX-License-Identifier: GPL-3.0-or-later
// Part of RXDK-360 - see LICENSE.md for the full GNU GPL v3.
//
// CLI over the XBDM client, to exercise the devkit debug path:
//
//   Rxdk.Xbox360.Xbdm info                              locate bin\win32\xbdm.dll + $(DefaultConsole)
//   Rxdk.Xbox360.Xbdm threads [console]                 connect and list modules + threads
//   Rxdk.Xbox360.Xbdm debuglaunch [console] <xex> [bp]  deploy + stop at entry; optional execute BP (VA or RVA)
//       --skip-faults: while waiting for the BP, step a faulting thread past the faulting
//       instruction and put the exception code in its r0 (instruction probes; see
//       tests/corpus/vmxops)
//   Rxdk.Xbox360.Xbdm run         [console] <xex>       deploy + reboot title (no debugger)
//   Rxdk.Xbox360.Xbdm watch       [console]             print XBDM notifications (watch an official F5)
//   Rxdk.Xbox360.Xbdm screenshot  [console] [file.bmp]  capture the kit front buffer via DmScreenShot
//   Rxdk.Xbox360.Xbdm deploy  <console> <file> <remote> send a file to the console
//   Rxdk.Xbox360.Xbdm dir     <console> <remoteDir>     list a directory on the console
//   Rxdk.Xbox360.Xbdm getfile <console> <remote> <file> fetch a file from the console (e.g. a crash dump)
//   Rxdk.Xbox360.Xbdm launch  <console> <imagePath>     reboot the console into a title (stopped)
//   Rxdk.Xbox360.Xbdm mem     <console> <hexAddr> <len> read memory
//   Rxdk.Xbox360.Xbdm reboot  [console]             cold-reboot the console (recovers a title
//                                                     stuck on an exception)
//
// Console defaults to HKCU\SOFTWARE\Microsoft\XenonSDK\XboxName ($(DefaultConsole)).
//
// Everything but "info" needs a real devkit reachable on the network.

using System;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using Rxdk.Xbox360.Xbdm;

if (args.Length < 1) { Console.Error.WriteLine("usage: Rxdk.Xbox360.Xbdm <info|threads|debuglaunch|run|halt|go|unbreak|watch|screenshot|deploy|launch|mem|reboot> ..."); return 1; }

string ConsoleName(int i)
{
    if (args.Length > i && !string.IsNullOrWhiteSpace(args[i])) return args[i];
    return XbdmClient.DefaultConsole()
        ?? throw new InvalidOperationException("no console: pass an IP/name or set XenonSDK\\XboxName ($(DefaultConsole)).");
}

try
{
    switch (args[0])
    {
        case "info":
        {
            Console.WriteLine($"process: {(Environment.Is64BitProcess ? "x64 (WRONG — need x86)" : "x86")}");
            int size = Marshal.SizeOf<XContext>();
            Console.WriteLine($"XContext marshalled size: {size} bytes (expected 2624)");
            Console.WriteLine($"DefaultConsole: {XbdmClient.DefaultConsole() ?? "(unset)"}");
            string? dll = XbdmClient.LocateXbdm();
            Console.WriteLine(dll != null
                ? $"xbdm.dll found: {dll}"
                : "xbdm.dll NOT found (install RXDK-360 / the XDK).");
            // Force-load via DmTranslateError so we know the PE32 host DLL actually mapped.
            var sb = new StringBuilder(512);
            int hr = NativeXbdm.DmTranslateError(XbdmHResults.CannotConnect, sb, sb.Capacity);
            Console.WriteLine($"DmTranslateError(XBDM_CANNOTCONNECT) hr=0x{(uint)hr:X8} \"{sb}\"");
            break;
        }
        case "threads":
        {
            string kit = ConsoleName(1);
            using var c = new XbdmClient();
            c.Connect(kit);
            Console.WriteLine($"connected: {kit}");
            try
            {
                foreach (var m in c.WalkLoadedModules())
                    Console.WriteLine($"  module {m.Name}  base=0x{unchecked((uint)m.BaseAddress.ToUInt32()):X8}  size=0x{m.Size:X}");
            }
            catch (Exception ex) { Console.WriteLine($"  (modules: {ex.Message})"); }
            var threads = c.GetThreads();
            Console.WriteLine($"{threads.Length} thread(s)");
            foreach (var tid in threads)
            {
                try
                {
                    var ctx = c.GetContext(tid, XContextFlags.Control | XContextFlags.Integer);
                    Console.WriteLine($"  thread 0x{tid:X8}  PC=0x{ctx.Iar:X8}  LR=0x{ctx.Lr:X8}  r1=0x{ctx.Gpr[1]:X16}");
                }
                catch (Exception ex)
                {
                    Console.WriteLine($"  thread 0x{tid:X8}  ({ex.Message})");
                }
            }
            break;
        }
        case "debuglaunch":
        case "run":
        {
            bool debug = args[0] == "debuglaunch";
            string? kit = null, image = null;
            uint? sourceBp = null;
            bool skipFaults = false;
            for (int i = 1; i < args.Length; i++)
            {
                if (args[i] == "--skip-faults") skipFaults = true;
                else if (LooksLikeHex(args[i])) sourceBp = ParseHex(args[i]);
                else if (IsImagePath(args[i])) image = args[i];
                else kit = args[i];
            }
            if (image == null)
                throw new InvalidOperationException("usage: Rxdk.Xbox360.Xbdm debuglaunch|run [console] <xex> [bp]");
            kit ??= ConsoleName(99);
            using var c = new XbdmClient();
            c.Notification += n => Console.WriteLine($"  notify {n}");
            c.Connect(kit);
            Console.WriteLine($"connected: {kit}");
            if (debug)
            {
                var stop = c.LaunchStopped(image);
                Console.WriteLine($"initial break: {stop}");
                // RXDK_POKE="0xADDR=0xVAL": write a 32-bit BE value at the initial
                // break (before any allocation) -- e.g. set g_fSmallPagesOverride.
                var poke = Environment.GetEnvironmentVariable("RXDK_POKE");
                if (!string.IsNullOrEmpty(poke))
                {
                    var pp = poke.Split('=');
                    uint pa = Convert.ToUInt32(pp[0], 16), pv = Convert.ToUInt32(pp[1], 16);
                    c.WriteMemory(pa, new byte[] { (byte)(pv >> 24), (byte)(pv >> 16), (byte)(pv >> 8), (byte)pv });
                    Console.WriteLine($"poked 0x{pa:X8} = 0x{pv:X8}");
                }
                string titleHint = Path.GetFileNameWithoutExtension(image);
                uint titleBase = 0, titleSize = 0;
                foreach (var m in c.WalkLoadedModules())
                {
                    uint b = unchecked((uint)m.BaseAddress.ToUInt32());
                    Console.WriteLine($"  module {m.Name}  base=0x{b:X8}  size=0x{m.Size:X}");
                    if (!string.IsNullOrEmpty(m.Name) &&
                        m.Name.IndexOf(titleHint, StringComparison.OrdinalIgnoreCase) >= 0)
                    { titleBase = b; titleSize = m.Size; }
                }
                DumpTitleThreads(c, titleBase, titleSize);
                if (sourceBp is uint cap)
                {
                    // Capture BPs from XBDM are already XEX VAs. Subtract the live
                    // XEX base to get the PDB RVA; add that RVA back onto the XEX
                    // base to plant. (Same as OG: kit VA ↔ RVA ↔ exe VA.)
                    uint xexBase = titleBase != 0 ? titleBase : TitleAddress.DefaultBase;
                    uint rva = TitleAddress.XexToRva(cap, xexBase);
                    uint exe = TitleAddress.RvaToExe(rva);
                    uint kitBp = TitleAddress.RvaToXex(rva, xexBase);
                    Console.WriteLine($"xex=0x{cap:X8} -> rva=0x{rva:X} -> exe=0x{exe:X8}; plant xex=0x{kitBp:X8} (base=0x{xexBase:X8})");
                    c.SetBreakpoint(kitBp);
                    c.ContinueAll();
                    Console.WriteLine("auto-continued past initial break; waiting for source BP");
                    if (c.WaitFor(n =>
                        {
                            if (skipFaults && n.Kind == "exception" && n.Thread is uint ft)
                            {
                                SkipFault(c, ft, n);
                                return false;
                            }
                            return n.Kind is "break" or "singlestep" or "exception" &&
                                   n.Addr is uint a && a == kitBp;
                        }, skipFaults ? TimeSpan.FromMinutes(10) : TimeSpan.FromSeconds(30), out var hit))
                        Console.WriteLine($"source BP hit: {hit}");
                    else
                        Console.WriteLine("timed out waiting for source BP");
                    DumpTitleThreads(c, titleBase, titleSize);
                    try { c.RemoveBreakpoint(kitBp); Console.WriteLine($"removed BP 0x{kitBp:X8}"); }
                    catch (Exception ex) { Console.WriteLine($"remove BP: {ex.Message}"); }
                }
                else
                {
                    Console.WriteLine("no source BPs — auto-continue");
                    c.ContinueAll();
                    // Keep the debug-string channel open so DbgPrint/OutputDebugString
                    // from the running title (e.g. [SMWIN]) is captured. Detach after
                    // RXDK_LISTEN seconds (default 45).
                    int listen = int.TryParse(Environment.GetEnvironmentVariable("RXDK_LISTEN"), out var ls) ? ls : 45;
                    if (listen > 0)
                    {
                        Console.WriteLine($"listening for debug strings for {listen}s...");
                        var until = DateTime.UtcNow.AddSeconds(listen);
                        while (DateTime.UtcNow < until) Thread.Sleep(200);
                    }
                }
                c.Resume(detach: true);
                Console.WriteLine("resumed (debugger detached)");
            }
            else
            {
                string remote = File.Exists(image)
                    ? c.DeployTitle(image, "devkit:\\" + Path.GetFileNameWithoutExtension(image))
                    : image;
                var (dir, name) = SplitXboxPath(remote);
                c.SetTitle(dir, name);
                c.LaunchTitle(remote, stopAtLoad: false);
                Console.WriteLine($"running {remote}");
            }
            break;
        }
        case "reboot":
        {
            string kit = ConsoleName(1);
            using var c = new XbdmClient();
            c.Connect(kit);
            c.Reboot(DmBoot.Cold);
            Console.WriteLine($"rebooted {kit}");
            break;
        }
        case "trace":
        {
            // trace [console] <xex> <bpHex> [reg] [seconds] : deploy+launch, plant a
            // breakpoint, and on every hit log GPR[reg] then auto-continue -- a
            // tracepoint that captures a full allocation sequence without stopping.
            string? kit = null, image = null; uint bp = 0; int reg = 31; int seconds = 80; int maxHits = 8000;
            var seen = new System.Collections.Generic.List<int>();
            for (int i = 1; i < args.Length; i++)
            {
                if (IsImagePath(args[i])) image = args[i];
                else if (LooksLikeHex(args[i])) bp = ParseHex(args[i]);
                else if (int.TryParse(args[i], out var iv)) seen.Add(iv);
                else kit = args[i];
            }
            if (seen.Count > 0) reg = seen[0];
            if (seen.Count > 1) seconds = seen[1];
            if (image == null || bp == 0)
                throw new InvalidOperationException("usage: Rxdk.Xbox360.Xbdm trace [console] <xex> <bpHex> [reg] [seconds]");
            kit ??= ConsoleName(99);
            using var c = new XbdmClient();
            c.Connect(kit);
            // RXDK_ARM=0xADDR : run to this BP first (once), THEN plant the trace BP.
            // Lets us skip every allocation before a specific call site.
            uint arm = 0;
            var armEnv = Environment.GetEnvironmentVariable("RXDK_ARM");
            if (!string.IsNullOrEmpty(armEnv)) arm = Convert.ToUInt32(armEnv, 16);
            Console.WriteLine($"connected: {kit}; trace bp=0x{bp:X8} reg=r{reg} for {seconds}s" + (arm != 0 ? $"; arm=0x{arm:X8}" : ""));
            var stop = c.LaunchStopped(image);
            if (arm != 0)
            {
                c.SetBreakpoint(arm);
                c.ContinueAll();
                if (c.WaitFor(x => x.Kind is "break" && x.Addr is uint aa && aa == arm, TimeSpan.FromSeconds(seconds), out var ah))
                {
                    Console.WriteLine($"[ARM] hit 0x{arm:X8} thread=0x{ah.Thread:X8} -- planting trace BP");
                    try { c.RemoveBreakpoint(arm); } catch { }
                    c.SetBreakpoint(bp);
                    uint atid = ah.Thread ?? 0;
                    try { c.ContinueThread(atid, false); c.Go(); } catch { try { c.ContinueAll(); } catch { } }
                }
                else { Console.WriteLine("[ARM] timed out waiting for arm BP"); try { c.Resume(detach: true); } catch { } break; }
            }
            else
            {
                c.SetBreakpoint(bp);
                c.ContinueAll();
            }
            var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(seconds);
            int n = 0, below = 0;
            while (n < maxHits)
            {
                var left = deadline - DateTime.UtcNow;
                if (left <= TimeSpan.Zero) break;
                if (!c.WaitFor(x => x.Kind is "break" && x.Addr is uint a && a == bp, left, out var hit)) break;
                uint tid = hit.Thread ?? 0; uint val = 0, r29 = 0, r30 = 0, r31 = 0;
                try { var ctx = c.GetContext(tid, XContextFlags.Integer); val = (uint)ctx.Gpr[reg]; r29 = (uint)ctx.Gpr[29]; r30 = (uint)ctx.Gpr[30]; r31 = (uint)ctx.Gpr[31]; } catch { }
                bool bad = val != 0 && val < 0xA0000000;
                if (bad) below++;
                Console.WriteLine($"[TRACE] #{n,-4} r{reg}=0x{val:X8} r29(align)=0x{r29:X8} r30(prot)=0x{r30:X8} r31(result)=0x{r31:X8}{(bad ? "  *** BELOW 0xA0000000 ***" : "")}");
                n++;
                // fast continue: step the faulting thread past the BP and go (no full thread walk)
                try { c.ContinueThread(tid, false); c.Go(); } catch { try { c.ContinueAll(); } catch { } }
            }
            Console.WriteLine($"[TRACE] done: {n} hits, {below} below 0xA0000000");
            try { c.Resume(detach: true); } catch { }
            break;
        }
        case "halt":
        {
            string kit = ConsoleName(1);
            using var c = new XbdmClient();
            c.Notification += n => Console.WriteLine($"  notify {n}");
            c.Connect(kit);
            c.ListenForEvents();
            c.AttachDebugger();
            c.Stop();
            Console.WriteLine("halted");
            uint titleBase = 0, titleSize = 0;
            foreach (var m in c.WalkLoadedModules())
            {
                uint b = unchecked((uint)m.BaseAddress.ToUInt32());
                Console.WriteLine($"  module {m.Name}  base=0x{b:X8}  size=0x{m.Size:X}");
                if (m.Name.IndexOf("Xbox360Game11", StringComparison.OrdinalIgnoreCase) >= 0)
                { titleBase = b; titleSize = m.Size; }
            }
            foreach (var tid in c.GetThreads())
            {
                try
                {
                    var ctx = c.GetContext(tid, XContextFlags.Control | XContextFlags.Integer);
                    uint start = 0;
                    try { start = unchecked((uint)c.GetThreadInfo(tid).StartAddress.ToUInt32()); } catch { }
                    bool inTitle = titleSize != 0 &&
                        ((ctx.Iar >= titleBase && ctx.Iar < titleBase + titleSize) ||
                         (start >= titleBase && start < titleBase + titleSize));
                    Console.WriteLine($"  thread 0x{tid:X8}  PC=0x{ctx.Iar:X8}  LR=0x{ctx.Lr:X8}  start=0x{start:X8}{(inTitle ? "  [title]" : "")}");
                }
                catch { }
            }
            c.Resume(detach: true);
            Console.WriteLine("resumed (debugger detached)");
            break;
        }
        case "go":
        {
            using var c = new XbdmClient();
            c.Connect(ConsoleName(1));
            c.Resume(detach: true);
            Console.WriteLine("go (debugger detached)");
            break;
        }
        case "unbreak":
        {
            uint addr = ParseHex(args.Length > 2 ? args[2] : args[1]);
            using var c = new XbdmClient();
            c.Connect(ConsoleName(args.Length > 2 ? 1 : 99));
            c.AttachDebugger();
            try { c.RemoveBreakpoint(addr); Console.WriteLine($"removed BP 0x{addr:X8}"); }
            catch (Exception ex) { Console.WriteLine($"remove: {ex.Message}"); }
            c.Resume(detach: true);
            Console.WriteLine("go (debugger detached)");
            break;
        }
        case "watch":
        {
            string kit = ConsoleName(1);
            using var c = new XbdmClient();
            c.Notification += n => Console.WriteLine($"{DateTime.Now:HH:mm:ss.fff}  {n}");
            c.Connect(kit);
            try
            {
                c.ListenForEvents(DmNotifySession.Persistent | DmNotifySession.AsyncSession);
            }
            catch (XbdmException)
            {
                c.ListenForEvents();
            }
            Console.WriteLine($"watching {kit} — F5 the official XDK title now (Ctrl+C to stop)");
            try
            {
                foreach (var m in c.WalkLoadedModules())
                    Console.WriteLine($"  module {m.Name}  base=0x{unchecked((uint)m.BaseAddress.ToUInt32()):X8}  size=0x{m.Size:X}");
            }
            catch (Exception ex) { Console.WriteLine($"  (modules: {ex.Message})"); }
            var until = DateTime.UtcNow.AddMinutes(15);
            while (DateTime.UtcNow < until)
                Thread.Sleep(250);
            break;
        }
        case "screenshot":
        {
            string console = ConsoleName(1);
            string file = args.Length > 2 && !string.IsNullOrWhiteSpace(args[2])
                ? args[2]
                : Path.Combine(Environment.CurrentDirectory, "xbox360-screenshot.bmp");
            // `Rxdk.Xbox360.Xbdm screenshot file.bmp` — first arg is a path, not a console.
            if (args.Length == 2 && (args[1].EndsWith(".bmp", StringComparison.OrdinalIgnoreCase)
                || args[1].Contains('\\') || args[1].Contains('/')))
            {
                console = XbdmClient.DefaultConsole()
                    ?? throw new InvalidOperationException("no console: pass an IP/name or set XenonSDK\\XboxName.");
                file = args[1];
            }
            using var c = new XbdmClient();
            c.Connect(console);
            c.ScreenShot(file);
            var fi = new FileInfo(file);
            Console.WriteLine($"screenshot {fi.FullName} ({fi.Length} bytes)");
            break;
        }
        case "deploy":
        {
            using var c = new XbdmClient();
            c.Connect(args[1]);
            c.SendFile(args[2], args[3]);
            Console.WriteLine($"sent {args[2]} -> {args[3]}");
            break;
        }
        case "dir":
        {
            using var c = new XbdmClient();
            c.Connect(args[1]);
            foreach (var e in c.WalkDir(args[2]))
            {
                ulong size = ((ulong)e.SizeHigh << 32) | e.SizeLow;
                string when = DateTime.FromFileTime(e.ChangeTime).ToString("yyyy-MM-dd HH:mm:ss");
                Console.WriteLine((e.Attributes & 0x10) != 0
                    ? $"{when}  <dir>        {e.Name}"
                    : $"{when}  {size,12}  {e.Name}");
            }
            break;
        }
        case "getfile":
        {
            using var c = new XbdmClient();
            c.Connect(args[1]);
            c.ReceiveFile(args[2], args[3]);
            Console.WriteLine($"received {args[2]} -> {args[3]}");
            break;
        }
        case "launch":
        {
            using var c = new XbdmClient();
            c.Connect(args[1]);
            c.AttachDebugger();
            c.LaunchTitle(args[2], stopAtLoad: true);
            Console.WriteLine($"launched {args[2]} (stopped at load)");
            break;
        }
        case "mem":
        {
            using var c = new XbdmClient();
            c.Connect(args[1]);
            uint addr = ParseHex(args[2]);
            int len = int.Parse(args[3]);
            var bytes = c.ReadMemory(addr, len);
            Console.WriteLine($"0x{addr:X8}: " + BitConverter.ToString(bytes));
            break;
        }
        default:
            Console.Error.WriteLine("unknown command: " + args[0]);
            return 1;
    }
}
catch (Exception e) { Console.Error.WriteLine("error: " + e.Message); return 1; }
return 0;

static uint ParseHex(string s)
{
    s = s.Trim(); if (s.StartsWith("0x") || s.StartsWith("0X")) s = s[2..];
    return uint.Parse(s, NumberStyles.HexNumber);
}

static bool LooksLikeHex(string s)
{
    s = s.Trim();
    if (s.StartsWith("0x", StringComparison.OrdinalIgnoreCase)) return true;
    return s.Length is >= 6 and <= 8 && uint.TryParse(s, NumberStyles.HexNumber, null, out _);
}

static bool IsImagePath(string s) =>
    s.EndsWith(".xex", StringComparison.OrdinalIgnoreCase) ||
    s.Contains('\\') || s.Contains('/') || File.Exists(s);

static void DumpTitleThreads(XbdmClient c, uint titleBase, uint titleSize)
{
    foreach (var tid in c.GetThreads())
    {
        try
        {
            var ctx = c.GetContext(tid, XContextFlags.Control | XContextFlags.Integer);
            uint start = 0;
            try { start = unchecked((uint)c.GetThreadInfo(tid).StartAddress.ToUInt32()); } catch { }
            bool halted = c.TryGetThreadStop(tid, out _);
            bool inTitle = titleSize != 0 &&
                ((ctx.Iar >= titleBase && ctx.Iar < titleBase + titleSize) ||
                 (start >= titleBase && start < titleBase + titleSize));
            if (halted || inTitle)
                Console.WriteLine($"  thread 0x{tid:X8}  PC=0x{ctx.Iar:X8}  LR=0x{ctx.Lr:X8}  start=0x{start:X8}{(inTitle ? "  [title]" : "")}{(halted ? "  [stopped]" : "")}");
            if (halted && ctx.Gpr != null)
                for (int g = 0; g < 32; g += 4)
                    Console.WriteLine($"    r{g,-2}=0x{(uint)ctx.Gpr[g]:X8} r{g + 1,-2}=0x{(uint)ctx.Gpr[g + 1]:X8} r{g + 2,-2}=0x{(uint)ctx.Gpr[g + 2]:X8} r{g + 3,-2}=0x{(uint)ctx.Gpr[g + 3]:X8}");
        }
        catch { }
    }
}

static (string dir, string name) SplitXboxPath(string remote)
{
    remote = remote.Replace('/', '\\');
    int i = remote.LastIndexOf('\\');
    if (i < 0) return ("devkit:\\", remote);
    return (remote[..(i + 1)], remote[(i + 1)..]);
}

// --skip-faults: an instruction probe executes candidate instructions with r0
// cleared and reads r0 back afterwards. A faulting instruction stops its thread
// with the debugger attached; step past it (4 bytes: every PowerPC
// instruction) and hand the exception code back in r0, then continue the thread
// with the exception handled and resume the title. XBDM reports each exception twice, so a
// notification whose thread is no longer at the faulting address has already
// been handled and is ignored (stepping again would skip the instruction that
// reads r0).
static void SkipFault(XbdmClient c, uint tid, XbdmNotification n)
{
    uint code = n.Fields.TryGetValue("code", out var cs)
        ? Convert.ToUInt32(cs.StartsWith("0x", StringComparison.OrdinalIgnoreCase) ? cs[2..] : cs, 16)
        : 0xFFFFFFFFu;
    var ctx = c.GetContext(tid);
    uint at = ctx.Iar;
    if (n.Addr is uint faultAt && at != faultAt)
        return;
    ctx.Iar += 4;
    ctx.Gpr[0] = code;
    c.SetContext(tid, ref ctx);
    c.ContinueThread(tid, handleException: true);
    c.Go(); // the exception stopped the whole title, not just this thread
    Console.WriteLine($"skipped fault code=0x{code:X8} at 0x{at:X8} thread=0x{tid:X8}");
}
