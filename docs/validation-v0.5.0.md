# V0.5.0 验证记录

2026-09-27。CLI、原生 FileGDB writer、Windows GUI、Python 客户端统一为 `0.5.0`。

## 完成范围

OBJ、GLB、glTF、WRL 均已在 Windows x64 和 Ubuntu 24.04.4 x86_64 通过真实带贴图 FileGDB 写入、关闭重开和复制数据库后的独立回读。CLI、安装后的标准库 Python 客户端及 Windows GUI 服务均执行真实转换；不是仅验证 Scene JSON 或模拟 writer。

- glTF JSON 使用与 GLB 相同的 cgltf 场景读取与材质校验，覆盖外置 BIN/图片、base64 data URI、稀疏属性、节点变换、选定 UV 和纹理变换。修复默认材质单双面、无图片时已有 UV 的保留及修复数量报告。
- WRL 支持静态 VRML97 IndexedFaceSet、DEF/USE、嵌套变换、旋转中心、scaleOrientation、镜像、独立法线/UV、凸凹多边形与每面颜色。按 VRML97 的 RGB/alpha 替换规则读取图片，含 JPEG、灰度与灰度 alpha PNG 回归。
- 共用原生后端和 Scene Bundle，不新增平台分支；原图字节、placement 来源、防覆盖和独占输出契约继续执行。
- `.max` 仅完成[实施路线设计](max-roadmap.md)，未作为已支持输入发布。

支持范围和拒绝策略分别见 [glTF](glb.md)、[WRL](wrl.md)、[兼容策略](compatibility.md)。复杂渲染、交互式 VRML、未知通道不会静默转换为灰模。

## 实测结果

| 检查 | Windows x64 | Ubuntu 24.04.4 x86_64 |
| --- | --- | --- |
| 根工程完整 CMake / CTest | 15/15 | 15/15 |
| OBJ / GLB / glTF / WRL 带贴图 GDB + 独立复制回读 | 全部通过 | 全部通过 |
| 原生异常输入、清理、散列和成功报告验证 | 83 项通过 | 84 项通过 |
| PNG/JPEG、alpha、UV、法线、颜色和 210,000 角点流式真实 GDB | 通过 | 通过 |
| 安装后 Python 客户端真实转换 | 13 项，8 份复制回读 | 13 项，8 份复制回读 |
| Python 标准库客户端契约 | 28 通过、3 跳过 | 31/31 |
| GUI 服务及真实转换 | 236/236 | 不适用 |
| 法线 / 缺图专项 | 14 / 24 项通过 | 14 / 29 项通过 |
| 最小部署目录，四种新格式实际入库及搬迁核验 | 通过 | 通过 |
| 依赖散列、版本一致性 | 通过 | 通过 |
| 14 份完整 GDB 样例及发布包 check-only | 通过 | 通过 |

Windows 使用任务已有 MSVC 19.44 工具链，Ubuntu 使用 GCC 13；两平台通过根工程构建当前源码。Ubuntu 在已有 QEMU 虚拟机中以原有内核、`noapic` 参数启动；没有使用旧版本二进制替代当前构建。两平台 [259 份实现、配置、测试及依赖文件逐字节 SHA-256 一致](evidence/V0.5.0/source-match.json)。

Windows 缺少创建符号链接权限，Python 相关 3 项明确跳过；junction 及共享 output_safety CTest 已运行。POSIX 权限、FIFO 和链接检查在 Ubuntu 上执行。MSVC 日志保留第三方 cgltf 和既有 `getenv` 的弃用提示；不将其描述为零警告的原生构建。

部署测试曾因新增格式时的测试命令参数列表错误而失败，修正测试脚本后在全新目录重跑通过；保留原始工作目录，最终证据指向修正后的脚本和成功结果。WRL 灰度/alpha 补充用例也在两平台单独重跑通过。Ubuntu 的发布检查最初缺少源码快照的 Git 元数据，在该测试目录初始化 master 元数据后通过；没有替换被测源码。发布检查同时确认 glTF、BIN、WRL 演示文件必需存在并进入归档文件列表。本次只做归档 check-only，没有生成新的发布 ZIP/TAR。

## 可复核证据

- Windows：[构建与 CTest](evidence/V0.5.0/windows-build.log)、[四格式成果摘要](evidence/V0.5.0/windows-formats.json)、[原生测试](evidence/V0.5.0/windows-native.json)、[GUI](evidence/V0.5.0/windows-gui.json)、[Python](evidence/V0.5.0/windows-python-client.json)、[部署](evidence/V0.5.0/windows-deployment.json)。
- Ubuntu：[构建与 CTest](evidence/V0.5.0/ubuntu-build.log)、[四格式成果摘要](evidence/V0.5.0/ubuntu-formats.json)、[原生测试](evidence/V0.5.0/ubuntu-native.json)、[Python](evidence/V0.5.0/ubuntu-python-client.json)、[部署](evidence/V0.5.0/ubuntu-deployment.json)。

Windows 可用程序在 `releases/V0.5.0/bin`；源码仓库不提交 EXE/DLL。自动验收不需要 ArcGIS Pro、ArcPy 或其他桌面 GIS。未进行本版本目标 GIS 图形外观验收、用户专有大模型验收或 MAX 运行时验证；上述边界不由自动数据库回读替代。历史 V0.3.0 / V0.4.0 的“尚未原生验证”记录保留原结论，本文件记录本次新增的实际证据。
