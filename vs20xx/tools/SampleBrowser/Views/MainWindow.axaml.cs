using System;
using System.Linq;
using Avalonia.Controls;
using Avalonia.Interactivity;
using Avalonia.Markup.Xaml;
using Avalonia.Platform.Storage;
using RXDK360.SampleBrowser.ViewModels;

namespace RXDK360.SampleBrowser.Views;

public partial class MainWindow : Window
{
    public MainWindow() => AvaloniaXamlLoader.Load(this);

    private async void OnBrowse(object? sender, RoutedEventArgs e)
    {
        if (DataContext is not MainViewModel vm) return;

        IStorageFolder? start = null;
        try
        {
            if (!string.IsNullOrWhiteSpace(vm.InstallLocation))
                start = await StorageProvider.TryGetFolderFromPathAsync(vm.InstallLocation);
        }
        catch { /* ignore */ }

        var picked = await StorageProvider.OpenFolderPickerAsync(new FolderPickerOpenOptions
        {
            Title = "Choose install location",
            AllowMultiple = false,
            SuggestedStartLocation = start,
        });

        var folder = picked.FirstOrDefault();
        if (folder?.TryGetLocalPath() is { } path && !string.IsNullOrEmpty(path))
            vm.InstallLocation = path;
    }
}
