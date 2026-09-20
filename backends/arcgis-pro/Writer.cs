using System.IO;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using ArcGIS.Core.Data;
using ArcGIS.Core.Data.DDL;
using ArcGIS.Core.Geometry;
using ArcGIS.Core.Hosting;

namespace GeoModelBridge.ArcGISPro;

internal static class Writer
{
    private sealed record PreparedTexture(TextureResource Resource, byte[] Buffer, string Storage, int Width, int Height, int BytesPerPixel, string SourceHash);
    private sealed record PreparedMesh(Mesh Source, Multipatch Geometry, List<(int Material, Vertex[] Corners)> Patches);

    public static int Run(Options options)
    {
        if (options.Probe)
        {
            Host.Initialize();
            Console.WriteLine(JsonSerializer.Serialize(new { status = "available", backend = "arcgis-pro-corehost", version = "0.1.4", assembly_version = typeof(Multipatch).Assembly.GetName().Version?.ToString(), license_initialized = true }));
            return 0;
        }
        if (options.VerifyGdb.Length != 0) { Host.Initialize(); return VerifyStandalone(options); }
        EnsureNew(options.Output);
        EnsureNew(options.Report);
        var bundle = Bundle.Load(options.Input);
        Host.Initialize();
        var sr = SpatialReferenceBuilder.CreateSpatialReference(bundle.Coordinates.Wkid);
        Bundle.Require(sr.IsProjected && sr.Unit is LinearUnit linear && Math.Abs(linear.MetersPerUnit - 1) < 1e-12, "WKID must identify a projected coordinate system whose horizontal units are metres.");
        Bundle.Require(sr.ZUnit == null || sr.ZUnit is LinearUnit zUnit && Math.Abs(zUnit.MetersPerUnit - 1) < 1e-12, "Non-metre vertical units are not supported.");
        var textures = bundle.Textures.Select(PrepareTexture).ToArray();
        var materials = bundle.Materials.Select(m => PrepareMaterial(m, textures)).ToArray();
        var defaultMaterial = new BasicMaterial { Color = Colors.White, IsCullBackFace = false };
        var meshes = bundle.Meshes.Select(m => PrepareMesh(m, sr, materials, defaultMaterial)).ToArray();
        var parent = Path.GetDirectoryName(options.Output)!;
        Directory.CreateDirectory(parent);
        Directory.CreateDirectory(Path.GetDirectoryName(options.Report)!);
        var staging = Path.Combine(parent, Path.GetFileNameWithoutExtension(options.Output) + ".gmb-" + Guid.NewGuid().ToString("N") + ".gdb");
        var stagedReport = options.Report + ".gmb-" + Guid.NewGuid().ToString("N") + ".tmp";
        var committed = false;
        try
        {
            Write(staging, options.FeatureClass, sr, meshes);
            var checks = Verify(staging, options.FeatureClass, sr, meshes, materials, defaultMaterial, bundle, textures);
            var report = new
            {
                status = "written_and_readback_verified", version = "0.1.4", backend = "arcgis-pro-corehost",
                input = options.Input, source = bundle.Source, reader_diagnostics = bundle.Diagnostics.Select(d => new { severity = d.Severity, code = d.Code, message = d.Message, context = d.Context }), output = options.Output, feature_class = options.FeatureClass,
                conversion_profile = bundle.ConversionProfile,
                compatibility_adjustments = bundle.Diagnostics.Any(d => d.Code is "STATIC_POSE_USED" or "MATERIAL_CHANNEL_OMITTED" or "DEGENERATE_TRIANGLES_REMOVED" or "JPEG_CONTAINER_NORMALIZED"),
                timestamp_utc = DateTimeOffset.UtcNow, assembly_version = typeof(Multipatch).Assembly.GetName().Version?.ToString(),
                texture_coordinate_policy = new { source = "FBX UV: V=0 at bottom", target = "FileGDB texture row: T=0 at top", mapping = "S=U; T=1-V", image_rows_reordered = false },
                coordinate_system = new { wkid = sr.Wkid, name = sr.Name, projected = sr.IsProjected, unit = "meter", source_coordinates_assigned_without_reprojection = true, origin_already_applied_by_reader = bundle.Coordinates.Origin },
                verification = new { level = "closed_reopened_file_geodatabase", graphical_acceptance = "pending_manual_review_in_target_software", feature_count = meshes.Length, geometry_material_uv_texture_readback = true, coordinate_tolerance_xy = sr.XYResolution * 2, coordinate_tolerance_z = 2 / sr.ZScale, uv_float_tolerance = "max(1,abs(value))*2e-6", normal_storage_model = "Exact component check against floor(float32(source)*128+0.5)/128; empirically verified with ArcGIS Pro 3.6.2, not an Esri cross-version guarantee.", checks },
                material_quantization = bundle.Materials.Select((m, i) => new { index = i, name = m.Name, source_rgba = m.Color, stored_rgb8 = new int[] { materials[i].Color.R, materials[i].Color.G, materials[i].Color.B }, stored_transparency_percent = materials[i].TransparencyPercent, stored_opacity = 1 - materials[i].TransparencyPercent / 100.0, double_sided = m.DoubleSided }),
                textures = textures.Select((t, i) => new { index = i, source_sha256 = t.SourceHash, storage = t.Storage, width = t.Width, height = t.Height, bytes_per_pixel = t.BytesPerPixel, stored_byte_length = t.Buffer.Length, stored_sha256 = Convert.ToHexString(SHA256.HashData(t.Buffer)).ToLowerInvariant(), readback_bytes_equal = true }),
                limitations = new[] { "Requires a licensed, installed ArcGIS Pro; this is not the native FileGDB API backend.", "RGB quantizes to 8-bit; material opacity quantizes to whole-percent transparency.", "FileGDB normal components quantize to a 1/128 grid in tested Pro 3.6.2; source normal hashes and actual component/angular errors are reported. Normals are not lossless or renormalized.", "PNG is stored as top-left row-major unpremultiplied RGBA8; original PNG container metadata is retained only in the source bundle.", "JPEG compressed bytes are preserved without re-encoding; ICC appearance and UV orientation require visual acceptance.", "No reprojection, datum conversion, vertical-datum conversion, or texture baking is performed.", "The writer validates SDK readback, not how ArcGIS Pro or GeoScene renders the result." }
            };
            using (var stream = new FileStream(stagedReport, FileMode.CreateNew, FileAccess.Write, FileShare.None)) JsonSerializer.Serialize(stream, report, new JsonSerializerOptions { WriteIndented = true });
            EnsureNew(options.Output);
            EnsureNew(options.Report);
            Directory.Move(staging, options.Output);
            committed = true;
            File.Move(stagedReport, options.Report, false);
            Console.WriteLine(JsonSerializer.Serialize(new { status = report.status, output = options.Output, report = options.Report, feature_count = meshes.Length, graphical_acceptance = "pending" }));
            return 0;
        }
        catch
        {
            // These two paths contain a fresh random ID created by this invocation only.
            // A committed output is never deleted, including when a later report move fails.
            if (!committed && Directory.Exists(staging))
            {
                try { Directory.Delete(staging, true); }
                catch (Exception cleanup) { Console.Error.WriteLine("Temporary geodatabase cleanup failed; retained at " + staging + ": " + cleanup.Message); }
            }
            if (File.Exists(stagedReport))
            {
                if (committed) Console.Error.WriteLine("Output geodatabase is verified; report retained at " + stagedReport);
                else File.Delete(stagedReport);
            }
            throw;
        }
    }

