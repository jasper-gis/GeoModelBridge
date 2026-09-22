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

## libjpeg-turbo 3.1.4.1 (Windows JPEG validation)

- Official source: https://github.com/libjpeg-turbo/libjpeg-turbo/releases/tag/3.1.4.1
- Source archive SHA-256: `ecae8008e2cc9ade2f2c1bb9d5e6d4fb73e7c433866a056bd82980741571a022`, checked against the official release asset digest.
- The unchanged decoder sources, supporting headers and original license texts are retained under `third_party/libjpeg-turbo/`; each distributed file is pinned in `third_party/manifest.json`.
- The root CMake project builds only a static libjpeg decoder for the Windows writer, with SIMD disabled. No encoder, TurboJPEG API, assembly toolchain or upstream command-line tools are built or installed. Linux continues to use its distribution-provided libjpeg library through the same validation code.
- Original IJG and BSD license texts are installed as `licenses/libjpeg-turbo/README.ijg` and `LICENSE.md` with the Windows native writer.

This software is based in part on the work of the Independent JPEG Group.

## libpng 1.6.58 and zlib 1.3.2 (Windows PNG decoding)

- Official sources: https://www.libpng.org/pub/png/libpng.html and https://zlib.net/.
- libpng archive SHA-256: `8c9b05b675ca7301a458df2c2e46f26e1d41ff36b8863f8c33530bc58c2e6225`, checked against the official libpng release page.
- zlib archive SHA-256: `bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16`, checked against the official GitHub release asset digest.
- Original decoder sources and headers are retained unchanged under `third_party/libpng/` and `third_party/zlib/`; every distributed file and source archive is pinned in `third_party/manifest.json`.
- The root CMake build generates a decoder-only libpng configuration from the unchanged upstream prebuilt header, disabling PNG write helpers. Only static PNG decoding and zlib inflate/checksum sources are compiled; no encoder, gzip tools, SIMD assembly or additional dynamic library is installed.
- Original licenses are installed as `licenses/libpng/LICENSE` (PNG Reference Library License version 2) and `licenses/zlib/LICENSE` (zlib license). Linux continues to link distribution-provided libraries with the same decoding implementation.

## Esri FileGDB API 1.5.5

V0.1.1 adds a separate Windows x64 native backend that links against the official FileGDB API SDK. This release includes the unmodified release `FileGDBAPI.dll`, its original Apache-2.0 license, `userestrictions.txt`, upstream Windows README, and a source/hash record under `dist/licenses/filegdb-api/`. The provenance and exact hashes are also recorded in `backends/native-filegdb/sdk-sources.json`.

SDK headers, import libraries, debugging DLLs/PDBs, C# wrappers, and the full SDK archive are not redistributed in this project. Building from source requires a separately obtained official SDK; running the included native executable does not require downloading the full SDK or invoking ArcGIS Pro. The V0.1.0 historical release did not implement this backend.

## Microsoft development and runtime components

Building the Windows native backend requires an x64 Microsoft C++ toolchain and Windows SDK. The backend statically links pinned libpng/zlib and libjpeg-turbo decoders for strict PNG and JPEG validation. Microsoft compiler packages, Windows SDK packages, and development caches are not part of the project distribution. Microsoft C++ runtime availability remains a separate runtime prerequisite; no local development environment is silently installed by the project build.

## Microsoft .NET and Windows Desktop runtimes in the GUI

V0.1.2 adds the self-contained Windows x64 `geomodelbridgeGUI.exe`. Its single-file distribution embeds the Microsoft .NET and WPF runtime components selected by the GUI's NuGet restore. The delivered executable uses `Microsoft.NETCore.App.Runtime.win-x64` 8.0.26 and `Microsoft.WindowsDesktop.App.Runtime.win-x64` 8.0.26. These runtime packages declare the MIT license; their exact original notice files are retained under `dist/licenses/dotnet/`:

- `microsoft.netcore.app.runtime.win-x64/8.0.26/LICENSE.TXT` and `THIRD-PARTY-NOTICES.TXT` from the .NET runtime package.
- `microsoft.windowsdesktop.app.runtime.win-x64/8.0.26/LICENSE` from the Windows Desktop runtime package. This package does not contain a separate third-party notices file.
- `manifest.json` records the resolved package versions, official NuGet sources, repository commits, package SHA-512 digests and copied notice SHA-256 digests. Original license text and line endings are unchanged.

`scripts/build.ps1 -WithGui` runs `scripts/install_dotnet_licenses.py` after publication. It reads the actual `project.assets.json`, finds those exact runtime versions in the configured NuGet package folders, verifies each local package archive against its NuGet SHA-512 file, and copies its original notices. The complete NuGet packages, development SDK, and compiler are not copied into the distribution. The GUI embeds its runtime independently of installed desktop GIS software.

## Project and fixtures

The C++/C# project and first-party fixtures were created for this project. ASCII FBX fixtures are authored test assets, and image fixtures are deterministic programmatic patterns. `tests/fixtures/upstream/` additionally includes two unmodified binary FBX regression models from the pinned ufbx repository; that directory retains the original `LICENSE`, exact source URLs, and SHA-256 digests in `manifest.json`. No user production models are included. No open-source license for the project's own code has been selected by the owner.

## Linux SDK and system image libraries (V0.1.6)

The Ubuntu x86_64 writer links the official Linux FileGDB API 1.5.5.330 SDK. The immutable download URL, upstream commit, archive SHA-256, shared-library hashes and every installed original notice hash are in `backends/native-filegdb/sdk-sources-linux.json`. The API is Apache-2.0; the separate **libfgdbunixrtl.so uses LGPL 2.1**, as stated by the SDK README. Its upstream source is available from [Esri FGDB Linux Runtime](https://sourceforge.net/projects/esrifgdblinuxru/files/).

With `GMB_INSTALL_FILEGDB_RUNTIME=ON`, installation copies the unmodified `libFileGDBAPI.so`, `libfgdbunixrtl.so`, complete SDK `license/` directory (including LGPL text and third-party acknowledgements), README and provenance manifest. Dynamic linking is retained. The complete SDK, development files and toolchain are not committed or packaged. Do not describe the whole Linux SDK runtime as Apache-only.

Ubuntu image decoding dynamically links the distribution's libpng and libjpeg-turbo; zlib is a transitive libpng dependency. These system libraries are installed through apt, not vendored in the project binary package. Their original notices are available on Ubuntu under `/usr/share/doc/libpng16-16t64/copyright`, `/usr/share/doc/libjpeg-turbo8/copyright`, and `/usr/share/doc/zlib1g/copyright`. See [libpng](http://www.libpng.org/pub/png/libpng.html) and [libjpeg-turbo](https://github.com/libjpeg-turbo/libjpeg-turbo). Their installed versions are recorded with the Ubuntu validation evidence; no source images are re-encoded through these libraries.
