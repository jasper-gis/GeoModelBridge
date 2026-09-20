# V0.1.6 验证记录：Ubuntu 与 Windows 共用原生转换

验证日期：2026-09-21。结果来自实际 Windows 执行与独立 Ubuntu 虚拟机执行，不把 CI 配置或编译通过等同于数据库写入成功。原始记录位于 [evidence/V0.1.6](evidence/V0.1.6)。

## 结论与平台

Ubuntu 24.04 x86_64 可以运行完整静态 FBX → 原生 FileGDB Multipatch 流程，无需 ArcGIS Pro、ArcPy、.NET 或桌面会话。Windows 保留同一转换链路和 WPF GUI；Linux 本次交付命令行。两平台共用核心、Bundle 验证、材质/Shape Buffer 编解码和写入/回读逻辑。

Ubuntu 测试使用官方 Ubuntu 24.04.4 LTS minimal cloud image，在隔离 QEMU x86_64 虚拟机中安装系统编译与图片依赖；没有安装桌面 GIS。内核 6.8.0-139、GCC 13.3、CMake 3.28.3、Python 3.12.3、glibc 2.39、libpng 1.6.43、libjpeg-turbo 2.1.5。具体包版本和 ELF 依赖见 [环境记录](evidence/V0.1.6/ubuntu-environment.log)。该虚拟机用于功能验证，不用于性能结论。

Windows 使用本机 Windows x64、MSVC 19.44、Windows SDK 10.0.22621.0。分别验证原有 PowerShell 构建与新增统一 Python/CMake 构建入口。两平台均使用各自官方 FileGDB API 1.5.5.330 SDK；归档、共享库和许可文件均按固定清单核对。

## 实际执行结果

| 检查 | Ubuntu | Windows |
| --- | --- | --- |
| 完整 Release 构建与 SDK probe | 通过 | 通过 |
| CMake/CTest 核心、CLI、Reader、二进制 FBX、profile | 5/5 | 5/5 |
| 真实 GDB 写入、关闭重开、Alpha/JPEG/法线/UV/材质 | 通过 | 通过 |
| 无源 Bundle 的复制成果回读 | 通过 | 通过 |
| 原生拒绝/清理/假散列回归 | 30/30 | 30/30 |
| 调色板透明 PNG、2-bit 灰度、Adam7、Gamma 保持 | 4/4 像素散列通过 | 4/4 像素散列通过 |
| 中文路径与 SDK 中文属性往返 | 通过 | 通过 |
| 独立最小部署目录 | 通过，清除 LD_LIBRARY_PATH/LD_PRELOAD 后从 PATH 转换 | 通过，移除开发/GIS 路径 |
| 5 个合成 + 9 个 FBX 完整 GDB 样例 | 14/14 | 14/14 |
| 当前版本发布内容核查 | 通过；生成 tar.gz | 通过 |
| Windows GUI 服务回归，含真实原生转换 | 不适用 | 138/138 |

Ubuntu 另验证大小写不同的兄弟目录不会被误判为输入子目录、符号链接/悬空输出链接被拒绝，以及截断 JPEG 不因解码器恢复而被接受。输出、报告重复路径与 SDK 失败只清理本次暂存目录的既有检查在两侧通过。

Linux 发布 ELF 的 RPATH 为 `$ORIGIN`，依赖解析确认两份 FileGDB `.so` 来自 writer 相邻目录，没有遗留构建 SDK 路径。最小部署测试仅复制 CLI、writer、两份 SDK 库及演示输入到新目录，使用系统运行库，从不同工作目录通过 PATH 完成转换和复制回读。

证据：[Ubuntu 构建](evidence/V0.1.6/ubuntu-build.log)、[原生测试](evidence/V0.1.6/ubuntu-native.log)、[部署](evidence/V0.1.6/ubuntu-deployment.json)、[样例](evidence/V0.1.6/ubuntu-examples.json)；[Windows 构建](evidence/V0.1.6/windows-build.log)、[原生测试](evidence/V0.1.6/windows-native.log)、[部署](evidence/V0.1.6/windows-deployment.json)、[GUI](evidence/V0.1.6/windows-gui.json)。

## 两个操作系统之间的成果互读

1. 复制 Windows 生成的全部 14 个 GDB 和原始成功报告到 Ubuntu，由 Linux SDK 在新进程中逐个核验 Shape Buffer、纹理、属性与 WKID：14/14 通过。
2. 复制 Ubuntu 生成的全部 14 个 GDB 和原始成功报告到 Windows，以 Windows SDK 执行同样检查：14/14 通过。
3. 比较两个系统各自生成的同名 14 个样例，每个 feature 的回读 Shape Buffer SHA-256 和纹理存储 SHA-256 全部一致。此结论针对这组测试内容，不表示整个 GDB 目录二进制文件逐字节一致。

证据：[Windows → Ubuntu](evidence/V0.1.6/windows-to-ubuntu.json)、[Ubuntu → Windows](evidence/V0.1.6/ubuntu-to-windows.json)、[独立生成成果散列比较](evidence/V0.1.6/independent-os-output-comparison.json)。复核脚本 [cross_readback.py](evidence/V0.1.6/cross_readback.py) 使用复制数据库及原报告，不读取原 FBX 或 Bundle。

## 边界

- 这是通过官方原生 SDK 的数据库回读及跨操作系统一致性验证，三维目标软件的颜色、Alpha、接缝和外观验收仍需另行完成。
- 未宣称支持 Ubuntu ARM64、Ubuntu 22.04、macOS 或所有 Linux 发行版；未移植 WPF GUI。Ubuntu 客户机目标为 24.04 x86_64。
- 本次没有重新运行历史用户生产模型或历史 Pro 对照工具；历史报告保留原版本与实际来源。
- GitHub Actions 增加 Ubuntu 完整构建、原生测试及打包任务。上述结果来自实际本地 Windows/Ubuntu 执行；仓库托管 CI 的状态应查看对应提交运行，不能由配置文件推定通过。
