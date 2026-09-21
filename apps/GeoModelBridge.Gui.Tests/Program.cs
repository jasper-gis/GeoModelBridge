using System.Security.Cryptography;
using System.Collections.Concurrent;
using System.Text.Json;
using System.Text.Json.Nodes;
using GeoModelBridge.Gui.Core;

internal static class Program
{
    private const string Version = "0.1.10";
    private static readonly List<TestCase> Results = [];
    private static string Work = "";
    private static string Input = "";
    private static ConversionSettings Settings = new();

    private sealed record TestCase(string Name, bool Passed, string? Detail);
    private sealed class CollectedProgress : IProgress<EngineEvent>
    {
        public ConcurrentQueue<EngineEvent> Events { get; } = new();
        public void Report(EngineEvent value) => Events.Enqueue(value);
    }

    public static async Task<int> Main(string[] args)
    {
        if (File.Exists(Path.Combine(AppContext.BaseDirectory, "fake-engine-mode.txt")))
            return RunFakeEngine(args);
        try
        {
            var options = ParseOptions(args);
            Work = Path.GetFullPath(options.GetValueOrDefault("--work")
                ?? Path.Combine(Path.GetTempPath(), "gmb-gui-tests-" + Guid.NewGuid().ToString("N")));
            if (Directory.Exists(Work) || File.Exists(Work))
                throw new InvalidOperationException("Test work directory must be new: " + Work);
            Directory.CreateDirectory(Work);
            Input = Path.Combine(Work, "测试 & model with spaces.fbx");
            File.WriteAllText(Input, "; validation-only FBX placeholder");
            Settings = new ConversionSettings
            {
                InputPath = Input,
                OutputPath = Path.Combine(Work, "结果 & native model.gdb"),
                Wkid = "32650", OriginX = "500000", OriginY = "3000000", OriginZ = "100",
                Backend = "native-filegdb", FeatureClass = "Models"
            };
            ValidationTests();
            ArgumentTests();
            ReportTests();
            SummaryTests();
            await MissingEngineTests();
            await FakeEngineTests();
            var integration = options.ContainsKey("--engine-dir");
            if (integration)
            {
                if (!options.TryGetValue("--fixtures", out var fixtures))
                    throw new ArgumentException("--fixtures is required with --engine-dir.");
                await IntegrationTests(Path.GetFullPath(options["--engine-dir"]), Path.GetFullPath(fixtures));
            }
            else
            {
                Console.WriteLine("Real native conversion skipped; pass --engine-dir and --fixtures to include it.");
            }
            var failed = Results.Count(t => !t.Passed);
            var summaryPath = Path.Combine(Work, "test-results.json");
            File.WriteAllText(summaryPath, JsonSerializer.Serialize(new
            {
                version = Version,
                status = failed == 0 ? "passed" : "failed",
                test_count = Results.Count,
                failed_count = failed,
                real_native_conversion_requested = integration,
                graphical_acceptance_performed = false,
                tests = Results
            }, new JsonSerializerOptions { WriteIndented = true }));
            Console.WriteLine($"{Results.Count - failed}/{Results.Count} passed. Evidence: {summaryPath}");
            return failed == 0 ? 0 : 1;
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine(exception);
            return 2;
        }
    }

    private static Dictionary<string, string> ParseOptions(string[] args)
    {
        var result = new Dictionary<string, string>(StringComparer.Ordinal);
        for (var i = 0; i < args.Length; i += 2)
        {
            if (i + 1 >= args.Length || args[i] is not ("--work" or "--engine-dir" or "--fixtures"))
                throw new ArgumentException("Usage: --work NEW_DIRECTORY [--engine-dir DIST_BIN --fixtures FIXTURES]");
            if (!result.TryAdd(args[i], args[i + 1]))
                throw new ArgumentException("Repeated option: " + args[i]);
        }
        return result;
    }

    private static void Test(string name, Action action)
    {
        try { action(); Results.Add(new(name, true, null)); Console.WriteLine("PASS " + name); }
        catch (Exception ex) { Results.Add(new(name, false, ex.Message)); Console.WriteLine("FAIL " + name + ": " + ex.Message); }
    }

    private static async Task TestAsync(string name, Func<Task> action)
    {
        try { await action(); Results.Add(new(name, true, null)); Console.WriteLine("PASS " + name); }
        catch (Exception ex) { Results.Add(new(name, false, ex.Message)); Console.WriteLine("FAIL " + name + ": " + ex.Message); }
    }

    private static void Assert(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }

    private static void Valid(ConversionSettings settings)
    {
        var errors = ConversionValidator.Validate(settings);
        Assert(errors.Count == 0, "Unexpected errors: " + string.Join(" | ", errors));
    }

    private static void Invalid(ConversionSettings settings)
    {
        var errors = ConversionValidator.Validate(settings);
        Assert(errors.Count > 0 && errors.All(error => !string.IsNullOrWhiteSpace(error)), "Invalid request was accepted or errors were empty.");
    }

