using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using GeoModelBridge.Gui.Core;
using Microsoft.Win32;

namespace GeoModelBridge.Gui;

public partial class MainWindow : Window
{
    private readonly EngineService _engine = new();
    private bool _running;
    private string _lastOutput = "";
    private string _lastReport = "";
    private string _suggestedOutput = "";
    private string _demoSourcePath = "";

    public MainWindow() => InitializeComponent();

    private void Window_Loaded(object sender, RoutedEventArgs e)
    {
        AppendLog($"GeoModelBridge V{ProductInfo.Version} 已启动。");
        if (!_engine.IsEnginePresent)
            SetStatus("缺少转换引擎", "请将 GUI 与 geomodelbridge.exe 保持在同一个文件夹，并保留后端目录。", true);
        else
            AppendLog("请选择 FBX 或点击“加载演示”。");
    }

    private const string Backend = "native-filegdb";
    private string Profile => (ProfileBox.SelectedItem as ComboBoxItem)?.Tag?.ToString() ?? "gis-static";

    private ConversionSettings Settings() => new()
    {
        InputPath = InputPathBox.Text.Trim(),
        OutputPath = OutputPathBox.Text.Trim(),
        Wkid = WkidBox.Text.Trim(),
        OriginX = OriginXBox.Text.Trim(),
        OriginY = OriginYBox.Text.Trim(),
        OriginZ = OriginZBox.Text.Trim(),
        FeatureClass = FeatureClassBox.Text.Trim(),
        Backend = Backend,
        Profile = Profile,
        MissingTexturePolicy = MissingTextureFallbackBox.IsChecked == true ? "material-color" : "error",
        TextureDirectories = TextureDirectoriesBox.Text.Split(new[] { '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
    };

    private void SetStatus(string title, string message, bool error = false, bool warning = false)
    {
        StatusTitle.Text = title;
        StatusTitle.Foreground = new SolidColorBrush((Color)ColorConverter.ConvertFromString(error ? "#AF3C32" : warning ? "#996516" : "#087F8C"));
        StatusMessage.Text = message;
    }

    private void AppendLog(string message)
    {
        if (string.IsNullOrWhiteSpace(message)) return;
        // Bound the incoming entry too; a single long message must not bypass the view limit.
        if (message.Length > 8_000) message = message[..8_000] + "… [此条日志已截断，完整诊断请查看报告]";
        if (LogBox.Text.Length + message.Length + 20 > 120_000)
            LogBox.Text = "[较早的日志已折叠，完整结果请查看转换报告]\n" + LogBox.Text[^80_000..];
        LogBox.AppendText($"[{DateTime.Now:HH:mm:ss}] {message.TrimEnd()}\n");
        LogBox.ScrollToEnd();
    }

    private IProgress<EngineEvent> Progress() => new Progress<EngineEvent>(item =>
    {
        AppendLog(item.Message);
        if (item.Kind == EngineEventKind.Stage && _running) StatusMessage.Text = item.Message;
    });

    private void SetBusy(bool busy)
    {
        _running = busy;
        InputPanel.IsEnabled = !busy;
        ConvertButton.IsEnabled = !busy;
        DemoButton.IsEnabled = !busy;
        ProbeButton.IsEnabled = !busy;
        BusyIndicator.Visibility = busy ? Visibility.Visible : Visibility.Collapsed;
        ConvertButton.Content = busy ? "正在处理…" : "开始转换 →";
    }

    private async void Convert_Click(object sender, RoutedEventArgs e)
    {
        if (_running) return;
        var settings = Settings();
        var errors = ConversionValidator.Validate(settings);
        if (errors.Count != 0)
        {
            var message = string.Join("\n", errors);
            if (errors.Any(error => error.Contains("已存在", StringComparison.Ordinal)))
            {
                message += "\n可点击输出路径旁的“换个新名称”，保留已有数据与报告，再重新开始。";
                try
                {
                    var existingReport = ConversionCommand.GetReportPath(settings);
                    if (File.Exists(existingReport)) { _lastReport = existingReport; ReportButton.IsEnabled = true; }
                }
                catch (Exception ex) when (ex is ArgumentException or IOException or NotSupportedException) { }
            }
            SetStatus("请完善设置", message, true);
            AppendLog(string.Join("\n", errors));
            return;
        }
        SetBusy(true);
        _lastReport = _lastOutput = "";
        OpenOutputButton.IsEnabled = ReportButton.IsEnabled = false;
        LogBox.Clear();
        SetStatus("正在转换", "正在读取模型、检查材质并创建新数据库。请保留此窗口。");
        var elapsed = Stopwatch.StartNew();
        try
        {
            var result = await _engine.ConvertAsync(settings, Progress());
            _lastReport = result.ReportPath;
            ReportButton.IsEnabled = File.Exists(_lastReport);
            if (result.Success && result.Report is not null)
            {
                _lastOutput = result.Report.OutputPath;
                OpenOutputButton.IsEnabled = Directory.Exists(_lastOutput);
                var summary = result.Report.Summary;
                var message = $"已生成 {result.Report.FeatureCount} 个要素，数据库重新打开核验通过。\n耗时 {elapsed.Elapsed.TotalSeconds:F1} 秒。";
                if (summary is not null && summary.Groups.Count > 0) message += "\n" + summary.SummaryText;
                message += "\n颜色、透明效果及接缝仍需在目标软件中检查。";
                SetStatus(summary?.HasCompatibilityAdjustments == true ? "完成（有兼容处理）" : "转换完成", message,
                    warning: summary?.HasCompatibilityAdjustments == true);
                if (summary is not null && summary.Groups.Count > 0) AppendLog(summary.SummaryText);
                AppendLog("输出：" + _lastOutput);
                AppendLog("报告：" + _lastReport);
            }
            else
            {
                SetStatus("转换未完成", result.Message + (File.Exists(_lastReport)
                    ? "\n已保留本次报告。重试前可点“换个新名称”，避免覆盖。" : ""), true);
                AppendLog(result.Message);
            }
        }
        catch (Exception ex)
        {
            SetStatus("转换未完成", "发生错误：" + ex.Message, true);
            AppendLog(ex.ToString());
        }
        finally { SetBusy(false); }
    }

    private async void Probe_Click(object sender, RoutedEventArgs e)
    {
        if (_running) return;
        SetBusy(true);
        SetStatus("检查环境", "正在检查转换程序及所选后端。");
        try
        {
            var result = await _engine.ProbeBackendAsync(Backend, Progress());
            SetStatus(result.Success ? "环境可用" : "环境未就绪", result.Message, !result.Success);
            AppendLog(result.Message);
        }
        catch (Exception ex) { SetStatus("检查未完成", ex.Message, true); AppendLog(ex.Message); }
        finally { SetBusy(false); }
    }

    private void BrowseInput_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFileDialog { Title = "选择 FBX 模型", Filter = "FBX 模型 (*.fbx)|*.fbx", CheckFileExists = true, Multiselect = false };
        if (dialog.ShowDialog(this) == true) { InputPathBox.Text = dialog.FileName; DemoNotice.Visibility = Visibility.Collapsed; }
    }

    private void InputPath_Changed(object sender, TextChangedEventArgs e)
    {
        if (OutputPathBox is null || _running) return;
        var input = InputPathBox.Text.Trim();
        if (_demoSourcePath.Length > 0 && !string.Equals(input, _demoSourcePath, StringComparison.OrdinalIgnoreCase))
        {
            _demoSourcePath = "";
            WkidBox.Clear(); OriginXBox.Clear(); OriginYBox.Clear(); OriginZBox.Clear();
            DemoNotice.Visibility = Visibility.Collapsed;
            SetStatus("请填写模型位置", "已切换模型并清除演示坐标，请按新模型的实际坐标填写。");
        }
        if (!File.Exists(input) || !string.Equals(Path.GetExtension(input), ".fbx", StringComparison.OrdinalIgnoreCase)) return;
        if (string.IsNullOrWhiteSpace(OutputPathBox.Text) || OutputPathBox.Text == _suggestedOutput)
        {
            _suggestedOutput = NewOutput(Path.GetDirectoryName(Path.GetFullPath(input))!, Path.GetFileNameWithoutExtension(input));
            OutputPathBox.Text = _suggestedOutput;
        }
    }

    private static string NewOutput(string directory, string name)
    {
        var path = Path.Combine(directory, name + ".gdb");
        var count = 1;
        while (Directory.Exists(path) || File.Exists(path) || File.Exists(path + ".report.json") || Directory.Exists(path + ".report.json"))
            path = Path.Combine(directory, name + "_" + count++ + ".gdb");
        return path;
    }

    private void BrowseOutput_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFolderDialog { Title = "选择新数据库的保存位置" };
        if (dialog.ShowDialog(this) != true) return;
        try
        {
            var name = Path.GetFileNameWithoutExtension(InputPathBox.Text.Trim());
            _suggestedOutput = NewOutput(dialog.FolderName, string.IsNullOrWhiteSpace(name) ? "ConvertedModel" : name);
            OutputPathBox.Text = _suggestedOutput;
        }
        catch (Exception ex) { SetStatus("无法设置输出", ex.Message, true); }
    }

