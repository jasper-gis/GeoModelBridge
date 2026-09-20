# GeoModelBridge V0.1.6

**静态 FBX → 带颜色和贴图的 FileGDB Multipatch**。支持 Ubuntu 24.04 x86_64 命令行和 Windows x64 命令行/图形界面。唯一后端为 `native-filegdb`，构建、转换、测试和部署均无需 ArcGIS Pro、ArcPy 或 Pro 许可。

C++17 内核使用 ufbx 读取 FBX，生成 Scene Bundle；独立原生写入端使用官方 Esri FileGDB API 1.5.5 写入真实 GDB，关闭、重开并核对几何、材质、UV、法线及纹理后才报告成功。Linux 与 Windows 共用转换算法、Bundle 格式和报告契约。

## 平台与交付范围

| 平台 | 支持内容 | 运行依赖 |
| --- | --- | --- |
| Ubuntu 24.04 LTS x86_64 | inspect / prepare / convert、原生 GDB 写入与回读、命令行批处理 | glibc、libstdc++、libpng16、libjpeg-turbo、两份官方 FileGDB `.so` |
| Windows x64 | 同一命令行及 Windows WPF 中文 GUI | Microsoft Visual C++ x64 Runtime、FileGDBAPI.dll；发布的 GUI 自带 .NET |
| Ubuntu ARM64、macOS、其他 Linux 发行版 | 当前不作为支持目标 | 固定原生 SDK 为 x86_64，未完成这些平台的验证 |

