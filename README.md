# GeoModelBridge V0.1.5

Windows x64 三维模型转换工具：**静态 FBX → 带颜色和贴图的 FileGDB Multipatch**。仅提供原生 FileGDB 后端 `native-filegdb`，构建、转换、自动测试和发布均无需安装或授权 ArcGIS Pro。

C++17 内核使用 ufbx 读取 FBX，生成 Scene Bundle；独立原生写入端使用 Esri FileGDB API 1.5.5 生成 GDB，关闭并重开核对几何、材质、UV、法线和纹理后才报告成功。Windows 图形界面与命令行共用这条转换链路。

## 使用程序

本机新版程序位于 `releases/V0.1.5/bin/geomodelbridgeGUI.exe`。保留整个版本目录，在界面选择 FBX、新的输出 GDB、目标投影 WKID 和米制 X/Y/Z 原点后转换。转换后端固定为原生 FileGDB。[界面说明](docs/gui.md)

运行要求：Windows 11 x64（当前验证平台）、Microsoft Visual C++ x64 运行库。完整发布目录附带 `FileGDBAPI.dll` 和原始许可，GUI 自带 .NET；客户机无需下载完整 SDK。源码 Git 不包含编译程序和 SDK，克隆后需按下文构建。旧 `dist` 和 V0.1.4 目录是历史程序，不代表 V0.1.5。

以下命令以源码构建的默认安装目录 `dist` 为例；使用本机新版时替换为 `releases/V0.1.5`。所有输出路径必须不存在：

```powershell
.\dist\bin\geomodelbridge.exe --version
.\dist\bin\geomodelbridge.exe doctor
.\dist\bin\native-filegdb\GeoModelBridge.NativeWriter.exe --probe
.\dist\bin\geomodelbridge.exe inspect model.fbx --report new-inspection.json
.\dist\bin\geomodelbridge.exe prepare model.fbx --output new-bundle
.\dist\bin\geomodelbridge.exe convert model.fbx --output new-model.gdb --profile gis-static --wkid 32650 --origin 500000 3000000 100
```

`--backend native-filegdb` 可省略；其他后端会明确拒绝。原生可执行程序路径可用 `--writer` 或 `GMB_NATIVE_WRITER` 指定，不支持托管 DLL 写入端。`doctor` 查看组件路径，`--probe` 实际检查 FileGDB 运行库及坐标系目录。

输出含 `Models` Multipatch 要素类与旁置 `.report.json`。程序拒绝覆盖已有 FBX、贴图、GDB 和报告。已有 Scene Bundle 也可直接交给原生写入端，复制 GDB 后可只凭原报告进行新进程核验，见 [原生后端说明](backends/native-filegdb/README.md)。

## 转换边界

- 保留常规漫反射颜色、PNG/JPEG 贴图、Alpha、UV 与面片材质绑定，保留 UV/法线角点边界；节点变换只烘焙一次。
- CLI 默认 `strict`，GUI 默认明确展示 `gis-static`。GIS 静态策略可使用文件保存姿态、省略环境光/高光/反射效果、删除有限零面积三角形，每项处理均记录。
- GIS 静态策略仅为可保守识别的 Adobe YCbCr 基线 JPEG 补 JFIF APP0，保留原压缩数据与处理前后散列；不静默转码。
- 缺贴图、无效 UV、非有限几何、未知材质、PBR、自发光、骨骼与形变仍拒绝；没有自动纹理烘焙。
- 目标 RGB 为 8 位、透明度为整百分比、UV 为单精度，法线按 FileGDB 编码量化；报告保留精度与损失说明。[兼容策略](docs/compatibility.md)

**WKID 是空间参考赋值，origin 是归一化后的坐标平移，不能代替地理配准或重投影。** 上述定位数值只是测试示例，需替换为模型实际参数。原生后端要求米制投影坐标系且 WKID 在所用 SDK 的目录中。

## 从源码构建

内核需要 CMake ≥3.21、C/C++17 编译器、Ninja 和 Python 3。依赖源码已固定随工程提供。原生写入端另需 MSVC x64、Windows SDK 和 [官方 FileGDB API 1.5.5 Windows VS2022 SDK](https://github.com/Esri/file-geodatabase-api/tree/master/FileGDB_API_1.5.5)；来源与 SHA-256 见 [sdk-sources.json](backends/native-filegdb/sdk-sources.json)。GUI 需 .NET 8 或更高 SDK。可用 `python scripts/fetch_filegdb_sdk.py --output build/filegdb-sdk` 下载并核对固定 SDK 到新目录；不会自动安装开发工具。

在 x64 Visual Studio Developer PowerShell 中运行：

```powershell
.\scripts\build.ps1 -WithNative -WithGui -FileGDBApiRoot C:\SDKs\FileGDB_API -IncludeFileGDBRuntime -InstallDirectory .\releases\V0.1.5
python scripts/check_version.py
python scripts/verify_dependencies.py --install-dir releases/V0.1.5
python backends/native-filegdb/tests/integration.py --writer releases/V0.1.5/bin/native-filegdb/GeoModelBridge.NativeWriter.exe --work artifacts/native-tests
# GUI 测试工作目录必须不存在。
dotnet run --project apps/GeoModelBridge.Gui.Tests -c Release -- --work artifacts/gui-tests --engine-dir releases/V0.1.5/bin --fixtures tests/fixtures
python scripts/generate_examples.py --cli releases/V0.1.5/bin/geomodelbridge.exe --output examples/V0.1.5
python scripts/package.py --install-dir releases/V0.1.5 --check-only
```

`-IncludeFileGDBRuntime` 只附带官方 release DLL、原始许可与来源记录，不包含编译器、完整 SDK 或调试库。不带该开关时，运行前须让 SDK `bin64` 在子进程 PATH 中可用。安装目录若包含已移除后端，构建脚本要求改用新目录，保留旧版文件。

仅检查/准备链路可使用跨平台 CMake；当前原生 GDB 写入端为 Windows x64：

```text
cmake --preset release
cmake --build --preset release
ctest --preset release
```

## 验证与历史

[V0.1.5 验证记录](docs/validation-v0.1.5.md) 区分本机测试、干净客户机验收和显示验收。自动回读通过不能替代三维外观验收；历史完整色彩、Alpha 和接缝显示待办未因本次移除后端而自动通过。[验收边界](docs/acceptance.md)

V0.1.0–V0.1.4 的原始报告和截图保留其版本与实际验证来源，其中出现的旧后端仅代表历史。相关实现可从初始化提交查看，已从当前工程删除；不属于当前构建、测试或发布要求。[历史证据说明](docs/evidence/README.md)

仓库只在 **master** 上直接提交，不创建其他分支。版本以 `VERSION` 为准，变更见 [CHANGELOG](CHANGELOG.md)，第三方来源见 [THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES.md)。项目自身尚未选择开源许可证。