    private static PreparedTexture PrepareTexture(Texture texture)
    {
        using var stream = new MemoryStream(texture.Bytes, false);
        var decoder = BitmapDecoder.Create(stream, BitmapCreateOptions.PreservePixelFormat | BitmapCreateOptions.IgnoreColorProfile, BitmapCacheOption.OnLoad);
        Bundle.Require(decoder.Frames.Count == 1, "Animated/multi-frame images are not supported.");
        var frame = decoder.Frames[0];
        var width = frame.PixelWidth;
        var height = frame.PixelHeight;
        Bundle.Require(width > 0 && height > 0 && width <= 16384 && height <= 16384 && (long)width * height * 4 <= 256L * 1024 * 1024, "Texture dimensions exceed the 16384-axis / 256 MiB decoded limit.");
        if (texture.MimeType == "image/jpeg")
        {
            try
            {
                return new(new TextureResource(new JPEGTexture(texture.Bytes)), texture.Bytes, "jpeg_original_bytes", width, height, 3, texture.Sha256);
            }
            catch (ArgumentException error)
            {
                throw new InvalidDataException($"PRO_JPEG_UNSUPPORTED: ArcGIS Pro rejected JPEG texture '{texture.Name}' ({width}x{height}, SHA-256 {texture.Sha256}) although Windows decoded it. Re-read the original FBX with the gis-static profile to normalize a supported JPEG container. If its color space or orientation cannot be handled safely, export a standard JPEG in the source application. Switching database writers alone does not repair this container. No texture re-encoding was attempted.", error);
            }
        }
        // WPF Bgra32 is straight (not premultiplied) alpha. IgnoreColorProfile prevents ICC conversion.
        var bitmap = frame.Format == PixelFormats.Bgra32 ? (BitmapSource)frame : new FormatConvertedBitmap(frame, PixelFormats.Bgra32, null, 0);
        var buffer = new byte[checked(width * height * 4)];
        bitmap.CopyPixels(buffer, checked(width * 4), 0);
        for (var i = 0; i < buffer.Length; i += 4) (buffer[i], buffer[i + 2]) = (buffer[i + 2], buffer[i]);
        return new(new TextureResource(new UncompressedTexture(buffer, width, height, 4)), buffer, "rgba8_uncompressed", width, height, 4, texture.Sha256);
    }

