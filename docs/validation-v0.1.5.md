# V0.1.5 原生独立部署验证

日期：2026-09-21。该版本移除 Pro 适配器及其构建、运行、授权探测和发布条件，仅保留原生 FileGDB。旧版源码保存在初始化提交 9343edd，历史报告和图形证据保持原始来源。

## 本机执行结果

| 检查 | 结果 | 证据 |
|---|---|---|
| C++ 核心、CLI、FBX、策略 | CMake Release 构建；CTest 5/5 通过，包含旧后端拒绝和托管 DLL 拒绝 | [构建日志](evidence/V0.1.5/build.log) |
| 原生写入端 | 带 PNG Alpha / JPEG 的实际写库、关闭重开、独立副本通过；30 项异常/保护检查通过 | [原生结果](evidence/V0.1.5/native-tests.json) |
| GUI 服务 | 138/138 通过，包括旧后端请求拒绝、报告拒绝、实际带贴图转换、防覆盖和日志限量 | [GUI 结果](evidence/V0.1.5/gui-tests.json) |
| 完整参考样例 | 14 例实际 GDB、独立副本、输入拒绝及写入失败报告通过 | [样例汇总](evidence/V0.1.5/examples.json) |
| 最小运行目录 | 只复制 CLI、原生 EXE/DLL 与示例；子进程 PATH 仅含 Windows 目录，Pro 变量指向不存在的路径；默认原生转换和带贴图回读、副本核验通过 | [部署测试](evidence/V0.1.5/native-deployment.json) |
| 依赖 | 依赖源码、发行 FileGDB API、原始许可散列一致；CLI、原生端、FileGDB DLL 和 GUI 的 PE 导入未见 Pro 程序集 | [二进制散列与导入](evidence/V0.1.5/release-dependencies.json) |
| 发布检查 | 原生运行库探测及全部 14 个实际样例重新打开核验通过；不生成 ZIP | `scripts/package.py --install-dir releases/V0.1.5 --check-only` |
| 发布保护 | 拒绝含旧后端的目录、保留已有 ZIP、拒绝错误 SDK 散列后不解压 | [保护检查](evidence/V0.1.5/release-checks.json) |
| SDK 获取 | 使用固定归档及文件 SHA-256 验证解压结果，通过 | `scripts/fetch_filegdb_sdk.py --archive ... --output build/sdk-fetch-check` |

本轮 C++ 构建有一项既有 `getenv` 的 MSVC C4996 弃用警告，没有编译错误。GUI 发布与服务测试通过；本轮没有新做窗口视觉验收。

## 工程与运行依赖

当前代码不引用 `ArcGIS.Core` / `ArcGIS.CoreHost`，不调用 ArcPy、Pro 路径发现或许可初始化；不再支持 `--backend arcgis-pro`。GUI 只显示原生 FileGDB。已删除旧后端及 Pro 专用脚本，V0.1.3 历史只读工具的可编译源码也只保存在 Git 历史。

原生程序仍使用独立 Esri FileGDB API 1.5.5、Windows 系统组件和 Visual C++ x64 运行库；GUI 自带 .NET。SDK 原始来源和许可不变。构建使用 MSVC 14.44 与 Windows SDK 10.0.22621.0；本地程序保存在 `releases/V0.1.5`，旧 `dist` 和历史版本未被覆盖。

GitHub Actions 增加 `native-windows`：在 Windows Server 2022 runner 检查 Pro 默认安装目录和注册项不存在，获取固定 SDK，构建原生端与 GUI，执行原生集成、GUI 服务、最小部署、样例和发布检查。实际运行结果以 [仓库 Actions](https://github.com/jasper-gis/GeoModelBridge/actions/workflows/build.yml) 对应提交为准。

## 证据边界

本机安装着 Pro；本机结果是代码/二进制依赖检查和受限子进程环境验证，不冒充从未安装 Pro 的 Windows 11 客户机验收。CI 的 Windows Server 2022 环境与客户 Windows 11 实机验收也应分别记录。

本轮不改变 Scene Bundle、材质/纹理处理、坐标或法线量化，不宣称新增渲染保真能力。完整色彩、Alpha、接缝及历史双影的目标软件显示验收仍独立进行。历史跨 SDK 回读记录作为已有证据保留，不再成为运行或发布要求。
