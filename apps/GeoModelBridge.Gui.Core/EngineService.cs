using System.ComponentModel;
using System.Diagnostics;
using System.Text;
using System.Text.Json;

namespace GeoModelBridge.Gui.Core;

public sealed class EngineService
{
    private readonly string baseDirectory;
    private int running;
    public string EnginePath { get; }
    public bool IsEnginePresent => File.Exists(EnginePath);

    public EngineService(string? baseDirectory = null)
    {
        this.baseDirectory = Path.GetFullPath(baseDirectory ?? AppContext.BaseDirectory);
        EnginePath = Path.Combine(this.baseDirectory, "geomodelbridge.exe");
    }

    public async Task<ConversionResult> ConvertAsync(ConversionSettings settings, IProgress<EngineEvent>? progress = null)
    {
        var delivery = new ProgressDelivery(progress);
        var result = await ConvertCoreAsync(settings, delivery).ConfigureAwait(false);
        return delivery.Failed ? result with { Message = result.Message + ProgressFailureNotice } : result;
    }

    private async Task<ConversionResult> ConvertCoreAsync(ConversionSettings settings, IProgress<EngineEvent> progress)
    {
        if (Interlocked.CompareExchange(ref running, 1, 0) != 0)
            return new(false, null, "已有转换或环境检查正在运行，请等待完成。", "", null);
        var reportPath = "";
        int? exitCode = null;
        try
        {
            var requestDirectory = Environment.CurrentDirectory;
            var issues = ConversionValidator.Validate(settings);
            if (issues.Count > 0) return new(false, null, string.Join(Environment.NewLine, issues), "", null);
            // Bind the request before any callback or await. The host can change
            // its working directory or mutate the supplied texture list later.
            string FullPath(string path) => Path.TrimEndingDirectorySeparator(Path.GetFullPath(path.Trim(), requestDirectory));
            settings = settings with
            {
                InputPath = FullPath(settings.InputPath),
                OutputPath = FullPath(settings.OutputPath),
                ReportPath = FullPath(string.IsNullOrWhiteSpace(settings.ReportPath) ? FullPath(settings.OutputPath) + ".report.json" : settings.ReportPath),
                TextureDirectories = settings.TextureDirectories.Select(FullPath).ToArray()
            };
            reportPath = ConversionCommand.GetReportPath(settings);
            Stage(progress, "正在检查转换引擎版本…");
            var version = await CheckEngineAsync(progress).ConfigureAwait(false);
            if (!version.Success) return new(false, null, version.Message, reportPath, null);
            // Revalidate after the version check; another task might have created a destination.
            var info = ConversionCommand.CreateStartInfo(EnginePath, settings);
            Stage(progress, "正在启动转换：读取 FBX、写入 GDB 并回读核验…");
            var result = await RunAsync(info, progress).ConfigureAwait(false);
            exitCode = result.ExitCode;
            if (result.ExitCode != 0)
                return new(false, exitCode, await FailureMessageAsync(result, reportPath).ConfigureAwait(false), reportPath, null);
            Stage(progress, "转换进程已结束，正在核对结果报告…");
            var reportedFailure = await ReadFailureSummaryAsync(reportPath).ConfigureAwait(false);
            if (reportedFailure is not null) return new(false, exitCode, reportedFailure, reportPath, null);
            if (!Directory.Exists(PathRules.Normalize(settings.OutputPath)))
                return new(false, exitCode, "转换引擎退出，但未找到输出 GDB，不能确认成功。", reportPath, null);
            PathRules.RequireNoLinks(settings.OutputPath);
            if (!File.Exists(reportPath))
                return new(false, exitCode, "转换引擎退出，但未找到验证报告，不能确认成功。", reportPath, null);
            var report = ReportVerifier.Verify(await ReadReportAsync(reportPath).ConfigureAwait(false), settings);
            return new(true, exitCode, report.Summary?.SummaryText ?? $"转换完成，已核验 {report.FeatureCount} 个要素的几何、材质、UV 与贴图。显示效果仍可在目标软件中检查。", reportPath, report);
        }
        catch (Exception ex) when (ex is IOException or InvalidDataException or UnauthorizedAccessException or Win32Exception or InvalidOperationException or ArgumentException)
        { return new(false, exitCode, FriendlyError(ex), reportPath, null); }
        finally { Volatile.Write(ref running, 0); }
    }

