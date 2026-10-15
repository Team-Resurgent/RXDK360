// SPDX-License-Identifier: GPL-3.0-or-later
// Part of RXDK-360 - see LICENSE.md for the full GNU GPL v3.
//
// P/Invoke of the XDK PC-side debug monitor. Signatures follow
// include\win32\xbdm.h (DMHRAPI = HRESULT __stdcall) from the installed
// RXDK-360 tree (%RXDK360% / InstallPath = {app}). The module loaded at
// runtime is {app}\bin\win32\xbdm.dll (PE32) — this assembly is therefore
// x86. Stock %XEDK% is only a fallback. We do not reimplement the XBDM wire
// protocol.

using System;
using System.Runtime.InteropServices;
using System.Text;

namespace Rxdk.Xbox360.Xbdm
{
    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    internal delegate uint DmNotifyFunction(uint notification, UIntPtr param);

    [UnmanagedFunctionPointer(CallingConvention.StdCall, CharSet = CharSet.Ansi)]
    internal delegate uint DmExtNotifyFunction([MarshalAs(UnmanagedType.LPStr)] string notification);

    internal static class NativeXbdm
    {
        internal const string Dll = "xbdm.dll";
        private const CallingConvention Cc = CallingConvention.StdCall;

        // ---- connection ----
        [DllImport(Dll, CallingConvention = Cc, CharSet = CharSet.Ansi)]
        public static extern int DmSetXboxNameNoRegister(string name);
        [DllImport(Dll, CallingConvention = Cc, CharSet = CharSet.Ansi)]
        public static extern int DmSetXboxName(string name);
        [DllImport(Dll, CallingConvention = Cc, CharSet = CharSet.Ansi)]
        public static extern int DmGetXboxName(StringBuilder name, ref uint size);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int DmSetConnectionTimeout(uint connectTimeout, uint conversationTimeout);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int DmUseSharedConnection([MarshalAs(UnmanagedType.Bool)] bool enable);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int DmOpenConnection(out IntPtr connection);
        [DllImport(Dll, CallingConvention = Cc, CharSet = CharSet.Ansi)]
        public static extern int DmOpenSecureConnection(out IntPtr connection, string? password);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int DmCloseConnection(IntPtr connection);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int DmResolveXboxName(out uint address);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int DmGetConsoleType(out uint consoleType);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int DmGetConsoleFeatures(out uint features);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int DmGetSystemInfo(ref DmSystemInfo info);

        // ---- debugger session ----
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int DmConnectDebugger([MarshalAs(UnmanagedType.Bool)] bool connect);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int DmIsDebuggerPresent();
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int DmStopOn(uint stopFlags, [MarshalAs(UnmanagedType.Bool)] bool stop);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int DmSetInitialBreakpoint();

        // ---- notifications ----
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int DmOpenNotificationSession(uint flags, out IntPtr session);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int DmCloseNotificationSession(IntPtr session);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int DmNotify(IntPtr session, uint notification, DmNotifyFunction? handler);
        [DllImport(Dll, CallingConvention = Cc, CharSet = CharSet.Ansi)]
        public static extern int DmRegisterNotificationProcessor(IntPtr session, string type, DmExtNotifyFunction pfn);

        // ---- execution control ----
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int DmStop();
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int DmGo();
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int DmHaltThread(uint threadId);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int DmContinueThread(uint threadId, [MarshalAs(UnmanagedType.Bool)] bool handleException);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int DmSuspendThread(uint threadId);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int DmResumeThread(uint threadId);

        // ---- breakpoints ----
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int DmSetBreakpoint(UIntPtr addr);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int DmRemoveBreakpoint(UIntPtr addr);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int DmSetDataBreakpoint(UIntPtr addr, uint type, uint size);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int DmIsBreakpoint(UIntPtr addr, out uint type);

        // ---- memory ----
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int DmGetMemory(UIntPtr addr, uint cb, [Out] byte[] buf, out uint cbRet);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int DmSetMemory(UIntPtr addr, uint cb, byte[] buf, out uint cbRet);

