using System.Text;
using System.Text.Json;
using System.Globalization;

namespace GeoModelBridge.Gui.Core;

public sealed record DiagnosticGroup(string Code, string Severity, string Title, int Count,
    IReadOnlyList<string> Examples, string Advice)
{
    public long? AffectedTriangleCount { get; init; }
}

public sealed record ReportSummary(string Status, bool IsSuccess, bool HasCompatibilityAdjustments,
    int ErrorCount, int WarningCount, IReadOnlyList<DiagnosticGroup> Groups, string SummaryText, string DetailedText)
{
    public IReadOnlyList<string> VerificationIssues { get; init; } = Array.Empty<string>();
}

/// <summary>Readable report presentation only. ReportVerifier still independently authorizes a successful result.</summary>
public static class ReportSummaryFormatter
{
    private sealed record Entry(string Code, string Severity, string Context, string Message);
    private sealed record Description(string Key, string Title, string Advice);

    public static string FormatFailure(string json) => Parse(json).SummaryText;

    public static ReportSummary Parse(string json)
    {
        try
        {
            using var document = JsonDocument.Parse(json);
            var root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object) throw new InvalidDataException("转换报告应为 JSON 对象。");
            var status = Text(root, "status");
            var entries = new List<Entry>();
            foreach (var name in new[] { "diagnostics", "reader_diagnostics" })
            {
                if (!root.TryGetProperty(name, out var diagnostics)) continue;
                if (diagnostics.ValueKind != JsonValueKind.Array) throw new InvalidDataException("报告中的诊断列表格式无效。");
                foreach (var item in diagnostics.EnumerateArray())
                {
                    if (item.ValueKind != JsonValueKind.Object) throw new InvalidDataException("报告中的诊断条目格式无效。");
                    entries.Add(new(Text(item, "code", "UNKNOWN"), Text(item, "severity", "warning"), Text(item, "context"), Text(item, "message")));
                }
            }
            var groups = entries.GroupBy(e => (e.Code, e.Severity, Describe(e).Key))
                .OrderBy(g => g.Key.Severity == "error" ? 0 : g.Key.Severity == "warning" ? 1 : 2)
                .Select(g => new DiagnosticGroup(g.Key.Code, g.Key.Severity, Describe(g.First()).Title, g.Count(),
                    g.Select(e => CleanContext(e.Context)).Where(s => s.Length > 0).Distinct(StringComparer.Ordinal).Take(4).ToArray(),
                    Describe(g.First()).Advice) { AffectedTriangleCount = RemovedTriangleCount(g) }).ToArray();
            var errorCount = entries.Count(e => e.Severity == "error");
            var warningCount = entries.Count(e => e.Severity == "warning");
            var verificationIssues = status == "written_and_readback_verified" ? CheckReportedVerification(root) : [];
            var success = status == "written_and_readback_verified" && errorCount == 0 && verificationIssues.Count == 0;
            var compatibility = entries.Any(e => IsCompatibilityCode(e.Code));
            var renderingAdjustments = entries.Any(e => IsCompatibilityCode(e.Code) && e.Code != "JPEG_CONTAINER_NORMALIZED");
            var summary = new StringBuilder();
            summary.Append(success
                ? compatibility ? "已完成转换，存在明确的兼容处理。" : "已完成写入与回读核验。"
                : errorCount > 0 ? $"发现 {errorCount} 条问题，已归并为 {groups.Count(g => g.Severity == "error")} 类原因；未确认生成可用数据库。"
                : verificationIssues.Count > 0 ? "报告声明已写入，但验证信息不完整或不符合要求，不能据此确认转换成功。"
                : "转换尚未确认成功，请查看报告详情与运行日志。");
            if (verificationIssues.Count > 0)
            {
                foreach (var issue in verificationIssues.Take(4)) summary.Append("\n• ").Append(issue);
                if (verificationIssues.Count > 4) summary.Append($"\n另有 {verificationIssues.Count - 4} 项验证信息待核对，详见下方报告内容。");
            }
            foreach (var group in groups.Take(8))
                summary.Append("\n• ").Append(group.Title).Append("：")
                    .Append(group.AffectedTriangleCount is { } removed ? $"共删除 {removed} 个面（{group.Count} 条记录）" :
                        group.Code == "JPEG_CONTAINER_NORMALIZED" ? $"{group.Count} 条贴图记录" : $"{group.Count} 条")
                    .Append(group.Examples.Count > 0 ? "（如 " + string.Join("、", group.Examples.Take(2)) + "）" : "").Append('。');
            if (groups.Length > 8) summary.Append($"\n其余 {groups.Length - 8} 类信息可在完整报告中查看。");
            var advice = groups.Where(g => g.Severity == "error").Select(g => g.Advice).Where(s => s.Length > 0).Distinct().Take(3).ToArray();
            if (advice.Length > 0) summary.Append("\n建议：").Append(string.Join(" ", advice));
            if (renderingAdjustments) summary.Append("\n兼容处理可能改变原模型的渲染效果，请在目标软件中检查外观。");
            else if (compatibility) summary.Append("\nJPEG 封装修复不重新压缩图像，原压缩图像数据保持不变；实际外观仍需在目标软件中检查。");
            var detail = new StringBuilder(summary.ToString()).Append("\n\n报告信息\n");
            AppendField(detail, "版本", Text(root, "version"));
            AppendField(detail, "输入模型", Text(root, "source"));
            AppendField(detail, "输出数据库", Text(root, "output"));
            AppendField(detail, "转换后端", Text(root, "backend"));
            var profile = Text(root, "profile");
            if (profile.Length == 0) profile = Text(root, "conversion_profile");
            AppendField(detail, "转换策略", profile == "gis-static" ? "GIS 静态兼容" : profile == "strict" ? "严格检查" : profile);
            var missingPolicy = Text(root, "missing_texture_policy");
            AppendField(detail, "缺失贴图", missingPolicy == "material-color" ? "使用材质颜色继续；保留标量透明度" : missingPolicy == "error" ? "停止转换" : missingPolicy);
            if (root.TryGetProperty("counts", out var counts) && counts.ValueKind == JsonValueKind.Object)
                foreach (var (key, label) in new[] { ("meshes", "网格数"), ("materials", "材质数"), ("textures", "贴图数"), ("triangles", "三角形数") })
                    if (counts.TryGetProperty(key, out var count) && count.ValueKind == JsonValueKind.Number && count.TryGetInt64(out var amount) && amount >= 0)
                        detail.Append(label).Append('：').Append(amount).Append('\n');
            if (root.TryGetProperty("verification", out var verification) && verification.ValueKind == JsonValueKind.Object &&
                verification.TryGetProperty("feature_count", out var features) && features.ValueKind == JsonValueKind.Number &&
                features.TryGetInt64(out var featureCount) && featureCount >= 0)
                detail.Append("已回读要素数：").Append(featureCount).Append('\n');
            foreach (var (key, label) in new[] { ("textures", "贴图记录数"), ("material_quantization", "材质量化记录数") })
                if (root.TryGetProperty(key, out var array) && array.ValueKind == JsonValueKind.Array)
                    detail.Append(label).Append('：').Append(array.GetArrayLength()).Append('\n');
            detail.Append($"错误 {errorCount} 条；警告 {warningCount} 条。\n");
            if (verificationIssues.Count > 0)
            {
                detail.Append("\n验证信息待核对\n");
                foreach (var issue in verificationIssues) detail.Append("• ").Append(issue).Append('\n');
                detail.Append("请使用同一版本的完整程序重新生成报告；不要仅根据成功状态字样认定数据库可用。\n");
            }
            if (groups.Length > 0) detail.Append("\n按原因归并的诊断\n");
            foreach (var group in groups)
            {
                var severity = group.Severity == "error" ? "错误" : group.Severity == "warning" ? "警告" : "提示";
                detail.Append($"\n【{severity}】{group.Title}（{group.Count} 条）\n");
                if (group.AffectedTriangleCount is { } removed) detail.Append($"处理数量：共删除 {removed} 个零面积三角形。\n");
                if (group.Examples.Count > 0) detail.Append("涉及对象示例：").AppendJoin("、", group.Examples).Append('\n');
                if (group.Advice.Length > 0) detail.Append("说明：").Append(group.Advice).Append('\n');
                detail.Append("诊断代码：").Append(group.Code).Append('\n');
            }
            detail.Append("\n完整逐项信息、坐标与精度说明请查看“原始 JSON”页签。\n回读核验不等于外观验收，颜色、透明与接缝显示仍需在目标软件中检查。");
            return new(status, success, compatibility, errorCount, warningCount, groups, summary.ToString(), detail.ToString())
                { VerificationIssues = verificationIssues };
        }
        catch (Exception ex) when (ex is JsonException or InvalidOperationException)
        { throw new InvalidDataException("无法解析转换报告：" + ex.Message, ex); }
    }

    private static bool IsCompatibilityCode(string code) => code is "STATIC_POSE_USED" or "MATERIAL_CHANNEL_OMITTED" or "DEGENERATE_TRIANGLES_REMOVED" or "JPEG_CONTAINER_NORMALIZED" or "MISSING_TEXTURE_FALLBACK";

    private static IReadOnlyList<string> CheckReportedVerification(JsonElement root)
    {
        var issues = new List<string>();
        bool HasText(JsonElement parent, string key) => parent.TryGetProperty(key, out var value) &&
            value.ValueKind == JsonValueKind.String && !string.IsNullOrWhiteSpace(value.GetString());
        bool True(JsonElement parent, string key) => parent.TryGetProperty(key, out var value) && value.ValueKind == JsonValueKind.True;
        bool PositiveInteger(JsonElement parent, string key) => parent.TryGetProperty(key, out var value) &&
            value.ValueKind == JsonValueKind.Number && value.TryGetInt32(out var number) && number > 0;
        if (!HasText(root, "version")) issues.Add("缺少程序版本（version）。");
        if (!HasText(root, "backend")) issues.Add("缺少转换后端（backend）。");
        if (!HasText(root, "output")) issues.Add("缺少输出数据库位置（output）。");
        if (!HasText(root, "feature_class")) issues.Add("缺少要素类名称（feature_class）。");
        if (!root.TryGetProperty("verification", out var verification) || verification.ValueKind != JsonValueKind.Object)
            issues.Add("缺少数据库回读验证记录（verification）。");
        else
        {
            if (!verification.TryGetProperty("level", out var level) || level.ValueKind != JsonValueKind.String ||
                level.GetString() != "closed_reopened_file_geodatabase") issues.Add("未确认关闭并重新打开数据库后进行核验（verification.level）。");
            if (!True(verification, "geometry_material_uv_texture_readback"))
                issues.Add("未确认几何、材质、UV 和贴图回读通过（verification.geometry_material_uv_texture_readback）。");
            if (!PositiveInteger(verification, "feature_count")) issues.Add("缺少有效的已回读要素数量（verification.feature_count）。");
        }
        if (!root.TryGetProperty("coordinate_system", out var coordinates) || coordinates.ValueKind != JsonValueKind.Object ||
            !PositiveInteger(coordinates, "wkid") || !True(coordinates, "projected"))
            issues.Add("缺少有效的投影坐标系确认（coordinate_system）。");
        return issues;
    }

    private static long? RemovedTriangleCount(IEnumerable<Entry> entries)
    {
        const string prefix = "GIS static profile removed ";
        const string suffix = " strictly zero-area triangles; no area tolerance was used.";
        long total = 0;
        foreach (var entry in entries)
        {
            if (entry.Code != "DEGENERATE_TRIANGLES_REMOVED" || !entry.Message.StartsWith(prefix, StringComparison.Ordinal) ||
                !entry.Message.EndsWith(suffix, StringComparison.Ordinal)) return null;
            var length = entry.Message.Length - prefix.Length - suffix.Length;
            if (length <= 0 || length > 19) return null;
            var number = entry.Message.Substring(prefix.Length, length);
            if (number.Any(c => c < '0' || c > '9') || (number.Length > 1 && number[0] == '0') ||
                !long.TryParse(number, NumberStyles.None, CultureInfo.InvariantCulture, out var count)) return null;
            try { total = checked(total + count); }
            catch (OverflowException) { return null; }
        }
        return total;
    }

    private static Description Describe(Entry entry)
    {
        var code = entry.Code;
        if (code == "EMPTY_ANIMATION_IGNORED") return new(code, "已忽略空动画记录", "文件中只有空动画容器，不含实际动画曲线，不影响保存的静态姿态。");
        if (code == "DEFAULT_MATERIAL_ASSIGNED") return new(code, "未指定材质的表面已使用默认材质", "请在目标软件中检查默认颜色是否符合需要。");
        if (code == "UV_SETS_REDUCED") return new(code, "已保留当前漫反射贴图使用的 UV", "未参与当前贴图采样的额外 UV 通道不写入输出。");
        if (code == "UNSUPPORTED_TEXTURE_CHANNEL")
        {
            var channel = entry.Message.Contains("(FBX channel 5).", StringComparison.Ordinal) || entry.Message.Contains("(FBX channel 6).", StringComparison.Ordinal) ? "反射" :
                entry.Message.Contains("(FBX channel 11).", StringComparison.Ordinal) || entry.Message.Contains("(FBX channel 12).", StringComparison.Ordinal) ? "环境光" :
                Enumerable.Range(2, 3).Any(i => entry.Message.Contains($"(FBX channel {i}).", StringComparison.Ordinal)) ? "高光" : "非漫反射";
            return new(code + channel, "贴图使用" + channel + "通道", "GIS 静态兼容可省略常规材质的环境光、高光和反射贴图并记录；其他通道仍需在建模软件中烘焙为漫反射贴图。");
        }
        if (code == "UNSUPPORTED_TEXTURE_CONNECTION")
        {
            var property = entry.Message.Split(':').Last().Trim();
            var name = property switch { "ReflectionColor" or "ReflectionFactor" => "反射", "AmbientColor" or "AmbientFactor" => "环境光",
                "SpecularColor" or "SpecularFactor" or "Shininess" => "高光", "NormalMap" => "法线", "Bump" or "BumpFactor" => "凹凸",
                "DisplacementColor" or "DisplacementFactor" or "VectorDisplacementColor" => "位移", _ => "未支持的材质" };
            return new(code + name, "存在" + name + "贴图连接", "GIS 静态兼容仅允许省略常规材质的环境光、高光和反射连接；其他连接需修正或烘焙到漫反射贴图，详细连接名保留在原始报告中。");
        }
        if (code == "JPEG_CONTAINER_REQUIRES_NORMALIZATION") return new(code, "JPEG 贴图需要修复封装", "可选择 GIS 静态兼容，为符合要求的 JPEG 补入 JFIF 标记；原压缩图像数据保持不变，不重新压缩。也可先在图像软件中转换为受支持的 PNG/JPEG。");
        if (code == "JPEG_CONTAINER_UNSUPPORTED") return new(code, "JPEG 贴图封装无法安全兼容", "当前 JPEG 结构不满足自动修复条件；请在图像软件中另存为标准 RGB JPEG 或 PNG，再更新模型贴图。程序不会猜测或强行改写图像数据。");
        if (code == "UNSUPPORTED_ANIMATION") return new(code, "模型包含动画", "可选用 GIS 静态兼容，按 FBX 保存的静态姿态转换；如需某一动画帧，请先导出该帧的静态模型。");
        if (code == "UNRECOGNIZED_RENDER_PROPERTY")
        {
            var property = entry.Message.Split(':').Last().Trim();
            var name = property switch { "Reflectivity" => "反射率", "DisplacementColor" => "置换颜色", "VectorDisplacementColor" => "向量置换颜色", _ => Clip(property, 60) };
            return new(code + property, "需确认的渲染属性" + (name.Length > 0 ? "（" + name + "）" : ""), "GIS 静态兼容可处理已识别的默认无效属性；真实位移、未知渲染属性仍需先烘焙或修正。");
        }
        if (code == "UNSUPPORTED_MATERIAL_CHANNEL")
        {
            var channel = entry.Message.Contains("ambient", StringComparison.OrdinalIgnoreCase) ? "环境光" :
                entry.Message.Contains("specular", StringComparison.OrdinalIgnoreCase) ? "高光" :
                entry.Message.Contains("reflection", StringComparison.OrdinalIgnoreCase) ? "反射" : "其他渲染通道";
            return new(code + channel, "材质使用" + channel, channel == "其他渲染通道" ? "请在建模软件中烘焙到漫反射颜色或贴图，未支持的通道不会自动忽略。" : "GIS 静态兼容会省略环境光、高光及反射通道，并在报告中记录；如需保留其外观，请先烘焙到漫反射贴图。");
        }
        if (code == "DEGENERATE_TRIANGLE") return new(code, "存在退化三角形", "GIS 静态兼容可删除有限坐标的零面积面；非有限坐标等损坏几何仍需修复。");
        if (code == "MISSING_TEXTURE_FALLBACK") return new(code, "缺失贴图已回退为材质颜色", "对应图片确实不可用；保留材质颜色和标量透明度并继续转换，未生成替代图片。找回贴图后可添加目录重新转换。");
        if (code is "MISSING_TEXTURE" or "TEXTURE_NOT_FOUND" or "TEXTURE_READ_ERROR") return new(code, "无法读取贴图", "将贴图放在模型目录中，或添加正确的贴图目录。文件确实缺失时可选择材质颜色回退；损坏或不可读取的图片仍需修复。");
        if (code == "INVALID_NORMAL") return new(code, "模型法线无效", "部分法线为零或非有限值，请在建模软件中重算法线。此问题独立于缺失贴图。");
        if (code is "MISSING_UV" or "MISSING_UV_SET") return new(code, "有贴图的面缺少有效 UV", "仍在使用的图片需要有效 UV 才能定位；请补齐模型 UV。已回退为材质颜色的面不再要求缺失图片的 UV。");
        if (IsCompatibilityCode(code)) return CompatibilityDescription(entry);
        var message = Clip(entry.Message, 130);
        return new(code, "其他诊断（" + Clip(code, 60) + "）" + (message.Length > 0 ? "：" + message : ""),
            entry.Severity == "error" ? "请查看原始报告中的诊断；该问题仍会阻止转换，兼容模式不会跳过所有错误。" : "完整信息请查看原始报告。");
    }

    private static Description CompatibilityDescription(Entry entry)
    {
        var code = entry.Code;
        if (code == "JPEG_CONTAINER_NORMALIZED") return new(code, "已修复 JPEG 封装（不重新压缩图像）", "已补入兼容所需的 JFIF 标记（18 字节）；原压缩图像数据保持不变，没有重新压缩或重采样图像。");
        if (code.Contains("ANIMATION", StringComparison.Ordinal) || code.Contains("POSE", StringComparison.Ordinal))
            return new(code, "已使用模型保存的静态姿态", "不会导出动画时间轴，也不会自动选择某一动画帧。");
        if (code.Contains("DEGENERATE", StringComparison.Ordinal))
            return new(code, "已移除零面积三角形", "仅删除无法形成可见表面的退化面，非有限坐标仍会报错。");
        if (code.Contains("PROPERTY", StringComparison.Ordinal) || code.Contains("DEFAULT", StringComparison.Ordinal))
            return new(code, "已处理已知默认渲染属性", "这些已知默认属性不参与本次静态输出；真实位移等仍须先修复或烘焙。");
        if (code == "MATERIAL_CHANNEL_OMITTED")
        {
            var channel = entry.Message.Contains("ambient", StringComparison.OrdinalIgnoreCase) ? "环境光" :
                entry.Message.Contains("specular", StringComparison.OrdinalIgnoreCase) ? "高光" :
                entry.Message.Contains("reflection", StringComparison.OrdinalIgnoreCase) ? "反射" : "额外渲染";
            return new(code + channel, "已省略" + channel + "通道", "保留常规颜色、漫反射贴图和 UV；环境光、高光或反射效果可能与源模型不同。");
        }
        return new(code, "已进行兼容处理：" + Clip(entry.Message, 100), "具体处理记录请查看原始报告。");
    }

    private static string CleanContext(string context) => context == "scene" ? "整个场景" : Clip(context.StartsWith("material:", StringComparison.Ordinal) ? context[9..] : context, 70);
    private static string Clip(string text, int max) { text = text.Replace('\r', ' ').Replace('\n', ' '); return text.Length <= max ? text : text[..max] + "…"; }
    private static void AppendField(StringBuilder builder, string label, string value) { if (value.Length > 0) builder.Append(label).Append('：').Append(value).Append('\n'); }
    private static string Text(JsonElement element, string name, string fallback = "") => element.TryGetProperty(name, out var value)
        ? value.ValueKind == JsonValueKind.String ? value.GetString() ?? fallback : throw new InvalidDataException($"报告字段 {name} 格式无效。") : fallback;
}
