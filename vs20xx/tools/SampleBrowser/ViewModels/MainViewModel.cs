using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.IO;
using System.Linq;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using RXDK360.SampleBrowser.Models;
using RXDK360.SampleBrowser.Services;

namespace RXDK360.SampleBrowser.ViewModels;

public enum SortMode { Date, Name, Difficulty }

public partial class MainViewModel : ObservableObject
{
    private readonly CatalogService _catalog;
    private readonly InstallService _installer;
    private readonly DocService _docs;

    private readonly List<SampleViewModel> _all = new();

    public ObservableCollection<SampleViewModel> Samples { get; } = new();
    public ObservableCollection<string> Areas { get; } = new();
    public ObservableCollection<VsInstall> VsInstalls { get; } = new();

    public IReadOnlyList<string> SortModes { get; } = new[] { "Date", "Name", "Difficulty" };

    [ObservableProperty] private string _searchText = "";
    [ObservableProperty] private string _selectedArea = "All Areas";
    [ObservableProperty] private string _selectedSort = "Name";
    [ObservableProperty] private VsInstall? _selectedVs;
    [ObservableProperty] private int _visibleCount;
    [ObservableProperty] private string _headerSubtitle = "";

    // ---- install flow state ----
    [ObservableProperty] private SampleViewModel? _installTarget;
    [ObservableProperty] private string _installName = "";
    [ObservableProperty] private string _installLocation = "";
    [ObservableProperty] private bool _isInstalling;
    [ObservableProperty] private string _statusText = "";
    public bool InstallPanelOpen => InstallTarget is not null;
    public bool HasStatus => !string.IsNullOrEmpty(StatusText);

    public MainViewModel(SdkPaths sdk)
    {
        _catalog = new CatalogService(sdk);
        _installer = new InstallService(sdk);
        _docs = new DocService(sdk);

        foreach (var vs in VsService.Discover())
            VsInstalls.Add(vs);
        SelectedVs = VsInstalls.FirstOrDefault();

        InstallLocation = DefaultInstallRoot();

        LoadCatalog();
    }

    private static string DefaultInstallRoot()
    {
        var docs = Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments);
        return Path.Combine(docs, "RXDK-360 Samples");
    }

    private void LoadCatalog()
    {
        _all.Clear();
        foreach (var s in _catalog.Load())
            _all.Add(new SampleViewModel(s));

        Areas.Clear();
        Areas.Add("All Areas");
        foreach (var area in _all.Select(s => s.Area).Distinct().OrderBy(a => a, StringComparer.OrdinalIgnoreCase))
            Areas.Add(area);

        HeaderSubtitle = $"{_all.Count} samples";
        ApplyFilter();
    }

    partial void OnSearchTextChanged(string value) => ApplyFilter();
    partial void OnSelectedAreaChanged(string value) => ApplyFilter();
    partial void OnSelectedSortChanged(string value) => ApplyFilter();
    partial void OnInstallTargetChanged(SampleViewModel? value) => OnPropertyChanged(nameof(InstallPanelOpen));
    partial void OnStatusTextChanged(string value) => OnPropertyChanged(nameof(HasStatus));

    private void ApplyFilter()
    {
        IEnumerable<SampleViewModel> q = _all;

        if (!string.IsNullOrWhiteSpace(SelectedArea) && SelectedArea != "All Areas")
            q = q.Where(s => string.Equals(s.Area, SelectedArea, StringComparison.OrdinalIgnoreCase));

        if (!string.IsNullOrWhiteSpace(SearchText))
        {
            var t = SearchText.Trim();
            q = q.Where(s =>
                s.Name.Contains(t, StringComparison.OrdinalIgnoreCase) ||
                s.Description.Contains(t, StringComparison.OrdinalIgnoreCase) ||
                s.Keywords.Contains(t, StringComparison.OrdinalIgnoreCase));
        }

        q = ParseSort(SelectedSort) switch
        {
            SortMode.Date => q.OrderByDescending(s => s.Model.SortDate).ThenBy(s => s.Name, StringComparer.OrdinalIgnoreCase),
            SortMode.Difficulty => q.OrderBy(s => (int)s.Model.Difficulty == 0 ? int.MaxValue : (int)s.Model.Difficulty)
                                     .ThenBy(s => s.Name, StringComparer.OrdinalIgnoreCase),
            _ => q.OrderBy(s => s.Name, StringComparer.OrdinalIgnoreCase),
        };

        Samples.Clear();
        foreach (var s in q) Samples.Add(s);
        VisibleCount = Samples.Count;
    }

    private static SortMode ParseSort(string s) => s switch
    {
        "Date" => SortMode.Date,
        "Difficulty" => SortMode.Difficulty,
        _ => SortMode.Name,
    };

    // ---- commands ----------------------------------------------------------

    [RelayCommand]
    private void OpenDoc(SampleViewModel? vm)
    {
        if (vm is not null) _docs.Open(vm.Model);
    }

    [RelayCommand]
    private void BeginInstall(SampleViewModel? vm)
    {
        if (vm is null) return;
        InstallTarget = vm;
        InstallName = vm.Name;
        StatusText = "";
    }

    [RelayCommand]
    private void CancelInstall()
    {
        InstallTarget = null;
        StatusText = "";
    }

    [RelayCommand]
    private async System.Threading.Tasks.Task ConfirmInstall()
    {
        if (InstallTarget is null) return;
        IsInstalling = true;
        StatusText = "Installing…";

        var result = await _installer.InstallAsync(InstallTarget.Model, InstallLocation, InstallName, SelectedVs);

        IsInstalling = false;
        if (result.Success)
        {
            StatusText = $"Installed to {result.Message}";
            InstallTarget = null;
        }
        else
        {
            StatusText = $"Install failed: {result.Message}";
        }
    }
}
