using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Nodes;
using GeoModelBridge.Gui.Core;

internal static partial class Program
{
    private static InspectionSettings InspectionRequest(string name) => InspectionSettings.FromConversion(Settings,
        Path.Combine(Work, "inspection-" + name + ".json"));

    private static JsonObject GoodInspection(InspectionSettings settings)
    {
        var diagnostics = new JsonArray();
        if (string.Equals(Path.GetExtension(settings.InputPath), ".max", StringComparison.OrdinalIgnoreCase))
            diagnostics.Add(new JsonObject { ["severity"] = "info", ["code"] = "MAX_ADAPTER_PROVENANCE",
                ["context"] = settings.InputPath, ["message"] = new JsonObject
                {
                    ["adapter_protocol_version"] = 1, ["engine_version"] = Version, ["status"] = "exported",
                    ["frame"] = int.Parse(settings.MaxFrame), ["source"] = settings.InputPath,
                    ["batch_executable"] = settings.MaxBatchPath
                }.ToJsonString() });
        return new JsonObject
        {
            ["schema_version"] = 1, ["version"] = Version, ["status"] = "inspected", ["backend"] = "none",
            ["source"] = settings.InputPath, ["conversion_profile"] = settings.Profile,
            ["missing_texture_policy"] = settings.MissingTexturePolicy,
            ["coordinates"] = new JsonObject { ["space"] = "local", ["unit"] = "meter", ["up_axis"] = "Z",
                ["wkid"] = 0, ["origin_explicit"] = false, ["origin"] = new JsonArray(0, 0, 0) },
            ["counts"] = new JsonObject { ["meshes"] = 1, ["triangles"] = 2, ["corner_vertices"] = 4,
                ["materials"] = 1, ["textures"] = 1, ["texture_bytes"] = 123 },
            ["geometry_bounds"] = new JsonObject { ["min"] = new JsonArray(-2, -3, 0), ["max"] = new JsonArray(4, 5, 0) },
            ["fidelity"] = new JsonObject { ["validation_passed"] = true, ["strict_validation_passed"] = settings.Profile == "strict",
                ["compatibility_adjustments"] = false, ["gdb_written"] = false, ["gdb_readback_verified"] = false },
            ["diagnostics"] = diagnostics
        };
    }

