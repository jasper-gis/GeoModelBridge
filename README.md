# GeoModelBridge V0.1.4

**V0.1.4：降低大模型中间包写出的内存占用，并限制大量日志对界面的影响。** 本次可运行版本位于 [`releases/V0.1.4/bin/geomodelbridgeGUI.exe`](releases/V0.1.4/bin/geomodelbridgeGUI.exe)，请保留整个 `releases/V0.1.4` 目录。现有 `dist` 保留 V0.1.3，方便正在使用的旧版继续运行；下文 `dist` 是从源码构建的默认安装位置。实测范围与证据见 [V0.1.4 验证记录](docs/validation-v0.1.4.md)。

面向 GIS 的三维模型格式转换引擎。首期方向为 **静态 FBX → FileGDB Multipatch**，重点保留常规漫反射颜色、UV、面片材质绑定和纹理像素。支持严格检查和显式 GIS 静态兼容策略，所有兼容处理写入报告。

**V0.1.3 修复默认材质参数误报，增加 GIS 静态兼容模式和中文报告摘要。** `geomodelbridgeGUI.exe` 默认显示 GIS 静态兼容策略：按文件保存姿态转换，保留常规颜色、漫反射贴图和 UV；省略环境光、高光、反射渲染效果，移除有限坐标下的零面积三角形，并逐项记录。需要完全严格检查时可切换策略。[策略说明](docs/compatibility.md) 列出具体边界。

V0.1.3 完成 CTest 5/5、转换策略 78/78、GUI 125/125 回归，详见 [本版验证记录](docs/validation-v0.1.3.md)。本次用户模型修复 JPEG 封装后，两个后端各完成 56 个要素的写入与自回读；最终 GUI 原生成果经独立 Pro 读取并对照中间包核验几何、材质、UV、法线和 12 张贴图通过。两后端坐标量化精度不同，不宣称输出逐位一致；三维外观验收仍独立进行。V0.1.2 的测试结果保持在 [历史验证记录](docs/validation-v0.1.2.md)。

V0.1.1 的双后端各 14 例转换、跨后端核验及纹理方向修复记录保留在 [历史验证记录](docs/validation-v0.1.1.md)。该版 Pro 导出仍有红青双影，完整色彩、Alpha 外观及六面/接缝图形验收尚未通过；新增 GUI 不改变这一结论。[图形对照](docs/evidence/V0.1.1/visual/index.html) 保持原始证据。本版以工程目录交付，按要求删除旧 ZIP 及对应校验文件，不生成新 ZIP；历史样例和报告保留原版本。

原生后端需要 FileGDB API 1.5.5 Windows x64 运行库及 Microsoft C++ 运行库，转换不调用 Pro。本交付随程序附带官方 release `FileGDBAPI.dll`、原始许可和来源散列，运行时无需另行下载完整 SDK。Microsoft C++ 运行库须在本机可用。Pro 后端需要本机安装且许可可用的 ArcGIS Pro，Pro 程序集不随工程重新分发。

## 立即使用

双击 **`dist/bin/geomodelbridgeGUI.exe`**，先点“检查运行环境”，再选择 FBX 和新的输出 GDB 路径。默认使用原生 FileGDB 后端与 GIS 静态兼容策略；正确填写目标投影坐标系 WKID、以米计的 X/Y/Z 原点后，点击“开始转换”。报告提供中文摘要与原始 JSON；失败后可点“换个新名称”保留旧报告再重试。成功后可打开输出目录。界面操作见 [GUI 使用说明](docs/gui.md)。

GUI 是 Windows x64 自包含程序，无需另外安装 .NET。请保留整个 `dist` 目录，GUI 旁的命令行程序和后端子目录也是转换所需组件。选择 Pro 后端仍需要本机 ArcGIS Pro 及可用许可。

`dist/bin/geomodelbridge.exe` 是 Windows x64 命令行程序，`dist/bin/arcgis-pro/` 与 `dist/bin/native-filegdb/` 分别为两个后端。以下命令在项目根目录的 PowerShell 中执行；先准备所选后端的运行依赖，输出路径必须不存在。`doctor` 报告程序是否存在，后端 `--probe` 才实际检查依赖。

