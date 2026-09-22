# V0.2.1 验证记录

2026-09-22。CLI、原生 writer、GUI 和 Python 客户端的版本统一为 `0.2.1`。本轮继续审查 V0.2，并为复现的问题增加回归。

## 修复范围

- Scene Bundle 中含 NUL 的路径可能被文件系统截断；JSON 解析器还可能把根对象后的原始 NUL 视为 EOF，忽略后续内容。CLI 和原生端现在共用 64 KiB 缓冲输入，完整读取并检查累计大小，拒绝原始 NUL、解码后含 NUL 的字符串、重复字段和非法尾部。Scene JSON 上限仍为 16 GiB，报告上限 64 MiB。
- 原生独立复制核验曾接受非整数计数、失败检查和矛盾的定位报告。现在要求当前版本、完整成功状态、整数计数和连续唯一网格索引、逐要素通过标志、坐标一致及有效诊断，打开 GDB 前完成这些检查。报告路径必须是普通文件；Ubuntu 回归覆盖 FIFO。复制核验报告保留 `coordinates` 和 `coordinate_system`。
- GUI 报告阅读页拒绝把缺失、失败或重复的逐要素检查、异常坐标和非法诊断显示为成功；当前版本必须有 `reader_diagnostics`，旧报告保留必要兼容。要素类名称不接受尾随换行。
- CLI 拒绝空输入/参数值及重复的单值选项；可重复的 `--texture-dir` 保持有效。WKID 精确解析为正十进制整数，避免小数被 double 舍入后误接受。
- 纹理字节数必须是正整数；所有角点的 U 和映射后 `1-V` 在 float32 转换之前检查范围，仍在创建 GDB 之前完成完整预检。
- FBX 正式 `PremultiplyAlpha` 属性和模板默认值纳入检查；true 或无效属性类型会被拒绝，显式 false 可覆盖模板 true。14 项新 profile 用例覆盖外部/嵌入纹理、模板和无效类型，并核对可接受纹理的原始散列。

## 最终执行结果

Windows x64 使用 MSVC 19.44、Python 3.11.15；Ubuntu 24.04.4 x86_64 使用 GCC 13、Python 3.12。两平台从新的完整构建目录开始，随后对编译警告清理涉及的原生文件重新编译并执行最终回归。全部自动检查均不依赖桌面 GIS 软件。

| 检查 | Windows | Ubuntu |
| --- | --- | --- |
| 根工程 CMake / CTest | 11/11 | 11/11 |
| 渲染 profile（包含在 CTest 中） | 98/98 | 98/98 |
| 原生异常输入、清理及散列检查 | 80 项通过 | 81 项通过，含 FIFO |
| 真实 GDB：PNG/JPEG、透明度、UV、法线及 210,000 角点流式网格 | 通过 | 通过 |
| 独立复制 GDB、默认后端及可搬迁部署 | 通过 | 通过 |
| 安装后的 Python 真实调用 | 9/9，4 份 GDB 复制回读 | 同左 |
| Python 契约 | 29 项：26 通过、3 跳过 | 29/29 |
| GUI 服务与真实转换 | 222/222 | 不适用 |
| 法线专项 | 14/14 | 14/14 |
| 缺图专项 | 24 项通过 | 29 项通过 |
| 完整 GDB 样例、依赖哈希与发布检查 | 14/14，check-only 通过 | 同左 |

Windows 因账户权限跳过符号链接构造；Windows junction 检查已执行。POSIX FIFO、循环链接和权限场景按平台运行，跳过项在日志中单独记录，未计为通过。

两平台最终 244 份构建输入、配置及测试资源的 SHA-256 相同，见 [source-match.json](evidence/V0.2.1/source-match.json)。[Windows 构建](evidence/V0.2.1/windows-build.log)、[CTest 详细记录](evidence/V0.2.1/windows-ctest-detail.log)、[原生集成](evidence/V0.2.1/windows-native.json)、[GUI](evidence/V0.2.1/windows-gui.json)、[Python 调用](evidence/V0.2.1/windows-python-client.json)和[部署](evidence/V0.2.1/windows-deployment.json)均保留最终结果。

Ubuntu 证据：[构建与 CTest](evidence/V0.2.1/ubuntu-build.log)、[原生集成](evidence/V0.2.1/ubuntu-native.json)、[Python 调用](evidence/V0.2.1/ubuntu-python-client.json)、[部署](evidence/V0.2.1/ubuntu-deployment.json)及[发布检查](evidence/V0.2.1/ubuntu-package.log)。用户目录和测试工作路径已脱敏，CTest 中非 UTF-8 的本地时间文字以替换字符保留。

本次发布检查仅执行 `--check-only`，未生成本地归档。Windows 程序位于新目录 `releases/V0.2.1`。历史 V0.2 证据与程序保留。本轮未重跑用户大模型、ATBX、跨平台双向搬迁或目标 GIS 软件图形外观验收；自动回读结果不代表这些项目已通过。
