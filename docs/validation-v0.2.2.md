# V0.2.2 验证记录

2026-09-22。CLI、原生 writer、GUI 和 Python 客户端统一为 `0.2.2`。

## 确认并修复的问题

- 原生端曾只检查被三角形引用的法线；未引用角点的 `[0,0,2]` 或溢出长度法线仍可伴随成功 GDB。现在全部解码法线均在创建数据库前验证，和编码阶段共用同一单位长度规则。新增 3 项原生回归。
- FBX reader 曾只使用颜色前三分量和标量因子的首分量，静默忽略显式额外值。现在保留的 RGB 颜色可接受中性的第四分量 1；非中性 alpha、多维 diffuse/transparency factor 和 Opacity 明确拒绝。新增 26 项严格/GIS 静态回归，保留既有光照省略策略。
- Windows CLI 曾允许 `Bundle` 输出配合 `bundle/external.json` 报告，实际在输出内部写入“外部”报告。现在 CLI 与 native 复用平台包含检查，Windows 使用不区分大小写的组件比较，Linux 保留大小写敏感行为；Windows 尾随点/空格组件提前拒绝。回归覆盖 Unicode、组件前缀、点片段、不同盘符及平台差异。
- GUI 宿主在回调中切换当前目录，可能把相对路径重定向到别处，或把已完成成果判为缺失。服务现在在回调和异步操作前固定请求的绝对路径，并复制贴图目录列表。两项新增回归均先在旧实现复现失败。
- prepare 帮助和当前文档修正中间包报告名称为实际的 `report.json`。

## 最终验证

Windows x64 使用 MSVC 19.44、Python 3.11.15；Ubuntu 24.04.4 x86_64 使用 GCC 13、Python 3.12。两平台均使用新的完整构建目录，自动检查不依赖 ArcGIS Pro 或其他桌面 GIS 软件。

| 检查 | Windows | Ubuntu |
| --- | --- | --- |
| 根工程 CMake / CTest | 11/11 | 11/11 |
| 渲染 profile（包含在 CTest 中） | 124/124 | 124/124 |
| 原生异常输入、清理及散列检查 | 83 项通过 | 84 项通过 |
| PNG/JPEG、透明度、UV、法线及 210,000 角点流式网格真实 GDB 回读 | 通过 | 通过 |
| 安装后 Python 真实转换 | 9/9，4 份 GDB 复制回读 | 同左 |
| Python 契约 | 29 项：26 通过、3 跳过 | 29/29 |
| GUI 服务与真实转换 | 224/224 | 不适用 |
| 法线专项 / 缺图专项 | 14/14、24 项通过 | 14/14、29 项通过 |
| 可搬迁部署、14 个完整 GDB 样例、依赖哈希和发布 check-only | 通过 | 通过 |

Windows 账户缺少创建符号链接权限，对应构造项明确跳过；junction 检查已执行。POSIX FIFO、循环链接与权限场景按平台运行，跳过项不计为通过。

Ubuntu 虚拟机最初在内核启动阶段发生 IO-APIC 定时器故障，未进入项目测试。单线程指令计数模式恢复启动后编译过慢，该次构建被中断，不计为通过。随后使用原有内核及 microcode（复制前后 SHA-256 一致），按内核提示追加 `noapic`，以 QEMU Q35、多线程 TCG 启动，并在新的 `build/v022-verified` 完整重建和验证。原有 Windows `getenv` 弃用提示仍出现在构建日志中，不影响构建或运行结果。

最终两平台 244 份构建输入、配置及测试资源 SHA-256 相同，见 [source-match.json](evidence/V0.2.2/source-match.json)。Windows 证据：[构建与 CTest](evidence/V0.2.2/windows-build.log)、[原生集成](evidence/V0.2.2/windows-native.json)、[GUI](evidence/V0.2.2/windows-gui.json)、[Python](evidence/V0.2.2/windows-python-client.json)与[部署](evidence/V0.2.2/windows-deployment.json)。Ubuntu 证据：[构建与 CTest](evidence/V0.2.2/ubuntu-build.log)、[原生集成](evidence/V0.2.2/ubuntu-native.json)、[Python](evidence/V0.2.2/ubuntu-python-client.json)与[部署](evidence/V0.2.2/ubuntu-deployment.json)。用户目录和测试工作路径已脱敏。

本次只执行发布 `--check-only`，未生成本地归档；Windows 程序位于 `releases/V0.2.2`，旧发布目录及历史证据保留。未重跑用户大模型、ATBX、跨平台双向搬迁或目标 GIS 图形外观验收；自动数据库回读不能代替这些独立项目。