        // ---- threads ----
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int DmGetThreadList([Out] uint[]? threads, ref uint count);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int DmGetThreadContext(uint threadId, ref XContext context);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int DmSetThreadContext(uint threadId, ref XContext context);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int DmIsThreadStopped(uint threadId, out DmThreadStop stop);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int DmGetThreadInfoEx(uint threadId, ref DmThreadInfoEx info);

        // ---- modules ----
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int DmWalkLoadedModules(ref IntPtr walk, out DmnModLoad module);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int DmCloseLoadedModules(IntPtr walk);

        // ---- files / deploy ----
        [DllImport(Dll, CallingConvention = Cc, CharSet = CharSet.Ansi, EntryPoint = "DmSendFileA")]
        public static extern int DmSendFile(string localName, string remoteName);
        [DllImport(Dll, CallingConvention = Cc, CharSet = CharSet.Ansi)]
        public static extern int DmMkdir(string directoryName);
        [DllImport(Dll, CallingConvention = Cc, CharSet = CharSet.Ansi)]
        public static extern int DmDeleteFile(string fileName, [MarshalAs(UnmanagedType.Bool)] bool isDirectory);
        [DllImport(Dll, CallingConvention = Cc, CharSet = CharSet.Ansi, EntryPoint = "DmReceiveFileA")]
        public static extern int DmReceiveFile(string localName, string remoteName);
        [DllImport(Dll, CallingConvention = Cc, CharSet = CharSet.Ansi)]
        public static extern int DmWalkDir(ref IntPtr walk, string directory, out DmFileAttributes attributes);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int DmCloseDir(IntPtr walk);

        // ---- reboot / launch ----
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int DmReboot(uint flags);
        [DllImport(Dll, CallingConvention = Cc, CharSet = CharSet.Ansi)]
        public static extern int DmRebootEx(uint flags, string? imagePath, string? mediaPath, string? dbgCmdLine);
        [DllImport(Dll, CallingConvention = Cc, CharSet = CharSet.Ansi)]
        public static extern int DmSetTitle(string? dir, string title, string? cmdLine);
        [DllImport(Dll, CallingConvention = Cc, CharSet = CharSet.Ansi)]
        public static extern int DmSetTitleEx(string? dir, string title, string? cmdLine, uint flags);

        // ---- screenshot ----
        [DllImport(Dll, CallingConvention = Cc, CharSet = CharSet.Ansi)]
        public static extern int DmScreenShot(string filename);

        // ---- errors ----
        [DllImport(Dll, CallingConvention = Cc, CharSet = CharSet.Ansi, EntryPoint = "DmTranslateErrorA")]
        public static extern int DmTranslateError(int hr, StringBuilder buffer, int bufferMax);
    }

    /// <summary>Notification kinds (DmNotify / PDM_NOTIFY_FUNCTION).</summary>
    public static class DmNotification
    {
        public const uint None = 0;
        public const uint Break = 1;
        public const uint DebugStr = 2;
        public const uint Exec = 3;
        public const uint SingleStep = 4;
        public const uint ModLoad = 5;
        public const uint ModUnload = 6;
        public const uint CreateThread = 7;
        public const uint DestroyThread = 8;
        public const uint Exception = 9;
        public const uint Assert = 12;
        public const uint DataBreak = 13;
        public const uint Rip = 14;
        public const uint SectionLoad = 16;
        public const uint SectionUnload = 17;
        public const uint Fiber = 18;
        public const uint StackTrace = 19;
        public const uint BugCheck = 20;
        public const uint AssertionFailure = 21;
        public const uint Mask = 0x00FFFFFF;
        public const uint StopThread = 0x80000000;
    }

    public static class DmNotifySession
    {
        public const uint Persistent = 0x00000001;
        public const uint DebugSession = 0x00000002;
        public const uint AsyncSession = 0x00000004;
    }