    private static BasicMaterial PrepareMaterial(Material material, PreparedTexture[] textures)
    {
        var color = material.Color;
        var result = new BasicMaterial
        {
            Color = Color.FromRgb(Quantize(color[0]), Quantize(color[1]), Quantize(color[2])),
            TransparencyPercent = (byte)Math.Round((1 - color[3]) * 100, MidpointRounding.AwayFromZero),
            IsCullBackFace = !material.DoubleSided
        };
        if (material.Texture >= 0) result.TextureResource = textures[material.Texture].Resource;
        return result;
    }

    private static byte Quantize(double v) => (byte)Math.Round(v * 255, MidpointRounding.AwayFromZero);
    private static Coordinate3D Coord3(double[] v) => new(v[0], v[1], v[2]);

    private static PreparedMesh PrepareMesh(Mesh mesh, SpatialReference sr, BasicMaterial[] materials, BasicMaterial defaultMaterial)
    {
        var builder = new MultipatchBuilderEx(sr);
        var expected = new List<(int Material, Vertex[] Corners)>();
        foreach (var group in mesh.Triangles.GroupBy(t => t.Material))
        {
            var corners = group.SelectMany(t => t.Indices).Select(i => mesh.Vertices[i]).ToArray();
            var patch = builder.MakePatch(PatchType.Triangles);
            patch.Coords = corners.Select(v => Coord3(v.Position)).ToList();
            patch.Material = group.Key >= 0 ? materials[group.Key] : defaultMaterial;
            // FileGDB texture row zero is T=0; FBX V=0 is the bottom of an image.
            // Convert coordinates for both PNG and JPEG without changing source image bytes.
            if (corners.All(v => v.Uv != null)) patch.TextureCoords2D = corners.Select(v => new Coordinate2D(v.Uv![0], 1 - v.Uv[1])).ToList();
            if (corners.All(v => v.Normal != null))
            {
                builder.HasNormals = true;
                patch.Normals = corners.Select(v => Coord3(v.Normal!)).ToList();
            }
            builder.Patches.Add(patch);
            expected.Add((group.Key, corners));
        }
        return new(mesh, builder.ToGeometry(), expected);
    }

    private static void Write(string gdbPath, string featureClass, SpatialReference sr, PreparedMesh[] meshes)
    {
        using var gdb = SchemaBuilder.CreateGeodatabase(new FileGeodatabaseConnectionPath(new Uri(gdbPath)));
        var description = new FeatureClassDescription(featureClass,
        [
            FieldDescription.CreateObjectIDField(),
            FieldDescription.CreateIntegerField("MeshIndex"),
            FieldDescription.CreateStringField("MeshName", 512),
            FieldDescription.CreateStringField("SourceNode", 2048)
        ], new ShapeDescription(GeometryType.Multipatch, sr) { HasZ = true, HasM = false });
        var schema = new SchemaBuilder(gdb);
        schema.Create(description);
        if (!schema.Build()) throw new InvalidOperationException("Feature class creation failed: " + string.Join("; ", schema.ErrorMessages));
        using var table = gdb.OpenDataset<FeatureClass>(featureClass);
        using var definition = table.GetDefinition();
        var shapeField = definition.GetShapeField();
        gdb.ApplyEdits(() =>
        {
            for (var i = 0; i < meshes.Length; i++)
            {
                using var rowBuffer = table.CreateRowBuffer();
                rowBuffer[shapeField] = meshes[i].Geometry;
                rowBuffer["MeshIndex"] = i;
                rowBuffer["MeshName"] = meshes[i].Source.Name;
                rowBuffer["SourceNode"] = meshes[i].Source.SourceNode;
                using var row = table.CreateRow(rowBuffer);
            }
        });
    }

