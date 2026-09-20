using System.IO;
using System.Security.Cryptography;
using System.Text.Json;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using ArcGIS.Core.Data;
using ArcGIS.Core.Geometry;
using ArcGIS.Core.Hosting;
namespace GeoModelBridge.ArcGISPro;
internal static class Readback
{
    private sealed record ExpectedTexture(byte[] Bytes, int Width, int Height, bool Jpeg, string SourceHash);
    private static ExpectedTexture Texture(Texture texture)
    {
        using var memory = new MemoryStream(texture.Bytes, false);
        var decoder = BitmapDecoder.Create(memory, BitmapCreateOptions.PreservePixelFormat | BitmapCreateOptions.IgnoreColorProfile, BitmapCacheOption.OnLoad);
        Bundle.Require(decoder.Frames.Count == 1, "Multi-frame texture");
        var frame = decoder.Frames[0];
        if(texture.MimeType == "image/jpeg") return new(texture.Bytes,frame.PixelWidth,frame.PixelHeight,true,texture.Sha256);
        var bitmap = frame.Format == PixelFormats.Bgra32 ? (BitmapSource)frame : new FormatConvertedBitmap(frame,PixelFormats.Bgra32,null,0);
        var bytes = new byte[checked(frame.PixelWidth*frame.PixelHeight*4)];
        bitmap.CopyPixels(bytes, checked(frame.PixelWidth*4),0);
        for(int i=0;i<bytes.Length;i+=4) (bytes[i],bytes[i+2]) = (bytes[i+2],bytes[i]);
        return new(bytes,frame.PixelWidth,frame.PixelHeight,false,texture.Sha256);
    }
    private static string Hash(byte[] bytes)=>Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
    private static byte Q(double v)=>(byte)Math.Round(v*255,MidpointRounding.AwayFromZero);
    private static void Close(double actual,double expected,double tolerance,string context)=>Bundle.Require(double.IsFinite(actual)&&Math.Abs(actual-expected)<=Math.Max(tolerance,1e-9),$"{context} mismatch: {actual:R} / {expected:R}");
    public static int Run(string[] args)
    {
        var timer=System.Diagnostics.Stopwatch.StartNew();
        var bundlePath=Path.GetFullPath(args[0]); var gdbPath=Path.GetFullPath(args[1]); var reportPath=Path.GetFullPath(args[2]);
        Bundle.Require(!File.Exists(reportPath)&&!Directory.Exists(reportPath),"Report already exists");
        var bundle=Bundle.Load(bundlePath);
        var textures=bundle.Textures.Select(Texture).ToArray();
        Host.Initialize();
        var checks=new List<object>(); var seen=new HashSet<int>(); var textureSeen=new HashSet<int>(); var texturedPatches=0; var triangles=0;
        using(var gdb=new Geodatabase(new FileGeodatabaseConnectionPath(new Uri(gdbPath))))
        using(var table=gdb.OpenDataset<FeatureClass>("Models"))
        using(var def=table.GetDefinition())
        {
            Bundle.Require(def.GetShapeType()==GeometryType.Multipatch,"Shape is not Multipatch");
            Bundle.Require(table.GetCount()==bundle.Meshes.Length,"Feature count mismatch");
            var sr=def.GetSpatialReference();
            Bundle.Require(sr.Wkid==bundle.Coordinates.Wkid,"WKID mismatch");
            using var cursor=table.Search(new QueryFilter(),false);
            while(cursor.MoveNext())
            {
                using var feature=(Feature)cursor.Current;
                var index=Convert.ToInt32(feature["MeshIndex"]);
                Bundle.Require(index>=0&&index<bundle.Meshes.Length&&seen.Add(index),"Invalid or duplicate MeshIndex");
                var mesh=bundle.Meshes[index];
                Bundle.Require((string)feature["MeshName"]==mesh.Name&&(string)feature["SourceNode"]==mesh.SourceNode,"Attributes mismatch");
                var geometry=feature.GetShape() as Multipatch ?? throw new InvalidDataException("Not a Multipatch");
                var patches=mesh.Triangles.GroupBy(t=>t.Material).ToArray();
                Bundle.Require(geometry.PartCount==patches.Length&&geometry.PointCount==mesh.Triangles.Length*3,"Patch or point count mismatch");
                Bundle.Require(geometry.MaterialCount==patches.Length,"Native material count differs from one material per patch policy");
                var patchChecks=new List<object>();
                for(int p=0;p<patches.Length;p++)
                {
                    var group=patches[p]; var vertices=group.SelectMany(t=>t.Indices).Select(v=>mesh.Vertices[v]).ToArray();
                    var material=bundle.Materials[group.Key]; var mi=geometry.GetPatchMaterialIndex(p);
                    var color=geometry.GetMaterialColor(mi);
                    Bundle.Require(geometry.GetPatchType(p)==PatchType.Triangles&&geometry.GetPatchPointCount(p)==vertices.Length,"Patch type/size mismatch");
                    Bundle.Require(color.R==Q(material.Color[0])&&color.G==Q(material.Color[1])&&color.B==Q(material.Color[2]),"Material color mismatch");
                    Bundle.Require(geometry.GetMaterialTransparencyPercent(mi)==(byte)Math.Round((1-material.Color[3])*100,MidpointRounding.AwayFromZero),"Opacity mismatch");
                    Bundle.Require(geometry.IsMaterialCullBackFace(mi)==!material.DoubleSided,"Culling mismatch");
                    bool uv=vertices.All(v=>v.Uv!=null), normals=vertices.All(v=>v.Normal!=null);
                    Bundle.Require(geometry.GetPatchTextureVertexCount(p)==(uv?vertices.Length:0),"UV count mismatch");
                    Bundle.Require(!normals||geometry.HasNormals,"Normals missing");
                    int start=geometry.GetPatchStartPointIndex(p);
                    using var values=new MemoryStream(); using var writer=new BinaryWriter(values);
                    for(int c=0;c<vertices.Length;c++)
                    {
                        var expected=vertices[c]; var point=geometry.Points[start+c];
                        Close(point.X,expected.Position[0],sr.XYResolution*2,"X"); Close(point.Y,expected.Position[1],sr.XYResolution*2,"Y"); Close(point.Z,expected.Position[2],2/sr.ZScale,"Z");
                        writer.Write(point.X);writer.Write(point.Y);writer.Write(point.Z);
                        if(uv) { var actual=geometry.GetPatchTextureCoordinate(p,c); Close(actual.X,expected.Uv![0],Math.Max(1,Math.Abs(expected.Uv[0]))*2e-6,"U");Close(actual.Y,1-expected.Uv[1],Math.Max(1,Math.Abs(1-expected.Uv[1]))*2e-6,"1-V");writer.Write(actual.X);writer.Write(actual.Y); }
                        if(normals) { var actual=geometry.GetPatchNormal(p,c); var actualValues=new[]{actual.X,actual.Y,actual.Z}; for(int n=0;n<3;n++){var predicted=Math.Floor((double)(float)expected.Normal![n]*128+0.5)/128;Bundle.Require(actualValues[n]==predicted,"Normal codec mismatch");writer.Write(actualValues[n]);} }
                    }
                    string? textureHash=null;
                    Bundle.Require(geometry.IsMaterialTextured(mi)==(material.Texture>=0),"Texture binding mismatch");
                    if(material.Texture>=0)
                    {
                        var expected=textures[material.Texture]; var bytes=geometry.GetMaterialTexture(mi);
                        Bundle.Require(bytes.AsSpan().SequenceEqual(expected.Bytes),"Texture original JPEG or PNG RGBA bytes mismatch");
                        Bundle.Require(geometry.GetMaterialTextureColumnCount(mi)==expected.Width&&geometry.GetMaterialTextureRowCount(mi)==expected.Height,"Texture dimensions mismatch");
                        Bundle.Require(geometry.GetMaterialTextureBytesPerPixel(mi)==(expected.Jpeg?3:4),"Texture channel count mismatch");
                        Bundle.Require(geometry.GetMaterialTextureCompressionType(mi)==(expected.Jpeg?TextureCompressionType.CompressionJPEG:TextureCompressionType.CompressionNone),"Texture compression mismatch");
                        textureHash=Hash(bytes);textureSeen.Add(material.Texture);texturedPatches++;
                    }
                    writer.Flush();
                    patchChecks.Add(new{patch_index=p,material_index=mi,source_material=group.Key,corner_count=vertices.Length,geometry_normal_uv_hash=Hash(values.ToArray()),texture_sha256=textureHash,passed=true});
                }
                triangles+=mesh.Triangles.Length;
                checks.Add(new{mesh_index=index,mesh_name=mesh.Name,source_node=mesh.SourceNode,point_count=geometry.PointCount,patch_count=geometry.PartCount,material_count=geometry.MaterialCount,patches=patchChecks,passed=true});
            }
        }
        Bundle.Require(seen.Count==bundle.Meshes.Length&&textureSeen.Count==textures.Length,"Not all source meshes/textures verified");
        timer.Stop();
        var report=new{status="independent_native_readback_verified",version="0.1.3",backend="arcgis-pro-corehost",geodatabase=gdbPath,expected_bundle=bundlePath,expected_scene_sha256=Hash(File.ReadAllBytes(Path.Combine(bundlePath,"scene.json"))),feature_class="Models",feature_count=seen.Count,triangle_count=triangles,unique_texture_count=textureSeen.Count,textured_patch_count=texturedPatches,elapsed_seconds=timer.Elapsed.TotalSeconds,verification=new{geometry_and_uv_against_source_bundle=true,coordinate_tolerance_xy="2*ArcGIS spatial-reference XY resolution",coordinate_tolerance_z="2/ArcGIS spatial-reference Z scale",uv_tolerance="max(1,abs(expected))*2e-6",exact_normal_codec_prediction=true,exact_material_color_opacity_culling=true,exact_source_jpeg_bytes_and_decoded_png_rgba=true,texture_dimensions_and_compression=true,no_geodatabase_write_performed=true,graphical_acceptance="pending"},checks};
        Directory.CreateDirectory(Path.GetDirectoryName(reportPath)!);
        using(var output=new FileStream(reportPath,FileMode.CreateNew,FileAccess.Write,FileShare.None))JsonSerializer.Serialize(output,report,new JsonSerializerOptions{WriteIndented=true});
        Console.WriteLine(JsonSerializer.Serialize(new{report.status,report.feature_count,report.triangle_count,report.unique_texture_count,report.elapsed_seconds}));return 0;
    }
}
