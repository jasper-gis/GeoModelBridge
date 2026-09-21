# Native FileGDB 后端 · V0.1.11

这个 Windows x64 / Ubuntu 24.04 x86_64 C++17 后端把 Scene Bundle 中的几何、RGB、透明度、UV 和 PNG/JPEG 纹理直接写入新 FileGDB Multipatch。转换时不加载 ArcGIS Pro、不调用 ArcPy、不借用 Pro 导出的 Shape Buffer。第三方 Esri FileGDB API 负责数据库文件格式，项目代码按 Esri 公开文档独立生成扩展 Multipatch Shape Buffer。

## Linux 构建与运行

从项目根目录执行 `python3 scripts/fetch_filegdb_sdk.py --output build/filegdb-sdk`，随后 `python3 scripts/build.py --sdk build/filegdb-sdk --include-runtime`。需要 Ubuntu 24.04 x86_64、GCC 13、CMake、Ninja、Python ≥ 3.11、libpng-dev、libjpeg-dev；具体 apt 命令见[主 README](../../README.md)。固定 Linux SDK 来源和散列见 [sdk-sources-linux.json](sdk-sources-linux.json)。

Linux 程序名为 `dist/bin/native-filegdb/GeoModelBridge.NativeWriter`，没有 `.exe`，参数与下文 Windows 命令完全相同。它通过 `$ORIGIN` 加载同目录的 `libFileGDBAPI.so` 和 `libfgdbunixrtl.so`。后者为 LGPL 2.1 的上游 Unix 运行库，完整原始许可、README 和来源清单随安装保留。PNG 使用 libpng 解码为直通 RGBA8，libjpeg 检查 JPEG 扫描流但不重新编码。无需 .NET 或显示服务。

平台差异仅在 `platform_*.cpp`（路径/UTF/文件提交）及 `images_*.cpp`（解码），其余原生逻辑共用。Linux 按大小写判断路径范围，拒绝符号链接，以 `renameat2(RENAME_NOREPLACE)` 提交新文件；不回退到会覆盖目标的 rename。GUI 仅 Windows。

## Windows 运行

Windows 完整安装可附带 `dist/bin/native-filegdb/GeoModelBridge.NativeWriter.exe` 和官方 release `FileGDBAPI.dll`。还需要 Windows 11 x64 与 Microsoft Visual C++ x64 Runtime；验证机器已有该运行库。Esri SDK 原许可、README 和 SHA256 清单位于 `dist/licenses/filegdb-api`。本工程不包含 MSVC 编译器或 ArcGIS Pro 运行库。

普通 FBX 转换使用主 CLI：

```powershell
.\dist\bin\geomodelbridge.exe convert .\model.fbx --output .\new-model.gdb --backend native-filegdb --wkid 32650 --origin 500000 3000000 100
```

已有 Scene Bundle 可以直接调用：

```powershell
.\dist\bin\native-filegdb\GeoModelBridge.NativeWriter.exe --input .\bundle --output .\new-model.gdb --feature-class Models --report .\new-model.json
```

关闭 GDB 后复制到其他目录，并用原始成功报告在新进程中验证；此模式不需要 FBX、Bundle 或源图片：

```powershell
.\dist\bin\native-filegdb\GeoModelBridge.NativeWriter.exe --verify-gdb .\copied.gdb --expected-report .\new-model.json --report .\copy-check.json
```

`--probe` 检查本进程能否加载 FileGDB API 和查询其坐标系目录。所有输出都必须是新路径；写入、回读和报告核验成功后才提交最终 GDB。失败只清理本次创建的临时目录，已有模型与数据库不会被覆盖。

## 构建