    private static List<object> Verify(string gdbPath, string featureClass, SpatialReference sr, PreparedMesh[] meshes, BasicMaterial[] materials, BasicMaterial defaultMaterial, Bundle bundle, PreparedTexture[] textures)
    {
        // Deliberately open a fresh connection after Write disposed every row, dataset and geodatabase.
        using var gdb = new Geodatabase(new FileGeodatabaseConnectionPath(new Uri(gdbPath)));
        using var table = gdb.OpenDataset<FeatureClass>(featureClass);
        using var definition = table.GetDefinition();
        Bundle.Require(definition.GetShapeType() == GeometryType.Multipatch && definition.GetSpatialReference().Wkid == sr.Wkid, "Readback feature class type or WKID mismatch.");
        Bundle.Require(table.GetCount() == meshes.Length, "Readback feature count mismatch.");
        using var cursor = table.Search(new QueryFilter(), false);
        var seen = new HashSet<int>();
        var checks = new List<object>();
        while (cursor.MoveNext())
        {
            using var feature = (Feature)cursor.Current;
            var index = Convert.ToInt32(feature["MeshIndex"]);
            Bundle.Require(index >= 0 && index < meshes.Length && seen.Add(index), "Readback mesh index is invalid or duplicated.");
            var source = meshes[index];
            Bundle.Require((string)feature["MeshName"] == source.Source.Name && (string)feature["SourceNode"] == source.Source.SourceNode, "Readback feature attributes mismatch.");
            var geometry = feature.GetShape() as Multipatch ?? throw new InvalidDataException("Readback geometry is not a multipatch.");
            Bundle.Require(geometry.PartCount == source.Patches.Count && geometry.PointCount == source.Patches.Sum(p => p.Corners.Length), "Readback patch/vertex count mismatch.");
            Bundle.Require(geometry.MaterialCount == source.Geometry.MaterialCount, "Readback material count mismatch.");
            var texturedPatches = 0;
            var normalCount = 0;
            var quantizedNormalCount = 0;
            double maxNormalComponentError = 0, maxNormalAngleDegrees = 0, maxNormalLengthError = 0, maxBuilderNormalError = 0;
            using var sourceNormalBytes = new MemoryStream();
            using var sourceNormalWriter = new BinaryWriter(sourceNormalBytes, System.Text.Encoding.UTF8, true);
            for (var patchIndex = 0; patchIndex < source.Patches.Count; patchIndex++)
            {
                var expected = source.Patches[patchIndex];
                Bundle.Require(geometry.GetPatchType(patchIndex) == PatchType.Triangles && geometry.GetPatchPointCount(patchIndex) == expected.Corners.Length, "Readback patch type/size mismatch.");
                var uvExpected = expected.Corners.All(v => v.Uv != null);
                Bundle.Require(geometry.GetPatchTextureVertexCount(patchIndex) == (uvExpected ? expected.Corners.Length : 0), "Readback UV count mismatch.");
                var start = geometry.GetPatchStartPointIndex(patchIndex);
                for (var i = 0; i < expected.Corners.Length; i++)
                {
                    var vertex = expected.Corners[i];
                    var point = geometry.Points[start + i];
                    Close(point.X, vertex.Position[0], sr.XYResolution * 2, "X coordinate");
                    Close(point.Y, vertex.Position[1], sr.XYResolution * 2, "Y coordinate");
                    Close(point.Z, vertex.Position[2], 2 / sr.ZScale, "Z coordinate");
                    if (uvExpected)
                    {
                        var uv = geometry.GetPatchTextureCoordinate(patchIndex, i);
                        CloseFloat(uv.X, vertex.Uv![0], "S=U"); CloseFloat(uv.Y, 1 - vertex.Uv[1], "T=1-V");
                    }
                    if (vertex.Normal != null)
                    {
                        Bundle.Require(geometry.HasNormals, "Readback normals are missing.");
                        var normal = geometry.GetPatchNormal(patchIndex, i);
                        var built = source.Geometry.GetPatchNormal(patchIndex, i);
                        var sourceValues = vertex.Normal;
                        var builtValues = new[] { built.X, built.Y, built.Z };
                        var storedValues = new[] { normal.X, normal.Y, normal.Z };
                        normalCount++;
                        var changed = false;
                        for (var component = 0; component < 3; component++)
                        {
                            sourceNormalWriter.Write(sourceValues[component]);
                            // Builder retains float32 normals; FileGDB independently quantizes them.
                            Close(builtValues[component], (double)(float)sourceValues[component], 1e-12, "builder normal component");
                            var predicted = Math.Floor(builtValues[component] * 128 + 0.5) / 128;
                            Bundle.Require(storedValues[component] == predicted, FormattableString.Invariant($"Readback normal component disagrees with the measured Pro 3.6.2 FileGDB codec: source={sourceValues[component]:R}, builder={builtValues[component]:R}, predicted={predicted:R}, actual={storedValues[component]:R}."));
                            var error = Math.Abs(storedValues[component] - sourceValues[component]);
                            changed |= error > 1e-9;
                            maxNormalComponentError = Math.Max(maxNormalComponentError, error);
                            maxBuilderNormalError = Math.Max(maxBuilderNormalError, Math.Abs(builtValues[component] - sourceValues[component]));
                        }
                        if (changed) quantizedNormalCount++;
                        var sourceLength = Math.Sqrt(sourceValues.Sum(n => n * n));
                        var storedLength = Math.Sqrt(storedValues.Sum(n => n * n));
                        Bundle.Require(storedLength > 0, "Readback quantized normal became zero.");
                        var cosine = (sourceValues[0] * storedValues[0] + sourceValues[1] * storedValues[1] + sourceValues[2] * storedValues[2]) / (sourceLength * storedLength);
                        maxNormalAngleDegrees = Math.Max(maxNormalAngleDegrees, Math.Acos(Math.Clamp(cosine, -1, 1)) * 180 / Math.PI);
                        maxNormalLengthError = Math.Max(maxNormalLengthError, Math.Abs(storedLength - sourceLength));
                    }
                }
                var materialIndex = geometry.GetPatchMaterialIndex(patchIndex);
                var material = expected.Material < 0 ? defaultMaterial : materials[expected.Material];
                Bundle.Require(materialIndex >= 0 && geometry.GetMaterialColor(materialIndex) == material.Color && geometry.GetMaterialTransparencyPercent(materialIndex) == material.TransparencyPercent && geometry.IsMaterialCullBackFace(materialIndex) == material.IsCullBackFace, "Readback material color, opacity, or culling mismatch.");
                var textureIndex = expected.Material < 0 ? -1 : bundle.Materials[expected.Material].Texture;
                Bundle.Require(geometry.IsMaterialTextured(materialIndex) == (textureIndex >= 0), "Readback material texture binding mismatch.");
                if (textureIndex >= 0)
                {
                    texturedPatches++;
                    var expectedTexture = textures[textureIndex];
                    Bundle.Require(geometry.GetMaterialTexture(materialIndex).AsSpan().SequenceEqual(expectedTexture.Buffer), "Readback texture bytes differ from the original JPEG or decoded PNG RGBA pixels.");
                    Bundle.Require(geometry.GetMaterialTextureColumnCount(materialIndex) == expectedTexture.Width && geometry.GetMaterialTextureRowCount(materialIndex) == expectedTexture.Height, "Readback texture dimensions mismatch.");
                    var expectedCompression = expectedTexture.Storage == "jpeg_original_bytes" ? TextureCompressionType.CompressionJPEG : TextureCompressionType.CompressionNone;
                    Bundle.Require(geometry.GetMaterialTextureCompressionType(materialIndex) == expectedCompression, "Readback texture compression changed.");
                    if (expectedCompression == TextureCompressionType.CompressionNone) Bundle.Require(geometry.GetMaterialTextureBytesPerPixel(materialIndex) == 4, "Readback PNG alpha channel is missing.");
                }
            }
            sourceNormalWriter.Flush();
            checks.Add(new { mesh_index = index, mesh_name = source.Source.Name, source_node = source.Source.SourceNode, source_vertices = source.Source.Vertices.Length, written_corner_vertices = geometry.PointCount, patches = geometry.PartCount, materials = geometry.MaterialCount, texture_coordinate_count = geometry.TextureVertexCount, textured_patches = texturedPatches, normals_present = geometry.HasNormals,
                normal_storage = new { source_corner_normal_count = normalCount, quantized_corner_normal_count = quantizedNormalCount, source_normals_sha256 = normalCount > 0 ? Convert.ToHexString(SHA256.HashData(sourceNormalBytes.ToArray())).ToLowerInvariant() : null, source_hash_encoding = "little-endian binary64 XYZ, material-grouped triangle-corner order", component_grid_step = 1.0 / 128, half_step = 1.0 / 256, midpoint_rounding = "toward_positive_infinity", renormalized = false, exact_codec_prediction_verified = true, max_builder_float32_component_error = maxBuilderNormalError, max_component_error = maxNormalComponentError, max_angular_error_degrees = maxNormalAngleDegrees, max_vector_length_error = maxNormalLengthError },
                signature = Snapshot(geometry), passed = true });
        }
        Bundle.Require(seen.Count == meshes.Length, "Readback has missing meshes.");
        return checks;
    }

