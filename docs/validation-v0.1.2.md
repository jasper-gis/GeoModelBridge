# V0.1.2 验证记录

日期：2026-09-20。此次迭代增加 Windows 中文 GUI，并将 CLI、两个写入后端及 GUI 版本统一为 **0.1.2**。以下结果分别记录核心测试、界面逻辑测试和实际窗口操作，不复用旧版结果冒充本版重跑。

## 已确认结果

| 验证项 | 结果与范围 | 证据 |
|---|---|---|
| C++ 核心与 CLI | CTest **4/4 通过**：core、cli、reader_integration、binary_fbx | [原始 CTest 日志](evidence/V0.1.2/core-tests.log)、[构建日志](evidence/V0.1.2/build.log) |
| GUI 共享逻辑 | **84/84 通过**，失败 0；包含参数校验、安全进程参数、后端探测、报告真实性与覆盖保护 | [逐项结果](evidence/V0.1.2/gui-tests.json) |
| 原生后端集成 | 实际写库、源文件不可用时的独立副本核验通过；**29 项**错误输入、覆盖保护与临时目录处理检查通过 | [原生集成结果](evidence/V0.1.2/native-tests.json) |
| Pro 后端集成 | 实际写库、源文件不可用时的独立副本核验通过；**19 项**错误输入、覆盖保护与临时目录处理检查通过 | [Pro 集成结果](evidence/V0.1.2/pro-tests.json) |
| 真实原生转换 | GUI 使用的服务层实际调用 V0.1.2 原生后端，带贴图 FBX 写库/重开核验通过；路径含中文、空格和 `&`，重复转换拒绝覆盖 GDB 与报告 | [GUI 服务测试](evidence/V0.1.2/gui-tests.json)，`real_native_textured_conversion_from_gui_service` 等项 |
| 后端运行条件 | 本机 V0.1.2 原生后端返回可用且无需 Pro；Pro 后端初始化已安装许可后返回可用 | [构建日志末尾](evidence/V0.1.2/build.log) |
| 发布方式 | Windows x64 WPF 自包含单文件 `geomodelbridgeGUI.exe`，**71,728,518 字节**（约 71.7 MB），产品版本 **0.1.2**；同目录保留 CLI 和后端组件 | `dist/bin/geomodelbridgeGUI.exe` |
| 源码版本 | 12 处必需版本声明及运行时代码版本检查通过 | 构建日志、`scripts/check_version.py` |
| 运行时许可 | 两个 NuGet 运行时包为 **8.0.26**；包 SHA-512、原始许可条目及复制字节核对通过 | [来源与散列清单](evidence/V0.1.2/dotnet-runtime-licenses.json)、[第三方说明](../THIRD_PARTY_NOTICES.md) |
| 交付清理 | 已删除原 V0.1.0、V0.1.1 ZIP 及各自 `.zip.sha256`；本版不生成 ZIP | 交付保留工程目录，历史样例/报告/证据不改版 |

GUI 逻辑测试是独立 .NET 控制台测试，调用与窗口相同的校验和转换服务。测试结果中的 `graphical_acceptance_performed:false` 表示这些自动测试没有进行目标三维场景的外观验收，也不能替代真实 GUI 窗口操作。

构建过程中有一处 MSVC `getenv` 的 C4996 弃用提示；编译及全部 CTest 均完成，未将该提示记录为“零警告”。

## 实际 GUI 操作

使用最终发布的 `geomodelbridgeGUI.exe` 实际打开窗口并操作，完成以下检查：

| 操作 | 结果 |
|---|---|
| 启动与显示 | 界面正常打开，标题与产品版本为 0.1.2；按钮文字对比度问题已修正 |
| 空白参数 | 以中文提示缺失字段，不开始转换；此项在最终按钮颜色与状态区布局调整前检查，参数校验逻辑未改变 |
| 原生运行条件 | 点击“检查运行环境”，原生后端返回可用 |
| 加载演示 | 填入 WKID `32650`、原点 `500000 / 3000000 / 100`，显示橙色演示坐标提示 |
| 点击转换 | 生成新的 `GUI演示.gdb`，含 1 个 `Models` 要素；原生后端关库重开核验几何、材质、UV 和 PNG 纹理通过 |
| 查看报告 | 点击“查看转换报告”，窗口显示此次转换实际生成的 JSON |
| 重复转换 | 中文提示拒绝已有 GDB 和报告，已有成果及报告的全部 SHA-256 保持不变 |
| 切换输入模型 | WKID 和 X/Y/Z 全部清空，演示提示消失，避免将演示坐标沿用到其他模型 |
| 切换 Pro 并检查 | 返回 `license_initialized:true` 和 `version:0.1.2`；这是运行条件检查，不宣称在该窗口内又执行了一次 Pro 转换 |
| 关闭窗口 | 空闲时正常关闭 |

证据包括 [11 项窗口检查清单](evidence/V0.1.2/gui-ui-checks.json)、[转换成功截图](evidence/V0.1.2/gui-native-success.png)、[窗口状态](evidence/V0.1.2/gui-native-success.txt)、[实际转换报告](evidence/V0.1.2/gui-native-report.json)、[切换模型后的清空状态](evidence/V0.1.2/gui-demo-reset.txt) 与 [Pro 运行条件检查](evidence/V0.1.2/gui-pro-probe.txt)。这些检查验证转换软件界面及成果读回，仍与三维场景渲染验收分开记录。

最终 GUI EXE 的 SHA-256：`9271f88fd24d1b50a839a391279c7e1e75d110af4893713cdc42a8f54917288f`。

## 验证边界

- 本次重新运行了 Pro 19 项和原生 29 项集成回归，但没有重跑双后端各 14 例完整样例及其跨后端核验。旧记录保持原版本，见 [V0.1.1 验证记录](validation-v0.1.1.md)。
- GUI 复用现有转换流程；输出成功要求实际 GDB 存在，报告版本、后端、路径、坐标系、要素类、要素数和重开核验状态符合请求。单独进程退出码 0 不会被界面当作成功。
- 目标 Pro 导出的红青双影、完整颜色、Alpha 和六面/接缝外观仍需独立场景验收。本次 GUI 发布不表示这些旧待办已经解决。
- 未完成未安装 .NET/Pro 的全新 Windows 机器验收。GUI 自包含 .NET；原生后端仍需要本机 Microsoft C++ 运行库，Pro 后端仍需要已安装且许可可用的 Pro。

## 重现自动检查

在工程根目录执行，测试工作目录须为尚不存在的新目录：

```powershell
python .\scripts\check_version.py
python .\scripts\verify_dependencies.py
dotnet run --project .\apps\GeoModelBridge.Gui.Tests -c Release -- --work C:\temp\gmb-gui-v012-new --engine-dir .\dist\bin --fixtures .\tests\fixtures
python .\scripts\install_dotnet_licenses.py --assets .\apps\GeoModelBridge.Gui\obj\project.assets.json --output .\dist\licenses\dotnet
```

核心测试由 `scripts/build.ps1` 自动执行。GUI 发布使用 `-WithGui`，原生和 Pro 后端可分别加 `-WithNative`/`-WithPro`；完整参数见 [构建说明](../README.md)。
