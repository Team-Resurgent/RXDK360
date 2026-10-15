using System;
using System.IO;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using RXDK360.SampleBrowser.Models;

namespace RXDK360.SampleBrowser.ViewModels;

public sealed class SampleViewModel
{
    public Sample Model { get; }

    public SampleViewModel(Sample model) => Model = model;

    public string Name => Model.Name;
    public string Area => Model.Area;
    public string Description => Model.Description;
    public string DateLabel => Model.DateLabel;
    public bool HasDate => Model.Year > 0;
    public string Keywords => string.Join("  ", Model.Keywords);
    public bool HasKeywords => Model.Keywords.Count > 0;
    public bool HasDoc => Model.DocTopic is not null;

    public string DifficultyText => Model.Difficulty switch
    {
        Difficulty.Beginner => "Beginner",
        Difficulty.Intermediate => "Intermediate",
        Difficulty.Advanced => "Advanced",
        _ => "",
    };

    public bool HasDifficulty => Model.Difficulty != Difficulty.Unknown;

    public IBrush DifficultyBrush => Model.Difficulty switch
    {
        Difficulty.Beginner => new SolidColorBrush(Color.Parse("#3FB950")),
        Difficulty.Intermediate => new SolidColorBrush(Color.Parse("#D29922")),
        Difficulty.Advanced => new SolidColorBrush(Color.Parse("#F85149")),
        _ => new SolidColorBrush(Color.Parse("#6E7681")),
    };

    private Bitmap? _screenshot;
    private bool _triedLoad;
    public Bitmap? Screenshot
    {
        get
        {
            if (_triedLoad) return _screenshot;
            _triedLoad = true;
            try
            {
                if (Model.ScreenshotPath is { } p && File.Exists(p))
                    _screenshot = new Bitmap(p);
            }
            catch { _screenshot = null; }
            return _screenshot;
        }
    }

    public bool HasScreenshot => Screenshot is not null;
}
