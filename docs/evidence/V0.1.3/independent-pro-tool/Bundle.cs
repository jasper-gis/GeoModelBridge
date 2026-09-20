using System.IO;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace GeoModelBridge.ArcGISPro;

internal sealed class Bundle
{
    public required int SchemaVersion { get; init; }
    public required string Generator { get; init; }
    public required string Version { get; init; }
    public required string Name { get; init; }
    public required string Source { get; init; }
    public string ConversionProfile { get; init; } = "strict";
    public required Coordinates Coordinates { get; init; }
    public required Mesh[] Meshes { get; init; }
    public required Material[] Materials { get; init; }
    public required Texture[] Textures { get; init; }
    public required Node[] Nodes { get; init; }
    public Diagnostic[] Diagnostics { get; init; } = [];

    public static Bundle Load(string root)
    {
        var scenePath = SafePath(root, "scene.json");
        if (new FileInfo(scenePath).Length > 512L * 1024 * 1024) throw new InvalidDataException("Scene JSON exceeds 512 MiB.");
        using var stream = File.OpenRead(scenePath);
        var bundle = JsonSerializer.Deserialize<Bundle>(stream, new JsonSerializerOptions { PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower, UnmappedMemberHandling = JsonUnmappedMemberHandling.Disallow }) ?? throw new InvalidDataException("Empty scene JSON.");
        bundle.Validate(root);
        return bundle;
    }

