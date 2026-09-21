# 构建、部署与发布指南

[← 返回项目首页](../README.md) · [原生后端](../backends/native-filegdb/README.md) · [验证记录](validation-v0.1.7.md)

本文补充 README 的快速开始流程，面向需要自定义构建、迁移客户机或维护发布的使用者。所有命令在仓库根目录执行；默认安装目录为 `dist`。

## 构建选项

工具要求 CMake ≥ 3.21、Python ≥ 3.11 和支持 C++17 的编译器；[首页快速开始](../README.md#quick-start)列出的 Ubuntu 软件包满足要求。固定 Linux SDK 的 README 要求 GCC ≥ 11.5，本项目使用 Ubuntu GCC 13 验证。Linux 归档名称中的 `RHEL8-64gcc8` 是上游命名，不用它推断最低版本。下载来源与散列见 [Linux SDK 清单](../backends/native-filegdb/sdk-sources-linux.json)。

离线环境使用已下载的官方归档，仍执行同样的散列校验；输出目录必须不存在：

```bash
python3 scripts/fetch_filegdb_sdk.py \
  --archive /path/to/FileGDB_API-RHEL8-64gcc8.tar.gz --output build/filegdb-sdk
```

`build.py` 执行版本检查、依赖核对、CMake Release 构建、CTest、安装和运行库探测；可用 `--build-dir`、`--install-dir` 指定目录。构建会更新自己的安装文件，请不要把用户数据放入构建/安装目录。转换、报告和 SDK 下载拒绝覆盖已有输出。首次构建后如只改源码，直接重新运行构建命令，不必重新下载 SDK。

也可直接使用统一 CMake：

```bash
cmake -S . -B build/linux-x64 -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DGMB_BUILD_NATIVE=ON -DFILEGDB_API_ROOT="$PWD/build/filegdb-sdk" \
  -DGMB_INSTALL_FILEGDB_RUNTIME=ON
cmake --build build/linux-x64 --parallel 2
ctest --test-dir build/linux-x64 --output-on-failure
cmake --install build/linux-x64 --prefix "$PWD/dist"
```

仅需要检查 FBX 或生成中间 Bundle 时，可运行 `cmake --preset release`、`cmake --build --preset release`、`ctest --preset release`。默认不启用原生后端，此时不能转换为 GDB。

### Windows 命令行构建

在 x64 Visual Studio Developer PowerShell 中，先准备 Windows 对应的固定 SDK。仅构建命令行与原生写入端可使用共享入口：

```powershell
python scripts/build.py --sdk build/filegdb-sdk --include-runtime
```

需要 WPF GUI 时使用 `scripts/build.ps1 -WithNative -WithGui`，完整参数见[首页快速开始](../README.md#quick-start)。Windows 原生后端要求 MSVC ABI；GUI 另需 .NET 8 SDK。不要将 Linux SDK 与 Windows 构建混用。

## 客户机部署

客户机使用已构建的完整安装目录，不需要安装编译器或下载完整 SDK。源码仓库本身不包含二进制程序；GitHub Actions 只有在对应构建运行成功时才会生成 Ubuntu 打包附件。

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

`--include-runtime` 明确附带官方共享库与原始许可；不使用此选项时需自行提供 SDK 运行库：Linux 在运行前设置 `LD_LIBRARY_PATH=/path/to/sdk/lib`，Windows 将 SDK `bin64` 加入 PATH。无需完整 SDK 的是客户机，源码编译仍需要平台对应 SDK。

### Windows x64

复制完整安装目录并安装 Microsoft Visual C++ x64 Runtime。保留 `bin/native-filegdb/GeoModelBridge.NativeWriter.exe`、同目录的 `FileGDBAPI.dll` 和原始许可文件。若包含 GUI，运行 `bin/geomodelbridgeGUI.exe`；其 .NET 运行库已随程序提供。客户机无需 ArcGIS Pro 或 Pro 许可。

## 环境检查与排错

`geomodelbridge doctor` 显示 writer 路径及文件是否存在；writer `--probe` 才会实际加载 SDK 并检查坐标系目录。两项检查用途不同，部署后都应执行。

| 现象 | 检查方式 |
| --- | --- |
| writer not found | 保留安装目录结构，或用 `--writer` / `GMB_NATIVE_WRITER` 指定原生可执行文件 |
| 缺少 FileGDB 共享库 / DLL | 使用附带运行库的完整安装；未附带时按上文设置平台运行库路径 |
| 输出或报告已存在 | 使用新的名称；程序拒绝覆盖已有用户成果 |
| 坐标系被拒绝 | 检查 WKID 是否属于 SDK 支持的米制投影坐标系，并显式设置 origin |
| 缺少贴图文件 | 默认以材质颜色继续并告警；可用重复的 `--texture-dir DIR` 补充目录。要求完整贴图时使用 `--missing-textures error` |
| 无效法线 / UV 或材质语义不支持 | 查看中文分类与完整诊断，在建模软件中修正后重试；缺图回退不会修复这些独立问题 |

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
python3 tests/native_deployment_test.py --install-dir dist --work artifacts/deployment
python3 scripts/generate_examples.py --output examples/V0.1.7
python3 scripts/package.py --check-only
# 包含源码、dist 和已验证样例，必须指定仓库外的新归档路径。
python3 scripts/package.py --output ../GeoModelBridge-V0.1.7-ubuntu24.04-x86_64.tar.gz
```

生成样例和部署测试目录必须为新路径。Windows 运行同样的 Python 脚本，writer 文件名增加 `.exe`，发布归档用 `.zip`；还需运行 `dotnet run --project apps/GeoModelBridge.Gui.Tests -c Release -- --work artifacts/gui-tests --engine-dir dist/bin --fixtures tests/fixtures`。打包会检查运行版本、依赖散列、14 个真实 GDB 与复制成果回读；生成 `.sha256` 文件，Linux tar.gz 保留执行权限。

验证记录明确区分核心测试、原生数据库回读、可搬迁部署与目标软件显示验收。自动回读通过不等于完成三维外观验收。[V0.1.7 记录](validation-v0.1.7.md) · [平台迁移记录](validation-v0.1.6.md) · [验收边界](acceptance.md)
