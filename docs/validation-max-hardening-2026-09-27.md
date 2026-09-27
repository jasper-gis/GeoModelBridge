# MAX 适配器优化验证 — 2026-09-27

本次为 V0.6.0 后的修复，版本号保持 `0.6.0`，变更列入 Unreleased。新的安装目录为 `releases/V0.6.0-hardening`；此前发布产物和历史验证记录保留。

## 修改范围

- 网格与材质使用名称索引，纹理 SHA-256 在每次导出核验中各计算一次，避免按材质及资源反复扫描与哈希；未进行大型真实 MAX 性能基准测试。
- 清单要求覆盖全部导出的网格、材质及纹理内容，拒绝重复网格、重复材质、遗漏材质、遗漏纹理及重复纹理记录。
- 可选系统/监听日志无法读取时保留 `MAX_LOG_READ_ERROR` 警告，不覆盖原始超时、worker 拒绝或后续核验成功结果。模型、图片、完成清单等必需资源仍严格校验。

## 本次执行结果

| 检查 | Windows x64 | Ubuntu 24.04 x86_64 |
| --- | --- | --- |
| 根 CMake 完整原生构建 / CTest | 17/17 | 17/17 |
| 官方依赖哈希、安装包依赖、版本一致性 | 通过 | 通过 |
| 原生 FileGDB 写入、关闭重开、迁移副本验证 | 通过；83 项异常检查 | 通过；84 项异常检查 |
| 最小部署，OBJ / GLB / glTF / WRL 纹理 GDB 与独立复制回读 | 通过 | 通过 |
| 标准库 Python 客户端契约 | 33 项，3 项平台跳过 | 33 项，1 项平台跳过 |
| 安装后 Python 客户端真实 GDB | 13 场景，8 个 GDB 独立复制回读 | 13 场景，8 个 GDB 独立复制回读 |
| 法线修复 / 缺图策略回归 | 14 / 24 项 | 14 / 29 项 |
| GUI 服务及原生转换 | 245/245 | 不适用 |
| 实现、构建、测试、依赖文件 SHA-256 一致性 | 270 文件通过 | 相同 270 文件通过 |

Windows MAX 协议替身回归覆盖 25 种运行模式，包括多网格、多材质、错误清单、超时、日志截断及日志读取失败。安装后客户端经替身预处理写入真实纹理 GDB，并独立复制重开；六类错误导出清单全部在创建 GDB 前被拒绝，日志故障也保留已核验转换结果。

## 证据与复现

原始测试日志及机器可读结果保存在 [本次证据目录](evidence/max-hardening-2026-09-27/)，包括双平台构建日志、原生集成结果、部署结果、Python 结果、GUI 结果及源码哈希清单。

```powershell
# Windows：在已配置 x64 MSVC 的终端中，使用新的安装与工作目录。
scripts/build.ps1 -WithGui -BuildDirectory build/v060-native -InstallDirectory releases/V0.6.0-hardening
python tests/max_adapter_native_test.py --install-dir releases/V0.6.0-hardening --stub build/v060-native/bin/gmb_max_batch_stub.exe --work artifacts/max-hardening/max-native
```

```sh
# Ubuntu：SDK 路径按环境指定。
python3 scripts/build.py --sdk /home/gmb/filegdb-sdk --build-dir build/native --install-dir releases/V0.6.0-hardening --jobs 3
```

双平台另执行 `backends/native-filegdb/tests/integration.py`、`tests/native_deployment_test.py`、`tests/python_client_test.py`、`tests/python_client_integration_test.py`、法线及缺图回归；Windows 另执行 `apps/GeoModelBridge.Gui.Tests`。

**未完成的验证：** 本次未使用真实 Autodesk Max Batch，也未读取真实 `.max` 样本。协议替身不证明 MAX 导出插件、许可、渲染材质或真实场景的兼容性；目标 GIS 软件视觉验收亦未执行。Linux 仍接受导出的 FBX / Scene Bundle，不直接运行 MAX 预处理。真实 MAX 验收入口及接受边界见 [MAX 适配器说明](max-adapter.md)。
