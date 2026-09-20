using System.Diagnostics;
using System.Globalization;
using System.Text;

namespace GeoModelBridge.Gui.Core;

public static class ConversionCommand
{
    public static string GetReportPath(ConversionSettings settings) => PathRules.Normalize(
        string.IsNullOrWhiteSpace(settings.ReportPath) ? PathRules.Normalize(settings.OutputPath) + ".report.json" : settings.ReportPath);

    public static IReadOnlyList<string> BuildArguments(ConversionSettings settings)
    {
        var issues = ConversionValidator.Validate(settings);
        if (issues.Count > 0) throw new ArgumentException(string.Join(Environment.NewLine, issues), nameof(settings));
        var result = new List<string>
        {
            "convert", PathRules.Normalize(settings.InputPath),
            "--output", PathRules.Normalize(settings.OutputPath),
            "--backend", settings.Backend,
            "--profile", settings.Profile,
            "--wkid", int.Parse(settings.Wkid.Trim(), NumberStyles.None, CultureInfo.InvariantCulture).ToString(CultureInfo.InvariantCulture),
            "--origin", Number(settings.OriginX), Number(settings.OriginY), Number(settings.OriginZ),
            "--feature-class", settings.FeatureClass,
            "--report", GetReportPath(settings)
        };
        foreach (var directory in settings.TextureDirectories) { result.Add("--texture-dir"); result.Add(PathRules.Normalize(directory)); }
        return result;
    }

    public static ProcessStartInfo CreateStartInfo(string enginePath, ConversionSettings settings) => StartInfo(enginePath, BuildArguments(settings));

    internal static ProcessStartInfo StartInfo(string executable, IEnumerable<string> arguments)
    {
        var info = new ProcessStartInfo
        {
            FileName = executable,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = new UTF8Encoding(false),
            StandardErrorEncoding = new UTF8Encoding(false)
        };
        if (Path.IsPathFullyQualified(executable)) info.WorkingDirectory = Path.GetDirectoryName(executable)!;
        // Let .NET encode each argument. A shell command string cannot safely represent user paths.
        foreach (var argument in arguments) info.ArgumentList.Add(argument);
        return info;
    }

    private static string Number(string value) => double.Parse(value.Trim(), NumberStyles.Float, CultureInfo.InvariantCulture).ToString("R", CultureInfo.InvariantCulture);
}
