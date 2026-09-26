# V0.3.0 验证状态（2026-09-26）

此版增加 OBJ/MTL 读取与共享 Scene Bundle 转换入口。此记录只描述本版实际执行的检查；历史 V0.2.2 的 FileGDB 证据不等同于本版 OBJ 入库核验。

## 已完成

- Windows 核心构建：`cmake --preset core-release`、`cmake --build --preset core-release`、`ctest --preset core-release --output-on-failure`，10/10 通过，包括 OBJ/MTL 角点、材质、贴图、定位及路径/回退策略检查。
- Windows GUI：`dotnet build apps/GeoModelBridge.Gui/GeoModelBridge.Gui.csproj -c Release` 成功；`dotnet run --project apps/GeoModelBridge.Gui.Tests -c Release -- --work build/gui-obj-test`，222/222 服务测试通过。该次未执行 GUI 服务的真实 GDB 转换。
- `python scripts/verify_dependencies.py` 与 `python scripts/check_version.py` 通过；FileGDB SDK 1.5.5.330 已按固定哈希下载核对。

## 尚待验证

- 本版 OBJ 的 Windows 与 Ubuntu 原生 FileGDB 写入、关闭后重开、独立复制 GDB 核验，以及安装后 Python 客户端的真实 OBJ GDB 转换尚未执行，不能据此宣称 OBJ→FileGDB 已通过原生验收。
- [GitHub Actions 本版运行](https://github.com/jasper-gis/GeoModelBridge/actions/runs/36226969027) 中各任务因账户计费锁定而未启动；本机缺少 MSVC C++ 工具链与 Ubuntu 环境。工作流已包含双平台原生与安装客户端检查，需在运行环境恢复后执行并记录结果。
- 目标 GIS 中的三维视觉验收依旧是独立检查。