    private static void ValidationTests()
    {
        Test("valid_chinese_spaces_ampersand_paths", () => Valid(Settings));
        Test("finite_signed_decimal_origin", () => Valid(Settings with { OriginX = "-500.25", OriginY = "0", OriginZ = "1.2e2" }));
        foreach (var wkid in new[] { "", " ", "0", "-1", "EPSG:32650", "32650.5", "2147483648" })
            Test("reject_wkid_" + JsonSerializer.Serialize(wkid), () => Invalid(Settings with { Wkid = wkid }));
        foreach (var value in new[] { "", "NaN", "Infinity", "-Infinity", "1e999", "abc", "1,25" })
        {
            Test("reject_origin_x_" + JsonSerializer.Serialize(value), () => Invalid(Settings with { OriginX = value }));
            Test("reject_origin_y_" + JsonSerializer.Serialize(value), () => Invalid(Settings with { OriginY = value }));
            Test("reject_origin_z_" + JsonSerializer.Serialize(value), () => Invalid(Settings with { OriginZ = value }));
        }
        Test("reject_missing_input", () => Invalid(Settings with { InputPath = Path.Combine(Work, "missing.fbx") }));
        Test("reject_empty_input", () => Invalid(Settings with { InputPath = "" }));
        var objPath = Path.Combine(Work, "wrong.obj");
        File.WriteAllText(objPath, "test");
        Test("reject_non_fbx_input", () => Invalid(Settings with { InputPath = objPath }));
        Test("reject_empty_output", () => Invalid(Settings with { OutputPath = "" }));
        Test("reject_non_gdb_output", () => Invalid(Settings with { OutputPath = Path.Combine(Work, "wrong.sqlite") }));
        var existingGdb = Path.Combine(Work, "existing.gdb");
        Directory.CreateDirectory(existingGdb);
        File.WriteAllText(Path.Combine(existingGdb, "keep.txt"), "User data must remain untouched.");
        Test("reject_existing_gdb_directory", () => Invalid(Settings with { OutputPath = existingGdb }));
        var existingFile = Path.Combine(Work, "file.gdb");
        File.WriteAllText(existingFile, "User file must remain untouched.");
        Test("reject_existing_gdb_file", () => Invalid(Settings with { OutputPath = existingFile }));
        var existingReport = Path.Combine(Work, "existing-report.json");
        File.WriteAllText(existingReport, "User report must remain untouched.");
        Test("reject_existing_report", () => Invalid(Settings with { ReportPath = existingReport }));
        Test("reject_unknown_backend", () => Invalid(Settings with { Backend = "shell" }));
        Test("reject_removed_pro_backend", () => Invalid(Settings with { Backend = "arcgis-pro" }));
        Test("default_profile_is_gis_static", () => Assert(new ConversionSettings().Profile == "gis-static", "GUI default policy is not static GIS."));
        Test("default_missing_texture_policy_is_material_color", () => Assert(new ConversionSettings().MissingTexturePolicy == "material-color", "GUI must allow missing-file fallback by default."));
        foreach (var policy in new[] { "material-color", "error" })
            Test("accept_missing_texture_policy_" + policy, () => Valid(Settings with { MissingTexturePolicy = policy }));
        Test("reject_unknown_missing_texture_policy", () => Invalid(Settings with { MissingTexturePolicy = "ignore-all" }));
        foreach (var profile in new[] { "strict", "gis-static" })
            Test("accept_profile_" + profile, () => Valid(Settings with { Profile = profile }));
        foreach (var profile in new[] { "", "Strict", "auto", "gis-static --ignore-errors" })
            Test("reject_profile_" + JsonSerializer.Serialize(profile), () => Invalid(Settings with { Profile = profile }));
        foreach (var name in new[] { "", "Bad name", "bad;table", "../outside" })
            Test("reject_feature_class_" + JsonSerializer.Serialize(name), () => Invalid(Settings with { FeatureClass = name }));
        Test("reject_missing_texture_directory", () => Invalid(Settings with { TextureDirectories = [Path.Combine(Work, "missing-textures")] }));
        Test("validation_does_not_touch_existing_files", () =>
        {
            Assert(File.ReadAllText(Path.Combine(existingGdb, "keep.txt")) == "User data must remain untouched.", "Existing GDB marker changed.");
            Assert(File.ReadAllText(existingReport) == "User report must remain untouched.", "Existing report changed.");
        });
    }

    private static void ArgumentTests()
    {
        foreach (var policy in new[] { "material-color", "error" })
            Test("arguments_preserve_missing_texture_policy_" + policy, () =>
            {
                var args = ConversionCommand.BuildArguments(Settings with { MissingTexturePolicy = policy }).ToArray();
                Assert(After(args, "--missing-textures") == policy && args.Count(a => a == "--missing-textures") == 1, "Missing texture policy was omitted, duplicated, or changed.");
            });
        var textureDirectory = Path.Combine(Work, "贴图 & texture directory");
        Directory.CreateDirectory(textureDirectory);
        var settings = Settings with { TextureDirectories = [textureDirectory], ReportPath = Path.Combine(Work, "报告 & conversion.json") };
        Test("arguments_preserve_paths_as_single_tokens", () =>
        {
            var args = ConversionCommand.BuildArguments(settings).ToArray();
            Assert(args.Length > 1 && args[0] == "convert" && args[1] == Input, "Input path was changed or split.");
            Assert(After(args, "--output") == settings.OutputPath, "Output path was changed or split.");
            Assert(After(args, "--report") == settings.ReportPath, "Report path was changed or split.");
            Assert(After(args, "--texture-dir") == textureDirectory, "Texture path was changed or split.");
            Assert(After(args, "--wkid") == "32650", "WKID differs.");
            var originIndex = Array.IndexOf(args, "--origin");
            Assert(originIndex >= 0 && args.Skip(originIndex + 1).Take(3).SequenceEqual(new[] { "500000", "3000000", "100" }), "Origin is not three distinct arguments.");
        });
        Test("process_uses_argument_list_without_shell", () =>
        {
            var engine = Path.Combine(Work, "引擎 & installed", "geomodelbridge.exe");
            var process = ConversionCommand.CreateStartInfo(engine, settings);
            Assert(process.FileName == engine, "Process does not directly target the engine.");
            Assert(!process.UseShellExecute, "Shell execution must be disabled.");
            Assert(process.Arguments.Length == 0, "Unsafe assembled argument string was used.");
            Assert(process.ArgumentList.SequenceEqual(ConversionCommand.BuildArguments(settings)), "ArgumentList did not preserve individual arguments.");
            Assert(process.RedirectStandardOutput && process.RedirectStandardError, "Both process streams must be captured.");
        });
        Test("default_report_is_absolute_next_to_output", () =>
        {
            var report = ConversionCommand.GetReportPath(Settings);
            Assert(Path.IsPathFullyQualified(report) && report == Settings.OutputPath + ".report.json", "Unexpected default report path: " + report);
        });
        foreach (var profile in new[] { "strict", "gis-static" })
            Test("arguments_preserve_explicit_profile_" + profile, () =>
            {
                var args = ConversionCommand.BuildArguments(Settings with { Profile = profile }).ToArray();
                Assert(After(args, "--profile") == profile && args.Count(a => a == "--profile") == 1, "Profile was omitted, duplicated, or altered.");
            });
    }

    private static string After(string[] args, string option)
    {
        var index = Array.IndexOf(args, option);
        Assert(index >= 0 && index + 1 < args.Length, "Missing option: " + option);
        return args[index + 1];
    }

    private static JsonObject GoodReport(ConversionSettings? settings = null)
    {
        settings ??= Settings;
        return new JsonObject
        {
            ["version"] = Version,
            ["status"] = "written_and_readback_verified",
            ["backend"] = "native-filegdb",
            ["conversion_profile"] = settings.Profile,
            ["missing_texture_policy"] = settings.MissingTexturePolicy,
            ["output"] = Path.GetFullPath(settings.OutputPath),
            ["feature_class"] = settings.FeatureClass,
            ["coordinate_system"] = new JsonObject { ["wkid"] = int.Parse(settings.Wkid), ["projected"] = true, ["unit"] = "meter" },
            ["coordinates"] = new JsonObject
            {
                ["wkid"] = int.Parse(settings.Wkid), ["unit"] = "meter", ["up_axis"] = "Z", ["space"] = "referenced",
                ["origin_explicit"] = true,
                ["origin"] = new JsonArray(double.Parse(settings.OriginX, System.Globalization.CultureInfo.InvariantCulture),
                    double.Parse(settings.OriginY, System.Globalization.CultureInfo.InvariantCulture),
                    double.Parse(settings.OriginZ, System.Globalization.CultureInfo.InvariantCulture))
            },
            ["reader_diagnostics"] = new JsonArray(),
            ["verification"] = new JsonObject
            {
                ["level"] = "closed_reopened_file_geodatabase",
                ["geometry_material_uv_texture_readback"] = true,
                ["feature_count"] = 1
            }
        };
    }

