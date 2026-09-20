using System.IO;
using System.Text.Json;
using ArcGIS.Core.Data;
using ArcGIS.Core.Geometry;
using ArcGIS.Core.Hosting;
namespace GeoModelBridge.ArcGISPro;
internal static class Compare
{
    private static Dictionary<int,Multipatch> Load(string path)
    {
        using var gdb = new Geodatabase(new FileGeodatabaseConnectionPath(new Uri(Path.GetFullPath(path))));
        using var table = gdb.OpenDataset<FeatureClass>("Models");
        using var cursor = table.Search(new QueryFilter(),false);
        var result = new Dictionary<int,Multipatch>();
        while(cursor.MoveNext()){using var row=(Feature)cursor.Current;result.Add(Convert.ToInt32(row["MeshIndex"]),(Multipatch)row.GetShape());}
        return result;
    }
    public static int Run(string[] args)
    {
        Bundle.Require(args[0]=="--compare" && !File.Exists(args[3]),"Invalid compare command/output");
        Host.Initialize();
        var first=Load(args[1]);var second=Load(args[2]);
        Bundle.Require(first.Count==second.Count,"Count differs");
        var different=new long[8];var bitDifferent=new long[8];var maximum=new double[8];var samples=new List<object>();
        var fields=new[]{"x","y","z","nx","ny","nz","u","v"};
        foreach(var (id,a) in first)
        {
            var b=second[id];Bundle.Require(a.PartCount==b.PartCount&&a.PointCount==b.PointCount&&a.MaterialCount==b.MaterialCount,"Geometry cardinality differs");
            for(int p=0;p<a.PartCount;p++)
            {
                Bundle.Require(a.GetPatchPointCount(p)==b.GetPatchPointCount(p)&&a.GetPatchTextureVertexCount(p)==b.GetPatchTextureVertexCount(p),"Patch differs");
                var sa=a.GetPatchStartPointIndex(p);var sb=b.GetPatchStartPointIndex(p);
                for(int v=0;v<a.GetPatchPointCount(p);v++)
                {
                    var pa=a.Points[sa+v];var pb=b.Points[sb+v];
                    var na=a.GetPatchNormal(p,v);var nb=b.GetPatchNormal(p,v);
                    var ua=a.GetPatchTextureVertexCount(p)>0?a.GetPatchTextureCoordinate(p,v):new Coordinate2D(0,0);
                    var ub=b.GetPatchTextureVertexCount(p)>0?b.GetPatchTextureCoordinate(p,v):new Coordinate2D(0,0);
                    var av=new[]{pa.X,pa.Y,pa.Z,na.X,na.Y,na.Z,ua.X,ua.Y};var bv=new[]{pb.X,pb.Y,pb.Z,nb.X,nb.Y,nb.Z,ub.X,ub.Y};
                    for(int c=0;c<8;c++)
                    {
                        if(BitConverter.DoubleToInt64Bits(av[c])!=BitConverter.DoubleToInt64Bits(bv[c]))bitDifferent[c]++;
                        if(av[c]==bv[c])continue;
                        different[c]++;maximum[c]=Math.Max(maximum[c],Math.Abs(av[c]-bv[c]));
                        if(samples.Count<20)samples.Add(new{mesh=id,patch=p,corner=v,component=fields[c],first=av[c],second=bv[c],absolute_difference=Math.Abs(av[c]-bv[c])});
                    }
                }
            }
        }
        var report=new{first=args[1],second=args[2],feature_count=first.Count,fields,different_value_counts=different,different_binary64_counts=bitDifferent,max_absolute_differences=maximum,samples};
        File.WriteAllText(args[3],JsonSerializer.Serialize(report,new JsonSerializerOptions{WriteIndented=true}));Console.WriteLine(JsonSerializer.Serialize(new{report.feature_count,fields,different,maximum}));return 0;
    }
}
