using System;
using System.Collections.Generic;

namespace RXDK360.SampleBrowser.Models;

/// <summary>
/// One installable sample. Existence is driven by what's on disk under
/// Source\Samples (the modernized source of truth); descriptive metadata
/// (area, description, complexity, date, screenshot, doc topic, media) is
/// enriched from the stock sampleinfo.xml when a matching entry exists.
/// </summary>
public sealed class Sample
{
    public required string Name { get; init; }

    /// <summary>Display area, e.g. "Graphics", "Natural Input", "Online".</summary>
    public required string Area { get; init; }

    /// <summary>Raw dxarea token from the catalog, e.g. "NaturalInput", "Ui".</summary>
    public required string AreaKey { get; init; }

    public string Description { get; init; } = "";

    public Difficulty Difficulty { get; init; } = Difficulty.Unknown;
    public int Complexity { get; init; }

    public int Year { get; init; }
    public int Month { get; init; }

    /// <summary>Path to the sample folder relative to Source\Samples (e.g. "NaturalInput\Fitness").</summary>
    public required string RelativeFolder { get; init; }

    /// <summary>Absolute path to the sample folder on disk.</summary>
    public required string FolderPath { get; init; }

    /// <summary>Absolute path to the sample's screenshot.jpg, or null.</summary>
    public string? ScreenshotPath { get; init; }

    /// <summary>CHM topic after "::/", e.g. "sample_NaturalInput_Fitness.htm", or null.</summary>
    public string? DocTopic { get; init; }

    /// <summary>Absolute path to the sample's .sln, or null (fallback: first .vcxproj).</summary>
    public string? SolutionPath { get; init; }

    public IReadOnlyList<string> Keywords { get; init; } = Array.Empty<string>();

    /// <summary>Media items to copy on install (relative to a shared Media root).</summary>
    public IReadOnlyList<string> InstallMedia { get; init; } = Array.Empty<string>();

    public DateTime SortDate => Year > 0 ? new DateTime(Year, Month is >= 1 and <= 12 ? Month : 1, 1) : DateTime.MinValue;

    public string DateLabel => Year > 0
        ? $"{System.Globalization.CultureInfo.InvariantCulture.DateTimeFormat.GetMonthName(Month is >= 1 and <= 12 ? Month : 1)} {Year}"
        : "";
}
