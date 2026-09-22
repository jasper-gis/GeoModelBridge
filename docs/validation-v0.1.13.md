# V0.1.13 验证记录

2026-09-22，在 Windows x64（MSVC 19.44、Python 3.11）和 Ubuntu 24.04.4 x86_64（GCC 13、Python 3.12）验证。此次调整命令行帮助、原生构建 / 运行库部署和研发文档；不改动模型解析、材质编码或数据库写入语义。新 Windows 安装位于 `releases/V0.1.13`，保留旧安装和已有成果。

## 结果

| 检查 | Windows | Ubuntu |
| --- | --- | --- |
| CMake/CTest，含 CLI 帮助、输出安全和 Python 契约 | 9/9 | 9/9 |
| Python 契约单独执行 | 25 通过，1 项链接构造因权限跳过 | 26/26 |
| 原生纹理写入、回读、输入拒绝 / 清理检查 | 37 项检查通过，含 21 万角点流式输入 | 同左 |
| 完整安装搬迁、连续 CLI 调用、冲突保护和复制回读 | 通过 | 通过 |
| 安装后的 Python 真实调用 | 9/9，4 份 GDB 独立复制回读 | 同左 |
| GUI 服务和真实转换 | 163/163 | 不适用 |
| 完整样例与发布检查 | 14/14、check-only 通过 | 同左 |
| 构建目录 SDK 散列、无开发路径 probe | 通过 | 通过，并用 ldd 核对实际库位置 |
| 缺失运行库恢复，含只构建 writer 目标 | 通过 | 通过 |
| 文档连续调用脚本 | PowerShell 和 cmd.exe 通过 | Bash 通过 |
| 版本一致性和依赖散列 | 通过 | 通过 |

## FileGDB API 核实

- [Windows 导入表](evidence/V0.1.13/windows-writer-dependencies.log) 明确列出 `FileGDBAPI.dll`；SDK `--probe` 返回 `backend=native-filegdb`、`filegdb_api=1.5.5` 和 `version=0.1.13`。
- Windows 新建独立 native 构建目录，未传 `GMB_INSTALL_FILEGDB_RUNTIME`，默认复制 DLL 并通过散列、帮助和 probe；另检查显式 OFF 的配置提示，以及只传 `-IncludeFileGDBRuntime` 未传 `-WithNative` 会提早失败。
- 两平台移除本次构建的运行库副本后，单独构建 `GeoModelBridge.NativeWriter` 即补回副本；writer 无需重链接。见 [Windows](evidence/V0.1.13/windows-runtime-recovery-target.log) / [Ubuntu](evidence/V0.1.13/ubuntu-runtime-recovery.log)。
- Linux 构建目录和安装目录均使用 `$ORIGIN`；[依赖记录](evidence/V0.1.13/ubuntu-writer-dependencies.log) 记录 ELF NEEDED 和 RPATH。测试清除 `LD_LIBRARY_PATH`，并通过 ldd 断言两个 SDK SO 都从 writer 旁加载，防止开发 SDK 路径掩盖缺文件。
- 部署测试把 CLI、writer、SDK 库和演示输入搬迁到独立目录，清理 SDK / GIS 环境路径；生成纹理 GDB 后复制，并由新进程按成功报告核验。

## 连续调用验证

新增部署用例先生成 `textured.gdb`（WKID 32650、origin 500000/3000000/100），再次调用同名目标应返回 2；逐文件散列和原报告字节保持不变。随后新进程生成 `second.gdb`（WKID 3857、origin 100/200/300、要素类 SecondModels），核对成功状态、定位和要素类，再复制到 `second-copy.gdb` 做独立进程回读。两次成功调用各有独立成果和报告。见 [Windows 部署](evidence/V0.1.13/windows-deployment.json) / [Ubuntu 部署](evidence/V0.1.13/ubuntu-deployment.json)。

帮助回归覆盖总入口和五个子命令的 `-h` / `--help`，未知命令和选项仍然返回参数错误。三个 shell 示例从最终 Markdown 提取并执行；未把示例命令通过等同于所有用户模型或所有环境通过。

## 首轮问题与验证边界

Windows 首轮新增运行库测试将 `os.environ` 转成普通 dict 后以混合大小写读取 `SystemRoot`，导致 KeyError。已改用 Windows 不区分大小写的原始环境映射，最终全部通过；[首轮日志](evidence/V0.1.13/windows-build.log) 和[最终日志](evidence/V0.1.13/windows-build-final.log) 均保留。

PowerShell 示例首轮在较深的仓库测试子目录下生成 GUID 结果目录，临时贴图路径超出当前 Windows 文件路径边界而返回 6。已明确文档限制，示例改用系统临时目录的独立结果路径，并重新通过。此次没有宣称修复任意 Windows 长路径；见 [首轮](evidence/V0.1.13/windows-doc-powershell.log) / [最终](evidence/V0.1.13/windows-doc-powershell-final.log)。

本次执行了发布 `--check-only`，没有创建本地发行归档。CI 已配置 Windows ZIP / SHA-256 附件，但只有实际工作流成功后才会产生附件，不能把配置变更表述为已上传。未重跑历史用户大模型、未进行图形外观验收、未进行 ATBX 验收，历史证据仍保持原版本。

原始记录的用户、临时目录和工作路径经脱敏，位于 [evidence/V0.1.13](evidence/V0.1.13)。
