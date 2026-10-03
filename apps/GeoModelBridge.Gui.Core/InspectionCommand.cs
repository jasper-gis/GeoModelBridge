using System.Diagnostics;

namespace GeoModelBridge.Gui.Core;

public static class InspectionValidator
{
    public static IReadOnlyList<string> ValidateReader(InspectionSettings settings)
    {
        ArgumentNullException.ThrowIfNull(settings);
        var issues = new List<string>();
        ConversionValidator.ValidateReader(settings.ReaderSettings(), issues);
        return issues;
    }

    public static IReadOnlyList<string> Validate(InspectionSettings settings)
    {
        ArgumentNullException.ThrowIfNull(settings);
        var issues = new List<string>();
        var input = ConversionValidator.ValidateReader(settings.ReaderSettings(), issues);
        var report = ConversionValidator.FullPath(settings.ReportPath, "模型检查报告", issues);
        if (report is not null)
        {
            ConversionValidator.CheckNewDestination(report, "模型检查报告", issues);
            if (string.Equals(Path.GetExtension(report), ".gdb", StringComparison.OrdinalIgnoreCase))
                issues.Add("检查报告不能使用 .gdb 名称，请选择新的 JSON 文件。");
            if (input is not null && PathRules.Equal(report, input)) issues.Add("检查报告不能覆盖输入模型。");
        }
        return issues;
    }
}

public static class InspectionCommand
{
    public static IReadOnlyList<string> BuildArguments(InspectionSettings settings)
    {
        var issues = InspectionValidator.Validate(settings);
        if (issues.Count > 0) throw new ArgumentException(string.Join(Environment.NewLine, issues), nameof(settings));
        var result = new List<string>
        {
            "inspect", PathRules.Normalize(settings.InputPath), "--report", PathRules.Normalize(settings.ReportPath),
            "--profile", settings.Profile, "--missing-textures", settings.MissingTexturePolicy
        };
        ConversionCommand.AddReaderArguments(result, settings.ReaderSettings());
        return result;
    }

    public static ProcessStartInfo CreateStartInfo(string enginePath, InspectionSettings settings) =>
        ConversionCommand.StartInfo(enginePath, BuildArguments(settings));
}