WPF GUI 仍仅支持 Windows；Linux 不需要 .NET、桌面会话或显示服务器。当前 Ubuntu 构建不承诺兼容更旧的 glibc（例如 Ubuntu 22.04）。[官方 SDK 支持说明](https://github.com/Esri/file-geodatabase-api/tree/master/FileGDB_API_1.5.5) 与本项目实际验证范围分别记录，详见 [V0.1.6 验证](docs/validation-v0.1.6.md)。

Git 仓库提供源码、脚本、测试与文档，不存放编译程序、完整 SDK、生产模型或临时虚拟机。使用源码按下文构建；CI 配置包含 Ubuntu 完整构建、集成测试和可下载打包产物，只有对应运行成功后才会产生附件。

## Ubuntu 从源码构建

在 Ubuntu 24.04 x86_64 上：

```bash
sudo apt-get update
sudo apt-get install -y git g++ cmake ninja-build python3 libpng-dev libjpeg-dev
git clone https://github.com/jasper-gis/GeoModelBridge.git
cd GeoModelBridge

# 下载到新目录，自动选择 Linux 固定 SDK，核对归档及运行文件 SHA-256。
python3 scripts/fetch_filegdb_sdk.py --output build/filegdb-sdk
python3 scripts/build.py --sdk build/filegdb-sdk --include-runtime --jobs 2

./dist/bin/geomodelbridge --version
./dist/bin/geomodelbridge doctor
./dist/bin/native-filegdb/GeoModelBridge.NativeWriter --probe
```

工具要求 CMake ≥ 3.21、Python ≥ 3.11 和支持 C++17 的编译器；上述 Ubuntu 软件包满足要求。固定 Linux SDK 的 README 要求 GCC ≥ 11.5，本项目使用 Ubuntu GCC 13 验证。Linux 归档名称中的 `RHEL8-64gcc8` 是上游命名，不用它推断最低版本。下载来源与散列见 [Linux SDK 清单](backends/native-filegdb/sdk-sources-linux.json)。离线环境可用 `--archive /path/to/FileGDB_API-RHEL8-64gcc8.tar.gz`，仍执行同样的校验。

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

## Ubuntu 客户机部署

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

## 转换示例

以下以 Ubuntu 默认安装位置为例。Windows 将命令替换为 `.\dist\bin\geomodelbridge.exe`，参数相同。每次运行使用新的输出路径：

```bash
./dist/bin/geomodelbridge inspect model.fbx --report new-inspection.json
./dist/bin/geomodelbridge prepare model.fbx --output new-bundle
./dist/bin/geomodelbridge convert model.fbx --output new-model.gdb \
  --profile gis-static --wkid 32650 --origin 500000 3000000 100

# 用工程内自带的测试输入跑通完整链路。
./dist/bin/geomodelbridge convert dist/bin/demo/textured_quad.fbx \
  --output new-demo.gdb --wkid 32650 --origin 500000 3000000 100
```

`--backend native-filegdb` 可省略。`--writer /absolute/path/to/GeoModelBridge.NativeWriter` 或环境变量 `GMB_NATIVE_WRITER` 可指定另一原生写入端。`--texture-dir DIR` 可重复提供外置纹理搜索目录；`--feature-class NAME` 设置输出要素类，默认 `Models`。引号包围含空格的路径，支持 UTF-8 中文文件名；Bundle 资源路径使用 `/`，禁止绝对路径、`..` 和符号链接。

输出包含 GDB 和旁置 `new-model.gdb.report.json`。成功报告为 `status=written_and_readback_verified`、`backend=native-filegdb`。退出码：0 成功，2 参数错误，3 模型校验拒绝，4 后端不可用，5 写入进程失败，6 其他操作异常。检查 `doctor` 后还应执行 writer `--probe`：前者只显示路径和文件存在性，后者实际检查 SDK 和坐标系目录。

已有 Bundle 写入和复制成果独立回读：

```bash
./dist/bin/native-filegdb/GeoModelBridge.NativeWriter \
  --input new-bundle --output new-from-bundle.gdb --report new-writer-report.json
./dist/bin/native-filegdb/GeoModelBridge.NativeWriter \
  --verify-gdb copied.gdb --expected-report new-writer-report.json --report new-copy-check.json
```

回读副本不需要原始 FBX、Bundle 或外部纹理，只需复制的 GDB 与原成功报告。更多说明见 [原生后端](backends/native-filegdb/README.md)。

## 转换边界

- 保留常规漫反射颜色、PNG/JPEG 纹理、Alpha、UV 和面片材质绑定，保留 UV/法线角点边界；节点变换只烘焙一次。
- CLI 默认 `strict`；GUI 默认明确展示 `gis-static`。后者可使用文件保存姿态、省略环境光/高光/反射效果、删除有限零面积三角形，每项处理均记录。
- PNG 解码为未预乘 RGBA8，不做 Gamma/ICC 颜色变换，保留低 Alpha 和全透明像素中的 RGB；16 位 PNG、动画 PNG 等不支持内容明确拒绝。JPEG 验证后保留原压缩字节，不重新编码。
- GIS 静态策略仅为保守识别的 Adobe YCbCr 基线 JPEG 补 JFIF APP0，记录原/新散列；不静默转码。
- 缺贴图、无效 UV、非有限几何、未知材质、PBR、自发光、骨骼与形变仍拒绝；没有自动纹理烘焙。
- 目标 RGB 为 8 位、透明度为整百分比、UV 为单精度，法线按 FileGDB 编码量化；报告记录精度和损失。[兼容策略](docs/compatibility.md)

**WKID 赋值与 origin 平移不执行重投影或地理配准。** 示例定位数值仅为测试，需替换为模型实际参数。原生后端要求明确的米制投影坐标系，且 WKID 存在于 SDK 目录中。转换不修改源 FBX 或贴图，不覆盖已有 GDB/报告；Linux 通过不覆盖的原子重命名提交成果，不支持该操作的文件系统会明确失败。避免多个进程同时修改同一 GDB。

## Windows 构建与 GUI

安装 Visual Studio 2022 C++ x64 工具链/Windows SDK、CMake、Ninja、Python ≥ 3.11；GUI 另需 .NET 8 SDK。在 x64 Visual Studio Developer PowerShell 中：

```powershell
python scripts/fetch_filegdb_sdk.py --output build/filegdb-sdk
.\scripts\build.ps1 -WithNative -WithGui -FileGDBApiRoot "$PWD/build/filegdb-sdk" -IncludeFileGDBRuntime
.\dist\bin\geomodelbridgeGUI.exe
```

保留完整安装目录，客户机安装 Microsoft Visual C++ x64 Runtime。GUI 自带 .NET，转换链路与 Linux CLI 相同。[GUI 使用说明](docs/gui.md)。不需要 GUI 时，可改用与 Ubuntu 相同的 `python scripts/build.py --sdk build/filegdb-sdk --include-runtime`。

## 测试与打包

Ubuntu：

```bash
python3 scripts/check_version.py
python3 scripts/verify_dependencies.py --install-dir dist
python3 backends/native-filegdb/tests/integration.py \
  --writer dist/bin/native-filegdb/GeoModelBridge.NativeWriter --work artifacts/native-tests
python3 tests/native_deployment_test.py --install-dir dist --work artifacts/deployment
python3 scripts/generate_examples.py --output examples/V0.1.6
python3 scripts/package.py --check-only
# 包含源码、dist 和已验证样例，必须指定仓库外的新归档路径。
python3 scripts/package.py --output ../GeoModelBridge-V0.1.6-ubuntu24.04-x86_64.tar.gz
```

生成样例和部署测试目录必须为新路径。Windows 运行同样的 Python 脚本，writer 文件名增加 `.exe`，发布归档用 `.zip`；还需运行 `dotnet run --project apps/GeoModelBridge.Gui.Tests -c Release -- --work artifacts/gui-tests --engine-dir dist/bin --fixtures tests/fixtures`。打包会检查运行版本、依赖散列、14 个真实 GDB 与复制成果回读；生成 `.sha256` 文件，Linux tar.gz 保留执行权限。

验证记录明确区分核心测试、原生数据库回读、可搬迁部署与目标软件显示验收。自动回读通过不等于完成三维外观验收。[V0.1.6 记录](docs/validation-v0.1.6.md) · [验收边界](docs/acceptance.md)

## 维护方式

只维护 **master 一条共享代码线**，无需建立 Linux 专用分支。`src/`、`include/` 和原生 `bundle.hpp`、`codec.hpp`、`main.cpp` 共享；仅 `platform_windows.cpp` / `platform_linux.cpp` 和 `images_windows.cpp` / `images_linux.cpp` 区分平台。SDK 来源按平台记录，构建与发布脚本通过 `scripts/gmb_platform.py` 选择。CI 分别运行 Windows 和 Ubuntu 原生测试，避免一侧修改破坏另一侧。[架构说明](docs/architecture.md)

版本以 `VERSION` 为准，同步当前版本声明及 [CHANGELOG](CHANGELOG.md)。历史报告、截图和旧版本程序保留原版本；历史旧后端不属于当前工程依赖。[历史证据](docs/evidence/README.md) · [第三方许可与来源](THIRD_PARTY_NOTICES.md)。项目自身尚未选择开源许可证。