    private void NewOutput_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            if (string.IsNullOrWhiteSpace(OutputPathBox.Text)) { SetStatus("请先选择输出位置", "选择保存目录或填写希望使用的 .gdb 路径后，再生成新名称。", true); return; }
            var current = Path.GetFullPath(OutputPathBox.Text.Trim());
            _suggestedOutput = NewOutput(Path.GetDirectoryName(current)!, Path.GetFileNameWithoutExtension(current));
            OutputPathBox.Text = _suggestedOutput;
            SetStatus("输出名称已准备", "已选择未占用的新路径，原有数据库和报告均保留。确认模型位置与转换策略后，可重新开始。");
            AppendLog("新输出路径：" + _suggestedOutput);
        }
        catch (Exception ex) { SetStatus("无法生成新名称", "请检查输出路径：" + ex.Message, true); }
    }

    private void AddTexture_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFolderDialog { Title = "添加外置贴图目录" };
        if (dialog.ShowDialog(this) == true)
            TextureDirectoriesBox.Text = string.Join(Environment.NewLine, TextureDirectoriesBox.Text.Split(new[] { '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries).Append(dialog.FolderName).Distinct(StringComparer.OrdinalIgnoreCase));
    }


    private void Profile_Changed(object sender, SelectionChangedEventArgs e)
    {
        if (ProfileHint is null) return;
        ProfileHint.Text = Profile == "gis-static"
            ? "按 FBX 保存的姿态转换；保留几何、常规颜色、漫反射贴图和 UV；省略环境光、高光及反射，删除零面积面；修复符合条件的 JPEG 封装，不重新压缩图像。处理均写入报告。"
            : "遇到实际动画、不能保留的渲染通道、退化面或需修复封装的 JPEG 时停止，并说明原因。已知默认无效属性可正常通过。";
        ProfileHint.Foreground = new SolidColorBrush((Color)ColorConverter.ConvertFromString(Profile == "gis-static" ? "#795415" : "#647B86"));
    }

    private void Demo_Click(object sender, RoutedEventArgs e)
    {
        var sample = Path.Combine(AppContext.BaseDirectory, "demo", "textured_quad.fbx");
        if (!File.Exists(sample)) sample = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "..", "tests", "fixtures", "textured_quad.fbx"));
        if (!File.Exists(sample)) { SetStatus("未找到演示模型", "请保留程序目录中的 demo 文件夹，或手动选择 FBX。", true); return; }
        _demoSourcePath = sample;
        InputPathBox.Text = sample;
        _suggestedOutput = NewOutput(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "GeoModelBridge"), "演示模型_" + DateTime.Now.ToString("yyyyMMdd_HHmmss"));
        OutputPathBox.Text = _suggestedOutput;
        WkidBox.Text = "32650";
        OriginXBox.Text = "500000";
        OriginYBox.Text = "3000000";
        OriginZBox.Text = "100";
        FeatureClassBox.Text = "Models";
        ProfileBox.SelectedIndex = 0;
        MissingTextureFallbackBox.IsChecked = true;
        TextureDirectoriesBox.Clear();
        DemoNotice.Visibility = Visibility.Visible;
        SetStatus("演示已准备", "已选择带贴图的测试模型。点击“开始转换”即可验证；演示坐标不代表真实位置。");
        AppendLog("演示模型已加载，尚未开始转换。");
    }

    private void Input_DragOver(object sender, DragEventArgs e)
    {
        e.Effects = !_running && e.Data.GetDataPresent(DataFormats.FileDrop) ? DragDropEffects.Copy : DragDropEffects.None;
        e.Handled = true;
    }

    private void Input_Drop(object sender, DragEventArgs e)
    {
        e.Handled = true;
        if (_running || e.Data.GetData(DataFormats.FileDrop) is not string[] files) return;
        if (files.Length != 1 || !string.Equals(Path.GetExtension(files[0]), ".fbx", StringComparison.OrdinalIgnoreCase))
        { SetStatus("请选择一个 FBX", "一次拖入一个 .fbx 模型文件。", true); return; }
        InputPathBox.Text = files[0];
        DemoNotice.Visibility = Visibility.Collapsed;
    }

    private void OpenOutput_Click(object sender, RoutedEventArgs e)
    {
        if (Directory.Exists(_lastOutput)) OpenFolder(Path.GetDirectoryName(_lastOutput)!);
    }

    private void OpenFolder(string directory)
    {
        try { Process.Start(new ProcessStartInfo(directory) { UseShellExecute = true }); }
        catch (Exception ex) { SetStatus("无法打开目录", ex.Message, true); }
    }

    private void Report_Click(object sender, RoutedEventArgs e)
    {
        if (!File.Exists(_lastReport)) return;
        try
        {
            if (new FileInfo(_lastReport).Length > 8 * 1024 * 1024) { SetStatus("报告文件较大", "请从输出目录中打开 .report.json 文件。", true); return; }
            var json = File.ReadAllText(_lastReport, Encoding.UTF8);
            string summary;
            try { summary = ReportSummaryFormatter.Parse(json).DetailedText; }
            catch (InvalidDataException ex) { summary = "无法生成中文摘要：" + ex.Message + "\n请查看“原始 JSON”页签。"; }
            var tabs = new TabControl { Margin = new Thickness(12), FontSize = 13 };
            tabs.Items.Add(new TabItem { Header = "中文摘要", Content = TextViewer(summary) });
            tabs.Items.Add(new TabItem { Header = "原始 JSON", Content = TextViewer(json, raw: true) });
            new Window { Owner = this, Title = "GeoModelBridge · 转换报告", Content = tabs, Width = 900, Height = 700,
                MinWidth = 600, MinHeight = 400, WindowStartupLocation = WindowStartupLocation.CenterOwner }.Show();
        }
        catch (Exception ex) { SetStatus("无法读取报告", ex.Message, true); }
    }

    private void ShowText(string title, string content)
    {
        var viewer = TextViewer(content);
        new Window { Owner = this, Title = "GeoModelBridge · " + title, Content = viewer, Width = 850, Height = 640, MinWidth = 600, MinHeight = 400, WindowStartupLocation = WindowStartupLocation.CenterOwner }.Show();
    }

    private static TextBox TextViewer(string content, bool raw = false) => new() { Text = content, IsReadOnly = true,
        TextWrapping = raw ? TextWrapping.NoWrap : TextWrapping.Wrap, VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
        HorizontalScrollBarVisibility = raw ? ScrollBarVisibility.Auto : ScrollBarVisibility.Disabled, Padding = new Thickness(24),
        FontSize = 13, FontFamily = new FontFamily(raw ? "Consolas, Microsoft YaHei UI" : "Microsoft YaHei UI, Consolas"), BorderThickness = new Thickness(0) };

    private void Help_Click(object sender, RoutedEventArgs e) => ShowText("使用说明",
        "GeoModelBridge V" + ProductInfo.Version + "\n\n" +
        "1. 选择一个静态 FBX。外置 PNG/JPEG 通常放在模型目录中；其他位置可在转换选项中添加贴图目录。缺图时默认保留材质颜色和标量透明度并继续，报告列出缺图项；取消“缺少贴图时使用材质颜色继续转换”勾选可要求图片完整。\n\n" +
        "2. 指定尚不存在的 .gdb 输出路径。默认要素类名为 Models，已有数据库不会被覆盖。\n\n" +
        "3. 填写米制投影坐标系 WKID 和 XYZ 原点。模型先统一 Z-up、米制，再进行平移。这里不会重投影、旋转配准或推断真实位置；已经使用目标坐标的模型也应明确填写所需偏移。\n\n" +
        "4. 使用原生 FileGDB 后端，无需安装 ArcGIS Pro；可先点击“检查运行环境”。\n\n" +
        "5. 默认 GIS 静态兼容按 FBX 保存的姿态转换，省略环境光、高光和反射通道，删除有限坐标的零面积面；还可修复符合条件的 JPEG 封装，保留原压缩图像数据，不重新压缩图像。每项处理都会记录。它不会选择动画第 0 帧，也不会跳过所有错误。如需保留某一动画帧或渲染外观，请先在建模软件中导出静态快照或烘焙漫反射贴图。可切换严格检查，遇到这些内容时停止。\n\n" +
        "6. 开始转换后请保留窗口。完成后查看输出目录和报告。中文报告按原因归并诊断，也可切换查看原始 JSON。程序核验几何、UV、材质与内嵌图片，实际颜色、透明与接缝显示仍需目标软件验收。\n\n" +
        "仅测试程序时，可使用“加载演示”，它会填入明确的测试坐标。\n\n" +
        "支持常规漫反射材质及 PNG/JPEG。实际 PBR、顶点色、蒙皮变形、真实位移及未知复杂材质仍会拒绝；兼容模式不等于外观全保真。RGB、透明度、UV 和法线有目标格式量化；具体精度见报告。\n\n" +
        "移动程序时，请保留整个 dist 目录。GUI 自带 .NET 运行时，仍需同目录转换引擎及后端。独立后端需要 Microsoft Visual C++ x64 运行库。不会联网获取模型或上传数据。");

    private void Window_Closing(object? sender, CancelEventArgs e)
    {
        if (!_running) return;
        e.Cancel = true;
        SetStatus("任务仍在运行", "请等待当前任务结束后关闭窗口，以便完整写入并核验数据库。此窗口不会强制中断写入。");
    }
}
