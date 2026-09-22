# FileGDB API 调用与依赖核实 · V0.2.1

[首页](../README.md) · [命令行调用](command-line.md) · [构建与发布](build-and-release.md)

## 实际调用链

项目使用 Esri 官方 **File Geodatabase C++ API 1.5.5.330**。SDK 来自 [Esri 官方仓库](https://github.com/Esri/file-geodatabase-api/)，本项目固定下载提交和 SHA-256，详见 [Windows 清单](../backends/native-filegdb/sdk-sources.json) / [Linux 清单](../backends/native-filegdb/sdk-sources-linux.json)。未使用 ArcPy、ArcGIS Pro，也不需要 .NET 版 `Esri.FileGDBAPI.dll`。

```text
geomodelbridge.exe convert FBX ...
  -> ufbx 读取 FBX -> Scene Bundle
  -> 启动 native-filegdb/GeoModelBridge.NativeWriter.exe
  -> FileGDBAPI.dll -> 创建 GDB / Multipatch -> 关闭重开回读 -> 成功报告
```

可复核的源码位置：

| 文件 | 证据 |
| --- | --- |
| [src/main.cpp](../src/main.cpp) | `default_writer()` 查找 writer；`convert()` 生成临时 Bundle，`run_process()` 启动并等待原生进程，最后校验报告 |
| [native main.cpp](../backends/native-filegdb/main.cpp) | `#include <FileGDBAPI.h>`、`FileGDBAPI::CreateGeodatabase`、`Geodatabase::CreateTable`、`Table::Insert`；关闭后 `OpenGeodatabase`、`OpenTable`、`Search` 回读 |
| [根 CMakeLists.txt](../CMakeLists.txt) | 统一定义 CLI 和 writer；Windows 链接 `${FILEGDB_API_ROOT}/lib64/FileGDBAPI.lib`，Linux 链接 `lib/libFileGDBAPI.so` |
| [codec.hpp](../backends/native-filegdb/codec.hpp) | 按官方 Extended Shape Buffer Format 编码几何、材质和纹理后交给 SDK 存储 |

Windows 的 `.lib` 是 DLL 导入库，并不是将数据库引擎静态编入 EXE。**主 CLI 本身不导入 FileGDBAPI.dll，真正依赖 DLL 的是原生 writer**。因此只检查主 CLI 的依赖表或搜索 C# 的 `DllImport` 会错过调用链。Windows 中可在 MSVC 开发终端执行：

```powershell
dumpbin /DEPENDENTS .\dist\bin\native-filegdb\GeoModelBridge.NativeWriter.exe
.\dist\bin\native-filegdb\GeoModelBridge.NativeWriter.exe --probe
```

前者显示 `FileGDBAPI.dll` 导入，后者通过 SDK 查询 CRS 目录。真实纹理写入、关闭重开及独立复制回读由 [集成测试](../backends/native-filegdb/tests/integration.py) 和 [部署测试](../tests/native_deployment_test.py) 验证；统一构建实测结果见 [V0.1.14 记录](validation-v0.1.14.md)，先前依赖核实见 [V0.1.13](validation-v0.1.13.md)。历史证据保留原版本，回读验收不等同于目标 GIS 软件外观验收。

## 下载仓库或编译后为何没有 DLL

1. **git clone / Download ZIP 是源码分发。** `.gitignore` 排除了 `build/`、`dist/`、`releases/` 和 `**/bin/`；SDK、编译器与 EXE / DLL 不作为源码提交。仓库保留固定下载脚本、散列和第三方声明。源码包与完整安装包是两类产物。
2. **V0.1.13 及以前的默认 preset 只构建 core。** 当时 `cmake --preset release` / 未带 `-WithNative` 的 PowerShell 构建不会编译 writer。V0.1.14 改为默认完整构建；现在只有显式 `core-release` / `-CoreOnly` / `GMB_BUILD_NATIVE=OFF` 才不构建 writer。
3. **V0.1.12 及以前，运行库附带默认关闭。** 即使构建 writer，只有 `-IncludeFileGDBRuntime` / `--include-runtime` / `GMB_INSTALL_FILEGDB_RUNTIME=ON` 才会安装运行库。
4. **旧版只在 install 阶段复制 DLL。** `cmake --build` 后的 writer 目录没有 DLL，开发机可能靠 PATH 中 SDK 目录运行，掩盖交付缺文件。只拷贝 writer EXE 不能在客户机转换。

V0.1.13 保留纯核心构建，但**启用 native 时默认附带运行库**：构建复制到 writer 旁，安装复制到 `bin/native-filegdb` 并保留官方许可 / README / 来源清单。每次默认 build 都检查并补回缺失的运行库，即使 writer 无需重新链接。构建脚本在删除开发 SDK 环境路径的子进程中核对 DLL / SO 散列并执行 probe，以暴露缺依赖。

V0.1.14 进一步将所有 CMake 规则合并到根目录，移除后端独立工程。默认构建及只选择 `geomodelbridge` 目标构建都会同时生成 writer；两个 EXE 和运行库直接按可调用结构放在 `<build>/bin`。CTest 在安装前验证 CLI 的默认发现路径，实际生成纹理 GDB 并复制回读。

直接使用已配置过的 CMake 目录时，缓存中的旧 `OFF` 不会被新默认值覆盖：请显式加 `-DGMB_INSTALL_FILEGDB_RUNTIME=ON`，或使用新构建目录。两种构建脚本会每次传入实际的 ON / OFF 值。

## 从源码获得完整 Windows CLI

准备 Git、Python ≥ 3.11、CMake ≥ 3.21、Ninja、MSVC x64 / Windows SDK。在 x64 Visual Studio Developer PowerShell 中：

```powershell
git clone https://github.com/jasper-gis/GeoModelBridge.git
cd GeoModelBridge
python scripts/fetch_filegdb_sdk.py --output build/filegdb-sdk
.\scripts\build.ps1
python scripts/verify_dependencies.py --install-dir dist
.\dist\bin\geomodelbridge.exe convert -h
.\dist\bin\native-filegdb\GeoModelBridge.NativeWriter.exe --probe
```

此命令不构建 GUI，不需要 .NET SDK。也可使用 `python scripts/build.py` 或根目录 `release` preset 统一构建 core 和 writer。SDK 下载路径必须全新；后续重建不重复下载。自定义 SDK 可用 `-FileGDBApiRoot` / `--sdk` 或 `FILEGDB_API_ROOT`，默认目录为 `build/filegdb-sdk`。构建脚本可用自定义 build / install 路径保留旧安装；构建和安装目录不能存放用户成果。

完整安装关键文件：

```text
dist/
  bin/
    geomodelbridge.exe
    native-filegdb/
      GeoModelBridge.NativeWriter.exe
      FileGDBAPI.dll
    demo/textured_quad.fbx
    demo/checker.png
  licenses/filegdb-api/
    Apache License.pdf
    userestrictions.txt
    README-windows_VS2022.txt
    sdk-sources.json
  docs/command-line.md
  docs/filegdb-api.md
  python/geomodelbridge/...
```

客户机还需 Microsoft Visual C++ x64 Runtime。不用复制 SDK include、`.lib`、PDB、debug DLL、示例程序、.NET wrapper。分发完整安装目录和许可；构建目录仅用于开发调试，不能只复制其中的 EXE / DLL 当成完整发布。

Linux 使用 `python3 scripts/build.py --sdk build/filegdb-sdk`；相应文件是无扩展名 writer、`libFileGDBAPI.so` 与 `libfgdbunixrtl.so`，运行时通过 `$ORIGIN` 从 writer 旁加载。系统 libpng、libjpeg、C++ runtime 仍按[部署指南](build-and-release.md)安装。

## 显式不附带 SDK 运行库

由部署方自行供应 SDK 时，使用 `build.ps1 -WithNative ... -ExcludeFileGDBRuntime`、`build.py --sdk ... --no-runtime`，或 CMake `-DGMB_INSTALL_FILEGDB_RUNTIME=OFF`。Windows 设置 SDK `bin64` 到 PATH；Linux 设置 SDK `lib` 到 `LD_LIBRARY_PATH`。构建会显示非独立部署提示。

既有 `-IncludeFileGDBRuntime` / `--include-runtime` 保持兼容，现在等同默认。关闭选项不会删除之前复制的 DLL；验证无运行库模式须使用新的 build / install 目录。不要把一台开发机能够运行，视为发布依赖齐全。
