# V0.1.14 验证记录

2026-09-22，在 Windows x64（MSVC 19.44、Python 3.11）和 Ubuntu 24.04.4 x86_64（GCC 13、Python 3.12）验证。此次仅重组构建与配置入口、调整默认构建范围和输出布局，更新版本及文档；模型解析、Scene Bundle、原生存储语义和输出保护代码不变。

## 统一构建结果

| 检查 | Windows | Ubuntu |
| --- | --- | --- |
| 根工程完整 CMake/CTest | 11/11 | 11/11 |
| 安装前默认 writer 查找、纹理 GDB 与复制回读 | 通过，纳入 CTest | 同左 |
| SDK 运行库散列及脱离开发 SDK 路径的 probe | 通过，纳入 CTest | 同左，含 ldd 实际加载位置核对 |
| 原生写入、回读、输入拒绝 / 清理检查 | 37 项通过，含 21 万角点流式输入 | 同左 |
| 可搬迁安装、连续 CLI 调用、冲突保护、复制回读 | 通过 | 通过 |
| 安装后的 Python 真实调用 | 9/9，4 份 GDB 独立复制回读 | 同左 |
| Python 契约单独执行 | 25 通过、1 项链接构造因权限跳过 | 26/26 |
| GUI 服务及真实转换 | 163/163 | 不适用 |
| 完整样例与发布检查 | 14/14、check-only 通过 | 同左 |
| 纯核心模式 | core-release 构建及 CTest 9/9 | core-release 无 SDK 配置通过；核心运行测试包含于完整构建的 11 项 |
| 多配置生成器 | Ninja Multi-Config Release，两个原生 CTest 通过 | 未单独执行 |

Windows 使用新的 `build/v0114-unified`，由 PowerShell 脚本一次配置、构建、测试和安装，并另行发布 GUI 到 `releases/V0.1.14`。Ubuntu 使用新的 `build/v0114-unified`，由 Python 脚本以 `FILEGDB_API_ROOT` 指定已有 SDK，未传 `--sdk`。此前版本安装和已有用户成果保留。

根项目只保留一个 `CMakeLists.txt`；后端文件已删除，没有另建 CMake 子工程或 include 文件绕回旧结构。版本检查对应移除后端独立 project 声明，当前 6 项必需版本声明一致。

## 新增与重点回归

- `native_build_layout` 在系统临时目录启动构建树中的 CLI，清除 writer / SDK 环境覆盖，不传 `--writer`。`doctor` 必须定位到 CLI 旁 `native-filegdb` 中的目标 writer；转换带贴图 FBX，检查 WKID / origin，再复制 GDB 并在新进程中核验。
- 单配置构建输出位于 `<build>/bin`；Windows Ninja Multi-Config 输出位于 `<build>/bin/Release`，writer 和 DLL 保持相同相对布局。多配置测试只构建 `geomodelbridge` 目标，验证它确实带出 writer 与运行库，没有先手动构建后端。
- Windows `release` preset 以相对 `FILEGDB_API_ROOT=build/sdk-fetch-check` 完成构建与两个原生 CTest，验证 SDK 路径规范化。另在 x64 MSVC 开发环境中不指定 C/C++ 编译器，根 preset 正确识别 MSVC。
- 指向不存在的 SDK 时，根配置失败且提示 `fetch_filegdb_sdk.py` 和显式核心模式，未静默降级为没有 writer 的“成功构建”。纯核心 preset 显式关闭 native，不要求 SDK。
- 包含和不包含 SDK 运行库的配置开关、Windows release STL ABI 限制、Linux `$ORIGIN`、原始 SDK 许可和固定散列规则均迁移到根文件。核心与原生输出继续使用原有共享安全文件操作。

主要证据：[Windows 完整构建](evidence/V0.1.14/windows-build.log)、[Ubuntu 最终构建](evidence/V0.1.14/ubuntu-build-final.log)、[Windows 多配置](evidence/V0.1.14/windows-multiconfig.log)、[Windows release preset](evidence/V0.1.14/windows-release-preset.log)、[缺 SDK 提示](evidence/V0.1.14/windows-missing-sdk.log)、[Windows 部署](evidence/V0.1.14/windows-deployment.json)、[Ubuntu 部署](evidence/V0.1.14/ubuntu-deployment.json)。日志、报告中的用户和工作路径已脱敏，集中于 [evidence/V0.1.14](evidence/V0.1.14)。

本次发布检查只执行 `--check-only`，没有生成本地归档。没有重新执行历史用户大模型、ATBX 或目标 GIS 软件图形外观验收；历史证据保持原始版本。多配置验证使用 Ninja Multi-Config，不将其表述为已经测试 Visual Studio IDE 的所有配置。
