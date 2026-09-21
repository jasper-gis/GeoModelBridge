namespace GeoModelBridge.Gui.Core;

public sealed record ConversionSettings
{
    public string InputPath { get; init; } = "";
    public string OutputPath { get; init; } = "";
    public string Wkid { get; init; } = "";
    public string OriginX { get; init; } = "";
    public string OriginY { get; init; } = "";
    public string OriginZ { get; init; } = "";
    public string FeatureClass { get; init; } = "Models";
    public string Backend { get; init; } = "native-filegdb";
    public string Profile { get; init; } = "gis-static";
    public string MissingTexturePolicy { get; init; } = "material-color";
    public string ReportPath { get; init; } = "";
    public IReadOnlyList<string> TextureDirectories { get; init; } = Array.Empty<string>();
}

public enum EngineEventKind { Stage, StandardOutput, StandardError, LogSummary }
public sealed record EngineEvent(EngineEventKind Kind, string Message);
public sealed record ProbeResult(bool Success, string Message);
public sealed record ConversionResult(bool Success, int? ExitCode, string Message, string ReportPath, ConversionReport? Report);
public sealed record ConversionReport(string Backend, string Version, string OutputPath, string FeatureClass,
    int FeatureCount, IReadOnlyList<string> Diagnostics)
{
    public ReportSummary? Summary { get; init; }
}

public static class ProductInfo
{
    public const string Version = "0.1.10";
}