    public async Task<ProbeResult> ProbeBackendAsync(string backend, IProgress<EngineEvent>? progress = null)
    {
        var delivery = new ProgressDelivery(progress);
        var result = await ProbeBackendCoreAsync(backend, delivery).ConfigureAwait(false);
        return delivery.Failed ? result with { Message = result.Message + ProgressFailureNotice } : result;
    }

    private async Task<ProbeResult> ProbeBackendCoreAsync(string backend, IProgress<EngineEvent> progress)
    {
        if (Interlocked.CompareExchange(ref running, 1, 0) != 0)
            return new(false, "已有转换或环境检查正在运行，请等待完成。");
        try
        {
            if (backend != "native-filegdb") return new(false, "仅支持原生 FileGDB 转换方式。");
            Stage(progress, "正在检查转换引擎版本…");
            var engine = await CheckEngineAsync(progress).ConfigureAwait(false);
            if (!engine.Success) return engine;
            var configured = Environment.GetEnvironmentVariable("GMB_NATIVE_WRITER");
            var writer = string.IsNullOrEmpty(configured)
                ? Path.Combine(baseDirectory, "native-filegdb", "GeoModelBridge.NativeWriter.exe")
                : Path.GetFullPath(configured, baseDirectory);
            if (!File.Exists(writer)) return new(false, "找不到所选写入端，请保留完整程序目录及其子文件夹。" + Environment.NewLine + writer);
            if (string.Equals(Path.GetExtension(writer), ".dll", StringComparison.OrdinalIgnoreCase))
                return new(false, "写入端必须为原生可执行程序，不能使用托管 DLL。");
            Stage(progress, "正在检查原生 FileGDB 写入端…");
            var info = ConversionCommand.StartInfo(writer, ["--probe"]);
            info.WorkingDirectory = baseDirectory;
            var result = await RunAsync(info, progress).ConfigureAwait(false);
            if (result.ExitCode != 0) return new(false, $"写入端环境检查未通过（退出代码 {result.ExitCode}）。请检查运行库及安装状态。" + Environment.NewLine + ProcessDetails(result));
            ReportVerifier.Require(!result.StandardOutputTruncated, "写入端检查输出过长且已截断，无法确认完整检查结果。");
            using var document = JsonDocument.Parse(result.StandardOutput.Trim());
            var root = document.RootElement;
            ReportVerifier.RequireUniqueFields(root);
            ReportVerifier.Require(ReportVerifier.Text(root, "status") == "available", "写入端未报告环境可用。");
            ReportVerifier.Require(ReportVerifier.Text(root, "backend") == ReportVerifier.ExpectedBackend(backend), "写入端与所选转换方式不一致。");
            ReportVerifier.Require(ReportVerifier.Text(root, "version") == ProductInfo.Version, "写入端版本与 GUI 版本不一致，请使用同一版本完整目录。");
            ReportVerifier.Require(!root.GetProperty("arcgis_pro_required").GetBoolean(), "独立写入端的运行要求不符合预期。");
            return new(true, $"原生 FileGDB 转换环境可用（V{ProductInfo.Version}），无需 ArcGIS Pro。");
        }
        catch (Exception ex) when (ex is IOException or InvalidDataException or UnauthorizedAccessException or Win32Exception or InvalidOperationException or ArgumentException or JsonException or KeyNotFoundException)
        { return new(false, "环境检查未通过：" + FriendlyError(ex)); }
        finally { Volatile.Write(ref running, 0); }
    }

