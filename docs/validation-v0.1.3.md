# V0.1.3 验证记录

日期：2026-09-20。本次修复用户 FBX 的默认材质误报，增加显式 GIS 静态兼容策略、安全 JPEG 封装修复及中文报告。最终 Windows GUI、CLI 和两个后端版本均为 0.1.3。

## 真实模型与最终成果

输入为用户原始 `ZT-01(1).FBX`，8,920,976 字节。旧版检查产生 119 条错误，其中 87 条是中性默认材质参数误报；真实动画、活动光照通道及零面积面则需要明确处理策略。[旧版重现](evidence/V0.1.3/baseline-v012.json)、[最终严格检查](evidence/V0.1.3/final-strict-inspection.json) 保留完整诊断。

最终发布的 GUI 使用默认 GIS 静态兼容、原生后端，沿用原失败设置 WKID 3857、原点 0/0/0，实际点击转换成功，界面记录约 **4.4 秒**。生成 [ZT-01(1).gdb](../results/V0.1.3/ZT-01(1).gdb) 和 [完整报告](../results/V0.1.3/ZT-01(1).gdb.report.json)。坐标设置仅用于复现原操作，没有验证模型的真实地理位置。

| 内容 | 最终结果 |
|---|---|
| 要素 / 三角形 / 角点 | 56 / 69,936 / 209,808 |
| 材质 / 贴图 | 30 / 12 |
| 诊断 | 0 个错误，26 条兼容警告 |
| 动画 | 使用文件保存的静态姿态，1 条记录；不采样指定帧 |
| 光照效果 | 省略 4 个环境光、15 个高光、2 个反射通道，逐材质记录 |
| 退化面 | 删除严格零面积三角形 6 个，按网格汇总 1 条记录 |
| JPEG | 3 张安全识别的图片各插入 18 字节 JFIF 标识；不重新压缩 |

在自回读之外，独立 Pro CoreHost 工具实际读取最终 GUI 成果的全部 56 个要素，对照源中间包逐项核验坐标、UV、法线、材质绑定、颜色/透明度、12 张纹理及其尺寸通过。[最终成果独立读取报告](evidence/V0.1.3/gui-result-independent-pro.json) 保留检查精度和每个网格结果。此工具仅用于验收，原生转换不需要调用 Pro。

独立构建的 Pro 写入后端也完成本模型的 56 个要素写入和重开核验，见 [Pro 报告](evidence/V0.1.3/real-pro-report.json)。

## JPEG 原因与保真检查

本模型的 3 张 Exif/Adobe YCbCr JPEG 可被 Windows 解码，但 ArcGIS Pro 拒绝缺少前置 JFIF 标识的封装，导致原生 GDB 也无法在 Pro 中读取 Shape。仅补充 JFIF APP0 后，Pro 可读取。

修复仅在显式 GIS 策略中执行，限于经过颜色空间、baseline 分量、Exif 方向、像素比例与色度定位检查的安全情况；不修改源文件，不移动已有标识，不猜测未知颜色空间。扫描头截断、空扫描及缺少结束标记会拒绝，完整解码仍由后端检查。

[像素等价报告](evidence/V0.1.3/jpeg-normalization-pixels.json) 验证全部 5 张 JPEG 的 WIC BGRA32 像素散列：处理前后完全一致；3 张被修复的图片仅增加 18 字节，原图像压缩流及其余元数据字节全部保留。其余 2 张 JPEG 原样保留。该证明采用报告中注明的解码方式，不等同于跨软件色彩管理显示完全一致。

## 自动回归与界面验证

| 检查 | 结果 | 证据 |
|---|---|---|
| C++ 核心、CLI、FBX 读取 | CTest 5/5 通过 | [日志](evidence/V0.1.3/core-tests.log) |
| 转换策略 | 78/78，包括 JPEG 安全/拒绝边界及极小非零面保护 | [逐项结果](evidence/V0.1.3/profile-tests.txt) |
| GUI 参数、真实报告核验与转换服务 | 125/125，含 3 项真实原生集成 | [结果](evidence/V0.1.3/gui-tests.json) |
| 原生后端 | 实际写入、独立副本读回及 30 项错误输入/覆盖/清理检查通过 | [结果](evidence/V0.1.3/native-tests.json) |
| Pro 后端 | 实际写入、独立副本读回及 20 项错误输入/覆盖/清理检查通过 | [结果](evidence/V0.1.3/pro-tests.json) |
| 源码版本与依赖 | 12 处版本声明一致，固定依赖与 FileGDB 运行库散列通过 | `scripts/check_version.py`、`scripts/verify_dependencies.py` |

实际窗口操作验证了默认策略说明、原生运行条件、最终模型转换、中文摘要和原始 JSON 页签，以及重复转换拒绝覆盖。[成功截图](evidence/V0.1.3/gui-real-success.png)、[中文报告](evidence/V0.1.3/gui-readable-report.png)、[原始报告页](evidence/V0.1.3/gui-raw-report.png)、[防覆盖截图](evidence/V0.1.3/gui-no-overwrite.png) 保留证据。重复点击后，[GDB 和报告全部散列保持不变](evidence/V0.1.3/gui-no-overwrite-hashes.json)。

本轮较早的界面检查还验证了严格策略失败保留报告，以及“换个新名称”生成未占用路径、不会自动转换；对应 [严格失败截图](evidence/V0.1.3/gui-strict-failure.png) 与 [重试截图](evidence/V0.1.3/gui-retry-new-name.png) 拍摄于最后 JPEG 诊断翻译完善前，最终严格诊断以本页链接的 JSON 为准。

最终 GUI 是 Windows x64 自包含 EXE，71,742,246 字节，产品版本 0.1.3。SHA-256：`53b2c85f1040509879e5ff79abd3be52292aeb3cd677e37158a3a4e79385a9b2`。请保留完整 `dist` 目录中的 CLI、后端和许可。本版没有生成 ZIP。

## 精度与验收边界

- 两个写入后端使用既有的不同坐标量化网格：原生分辨率为 0.00001 米，Pro 为 0.0001 米。跨后端逐位签名比较因此仍然失败，未放宽该正式核验；全部角点 XYZ 最大差约 0.0000500083 米。UV、法线、材质和纹理相同，见 [逐分量差异](evidence/V0.1.3/native-pro-coordinate-differences.json)。独立对照源中间包的读取核验按各 GDB 空间参考精度通过，不能称两个 GDB 逐位一致。
- 静态姿态和省略光照效果属于明确的兼容处理，不承诺完整还原源软件渲染。颜色、Alpha、六面和接缝显示验收仍独立进行；历史 Pro 红青双影待办保持原记录。
- 没有完成无 .NET/Pro 的全新 Windows 机器验收。GUI 自带 .NET，原生后端仍需 Microsoft C++ 运行库；Pro 后端需本机已安装且许可可用的 Pro。
- 几何和图像解码正确性优先于兼容：未知语义、缺图、非法 UV、非有限值、骨骼形变等仍失败。
- [原始文件散列](evidence/V0.1.3/original-files-unchanged.json) 证明源 FBX 和原失败报告保持不变。

本次独立 Pro 核验工具的 [源码与复现说明](evidence/V0.1.3/independent-pro-tool/README.md) 随验证证据提供；源用户模型不打入源码测试样本。