    private static void InspectionContractTests()
    {
        var settings = InspectionRequest("arguments");
        Test("inspection_requires_no_output_or_placement", () =>
        {
            Assert(InspectionValidator.ValidateReader(settings with { ReportPath = "" }).Count == 0,
                "Selecting a report destination requires an unrelated preliminary report location.");
            Assert(InspectionValidator.Validate(settings).Count == 0, "Reader-only request unexpectedly required GDB/WKID/origin.");
            var arguments = InspectionCommand.BuildArguments(settings);
            Assert(arguments[0] == "inspect" && arguments[1] == Input, "Wrong command/source.");
            Assert(!arguments.Any(a => a is "--output" or "--wkid" or "--origin" or "--backend" or "--feature-class"), "Preflight contains conversion options.");
            var info = InspectionCommand.CreateStartInfo("geomodelbridge.exe", settings);
            Assert(!info.UseShellExecute && info.ArgumentList.Contains(Input), "Reader paths are not safely passed as separate arguments.");
        });
        Test("inspection_rejects_existing_or_gdb_report_paths", () =>
        {
            foreach (var path in new[] { Input, Work, "", Path.Combine(Work, "disguised.gdb"), Path.Combine(Work, "nested.gdb", "report.json") })
                Assert(InspectionValidator.Validate(settings with { ReportPath = path }).Count > 0, "Invalid report path accepted: " + path);
        });
        Test("inspection_validates_obj_reader_options", () =>
        {
            var input = Path.Combine(Work, "inspect reader.obj");
            File.WriteAllText(input, "v 0 0 0");
            var request = settings with { InputPath = input, ObjUpAxis = "Y", ObjUnitMeters = "0.01" };
            var arguments = InspectionCommand.BuildArguments(request).ToArray();
            Assert(After(arguments, "--obj-up-axis") == "Y" && After(arguments, "--obj-unit-meters") == "0.01", "OBJ source normalization was omitted.");
            Assert(InspectionValidator.Validate(request with { ObjUnitMeters = "NaN" }).Count > 0, "Invalid OBJ unit accepted.");
        });
        Test("inspection_report_includes_counts_local_bounds_and_no_gdb_claim", () =>
        {
            var report = InspectionReportVerifier.Verify(GoodInspection(settings).ToJsonString(), settings);
            Assert(report.Counts.Triangles == 2 && report.Bounds.Min.X == -2 && report.Bounds.Max.Y == 5, "Inspection data changed.");
            Assert(report.SummaryText.Contains("未写入 GDB") && report.SummaryText.Contains("未定位"), "Preflight was presented as a placed GDB.");
        });
        Test("inspection_supports_large_64_bit_counts", () =>
        {
            var report = GoodInspection(settings);
            report["counts"]!["triangles"] = 3_000_000_000L;
            Assert(InspectionReportVerifier.Verify(report.ToJsonString(), settings).Counts.Triangles == 3_000_000_000L, "Large count truncated.");
        });
        var mutations = new Dictionary<string, Action<JsonObject>>
        {
            ["schema"] = r => r["schema_version"] = 2,
            ["version"] = r => r["version"] = "0.0.1",
            ["converted_status"] = r => r["status"] = "written_and_readback_verified",
            ["backend"] = r => r["backend"] = "native-filegdb",
            ["source"] = r => r["source"] = Path.Combine(Work, "wrong.fbx"),
            ["relative_source"] = r => r["source"] = Path.GetFileName(Input),
            ["profile"] = r => r["conversion_profile"] = "strict",
            ["missing_policy"] = r => r["missing_texture_policy"] = "error",
            ["validation"] = r => r["fidelity"]!["validation_passed"] = false,
            ["written"] = r => r["fidelity"]!["gdb_written"] = true,
            ["readback"] = r => r["fidelity"]!["gdb_readback_verified"] = true,
            ["strict_claim"] = r => r["fidelity"]!["strict_validation_passed"] = true,
            ["fidelity_type"] = r => r["fidelity"]!["compatibility_adjustments"] = "false",
            ["referenced"] = r => r["coordinates"]!["space"] = "referenced",
            ["wkid"] = r => r["coordinates"]!["wkid"] = 32650,
            ["origin"] = r => r["coordinates"]!["origin"]![0] = 500000,
            ["explicit_origin"] = r => r["coordinates"]!["origin_explicit"] = true,
            ["unit"] = r => r["coordinates"]!["unit"] = "foot",
            ["axis"] = r => r["coordinates"]!["up_axis"] = "Y",
            ["count_missing"] = r => r["counts"]!.AsObject().Remove("texture_bytes"),
            ["negative_count"] = r => r["counts"]!["textures"] = -1,
            ["fractional_count"] = r => r["counts"]!["triangles"] = 1.5,
            ["empty_meshes"] = r => r["counts"]!["meshes"] = 0,
            ["empty_materials"] = r => r["counts"]!["materials"] = 0,
            ["inconsistent_counts"] = r => r["counts"]!["meshes"] = 3,
            ["inconsistent_texture_bytes"] = r => r["counts"]!["textures"] = 0,
            ["bounds_missing"] = r => r.Remove("geometry_bounds"),
            ["bounds_reversed"] = r => r["geometry_bounds"]!["min"]![0] = 10,
            ["bounds_dimensions"] = r => r["geometry_bounds"]!["max"] = new JsonArray(1, 2),
            ["bounds_type"] = r => r["geometry_bounds"]!["min"]![0] = "NaN",
            ["diagnostics_missing"] = r => r.Remove("diagnostics"),
            ["diagnostics_type"] = r => r["diagnostics"] = new JsonObject(),
            ["diagnostic_error"] = r => r["diagnostics"]!.AsArray().Add(new JsonObject { ["severity"] = "error", ["code"] = "FAILED", ["message"] = "bad" }),
            ["diagnostic_context"] = r => r["diagnostics"]!.AsArray().Add(new JsonObject { ["severity"] = "warning", ["code"] = "X", ["message"] = "bad", ["context"] = 7 })
        };
        foreach (var (name, change) in mutations)
            Test("inspection_rejects_" + name, () =>
            {
                var report = GoodInspection(settings); change(report);
                ExpectInspectionRejected(report.ToJsonString(), settings);
            });
        Test("inspection_rejects_duplicate_fields_and_nonfinite_bounds", () =>
        {
            var json = GoodInspection(settings).ToJsonString();
            ExpectInspectionRejected(json.Replace("\"backend\":", "\"backend\":\"native-filegdb\",\"backend\":"), settings);
            ExpectInspectionRejected(json.Replace("[-2,-3,0]", "[1e999,-3,0]"), settings);
        });
        Test("inspection_strict_can_record_unlit_compatibility", () =>
        {
            var strict = settings with { Profile = "strict" };
            var report = GoodInspection(strict);
            report["fidelity"]!["compatibility_adjustments"] = true;
            report["fidelity"]!["strict_validation_passed"] = false;
            report["diagnostics"]!.AsArray().Add(new JsonObject { ["severity"] = "info", ["code"] = "UNLIT_SHADING_MAPPED", ["message"] = "Unlit mapped." });
            InspectionReportVerifier.Verify(report.ToJsonString(), strict);
        });
        Test("inspection_warnings_preserved_and_error_policy_rejects_fallback", () =>
        {
            var report = GoodInspection(settings);
            report["fidelity"]!["compatibility_adjustments"] = true;
            report["diagnostics"]!.AsArray().Add(new JsonObject { ["severity"] = "warning", ["code"] = "MISSING_TEXTURE_FALLBACK", ["message"] = "unavailable image" });
            var parsed = InspectionReportVerifier.Verify(report.ToJsonString(), settings);
            Assert(parsed.WarningCount == 1 && parsed.Diagnostics[0].Contains("MISSING_TEXTURE_FALLBACK"), "Missing image warning lost.");
            report["missing_texture_policy"] = "error";
            ExpectInspectionRejected(report.ToJsonString(), settings with { MissingTexturePolicy = "error" });
        });
        Test("inspection_requires_matching_unique_max_provenance", () =>
        {
            var request = settings with { InputPath = Path.Combine(Work, "scene.max"), MaxBatchPath = Path.Combine(Work, "runtime.exe"), MaxFrame = "-12" };
            InspectionReportVerifier.Verify(GoodInspection(request).ToJsonString(), request);
            var report = GoodInspection(request);
            ExpectInspectionRejected(report.ToJsonString(), request with { MaxFrame = "5" });
            ExpectInspectionRejected(report.ToJsonString(), request with { MaxBatchPath = Path.Combine(Work, "other.exe") });
            report["diagnostics"]!.AsArray().Add(report["diagnostics"]![0]!.DeepClone());
            ExpectInspectionRejected(report.ToJsonString(), request);
            report["diagnostics"] = new JsonArray();
            ExpectInspectionRejected(report.ToJsonString(), request);
        });
    }

