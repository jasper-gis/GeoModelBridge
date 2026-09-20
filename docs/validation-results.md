# V0.1.0 本机验证结果

**结果：C++ 工程可构建，4 组 CTest 全部通过；14 个参考 GDB 完整写入与关库读回通过；GDB 独立复制核验通过。** 验证日期为 2026-09-20，全部为真实本机运行结果。

## 环境

Windows x64；GCC/MinGW-w64 16.1.0、CMake 4.3.3、Ninja；.NET SDK 10.0.203、.NET 8.0.26；ArcGIS Pro 3.6.2.59530（Core assembly 13.6.0.0）。Pro CoreHost 初始化和本机许可探测通过，未修改许可配置。

## 自动验收

| 验收项 | 结果与证据 |
|---|---|
| 核心模型与数据验证 | 索引、材质/纹理引用、有限数字、UV、退化面、SHA-256 标准向量、定位、接缝/硬边、输出保护通过 |
| 命令行与中间包 | 实际运行 CLI；原图字节、PNG CRC/解压/Alpha、混合材质、已有成果不变、失败无部分中间包通过 |
| ASCII FBX Reader | 外置/内嵌贴图、单位/轴归一化、节点/几何变换、镜像、多材质、逆转置法线、UV 选择/变换、缺贴图拒绝通过 |
| 二进制 FBX Reader | 固定版本上游 Blender 样本正向通过（含中文路径）；Maya 样本可解析并按严格规则拒绝超出范围的内容 |
| Pro 写入后端 | 实际 FileGDB/Multipatch 创建、关库后重开、材质/UV/图片/几何/属性回读通过；PNG 透明像素含隐藏 RGB 和 JPEG 压缩字节均保持 |
| 失败处理 | 19 项非法输入、坐标系约束、reader error、既有成果保护、真实写库失败清理及独立纹理散列不匹配测试通过 |
| 完整参考成果 | 5 组合成模型 + 8 个自造 ASCII FBX + 1 个 Blender 二进制 FBX，共 14 个 GDB 转换案例全部通过 |
| 独立交付 | GDB 复制至新目录，让原 bundle 路径不可访问后，以新进程仅读复制库和原报告，几何/材质/纹理签名匹配 |
| CLI 后端失败报告 | 用地理坐标 WKID 触发后端拒绝，CLI 返回失败并保存含具体原因的报告，不产生 GDB |

CTest 原始记录见 [core-tests.log](evidence/core-tests.log)；后端失败测试的机器可读记录见 [pro-integration.json](evidence/pro-integration.json)。后端完整说明见 [TEST_RESULTS](../backends/arcgis-pro/TEST_RESULTS.md)。

14 案例清单、复制验证和 CLI 失败处理结果见 [verification-summary.json](../examples/V0.1.0/verification-summary.json)。每个 GDB 对应的详细报告在 `examples/V0.1.0/reports/`，GDB 本体在 `examples/V0.1.0/filegdb/`。

## 法线存储精度的专项验证

斜面样本揭示：源 `(0,-0.8,0.6)` 在本机 Pro 写 GDB 后变为 `(0,-0.796875,0.6015625)`。独立探针覆盖 12,265 个单位法线、36,795 个分量，均精确符合 `floor(float32(n)*128+0.5)/128`，半步向正无穷取整，未重新归一化。斜面实际方向误差约 0.17933°。

因此后端分别检查 builder 的单精度值和 GDB 的编码预测值；不通过扩大普通浮点容差掩盖变化。报告记录源法线 SHA-256、量化角点数、最大分量/角度/长度误差。斜面、三轴均分及正负半步回归已通过。此规则是对本机 Pro 3.6.2 的实测结论，未当作 Esri 对所有版本的承诺。

## 尚未声称通过的部分

- ArcGIS Pro/GeoScene 中的实际显示验收：UV 朝向、色彩管理、透明排序、光照与跨软件兼容性仍需按 [验收说明](acceptance.md) 检查。
- 不依赖 Pro 的原生 C++ FileGDB textured writer：未实现。
- 用户业务 FBX、大规模性能、其他操作系统及其他 Pro/GeoScene 版本：本次未实测。
- 项目提供本地 Git 初始化和 CI workflow；未设置远程仓库，也未运行远程 CI。首次提交可由项目所有者设置 Git 身份后完成。

以上边界不影响将本工程作为 V0.1.0 原型使用，但不能将样本通过扩展为所有 FBX 材质、所有软件版本或所有精度数据均无损。