    public static class DmStop
    {
        public const uint CreateThread = 0x00000001;
        public const uint Fce = 0x00000002;
        public const uint DebugStr = 0x00000004;
        public const uint StackTrace = 0x00000008;
        public const uint ModLoad = 0x00000010;
        public const uint TitleLaunch = 0x00000020;
        public const uint PgoModStartup = 0x00000040;
        public const uint All = CreateThread | Fce | DebugStr | StackTrace | ModLoad | TitleLaunch | PgoModStartup;
    }

    public static class DmBoot
    {
        public const uint Title = 0;
        public const uint Wait = 1;
        public const uint Warm = 2;
        public const uint Cold = 4;
        public const uint Stop = 8;
    }

    public static class DmBreak
    {
        public const uint None = 0, Write = 1, ReadWrite = 2, Execute = 3, Fixed = 4, Read = 5;
    }

    public static class DmExec
    {
        public const uint Stop = 0;
        public const uint Start = 1;
        public const uint Reboot = 2;
        public const uint Pending = 3;
        public const uint RebootTitle = 4;
        public const uint PendingTitle = 5;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    public struct DmnBreak
    {
        public UIntPtr Address;
        public uint ThreadId;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    public struct DmnDataBreak
    {
        public UIntPtr Address;
        public uint ThreadId;
        public uint BreakType;
        public UIntPtr DataAddress;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    public struct DmnDebugStr
    {
        public uint ThreadId;
        public uint Length;
        public IntPtr String;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8, CharSet = CharSet.Ansi)]
    public struct DmnModLoad
    {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)]
        public string Name;
        public UIntPtr BaseAddress;
        public uint Size;
        public uint TimeStamp;
        public uint CheckSum;
        public uint Flags;
        public UIntPtr PDataAddress;
        public uint PDataSize;
        public uint ThreadId;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    public struct DmnCreateThread
    {
        public uint ThreadId;
        public UIntPtr StartAddress;
        public IntPtr ThreadNameAddress;
    }

    /// <summary>DM_FILE_ATTRIBUTES (xbdm.h): one DmWalkDir entry.</summary>
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
    public struct DmFileAttributes
    {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)]
        public string Name;
        public long CreationTime;   // FILETIME
        public long ChangeTime;     // FILETIME
        public uint SizeHigh;
        public uint SizeLow;
        public uint Attributes;     // FILE_ATTRIBUTE_*: 0x10 = directory
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    public struct DmnException
    {
        public uint ThreadId;
        public uint Code;
        public UIntPtr Address;
        public uint Flags;
        public uint Information0;
        public uint Information1;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    public struct DmnBugCheck
    {
        public uint Data0, Data1, Data2, Data3, Data4;
    }

    [StructLayout(LayoutKind.Explicit, Pack = 8)]
    public struct DmThreadStop
    {
        [FieldOffset(0)] public uint NotifiedReason;
        [FieldOffset(4)] public DmnBreak Break;
        [FieldOffset(4)] public DmnDataBreak DataBreak;
        [FieldOffset(4)] public DmnException Exception;
        [FieldOffset(4)] public DmnDebugStr DebugStr;
        [FieldOffset(4)] public DmnBugCheck BugCheck;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    public struct DmThreadInfoEx
    {
        public uint Size;
        public uint SuspendCount;
        public uint Priority;
        public UIntPtr TlsBase;
        public UIntPtr StartAddress;
        public UIntPtr StackBase;
        public UIntPtr StackLimit;
        public uint CreateTimeLow;
        public uint CreateTimeHigh;
        public uint StackSlackSpace;
        public IntPtr ThreadNameAddress;
        public uint ThreadNameLength;
        public byte CurrentProcessor;
        public uint LastError;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    public struct DmVersionInfo
    {
        public ushort Major, Minor, Build, Qfe;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    public struct DmSystemInfo
    {
        public int SizeOfStruct;
        public DmVersionInfo BaseKernelVersion;
        public DmVersionInfo KernelVersion;
        public DmVersionInfo XdkVersion;
        public uint Flags;
    }
}