    private static void ExpectInspectionRejected(string json, InspectionSettings settings)
    {
        try { InspectionReportVerifier.Verify(json, settings); }
        catch (InvalidDataException) { return; }
        throw new InvalidOperationException("Invalid inspection report accepted.");
    }

    private static async Task InspectionServiceTests()
    {
        foreach (var mode in new[] { "inspect-old-version", "inspect-no-report", "inspect-invalid-report", "inspect-oversize", "inspect-zero-exit-errors", "inspect-nonzero-errors" })
            await TestAsync("inspection_service_rejects_" + mode, async () =>
            {
                var directory = CreateFakeEngine(mode);
                var request = InspectionRequest(mode);
                var result = await new EngineService(directory).InspectAsync(request);
                Assert(!result.Success && result.Report is null, "Invalid inspection succeeded.");
                if (mode == "inspect-old-version") Assert(!File.Exists(Path.Combine(directory, "inspect-invoked.txt")), "Inspection ran before version check.");
                if (mode == "inspect-oversize") Assert(result.Message.Contains("64 MiB"), "Oversized report bypassed bounded loader.");
                if (mode.EndsWith("errors")) Assert(result.Message.Contains("动画"), "Reader error diagnostic was lost.");
            });
        await TestAsync("inspection_works_without_writer_and_retains_result_after_callback_failure", async () =>
        {
            var directory = CreateFakeEngine("inspect-good");
            Assert(!Directory.Exists(Path.Combine(directory, "native-filegdb")), "Test has a writer.");
            var progress = new InlineProgress<EngineEvent>(e => { if (e.Message.Contains("正在核对结果报告")) throw new InvalidOperationException("Host message sink failed."); });
            var result = await new EngineService(directory).InspectAsync(InspectionRequest("callback"), progress);
            Assert(result.Success && result.Report is not null && result.Message.Contains("部分运行消息无法显示"), "Verified inspection lost after callback failure.");
            Assert(!File.Exists(Path.Combine(directory, "probe-invoked.txt")), "Inspection probed a writer.");
        });
        await TestAsync("inspection_refuses_report_created_during_version_callback", async () =>
        {
            var directory = CreateFakeEngine("inspect-destination-race");
            var request = InspectionRequest("destination-race");
            var progress = new InlineProgress<EngineEvent>(e =>
            {
                if (!e.Message.Contains("正在检查转换引擎版本")) return;
                using var file = new FileStream(request.ReportPath, FileMode.CreateNew, FileAccess.Write);
                file.Write([1, 2, 3]);
            });
            var result = await new EngineService(directory).InspectAsync(request, progress);
            Assert(!result.Success && File.ReadAllBytes(request.ReportPath).SequenceEqual(new byte[] { 1, 2, 3 }), "Competing report overwritten.");
            Assert(!File.Exists(Path.Combine(directory, "inspect-invoked.txt")), "Inspection launched after report destination was occupied.");
        });
        await TestAsync("inspection_running_guard_covers_conversions_and_probes", async () =>
        {
            var service = new EngineService(CreateFakeEngine("inspect-busy"));
            Task<ConversionResult>? conversion = null; Task<ProbeResult>? probe = null; Task<InspectionResult>? other = null;
            var result = await service.InspectAsync(InspectionRequest("busy"), new InlineProgress<EngineEvent>(e =>
            {
                if (conversion is not null) return;
                conversion = service.ConvertAsync(Settings); probe = service.ProbeBackendAsync("native-filegdb"); other = service.InspectAsync(InspectionRequest("busy-other"));
            }));
            Assert(result.Success && conversion is not null && probe is not null && other is not null, "Running guard test was not reached.");
            Assert(!(await conversion!).Success && !(await probe!).Success && !(await other!).Success, "Parallel operation passed running guard.");
        });
        foreach (var extension in new[] { "fbx", "max" })
        foreach (var phase in new[] { "before", "after" })
            await TestAsync("inspection_snapshots_paths_and_lists_" + extension + "_" + phase, async () =>
            {
                var suffix = extension + phase;
                var directory = CreateFakeEngine("inspect-relative-" + suffix);
                var initial = Path.Combine(Work, "inspect-source-" + suffix); var changed = Path.Combine(Work, "inspect-changed-" + suffix);
                foreach (var path in new[] { initial, changed })
                {
                    Directory.CreateDirectory(path); Directory.CreateDirectory(Path.Combine(path, "textures"));
                    File.WriteAllText(Path.Combine(path, "model." + extension), "placeholder"); File.WriteAllText(Path.Combine(path, "runtime.exe"), "placeholder");
                }
                var textures = new List<string> { "textures" };
                var request = InspectionRequest(suffix) with { InputPath = " model." + extension + " ", ReportPath = " inspect.json ", TextureDirectories = textures, MaxBatchPath = " runtime.exe ", MaxFrame = "-12" };
                var previous = Environment.CurrentDirectory;
                try
                {
                    Environment.CurrentDirectory = initial; var switched = false;
                    var result = await new EngineService(directory).InspectAsync(request, new InlineProgress<EngineEvent>(e =>
                    {
                        if (switched || e.Kind != EngineEventKind.Stage || !(phase == "before" ? e.Message.Contains("正在检查转换引擎") : e.Message.Contains("正在核对结果报告"))) return;
                        switched = true; Environment.CurrentDirectory = changed; textures[0] = "wrong-textures";
                    }));
                    Assert(switched && result.Success && result.ReportPath == Path.Combine(initial, "inspect.json"), "Inspection was redirected by callback: " + result.Message);
                    var args = JsonSerializer.Deserialize<string[]>(File.ReadAllText(Path.Combine(directory, "inspect-invoked.txt")))!;
                    Assert(args[1] == Path.Combine(initial, "model." + extension) && After(args, "--texture-dir") == Path.Combine(initial, "textures"), "Reader snapshot changed.");
                    Assert(!File.Exists(Path.Combine(changed, "inspect.json")), "Report written in changed directory.");
                }
                finally { Environment.CurrentDirectory = previous; }
            });
        await TestAsync("inspection_drains_bounded_dual_stream_logs", async () =>
        {
            var directory = CreateFakeEngine("inspect-log-flood");
            var progress = new CollectedProgress();
            var result = await new EngineService(directory).InspectAsync(InspectionRequest("flood"), progress).WaitAsync(TimeSpan.FromSeconds(30));
            Assert(!result.Success && result.ExitCode == 6 && result.Message.Contains("END-ERROR") && result.Message.Contains("END-OUTPUT"), "Failed process logs lost.");
            Assert(result.Message.Contains("已截断") && result.Message.Length < 4500 && progress.Events.Count < 40, "Inspection log flood was not bounded or exposed.");
        });
    }

