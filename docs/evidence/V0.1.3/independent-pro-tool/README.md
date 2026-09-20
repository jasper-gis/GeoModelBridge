# V0.1.3 独立 ArcGIS Pro 回读验证工具

这是一份本次验证所用的只读工具源码，便于复核证据，不属于正式转换功能。它需要 Windows、.NET 8 和已安装且许可可用的 ArcGIS Pro（本次使用 3.6.2）；不随源码分发 Esri 程序集。该目录不含用户模型、纹理、Scene Bundle 或编译产物。

工具不创建或修改 GDB。它只打开现有 GDB、读取 Scene Bundle 和贴图，并向一个不存在的新路径写 JSON 报告。ArcGIS 读取 GDB 时可能自行创建短暂锁文件。请保留输入原件并使用可写的新报告位置。

## 构建

在本目录运行：

```powershell
dotnet build GeoModelBridge.ReadbackEvidence.csproj -c Release -o C:\NEW_WORK\reader
```

默认 Pro 安装路径为 `C:\Program Files\ArcGIS\Pro`；其他路径构建时传 `/p:ArcGISProInstallDir=...`，运行时设置 `ARCGIS_PRO_INSTALL_DIR`。

## 从另一套 SDK 独立核验 native GDB

```powershell
C:\NEW_WORK\reader\GeoModelBridge.ReadbackEvidence.exe C:\EXISTING\bundle C:\EXISTING\native.gdb C:\NEW_WORK\readback.json
```

`bundle` 应为相同参数和 V0.1.3 reader 生成的 Scene Bundle；真实模型验证参数为 `--profile gis-static --wkid 3857 --origin 0 0 0`。该 CRS/原点只用于验证流程，不代表模型真实地理定位。

验证实际逐行读取所有要素和 Multipatch Shape，并对照独立 Scene Bundle 检查：网格属性、要素数量、三角面/材质分组、位置、UV、法线量化、RGB8/透明度/剔除设置、贴图绑定、尺寸、压缩类型和全部贴图字节。位置容差为目标空间参考 XY 分辨率的两倍、ZScale 倒数的两倍（最低 1e-9）；UV 容差为 `max(1, abs(expected))*2e-6`；法线按现有 1/128 codec 预测精确比较。JPEG 使用 reader 已明确标准化后的容器字节，PNG 使用 WIC 解码的 RGBA8，逐字节比对。

这份核验不是原始 FBX 外观的视觉验收，也不把不同 GDB 坐标精度造成的差异隐藏为二进制哈希相等。

## 定位两套后端的数值差异

```powershell
C:\NEW_WORK\reader\GeoModelBridge.ReadbackEvidence.exe --compare C:\EXISTING\pro.gdb C:\EXISTING\native.gdb C:\NEW_WORK\differences.json
```

它统计 XYZ、法线 XYZ 和 UV 的数值及 binary64 差异。本次 native 的 1e-5 米坐标分辨率与 Pro 默认的 1e-4 米不同，因此跨后端位置哈希并不相同；UV、法线以及材质和贴图分别由上述核验/原 signature 证据检查。此诊断程序不会修改正式 `--verify-gdb` 的精确签名判定。

## JPEG 补头的逐像素等价检查

```powershell
C:\NEW_WORK\reader\GeoModelBridge.ReadbackEvidence.exe --pixels C:\EXISTING\original_bundle C:\EXISTING\normalized_bundle C:\NEW_WORK\pixels.json
```

检查所有 JPEG：若容器变化，必须只是在 SOI 后插入 18 字节，其余全部原字节按原顺序保留；前后 WIC 解码的 Bgra32 图像尺寸和全部像素 SHA-256 必须完全一致。解码不应用 ICC 转换或 EXIF 方向变换；reader 已在补头之前拒绝非默认方向及不明确的颜色空间/像素布局。
