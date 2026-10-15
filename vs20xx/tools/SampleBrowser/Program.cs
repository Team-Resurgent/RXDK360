using Avalonia;
using System;

namespace RXDK360.SampleBrowser;

internal static class Program
{
    // Avalonia entry point. Keep this minimal; initialization lives in App.
    [STAThread]
    public static void Main(string[] args) => BuildAvaloniaApp()
        .StartWithClassicDesktopLifetime(args);

    public static AppBuilder BuildAvaloniaApp() => AppBuilder.Configure<App>()
        .UsePlatformDetect()
        .WithInterFont()
        .LogToTrace();
}
