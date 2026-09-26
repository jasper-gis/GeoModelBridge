using System.Globalization;
using System.Text.RegularExpressions;

namespace GeoModelBridge.Gui.Core;

public static partial class ConversionValidator
{
    [GeneratedRegex("\\A[A-Za-z][A-Za-z0-9_]{0,63}\\z", RegexOptions.CultureInvariant)]
    private static partial Regex FeatureClassPattern();

    public static IReadOnlyList<string> Validate(ConversionSettings settings)
    {
        ArgumentNullException.ThrowIfNull(settings);
        var issues = new List<string>();
        var input = FullPath(settings.InputPath, "输入模型", issues);
        var output = FullPath(settings.OutputPath, "输出 GDB", issues);
        if (input is not null)
        {
            if (!new[] { ".fbx", ".obj" }.Contains(Path.GetExtension(input), StringComparer.OrdinalIgnoreCase))
                issues.Add("输入模型必须是 .fbx 或 .obj 文件。");
            if (!File.Exists(input)) issues.Add("找不到输入模型文件，请重新选择。");
        }
        if (output is not null)
        {
            if (!string.Equals(Path.GetExtension(output), ".gdb", StringComparison.Ordinal))
                issues.Add("输出名称必须以小写 .gdb 结尾。");
            CheckNewDestination(output, "输出 GDB", issues);
        }
        if (!int.TryParse(settings.Wkid?.Trim(), NumberStyles.None, CultureInfo.InvariantCulture, out var wkid) || wkid <= 0)
            issues.Add("请填写正整数 WKID；应使用以米为单位的投影坐标系。");
        CheckNumber(settings.OriginX, "原点 X", issues);
        CheckNumber(settings.OriginY, "原点 Y", issues);
        CheckNumber(settings.OriginZ, "原点 Z", issues);
        if (settings.FeatureClass is null || !FeatureClassPattern().IsMatch(settings.FeatureClass))
            issues.Add("要素类名称须以英文字母开头，长度为 1–64 个字符，仅使用英文字母、数字和下划线。");
        if (settings.Backend != "native-filegdb")
            issues.Add("仅支持原生 FileGDB 转换方式。");
        if (settings.Profile is not ("gis-static" or "strict"))
            issues.Add("请选择 GIS 静态兼容或严格检查转换策略。");
        if (settings.MissingTexturePolicy is not ("material-color" or "error"))
            issues.Add("缺失贴图策略必须为材质颜色回退或停止转换。");
        if (input is not null && string.Equals(Path.GetExtension(input), ".obj", StringComparison.OrdinalIgnoreCase))
        {
            if (settings.ObjUpAxis is not ("Z" or "Y")) issues.Add("OBJ 源模型向上轴必须为 Z 或 Y。");
            if (!double.TryParse(settings.ObjUnitMeters?.Trim(), NumberStyles.Float, CultureInfo.InvariantCulture, out var unit) ||
                !double.IsFinite(unit) || unit <= 0) issues.Add("OBJ 每单位米数必须为正的有限数值。");
        }
        if (output is not null)
        {
            var report = FullPath(string.IsNullOrWhiteSpace(settings.ReportPath) ? output + ".report.json" : settings.ReportPath,
                "转换报告", issues);
            if (report is not null)
            {
                CheckNewDestination(report, "转换报告", issues);
                if (PathRules.IsWithinOrEqual(report, output)) issues.Add("转换报告必须保存在输出 GDB 目录之外。");
                if (input is not null && PathRules.Equal(report, input)) issues.Add("转换报告不能覆盖输入模型。");
            }
        }
        if (settings.TextureDirectories is null) issues.Add("贴图目录列表无效。");
        else foreach (var directory in settings.TextureDirectories)
        {
            var path = FullPath(directory, "贴图目录", issues);
            if (path is not null && !Directory.Exists(path)) issues.Add($"找不到贴图目录：{path}");
        }
        return issues;
    }

    private static void CheckNumber(string? value, string label, List<string> issues)
    {
        if (!double.TryParse(value?.Trim(), NumberStyles.Float, CultureInfo.InvariantCulture, out var number) || !double.IsFinite(number))
            issues.Add($"请填写{label}的有限数值，使用小数点“.”，不要使用千位分隔符。");
    }

    private static string? FullPath(string? value, string label, List<string> issues)
    {
        if (string.IsNullOrWhiteSpace(value)) { issues.Add($"请选择或填写{label}路径。"); return null; }
        try
        {
            if (value.IndexOfAny(['\r', '\n', '\0']) >= 0) throw new ArgumentException("路径包含控制字符");
            var result = Path.GetFullPath(value.Trim());
            if (result.IndexOfAny(['*', '?', '"', '<', '>', '|']) >= 0) throw new ArgumentException("路径包含无效字符");
            return Path.TrimEndingDirectorySeparator(result);
        }
        catch (Exception ex) when (ex is ArgumentException or NotSupportedException or PathTooLongException)
        { issues.Add($"{label}路径无效：{ex.Message}"); return null; }
    }

    private static void CheckNewDestination(string path, string label, List<string> issues)
    {
        if (File.Exists(path) || Directory.Exists(path)) issues.Add($"{label}已存在，请改用新名称；已有数据不会被覆盖。");
        for (var parent = Path.GetDirectoryName(path); !string.IsNullOrEmpty(parent); parent = Path.GetDirectoryName(parent))
        {
            if (File.Exists(parent)) { issues.Add($"{label}的父路径是文件，不能在其中创建输出。"); break; }
            if (string.Equals(Path.GetExtension(parent), ".gdb", StringComparison.OrdinalIgnoreCase))
            { issues.Add($"{label}不能放在现有或其他 GDB 目录内部。"); break; }
        }
    }
}

internal static class PathRules
{
    private static readonly StringComparison Comparison = OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
    public static string Normalize(string path) => Path.TrimEndingDirectorySeparator(Path.GetFullPath(path.Trim()));
    public static bool Equal(string first, string second) => string.Equals(Normalize(first), Normalize(second), Comparison);
    public static void RequireNoLinks(string path)
    {
        for (var part = Normalize(path); !string.IsNullOrEmpty(part); part = Path.GetDirectoryName(part))
        {
            if ((File.GetAttributes(part) & FileAttributes.ReparsePoint) != 0)
                throw new InvalidDataException("转换结果路径包含链接或目录联接，无法确认本次输出：" + part);
        }
    }
    public static bool IsWithinOrEqual(string candidate, string directory)
    {
        var root = Normalize(directory);
        var path = Normalize(candidate);
        var prefix = Path.EndsInDirectorySeparator(root) ? root : root + Path.DirectorySeparatorChar;
        return string.Equals(path, root, Comparison) || path.StartsWith(prefix, Comparison);
    }
}