    private void Validate(string root)
    {
        Require(SchemaVersion == 1 && Generator == "GeoModelBridge", "Unsupported scene schema or generator.");
        Require(ConversionProfile is "strict" or "gis-static", "Invalid conversion profile.");
        Require(Diagnostics != null && Diagnostics.All(d => d != null && d.Severity is "warning" or "error"), "Invalid reader diagnostics.");
        Require(!Diagnostics!.Any(d => d.Severity == "error"), "Bundle contains reader errors; correct them before writing a geodatabase.");
        Require(Coordinates != null && Meshes != null && Materials != null && Textures != null && Nodes != null, "Scene arrays and coordinates must not be null.");
        Require(Coordinates!.Unit == "meter" && Coordinates.UpAxis == "Z", "Only Z-up metre coordinates are supported.");
        Require(Coordinates.Space == "referenced" && Coordinates.Wkid > 0 && Coordinates.OriginExplicit, "An explicit projected metre WKID and origin are required. This adapter does not assign WGS84 or reproject coordinates.");
        Vector(Coordinates.Origin, 3, "origin");
        Require(Meshes!.Length > 0, "At least one mesh is required.");
        foreach (var texture in Textures!)
        {
            Require(texture != null, "Null texture.");
            Require(texture!.MimeType is "image/png" or "image/jpeg", "Only PNG and JPEG are supported; no implicit image transcode is performed.");
            Require(texture.ByteLength is > 0 and <= 256L * 1024 * 1024, "Invalid texture byte length (maximum 256 MiB).");
            var path = SafePath(root, texture.Path);
            Require(new FileInfo(path).Length == texture.ByteLength, "Texture byte length mismatch: " + texture.Path);
            texture.Bytes = File.ReadAllBytes(path);
            Require(Convert.ToHexString(SHA256.HashData(texture.Bytes)).Equals(texture.Sha256, StringComparison.OrdinalIgnoreCase), "Texture SHA256 mismatch: " + texture.Path);
            if (texture.MimeType == "image/png")
            {
                Require(texture.Bytes.Length >= 33 && texture.Bytes.AsSpan(0, 8).SequenceEqual(new byte[] { 137, 80, 78, 71, 13, 10, 26, 10 }), "Invalid PNG signature.");
                Require(texture.Bytes[24] <= 8, "16-bit PNG is not supported because it cannot be preserved by this 8-bit multipatch writer.");
                for (var offset = 8; offset + 12 <= texture.Bytes.Length;)
                {
                    var length = System.Buffers.Binary.BinaryPrimitives.ReadUInt32BigEndian(texture.Bytes.AsSpan(offset, 4));
                    Require(length <= texture.Bytes.Length - offset - 12, "Invalid PNG chunk length.");
                    Require(!texture.Bytes.AsSpan(offset + 4, 4).SequenceEqual("acTL"u8), "Animated PNG is not supported.");
                    offset = checked(offset + 12 + (int)length);
                }
            }
            else ValidateJpeg(texture.Bytes);
        }
        foreach (var material in Materials!)
        {
            Require(material != null, "Null material.");
            Vector(material!.Color, 4, "material color");
            Require(material.Color.All(v => v is >= 0 and <= 1), "Material RGBA must be in 0..1.");
            Require(material.Texture >= -1 && material.Texture < Textures.Length, "Material texture index is invalid.");
        }
        foreach (var mesh in Meshes)
        {
            Require(mesh != null && mesh.Vertices != null && mesh.Triangles != null && mesh.Triangles.Length > 0, "Mesh must contain vertices and triangles.");
            Require(mesh!.Name != null && mesh.Name.Length <= 512 && mesh.SourceNode != null && mesh.SourceNode.Length <= 2048, "Mesh name or source node exceeds field capacity.");
            foreach (var vertex in mesh.Vertices!)
            {
                Require(vertex != null, "Null vertex.");
                Vector(vertex!.Position, 3, "position");
                if (vertex.Normal != null) { Vector(vertex.Normal, 3, "normal"); Require(Math.Abs(vertex.Normal.Sum(v => v * v) - 1) <= 2e-5, "Normals must be unit length within 1e-5; zero and non-unit normals are not silently normalized."); }
                if (vertex.Uv != null) Vector(vertex.Uv, 2, "uv");
            }
            foreach (var triangle in mesh.Triangles!)
            {
                Require(triangle != null && triangle.Indices != null && triangle.Indices.Length == 3, "Triangle must have exactly three indices.");
                Require(triangle!.Indices!.All(i => i >= 0 && i < mesh.Vertices.Length), "Triangle vertex index is invalid.");
                Require(triangle.Material >= -1 && triangle.Material < Materials.Length, "Triangle material index is invalid.");
                if (triangle.Material >= 0 && Materials[triangle.Material].Texture >= 0) Require(triangle.Indices!.All(i => mesh.Vertices[i].Uv != null), "Textured triangle is missing UV coordinates.");
            }
            var corners = mesh.Triangles.SelectMany(t => t.Indices).Select(i => mesh.Vertices[i]).ToArray();
            Require(!corners.Any(v => v.Normal != null) || corners.All(v => v.Normal != null), "Mixed present and missing normals in a mesh are not supported.");
            foreach (var group in mesh.Triangles.GroupBy(t => t.Material))
            {
                var vertices = group.SelectMany(t => t.Indices).Select(i => mesh.Vertices[i]).ToArray();
                Require(!vertices.Any(v => v.Uv != null) || vertices.All(v => v.Uv != null), "Mixed present and missing UV coordinates in one material patch are not supported.");
            }
        }
        for (var nodeIndex = 0; nodeIndex < Nodes!.Length; nodeIndex++)
        {
            var node = Nodes[nodeIndex];
            Require(node != null && node.Meshes != null, "Null node or node meshes.");
            Vector(node!.SourceWorldTransform, 16, "node source transform");
            Require(node.Parent >= -1 && node.Parent < Nodes.Length && node.Parent != nodeIndex, "Invalid node parent index.");
            Require(node.Meshes!.All(i => i >= 0 && i < Meshes.Length), "Invalid node mesh index.");
        }
        for (var nodeIndex = 0; nodeIndex < Nodes.Length; nodeIndex++)
        {
            var seen = new HashSet<int>();
            for (var index = nodeIndex; index >= 0; index = Nodes[index].Parent) Require(seen.Add(index), "Node hierarchy contains a cycle.");
        }
    }