    private static JsonElement Snapshot(Multipatch geometry)
    {
        var patches = new List<object>();
        using var geometryBytes = new MemoryStream();
        using var writer = new BinaryWriter(geometryBytes, System.Text.Encoding.UTF8, true);
        for (var patch = 0; patch < geometry.PartCount; patch++)
        {
            var pointCount = geometry.GetPatchPointCount(patch);
            var uvCount = geometry.GetPatchTextureVertexCount(patch);
            var start = geometry.GetPatchStartPointIndex(patch);
            writer.Write((int)geometry.GetPatchType(patch)); writer.Write(pointCount); writer.Write(uvCount);
            for (var pointIndex = 0; pointIndex < pointCount; pointIndex++)
            {
                var point = geometry.Points[start + pointIndex];
                writer.Write(point.X); writer.Write(point.Y); writer.Write(point.Z);
                if (geometry.HasNormals)
                {
                    var normal = geometry.GetPatchNormal(patch, pointIndex);
                    writer.Write(normal.X); writer.Write(normal.Y); writer.Write(normal.Z);
                }
                if (uvCount > 0)
                {
                    var uv = geometry.GetPatchTextureCoordinate(patch, pointIndex);
                    writer.Write(uv.X); writer.Write(uv.Y);
                }
            }
            var material = geometry.GetPatchMaterialIndex(patch);
            Bundle.Require(material >= 0, "Standalone snapshot requires a material on every patch.");
            var color = geometry.GetMaterialColor(material);
            var textured = geometry.IsMaterialTextured(material);
            patches.Add(new
            {
                patch_index = patch, patch_type = geometry.GetPatchType(patch).ToString(), point_count = pointCount, uv_count = uvCount,
                material_index = material, color_rgb8 = new int[] { color.R, color.G, color.B }, transparency_percent = geometry.GetMaterialTransparencyPercent(material), cull_back_face = geometry.IsMaterialCullBackFace(material),
                textured, texture_sha256 = textured ? Convert.ToHexString(SHA256.HashData(geometry.GetMaterialTexture(material))).ToLowerInvariant() : null,
                texture_width = textured ? geometry.GetMaterialTextureColumnCount(material) : 0,
                texture_height = textured ? geometry.GetMaterialTextureRowCount(material) : 0,
                texture_bytes_per_pixel = textured ? geometry.GetMaterialTextureBytesPerPixel(material) : 0,
                texture_compression = textured ? geometry.GetMaterialTextureCompressionType(material).ToString() : null
            });
        }
        writer.Flush();
        return JsonSerializer.SerializeToElement(new
        {
            version = 1, point_count = geometry.PointCount, patch_count = geometry.PartCount, material_count = geometry.MaterialCount,
            wkid = geometry.SpatialReference.Wkid, has_normals = geometry.HasNormals,
            coordinates_normals_uv_sha256 = Convert.ToHexString(SHA256.HashData(geometryBytes.ToArray())).ToLowerInvariant(), patches
        });
    }