```powershell
.\dist\bin\geomodelbridge.exe --version
.\dist\bin\geomodelbridge.exe doctor
dotnet .\dist\bin\arcgis-pro\GeoModelBridge.ProWriter.dll --probe

# 检查 FBX，包括缺贴图和不支持材质；不生成 GDB。
.\dist\bin\geomodelbridge.exe inspect .\tests\fixtures\textured_quad.fbx --report .\artifacts\inspection.json

# 针对GIS静态展示，明确启用有记录的兼容处理。
.\dist\bin\geomodelbridge.exe inspect .\tests\fixtures\textured_quad.fbx --profile gis-static --report .\artifacts\gis-inspection.json

# 先以默认严格策略生成可审阅的中间包，贴图保留原始文件字节。
.\dist\bin\geomodelbridge.exe prepare .\tests\fixtures\textured_quad.fbx --output .\artifacts\textured.gmbscene

# 实际写入 GDB；32650 和此原点仅是测试参数，不代表真实模型的位置。
.\dist\bin\geomodelbridge.exe convert .\tests\fixtures\textured_quad.fbx --output .\artifacts\textured.gdb --backend arcgis-pro --wkid 32650 --origin 500000 3000000 100

# 原生后端：本交付已包含 release FileGDBAPI.dll。
.\dist\bin\native-filegdb\GeoModelBridge.NativeWriter.exe --probe
.\dist\bin\geomodelbridge.exe convert .\tests\fixtures\textured_quad.fbx --output .\artifacts\native-textured.gdb --backend native-filegdb --wkid 32650 --origin 500000 3000000 100
```

转换结果有 `Models` Multipatch 要素类，以及 GDB 旁的 `.report.json`。只有写入完成、关闭数据库并重新打开核对通过，命令才报告成功。目标软件中的视觉验收仍独立进行。

CLI 强制核对成功报告的后端、版本、转换策略和要素类。通过 `--writer` 指定旧版程序或不匹配的后端不会被报告为本版成功。打包脚本也验证 Pro 程序的实际二进制版本。

GUI 负责填写参数、启动同目录 CLI 并展示结果；命令行保留用于批处理和其他程序调用。[ArcGIS Pro 图形验收工程](docs/visual-acceptance-setup.md) 用于查看参考成果。

## 从源码构建

需要 CMake ≥3.21、C/C++17 编译器、Ninja 和 Python 3（运行测试）。Windows 可以使用带 C++ 工具链的 Visual Studio Developer PowerShell，或 PATH 中的 MinGW-w64。依赖源码已随工程固定版本提供，C++ 构建无需联网下载第三方库。

```powershell
# C++ 内核、CLI、测试和安装
.\scripts\build.ps1

# 同时发布 Windows x64 自包含 GUI：需要 .NET 8 或更高 SDK。
.\scripts\build.ps1 -WithGui

# 同时构建并探测 Pro 后端：还需要 .NET 8 或更高 SDK、.NET 8 Windows Desktop runtime、ArcGIS Pro。
.\scripts\build.ps1 -WithPro

# 在 MSVC x64 环境中同时构建原生后端并附带官方 release 运行库和许可。
.\scripts\build.ps1 -WithNative -FileGDBApiRoot C:\SDKs\FileGDB_API -IncludeFileGDBRuntime

python .\scripts\check_version.py
```

原生后端单独使用 **MSVC x64** 和 Windows SDK 构建，因为官方 Windows FileGDB SDK 使用 Microsoft C++ ABI。内核仍可使用 MinGW，两者通过进程传递中间包。在已配置 C++ 工具链的 Developer PowerShell 中执行：

