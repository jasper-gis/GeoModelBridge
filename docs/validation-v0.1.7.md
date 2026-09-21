# V0.1.7 验证记录：缺少贴图不再阻断转换

验证日期：2026-09-21。使用 Windows x64 / MSVC 19.44 和 Ubuntu 24.04.4 x86_64 / GCC 13.3，后者沿用 V0.1.6 的独立无桌面 GIS 虚拟机。两平台均使用原生 FileGDB API 1.5.5.330；没有调用 Pro 后端。以下为实际执行结果，原始日志见 [evidence/V0.1.7](evidence/V0.1.7)。

## 本次结果

| 检查 | Windows | Ubuntu |
| --- | --- | --- |
| Release 构建、版本声明、依赖散列与 SDK probe | 通过 | 通过 |
| CMake/CTest 核心与 CLI 回归 | 6/6 | 6/6 |
| 缺图专项：11 个输入场景 + 3 个真实 GDB 写入/复制回读 | 14/14 | 14/14 |
| 原生错误输入、清理与散列篡改回归 | 33/33 | 33/33 |
| 既有完整 GDB 样例与独立回读 | 14/14 | 14/14 |
| 独立最小部署、当前版本发布内容检查 | 通过 | 通过 |
| GUI 服务与真实转换回归 | 148/148 | 不适用 |

专项测试覆盖默认颜色/标量透明度回退、显式要求完整贴图、两种渲染 profile、缺失透明度贴图别名、缺图且没有 UV、正常图片与缺失图片并存。真实 GDB 的三种场景为纯缺图、缺图且没有 UV、部分图片缺失，均关闭重开并复制到新路径，由新进程再次核验。

损坏的现有图片、有贴图却没有 UV、无效法线及仍在使用的不支持的透明度通道继续拒绝。两个非法策略值在参数阶段被拒绝。后端另验证未知策略、要求完整贴图却含回退记录的矛盾 Bundle，以及声明过但丢失的 Bundle 图片，均不会生成成功成果。

证据：[Windows 构建](evidence/V0.1.7/windows-build.log)、[缺图专项](evidence/V0.1.7/windows-missing-textures.log)、[原生](evidence/V0.1.7/windows-native.log)、[GUI](evidence/V0.1.7/windows-gui.json)、[部署](evidence/V0.1.7/windows-deployment.json)、[样例](evidence/V0.1.7/windows-examples.json)、[发布检查](evidence/V0.1.7/windows-package-check.log)；[Ubuntu 构建](evidence/V0.1.7/ubuntu-build.log)、[缺图专项](evidence/V0.1.7/ubuntu-missing-textures.log)、[原生](evidence/V0.1.7/ubuntu-native.log)、[部署](evidence/V0.1.7/ubuntu-deployment.json)、[样例](evidence/V0.1.7/ubuntu-examples.json)、[发布检查](evidence/V0.1.7/ubuntu-package-check.log)。

## 用户模型复测

对提供的 `G238国道.fbx`（286,703,164 字节）执行 `inspect --profile gis-static`，使用默认 `material-color` 缺图策略。用户已确认图片缺失。本次检查保留 4,349 个网格、171 个材质和 10,832,311 个三角形；没有补造图片或修改源 FBX。

| 诊断 | 原 V0.1.3 报告 | V0.1.7 复测 |
| --- | --- | --- |
| 缺图错误 | 170 | 0 |
| 缺失图片的通道/连接错误 | 3 | 0 |
| 缺少 UV 错误 | 11 | 0 |
| 缺图回退警告 | 0 | 171 |
| 无效法线错误 | 15 | 15 |

新版统计所有缺失文件连接，包括旧版按光照通道省略的连接，因此回退警告数不等于旧版缺图错误数。既有 21 条零面积面删除记录和 1 条光照通道省略记录保持。

**缺图已不再阻断，但该原模型仍因 15 条无效法线错误而被拒绝，退出码为 3，未生成 GDB。** 本次未重算法线，也未声称此模型已转换成功。源 FBX 与用户原报告的 SHA-256 均保持一致；仓库仅保存[汇总与散列](evidence/V0.1.7/real-model-assessment.json)，不包含用户模型或其详细贴图路径。

## 验证边界

缺图回退保留已知材质颜色与标量透明度，无法恢复图片内容、透明度遮罩或原有渲染外观。数据库回读核验与目标软件三维显示验收分别进行；本次没有新增视觉验收结论。Windows ↔ Ubuntu 的双向成果互读证据仍属于 [V0.1.6](validation-v0.1.6.md)，未重新标记为本版结果。
