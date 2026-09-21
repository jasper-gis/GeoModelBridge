# V0.1.9 验证记录

2026-09-21，本版新增 Python 调用库，为后续 Windows ATBX 调用 EXE 准备接口；未新增工具箱文件。下列结果来自本地 Windows x64 与无 Pro 的 Ubuntu 24.04.4 x86_64 QEMU 虚拟机，使用各平台原生 FileGDB API 1.5.5。Windows Python 为 3.11.15，Ubuntu Python 为 3.12.3。

## 实际结果

| 检查 | Windows | Ubuntu |
| --- | --- | --- |
| Release 构建、CMake/CTest | 8/8 | 8/8 |
| Python 调用库契约测试 | 19 项：18 通过，1 项跳过 | 19/19 |
| 安装后 Python 调用真实引擎 | 6/6，3 份 GDB 独立复制回读 | 6/6，3 份 GDB 独立复制回读 |
| 原生后端集成 | 37 个拒绝/清理/散列检查通过，含 21 万角点流式写入 | 同左 |
| 搬迁后最小目录部署 | 通过，包含 `python -S` 导入复制后的库并转换 | 同左 |
| 14 个完整 GDB 样例与复制回读 | 14/14 | 14/14 |
| 当前版本与依赖散列、发布清单检查 | 通过 | 通过 |
| GUI 服务 | 153/153 | 不适用 |
| 发布目录中的 Python 普通示例 | 实际写入带贴图 GDB 并核验 | 本版未单独运行示例脚本 |

Windows 的一项符号链接构造测试因当前账户无无特权创建权限而跳过；该项在 Ubuntu 实际运行通过。CMake/CTest 的 `python_client` 项在 Windows 允许这个明确记录的跳过。库仍拒绝链接 / Windows reparse point，不将跳过表述为已验证 Windows 链接场景。

## Python 接口覆盖

契约测试涵盖参数类型、有限 XYZ、有效 WKID、已有输出/报告、嵌套 GDB 路径、中文和空格、无 shell 参数传递、版本/运行库不匹配、无法启动、非零退出、退出为零但缺失/矛盾报告、失败诊断、报告超限与重复字段、大量日志的有界内存读取，以及调用线程内阶段回调。报告须匹配原 FBX、输出路径、要素类、策略和投影 WKID。

安装后集成的六项分别为：带贴图 FBX 转换、拒绝重用已有成果、缺图回退、要求完整贴图时拒绝、严格模式拒绝坏法线、GIS 静态修复法线。三份成功 GDB 都复制到新的独立目录，仅凭报告完成原生回读；带贴图及法线修复案例检查 `textured_patches > 0`。所有源样例散列保持不变，Python 进程没有导入 `arcpy`。

搬迁测试仅复制 CLI、writer、官方运行库、演示输入及 Python 源码到新目录，移除 GIS 环境变量、收窄子进程 PATH，使用 `python -S` 禁用 site-packages 后导入复制的库并完成转换。Windows 主机本身并非“从未安装 GIS 软件”的证明；Ubuntu 虚拟机不安装 Pro。

## 证据与边界

原始日志和摘要位于 [evidence/V0.1.9](evidence/V0.1.9)，仅将本机工作目录 / 用户目录替换为占位符：

- [Windows 构建](evidence/V0.1.9/windows-build.log)、[Python 契约](evidence/V0.1.9/windows-python-unit.log)、[真实调用](evidence/V0.1.9/windows-python-client.json)、[独立部署](evidence/V0.1.9/windows-deployment.json)、[GUI](evidence/V0.1.9/windows-gui.json)。
- [Ubuntu 构建](evidence/V0.1.9/ubuntu-build.log)、[Python 契约](evidence/V0.1.9/ubuntu-python-unit.log)、[真实调用](evidence/V0.1.9/ubuntu-python-client.json)、[独立部署](evidence/V0.1.9/ubuntu-deployment.json)。
- 两平台的 `*-native.log`、`*-examples.json`、`*-package-check.log` 保存其他回归结果。

Python 3.9 为语言兼容目标，已配置 Windows / Ubuntu 的 3.9 和 3.12 CI 矩阵，本地未运行 3.9；不能把配置矩阵当作托管 CI 已通过。本版没有运行 ATBX、ArcGIS 内参数/派生输出/取消链路或三维显示验收；同步库也未提供进程树取消接口。

本版不重新标注旧大模型证据：4,349 个要素和 1,083 万三角形的用户模型转换仍属于 [V0.1.8 验证](validation-v0.1.8.md)。已有用户 FBX、GDB、报告及旧发布目录保留，本次新增 `releases/V0.1.9` 完整目录，不生成归档。
