# V0.1.11 验证记录

2026-09-21，本版修复中间包 / 报告提交时的覆盖竞态，将核心与原生 FileGDB writer 的文件操作合并为共享实现。自动检查仍不依赖 Pro 或 ArcPy。

## 验证结果

| 检查 | Windows x64 | Ubuntu 24.04.4 x86_64 |
| --- | --- | --- |
| CMake/CTest | 9/9 | 9/9 |
| 新增输出保护 | 7 组通过，1 组链接检查跳过 | 8/8，无跳过 |
| 原生写入及拒绝 / 清理检查 | 37 项通过，含 21 万角点流式输入 | 37 项通过，含 21 万角点流式输入 |
| 安装后的 Python 真实调用 | 7/7，4 份 GDB 独立复制回读 | 7/7，4 份 GDB 独立复制回读 |
| Python 契约测试 | 25 通过，1 项链接测试跳过 | 26/26 |
| 可搬迁部署与独立 Python 导入 | 通过 | 通过 |
| GDB 样例及回读 | 14/14 | 14/14 |
| 版本、依赖散列、发布检查 | 通过 | 通过 |
| GUI 服务与实际转换 | 161/161 | 不适用 |

Windows 当前账户没有创建符号链接的权限，新输出测试明确打印 SKIP，不把链接检查计为通过。Windows 使用 Python 3.11.15；Ubuntu 使用 Python 3.12.3。发布检查为 `--check-only`，不生成归档；本机程序目录为 `releases/V0.1.11`，旧成果保留。

## 新增回归覆盖

- 暂存文件独占创建：已有文件的内容（含二进制零字节）不被截断、删除或改写。
- 提交前出现同名目标文件、空目录或悬空链接时失败；目标内容 / 类型保留，暂存源仍可读取。向新名称提交仍能完成。
- 8 个并发文件提交者只有一个成功；未成功者的源文件保留。
- 8 个并发报告写入者、8 个并发中间包写入者各只有一个成功。报告和中间包来源一致，贴图资源完整，失败写入者不留下暂存项。
- 已有空中间包保持为空；JSON 序列化异常不会留下报告或暂存文件。
- 对悬空链接、链接父目录的输出拒绝，不创建链接目标或下层目录。

同名冲突测试直接覆盖提交原语，因此不依赖碰巧撞中检查与提交之间的时间窗口；并发测试另行覆盖完整报告和中间包流程。链接策略和文件系统限制见[输出保护约定](architecture.md#输出保护约定)。

## 构建修正与证据

Windows 首次构建的核心 9 项测试已通过，但独立 writer 构建发现 `bundle.hpp` 仍保留旧的 `reject_reparse` 前向声明。已改为引用共享声明，完整重建、原生转换及部署检查通过。Ubuntu 首次编译新增测试时发现泛型 lambda 中的依赖模板解析差异；改为明确的 `std::filesystem::path` 参数，并在两平台重新编译测试。

日志和 JSON 位于 [evidence/V0.1.11](evidence/V0.1.11)，本机用户和工作目录替换为占位符。首轮失败日志与修正后的最终日志分别保留。

- Windows：[最终 CTest](evidence/V0.1.11/windows-ctest-final.log)、[输出保护](evidence/V0.1.11/windows-output-safety.log)、[原生集成](evidence/V0.1.11/windows-native.json)、[Python 真实调用](evidence/V0.1.11/windows-python-client.json)、[独立部署](evidence/V0.1.11/windows-deployment.json)、[GUI](evidence/V0.1.11/windows-gui.json)。
- Ubuntu：[最终 CTest](evidence/V0.1.11/ubuntu-ctest-final.log)、[输出保护](evidence/V0.1.11/ubuntu-output-safety.log)、[原生集成](evidence/V0.1.11/ubuntu-native.json)、[Python 真实调用](evidence/V0.1.11/ubuntu-python-client.json)、[独立部署](evidence/V0.1.11/ubuntu-deployment.json)。

## 范围

本次未改变几何、贴图或法线兼容策略，未重跑用户大模型；其历史转换证据仍见 [V0.1.8](validation-v0.1.8.md)。未制作 ATBX、实现取消 / 追加已有 GDB / 重投影，也未进行三维外观验收。GDB 与外部报告分别提交，不构成跨文件事务；不能把正常并发保护描述为抵御恶意父目录替换的沙箱。文件系统不支持禁止覆盖重命名时明确失败，不退回普通覆盖式 rename。