    private static void RejectedReport(string name, Action<JsonObject> mutate)
    {
        Test(name, () =>
        {
            var report = GoodReport();
            mutate(report);
            try { ReportVerifier.Verify(report.ToJsonString(), Settings); }
            catch (InvalidDataException) { return; }
            throw new InvalidOperationException("False or mismatching success report was accepted.");
        });
    }

    private static void ReportTests()
    {
        Test("accept_matching_reopened_gdb_report", () => Assert(ReportVerifier.Verify(GoodReport().ToJsonString(), Settings) is not null, "No parsed report."));
        Test("reject_pro_settings_even_with_native_report", () =>
        {
            try { ReportVerifier.Verify(GoodReport().ToJsonString(), Settings with { Backend = "arcgis-pro" }); }
            catch (InvalidDataException) { return; }
            throw new InvalidOperationException("Removed backend was accepted.");
        });
        RejectedReport("reject_old_report_version", report => report["version"] = "0.1.1");
        RejectedReport("reject_missing_report_version", report => report.Remove("version"));
        RejectedReport("reject_missing_conversion_profile", report => report.Remove("conversion_profile"));
        RejectedReport("reject_missing_texture_policy", report => report.Remove("missing_texture_policy"));
        RejectedReport("reject_mismatched_texture_policy", report => report["missing_texture_policy"] = "error");
        RejectedReport("reject_mismatched_conversion_profile", report => report["conversion_profile"] = "strict");
        RejectedReport("reject_failed_report_status", report => report["status"] = "failed");
        RejectedReport("reject_report_wrong_backend", report => report["backend"] = "arcgis-pro-corehost");
        RejectedReport("reject_report_wrong_output", report => report["output"] = Path.Combine(Work, "unrelated.gdb"));
        RejectedReport("reject_report_relative_output", report => report["output"] = "relative.gdb");
        RejectedReport("reject_report_wrong_feature_class", report => report["feature_class"] = "OtherModels");
        RejectedReport("reject_report_missing_verification", report => report.Remove("verification"));
        RejectedReport("reject_report_unverified_geometry", report => report["verification"]!["geometry_material_uv_texture_readback"] = false);
        RejectedReport("reject_report_wrong_verification_level", report => report["verification"]!["level"] = "memory_only");
        RejectedReport("reject_report_zero_features", report => report["verification"]!["feature_count"] = 0);
        RejectedReport("reject_report_negative_features", report => report["verification"]!["feature_count"] = -1);
        RejectedReport("reject_report_fractional_features", report => report["verification"]!["feature_count"] = 1.5);
        RejectedReport("reject_report_wrong_wkid", report => report["coordinate_system"]!["wkid"] = 3857);
        RejectedReport("reject_report_geographic_crs", report => report["coordinate_system"]!["projected"] = false);
        RejectedReport("reject_report_missing_crs", report => report.Remove("coordinate_system"));
        RejectedReport("reject_report_nonmetric_crs", report => report["coordinate_system"]!["unit"] = "feet");
        RejectedReport("reject_report_missing_placement", report => report.Remove("coordinates"));
        RejectedReport("reject_report_wrong_origin", report => report["coordinates"]!["origin"]![2] = 99);
        RejectedReport("reject_report_short_origin", report => report["coordinates"]!["origin"] = new JsonArray(1, 2));
        RejectedReport("reject_report_origin_not_explicit", report => report["coordinates"]!["origin_explicit"] = false);
        RejectedReport("reject_report_placement_wrong_wkid", report => report["coordinates"]!["wkid"] = 3857);
        RejectedReport("reject_report_placement_wrong_axis", report => report["coordinates"]!["up_axis"] = "Y");
        RejectedReport("reject_report_missing_diagnostics", report => report.Remove("reader_diagnostics"));
        RejectedReport("reject_success_report_with_reader_error", report => report["reader_diagnostics"] = new JsonArray
        {
            new JsonObject { ["severity"] = "error", ["code"] = "MATERIAL_UNSUPPORTED", ["message"] = "Unsupported material." }
        });
        Test("reject_malformed_json_report", () =>
        {
            try { ReportVerifier.Verify("{broken", Settings); }
            catch (InvalidDataException) { return; }
            throw new InvalidOperationException("Malformed JSON was not rejected with a report validation error.");
        });
    }

