# V0.1.12 验证记录

2026-09-22，在 Windows x64（Python 3.11.15）和未安装 Pro 的 Ubuntu 24.04.4 x86_64（Python 3.12.3）验证。重点为缺失图片与错误 / 不可访问路径的判定、同次读取的路径查询复用，以及 GUI / Python 的错误传递。

## 验证结果

| 检查 | Windows | Ubuntu |
| --- | --- | --- |
| CMake/CTest | 9/9 | 9/9 |
| 贴图专项，含真实 GDB 及复制回读 | 24 项通过 | 29 项通过 |
| Python 契约测试 | 25 通过，1 项链接构造因权限跳过 | 26/26 |
| 安装后的 Python 真实调用 | 9/9，4 份 GDB 独立复制回读 | 同左 |
| 原生写入 / 拒绝 / 清理检查 | 37 项通过，含 21 万角点流式输入 | 同左 |
| 可搬迁部署与独立 Python 导入 | 通过 | 通过 |
| 完整 GDB 样例 | 14/14 | 14/14 |
| GUI 服务与实际转换 | 163/163 | 不适用 |
| 版本、依赖散列、发布检查 | 通过 | 通过 |

Windows 的贴图专项明确跳过 POSIX FIFO、符号链接和目录权限场景，这些场景在 Ubuntu 普通账户下实际执行，权限拒绝测试未跳过。既有输出保护测试中的 Windows 链接构造也因账户权限跳过。程序安装于新的 `releases/V0.1.12`，保留旧版本与已有数据；发布检查使用 `--check-only`，不生成归档。

## 问题复现与修复验证

相同 FBX 引用一个名为 `image-directory.png` 的目录，使用默认 `material-color` 策略：V0.1.11 返回 0、报告 `prepared` 和 `MISSING_TEXTURE_FALLBACK`；V0.1.12 返回 3、报告 `rejected` 和 `TEXTURE_READ_ERROR`，不生成中间包，输入 FBX 与目录保持原样。见[前后对比](evidence/V0.1.12/texture-path-before-after.json)。此样例用于复现路径判定错误，不代表用户原始大模型的贴图实际存在。

新增回归覆盖：

- 目录占据图片路径、普通文件占据父路径：两种 profile 与两种缺图策略均拒绝，错误包含具体路径，不产生缺图回退告警。
- 首先命中的无效路径不会被附加目录中的同名有效图片掩盖；真正缺失的图片仍能从附加目录找到并保留原始散列。
- POSIX FIFO 在打开前拒绝；循环链接产生读取错误；无搜索权限的目录不能冒充缺图，即使旁边存在有效同名图片也不能静默替换。
- `alias/../checker.png` 经过目录符号链接时，与文本简化后的 `checker.png` 可能对应不同位置。去重保留这些候选差异，仍能找到可用的同名贴图。
- Python 在严格与 GIS 静态模式下收到结构化 `ConversionError`、退出码 3 和 `TEXTURE_READ_ERROR`；GUI 展示路径 / 访问修复建议，保留失败报告，不生成 GDB。
- 既有缺图、透明度别名、无 UV 缺图、有效图片 UV 拒绝、损坏图片拒绝、有效与缺失贴图混合场景继续通过；三份专项成功 GDB 均复制到独立目录回读。

一次 FBX 读取内复用路径查询结果并去掉相同候选，不缓存图片字节跨次使用，也不省略实际读取时的类型 / 大小 / 完整内容检查。本次未量化运行时间或内存收益。

## 构建过程与证据

Windows 首轮 CTest 为 8/9：新增“父路径为普通文件”用例暴露 Windows 将该情形映射为路径不存在。已增加最近已有父目录检查，完整重建及最终 CTest 为 9/9。后续复查改为按原始候选路径去重，补充符号链接 / `..` 场景并在两平台重新构建及回归。首轮失败与最终成功记录分别保留。

原始结果的用户 / 工作目录已替换为占位符，并去除日志行尾空白；证据位于 [evidence/V0.1.12](evidence/V0.1.12)。

- Windows：[最终 CTest](evidence/V0.1.12/windows-ctest-final.log)、[贴图专项](evidence/V0.1.12/windows-missing-textures.log)、[Python 真实调用](evidence/V0.1.12/windows-python-client.json)、[部署](evidence/V0.1.12/windows-deployment.json)、[GUI](evidence/V0.1.12/windows-gui.json)。
- Ubuntu：[最终 CTest](evidence/V0.1.12/ubuntu-ctest-final.log)、[贴图专项](evidence/V0.1.12/ubuntu-missing-textures.log)、[Python 真实调用](evidence/V0.1.12/ubuntu-python-client.json)、[部署](evidence/V0.1.12/ubuntu-deployment.json)。

本版不改变几何、贴图编码或法线修复规则；未重跑用户大模型，其转换证据仍属于 [V0.1.8](validation-v0.1.8.md)。未制作 ATBX，也未进行三维外观验收。真正缺图仍按用户要求默认回退；此修复只防止把明确的路径 / 读取错误错误地当成缺图放行。
