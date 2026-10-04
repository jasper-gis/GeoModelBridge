using System.Text.Json.Nodes;
using GeoModelBridge.Gui.Core;

internal static partial class Program
{
    private static void MeshFormatSummaryTests()
    {
        foreach (var (code, title, advice) in new[]
        {
            ("PLY_COORDINATE_ASSUMPTION", "源轴与单位", "不执行重投影"),
            ("STL_COORDINATE_ASSUMPTION", "源轴与单位", "每单位一米"),
            ("PLY_COLOR_MAPPED", "颜色与透明度", "不会平均"),
            ("PLY_DEFAULT_MATERIAL", "白色", "不透明"),
            ("STL_MATERIAL_ASSUMPTION", "无贴图", "不含材质或贴图"),
            ("PLY_DEFAULT_NORMALS_GENERATED", "平面法线", "角点保持独立"),
            ("PLY_UNUSED_VERTICES", "未引用", "无效法线仍会拒绝"),
            ("PLY_METADATA_OMITTED", "元数据", "不作为渲染通道"),
            ("X3D_METADATA_OMITTED", "元数据", "不作为渲染通道"),
            ("X3D_COORDINATE_CONVENTION", "源坐标", "源文件声明的长度单位"),
            ("X3D_DEFAULT_NORMALS_GENERATED", "平面法线", "零折痕角"),
            ("X3D_DEFAULT_UV_GENERATED", "默认 UV", "局部包围盒"),
            ("X3D_URL_ALTERNATIVE_USED", "备用路径", "不会被当作缺失跳过")
        })
        {
            Test("mesh_format_warning_explained_" + code, () =>
            {
                var report = GoodReport();
                report["reader_diagnostics"] = new JsonArray(new JsonObject
                    { ["severity"] = "warning", ["code"] = code, ["message"] = "Source format policy.", ["context"] = "mesh" });
                var summary = ReportSummaryFormatter.Parse(report.ToJsonString());
                var group = summary.Groups.Single();
                Assert(summary.IsSuccess && summary.WarningCount == 1 && !summary.HasCompatibilityAdjustments,
                    "An explicit source convention became a failed or modified rendering result.");
                Assert(group.Title.Contains(title) && group.Advice.Contains(advice), "Missing Chinese explanation for " + code);
                Assert(summary.DetailedText.Contains(code), "Machine-readable diagnostic disappeared.");
            });
        }
        foreach (var (code, title, advice) in new[]
        {
            ("UNSUPPORTED_PLY_VERTEX_COLOR", "顶点颜色渐变", "不平均或丢弃"),
            ("UNSUPPORTED_X3D_VERTEX_COLOR", "顶点颜色渐变", "静态兼容也不会跳过"),
            ("UNSUPPORTED_PLY_COLOR", "颜色类型或绑定", "8 位 RGB"),
            ("UNSUPPORTED_PLY_NORMAL_BINDING", "法线绑定", "保留一种明确绑定"),
            ("UNSUPPORTED_PLY_METADATA", "渲染或位置声明", "TextureFile"),
            ("UNSUPPORTED_PLY_PROPERTY", "元素或属性", "不会静默跳过"),
            ("UNSUPPORTED_PLY_ELEMENT", "元素或属性", "不会静默跳过"),
            ("UNSUPPORTED_PLY_HEADER", "元素或属性", "不会静默跳过"),
            ("UNSUPPORTED_STL_COLOR", "非标准颜色", "不会猜测或丢弃"),
            ("UNSUPPORTED_STL_ATTRIBUTE", "非标准颜色", "不会猜测或丢弃"),
            ("AMBIGUOUS_X3D_TEXTURE_MATERIAL", "组合存在歧义", "中性白色"),
            ("UNSUPPORTED_X3D_XML", "XML 声明", "外部实体"),
            ("UNSUPPORTED_X3D_VERSION", "版本或配置", "3.0–3.3"),
            ("UNSUPPORTED_X3D_PROFILE", "版本或配置", "静态场景子集"),
            ("UNSUPPORTED_X3D_UNIT", "单位声明", "长度单位"),
            ("UNSUPPORTED_X3D_MATERIAL", "材质通道", "发光"),
            ("UNSUPPORTED_X3D_NORMALS", "折痕角", "角点法线"),
            ("UNSUPPORTED_X3D_SAMPLER", "采样方式", "不会自动改成重复采样"),
            ("UNSUPPORTED_X3D_NODE", "节点或字段", "未知渲染扩展"),
            ("UNSUPPORTED_X3D_FIELD", "节点或字段", "未知渲染扩展"),
            ("INVALID_PLY", "结构或多边形", "索引"),
            ("INVALID_PLY_FACE", "结构或多边形", "自交"),
            ("INVALID_STL", "结构或多边形", "文件截断"),
            ("INVALID_X3D", "结构或多边形", "三角化"),
            ("INVALID_X3D_FACE", "结构或多边形", "不共面"),
            ("INVALID_X3D_INDICES", "结构或多边形", "索引"),
            ("PLY_GEOMETRY_LIMIT", "容量或复杂度", "不会静默抽稀"),
            ("PLY_COMPLEXITY_LIMIT", "容量或复杂度", "不会静默抽稀"),
            ("PLY_MATERIAL_LIMIT", "容量或复杂度", "合并颜色"),
            ("STL_SOLID_LIMIT", "容量或复杂度", "拆分模型"),
            ("X3D_EXPANSION_LIMIT", "容量或复杂度", "拆分模型"),
            ("MESH_TOO_LARGE", "容量或复杂度", "不会静默抽稀"),
            ("UNREPRESENTABLE_COORDINATE", "坐标精度", "不会把因此消失的面"),
            ("INVALID_MESH_UNITS", "源网格单位", "有限正数")
        })
        {
            Test("mesh_format_failure_explained_" + code, () =>
            {
                var report = GoodReport();
                report["reader_diagnostics"] = new JsonArray(new JsonObject
                    { ["severity"] = "error", ["code"] = code, ["message"] = "Unsupported source semantics.", ["context"] = "mesh" });
                var summary = ReportSummaryFormatter.Parse(report.ToJsonString());
                var group = summary.Groups.Single();
                Assert(!summary.IsSuccess && summary.ErrorCount == 1, "Unsupported format content became success.");
                Assert(group.Title.Contains(title) && group.Advice.Contains(advice), "Missing Chinese explanation for " + code);
                Assert(summary.SummaryText.Contains(advice), "Recovery guidance is absent from the visible summary.");
            });
        }
        Test("mesh_format_normal_repair_keeps_corner_counts", () =>
        {
            var report = GoodReport();
            report["reader_diagnostics"] = new JsonArray(
                new JsonObject { ["severity"] = "warning", ["code"] = "NORMALS_REPAIRED", ["message"] = "GIS static profile rebuilt 2 invalid corner normals across 2 triangles.", ["context"] = "PLY mesh" },
                new JsonObject { ["severity"] = "warning", ["code"] = "NORMALS_REPAIRED", ["message"] = "GIS static profile rebuilt 3 invalid corner normals across 1 triangles.", ["context"] = "STL solid" });
            var summary = ReportSummaryFormatter.Parse(report.ToJsonString());
            Assert(summary.Groups.Single().RepairedNormalCount == 5 && summary.SummaryText.Contains("共修复 5 个角点法线"), "New format corner repair counts were lost.");
        });
        Test("unlit_mapping_explanation_is_format_neutral", () =>
        {
            var report = GoodReport();
            report["reader_diagnostics"] = new JsonArray(new JsonObject
                { ["severity"] = "warning", ["code"] = "UNLIT_SHADING_MAPPED", ["message"] = "X3D unlit material mapped." });
            var summary = ReportSummaryFormatter.Parse(report.ToJsonString());
            Assert(summary.HasCompatibilityAdjustments && summary.Groups.Single().Title.Contains("无光照材质") &&
                !summary.Groups.Single().Title.Contains("GLB"), "X3D unlit handling was mislabeled as GLB.");
        });
    }
}