    private static void SummaryTests()
    {
        Test("normal_repair_summary_counts_corners", () =>
        {
            var report = GoodReport();
            report["reader_diagnostics"] = new JsonArray(
                new JsonObject { ["severity"]="warning", ["code"]="NORMALS_REPAIRED", ["message"]="GIS static profile rebuilt 6 invalid corner normals across 2 triangles from transformed triangle edges." },
                new JsonObject { ["severity"]="warning", ["code"]="NORMALS_REPAIRED", ["message"]="GIS static profile rebuilt 3 invalid corner normals across 1 triangles from transformed triangle edges." });
            var summary = ReportSummaryFormatter.Parse(report.ToJsonString());
            Assert(summary.IsSuccess && summary.HasCompatibilityAdjustments && summary.SummaryText.Contains("共修复 9 个角点法线（2 条记录）"), "Normal repairs must count corners separately from diagnostic records.");
        });
        Test("normal_repair_invalid_quantity_is_not_invented", () =>
        {
            var report = GoodReport();
            report["reader_diagnostics"] = new JsonArray(new JsonObject { ["severity"]="warning", ["code"]="NORMALS_REPAIRED", ["message"]="GIS static profile rebuilt 999999999999999999999 invalid corner normals across 1 triangles." });
            var summary = ReportSummaryFormatter.Parse(report.ToJsonString());
            Assert(summary.Groups.Single().RepairedNormalCount is null && summary.WarningCount == 1, "Malformed quantity must retain the warning without a guessed count.");
        });
        Test("success_word_alone_is_not_a_verified_database_report", () =>
        {
            var summary = ReportSummaryFormatter.Parse("{\"status\":\"written_and_readback_verified\"}");
            Assert(!summary.IsSuccess && summary.ErrorCount == 0 && summary.VerificationIssues.Count > 0, "A bare success label was treated as evidence of database readback.");
            Assert(summary.SummaryText.Contains("不能据此确认转换成功") && summary.DetailedText.Contains("verification"), "Missing verification is not explained to the report reader.");
        });
        foreach (var field in new[] { "verification", "coordinate_system", "version", "output", "feature_class" })
            Test("summary_explains_missing_success_evidence_" + field, () =>
            {
                var report = GoodReport();
                report.Remove(field);
                var summary = ReportSummaryFormatter.Parse(report.ToJsonString());
                Assert(!summary.IsSuccess && summary.VerificationIssues.Any(issue => issue.Contains(field)), "Missing evidence was not called out: " + field);
            });
        foreach (var malformed in new JsonNode?[] { JsonValue.Create(false), JsonValue.Create("true"), null })
            Test("summary_requires_a_real_true_readback_flag_" + (malformed?.ToJsonString() ?? "null"), () =>
            {
                var report = GoodReport();
                report["verification"]!["geometry_material_uv_texture_readback"] = malformed?.DeepClone();
                var summary = ReportSummaryFormatter.Parse(report.ToJsonString());
                Assert(!summary.IsSuccess && summary.VerificationIssues.Any(issue => issue.Contains("geometry_material_uv_texture_readback")), "A false, string, or null readback flag was displayed as successful.");
            });
        Test("historical_complete_report_does_not_require_current_gui_version_or_profile", () =>
        {
            var report = GoodReport();
            report["version"] = "0.1.2";
            report.Remove("conversion_profile");
            var summary = ReportSummaryFormatter.Parse(report.ToJsonString());
            Assert(summary.IsSuccess && summary.VerificationIssues.Count == 0, "Presentation rejected an otherwise complete historical report; current conversion validation is handled separately.");
        });
        var repeated = new JsonArray();
        for (var i = 0; i < 144; ++i)
            repeated.Add(new JsonObject { ["severity"] = "error", ["code"] = "UNSUPPORTED_ANIMATION", ["message"] = "Animation data is present.", ["context"] = "node:model-" + i });
        for (var i = 0; i < 180; ++i)
            repeated.Add(new JsonObject { ["severity"] = "error", ["code"] = "UNRECOGNIZED_RENDER_PROPERTY", ["message"] = "Unrecognized rendering property requires explicit review/baking: Reflectivity", ["context"] = "material:材料-" + i });
        var json = new JsonObject { ["status"] = "rejected", ["version"] = Version, ["diagnostics"] = repeated }.ToJsonString();
        Test("hundreds_of_diagnostics_grouped_without_losing_total", () =>
        {
            var summary = ReportSummaryFormatter.Parse(json);
            Assert(!summary.IsSuccess && summary.ErrorCount == 324 && summary.WarningCount == 0, "Diagnostic count or failed state was lost.");
            Assert(summary.Groups.Count == 2 && summary.Groups.Sum(g => g.Count) == 324, "Repeated problems did not collapse into two cause groups.");
            Assert(summary.Groups.All(g => g.Examples.Count <= 4), "Example contexts are unbounded.");
            Assert(summary.SummaryText.Length < 1800 && summary.DetailedText.Length < 3500, "Summary still repeats hundreds of entries.");
            Assert(summary.SummaryText.Contains("动画") && summary.SummaryText.Contains("反射率"), "Known causes are not presented in Chinese.");
            Assert(ReportSummaryFormatter.FormatFailure(json) == summary.SummaryText, "Failure presentation differs from grouped summary.");
        });
        Test("unknown_error_is_visible_and_never_success", () =>
        {
            var report = GoodReport();
            report["reader_diagnostics"] = new JsonArray(new JsonObject { ["severity"] = "error", ["code"] = "FUTURE_UNSUPPORTED_SURFACE", ["message"] = "An unknown surface remains unsupported.", ["context"] = "mesh:example" });
            var summary = ReportSummaryFormatter.Parse(report.ToJsonString());
            Assert(!summary.IsSuccess && summary.ErrorCount == 1, "Unknown error was ignored.");
            Assert(summary.DetailedText.Contains("FUTURE_UNSUPPORTED_SURFACE"), "Unknown diagnostic code disappeared.");
        });
        Test("both_diagnostic_arrays_are_counted", () =>
        {
            var report = GoodReport();
            report["diagnostics"] = new JsonArray(new JsonObject { ["severity"] = "error", ["code"] = "DEGENERATE_TRIANGLE", ["message"] = "Triangle has zero area." });
            report["reader_diagnostics"] = new JsonArray(new JsonObject { ["severity"] = "warning", ["code"] = "MATERIAL_CHANNEL_OMITTED", ["message"] = "ambient omitted" });
            var summary = ReportSummaryFormatter.Parse(report.ToJsonString());
            Assert(summary.ErrorCount == 1 && summary.WarningCount == 1 && !summary.IsSuccess, "One diagnostic array was dropped.");
        });
        foreach (var code in new[] { "STATIC_POSE_USED", "MATERIAL_CHANNEL_OMITTED", "DEGENERATE_TRIANGLES_REMOVED", "JPEG_CONTAINER_NORMALIZED", "MISSING_TEXTURE_FALLBACK", "NORMALS_REPAIRED", "DEGENERATE_NORMALS_DISCARDED" })
            Test("compatibility_adjustment_is_explicit_" + code, () =>
            {
                var report = GoodReport();
                report["reader_diagnostics"] = new JsonArray(new JsonObject { ["severity"] = "warning", ["code"] = code, ["message"] = "Explicit supported compatibility handling.", ["context"] = "scene" });
                var summary = ReportSummaryFormatter.Parse(report.ToJsonString());
                Assert(summary.IsSuccess && summary.HasCompatibilityAdjustments && summary.WarningCount == 1, "Successful compatibility conversion lost its warning.");
                Assert(summary.SummaryText.Contains("兼容") && summary.SummaryText.Contains("外观"), "Appearance caveat is not visible.");
            });
        Test("unsupported_texture_diagnostics_have_chinese_causes_and_next_steps", () =>
        {
            var report = GoodReport();
            report["status"] = "rejected";
            report["reader_diagnostics"] = new JsonArray(
                new JsonObject { ["severity"] = "error", ["code"] = "UNSUPPORTED_TEXTURE_CHANNEL", ["message"] = "Only the diffuse/base-color texture channel is supported (FBX channel 6).", ["context"] = "material:反光玻璃" },
                new JsonObject { ["severity"] = "error", ["code"] = "UNSUPPORTED_TEXTURE_CONNECTION", ["message"] = "Unrepresented material texture connection: ReflectionColor", ["context"] = "material:反光玻璃" });
            var summary = ReportSummaryFormatter.Parse(report.ToJsonString());
            Assert(!summary.IsSuccess && summary.ErrorCount == 2 && summary.Groups.Count == 2, "Texture errors were dropped or mislabeled as successful.");
            Assert(summary.Groups.All(g => g.Title.Contains("反射") && g.Advice.Contains("漫反射")), "Reflection texture causes lack Chinese remediation.");
            Assert(!summary.SummaryText.Contains("Only the diffuse") && !summary.SummaryText.Contains("Unrepresented"), "Repeated raw English is still used as the readable cause.");
        });
        Test("jpeg_repairs_are_counted_as_texture_records_without_recompression_claims", () =>
        {
            var report = GoodReport();
            report["reader_diagnostics"] = new JsonArray(
                new JsonObject { ["severity"] = "warning", ["code"] = "JPEG_CONTAINER_NORMALIZED", ["message"] = "JPEG container normalized.", ["context"] = "texture:first.jpg" },
                new JsonObject { ["severity"] = "warning", ["code"] = "JPEG_CONTAINER_NORMALIZED", ["message"] = "JPEG container normalized.", ["context"] = "texture:second.jpg" });
            var summary = ReportSummaryFormatter.Parse(report.ToJsonString());
            Assert(summary.IsSuccess && summary.HasCompatibilityAdjustments && summary.WarningCount == 2 && summary.Groups.Single().Count == 2, "JPEG repair warnings were lost or conflated with face counts.");
            Assert(summary.SummaryText.Contains("2 条贴图记录") && summary.SummaryText.Contains("不重新压缩"), "JPEG repair count or no-recompression explanation is absent.");
            Assert(summary.DetailedText.Contains("18 字节") && summary.DetailedText.Contains("原压缩图像数据保持不变"), "Container-only repair is not explained precisely.");
        });
        foreach (var code in new[] { "JPEG_CONTAINER_REQUIRES_NORMALIZATION", "JPEG_CONTAINER_UNSUPPORTED" })
            Test("jpeg_failure_remains_a_chinese_blocking_error_" + code, () =>
            {
                var report = GoodReport();
                report["reader_diagnostics"] = new JsonArray(new JsonObject { ["severity"] = "error", ["code"] = code, ["message"] = "Unsupported JPEG container.", ["context"] = "texture:example.jpg" });
                var summary = ReportSummaryFormatter.Parse(report.ToJsonString());
                Assert(!summary.IsSuccess && !summary.HasCompatibilityAdjustments && summary.ErrorCount == 1, "A JPEG failure was silently treated as a repair.");
                Assert(summary.Groups.Single().Title.Contains("封装") && summary.Groups.Single().Advice.Contains("PNG"), "JPEG failure lacks a readable cause or actionable alternative.");
            });
        Test("jpeg_compatibility_warning_never_hides_an_unknown_error", () =>
        {
            var report = GoodReport();
            report["reader_diagnostics"] = new JsonArray(
                new JsonObject { ["severity"] = "warning", ["code"] = "JPEG_CONTAINER_NORMALIZED", ["message"] = "JPEG container normalized." },
                new JsonObject { ["severity"] = "error", ["code"] = "FUTURE_UNSUPPORTED_SURFACE", ["message"] = "Unknown surface." });
            var summary = ReportSummaryFormatter.Parse(report.ToJsonString());
            Assert(!summary.IsSuccess && summary.ErrorCount == 1 && summary.HasCompatibilityAdjustments && summary.WarningCount == 1, "Repair mode hid an unrelated blocking error.");
            Assert(summary.DetailedText.Contains("FUTURE_UNSUPPORTED_SURFACE"), "Unknown diagnostic disappeared after JPEG repair.");
        });
        Test("empty_animation_stack_is_not_rendering_loss", () =>
        {
            var report = GoodReport();
            report["reader_diagnostics"] = new JsonArray(new JsonObject { ["severity"] = "warning", ["code"] = "EMPTY_ANIMATION_IGNORED", ["message"] = "Empty animation container ignored." });
            var summary = ReportSummaryFormatter.Parse(report.ToJsonString());
            Assert(summary.IsSuccess && !summary.HasCompatibilityAdjustments, "An empty animation container was labeled as a fidelity change.");
        });
        Test("removed_triangle_totals_are_distinct_from_warning_records", () =>
        {
            var report = GoodReport();
            report["reader_diagnostics"] = new JsonArray(
                new JsonObject { ["severity"] = "warning", ["code"] = "DEGENERATE_TRIANGLES_REMOVED", ["message"] = "GIS static profile removed 6 strictly zero-area triangles; no area tolerance was used.", ["context"] = "mesh:first" },
                new JsonObject { ["severity"] = "warning", ["code"] = "DEGENERATE_TRIANGLES_REMOVED", ["message"] = "GIS static profile removed 2 strictly zero-area triangles; no area tolerance was used.", ["context"] = "mesh:second" });
            var summary = ReportSummaryFormatter.Parse(report.ToJsonString());
            var group = summary.Groups.Single(g => g.Code == "DEGENERATE_TRIANGLES_REMOVED");
            Assert(group.Count == 2 && group.AffectedTriangleCount == 8 && summary.WarningCount == 2, "Removed-face total was confused with warning count.");
            Assert(summary.SummaryText.Contains("8") && summary.SummaryText.Contains("面"), "Readable summary omits the total affected faces.");
        });
        foreach (var value in new[] { "six", "0006", "9223372036854775808" })
            Test("removed_triangle_total_never_guessed_" + value, () =>
            {
                var report = GoodReport();
                report["reader_diagnostics"] = new JsonArray(new JsonObject { ["severity"] = "warning", ["code"] = "DEGENERATE_TRIANGLES_REMOVED", ["message"] = $"GIS static profile removed {value} strictly zero-area triangles; no area tolerance was used." });
                var group = ReportSummaryFormatter.Parse(report.ToJsonString()).Groups.Single();
                Assert(group.AffectedTriangleCount is null, "Invalid or noncanonical face total was guessed.");
            });
        Test("removed_triangle_sum_overflow_does_not_crash_or_wrap", () =>
        {
            var report = GoodReport();
            report["reader_diagnostics"] = new JsonArray(
                new JsonObject { ["severity"] = "warning", ["code"] = "DEGENERATE_TRIANGLES_REMOVED", ["message"] = "GIS static profile removed 9223372036854775807 strictly zero-area triangles; no area tolerance was used." },
                new JsonObject { ["severity"] = "warning", ["code"] = "DEGENERATE_TRIANGLES_REMOVED", ["message"] = "GIS static profile removed 1 strictly zero-area triangles; no area tolerance was used." });
            var group = ReportSummaryFormatter.Parse(report.ToJsonString()).Groups.Single();
            Assert(group.Count == 2 && group.AffectedTriangleCount is null, "Overflowing aggregate was wrapped or accepted.");
        });
        Test("report_statistics_display_only_well_typed_nonnegative_counts", () =>
        {
            var report = GoodReport();
            report["counts"] = new JsonObject { ["meshes"] = 2, ["materials"] = 3, ["textures"] = 4, ["triangles"] = 12 };
            report["textures"] = new JsonArray(new JsonObject(), new JsonObject());
            report["material_quantization"] = new JsonArray(new JsonObject());
            var good = ReportSummaryFormatter.Parse(report.ToJsonString()).DetailedText;
            Assert(good.Contains("网格数：2") && good.Contains("三角形数：12") && good.Contains("已回读要素数：1") && good.Contains("贴图记录数：2"), "Valid report statistics were omitted.");
            report["counts"] = new JsonObject { ["meshes"] = "2", ["materials"] = -1, ["textures"] = new JsonArray(), ["triangles"] = 1.5 };
            report["verification"] = new JsonObject { ["feature_count"] = "1" };
            report["textures"] = new JsonObject();
            report["material_quantization"] = "1";
            var invalid = ReportSummaryFormatter.Parse(report.ToJsonString()).DetailedText;
            Assert(new[] { "网格数：", "三角形数：", "材质数：", "贴图数：", "已回读要素数：", "贴图记录数：", "材质量化记录数：" }.All(label => !invalid.Contains(label)), "Malformed statistics were presented as factual counts.");
            report["counts"] = 42;
            report["verification"] = new JsonArray();
            ReportSummaryFormatter.Parse(report.ToJsonString());
        });
        foreach (var status in new[] { "failed", "rejected", "prepared", "inspected", "" })
            Test("summary_never_claims_gdb_success_for_" + status, () =>
            {
                var report = GoodReport();
                report["status"] = status;
                Assert(!ReportSummaryFormatter.Parse(report.ToJsonString()).IsSuccess, "A non-written status was presented as GDB success.");
            });
        foreach (var broken in new[] { "{broken", "[]", "{\"diagnostics\":{}}", "{\"diagnostics\":[42]}" })
            Test("summary_rejects_invalid_shape_" + broken, () =>
            {
                try { ReportSummaryFormatter.Parse(broken); }
                catch (InvalidDataException) { return; }
                throw new InvalidOperationException("Malformed report was accepted or produced an unhandled parser error.");
            });
    }

