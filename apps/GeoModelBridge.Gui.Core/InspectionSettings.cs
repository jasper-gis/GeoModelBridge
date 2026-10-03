namespace GeoModelBridge.Gui.Core;

/// <summary>A model-read request without a database destination or geographic placement.</summary>
public sealed record InspectionSettings
{
    public string InputPath { get; init; } = "";
    public string ReportPath { get; init; } = "";
    public string Profile { get; init; } = "gis-static";
    public string MissingTexturePolicy { get; init; } = "material-color";
    public string ObjUpAxis { get; init; } = "Z";
    public string ObjUnitMeters { get; init; } = "1";
    public string MaxBatchPath { get; init; } = "";
    public string MaxFrame { get; init; } = "";
    public string MaxTimeout { get; init; } = "600";
    public IReadOnlyList<string> TextureDirectories { get; init; } = Array.Empty<string>();

    public static InspectionSettings FromConversion(ConversionSettings settings, string reportPath) => new()
    {
        InputPath = settings.InputPath, ReportPath = reportPath, Profile = settings.Profile,
        MissingTexturePolicy = settings.MissingTexturePolicy, ObjUpAxis = settings.ObjUpAxis,
        ObjUnitMeters = settings.ObjUnitMeters, MaxBatchPath = settings.MaxBatchPath,
        MaxFrame = settings.MaxFrame, MaxTimeout = settings.MaxTimeout,
        TextureDirectories = settings.TextureDirectories
    };

    internal ConversionSettings ReaderSettings() => new()
    {
        InputPath = InputPath, Profile = Profile, MissingTexturePolicy = MissingTexturePolicy,
        ObjUpAxis = ObjUpAxis, ObjUnitMeters = ObjUnitMeters, MaxBatchPath = MaxBatchPath,
        MaxFrame = MaxFrame, MaxTimeout = MaxTimeout, TextureDirectories = TextureDirectories
    };
}

public sealed record InspectionResult(bool Success, int? ExitCode, string Message, string ReportPath, InspectionReport? Report);
public sealed record InspectionCounts(long Meshes, long Triangles, long CornerVertices, long Materials, long Textures, long TextureBytes);
public sealed record ModelPoint(double X, double Y, double Z);
public sealed record GeometryBounds(ModelPoint Min, ModelPoint Max);
public sealed record InspectionReport(string Source, string Version, InspectionCounts Counts, GeometryBounds Bounds,
    int WarningCount, IReadOnlyList<string> Diagnostics)
{
    public string SummaryText => FormattableString.Invariant(
        $"模型检查通过；未写入 GDB。\n网格 {Counts.Meshes}，三角形 {Counts.Triangles}，角点 {Counts.CornerVertices}。\n材质 {Counts.Materials}，贴图 {Counts.Textures}（{Counts.TextureBytes} 字节）。\n本地范围（Z 向上，米；未定位）：\n最小 ({Bounds.Min.X:G6}, {Bounds.Min.Y:G6}, {Bounds.Min.Z:G6})\n最大 ({Bounds.Max.X:G6}, {Bounds.Max.Y:G6}, {Bounds.Max.Z:G6})\n警告 {WarningCount} 条；详情见检查报告。");
}