    private async Task<ProbeResult> CheckEngineAsync(IProgress<EngineEvent>? progress)
    {
        if (!IsEnginePresent) return new(false, "找不到 geomodelbridge.exe。请将 GUI 放在完整发布目录的 dist\\bin 中，与转换引擎放在一起。");
        var result = await RunAsync(ConversionCommand.StartInfo(EnginePath, ["--version"]), progress).ConfigureAwait(false);
        var version = result.StandardOutput.Trim();
        if (result.ExitCode != 0 || result.StandardOutputTruncated || (version != "GeoModelBridge V" + ProductInfo.Version && version != "GeoModelBridge " + ProductInfo.Version))
            return new(false, "转换引擎版本与 GUI 不一致或引擎无法运行；需要完整的 V" + ProductInfo.Version + " 程序目录。" + Environment.NewLine + Tail(version + Environment.NewLine + result.StandardError));
        return new(true, "转换引擎版本一致。");
    }

    private sealed record ProcessResult(int ExitCode, string StandardOutput, string StandardError,
        bool StandardOutputTruncated, bool StandardErrorTruncated);

    private const string ProgressFailureNotice = "\n部分运行消息无法显示；以上结果仍按进程退出状态和完整报告核验，请勿因此重复转换到同一输出位置。";
    private sealed class ProgressDelivery(IProgress<EngineEvent>? target) : IProgress<EngineEvent>
    {
        private int failed;
        public bool Failed => Volatile.Read(ref failed) != 0;
        public void Report(EngineEvent value)
        {
            if (Failed || target is null) return;
            // A synchronous host callback must not stop a pipe drainer and
            // deadlock the child, or erase a successfully verified result.
            try { target.Report(value); }
            catch (Exception) { Interlocked.Exchange(ref failed, 1); }
        }
    }

    private static async Task<ProcessResult> RunAsync(ProcessStartInfo info, IProgress<EngineEvent>? progress)
    {
        using var process = new Process { StartInfo = info };
        if (!process.Start()) throw new InvalidOperationException("无法启动转换进程。");
        var standardOutput = new StringBuilder();
        var standardError = new StringBuilder();
        var stdout = ReadAsync(process.StandardOutput, standardOutput, EngineEventKind.StandardOutput, progress);
        var stderr = ReadAsync(process.StandardError, standardError, EngineEventKind.StandardError, progress);
        await Task.WhenAll(stdout, stderr, process.WaitForExitAsync()).ConfigureAwait(false);
        return new(process.ExitCode, standardOutput.ToString(), standardError.ToString(), stdout.Result, stderr.Result);
    }

    private static async Task<bool> ReadAsync(StreamReader reader, StringBuilder captured, EngineEventKind kind, IProgress<EngineEvent>? progress)
    {
        const int captureLimit = 262144, previewLimit = 2048, visibleLines = 12;
        var buffer = new char[4096];
        var preview = new StringBuilder(previewLimit);
        long lineCount = 0;
        bool lineHasContent = false, lineTruncated = false, previousWasCr = false, truncated = false;
        void PublishLine()
        {
            if (!lineHasContent) return;
            ++lineCount;
            if (lineCount <= visibleLines)
                progress?.Report(new(kind, preview + (lineTruncated ? "… [此行日志已截断，完整诊断请查看报告]" : "")));
            preview.Clear();
            lineHasContent = lineTruncated = false;
        }
        // Fixed-size reads drain both redirected pipes even when a process emits
        // one enormous line. Bound callbacks here, before they reach the UI queue.
        int count;
        while ((count = await reader.ReadAsync(buffer.AsMemory()).ConfigureAwait(false)) > 0)
        {
            captured.Append(buffer, 0, count);
            if (captured.Length > captureLimit)
            {
                captured.Remove(0, captured.Length - captureLimit);
                truncated = true;
            }
            for (var i = 0; i < count; ++i)
            {
                var character = buffer[i];
                if (character is '\r' or '\n')
                {
                    if (character != '\n' || !previousWasCr) PublishLine();
                    previousWasCr = character == '\r';
                    continue;
                }
                previousWasCr = false;
                lineHasContent = true;
                if (preview.Length < previewLimit) preview.Append(character);
                else lineTruncated = true;
            }
        }
        PublishLine();
        if (lineCount > visibleLines)
            progress?.Report(new(EngineEventKind.LogSummary,
                $"{(kind == EngineEventKind.StandardError ? "错误输出" : "程序输出")}已折叠 {lineCount - visibleLines} 条日志；完整模型诊断请查看转换报告。"));
        return truncated;
    }