```powershell
cmake -S .\backends\native-filegdb -B .\build\native-release -G Ninja -DCMAKE_CXX_COMPILER=cl -DCMAKE_BUILD_TYPE=Release -DFILEGDB_API_ROOT=C:/SDKs/FileGDB_API -DGMB_INSTALL_FILEGDB_RUNTIME=ON
cmake --build .\build\native-release
cmake --install .\build\native-release --prefix .\dist
```

`FILEGDB_API_ROOT` 应包含 `include/FileGDBAPI.h`、`lib64/FileGDBAPI.lib` 和 `bin64/`。源码构建需自行准备 MSVC、Windows SDK 和完整官方 FileGDB SDK。`-IncludeFileGDBRuntime` 或 `GMB_INSTALL_FILEGDB_RUNTIME=ON` 只附带 release DLL、原始许可与来源记录，不包含 SDK 头文件、导入库、调试库或编译器。关闭该选项时，运行程序前应将自己的 SDK `bin64` 加入 `PATH`。原生后端说明见 [native-filegdb](backends/native-filegdb/README.md)。

也可使用标准 CMake；Linux/macOS 仅提供 C++ 准备和检查链路，本次没有在这些系统上实测。

```text
cmake --preset release
cmake --build --preset release
ctest --preset release
```

Pro 适配器的构建属性是 `ArcGISProInstallDir`，运行路径覆盖变量是 `ARCGIS_PRO_INSTALL_DIR`。它只引用本机 Pro DLL，交付包不包含 Pro 程序集。CLI 可通过 `--writer <程序路径>` 覆盖所选后端，也可分别用 `GMB_PRO_WRITER` 或 `GMB_NATIVE_WRITER` 指定路径。

## V0.1.3 能力边界

| 内容 | 本版行为 |
|---|---|
| 常规静态多边形 FBX | 三角化；节点/几何变换烘焙；实例分别保留材质绑定 |
| 单位与坐标轴 | 统一为右手 Z-up、米；镜像调整面绕序，法线使用逆转置变换 |
| 漫反射纯色和标量透明度 | 保留到统一材质；GDB RGB 为 8 位、透明度为整百分比，报告列出源值和存储值 |
| 外置和内嵌 PNG/JPEG | PNG 保留源文件进入中间包，写库时解码为未预乘 RGBA8 并保留 Alpha；兼容 JPEG 原字节写入，特定缺 JFIF 的 Adobe YCbCr JPEG 可按 GIS 策略仅补封装并记录，原压缩数据不重编码 |
| 多材质/多贴图、UV 接缝/硬法线 | 按完整角点与三角形材质保存，按材质分 Multipatch patch；GDB 法线分量有 1/128 量化，报告记录方向误差 |
| UV 集与 UV 变换 | 按所选贴图的 UV 集提取并烘焙 UV 变换；支持 repeat wrap；未采样的额外 UV 集不保留并记录警告 |
| 目标纹理方向 | 所有有 UV 的 patch 写入 `U=sourceU, V=1-sourceV`，无纹理材质也采用相同 UV 语义；不改变中间包源 UV，不翻转图片行序 |
| 中性默认反射/位移参数 | 对明确为零且无活动连接的标准默认参数不再误报 |
| 环境光、高光、反射 | 严格模式拒绝活动通道；GIS 静态兼容模式省略这些效果并按材质记录，保留漫反射颜色/纹理 |
| 动画标记与曲线 | 空动画容器不阻断；真实曲线严格模式拒绝，GIS 模式采用文件保存的静态姿态并记录，不采样时间帧 |
| 有限坐标下的零面积三角形 | 严格模式拒绝；GIS 模式删除并按网格汇总数量；非有限值与其他几何错误仍拒绝 |
| JPEG 封装兼容 | 仅对缺少 JFIF、明确为 8 位三分量 Adobe YCbCr 基线 且 EXIF 方向缺省或为 1 的图片，GIS 模式补 18 字节 JFIF APP0；严格模式提示需修复，未知颜色空间或方向不自动处理 |
| 顶点颜色、PBR、自定义 shader、自发光、法线、活动位移等未知渲染内容 | 仍拒绝，无纹理烘焙或无条件忽略错误 |
| 骨骼、形变、细分、曲线几何 | 仍拒绝；请先导出显式静态网格 |
| PNG 16 位、非 PNG/JPEG、缺贴图或无效 UV | 拒绝；图片完整解码及位深检查由所选后端执行 |
| 纯 C++ 直写 FileGDB | V0.1.1 的 14 例直写/重开读回及独立 Pro 精确比对通过；完整图形显示的未通过项单独记录 |
| Windows 图形界面 | 中文单模型转换、环境检查、演示参数、日志和成果报告；不提供模型预览或自动配准 |

