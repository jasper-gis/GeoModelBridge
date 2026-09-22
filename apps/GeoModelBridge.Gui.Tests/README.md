# GUI service tests

These dependency-free .NET 8 console tests exercise the same validation, safe process arguments, backend discovery, and report verification used by the WPF application. They do not reference WPF and do not simulate graphical acceptance.

From the project directory:

```powershell
dotnet run --project apps/GeoModelBridge.Gui.Tests -c Release -- --work C:\temp\gmb-gui-test-new
```

To also exercise real V0.1.13 native conversions through the GUI service, provide the installed engine directory and source fixtures:

```powershell
dotnet run --project apps/GeoModelBridge.Gui.Tests -c Release -- --work C:\temp\gmb-gui-integration-new --engine-dir dist/bin --fixtures tests/fixtures
```

The work directory must not exist. Each run creates its own fixtures and writes `test-results.json` with every case and failure. The integration test uses a textured FBX in a path containing Chinese characters, spaces, and `&`, checks the native backend probe and reopened-GDB success report, and verifies that a repeated conversion cannot overwrite the GDB or report. A successful process exit alone never counts as a successful conversion.

V0.1.3 also checks strict/static-GIS profile selection, profile mismatches in reports, aggregation of hundreds of repeated diagnostics, readable Chinese failure summaries, unknown-error retention, and failure reports from processes that exit with either zero or a nonzero status.

V0.1.6 rejects the removed backend in settings, environment probes and reports; the GUI exposes only native FileGDB conversion. The current suite runs without desktop GIS software.

V0.1.7 checks the independent missing-file policy, safe arguments, report-policy matching, Chinese fallback warnings, and a real missing-texture FBX conversion with no replacement images.

V0.1.8 checks explicit normal-repair warnings, valid corner-count aggregation, malformed count handling, and a real textured FBX with repaired normals through the GUI service.
