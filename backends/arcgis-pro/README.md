# ArcGIS Pro CoreHost writer · V0.1.4

此可选后端将 Scene Bundle v1 写为真实 FileGDB Multipatch。它使用本机安装、已授权的 ArcGIS Pro 的公开 `ArcGIS.Core` API，与独立的 `native-filegdb` 后端分开构建和调用。没有复制、再分发 Esri DLL，也不修改 Pro 许可配置。V0.1.1 的回归和显示状态见 [历史验证记录](../../docs/validation-v0.1.1.md)，首版 [TEST_RESULTS](TEST_RESULTS.md) 保持历史原样。V0.1.3 继续由 GUI 或 CLI 调用，并在报告中记录输入 bundle 的转换策略与兼容处理。

本次用户模型原有 3 张 Adobe JPEG 缺少兼容封装，曾导致 Pro 拒绝。V0.1.3 的 GIS 策略补充 JFIF 标记后，本 Pro 后端写入并自回读 56 个要素通过；原生后端对应成果也通过独立 Pro 按 bundle 读取核验。该结论不代替目标软件的颜色、Alpha 与接缝外观验收。

## 环境与构建

- Windows x64；已安装且已授权的 ArcGIS Pro。V0.1.0 已验证 3.6.2；其他 Pro 版本需重新验收。
- .NET SDK 8 或以上。已实测：Pro 3.6.2.59530，SDK 10.0.203，.NET Runtime 8.0.26。
- 框架须与本机 Pro 安装版本匹配。本版目标为 `net8.0-windows`，不宣称已验证其他 Pro 版本。

```powershell
dotnet build .\backends\arcgis-pro\GeoModelBridge.ArcGISPro.csproj -c Release
dotnet publish .\backends\arcgis-pro\GeoModelBridge.ArcGISPro.csproj -c Release --no-self-contained -o .\bin\arcgis-pro
.\bin\arcgis-pro\GeoModelBridge.ProWriter.exe --probe
```

非默认安装路径，构建时加 `/p:ArcGISProInstallDir="D:\ArcGIS\Pro"`，运行时设置 `ARCGIS_PRO_INSTALL_DIR`。运行时首先使用该环境变量，其次查已安装 Pro 的注册表位置。不要把 Esri DLL 拷贝进发行目录。

## 写入

```powershell
.\bin\arcgis-pro\GeoModelBridge.ProWriter.exe `
  --input C:\data\building-bundle `
  --output C:\data\building.gdb `
  --feature-class Models `
  --report C:\data\building.writer-report.json
```

省略 `--report` 时使用与 GDB 同目录的 `<名称>.writer-report.json`。输入 bundle、GDB 和报告必须分开；报告不能放进 GDB，两个成果都不能放进输入 bundle。现有文件/目录不覆盖。成功退出码 0；任何验证失败退出码 1，标准错误包含 `status: failed` 和原因。成功标准输出及报告的 `status` 为 `written_and_readback_verified`。

要求 bundle 明确给出 `space: referenced`、`origin_explicit: true`、正 WKID，并且该 WKID 是以米为水平单位的投影坐标系。经纬度、英尺、本地未知坐标系被拒绝。坐标在 reader 已变换并加入 origin，writer 不再乘矩阵、平移、重投影或改变高程基准。

每个 mesh 对应一个要素，每个材质组对应一个 triangle patch。按三角形角点展开可保留 UV 接缝、硬边和不同法线，不焊接。未绑定材质的 patch 显式使用白色双面材质。一个 mesh 中部分有/部分没有法线，或同一材质 patch 中部分有/部分没有 UV，目前明确拒绝；textured patch 必须具备全部 UV。

## 保真及验证边界

- JPEG：原始压缩字节直接交给 `JPEGTexture`，不重编码。
- JPEG 封装：V0.1.3 FBX reader 在 GIS 策略下可为明确安全的缺 JFIF Adobe YCbCr JPEG 补充标准 APP0；本 writer 接收该 bundle 并保持其字节，不再次转换。归一化诊断记录源图和 bundle 图的散列，不能把封装字节变化描述为图片文件逐位不变。
- UV：所有有 UV 的 patch 写入 `U=sourceU, V=1-sourceV`，以匹配源 FBX 与目标纹理行原点；无纹理材质也采用相同 UV 语义，不翻转图片行序、不改变中间包源 UV。报告 `texture_coordinate_policy` 记录此映射，读回与映射后的值比较。
- PNG：用 WPF 解码为从左上角开始、逐行排列的直通 RGBA8 像素，再使用 `UncompressedTexture`；不丢弃 alpha，不预乘 alpha，不做 ICC 转换。拒绝 16-bit PNG，防止无声降位；PNG 文件容器、ICC 和其他元数据仍只在原 bundle 保留。
- RGB 材质量化为 8-bit；整体不透明度量化为整数百分比透明度。报告记录原始值与写入值。纹理 alpha 和材质整体透明度是两套独立值。
- 法线必须为单位向量（长度误差最多约 `1e-5`），不暗中归一化。在实测 Pro 3.6.2 路径中，builder 保留 float32 法线，FileGDB 将各分量量化到 `1/128` 网格，正负精确半步均朝正无穷舍入，存储后不重新归一化。GDB 内法线不是逐位无损，原值仍在源 bundle；报告保留源角点法线 hash、实际最大分量误差、角度误差与长度误差。
- 只接受 PNG/JPEG，核对文件长度、SHA256、签名与声明格式。图片最大 256 MiB，解码最大单轴 16384 像素/256 MiB。相对路径越界和 bundle 内重解析点被拒绝。
- 拒绝动画 PNG、CMYK/YCCK JPEG、高于 8-bit 的 JPEG。节点索引、父子循环、矩阵、三角索引、UV、法线及颜色范围也会验证。
- 临时 GDB 建在目标旁边，完整关闭后重新打开。验证要素数、属性、类型、WKID、patch 类型/数量、角点数/坐标、材质数/颜色/透明度/双面、UV 数量/数值、法线、纹理绑定/尺寸/编码/全部字节。数据库网格坐标允许 2 倍空间参考分辨率误差，UV 允许 float 存储量化误差。法线单独验证：builder 值须匹配 `float32(source)`，GDB 回读须逐分量**精确匹配** `floor(builder*128+0.5)/128`，而非笼统放宽浮点容差。
- 全部回读通过才将临时 GDB 移到最终名称，并落盘报告。失败仅清理本次随机名称的临时文件；已存在的用户成果不删除。

