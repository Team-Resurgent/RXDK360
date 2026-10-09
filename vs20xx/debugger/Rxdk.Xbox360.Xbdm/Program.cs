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
