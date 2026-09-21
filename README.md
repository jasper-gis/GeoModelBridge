<div align="center">

<h1>GeoModelBridge</h1>
<p><strong>将静态三维模型转换为 GIS 数据</strong></p>
<p>FBX → 带颜色与贴图的 FileGDB Multipatch</p>

<p>
  <a href="CHANGELOG.md"><code>V0.1.11</code></a> &nbsp;
  <a href="docs/architecture.md"><code>C++17</code></a> &nbsp;
  <a href="#platforms"><code>Windows · Ubuntu</code></a>
</p>

<p>
  <a href="#quick-start">快速开始</a> ·
  <a href="#usage">使用方法</a> ·
  <a href="docs/build-and-release.md">部署指南</a> ·
  <a href="#verification">验证结果</a> ·
  <a href="#documentation">文档导航</a>
</p>

</div>

---

GeoModelBridge 使用 **原生 FileGDB 后端 `native-filegdb`**，支持 Ubuntu 命令行与 Windows 命令行 / 图形界面。构建、转换和部署均无需安装 ArcGIS Pro、ArcPy 或获取 Pro 许可。

- **保留模型表达**：处理漫反射颜色、PNG/JPEG 纹理、Alpha、UV 和法线角点边界，节点变换只烘焙一次。
- **写入后核验**：生成真实 GDB，关闭并重开，核对几何、材质、纹理和空间参考后才报告成功。
- **缺图也能继续**：缺失的图片默认回退为材质颜色和标量透明度，保留警告及原路径；有效贴图照常写入。
- **共用转换链路**：两平台使用同一套 C++17 核心、Scene Bundle 协议和报告格式，转换不覆盖已有模型或成果。

```mermaid
flowchart LR
    A[FBX + 纹理] --> B[解析与校验]
    B --> C[Scene Bundle]
    C --> D[原生 FileGDB 写入]
    D --> E[GDB + 回读报告]
```

<a id="platforms"></a>

## 平台支持

| 平台 | 使用方式 | 客户机运行要求 |
| --- | --- | --- |
| **Ubuntu 24.04 LTS · x86_64** | 完整 CLI，无需桌面会话或 .NET | 系统 C++ / PNG / JPEG 运行库及两份 FileGDB `.so` |
| **Windows · x64** | 完整 CLI + WPF 中文 GUI | Visual C++ x64 Runtime、FileGDBAPI.dll；GUI 自带 .NET |

