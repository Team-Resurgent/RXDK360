using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Text.Json;

namespace RXDK360.SampleBrowser.Services;

/// <summary>A discovered Visual Studio installation (or the shell-default sentinel).</summary>
public sealed class VsInstall
{
    public required string DisplayName { get; init; }   // e.g. "Visual Studio 2022 (Enterprise)"
    public string? DevEnvPath { get; init; }            // devenv.exe, or null for shell-default
    public string Version { get; init; } = "";          // e.g. "17.11.5"
    public bool IsShellDefault => DevEnvPath is null;

    public static VsInstall ShellDefault { get; } = new()
    {
        DisplayName = "Default (file association)",
        DevEnvPath = null,
    };

    public override string ToString() => DisplayName;
}

/// <summary>
/// Enumerates installed Visual Studio instances via vswhere so the user can
/// choose which one opens a sample's solution — no registry shim required.
/// </summary>
public static class VsService
{
    public static IReadOnlyList<VsInstall> Discover()
    {
        var list = new List<VsInstall>();
        try
        {
            foreach (var inst in RunVswhere())
                list.Add(inst);
        }
        catch { /* vswhere missing -> shell default only */ }

        // Newest first, then the shell-default fallback last.
        list.Sort((a, b) => string.CompareOrdinal(b.Version, a.Version));
        list.Add(VsInstall.ShellDefault);
        return list;
    }

    private static IEnumerable<VsInstall> RunVswhere()
    {
        var pf86 = Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86);
        var vswhere = Path.Combine(pf86, "Microsoft Visual Studio", "Installer", "vswhere.exe");
        if (!File.Exists(vswhere))
            yield break;

        var psi = new ProcessStartInfo(vswhere)
        {
            RedirectStandardOutput = true,
            UseShellExecute = false,
            CreateNoWindow = true,
        };
        // -prerelease so VS 2026 previews show up; -all so BuildTools-less SKUs list too.
        foreach (var a in new[] { "-prerelease", "-all", "-utf8",
                                  "-products", "*",
                                  "-format", "json" })
            psi.ArgumentList.Add(a);

        string json;
        using (var p = Process.Start(psi)!)
        {
            json = p.StandardOutput.ReadToEnd();
            p.WaitForExit(10_000);
        }
        if (string.IsNullOrWhiteSpace(json))
            yield break;

        using var doc = JsonDocument.Parse(json);
        foreach (var el in doc.RootElement.EnumerateArray())
        {
            var devenv = el.TryGetProperty("productPath", out var pp) ? pp.GetString() : null;
            if (string.IsNullOrEmpty(devenv) || !File.Exists(devenv))
                continue;

            var display = el.TryGetProperty("displayName", out var dn) ? dn.GetString() : "Visual Studio";
            var version = el.TryGetProperty("installationVersion", out var iv) ? iv.GetString() ?? "" : "";
            var channel = el.TryGetProperty("catalog", out var cat) &&
                          cat.TryGetProperty("productLineVersion", out var plv) ? plv.GetString() : null;

            var name = channel is not null ? $"{display} ({channel})" : display;
            yield return new VsInstall { DisplayName = name!, DevEnvPath = devenv, Version = version };
        }
    }
}