    private static async Task<string> FailureMessageAsync(ProcessResult result, string reportPath)
    {
        // Model diagnostics contain the useful reason; stderr can end with hundreds of repetitive rows.
        var summary = await ReadFailureSummaryAsync(reportPath).ConfigureAwait(false);
        if (summary is not null) return summary + $"\n退出代码：{result.ExitCode}。完整内容可点击“查看转换报告”。";
        var reason = result.ExitCode switch
        {
            2 => "参数不符合要求，请检查文件路径、WKID 和原点。",
            3 => "模型未通过所选策略的检查，请查看转换报告中的原因。",
            4 => "写入端不可用，请先运行环境检查。",
            5 => "GDB 写入或回读核验失败，请查看转换报告及下方日志。",
            6 => "文件读取或转换失败，请查看路径权限、模型内容及下方日志。",
            _ => "转换引擎异常退出，未确认转换成功。"
        };
        return reason + $"（退出代码 {result.ExitCode}）" + Environment.NewLine + ProcessDetails(result);
    }

    private static async Task<string?> ReadFailureSummaryAsync(string reportPath)
    {
        if (File.Exists(reportPath))
        {
            try
            {
                if (new FileInfo(reportPath).Length <= 8 * 1024 * 1024)
                {
                    var summary = ReportSummaryFormatter.Parse(await ReadReportAsync(reportPath).ConfigureAwait(false));
                    if (summary.ErrorCount > 0) return summary.SummaryText;
                }
            }
            catch (Exception ex) when (ex is IOException or InvalidDataException or UnauthorizedAccessException)
            { /* A partial report must never hide the process failure or be treated as successful. */ }
        }
        return null;
    }

    private static async Task<string> ReadReportAsync(string path)
    {
        const int limit = 64 * 1024 * 1024;
        PathRules.RequireNoLinks(path);
        await using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read,
            8192, FileOptions.Asynchronous | FileOptions.SequentialScan);
        if (stream.Length > limit) throw new InvalidDataException("转换报告超过 64 MiB，无法核验。");
        using var reader = new StreamReader(stream, new UTF8Encoding(false, true), true);
        var contents = new StringBuilder();
        var buffer = new char[8192];
        int count;
        while ((count = await reader.ReadAsync(buffer.AsMemory()).ConfigureAwait(false)) > 0)
        {
            if (stream.Position > limit || contents.Length > limit - count) throw new InvalidDataException("转换报告超过 64 MiB，无法核验。");
            contents.Append(buffer, 0, count);
        }
        return contents.ToString();
    }

    private static string FriendlyError(Exception exception) => exception switch
    {
        Win32Exception => "无法启动程序；请检查完整程序目录、运行库及安装状态。" + exception.Message,
        UnauthorizedAccessException => "无法访问文件或目录，请选择有读写权限的位置。" + exception.Message,
        InvalidDataException => exception.Message,
        _ => "操作未完成：" + exception.Message
    };
    private static void Stage(IProgress<EngineEvent>? progress, string message) => progress?.Report(new(EngineEventKind.Stage, message));
    private static string ProcessDetails(ProcessResult result) => string.Join(Environment.NewLine,
        new[] { string.IsNullOrWhiteSpace(result.StandardError) ? "" : "错误输出" + (result.StandardErrorTruncated ? "（已截断）" : "") + "：" + Tail(result.StandardError, 2000),
            string.IsNullOrWhiteSpace(result.StandardOutput) ? "" : "程序输出" + (result.StandardOutputTruncated ? "（已截断）" : "") + "：" + Tail(result.StandardOutput, 2000) }.Where(value => value.Length > 0));
    private static string Tail(string value, int limit = 4000) => value.Length <= limit ? value.Trim() : "…" + value[^limit..].Trim();
}