Ubuntu ARM64、Ubuntu 22.04、macOS 和其他 Linux 发行版暂不作为已验证支持目标。WPF GUI 仅支持 Windows。详见[客户机部署](docs/build-and-release.md#客户机部署)和[平台验证记录](docs/validation-v0.1.6.md)。

<a id="quick-start"></a>

## 快速开始

**已有完整安装目录？** 直接阅读[客户机部署](docs/build-and-release.md#客户机部署)或[使用方法](#usage)。源码仓库不包含编译程序与完整 SDK；首次使用源码按下方对应平台构建。

<details open>
<summary><strong>Ubuntu 24.04 · 构建命令行与原生写入端</strong></summary>

```bash
sudo apt-get update
sudo apt-get install -y git g++ cmake ninja-build python3 libpng-dev libjpeg-dev

git clone https://github.com/jasper-gis/GeoModelBridge.git
cd GeoModelBridge
python3 scripts/fetch_filegdb_sdk.py --output build/filegdb-sdk
python3 scripts/build.py --sdk build/filegdb-sdk --include-runtime --jobs 2

./dist/bin/geomodelbridge doctor
./dist/bin/native-filegdb/GeoModelBridge.NativeWriter --probe
```

脚本核对固定 SDK 的 SHA-256，完成 Release 构建、核心测试与安装。`--include-runtime` 将官方 FileGDB 运行库及许可一起放入 `dist`，方便复制到客户机。

</details>

<details>
<summary><strong>Windows x64 · 构建命令行与中文 GUI</strong></summary>

准备 Git、Visual Studio 2022 C++ x64 工具链 / Windows SDK、CMake、Ninja、Python ≥ 3.11 和 .NET 8 SDK。在 **x64 Visual Studio Developer PowerShell** 中执行：

```powershell
git clone https://github.com/jasper-gis/GeoModelBridge.git
cd GeoModelBridge
python scripts/fetch_filegdb_sdk.py --output build/filegdb-sdk
.\scripts\build.ps1 -WithNative -WithGui -FileGDBApiRoot "$PWD/build/filegdb-sdk" -IncludeFileGDBRuntime
.\dist\bin\geomodelbridgeGUI.exe
```

在界面选择 FBX、新的输出 GDB、目标 WKID 与米制原点坐标，然后转换。详见[GUI 使用说明](docs/gui.md)。

</details>

SDK 下载目录必须为新路径。后续修改源码时重新执行构建命令即可，不必重复下载。离线 SDK、手动 CMake、自定义安装目录及发布操作见[构建与发布指南](docs/build-and-release.md)。

<a id="usage"></a>

## 使用方法

### 先跑通演示模型

Ubuntu 在仓库根目录执行；Windows 将程序路径替换为 `.\dist\bin\geomodelbridge.exe`，并将命令写在同一行：

```bash
./dist/bin/geomodelbridge convert dist/bin/demo/textured_quad.fbx \
  --output new-demo.gdb --wkid 32650 --origin 500000 3000000 100
```

成功后得到 `new-demo.gdb` 和 `new-demo.gdb.report.json`。报告中 `status=written_and_readback_verified` 表示已完成数据库写入与回读核验。再次执行时换用新的输出名称。

> [!IMPORTANT]
> 示例 WKID 与原点仅用于测试。转换实际模型时必须填写真实定位参数：**WKID 赋值和 origin 平移不执行重投影或地理配准**。目标坐标系须为 SDK 支持的米制投影坐标系。

### 检查、准备与转换

```bash
# 检查模型与诊断信息。
./dist/bin/geomodelbridge inspect model.fbx --report new-inspection.json

# 生成中间 Scene Bundle，尚未写入 GDB。
./dist/bin/geomodelbridge prepare model.fbx --output new-bundle

# 显式使用 GIS 静态兼容策略转换真实模型；定位参数需自行替换。
./dist/bin/geomodelbridge convert model.fbx --output new-model.gdb \
  --profile gis-static --wkid 32650 --origin 500000 3000000 100
```

| 常用参数 | 用途 |
| --- | --- |
| `--profile strict\|gis-static` | CLI 默认 `strict`；兼容策略需显式选择 |
| `--texture-dir DIR` | 添加外部贴图搜索目录，可重复指定 |
| `--missing-textures material-color\|error` | 默认缺图时使用材质颜色继续；`error` 要求引用的贴图完整 |
| `--feature-class NAME` | 输出要素类名称，默认 `Models` |
| `--report NEW.json` | 指定新的报告路径 |
| `--writer PATH` | 指定原生写入程序，也可设置 `GMB_NATIVE_WRITER` |

`--backend native-filegdb` 可以省略。含空格的路径需加引号，支持中文文件名。已有 Bundle 写入、复制 GDB 的独立回读命令见[原生后端说明](backends/native-filegdb/README.md)；退出码与常见问题见[排错指南](docs/build-and-release.md#环境检查与排错)。

### Python 调用与后续工具箱接入

V0.1.9 提供标准库实现的 `geomodelbridge` Python 调用库，构建时安装到发布目录的 `python/`。后续 ArcGIS ATBX 脚本可导入它，复用参数检查、EXE 调用、诊断和 GDB 回读结果核验；库本身无需 ArcPy。

```python
import sys
sys.path.insert(0, r"D:\Tools\GeoModelBridge\python")

from geomodelbridge import ConversionRequest, Engine

result = Engine(r"D:\Tools\GeoModelBridge\bin\geomodelbridge.exe").convert(
    ConversionRequest(r"D:\Models\model.fbx", r"D:\Results\new-model.gdb",
                      wkid=3857, origin=(100, 100, 100), profile="gis-static")
)
print(result.feature_class_path)
```

替换示例中的完整发布目录、模型 / 输出路径和定位参数。当前接口同步创建**新的 GDB**，不追加到已有库；本次不含 ATBX 文件或取消接口。调用库核对报告中的 WKID / XYZ、限制进程日志占用，并在完成回调异常时通过 `CallbackError.result` 保留已验证成果。完整接入方式见 [Python 函数库文档](docs/python-client.md)，可运行示例位于 [python/examples/convert_fbx.py](python/examples/convert_fbx.py)。

V0.1.11 加强中间包与报告的并发输出保护：同名目标已被其他任务创建时提交失败，保留已有内容。输出及父目录不得经过符号链接 / Windows 重解析点；改用实际目录并为每个任务指定独立名称。详见[输出保护约定](docs/architecture.md#输出保护约定)。

### 转换策略与边界

| 策略 | 行为 |
| --- | --- |
| **strict** · CLI 默认 | 严格校验，不自动省略不支持的渲染语义 |
| **gis-static** · GUI 默认明确展示 | 使用文件保存姿态；允许省略环境光 / 高光 / 反射、移除有限零面积三角形，并从有效三角形重建无效角点法线；逐项记录 |

**缺图策略独立于上表的渲染策略。** 两种 profile 默认均允许缺图回退；GUI 中“缺少贴图时使用材质颜色继续转换”默认勾选。报告以 `MISSING_TEXTURE_FALLBACK` 列出受影响材质与图片路径，回退不能恢复图片的原有外观。若要求完整贴图，取消勾选或加 `--missing-textures error`。

**法线修复需选择 `gis-static`。** 仅用所在三角形的面法线替换无效角点，保留原有有效法线、UV 和材质边界；报告记录 `NORMALS_REPAIRED` 及数量。局部光照可能变硬，严格模式继续拒绝无效法线。

有效贴图缺少 UV、非有限位置、无法形成有效三角形的几何、损坏或无法读取的现有图片、未知材质以及不支持的 PBR、自发光、骨骼或形变仍会拒绝。PNG 保存为未预乘 RGBA8，JPEG 保留压缩数据，不静默转码；颜色、透明度、UV 与法线的存储量化会记录在报告中。详细格式范围、JPEG 封装兼容处理及精度说明见[兼容策略](docs/compatibility.md)和[原生存储规则](backends/native-filegdb/README.md#存储规则与范围)。

<a id="verification"></a>

## 已有验证

**V0.1.11**：Windows / Ubuntu 各通过 7 项真实 Python 调用场景及 4 份 GDB 的独立复制回读；CTest 各 9/9，原生后端、独立部署与 14 个样例均通过。Python 契约测试为 Windows 25 通过 / 1 跳过、Ubuntu 26/26；Windows GUI 为 161/161。新增输出保护测试在 Windows 为 7 组通过 / 1 组链接检查跳过，Ubuntu 为 8/8。详情见[本版验证记录](docs/validation-v0.1.11.md)，Python 调用库首版见 [V0.1.9](docs/validation-v0.1.9.md)。未进行 ATBX 内运行或取消行为验收。

以下保留 **V0.1.8 实际 Windows / Ubuntu 执行结果**，大模型与法线专项的来源见[历史验证记录](docs/validation-v0.1.8.md)。

| 检查 | 结果 |
| --- | --- |
| 核心与 CLI 回归 | 两平台各 **7/7** |
| 完整 GDB 样例 | 两平台各 **14/14**，几何与纹理回读散列一致 |
| 法线专项 | 两平台各 **14/14**，包含修复后 GDB 的复制回读 |
| 大模型 | Windows 实际转换 **4,349 个要素、1,083 万三角形**，完成独立复制回读 |
| Windows GUI 服务回归 | **153/153**，包含真实缺图与法线修复转换 |
| 独立目录部署 | 两平台通过 |

Windows ↔ Ubuntu 双向互读及归档解压验证见 [V0.1.6 历史记录](docs/validation-v0.1.6.md)。数据库回读与目标软件三维外观验收分别进行；图形验收边界见[验收说明](docs/acceptance.md)。[GitHub Actions](https://github.com/jasper-gis/GeoModelBridge/actions/workflows/build.yml) 展示托管 CI 的实际状态，以上数字来自已保存的本地验证记录。

<a id="documentation"></a>

## 文档与维护

| 你想了解 | 文档 |
| --- | --- |
| 客户机部署、离线构建、排错、测试与打包 | [构建与发布指南](docs/build-and-release.md) |
| Windows 图形界面操作 | [GUI 使用说明](docs/gui.md) |
| Python 函数调用、后续 ATBX 接入与错误处理 | [Python 调用库](docs/python-client.md) |
| 支持的模型、材质与兼容行为 | [兼容策略](docs/compatibility.md) |
| 原生写入、存储精度与独立回读 | [FileGDB 后端](backends/native-filegdb/README.md) |
| 共享核心、平台适配与数据契约 | [架构](docs/architecture.md) · [Scene Bundle](docs/bundle-format.md) |
| 版本变化与历史证据 | [CHANGELOG](CHANGELOG.md) · [历史记录](docs/evidence/README.md) |

项目直接在 **master** 维护一套共享代码，平台差异收敛到文件操作和图片解码适配层。版本以 [VERSION](VERSION) 为准；贡献约定见 [AGENTS.md](AGENTS.md)。SDK 与第三方依赖的来源、散列和许可见[第三方说明](THIRD_PARTY_NOTICES.md)。**项目自身尚未选择开源许可证。**