从 [Esri 官方仓库](https://github.com/Esri/file-geodatabase-api/tree/master/FileGDB_API_1.5.5) 下载 Windows VS2022 x64 SDK。固定下载地址、版本及 SHA256 见 [sdk-sources.json](sdk-sources.json)。本次验证使用 SDK 1.5.5.330、MSVC 14.44 和 Windows SDK 10.0.22621.0。

在 MSVC x64 开发环境中执行：

```powershell
cmake -S backends/native-filegdb -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Release -DFILEGDB_API_ROOT=C:/SDK/FileGDB_API -DGMB_INSTALL_FILEGDB_RUNTIME=ON
cmake --build build/native
cmake --install build/native --prefix dist
```

`GMB_INSTALL_FILEGDB_RUNTIME` 默认 `OFF`。设为 `ON` 时仅安装官方 release `FileGDBAPI.dll`、完整 Apache 2.0 许可、SDK 的 sample use restrictions、README 及固定来源清单，不安装 debug DLL、PDB、.NET wrapper 或 SDK 开发文件。保持 `OFF` 时，运行前把自己的 `FILEGDB_API_ROOT/bin64` 加入 `PATH`。

Windows 后端必须用 MSVC ABI 编译。主 C++ 引擎可继续用 MinGW；两者通过独立进程和 Scene Bundle 协议通信，不混用 C++ STL ABI。

Windows 原生后端只链接 release `FileGDBAPI.lib`，支持 `Release`、`RelWithDebInfo` 和 `MinSizeRel`，固定 `/MD` 与 `_ITERATOR_DEBUG_LEVEL=0`。`Debug` 会在配置阶段拒绝，以免 `/MDd` 或调试迭代器布局跨越 SDK 的 STL ABI。主引擎的 Debug 构建不受这个限制影响。

## 存储规则与范围

- 一个源 mesh 对应一个 feature，按首次出现的材质顺序分组为 `Triangles` patch。角点不按位置焊接；UV 与法线边界保留。
- 源坐标必须已经统一 Z-up、米、右手系，并有显式投影米制 WKID 与 origin。只赋坐标系，不再乘节点矩阵、重复平移或重投影。坐标每个分量须落在 ±99,999,999 米内；XY/Z 分辨率为 0.00001 米，回读容差为 0.00002 米。
- RGB 四舍五入到 8 位；opacity 换算为整数百分比 transparency。法线先 float32，再由实测 FileGDB 1.5.5 存为 `floor(float32(n)*128+0.5)/128`，逐分量核验并报告最大分量/角度误差；不宣称无损法线。
- PNG 经 Windows WIC 或 Linux libpng 解码为未预乘 RGBA8，保持透明像素的 RGB，不执行 ICC 色彩转换；JPEG 保留原压缩字节。拒绝 16-bit/动画 PNG 与 CMYK/YCCK/高位深 JPEG。图片每轴不超过 16384、解码像素不超过 256 MiB。
- V0.1.3 的上游 FBX reader 可在 GIS 策略下对明确安全的缺 JFIF Adobe YCbCr JPEG 补封装，并记录 `JPEG_CONTAINER_NORMALIZED` 及源/目标散列。此 writer 保存收到的 bundle 图片字节，不重复修复、不重新压缩；源图片不变。
- FBX 的 V=0 对应图片底部；Esri 文档定义 t=0 对应存储图片首行。PNG 解码行和 JPEG 首行均从顶部开始，因此**所有有 UV 的 patch 都写入 U′=U、V′=1−V**，包括当前无贴图的 UV。源 Bundle 不改写，报告记录转换策略，PNG 与 JPEG 用相同规则。
- 每个 mesh 最多 1000 万源顶点/展开角点，单 Shape Buffer 上限 512 MiB。未支持的字段、混合缺失的法线、同材质 patch 内混合缺失的 UV、非法索引、图片散列不符与路径逃逸均拒绝。
- V0.1.7 的 `missing_texture_policy` 随 Bundle 带入报告。缺图回退在 FBX Reader 完成：无图片材质保留颜色和标量透明度，writer 按普通无贴图 patch 写入并回读。已声明的 Bundle 资源若丢失仍拒绝；该策略不会绕过图片散列或几何检查。

## 技术依据与验证

编码依据是官方 SDK 内 `doc/html/extended_shape_buffer_format.pdf`，标题 *Extended Shape Buffer Format*，2012-06-20。第 4 页定义 part descriptors，第 10 页定义 normal/UV，第 11–12 页完整定义 MaterialBlock、RGBA/JPEG、transparency/culling 与纹理行方向，第 16 页给出完整 general multipatch record。固定文档 SHA256：`7dc12913a7c20c25b81d8d23e3232118c8a432359d07c8f402a3af9bacaeb90e`。

原生写入后关闭并重开数据库，核验几何、patch 材质绑定、UV、法线量化、RGB/透明度/culling、纹理字节/尺寸/格式及空间参考。成功状态为 `written_and_readback_verified`，`backend=native-filegdb`、`verification.level=closed_reopened_file_geodatabase`。独立复制验证对逐 feature 回读 Shape Buffer SHA256、属性和 WKID 做精确比对。

V0.1.3 报告还记录输入 bundle 的 `conversion_profile` 和兼容处理标记；writer 不会再次求值动画或执行材质烘焙。以下为 V0.1.3 历史证据，不属于当前运行或发布要求：原生自回读与独立 SDK 读取是不同验收。当时的用户模型补充 3 张 JPEG 的 JFIF 标记后，本后端写入与自回读 56 个要素通过，原生成果经独立 Pro 对照 bundle 核验 69,936 个三角形及 12 张贴图通过。目标软件外观验收仍需另行完成。

原生坐标分辨率为 `1e-5` 米，本机 Pro 后端为 `1e-4` 米；该模型两份成果的 XYZ 最大差约 `5.00083e-5` 米，UV、法线、材质与纹理一致。因此不同 writer 产生的精确 signature 不同，不能使用跨 writer signature 匹配来宣称两份成果逐位相等。同一份原生成果的复制核验仍严格检查其原生报告。

运行原生集成测试：

```powershell
python backends/native-filegdb/tests/integration.py --writer dist/bin/native-filegdb/GeoModelBridge.NativeWriter.exe --work build/native-integration
```

该测试覆盖低 alpha/透明 RGB、JPEG 原字节、非轴法线和正负半格舍入、颜色/透明度量化、UV 接缝、多材质、无 UV/无法线的白色或纯色几何、源文件不可用时的复制验证，以及坏输入、整数溢出、路径、不覆盖、SDK 失败清理和假散列检查。V0.1.3 新增未知转换策略拒绝检查。它使用 Python 标准库构造输入，数据库写入和回读均由本 C++ 后端完成；通过数量以对应版本验证记录为准。

V0.1.1 历史交付样例在 `examples/V0.1.1/native`，保留原生成版本与报告。ArcGIS Pro 独立核验与目标软件显示验收由主工程验证记录另行列出；原生回读成功本身不替代图形显示验收。

V0.1.8 支持最大 16 GiB Scene JSON 的流式读取，逐个释放角点、三角形和网格 JSON。单网格 1000 万角点、单 Shape Buffer 512 MiB 的边界保持不变。完整元数据、几何、资源和坐标域校验通过后才创建 GDB；根字段和网格字段重复会明确拒绝。法线兼容修复属于上游 GIS 静态策略，writer 只接受有效单位法线。
