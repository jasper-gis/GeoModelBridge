# V0.1.8 验证记录：法线兼容与大模型转换

验证日期：2026-09-21。Windows 使用 MSVC 19.44，Ubuntu 24.04.4 x86_64 使用 GCC 13.3；两平台沿用已固定散列的 FileGDB API 1.5.5.330，没有调用 ArcGIS Pro。原始记录见 [evidence/V0.1.8](evidence/V0.1.8)。

## 用户模型已完成转换

Windows 上使用用户提供的 `G238国道.fbx`，按指定 WKID **3857**、原点 **100,100,100**，选择 `gis-static` 和 `material-color`。模型先生成完整 Scene Bundle，再由原生 writer 写入新 GDB，关闭重开并逐要素核验；复制成果由新的 writer 进程再次核验，独立回读不依赖 FBX 或 Bundle。

| 项目 | 实测结果 |
| --- | --- |
| 最终状态 | `written_and_readback_verified` |
| 独立复制回读 | `standalone_copy_verified` |
| 网格 / GDB 要素 | 4,349 / 4,349 |
| 三角形 / 写入角点 | 10,832,311 / 32,496,933 |
| 材质 / 图片 | 171 / 0（用户确认图片缺失，保留颜色与标量透明度） |
| 修复法线 | 954 个角点，涉及 15 个网格、331 个三角形 |
| Scene JSON | 5,602,887,143 字节，超过旧版 512 MiB 上限 |
| 成果目录文件合计 | 387,220,498 字节 |
| 原 FBX 与先前失败报告 | SHA-256 均未变化 |

修复后的有效几何数量与 V0.1.7 检查一致。既有零面积面处理仍单独报告；其无效法线另记录为 `DEGENERATE_NORMALS_DISCARDED`，不作为有效面法线修复数。汇总及源文件、最终报告散列见 [real-model-assessment.json](evidence/V0.1.8/real-model-assessment.json)。用户 FBX、完整含源路径的报告和 GDB 不进入源码仓库。本用户模型在 Windows 实测成功，未在 4 GiB 内存的 Ubuntu 测试虚拟机中重复整模转换。

## 双平台回归

| 检查 | Windows | Ubuntu |
| --- | --- | --- |
| CMake/CTest 核心与 CLI | 7/7 | 7/7 |
| 法线专项（含真实 GDB 写入、复制回读） | 14/14 | 14/14 |
| 原生错误输入、清理、散列回归 | 37/37 | 37/37 |
| 单网格 210,000 角点流式解析、写入与回读 | 通过 | 通过 |
| 既有完整 GDB 样例与复制回读 | 14/14 | 14/14 |
| 最小目录部署与发布内容检查 | 通过 | 通过 |
| GUI 服务回归（含实际法线修复转换） | 153/153 | 不适用 |

法线专项包括严格模式拒绝、零法线与溢出长度修复、有效角点不变、非均匀缩放、镜像绕序、退化面计数、颜色/UV/图片不变和真实数据库回读。流式读取还验证元数据顺序无关、非对象网格及重复根字段拒绝，所有完整场景检查仍先于数据库创建。

Windows 样例首次运行遇到暂存 Bundle 重命名的 `Access denied`；保留该失败输出，在新目录重跑全部 14 例通过。用户成果的写入与独立复制回读均通过，未覆盖任何已有路径。

证据：[Windows 构建](evidence/V0.1.8/windows-build.log)、[法线](evidence/V0.1.8/windows-normal.log)、[原生](evidence/V0.1.8/windows-native.log)、[GUI](evidence/V0.1.8/windows-gui.json)、[部署](evidence/V0.1.8/windows-deployment.json)、[样例](evidence/V0.1.8/windows-examples.json)；[Ubuntu 构建](evidence/V0.1.8/ubuntu-build.log)、[最终原生构建](evidence/V0.1.8/ubuntu-final-native-build.log)、[法线](evidence/V0.1.8/ubuntu-normal.log)、[原生](evidence/V0.1.8/ubuntu-native.log)、[部署](evidence/V0.1.8/ubuntu-deployment.json)、[样例](evidence/V0.1.8/ubuntu-examples.json)。

## 边界

面法线回退可能让局部光照变硬；缺失图片无法恢复。未声称目标 GIS 软件的三维外观已完成验收。流式读取减少 JSON 中间对象，但仍保留类型化场景与准备数据，内存需求随模型规模增长；16 GiB Scene JSON、每网格 1000 万角点、每 Shape Buffer 512 MiB 上限仍生效。
