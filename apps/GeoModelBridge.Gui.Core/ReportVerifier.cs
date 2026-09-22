using System.Globalization;
using System.Text.Json;

namespace GeoModelBridge.Gui.Core;

public static class ReportVerifier
{
    public static ConversionReport Verify(string json, ConversionSettings settings)
    {
        try
        {
            using var document = JsonDocument.Parse(json);
            var root = document.RootElement;
            RequireUniqueFields(root);
            Require(Text(root, "status") == "written_and_readback_verified", "报告未确认写入与回读成功。");
            Require(Text(root, "version") == ProductInfo.Version, "报告版本与 GUI 版本不一致。");
            Require(Text(root, "conversion_profile") == settings.Profile, "报告中的转换策略与本次选择不一致。");
            Require(Text(root, "missing_texture_policy") == settings.MissingTexturePolicy, "报告中的缺失贴图策略与本次选择不一致。");
            var backend = Text(root, "backend");
            Require(backend == ExpectedBackend(settings.Backend), "报告中的转换方式与本次选择不一致。");
            var output = Text(root, "output");
            Require(Path.IsPathFullyQualified(output) && PathRules.Equal(output, settings.OutputPath), "报告中的输出位置与本次选择不一致。");
            var source = Text(root, "source");
            Require(Path.IsPathFullyQualified(source) && PathRules.Equal(source, settings.InputPath), "报告中的输入模型与本次选择不一致。");
            Require(Text(root, "feature_class") == settings.FeatureClass, "报告中的要素类名称不一致。");
            var verification = root.GetProperty("verification");
            Require(Text(verification, "level") == "closed_reopened_file_geodatabase", "报告缺少关闭后重新打开 GDB 的验证。");
            Require(verification.GetProperty("geometry_material_uv_texture_readback").GetBoolean(), "报告缺少几何、材质、UV 与贴图的回读验证。");
            var featureCount = verification.GetProperty("feature_count").GetInt32();
            Require(featureCount > 0, "报告未验证任何要素。");
            Require(HasCompleteReadbackChecks(verification, featureCount), "报告中的逐要素回读核验缺失、失败、重复或索引无效。");
            var coordinateSystem = root.GetProperty("coordinate_system");
            Require(coordinateSystem.GetProperty("wkid").GetInt32() == int.Parse(settings.Wkid.Trim(), CultureInfo.InvariantCulture), "报告中的 WKID 与本次设置不一致。");
            Require(coordinateSystem.GetProperty("projected").GetBoolean(), "报告未确认输出使用投影坐标系。");
            Require(Text(coordinateSystem, "unit") == "meter", "报告未确认输出使用米制单位。");
            Require(coordinateSystem.GetProperty("source_coordinates_assigned_without_reprojection").GetBoolean(),
                "报告未确认仅指定坐标系而未重新投影模型。");
            var coordinates = root.GetProperty("coordinates");
            Require(Text(coordinates, "unit") == "meter" && Text(coordinates, "up_axis") == "Z" && Text(coordinates, "space") == "referenced",
                "报告中的模型坐标约定不一致。");
            Require(coordinates.GetProperty("wkid").GetInt32() == coordinateSystem.GetProperty("wkid").GetInt32()
                && coordinates.GetProperty("origin_explicit").GetBoolean(), "报告中的定位参数不完整或不一致。");
            var origin = coordinates.GetProperty("origin");
            Require(origin.ValueKind == JsonValueKind.Array && origin.GetArrayLength() == 3, "报告缺少完整原点 X、Y、Z。");
            var expectedOrigin = new[] { settings.OriginX, settings.OriginY, settings.OriginZ };
            for (var i = 0; i < 3; ++i)
            {
                var value = origin[i].GetDouble();
                Require(double.IsFinite(value) && value == double.Parse(expectedOrigin[i], CultureInfo.InvariantCulture),
                    "报告中的原点 X、Y、Z 与本次设置不一致。");
            }
            var diagnostics = new List<string>();
            Require(root.TryGetProperty("reader_diagnostics", out _), "报告缺少模型诊断列表。");
            foreach (var name in new[] { "diagnostics", "reader_diagnostics" })
            {
                if (!root.TryGetProperty(name, out var entries)) continue;
                Require(entries.ValueKind == JsonValueKind.Array, "报告中的模型诊断格式无效。");
                foreach (var entry in entries.EnumerateArray())
                {
                    var severity = Text(entry, "severity");
                    var code = Text(entry, "code");
                    var message = Text(entry, "message");
                    Require(code.Length > 0, "报告中的模型诊断代码为空。");
                    Require(severity is "info" or "warning" or "error", "报告中的模型诊断级别无效。");
                    if (entry.TryGetProperty("context", out var context))
                        Require(context.ValueKind == JsonValueKind.String, "报告中的模型诊断上下文格式无效。");
                    Require(severity != "error", "成功报告仍包含模型错误，不能确认转换成功。");
                    Require(settings.MissingTexturePolicy != "error" || code != "MISSING_TEXTURE_FALLBACK", "要求完整贴图时不能接受缺图回退报告。");
                    diagnostics.Add($"[{code}] {message}");
                }
            }
            return new ConversionReport(backend, ProductInfo.Version, PathRules.Normalize(output), settings.FeatureClass, featureCount, diagnostics)
                { Summary = ReportSummaryFormatter.Parse(json) };
        }
        catch (Exception ex) when (ex is JsonException or KeyNotFoundException or InvalidOperationException or FormatException or OverflowException or ArgumentException)
        { throw new InvalidDataException("转换报告缺少必需信息或格式无效，无法确认成功。" + ex.Message, ex); }
    }

    internal static string ExpectedBackend(string backend) => backend switch
    {
        "native-filegdb" => "native-filegdb",
        _ => throw new InvalidDataException("未知转换方式。")
    };
    internal static string Text(JsonElement element, string name) => element.GetProperty(name).GetString() ?? throw new InvalidDataException($"报告字段 {name} 为空。");
    internal static bool HasCompleteReadbackChecks(JsonElement verification, int featureCount)
    {
        if (featureCount <= 0 || !verification.TryGetProperty("checks", out var checks)
            || checks.ValueKind != JsonValueKind.Array || checks.GetArrayLength() != featureCount) return false;
        var indices = new HashSet<int>();
        foreach (var check in checks.EnumerateArray())
        {
            if (check.ValueKind != JsonValueKind.Object
                || !check.TryGetProperty("mesh_index", out var value) || value.ValueKind != JsonValueKind.Number
                || !value.TryGetInt32(out var index) || index < 0 || index >= featureCount || !indices.Add(index)
                || !check.TryGetProperty("passed", out var passed) || passed.ValueKind != JsonValueKind.True) return false;
        }
        return true;
    }
    internal static void RequireUniqueFields(JsonElement element)
    {
        if (element.ValueKind == JsonValueKind.Object)
        {
            var names = new HashSet<string>(StringComparer.Ordinal);
            foreach (var property in element.EnumerateObject())
            {
                Require(names.Add(property.Name), $"报告中存在重复字段：{property.Name}。");
                RequireUniqueFields(property.Value);
            }
        }
        else if (element.ValueKind == JsonValueKind.Array)
            foreach (var value in element.EnumerateArray()) RequireUniqueFields(value);
    }
    internal static void Require(bool condition, string message) { if (!condition) throw new InvalidDataException(message); }
}