    private static async Task MissingEngineTests()
    {
        var missingDirectory = Path.Combine(Work, "missing-installation");
        var engine = new EngineService(missingDirectory);
        Test("missing_engine_detected", () => Assert(!engine.IsEnginePresent, "Missing engine reported present."));
        await TestAsync("removed_backend_probe_rejected_before_engine_discovery", async () =>
        {
            var result = await engine.ProbeBackendAsync("arcgis-pro");
            Assert(!result.Success && result.Message.Contains("仅支持原生"), "Removed backend reached discovery.");
        });
        await TestAsync("missing_engine_probe_is_actionable", async () =>
        {
            var result = await engine.ProbeBackendAsync("native-filegdb");
            Assert(!result.Success && !string.IsNullOrWhiteSpace(result.Message), "Missing engine probe gave no actionable failure.");
        });
        await TestAsync("missing_engine_conversion_is_failure", async () =>
        {
            var result = await engine.ConvertAsync(Settings);
            Assert(!result.Success && !string.IsNullOrWhiteSpace(result.Message), "Missing engine conversion was accepted.");
            Assert(!Directory.Exists(Settings.OutputPath), "Missing engine conversion created output.");
        });
    }

    private static string CreateFakeEngine(string mode, bool includeWriter = false)
    {
        var directory = Path.Combine(Work, "fake-engine-" + mode);
        Directory.CreateDirectory(directory);
        foreach (var file in Directory.EnumerateFiles(AppContext.BaseDirectory))
            File.Copy(file, Path.Combine(directory, Path.GetFileName(file)));
        var appHost = Path.Combine(AppContext.BaseDirectory, "GeoModelBridge.Gui.Tests.exe");
        Assert(File.Exists(appHost), "Test apphost is required for child-process contract checks.");
        File.Copy(appHost, Path.Combine(directory, "geomodelbridge.exe"));
        File.WriteAllText(Path.Combine(directory, "fake-engine-mode.txt"), mode);
        if (includeWriter)
        {
            var writerDirectory = Path.Combine(directory, "native-filegdb");
            Directory.CreateDirectory(writerDirectory);
            foreach (var file in Directory.EnumerateFiles(directory))
                File.Copy(file, Path.Combine(writerDirectory, Path.GetFileName(file)));
            File.Copy(appHost, Path.Combine(writerDirectory, "GeoModelBridge.NativeWriter.exe"));
        }
        return directory;
    }

