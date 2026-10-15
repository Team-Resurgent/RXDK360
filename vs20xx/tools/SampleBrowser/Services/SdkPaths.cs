using System;
using System.IO;
using Microsoft.Win32;

namespace RXDK360.SampleBrowser.Services;

/// <summary>
/// Resolves the RXDK-360 install tree relative to the running executable, with
/// a registry fallback and a repo/dev fallback. The browser normally lives at
/// {app}\Source\Tools\SampleBrowser\, so {app} is three levels up.
/// </summary>
public sealed class SdkPaths
{
    public string AppRoot { get; }
    public string SamplesRoot { get; }        // {app}\Source\Samples
    public string CatalogXml { get; }         // sampleinfo.xml (next to exe, or stock)
    public string? ChmPath { get; }           // {app}\doc\1033\xbox360sdk.chm
    public string ExeDir { get; }

    private SdkPaths(string appRoot, string exeDir)
    {
        AppRoot = appRoot;
        ExeDir = exeDir;
        SamplesRoot = Path.Combine(appRoot, "Source", "Samples");

        // Catalog: prefer one shipped next to the exe, else the stock location.
        var nextToExe = Path.Combine(exeDir, "sampleinfo.xml");
        var stock = Path.Combine(appRoot, "Source", "Tools", "SampleBrowser", "sampleinfo.xml");
        CatalogXml = File.Exists(nextToExe) ? nextToExe : stock;

        var chm = Path.Combine(appRoot, "doc", "1033", "xbox360sdk.chm");
        ChmPath = File.Exists(chm) ? chm : null;
    }

    public static SdkPaths Discover()
    {
        var exeDir = AppContext.BaseDirectory.TrimEnd(Path.DirectorySeparatorChar);

        // 1) Walk up from the exe looking for a dir that contains Source\Samples.
        var root = FindAppRootFrom(exeDir);

        // 2) Registry (HKLM 32-bit view is where the installer writes InstallPath).
        root ??= ReadInstallPathFromRegistry();

        // 3) Dev fallback: repo layout (…\vs20xx\tools\SampleBrowser\bin\… -> repo).
        root ??= FindRepoDevRoot(exeDir) ?? exeDir;

        return new SdkPaths(root, exeDir);
    }

    private static string? FindAppRootFrom(string start)
    {
        var dir = new DirectoryInfo(start);
        for (int i = 0; i < 8 && dir is not null; i++, dir = dir.Parent!)
        {
            if (Directory.Exists(Path.Combine(dir.FullName, "Source", "Samples")))
                return dir.FullName;
        }
        return null;
    }

    private static string? ReadInstallPathFromRegistry()
    {
        try
        {
            using var view = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry32);
            using var key = view.OpenSubKey(@"SOFTWARE\TeamResurgent\RXDK-360");
            var p = key?.GetValue("InstallPath") as string;
            if (!string.IsNullOrEmpty(p) && Directory.Exists(Path.Combine(p, "Source", "Samples")))
                return p;
        }
        catch { /* registry not present in dev */ }
        return null;
    }

    // When running from the repo, the samples submodule is checked out at
    // <repo>\samples; expose that as a dev "Source\Samples" equivalent.
    private static string? FindRepoDevRoot(string start)
    {
        var dir = new DirectoryInfo(start);
        for (int i = 0; i < 10 && dir is not null; i++, dir = dir.Parent!)
        {
            if (Directory.Exists(Path.Combine(dir.FullName, "samples", "Common")))
                return null; // dev: handled by an override, not the packaged layout
        }
        return null;
    }
}
