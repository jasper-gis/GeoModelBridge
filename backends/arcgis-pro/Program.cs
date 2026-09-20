using System.IO;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Text.Json;
using Microsoft.Win32;

namespace GeoModelBridge.ArcGISPro;

internal static class Program
{
    [STAThread]
    private static int Main(string[] args)
    {
        try
        {
            var options = Options.Parse(args);
            if (options.Help)
            {
                Console.WriteLine("GeoModelBridge ArcGIS Pro adapter 0.1.4\n--input <bundle directory> --output <new.gdb> [--feature-class Models] [--report <new.json>]\n--verify-gdb <copied.gdb> --expected-report <original.writer-report.json> --report <new.json>\n--probe checks the installed ArcGIS Pro runtime and license. Existing outputs are never overwritten.");
                return 0;
            }
            var proBin = FindProBin();
            AppDomain.CurrentDomain.AssemblyResolve += (_, e) =>
            {
                var candidate = Path.Combine(proBin, new AssemblyName(e.Name).Name + ".dll");
                return File.Exists(candidate) ? Assembly.LoadFrom(candidate) : null;
            };
            return Run(options);
        }
        catch (Exception e)
        {
            Console.Error.WriteLine(JsonSerializer.Serialize(new { status = "failed", backend = "arcgis-pro-corehost", error = e.Message }));
            return 1;
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static int Run(Options options) => Writer.Run(options);

    private static string FindProBin()
    {
        var overridePath = Environment.GetEnvironmentVariable("ARCGIS_PRO_INSTALL_DIR");
        var installed = Registry.GetValue(@"HKEY_LOCAL_MACHINE\SOFTWARE\ESRI\ArcGISPro", "InstallDir", null) as string;
        var root = overridePath ?? installed ?? @"C:\Program Files\ArcGIS\Pro";
        var bin = Path.Combine(Path.GetFullPath(root), "bin");
        if (!File.Exists(Path.Combine(bin, "ArcGIS.CoreHost.dll"))) throw new InvalidOperationException("ArcGIS Pro is not installed. Set ARCGIS_PRO_INSTALL_DIR to its installation folder.");
        return bin;
    }
}

internal sealed record Options(string Input, string Output, string FeatureClass, string Report, bool Probe, bool Help, string VerifyGdb = "", string ExpectedReport = "")
{
    public static Options Parse(string[] args)
    {
        if (args.Length == 0 || args.Contains("--help")) return new("", "", "Models", "", false, true);
        if (args.Length == 1 && args[0] == "--probe") return new("", "", "Models", "", true, false);
        if (args.Contains("--verify-gdb")) return ParseVerification(args);
        var values = new Dictionary<string, string>(StringComparer.Ordinal);
        for (var i = 0; i < args.Length; i += 2)
        {
            if (i + 1 >= args.Length || args[i] is not ("--input" or "--output" or "--feature-class" or "--report") || !values.TryAdd(args[i], args[i + 1]))
                throw new ArgumentException("Invalid, duplicate, or incomplete argument: " + args[i]);
        }
        if (!values.TryGetValue("--input", out var input) || !values.TryGetValue("--output", out var output)) throw new ArgumentException("--input and --output are required.");
        output = Path.GetFullPath(output);
        if (!output.EndsWith(".gdb", StringComparison.OrdinalIgnoreCase)) throw new ArgumentException("--output must end in .gdb.");
        var featureClass = values.GetValueOrDefault("--feature-class", "Models");
        if (!System.Text.RegularExpressions.Regex.IsMatch(featureClass, "^[A-Za-z][A-Za-z0-9_]{0,63}$")) throw new ArgumentException("Feature class must begin with an ASCII letter and contain at most 64 letters, digits, or underscores.");
        input = Path.GetFullPath(input).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        var report = Path.GetFullPath(values.GetValueOrDefault("--report", Path.ChangeExtension(output, "writer-report.json")));
        if (report.StartsWith(output + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase) || report.Equals(output, StringComparison.OrdinalIgnoreCase)) throw new ArgumentException("Report must be outside the output geodatabase.");
        foreach (var destination in new[] { output, report })
        {
            if (destination.StartsWith(input + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase) || destination.Equals(input, StringComparison.OrdinalIgnoreCase)) throw new ArgumentException("Output and report must be outside the source bundle.");
            for (var parent = Path.GetDirectoryName(destination); !string.IsNullOrEmpty(parent); parent = Path.GetDirectoryName(parent))
                if (Directory.Exists(parent) && (File.GetAttributes(parent) & FileAttributes.ReparsePoint) != 0) throw new ArgumentException("Output and report parents must not use reparse points.");
        }
        return new(input, output, featureClass, report, false, false);
    }

    private static Options ParseVerification(string[] args)
    {
        var values = new Dictionary<string, string>(StringComparer.Ordinal);
        for (var i = 0; i < args.Length; i += 2)
            if (i + 1 >= args.Length || args[i] is not ("--verify-gdb" or "--expected-report" or "--report") || !values.TryAdd(args[i], args[i + 1])) throw new ArgumentException("Invalid standalone verification argument: " + args[i]);
        if (values.Count != 3) throw new ArgumentException("--verify-gdb, --expected-report and --report are all required.");
        var gdb = Path.GetFullPath(values["--verify-gdb"]).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        var expected = Path.GetFullPath(values["--expected-report"]);
        var report = Path.GetFullPath(values["--report"]);
        if (!gdb.EndsWith(".gdb", StringComparison.OrdinalIgnoreCase) || !Directory.Exists(gdb)) throw new ArgumentException("--verify-gdb must identify an existing .gdb directory.");
        if (report.Equals(expected, StringComparison.OrdinalIgnoreCase) || report.Equals(gdb, StringComparison.OrdinalIgnoreCase) || report.StartsWith(gdb + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)) throw new ArgumentException("Verification report must be distinct from the expected report and outside the geodatabase.");
        for (var parent = Path.GetDirectoryName(report); !string.IsNullOrEmpty(parent); parent = Path.GetDirectoryName(parent))
            if (Directory.Exists(parent) && (File.GetAttributes(parent) & FileAttributes.ReparsePoint) != 0) throw new ArgumentException("Report parents must not use reparse points.");
        return new("", "", "", report, false, false, gdb, expected);
    }
}
