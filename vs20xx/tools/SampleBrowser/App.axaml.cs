using Avalonia;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Markup.Xaml;
using RXDK360.SampleBrowser.Services;
using RXDK360.SampleBrowser.ViewModels;
using RXDK360.SampleBrowser.Views;

namespace RXDK360.SampleBrowser;

public partial class App : Application
{
    public override void Initialize() => AvaloniaXamlLoader.Load(this);

    public override void OnFrameworkInitializationCompleted()
    {
        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
        {
            var sdk = SdkPaths.Discover();
            var vm = new MainViewModel(sdk);
            desktop.MainWindow = new MainWindow { DataContext = vm };
        }

        base.OnFrameworkInitializationCompleted();
    }
}
