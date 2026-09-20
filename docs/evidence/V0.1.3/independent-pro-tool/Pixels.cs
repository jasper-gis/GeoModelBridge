using System.IO;
using System.Security.Cryptography;
using System.Text.Json;
using System.Windows.Media;
using System.Windows.Media.Imaging;
namespace GeoModelBridge.ArcGISPro;
internal static class Pixels
{
    private static string Hash(byte[] bytes)=>Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
    private static (string Hash,int Width,int Height) Decode(byte[] bytes)
    {
        using var stream=new MemoryStream(bytes,false);
        var decoder=BitmapDecoder.Create(stream,BitmapCreateOptions.PreservePixelFormat|BitmapCreateOptions.IgnoreColorProfile,BitmapCacheOption.OnLoad);
        var frame=decoder.Frames.Single();
        var bitmap=frame.Format==PixelFormats.Bgra32?(BitmapSource)frame:new FormatConvertedBitmap(frame,PixelFormats.Bgra32,null,0);
        var pixels=new byte[checked(frame.PixelWidth*frame.PixelHeight*4)];bitmap.CopyPixels(pixels,checked(frame.PixelWidth*4),0);
        return(Hash(pixels),frame.PixelWidth,frame.PixelHeight);
    }
    public static int Run(string[] args)
    {
        Bundle.Require(!File.Exists(args[3]),"Pixel report already exists");
        var before=Bundle.Load(args[1]);var after=Bundle.Load(args[2]);
        Bundle.Require(before.Textures.Length==after.Textures.Length,"Texture count changed");
        var records=new List<object>();int changed=0;
        for(int i=0;i<before.Textures.Length;i++)
        {
            var a=before.Textures[i];var b=after.Textures[i];if(a.MimeType!="image/jpeg")continue;
            bool normalized=!a.Bytes.AsSpan().SequenceEqual(b.Bytes);
            if(normalized){Bundle.Require(b.Bytes.Length==a.Bytes.Length+18&&b.Bytes.AsSpan(0,2).SequenceEqual(a.Bytes.AsSpan(0,2))&&b.Bytes.AsSpan(20).SequenceEqual(a.Bytes.AsSpan(2)),"Normalization changed original bytes");changed++;}
            var ap=Decode(a.Bytes);var bp=Decode(b.Bytes);
            Bundle.Require(ap==bp,"Decoded pixels differ");
            records.Add(new{index=i,texture_name=a.Name,normalized,source_sha256=Hash(a.Bytes),normalized_sha256=Hash(b.Bytes),original_image_and_metadata_bytes_unchanged=true,width=ap.Width,height=ap.Height,source_bgra32_sha256=ap.Hash,normalized_bgra32_sha256=bp.Hash,decoded_pixels_exactly_equal=true});
        }
        var report=new{status="jpeg_normalization_pixel_equivalence_verified",version="0.1.3",normalized_texture_count=changed,jpeg_texture_count=records.Count,decoder="Windows WIC via WPF Bgra32 straight alpha, IgnoreColorProfile, no orientation transform",checks=records};
        using(var stream=new FileStream(args[3],FileMode.CreateNew,FileAccess.Write,FileShare.None))JsonSerializer.Serialize(stream,report,new JsonSerializerOptions{WriteIndented=true});
        Console.WriteLine(JsonSerializer.Serialize(new{report.status,report.normalized_texture_count,report.jpeg_texture_count}));return 0;
    }
}