    private static void ValidateJpeg(byte[] bytes)
    {
        Require(bytes.Length >= 3 && bytes[0] == 255 && bytes[1] == 216 && bytes[2] == 255, "Invalid JPEG signature.");
        for (var offset = 2; offset < bytes.Length;)
        {
            Require(bytes[offset++] == 255, "Invalid JPEG marker.");
            while (offset < bytes.Length && bytes[offset] == 255) offset++;
            Require(offset < bytes.Length, "Truncated JPEG marker.");
            var marker = bytes[offset++];
            if (marker is 0xd8 or >= 0xd0 and <= 0xd7 or 0x01) continue;
            Require(marker is not (0xda or 0xd9) && offset + 2 <= bytes.Length, "JPEG has no supported frame header.");
            var length = (bytes[offset] << 8) | bytes[offset + 1];
            Require(length >= 2 && offset + length <= bytes.Length, "Truncated JPEG segment.");
            if (marker is >= 0xc0 and <= 0xcf && marker is not (0xc4 or 0xc8 or 0xcc))
            {
                Require(length >= 8 && bytes[offset + 2] == 8 && bytes[offset + 7] is 1 or 3, "Only 8-bit grayscale/RGB JPEG is supported; CMYK/YCCK and high-bit-depth JPEG are rejected.");
                return;
            }
            offset += length;
        }
        throw new InvalidDataException("JPEG frame header is missing.");
    }

    private static void Vector(double[]? values, int length, string context) => Require(values != null && values.Length == length && values.All(double.IsFinite), "Invalid " + context + ": expected finite numeric vector of length " + length);
    internal static void Require(bool ok, string message) { if (!ok) throw new InvalidDataException(message); }
    private static string SafePath(string root, string relative)
    {
        Require(!string.IsNullOrWhiteSpace(relative) && !Path.IsPathRooted(relative) && !relative.Contains(':') && !relative.Split('/', '\\').Contains(".."), "Bundle paths must be relative and must not traverse parents.");
        root = Path.GetFullPath(root).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        var candidate = Path.GetFullPath(Path.Combine(root, relative));
        Require(candidate.StartsWith(root + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase), "Bundle path escapes the input directory.");
        for (var current = candidate; current.Length >= root.Length; current = Path.GetDirectoryName(current) ?? "")
        {
            Require((File.GetAttributes(current) & FileAttributes.ReparsePoint) == 0, "Reparse points are not allowed in bundle paths.");
            if (current.Equals(root, StringComparison.OrdinalIgnoreCase)) break;
        }
        return candidate;
    }
}

internal sealed class Coordinates { public required string Unit { get; init; } public required string UpAxis { get; init; } public required string Space { get; init; } public required int Wkid { get; init; } public required double[] Origin { get; init; } public required bool OriginExplicit { get; init; } }
internal sealed class Diagnostic { public required string Severity { get; init; } public required string Code { get; init; } public required string Message { get; init; } public required string Context { get; init; } }
internal sealed class Node { public required string Name { get; init; } public required string SourceId { get; init; } public required int Parent { get; init; } public required double[] SourceWorldTransform { get; init; } public required int[] Meshes { get; init; } }
internal sealed class Mesh { public required string Name { get; init; } public required string SourceNode { get; init; } public required Vertex[] Vertices { get; init; } public required Triangle[] Triangles { get; init; } }
internal sealed class Vertex { public required double[] Position { get; init; } public required double[]? Normal { get; init; } public required double[]? Uv { get; init; } }
internal sealed class Triangle { public required int[] Indices { get; init; } public required int Material { get; init; } }
internal sealed class Material { public required string Name { get; init; } public required double[] Color { get; init; } public required int Texture { get; init; } public required bool DoubleSided { get; init; } }
internal sealed class Texture { public required string Name { get; init; } public required string MimeType { get; init; } public required string Source { get; init; } public required bool Embedded { get; init; } public required string Path { get; init; } public required string Sha256 { get; init; } public required long ByteLength { get; init; } [JsonIgnore] public byte[] Bytes { get; set; } = []; }