`inspect` 与 `prepare` 的成功只表示所选策略下的几何/材质预检或中间包准备通过，不表示 PNG/JPEG 已完整解码，更不表示 GDB 已完成。CLI 默认 `--profile strict`，GUI 默认显式展示 `gis-static` 策略。GDB 报告记录策略、兼容处理、数据库读回、颜色量化、坐标精度及待完成的显示验收。

外置图片默认只在 FBX 所在目录及其子目录内解析，可重复提供 `--texture-dir <目录>` 授权其他图片目录。不联网获取纹理。缺材质的白模会赋默认白色并记录警告；已有但不支持的材质不会替换成白色。
预乘 Alpha、旧式像素裁切、特殊纹理混合、分层材质和未支持的节点剔除行为也会被拒绝。GIS 策略省略的光照效果不属于 GDB 的外观保真承诺。

## 坐标和验收

`--wkid` 是空间参考赋值，`--origin X Y Z` 是在归一化后的坐标上平移。**这两个操作不能代替地理配准、旋转对齐或重投影。** 写 GDB 必须显式指定二者，目标 WKID 必须为以米为单位的投影坐标系。原生后端还要求 WKID 存在于所用 FileGDB API 的空间参考目录，并限制数值域在其支持范围。无定位的中间包保持本地坐标，不默认 WGS84。

运行 `fixture all --output <新目录>` 可生成六色立方体、UV 方向平面、混合材质、透明镂空和接缝硬边五组人工构造样本。[验收说明](docs/acceptance.md) 区分数据读回、成果独立交付和图形显示验收；[V0.1.1 验证记录](docs/validation-v0.1.1.md) 和 [V0.1.0 历史记录](docs/validation-results.md) 保持原样。

完整 14 例参考成果可用同一脚本按后端重建；下列输出目录必须尚不存在，Pro 方案需要本机 Pro 许可：

```powershell
python .\scripts\generate_examples.py --backend native-filegdb --output .\artifacts\native-examples-V0.1.3
python .\scripts\generate_examples.py --backend arcgis-pro --output .\artifacts\pro-examples-V0.1.3
```

## 工程结构

```text
include/gmb/          稳定的统一 Scene、网格、材质、纹理和诊断类型
src/                 C++ 内核、ufbx Reader、Scene Bundle、CLI、人工测试网格
backends/arcgis-pro/  独立 .NET 8 CoreHost 写入和读回验证后端
backends/native-filegdb/ 独立 MSVC / FileGDB API 写入和读回验证后端
apps/GeoModelBridge.Gui/ Windows WPF 中文图形界面
apps/GeoModelBridge.Gui.Core/ 图形界面的参数校验与进程调用逻辑
tests/               核心/命令行/FBX 回归测试、可追溯自造样本
third_party/         固定版本 ufbx、nlohmann/json、原许可和 SHA-256 清单
scripts/             构建、依赖完整性校验、Pro 集成测试
docs/                架构、格式契约、验收、路线图、验证记录
dist/                已构建程序（构建产物，不进入源码版本控制）
examples/            本次生成的参考成果与报告
```

版本号见 `VERSION` 和 [CHANGELOG](CHANGELOG.md)。初始交付为 **V0.1.0**；本次内存与日志处理小迭代为 **V0.1.4**。旧版成果与历史验证证据不会批量改写为新版本。

依赖来源及许可见 [THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES.md)。项目自身尚未选择对外开源许可证。