    private static int VerifyStandalone(Options options)
    {
        EnsureNew(options.Report);
        Bundle.Require(new FileInfo(options.ExpectedReport).Length <= 64L * 1024 * 1024, "Expected report exceeds 64 MiB.");
        using var expectedDocument = JsonDocument.Parse(File.ReadAllBytes(options.ExpectedReport));
        var expected = expectedDocument.RootElement;
        Bundle.Require(expected.GetProperty("status").GetString() == "written_and_readback_verified" && expected.GetProperty("backend").GetString() == "arcgis-pro-corehost", "Expected report is not a verified ArcGIS Pro writer report.");
        var featureClass = expected.GetProperty("feature_class").GetString() ?? throw new InvalidDataException("Missing expected feature class.");
        var verification = expected.GetProperty("verification");
        var expectedCount = verification.GetProperty("feature_count").GetInt32();
        var expectedChecks = verification.GetProperty("checks").EnumerateArray().ToDictionary(c => c.GetProperty("mesh_index").GetInt32());
        Bundle.Require(expectedCount > 0 && expectedChecks.Count == expectedCount, "Invalid expected feature count.");
        var seen = new HashSet<int>();
        using (var gdb = new Geodatabase(new FileGeodatabaseConnectionPath(new Uri(options.VerifyGdb))))
        using (var table = gdb.OpenDataset<FeatureClass>(featureClass))
        using (var definition = table.GetDefinition())
        {
            Bundle.Require(definition.GetShapeType() == GeometryType.Multipatch && table.GetCount() == expectedCount, "Copied GDB shape type or feature count does not match the expected report.");
            using var cursor = table.Search(new QueryFilter(), false);
            while (cursor.MoveNext())
            {
                using var feature = (Feature)cursor.Current;
                var index = Convert.ToInt32(feature["MeshIndex"]);
                Bundle.Require(expectedChecks.TryGetValue(index, out var check) && seen.Add(index), "Copied GDB has an unexpected or duplicate mesh index.");
                Bundle.Require((string)feature["MeshName"] == check.GetProperty("mesh_name").GetString() && (string)feature["SourceNode"] == check.GetProperty("source_node").GetString(), "Copied GDB feature attributes differ.");
                var geometry = feature.GetShape() as Multipatch ?? throw new InvalidDataException("Copied GDB geometry is not a multipatch.");
                Bundle.Require(check.TryGetProperty("signature", out var original), "Expected report has no standalone signature; regenerate it with the current writer.");
                var actual = Snapshot(geometry);
                Bundle.Require(JsonNode.DeepEquals(JsonNode.Parse(original.GetRawText()), JsonNode.Parse(actual.GetRawText())), "Copied GDB geometry, material binding, color, UV, normals or embedded texture differs from the expected report.");
            }
        }
        Bundle.Require(seen.Count == expectedCount, "Copied GDB is missing features.");
        Directory.CreateDirectory(Path.GetDirectoryName(options.Report)!);
        var report = new
        {
            status = "standalone_copy_verified", version = "0.1.4", backend = "arcgis-pro-corehost",
            geodatabase = options.VerifyGdb, expected_report = options.ExpectedReport, feature_class = featureClass,
            timestamp_utc = DateTimeOffset.UtcNow, feature_count = expectedCount,
            verification = new { level = "standalone_geodatabase_against_original_readback_signatures", independent_of_bundle_and_source_textures = true, exact_geometry_uv_normal_hashes = true, exact_material_properties_and_texture_hashes = true, graphical_acceptance = "pending_manual_review_in_target_software" }
        };
        using (var stream = new FileStream(options.Report, FileMode.CreateNew, FileAccess.Write, FileShare.None)) JsonSerializer.Serialize(stream, report, new JsonSerializerOptions { WriteIndented = true });
        Console.WriteLine(JsonSerializer.Serialize(new { status = report.status, report = options.Report, feature_count = expectedCount, independent_of_bundle = true }));
        return 0;
    }

    private static void CloseFloat(double actual, double expected, string context) => Close(actual, expected, Math.Max(1, Math.Abs(expected)) * 2e-6, context);
    private static void Close(double actual, double expected, double tolerance, string context) => Bundle.Require(double.IsFinite(actual) && double.IsFinite(tolerance) && Math.Abs(actual - expected) <= Math.Max(tolerance, 1e-9), FormattableString.Invariant($"Readback {context} differs beyond the documented storage tolerance: actual={actual:R}, expected={expected:R}, tolerance={Math.Max(tolerance, 1e-9):R}."));
    private static void EnsureNew(string path) { if (File.Exists(path) || Directory.Exists(path)) throw new IOException("Refusing to overwrite existing output: " + path); }
}
