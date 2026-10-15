using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Xml.Linq;
using RXDK360.SampleBrowser.Models;

namespace RXDK360.SampleBrowser.Services;

/// <summary>
/// Builds the sample list: disk is the source of truth for what exists
/// (Source\Samples\**\*.vcxproj), enriched with metadata from sampleinfo.xml.
/// </summary>
public sealed class CatalogService
{
    private readonly SdkPaths _sdk;

    public CatalogService(SdkPaths sdk) => _sdk = sdk;

    public IReadOnlyList<Sample> Load()
    {
        var meta = LoadCatalogMeta();               // key: normalized "area\name"
        var byName = meta.Values
            .GroupBy(m => Norm(m.Name))
            .ToDictionary(g => g.Key, g => g.First());

        var samples = new List<Sample>();
        if (!Directory.Exists(_sdk.SamplesRoot))
            return samples;

        foreach (var proj in EnumerateSampleProjects())
        {
            var folder = Path.GetDirectoryName(proj)!;
            var rel = Path.GetRelativePath(_sdk.SamplesRoot, folder);
            var relKey = Norm(rel);
            var name = Path.GetFileName(folder);

            CatalogEntry? m = null;
            if (!meta.TryGetValue(relKey, out m))
                byName.TryGetValue(Norm(name), out m);

            // Skip anything the catalog marks as a non-sample (article/link).
            if (m is not null && !string.Equals(m.Type, "sample", StringComparison.OrdinalIgnoreCase)
                              && !string.IsNullOrEmpty(m.Type))
                continue;

            var areaKey = m?.Area ?? FirstSegment(rel);
            var screenshot = FindScreenshot(folder, m);
            var sln = Directory.EnumerateFiles(folder, "*.sln").FirstOrDefault();

            samples.Add(new Sample
            {
                Name = name,
                AreaKey = areaKey,
                Area = FriendlyArea(areaKey),
                Description = (m?.Description ?? "").Trim(),
                Complexity = m?.Complexity ?? 0,
                Difficulty = DifficultyMap.FromComplexity(m?.Complexity ?? 0),
                Year = m?.Year ?? 0,
                Month = m?.Month ?? 0,
                RelativeFolder = rel,
                FolderPath = folder,
                ScreenshotPath = screenshot,
                DocTopic = m?.DocTopic,
                SolutionPath = sln ?? proj,
                Keywords = m?.Keywords ?? Array.Empty<string>(),
                InstallMedia = m?.Media ?? Array.Empty<string>(),
            });
        }

        return samples
            .OrderBy(s => s.Area, StringComparer.OrdinalIgnoreCase)
            .ThenBy(s => s.Name, StringComparer.OrdinalIgnoreCase)
            .ToList();
    }

    // One Application .vcxproj per sample folder; skip the shared Common project.
    private IEnumerable<string> EnumerateSampleProjects()
    {
        foreach (var proj in Directory.EnumerateFiles(_sdk.SamplesRoot, "*.vcxproj", SearchOption.AllDirectories))
        {
            var folder = Path.GetDirectoryName(proj)!;
            var folderName = Path.GetFileName(folder);
            if (folderName.Equals("Common", StringComparison.OrdinalIgnoreCase))
                continue;
            // Prefer the project whose name matches its folder (the sample itself).
            if (!Path.GetFileNameWithoutExtension(proj).Equals(folderName, StringComparison.OrdinalIgnoreCase))
                continue;
            yield return proj;
        }
    }

    private static string? FindScreenshot(string folder, CatalogEntry? m)
    {
        foreach (var candidate in new[] { "screenshot.jpg", "screenshot.png", "thumbnail.jpg" })
        {
            var p = Path.Combine(folder, candidate);
            if (File.Exists(p)) return p;
        }
        return null;
    }

    // ---- sampleinfo.xml parsing -------------------------------------------

    private sealed class CatalogEntry
    {
        public string Name = "";
        public string Area = "";
        public string Type = "";
        public string Description = "";
        public int Complexity;
        public int Year;
        public int Month;
        public string? DocTopic;
        public string[] Keywords = Array.Empty<string>();
        public string[] Media = Array.Empty<string>();
    }

    private Dictionary<string, CatalogEntry> LoadCatalogMeta()
    {
        var map = new Dictionary<string, CatalogEntry>(StringComparer.OrdinalIgnoreCase);
        if (!File.Exists(_sdk.CatalogXml))
            return map;

        XDocument doc;
        try { doc = XDocument.Load(_sdk.CatalogXml); }
        catch { return map; } // malformed catalog -> disk-only, no enrichment

        foreach (var s in doc.Descendants("sample"))
        {
            var name = (string?)s.Element("name") ?? "";
            if (string.IsNullOrWhiteSpace(name)) continue;

            var folder = ((string?)s.Element("folder") ?? "").Trim();
            var relKey = FolderKey(folder);

            var entry = new CatalogEntry
            {
                Name = name.Trim(),
                Area = ((string?)s.Element("dxarea") ?? "").Trim(),
                Type = ((string?)s.Element("type") ?? "").Trim(),
                Description = ((string?)s.Element("description") ?? "").Trim(),
                Complexity = ParseInt(s.Element("complexity")),
                Year = ParseInt(s.Element("yearadded")),
                Month = ParseInt(s.Element("monthadded")),
                DocTopic = ParseDocTopic((string?)s.Element("doc")),
                Keywords = s.Element("keywords")?.Elements("keyword")
                            .Select(k => k.Value.Trim()).Where(v => v.Length > 0).ToArray()
                           ?? Array.Empty<string>(),
                Media = s.Element("install")?.Elements("media")
                            .Select(m => m.Value.Trim()).Where(v => v.Length > 0).ToArray()
                        ?? Array.Empty<string>(),
            };

            if (!string.IsNullOrEmpty(relKey))
                map[relKey] = entry;
        }
        return map;
    }

    // "Samples\NaturalInput\Fitness\" -> "naturalinput\fitness"
    private static string FolderKey(string folder)
    {
        var f = folder.Replace('/', '\\').Trim('\\');
        const string prefix = "Samples\\";
        if (f.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
            f = f[prefix.Length..];
        return Norm(f);
    }

    private static string Norm(string p) =>
        p.Replace('/', '\\').Trim('\\').ToLowerInvariant();

    private static string FirstSegment(string rel) =>
        rel.Replace('/', '\\').Split('\\', StringSplitOptions.RemoveEmptyEntries).FirstOrDefault() ?? "";

    // "..\doc\1033\xbox360sdk.chm::/sample_Foo.htm" -> "sample_Foo.htm"
    private static string? ParseDocTopic(string? doc)
    {
        if (string.IsNullOrWhiteSpace(doc)) return null;
        var idx = doc.IndexOf("::/", StringComparison.Ordinal);
        return idx >= 0 ? doc[(idx + 3)..].Trim() : null;
    }

    private static int ParseInt(XElement? el) =>
        int.TryParse((el?.Value ?? "").Trim(), out var v) ? v : 0;

    private static string FriendlyArea(string key) => key.ToLowerInvariant() switch
    {
        "naturalinput" => "Natural Input",
        "ui" => "UI",
        "" => "Other",
        _ => char.ToUpperInvariant(key[0]) + key[1..],
    };
}