报告验证级别为 `closed_reopened_file_geodatabase`。`graphical_acceptance` 始终是 `pending_manual_review_in_target_software`：SDK 数值回读不等同于 ArcGIS Pro/GeoScene 显示验收。UV 朝向、颜色空间、透明排序以及软件版本显示兼容性仍须打开五组诊断模型人工检查。

## 脱离源文件的复制验证

原报告保存每个 patch 的材质绑定、RGB、透明度、纹理 SHA256/尺寸/编码，以及 GDB 回读后的坐标、UV、法线数值摘要。把已关闭的 `.gdb` 目录复制到另一位置后，可在新进程中验证它；该命令不会读取原 bundle 或源图片。

```powershell
.\bin\arcgis-pro\GeoModelBridge.ProWriter.exe `
  --verify-gdb C:\delivery\building-copy.gdb `
  --expected-report C:\data\building.writer-report.json `
  --report C:\delivery\standalone-verification.json
```

只有全部匹配才报告 `standalone_copy_verified`。原报告必须由具有 `signature` 字段的本 writer 生成。预期报告是内容核验的参照，应与交付记录一起保管；这不是数字签名或来源认证。图形验收状态仍为待验收。

此精确 signature 模式用于核验本 writer 成果的复制。原生与本机 Pro writer 使用不同坐标分辨率（分别 `1e-5` 与 `1e-4` 米），因此两份独立生成的库不保证精确 signature 相同；跨 writer 核验须分别对照源 bundle 与各自的量化规则。本次用户模型中 UV、法线、材质和纹理相同，XYZ 最大差约 `5.00083e-5` 米。

## 集成测试

测试仅用 Python 标准库生成 2×2 PNG（包括透明 RGB、alpha=1、127、255）、小 JPEG 和场景，不依赖原模型；仍需本机 Pro 许可。测试生成随机子目录并保留 GDB/报告证据，不删除既有目录。

```powershell
python .\backends\arcgis-pro\tests\integration.py `
  --writer .\bin\arcgis-pro\GeoModelBridge.ProWriter.exe `
  --work .\work
```

覆盖实际写入/关库回读、RGBA 全字节保持、JPEG 原字节保持、颜色/透明度量化、斜面/均分三轴/正负半步法线、复制后的 GDB 在原 bundle 路径不可用时独立验证；并检查覆盖保护、路径越界、错误散列、格式不匹配、非法颜色/零或非单位法线/UV/索引、非米/地理坐标系、缺少显式 origin、写入失败临时目录清理、独立纹理散列不匹配。

`1/128` 法线规则来自本机 Pro 3.6.2 的 12,265 个方向与临界值实测，是本 adapter 的严格验收模型，不是 Esri 发布的跨版本精度承诺。若其他版本产生不同存储结果，本 writer 明确失败，需要重新研究与验收。

## 官方依据

实际编译接口优先以本机 `ArcGIS.Core.XML` 和 `ArcGIS.CoreHost.XML` 为准，避免使用超出安装版本的接口。

- [Esri CoreHost 概念与部署](https://github.com/Esri/arcgis-pro-sdk/wiki/ProConcepts-CoreHost)：独立应用的 x64、STA、安装/授权要求，`Host.Initialize`，不复制 Pro 程序集。
- [Esri 构建 Multipatch 指南](https://github.com/Esri/arcgis-pro-sdk/wiki/ProGuide-Building-Multipatches)：`MultipatchBuilderEx`、`BasicMaterial`、`TextureResource`、材质分配及 UV。
- [Esri DDL](https://github.com/Esri/arcgis-pro-sdk/wiki/ProConcepts-DDL)：创建 FileGDB 和要素类。
- [Esri FileGDB API 仓库](https://github.com/Esri/file-geodatabase-api)：独立原生后端采用官方 1.5.5 SDK；不能将本 adapter 的通过误写为原生 FileGDB writer 通过。各后端数据、跨后端读取和目标显示分别验收。
