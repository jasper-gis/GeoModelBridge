# 构建、部署与发布指南

[← 返回项目首页](../README.md) · [原生后端](../backends/native-filegdb/README.md) · [Python 调用库](python-client.md) · [验证记录](validation-v0.2.md)

本文补充 README 的快速开始流程，面向需要自定义构建、迁移客户机或维护发布的使用者。所有命令在仓库根目录执行；默认安装目录为 `dist`。

## 构建选项

V0.1.14 只保留仓库根目录的 `CMakeLists.txt`。核心库、CLI、原生 writer、SDK 链接、运行库复制、CTest 和安装规则均由此文件定义；`backends/native-filegdb` 仅保留 C++ 源码、SDK 来源清单和测试，不再是可独立配置的 CMake 工程。

默认完整构建，缺少 SDK 会在配置阶段失败并给出下载命令，不会默认留下只能输出中间 JSON 的核心程序。统一构建目录和安装目录均包含 `bin/geomodelbridge[.exe]` 及其旁边的 `bin/native-filegdb/GeoModelBridge.NativeWriter[.exe]` 和 SDK 运行库。

工具要求 CMake ≥ 3.21、Python ≥ 3.11 和支持 C++17 的编译器；[首页快速开始](../README.md#quick-start)列出的 Ubuntu 软件包满足要求。固定 Linux SDK 的 README 要求 GCC ≥ 11.5，本项目使用 Ubuntu GCC 13 验证。Linux 归档名称中的 `RHEL8-64gcc8` 是上游命名，不用它推断最低版本。下载来源与散列见 [Linux SDK 清单](../backends/native-filegdb/sdk-sources-linux.json)。

离线环境使用已下载的官方归档，仍执行同样的散列校验；输出目录必须不存在：

```bash
python3 scripts/fetch_filegdb_sdk.py \
  --archive /path/to/FileGDB_API-RHEL8-64gcc8.tar.gz --output build/filegdb-sdk
```

`build.py` 和 `build.ps1` 都只调用根工程，执行一次配置、构建、CTest 和安装。SDK 优先使用显式 `--sdk` / `-FileGDBApiRoot`，其次 `FILEGDB_API_ROOT`，最后 `build/filegdb-sdk`。可用 `--build-dir` / `-BuildDirectory`、`--install-dir` / `-InstallDirectory` 指定目录。构建会更新自己的安装文件，请不要把用户数据放入构建/安装目录。转换、报告和 SDK 下载拒绝覆盖已有输出。首次构建后直接重跑构建命令，不必重新下载 SDK。

两平台都可在根目录直接使用同一套命令（Windows 在 x64 MSVC 开发终端中运行）：

```text
python scripts/fetch_filegdb_sdk.py --output build/filegdb-sdk
cmake --preset release
cmake --build --preset release
ctest --preset release
cmake --install build/release --prefix dist
```

Ubuntu 可将 `python` 换成 `python3`。默认 `release` 构建含 writer 和运行库；构建后即能从 `build/release/bin` 调用 CLI，不需要手动补 `--writer`。发布仍使用完整安装目录，包含许可和文档。

也可直接使用统一 CMake：

```bash
cmake -S . -B build/linux-x64 -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DGMB_BUILD_NATIVE=ON -DFILEGDB_API_ROOT="$PWD/build/filegdb-sdk" \
  -DGMB_INSTALL_FILEGDB_RUNTIME=ON
cmake --build build/linux-x64 --parallel 2
ctest --test-dir build/linux-x64 --output-on-failure
cmake --install build/linux-x64 --prefix "$PWD/dist"
```

仅需要检查 FBX 或生成中间 Bundle 时，显式运行 `cmake --preset core-release`、`cmake --build --preset core-release`、`ctest --preset core-release`，或 `build.ps1 -CoreOnly` / CMake `-DGMB_BUILD_NATIVE=OFF`。此模式不需要 SDK，也不会编译 writer。`debug` preset 同样仅构建核心，不使用与 release SDK 不兼容的 Debug STL ABI。

运行库沿用 V0.1.13 的默认附带策略；`release` preset 显式设置 `GMB_BUILD_NATIVE=ON` 和 `GMB_INSTALL_FILEGDB_RUNTIME=ON`。直接使用旧缓存时，原有 OFF 不会自动改变。纯命令行及连续调用见 [命令行文档](command-line.md)，SDK 调用链见 [FileGDB API 说明](filegdb-api.md)。

### Windows 命令行构建

在 x64 Visual Studio Developer PowerShell 中，先准备 Windows 对应的固定 SDK。仅构建命令行与原生写入端可使用共享入口：

```powershell
python scripts/build.py
```

也可执行 `scripts/build.ps1`；需要 WPF GUI 时加 `-WithGui`。`-WithNative` 保留兼容，但现在是默认行为。Windows 完整构建统一使用 x64 MSVC 编译 CLI 与 writer；GUI 另需 .NET 8 SDK。不要将 Linux SDK 与 Windows 构建混用。

### 从旧分离构建迁移

- 不再执行 `cmake -S backends/native-filegdb`，全部改成 `cmake -S .` 或根目录 preset。
- `build.ps1` 的 `-NativeBuildDirectory` 已移除；只用 `-BuildDirectory` 指定统一目录。建议首次迁移用新目录，保留旧产物。
- 原生子工程的 CMake 缓存不能当成根工程缓存使用。旧 MinGW 核心缓存也不能用于完整 Windows MSVC 构建；使用新目录。
- 构建产物不再散落在 build 根目录和 backend 子目录，全部归入 `<build>/bin`。多配置生成器使用 `<build>/bin/Release` 等配置子目录，其内同样保留 `native-filegdb`；构建、CTest、安装时分别加 `--config Release`、`-C Release`、`--config Release`。
- `core-release` / `-CoreOnly` 用新目录验证，不会删除旧目录中之前生成的 writer 或 DLL。

## 客户机部署

客户机使用已构建的完整安装目录，不需要安装编译器或下载完整 SDK。源码仓库本身不包含二进制程序；GitHub Actions 的对应原生任务成功后生成 Windows ZIP 或 Ubuntu tar.gz 打包附件及 SHA-256。下载源码 ZIP 不会包含这些构建附件。

### Ubuntu 24.04 x86_64

将整个 `dist` 目录复制到另一台 Ubuntu 24.04 x86_64 机器，保留可执行权限和目录结构。客户机仅安装系统运行库：

```bash
sudo apt-get install -y libstdc++6 libpng16-16t64 libjpeg-turbo8
/path/to/dist/bin/native-filegdb/GeoModelBridge.NativeWriter --probe
/path/to/dist/bin/geomodelbridge doctor
```

完整原生安装的目录结构为：

```text
dist/
  bin/
    geomodelbridge
    native-filegdb/
      GeoModelBridge.NativeWriter
      libFileGDBAPI.so
      libfgdbunixrtl.so
    demo/                         # FBX + 贴图试运行输入
  licenses/filegdb-api/           # 上游许可、README、来源与散列
  docs/
```

原生写入端通过自身目录的 `$ORIGIN` 加载 FileGDB 运行库，移动目录后无需开发 SDK 路径。CLI 支持从任意工作目录通过 PATH 调用，并按自身真实位置定位 writer。只复制 CLI 一个文件不足以转换。

默认附带官方共享库与原始许可，兼容 `--include-runtime` / `-IncludeFileGDBRuntime`。显式 `--no-runtime` / `-ExcludeFileGDBRuntime` 时需自行提供 SDK 运行库：Linux 在运行前设置 `LD_LIBRARY_PATH=/path/to/sdk/lib`，Windows 将 SDK `bin64` 加入 PATH。关闭附带不会删除已有运行库，需在新目录验证该模式。无需完整 SDK 的是客户机，源码编译仍需要平台对应 SDK。

### Windows x64

复制完整安装目录并安装 Microsoft Visual C++ x64 Runtime。保留 `bin/native-filegdb/GeoModelBridge.NativeWriter.exe`、同目录的 `FileGDBAPI.dll` 和原始许可文件。若包含 GUI，运行 `bin/geomodelbridgeGUI.exe`；其 .NET 运行库已随程序提供。客户机无需 ArcGIS Pro 或 Pro 许可。

## 环境检查与排错

`geomodelbridge doctor` 显示 writer 路径及文件是否存在；writer `--probe` 才会实际加载 SDK 并检查坐标系目录。两项检查用途不同，部署后都应执行。

| 现象 | 检查方式 |
| --- | --- |
| writer not found | 保留安装目录结构，或用 `--writer` / `GMB_NATIVE_WRITER` 指定原生可执行文件 |
| 缺少 FileGDB 共享库 / DLL | 使用附带运行库的完整安装；未附带时按上文设置平台运行库路径 |
| 输出或报告已存在 | 使用新的名称；程序拒绝覆盖已有用户成果 |
| Cannot commit report/bundle without replacing output | 检查是否有任务占用了同名目标、目录写权限及文件系统是否支持禁止覆盖重命名；保留已有成果，换新名称重试 |
| Symbolic links / Reparse points are forbidden | 输出或父目录经过链接，改用实际目录路径；Windows 联接目录也属于重解析点 |
| 坐标系被拒绝 | 检查 WKID 是否属于 SDK 支持的米制投影坐标系，并显式设置 origin |
| 缺少贴图文件 | 默认以材质颜色继续并告警；可用重复的 `--texture-dir DIR` 补充目录。要求完整贴图时使用 `--missing-textures error` |
| TEXTURE_READ_ERROR | 报告中的贴图路径可能是目录、父路径被文件占用，或没有访问权限；修正对应路径 / 权限后重试，不能靠缺图回退跳过 |
| 无效法线 / UV 或材质语义不支持 | 无效法线可选 GIS 静态兼容修复；缺少有效 UV 或损坏几何仍需在建模软件中修正 |

Linux 提交成果使用禁止覆盖的原子重命名，不支持该操作的文件系统会明确失败；避免多个进程同时修改同一 GDB。Bundle 资源路径使用 `/`，禁止绝对路径、`..` 和符号链接。

CLI 退出码：

| 退出码 | 含义 |
| --- | --- |
| 0 | 操作成功 |
| 2 | 参数错误或已有路径冲突 |
| 3 | 模型 / 转换策略校验拒绝 |
| 4 | 后端不支持或不可用 |
| 5 | 原生写入进程失败 |
| 6 | 解析、IO 或其他操作异常 |

## 测试与打包

Ubuntu：

```bash
python3 scripts/check_version.py
python3 scripts/verify_dependencies.py --install-dir dist
python3 backends/native-filegdb/tests/integration.py \
  --writer dist/bin/native-filegdb/GeoModelBridge.NativeWriter --work artifacts/native-tests
python3 tests/missing_texture_test.py dist/bin/geomodelbridge tests/fixtures \
  --writer dist/bin/native-filegdb/GeoModelBridge.NativeWriter
python3 tests/normal_repair_test.py dist/bin/geomodelbridge tests/fixtures \
  --writer dist/bin/native-filegdb/GeoModelBridge.NativeWriter
python3 tests/native_deployment_test.py --install-dir dist --work artifacts/deployment
python3 tests/python_client_test.py
python3 tests/python_client_integration_test.py --install-dir dist --work artifacts/python-client
python3 scripts/generate_examples.py --output examples/V0.2.0
python3 scripts/package.py --check-only
# 包含源码、dist 和已验证样例，必须指定仓库外的新归档路径。
python3 scripts/package.py --output ../GeoModelBridge-V0.2.0-ubuntu24.04-x86_64.tar.gz
```

生成样例和部署测试目录必须为新路径。Windows 运行同样的 Python 脚本，writer 文件名增加 `.exe`，发布归档用 `.zip`；还需运行 `dotnet run --project apps/GeoModelBridge.Gui.Tests -c Release -- --work artifacts/gui-tests --engine-dir dist/bin --fixtures tests/fixtures`。打包会检查运行版本、依赖散列、14 个真实 GDB 与复制成果回读；生成 `.sha256` 文件，Linux tar.gz 保留执行权限。

Python 库与普通调用示例自动安装到 `dist/python`，可在不改动 ArcGIS Python 环境的情况下导入；版本必须与 EXE / writer 一致。详见 [Python 接入文档](python-client.md)。

验证记录明确区分核心测试、原生数据库回读、可搬迁部署与目标软件显示验收。自动回读通过不等于完成三维外观验收。[V0.2 记录](validation-v0.2.md) · [统一构建历史](validation-v0.1.14.md) · [大模型历史记录](validation-v0.1.8.md) · [平台迁移记录](validation-v0.1.6.md) · [验收边界](acceptance.md)
