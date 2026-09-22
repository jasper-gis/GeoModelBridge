# V0.2（0.2.0）验证记录

2026-09-22。本版统一源码、CLI、原生 writer、GUI 和 Python 客户端的语义版本为 `0.2.0`，发布名称为 V0.2。历史版本报告和图形验收结论保持不变。

## 修复与回归范围

- Windows WIC 曾接受 JPEG 截断扫描、缺失 PNG IEND、错误 CRC，以及 CRC 正确但 deflate/Adler 数据损坏的 PNG。现在 Windows 与 Ubuntu 共用 `images_png.cpp` / `images_jpeg.cpp`，完整解码时把恢复警告视为失败。PNG 输出未预乘 RGBA8；JPEG 验证后仍写入原始压缩字节。
- Windows 静态链接官方 libjpeg-turbo 3.1.4.1、libpng 1.6.58 和 zlib 1.3.2 的解码代码；源码与下载归档的固定来源、SHA-256 见 `third_party/manifest.json`。原始许可随安装目录提供。Ubuntu 继续使用系统库，共享同一套解码和校验代码。
- CLI、GUI 和 Python 拒绝不匹配的来源/定位、重复 JSON 字段、缺失或失败的逐网格回读、矛盾或非法诊断。CLI 还确认已有读取诊断全部保留；截断探针和被链接替换的成果不能成为成功依据。
- Scene Bundle 的纹理和流式 JSON 共用独占创建句柄；测试覆盖分块写出、异常清理、已有文件/目录、并发写入和链接路径。失败创建不会清理已有数据。
- 隐藏父节点和隐藏显示层不能静默变成可见网格，两种 profile 都明确拒绝。完整原生预检在建库之前检查重复字段、所有角点 UV 和精确 Shape 大小，保留原有容量限制。
- GUI 消息回调抛异常时继续读取进程输出，保留已核验成果并说明显示失败；Python 的回调结果保留规则继续有效。

## 最终执行结果

Windows x64 使用 MSVC 19.44、Python 3.11.15；Ubuntu 24.04.4 x86_64 使用 GCC 13、Python 3.12。所有最终数据结果均来自本版程序，未使用桌面 GIS 软件执行自动验证。

| 检查 | Windows | Ubuntu |
| --- | --- | --- |
| 完整根工程 CMake / CTest | 11/11 | 11/11 |
| 原生错误输入、范围、散列与清理检查 | 52 项通过 | 52 项通过 |
| PNG/JPEG、透明度、UV、法线及 210,000 角点流式网格真实回读 | 通过 | 通过 |
| 独立复制 GDB 与可搬迁部署 | 通过 | 通过 |
| 安装后的 Python 真实调用 | 9/9，4 份 GDB 复制回读 | 同左 |
| Python 契约 | 29 项：26 通过、3 跳过 | 29/29 |
| GUI 服务与真实转换 | 191/191 | 不适用 |
| 法线专项 | 14/14 | 14/14 |
| 缺图专项 | 24 项通过 | 29 项通过 |
| 完整 GDB 样例与发布检查 | 14/14，check-only 通过 | 同左 |
| SDK-free 核心构建 | MSVC CTest 9/9，另行 MinGW 9/9 | 完整构建包含核心测试 |

Windows 符号链接构造因账户权限跳过；实际 Windows junction 回归已执行。Windows 的 POSIX FIFO 等测试按平台跳过。跳过项明确保留在日志，未计作通过。

最终 Ubuntu 使用新的 `build/v02-final` 完整重建。中间轮次复用构建目录时，早期源快照生成的旧 CLI 对象因文件时间顺序未被重新编译，新增溢出 WKID 负例失败；全新目录重建后通过。最终两平台的 243 份构建源码、配置及测试资源 SHA-256 一致，记录见 [source-match.json](evidence/V0.2/source-match.json)。此中间构建问题没有作为最终通过结果。

主要证据位于 [evidence/V0.2](evidence/V0.2)：[Windows 构建](evidence/V0.2/windows-build.log)、[原生集成](evidence/V0.2/windows-native.json)、[部署](evidence/V0.2/windows-deployment.json)、[Python 调用](evidence/V0.2/windows-python-client.json)、[GUI](evidence/V0.2/windows-gui.json)。用户目录和测试工作路径已脱敏；CTest 原始日志中的非 UTF-8 时间文字以替换字符保留。

Ubuntu 证据：[完整构建](evidence/V0.2/ubuntu-build.log)、[原生集成](evidence/V0.2/ubuntu-native.json)、[部署](evidence/V0.2/ubuntu-deployment.json)、[Python 调用](evidence/V0.2/ubuntu-python-client.json)、[发布检查](evidence/V0.2/ubuntu-package.log)。

本次只执行发布 `--check-only`，不生成本地归档。Windows 程序保存在新目录 `releases/V0.2.0`，保留旧版本。未重跑历史用户大模型、ATBX、跨平台双向搬迁或目标 GIS 软件图形外观验收；不能把本次自动回读结果表述为这些项目已通过。
