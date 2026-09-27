# V0.6.0 验证状态（2026-09-27）

## MAX 的实际状态

MAX Batch 适配器、CLI / GUI / Python 接入已实现。**没有安装可用的 Autodesk 3ds Max Batch，也未取得真实 MAX 样本，因此没有真实 `.max → FileGDB` 实机通过结论。** 不将进程替身、人工构造的 FBX 或其他格式的 GDB 结果算作真实 MAX 验收。

当前 Standard 材质/静态网格边界、未支持的复杂材质及烘焙需求见 [MAX 适配器](max-adapter.md)。已提供 `tests/max_real_integration_test.py`，有运行环境和样本后可直接执行真实输入、原文件保护、安装后客户端、纹理 GDB 和复制独立读回验收。

## 自动验证

| 检查 | Windows x64 | Ubuntu 24.04 x86_64 |
|---|---:|---:|
| 完整 CMake / CTest（含原生 FileGDB） | 17/17 | 17/17 |
| 原生异常输入与输出保护 | 83 项 | 84 项 |
| 已安装 Python 客户端 | 13 项、8 份 GDB 独立复制读回 | 13 项、8 份 GDB 独立复制读回 |
| Python 合约 | 30 通过、3 平台限制跳过 | 32 通过、1 Windows MAX 专用跳过 |
| GUI 服务 | 245/245 | 不适用 |
| 既有格式最小部署 | 通过 | 通过 |
| 法线修复真实入库 | 14 项 | 14 项 |
| 缺图策略真实入库 | 24 项 | 29 项 |
| 示例生成与打包检查 | 14 个示例，check-only | 14 个示例，check-only |

Windows 使用 MSVC 19.44 和固定 FileGDB API 1.5.5；Ubuntu 使用 GCC 13 和同版官方 Linux SDK。未把 ArcGIS Pro、ArcPy 或其他桌面 GIS 作为测试依赖。依赖原文件及许可散列通过 `scripts/verify_dependencies.py`。

跨平台参与构建/测试的 **270 个源码、配置、测试、夹具与固定依赖文件 SHA-256 一致**。记录见 [source-match.json](evidence/V0.6.0/source-match.json) 与 [source-sha256.json](evidence/V0.6.0/source-sha256.json)。

## MAX 专项测试覆盖

- 调度合约：Windows 成功、双流大日志截断、无清单、材质拒绝、FBX 哈希/帧/面数/包围盒/颜色/图片绑定/面材质绑定错误、源文件变化、重复 JSON 键、导出后非零退出、超时；同时运行多个工作目录，验证互不干扰。Linux 验证明确拒绝预处理。
- 工作脚本策略：版本、原图搜索顺序、无效父路径、非文件路径、读取权限错误、链接、大小限制、PNG alpha、非有限材质值和未知属性拒绝。该组不导入 `pymxs`，不能证明实际 Max API 兼容。
- GUI / Python：必填帧号、运行路径、参数边界，以及成功报告中的来源、运行环境和帧号匹配。
- Windows **替身预处理 → 安装后标准库客户端 → 真实原生贴图 GDB → 独立复制读回**通过；改变图片绑定时在创建 GDB 前拒绝。证据明确标注 `real_max_runtime_used: false`：[专项记录](evidence/V0.6.0/windows-max-protocol-native.json)。

## 尚未验证

真实 Max 版本/许可/插件、用户 MAX 模型、实际快照法线与 UV/变换表现、复杂材质烘焙以及目标 GIS 软件视觉验收尚未完成。超时测试验证进程终止与返回，不声称已完成全部实际 Autodesk 插件子进程的取消验收。Windows Job Object 生命周期保护已实现。

## 日志

- [Windows CTest](evidence/V0.6.0/windows-ctest.log)、[GUI](evidence/V0.6.0/windows-gui.json)、[原生 SDK](evidence/V0.6.0/windows-native.json)
- [Ubuntu 最终构建/CTest/打包](evidence/V0.6.0/ubuntu-final-checks.log)、[原生 SDK](evidence/V0.6.0/ubuntu-native.json)
- [Windows 客户端](evidence/V0.6.0/windows-python-client.json)、[Ubuntu 客户端](evidence/V0.6.0/ubuntu-python-client.json)
