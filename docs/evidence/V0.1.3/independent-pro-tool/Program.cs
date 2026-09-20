using System.IO;
using System.Reflection;
using System.Runtime.CompilerServices;
using Microsoft.Win32;
namespace GeoModelBridge.ArcGISPro;
internal static class Program
{
    [STAThread]
    private static int Main(string[] args)
    {
        try
        {
            if (args.Length != 3 && args.Length != 4) throw new ArgumentException("Usage: reader BUNDLE EXISTING_GDB NEW_REPORT or --compare FIRST_GDB SECOND_GDB NEW_REPORT");
            var root = Environment.GetEnvironmentVariable("ARCGIS_PRO_INSTALL_DIR") ?? Registry.GetValue(@"HKEY_LOCAL_MACHINE\SOFTWARE\ESRI\ArcGISPro", "InstallDir", null) as string ?? @"C:\Program Files\ArcGIS\Pro";
            var bin = Path.Combine(root, "bin");
            AppDomain.CurrentDomain.AssemblyResolve += (_, e) => { var candidate = Path.Combine(bin, new AssemblyName(e.Name).Name + ".dll"); return File.Exists(candidate) ? Assembly.LoadFrom(candidate) : null; };
            return Run(args);
        }
        catch(Exception e) { Console.Error.WriteLine(e); return 1; }
    }
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static int Run(string[] args) => args.Length == 4 ? (args[0] == "--pixels" ? Pixels.Run(args) : Compare.Run(args)) : Readback.Run(args);
}