    private static async Task FakeEngineTests()
    {
        await TestAsync("reject_old_engine_version_before_conversion", async () =>
        {
            var directory = CreateFakeEngine("old-version");
            var result = await new EngineService(directory).ConvertAsync(Settings);
            Assert(!result.Success, "Old engine was accepted.");
            Assert(!File.Exists(Path.Combine(directory, "convert-invoked.txt")), "Conversion started before engine version was rejected.");
        });
        await TestAsync("zero_exit_without_report_is_not_success", async () =>
        {
            var directory = CreateFakeEngine("no-report");
            var result = await new EngineService(directory).ConvertAsync(Settings);
            Assert(!result.Success, "Zero exit without report was accepted.");
            Assert(File.Exists(Path.Combine(directory, "convert-invoked.txt")), "Fake conversion was not reached, so no-report behavior was not exercised.");
            Assert(Directory.Exists(Settings.OutputPath), "Fake output directory was not generated, so report existence behavior was not exercised.");
        });
        await TestAsync("valid_report_without_gdb_is_not_success", async () =>
        {
            var directory = CreateFakeEngine("no-gdb");
            var request = Settings with
            {
                OutputPath = Path.Combine(Work, "fake-output-missing.gdb"),
                ReportPath = Path.Combine(Work, "fake-report-without-gdb.json")
            };
            var result = await new EngineService(directory).ConvertAsync(request);
            Assert(!result.Success, "Valid report without a real GDB was accepted.");
            Assert(File.Exists(request.ReportPath), "Fake report was not generated, so output existence behavior was not exercised.");
            ReportVerifier.Verify(File.ReadAllText(request.ReportPath), request);
        });
        await TestAsync("mismatched_report_is_returned_as_actionable_failure", async () =>
        {
            var directory = CreateFakeEngine("bad-report");
            var request = Settings with
            {
                OutputPath = Path.Combine(Work, "fake-output-bad-report.gdb"),
                ReportPath = Path.Combine(Work, "fake-bad-report.json")
            };
            var result = await new EngineService(directory).ConvertAsync(request);
            Assert(!result.Success && !string.IsNullOrWhiteSpace(result.Message), "Mismatched report did not return a failure.");
            Assert(Directory.Exists(request.OutputPath) && File.Exists(request.ReportPath), "Fake output and report were not generated, so report validation behavior was not exercised.");
        });
        foreach (var mode in new[] { "failed-report-zero-exit", "failed-report-nonzero-exit" })
            await TestAsync(mode + "_is_readable_and_never_success", async () =>
            {
                var directory = CreateFakeEngine(mode);
                var request = Settings with { OutputPath = Path.Combine(Work, mode + ".gdb"), ReportPath = Path.Combine(Work, mode + ".json") };
                var result = await new EngineService(directory).ConvertAsync(request);
                Assert(!result.Success && result.Report is null, "Rejected report was returned as a verified GDB result.");
                Assert(File.Exists(request.ReportPath), "Fake failed report was not produced; test did not reach the failure path.");
                Assert(result.Message.Contains("动画") && result.Message.Length < 1800, "User sees repeated raw diagnostics instead of a bounded Chinese cause summary.");
            });
        await TestAsync("large_dual_stream_logs_are_drained_bounded_and_keep_both_failure_tails", async () =>
        {
            var directory = CreateFakeEngine("log-flood");
            var request = Settings with { OutputPath = Path.Combine(Work, "log-flood.gdb"), ReportPath = Path.Combine(Work, "log-flood.json") };
            var progress = new CollectedProgress();
            var result = await new EngineService(directory).ConvertAsync(request, progress).WaitAsync(TimeSpan.FromSeconds(30));
            var events = progress.Events.ToArray();
            Assert(!result.Success && result.ExitCode == 6, "Flooded failed process was not drained to its exit status.");
            Assert(events.Length <= 40, "Every process line still creates a UI callback: " + events.Length);
            Assert(events.Where(e => e.Kind is EngineEventKind.StandardOutput or EngineEventKind.StandardError).All(e => e.Message.Length <= 2100), "An enormous process line escaped the preview bound.");
            Assert(events.Count(e => e.Kind == EngineEventKind.LogSummary && e.Message.Contains("已折叠")) == 2, "Suppressed lines are not explicitly reported for both streams.");
            Assert(events.Any(e => e.Message.Contains("此行日志已截断")), "Long line truncation was silent.");
            Assert(result.Message.Contains("END-ERROR") && result.Message.Contains("END-OUTPUT") && result.Message.Length < 4300, "Stdout flooding displaced the actual stderr failure or produced an unbounded result.");
        });
        await TestAsync("new_engine_without_writer_has_actionable_probe_failure", async () =>
        {
            var directory = CreateFakeEngine("no-writer");
            var result = await new EngineService(directory).ProbeBackendAsync("native-filegdb");
            Assert(!result.Success && !string.IsNullOrWhiteSpace(result.Message), "Missing writer probe did not report failure.");
        });
        foreach (var mode in new[] { "probe-old-version", "probe-failed-status", "probe-wrong-backend", "probe-needs-pro", "probe-invalid-json" })
            await TestAsync("runtime_rejects_" + mode, async () =>
            {
                var directory = CreateFakeEngine(mode, includeWriter: true);
                var result = await new EngineService(directory).ProbeBackendAsync("native-filegdb");
                Assert(!result.Success && !string.IsNullOrWhiteSpace(result.Message), "Invalid runtime contract was accepted.");
                Assert(File.Exists(Path.Combine(directory, "native-filegdb", "probe-invoked.txt")), "Runtime probe was not reached.");
            });
    }