    private static int RunFakeInspector(string[] args, string directory, string mode)
    {
        File.WriteAllText(Path.Combine(directory, "inspect-invoked.txt"), JsonSerializer.Serialize(args));
        if (args.Any(a => a is "--output" or "--origin" or "--wkid" or "--backend")) return 2;
        if (mode == "inspect-no-report") return 0;
        if (mode == "inspect-log-flood")
        {
            Console.Write(new string('o', 2 * 1024 * 1024)); Console.Error.Write(new string('e', 2 * 1024 * 1024));
            Console.Write("END-OUTPUT"); Console.Error.Write("END-ERROR"); return 6;
        }
        var request = new InspectionSettings { InputPath = args[1], ReportPath = After(args, "--report"),
            Profile = After(args, "--profile"), MissingTexturePolicy = After(args, "--missing-textures"),
            MaxBatchPath = args.Contains("--max-batch") ? After(args, "--max-batch") : "",
            MaxFrame = args.Contains("--max-frame") ? After(args, "--max-frame") : "" };
        var report = GoodInspection(request);
        if (mode == "inspect-invalid-report") report["geometry_bounds"]!["min"]![0] = 100;
        if (mode.EndsWith("errors"))
        {
            report["status"] = "rejected";
            report["diagnostics"]!.AsArray().Add(new JsonObject { ["severity"] = "error", ["code"] = "UNSUPPORTED_ANIMATION", ["message"] = "Animation data is present." });
        }
        File.WriteAllText(request.ReportPath, report.ToJsonString());
        if (mode == "inspect-oversize") { using var stream = new FileStream(request.ReportPath, FileMode.Open, FileAccess.Write); stream.SetLength(64 * 1024 * 1024 + 1); }
        return mode == "inspect-nonzero-errors" ? 3 : 0;
    }

