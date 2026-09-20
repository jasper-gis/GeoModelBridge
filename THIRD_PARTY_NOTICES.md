# Third-party notices

## ufbx 0.23.0

- Source: https://github.com/ufbx/ufbx
- Commit: `fcc5d6ba444cfd3eb80677dba5e37e493941abe5` (tag `v0.23.0`).
- Files: `third_party/ufbx/ufbx.c`, `ufbx.h`, `LICENSE`.
- Upstream license: dual MIT / public-domain alternative; original license text is included unchanged.

## nlohmann/json 3.12.0

- Source: https://github.com/nlohmann/json/tree/v3.12.0
- File: `third_party/nlohmann/json.hpp`.
- MIT license: `third_party/nlohmann/LICENSE.MIT`, retained unchanged.

The exact distributed files and SHA-256 digests are in `third_party/manifest.json`. `scripts/verify_dependencies.py` verifies them offline. No dependency is fetched automatically during C++ configuration.

## Esri ArcGIS Pro

The optional Pro adapter references an installed and licensed ArcGIS Pro runtime. Esri assemblies, SDK packages, license files, and credentials are **not redistributed**. Installation, licensing, platform and redistributable restrictions remain governed by Esri's terms.

## Esri FileGDB API 1.5.5

V0.1.1 adds a separate Windows x64 native backend that links against the official FileGDB API SDK. This release includes the unmodified release `FileGDBAPI.dll`, its original Apache-2.0 license, `userestrictions.txt`, upstream Windows README, and a source/hash record under `dist/licenses/filegdb-api/`. The provenance and exact hashes are also recorded in `backends/native-filegdb/sdk-sources.json`.

SDK headers, import libraries, debugging DLLs/PDBs, C# wrappers, and the full SDK archive are not redistributed in this project. Building from source requires a separately obtained official SDK; running the included native executable does not require downloading the full SDK or invoking ArcGIS Pro. The V0.1.0 historical release did not implement this backend.

## Microsoft development and runtime components

Building the native backend requires an x64 Microsoft C++ toolchain and Windows SDK. The backend uses Windows Imaging Component (WIC) for image decoding. Microsoft compiler packages, Windows SDK packages, and development caches are not part of the project distribution. Microsoft C++ runtime availability remains a separate runtime prerequisite; no local development environment is silently installed by the project build.

## Microsoft .NET and Windows Desktop runtimes in the GUI

V0.1.2 adds the self-contained Windows x64 `geomodelbridgeGUI.exe`. Its single-file distribution embeds the Microsoft .NET and WPF runtime components selected by the GUI's NuGet restore. The delivered executable uses `Microsoft.NETCore.App.Runtime.win-x64` 8.0.26 and `Microsoft.WindowsDesktop.App.Runtime.win-x64` 8.0.26. These runtime packages declare the MIT license; their exact original notice files are retained under `dist/licenses/dotnet/`:

- `microsoft.netcore.app.runtime.win-x64/8.0.26/LICENSE.TXT` and `THIRD-PARTY-NOTICES.TXT` from the .NET runtime package.
- `microsoft.windowsdesktop.app.runtime.win-x64/8.0.26/LICENSE` from the Windows Desktop runtime package. This package does not contain a separate third-party notices file.
- `manifest.json` records the resolved package versions, official NuGet sources, repository commits, package SHA-512 digests and copied notice SHA-256 digests. Original license text and line endings are unchanged.

`scripts/build.ps1 -WithGui` runs `scripts/install_dotnet_licenses.py` after publication. It reads the actual `project.assets.json`, finds those exact runtime versions in the configured NuGet package folders, verifies each local package archive against its NuGet SHA-512 file, and copies its original notices. The complete NuGet packages, development SDK, and compiler are not copied into the distribution. The GUI's included .NET runtime is separate from the Pro adapter's installed-runtime requirements.

## Project and fixtures

The C++/C# project and first-party fixtures were created for this project. ASCII FBX fixtures are authored test assets, and image fixtures are deterministic programmatic patterns. `tests/fixtures/upstream/` additionally includes two unmodified binary FBX regression models from the pinned ufbx repository; that directory retains the original `LICENSE`, exact source URLs, and SHA-256 digests in `manifest.json`. No user production models are included. No open-source license for the project's own code has been selected by the owner.
