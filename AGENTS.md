# GeoModelBridge contributor notes

This is a C++17 model conversion engine with an optional Windows/.NET ArcGIS Pro adapter. Keep the core independent of Esri assemblies and preserve the Scene Bundle contract.

- Do not describe the native FileGDB writer as implemented until a real textured GDB is written and independently verified through that backend.
- Do not silently drop colors, textures, UV bindings, alpha, transforms, or unsupported material channels. Reject unsupported render semantics or record an explicit supported policy.
- Never overwrite existing user FBX, texture directories, GDBs, or reports. Staging cleanup must be restricted to paths created by the current operation.
- Geometry is already normalized to right-handed Z-up metres and has its origin translation applied in the bundle. Do not apply node matrices twice or treat a WKID assignment as reprojection.
- Keep material/UV/normal corner boundaries. Avoid merging corners by position alone.
- Use official SDK documentation or the installed ArcGIS.Core XML/reference API. Do not invent FileGDB material blob encodings.
- Run CMake/CTest core tests for C++ changes. Run `scripts/test-pro.ps1` after writer changes where a licensed Pro installation is available; report when it is unavailable. Visual acceptance remains a separate check.
- Verify dependency hashes with `python scripts/verify_dependencies.py`. Update pinned sources and notices together.
- Initial release is V0.1.0. The current small iteration is V0.1.4, reducing bundle serialization memory and bounding GUI process logs. Update all version-bearing project files and changelog together; do not relabel historical evidence or unverified capabilities as newly verified.
- Strict is the CLI default. The explicit gis-static profile may use the saved static pose, omit ambient/specular/reflection shading, and remove finite zero-area triangles only when each adjustment is reported. It must not weaken missing textures, UVs, nonfinite geometry, unsupported deformation, or unknown material checks.
- GIS static may add a leading JFIF APP0 only for conservatively identified Adobe YCbCr baseline JPEGs with safe orientation/metadata. Preserve all original bytes after SOI, record before/after hashes, and reject ambiguous cases. Do not silently transcode textures in either backend.

Build: `scripts/build.ps1 -WithGui -WithPro` on Windows; `cmake --preset release`, `cmake --build --preset release`, `ctest --preset release` for core-only builds.