    private static async Task InspectionIntegrationTests(string engineDirectory, string fixtureDirectory)
    {
        // This standalone copy has no native writer or SDK runtime directory.
        var directory = Path.Combine(Work, "真实检查 无写入端 & spaces"); Directory.CreateDirectory(directory);
        File.Copy(Path.Combine(engineDirectory, "geomodelbridge.exe"), Path.Combine(directory, "geomodelbridge.exe"));
        var service = new EngineService(directory);
        foreach (var extension in new[] { "fbx", "obj", "glb", "gltf", "wrl", "dae" })
            await TestAsync("real_model_inspection_without_writer_" + extension, async () =>
            {
                var request = InspectionRequest("real-" + extension) with { InputPath = Path.Combine(fixtureDirectory, "textured_quad." + extension), Profile = "strict" };
                var result = await service.InspectAsync(request);
                Assert(result.Success && result.Report is not null, result.Message);
                Assert(result.Report!.Counts.Triangles == 2 && result.Report.Counts.Textures > 0, "Real model counts lost.");
                Assert(!Directory.EnumerateDirectories(Work, "*.gdb", SearchOption.TopDirectoryOnly).Contains(Path.ChangeExtension(request.ReportPath, ".gdb")), "Inspection generated a GDB.");
                var before = SHA256.HashData(File.ReadAllBytes(request.ReportPath));
                var repeated = await service.InspectAsync(request);
                Assert(!repeated.Success && before.SequenceEqual(SHA256.HashData(File.ReadAllBytes(request.ReportPath))), "Repeat inspection overwrote report.");
            });
        await TestAsync("real_inspection_missing_texture_policies", async () =>
        {
            var request = InspectionRequest("real-missing") with { InputPath = Path.Combine(fixtureDirectory, "missing_texture.fbx") };
            var fallback = await service.InspectAsync(request);
            Assert(fallback.Success && fallback.Report?.Counts.Textures == 0 && fallback.Report.WarningCount > 0, "Missing-texture fallback failed: " + fallback.Message);
            Assert(fallback.Report!.Diagnostics.Any(d => d.Contains("MISSING_TEXTURE_FALLBACK")), "Missing image warning absent.");
            var strictMissing = await service.InspectAsync(request with { ReportPath = Path.Combine(Work, "real-missing-error.json"), MissingTexturePolicy = "error" });
            Assert(!strictMissing.Success && strictMissing.ExitCode == 3, "Explicit missing-image rejection was ignored.");
        });
    }
}
