# DAE 支持验证 — 2026-09-30

本次变更列入 Unreleased，版本保持 `0.6.0`。Windows 安装产物位于 `releases/DAE-20260930`，此前发布和验证记录保留。接受范围及拒绝策略见 [DAE 输入说明](dae.md)。

## 执行结果

| 检查 | Windows x64 | Ubuntu 24.04.4 x86_64 |
| --- | --- | --- |
| 根 CMake 完整原生构建 / CTest | 18/18 | 18/18 |
| 原生 FileGDB 集成、重开及独立副本验证 | 通过；83 项异常检查 | 通过；84 项异常检查 |
| 最小部署及 FBX / OBJ / GLB / glTF / WRL / DAE 纹理 GDB | 通过 | 通过 |
| 安装后标准库 Python 客户端 | 14 场景、9 份 GDB 独立复制回读 | 14 场景、9 份 GDB 独立复制回读 |
| Windows GUI 服务及真实 DAE 转换 | 247/247 | 不适用 |
| 依赖 SHA-256、版本一致性、打包清单 | 通过 | 通过 |
| 最终源码校验集 | 329 文件匹配 | 相同 329 文件匹配 |

DAE 回归通过实际 XML 解析验证矩阵行序文本、变换顺序、父子与镜像实例、X/Y/Z 向上轴和单位转换、逆转置法线、独立角点 UV/法线、多材质、accessor offset/stride、标量透明度、缺图回退、法线修复和退化面处理。错误索引、循环/外部引用、DTD、路径逃逸、读取错误、未知材质/扩展、变形与动画在创建 GDB 前拒绝。并发输出及链接路径仍由 `output_safety` 覆盖。

`native_build_layout` 另生成项目自制 RGBA PNG，包含 Alpha 值 255、128、64、0，以相同 sampler/UV 显式绑定 diffuse 和 transparent/A_ONE，标量不透明度为 0.75。双平台真实 GDB 关闭重开后，原 PNG SHA-256、存储 RGBA 像素 SHA-256 和纹理回读均一致，材质透明度独立核对为 25%；复制数据库后再次独立验证。没有重新压缩源图片或猜测透明度绑定。

## 证据和复现

原始日志、机器可读结果、最终源码校验集见 [证据目录](evidence/dae-2026-09-30/)。包含最终双平台 CTest、部署、Python、原生验证及 Windows GUI 结果；校验集包括实现、构建、测试、依赖及现有文档，不包含证据目录本身。

```powershell
# 已配置 x64 MSVC 与官方 SDK；使用新的安装和测试目录。
scripts/build.ps1 -WithGui -BuildDirectory build/dae-native -InstallDirectory releases/DAE-20260930
python tests/native_deployment_test.py --install-dir releases/DAE-20260930 --work artifacts/new-deployment
python tests/python_client_integration_test.py --install-dir releases/DAE-20260930 --work artifacts/new-python
dotnet run --project apps/GeoModelBridge.Gui.Tests -c Release -- --work artifacts/new-gui --engine-dir releases/DAE-20260930/bin --fixtures tests/fixtures
```

```sh
python3 scripts/build.py --sdk /path/to/filegdb-sdk --build-dir build/native --install-dir releases/DAE --jobs 3
python3 tests/native_deployment_test.py --install-dir releases/DAE --work artifacts/new-deployment
python3 tests/python_client_integration_test.py --install-dir releases/DAE --work artifacts/new-python
```

## 验收边界

样本为项目自制 COLLADA 1.4.1，尚未执行外部导出器或用户生产 DAE 样本验收，目标 GIS 软件视觉验收也未执行。文件格式支持不代表所有 COLLADA 场景和渲染语义均可无损导入。

Windows 一轮 GUI 回归使用较长测试目录，纹理暂存路径达到 264–265 字符，触发现有 Win32 长路径限制，结果为 243/247；缩短测试工作目录后，同一最终实现通过 247/247。失败日志及结果一并保留为 `windows-gui-long-path-failure.*`。本次未修复这一现有长路径边界，Windows 应避免过长的模型/输出及测试工作目录。