    private static int RunFakeEngine(string[] args)
    {
        var directory = AppContext.BaseDirectory;
        var mode = File.ReadAllText(Path.Combine(directory, "fake-engine-mode.txt")).Trim();
        if (args.SequenceEqual(new[] { "--version" }))
        {
            Console.WriteLine("GeoModelBridge V" + (mode == "old-version" ? "0.1.1" : Version));
            return 0;
        }
        if (args.SequenceEqual(new[] { "--probe" }))
        {
            File.WriteAllText(Path.Combine(directory, "probe-invoked.txt"), mode);
            Console.WriteLine(mode == "probe-invalid-json" ? "not JSON" : JsonSerializer.Serialize(new
            {
                version = mode == "probe-old-version" ? "0.1.1" : Version,
                status = mode == "probe-failed-status" ? "unavailable" : "available",
                backend = mode == "probe-wrong-backend" ? "arcgis-pro-corehost" : "native-filegdb",
                arcgis_pro_required = mode == "probe-needs-pro"
            }));
            return 0;
        }
        if (args.Length == 0 || args[0] != "convert") return 2;
        File.WriteAllText(Path.Combine(directory, "convert-invoked.txt"), JsonSerializer.Serialize(args));
        if (mode == "log-flood")
        {
            Console.WriteLine(new string('o', 3 * 1024 * 1024));
            Console.Error.WriteLine(new string('e', 3 * 1024 * 1024));
            for (var i = 0; i < 20000; ++i)
            {
                Console.WriteLine("程序输出中文行 " + i);
                Console.Error.WriteLine("错误输出中文行 " + i);
            }
            Console.Write("END-OUTPUT");
            Console.Error.Write("END-ERROR");
            return 6;
        }
        if (mode == "no-report") Directory.CreateDirectory(After(args, "--output"));
        if (mode is "no-gdb" or "bad-report" or "failed-report-zero-exit" or "failed-report-nonzero-exit")
        {
            var originIndex = Array.IndexOf(args, "--origin");
            var settings = new ConversionSettings
            {
                InputPath = args[1], OutputPath = After(args, "--output"),
                ReportPath = After(args, "--report"), Wkid = After(args, "--wkid"),
                OriginX = args[originIndex + 1], OriginY = args[originIndex + 2], OriginZ = args[originIndex + 3],
                FeatureClass = After(args, "--feature-class"), Backend = After(args, "--backend"), Profile = After(args, "--profile"), MissingTexturePolicy = After(args, "--missing-textures")
            };
            var report = GoodReport(settings);
            if (mode == "bad-report")
            {
                Directory.CreateDirectory(settings.OutputPath);
                report["version"] = "0.1.1";
            }
            if (mode.StartsWith("failed-report-", StringComparison.Ordinal))
            {
                report["status"] = "rejected";
                report["backend"] = "none";
                report.Remove("reader_diagnostics");
                var diagnostics = new JsonArray();
                for (var i = 0; i < 160; ++i)
                    diagnostics.Add(new JsonObject { ["severity"] = "error", ["code"] = "UNSUPPORTED_ANIMATION", ["message"] = "Animation data is present.", ["context"] = "node:example-" + i });
                report["diagnostics"] = diagnostics;
            }
            File.WriteAllText(settings.ReportPath, report.ToJsonString());
        }
        return mode == "failed-report-nonzero-exit" ? 3 : 0;
    }

