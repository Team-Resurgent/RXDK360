using System.Diagnostics;
using RXDK360.SampleBrowser.Models;

namespace RXDK360.SampleBrowser.Services;

/// <summary>Opens a sample's documentation topic in the shipped XDK CHM via hh.exe.</summary>
public sealed class DocService
{
    private readonly SdkPaths _sdk;
    public DocService(SdkPaths sdk) => _sdk = sdk;

    public bool CanOpen(Sample s) => _sdk.ChmPath is not null && !string.IsNullOrEmpty(s.DocTopic);

    public void Open(Sample s)
    {
        if (_sdk.ChmPath is null || string.IsNullOrEmpty(s.DocTopic))
            return;

        // hh.exe "C:\...\xbox360sdk.chm::/topic.htm"
        var arg = $"\"{_sdk.ChmPath}::/{s.DocTopic}\"";
        try
        {
            Process.Start(new ProcessStartInfo("hh.exe", arg) { UseShellExecute = false });
        }
        catch
        {
            // Fall back to opening the CHM at its default topic.
            try { Process.Start(new ProcessStartInfo(_sdk.ChmPath) { UseShellExecute = true }); }
            catch { /* nothing to do */ }
        }
    }
}
