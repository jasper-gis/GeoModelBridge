using System.Text.Json;

namespace GeoModelBridge.Gui.Core;

public static class InspectionReportVerifier
{
    public static InspectionReport Verify(string json, InspectionSettings settings)
    {
        try
        {
            using var document = JsonDocument.Parse(json);
            var root = document.RootElement;
            ReportVerifier.RequireUniqueFields(root);
            Require(root.GetProperty("schema_version").GetInt32() == 1, "检查报告架构版本不受支持。");
            Require(Text(root, "status") == "inspected" && Text(root, "backend") == "none", "报告未确认独立模型检查。");
            Require(Text(root, "version") == ProductInfo.Version, "检查报告版本与 GUI 不一致。");
            Require(Text(root, "conversion_profile") == settings.Profile, "检查报告中的转换策略与本次选择不一致。");
            Require(Text(root, "missing_texture_policy") == settings.MissingTexturePolicy, "检查报告中的缺图策略与本次选择不一致。");
            var source = Text(root, "source");
            Require(Path.IsPathFullyQualified(source) && PathRules.Equal(source, settings.InputPath), "检查报告中的模型来源与本次选择不一致。");
            var fidelity = root.GetProperty("fidelity");
            Require(fidelity.GetProperty("validation_passed").GetBoolean() &&
                !fidelity.GetProperty("gdb_written").GetBoolean() && !fidelity.GetProperty("gdb_readback_verified").GetBoolean(),
                "模型检查必须确认验证通过，且未写入或回读 GDB。");
            // Compatibility adjustments can also occur for valid strict unlit materials.
            var compatibility = fidelity.GetProperty("compatibility_adjustments").GetBoolean();
            Require(fidelity.GetProperty("strict_validation_passed").GetBoolean() == (settings.Profile == "strict" && !compatibility),
                "检查报告中的严格验证与兼容处理状态不一致。");
            var coordinates = root.GetProperty("coordinates");
            Require(Text(coordinates, "space") == "local" && Text(coordinates, "unit") == "meter" && Text(coordinates, "up_axis") == "Z" &&
                coordinates.GetProperty("wkid").GetInt32() == 0 && !coordinates.GetProperty("origin_explicit").GetBoolean(),
                "检查报告必须使用未定位的本地 Z 向上米制坐标。");
            var origin = Point(coordinates.GetProperty("origin"));
            Require(origin == new ModelPoint(0, 0, 0), "模型检查不应应用地理原点平移。");
            var values = root.GetProperty("counts");
            var counts = new InspectionCounts(Count(values, "meshes", true), Count(values, "triangles", true),
                Count(values, "corner_vertices", true), Count(values, "materials", true), Count(values, "textures"), Count(values, "texture_bytes"));
            Require(counts.Triangles >= counts.Meshes && counts.CornerVertices >= 3 &&
                counts.Meshes <= counts.CornerVertices / 3 && (counts.Textures == 0 ? counts.TextureBytes == 0 : counts.TextureBytes >= counts.Textures),
                "检查报告中的网格、三角形、角点或贴图数量不一致。");
            var geometry = root.GetProperty("geometry_bounds");
            var bounds = new GeometryBounds(Point(geometry.GetProperty("min")), Point(geometry.GetProperty("max")));
            Require(bounds.Min.X <= bounds.Max.X && bounds.Min.Y <= bounds.Max.Y && bounds.Min.Z <= bounds.Max.Z,
                "检查报告中的几何范围顺序无效。");
            var diagnostics = ReportVerifier.VerifyDiagnostics(root, settings.ReaderSettings(), "diagnostics", out var warningCount);
            return new(PathRules.Normalize(source), ProductInfo.Version, counts, bounds, warningCount, diagnostics);
        }
        catch (Exception ex) when (ex is JsonException or KeyNotFoundException or InvalidOperationException or FormatException or OverflowException or ArgumentException)
        { throw new InvalidDataException("模型检查报告缺少必需信息或格式无效，无法确认检查通过。" + ex.Message, ex); }
    }

    private static long Count(JsonElement values, string name, bool positive = false)
    {
        var value = values.GetProperty(name).GetInt64();
        Require(value >= (positive ? 1 : 0), "检查报告数量无效：" + name);
        return value;
    }

    private static ModelPoint Point(JsonElement value)
    {
        Require(value.ValueKind == JsonValueKind.Array && value.GetArrayLength() == 3, "检查报告坐标须包含完整 XYZ。");
        var point = new ModelPoint(value[0].GetDouble(), value[1].GetDouble(), value[2].GetDouble());
        Require(double.IsFinite(point.X) && double.IsFinite(point.Y) && double.IsFinite(point.Z), "检查报告坐标或范围必须为有限数值。");
        return point;
    }

    private static string Text(JsonElement value, string name) => ReportVerifier.Text(value, name);
    private static void Require(bool condition, string message) => ReportVerifier.Require(condition, message);
}