    private static async Task IntegrationTests(string engineDirectory, string fixtureDirectory)
    {
        var service = new EngineService(engineDirectory);
        await TestAsync("native_backend_runtime_probe", async () =>
        {
            Assert(service.IsEnginePresent, "Built V0.1.10 engine is absent: " + service.EnginePath);
            var probe = await service.ProbeBackendAsync("native-filegdb");
            Assert(probe.Success, probe.Message);
        });
        var caseDirectory = Path.Combine(Work, "真实转换 中文 & spaces");
        Directory.CreateDirectory(caseDirectory);
        var fixtureInput = Path.Combine(caseDirectory, "贴图模型 & square.fbx");
        File.Copy(Path.Combine(fixtureDirectory, "textured_quad.fbx"), fixtureInput);
        File.Copy(Path.Combine(fixtureDirectory, "checker.png"), Path.Combine(caseDirectory, "checker.png"));
        var request = Settings with
        {
            InputPath = fixtureInput,
            OutputPath = Path.Combine(caseDirectory, "转换结果 & textured.gdb"),
            ReportPath = Path.Combine(caseDirectory, "核验报告 & textured.json"),
            TextureDirectories = [caseDirectory]
        };
        var events = new ConcurrentQueue<EngineEvent>();
        var progress = new InlineProgress<EngineEvent>(value => events.Enqueue(value));
        await TestAsync("real_native_textured_conversion_from_gui_service", async () =>
        {
            var result = await service.ConvertAsync(request, progress);
            Assert(result.Success, result.Message);
            Assert(result.ExitCode == 0 && result.Report is not null, "Successful conversion lacks exit/report evidence.");
            Assert(Directory.Exists(request.OutputPath) && Directory.EnumerateFiles(request.OutputPath).Any(), "No real FileGDB files exist.");
            Assert(File.Exists(request.ReportPath), "No report was written.");
            var json = File.ReadAllText(request.ReportPath);
            ReportVerifier.Verify(json, request);
            var report = JsonNode.Parse(json)!;
            Assert(report["backend"]!.GetValue<string>() == "native-filegdb", "Wrong writer backend.");
            var textures = report["textures"]!.AsArray();
            Assert(textures.Count > 0 && textures.All(texture => texture!["readback_bytes_equal"]!.GetValue<bool>()), "Texture bytes were not verified after GDB reopen.");
            Assert(events.Count > 0, "No GUI progress events were delivered.");
        });
        await TestAsync("repeat_conversion_refuses_to_overwrite_gdb_and_report", async () =>
        {
            Assert(File.Exists(request.ReportPath) && Directory.Exists(request.OutputPath), "The preceding conversion did not produce a valid case.");
            var beforeReport = SHA256.HashData(File.ReadAllBytes(request.ReportPath));
            var beforeGdb = HashDirectory(request.OutputPath);
            var result = await service.ConvertAsync(request);
            Assert(!result.Success, "Repeated conversion overwrote an existing output.");
            Assert(beforeReport.SequenceEqual(SHA256.HashData(File.ReadAllBytes(request.ReportPath))), "Existing report changed.");
            Assert(beforeGdb == HashDirectory(request.OutputPath), "Existing FileGDB files changed.");
        });
        await TestAsync("real_missing_texture_conversion_from_gui_service", async () =>
        {
            var input = Path.Combine(caseDirectory, "缺图模型.fbx");
            File.Copy(Path.Combine(fixtureDirectory, "missing_texture.fbx"), input);
            var fallbackRequest = request with { InputPath = input, OutputPath = Path.Combine(caseDirectory, "缺图回退.gdb"), ReportPath = Path.Combine(caseDirectory, "缺图回退.json") };
            var result = await service.ConvertAsync(fallbackRequest);
            Assert(result.Success && result.ExitCode == 0, result.Message);
            var json = File.ReadAllText(fallbackRequest.ReportPath);
            ReportVerifier.Verify(json, fallbackRequest);
            var report = JsonNode.Parse(json)!;
            Assert(report["textures"]!.AsArray().Count == 0, "Missing image was replaced by an invented texture.");
            var summary = ReportSummaryFormatter.Parse(json);
            Assert(summary.IsSuccess && summary.HasCompatibilityAdjustments && summary.SummaryText.Contains("材质颜色"), "Fallback is not visible in the GUI success summary.");
        });
        await TestAsync("real_normal_repair_conversion_from_gui_service", async () =>
        {
            var input = Path.Combine(caseDirectory, "无效法线.fbx");
            var source = File.ReadAllText(Path.Combine(fixtureDirectory, "textured_quad.fbx"));
            source = System.Text.RegularExpressions.Regex.Replace(source, @"Normals: \*\d+ \{ a: [^}]+", "Normals: *12 { a: 0,0,0,0,0,0,0,0,0,0,0,0 ");
            File.WriteAllText(input, source);
            var repairRequest = request with { InputPath=input, OutputPath=Path.Combine(caseDirectory,"法线修复.gdb"), ReportPath=Path.Combine(caseDirectory,"法线修复.json") };
            var result = await service.ConvertAsync(repairRequest);
            Assert(result.Success && result.ExitCode == 0, result.Message);
            var json = File.ReadAllText(repairRequest.ReportPath);
            ReportVerifier.Verify(json, repairRequest);
            var summary = ReportSummaryFormatter.Parse(json);
            Assert(summary.IsSuccess && summary.SummaryText.Contains("共修复 6 个角点法线"), "GUI must report the actual repair and GDB readback.");
        });
    }

    private static string HashDirectory(string path) => string.Join("\n", Directory.EnumerateFiles(path, "*", SearchOption.AllDirectories)
        .Order(StringComparer.Ordinal).Select(file => Path.GetRelativePath(path, file) + ":" + Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(file)))));

    private sealed class InlineProgress<T>(Action<T> callback) : IProgress<T>
    {
        public void Report(T value) => callback(value);
    }
}
