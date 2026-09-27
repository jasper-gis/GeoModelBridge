# V0.4.0 验证状态（2026-09-27）

此版增加 glTF 2.0 GLB 静态模型读取。以下仅记录当前版本实际执行的检查。历史版本的真实 GDB 回读不等同于 V0.4.0 的 GLB 原生写入核验。

## 已完成

- Windows 核心构建：`cmake --preset core-release`、`cmake --build --preset core-release -j 4`、`ctest --preset core-release --output-on-failure`，11/11 通过。GLB 用例覆盖坐标与原点、UV 原点及变换、镜像实例、嵌入/外置图片、缺图策略、动画保存姿态、PBR 策略、法线修复、无效缓冲区与路径、以及无法表示的渲染语义拒绝；`output_safety` 原有并发与链接路径检查仍通过。
- Windows GUI：`dotnet build apps/GeoModelBridge.Gui/GeoModelBridge.Gui.csproj -c Release` 成功，0 警告；`dotnet run --project apps/GeoModelBridge.Gui.Tests -c Release -- --work build/gui-glb-final`，224/224 服务测试通过（不含真实 FileGDB 转换）。
- `python scripts/verify_dependencies.py` 与 `python scripts/check_version.py` 通过，包含 cgltf 1.15 与固定 FileGDB SDK 文件的哈希校验。Python 客户端单元契约包含 `.glb` 请求。

## 尚待验证

- 本机无 MSVC x64 工具链；`cmake --preset release` 在 CMake 的 FileGDB SDK/MSVC ABI 检查处终止。没有可用的 Ubuntu WSL 发行版。[本版 GitHub Actions 运行](https://github.com/jasper-gis/GeoModelBridge/actions/runs/36288264581) 的原生 Windows/Ubuntu 作业没有获得运行器；GitHub 检查注释为 “The job was not started because your account is locked due to a billing issue.” 本版 GLB 的 Windows 与 Ubuntu 原生 FileGDB 写入、关闭重开、独立 GDB 副本核验、安装后 Python 客户端真实转换及 GUI 服务真实转换尚未执行。代码和 CI 已加入这些用例，运行通过前不能称 V0.4.0 GLB→FileGDB 已经通过原生验收。
- 目标 GIS 中的三维视觉验收为单独检查。GLB 的 UV 图片方向与透明度虽经过 Bundle 层检查，真实 FileGDB 回读与目标软件显示仍需核验。
