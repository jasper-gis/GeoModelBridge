# Python 调用库 · V0.1.9

`geomodelbridge` 把 FBX 入库封装为普通 Python 函数调用，供以后 Windows ArcGIS `.atbx` 的脚本层复用，也可用于独立 Python 脚本或 Ubuntu。库仅使用 Python 标准库，不导入 `arcpy`，不在 Python 进程内加载 FileGDB SDK；实际转换仍由同版本 EXE 完成。

本次交付函数库、普通 Python 示例和测试，未制作 `.atbx` / `.pyt`，未进行 ArcGIS 工具箱内运行验收。

## 目录与安装

构建会自动安装以下结构；打包检查会拒绝缺失或过期的 Python 文件：

```text
GeoModelBridge/
├── bin/
│   ├── geomodelbridge.exe
│   └── native-filegdb/
│       ├── GeoModelBridge.NativeWriter.exe
│       └── FileGDBAPI.dll
├── python/
│   ├── geomodelbridge/          # import geomodelbridge
│   └── examples/convert_fbx.py
└── docs/python-client.md
```

Python 语言兼容目标为 3.9 及以上；实际验证解释器见[本版验证记录](validation-v0.1.9.md)。不需要 `pip install`，将完整发布目录的 `python` 加入调用脚本的 `sys.path` 即可。无需修改 ArcGIS 的 Python 环境或安装第三方包。Ubuntu 使用同一套库，程序名不带 `.exe`，运行库布局见[部署指南](build-and-release.md)。

## 最小调用

```python
from pathlib import Path
import sys

release = Path(r"D:\Tools\GeoModelBridge")  # 替换为完整发布目录
sys.path.insert(0, str(release / "python"))

from geomodelbridge import ConversionRequest, Engine, GeoModelBridgeError

engine = Engine(release / "bin" / "geomodelbridge.exe")
request = ConversionRequest(
    input_fbx=r"D:\Models\model.fbx",
    output_gdb=r"D:\Results\new-model.gdb",  # 必须不存在；父目录需已创建
    wkid=3857,
    origin=(100, 100, 100),                  # 示例值，实际项目需填写真实定位参数
    feature_class="Models",
    profile="gis-static",                  # 显式启用静态兼容及无效法线修复
    missing_textures="material-color",
    texture_dirs=(),
)

try:
    result = engine.convert(request, on_message=lambda event: print(event.text))
except GeoModelBridgeError as error:
    print(error.code, error.exit_code, error.report_path)
    for diagnostic in error.diagnostics:
        print(diagnostic.code, diagnostic.context, diagnostic.message)
    raise

print(result.feature_class_path)  # D:\Results\new-model.gdb\Models
print(result.feature_count)
print(result.report_path)         # 默认为 new-model.gdb.report.json
```

默认 `profile="strict"` 与 CLI 一致；要处理无效法线模型，必须显式选择 `gis-static`。缺图策略独立，默认 `material-color`，可设为 `error` 要求贴图完整。WKID 与 origin 不执行重投影；目标须为写入端支持的米制投影坐标系。

可直接运行随发布目录提供的示例：

```powershell
python D:\Tools\GeoModelBridge\python\examples\convert_fbx.py --engine D:\Tools\GeoModelBridge\bin\geomodelbridge.exe --input D:\Models\model.fbx --output D:\Results\new-model.gdb --wkid 3857 --origin 100 100 100 --profile gis-static
```

## 调用接口

| API | 行为 |
| --- | --- |
| `Engine(executable, writer=None)` | 显式定位 CLI；默认写入端为相邻 `native-filegdb` 下的程序 |
| `engine.check()` | 核对 Python / CLI / writer 完整版本号，实际执行 writer `--probe` 检查运行库，返回 `ProbeResult` |
| `engine.validate(request)` | 不执行程序、不创建目录；返回路径绝对化后的新请求，原请求不变 |
| `engine.command(request)` | 返回已验证的参数元组，可用于日志或预览，不能拼接为 shell 命令 |
| `engine.convert(request, on_message=None)` | 同步调用 CLI，完成严格报告核对后返回 `ConversionResult` |

`ConversionRequest` 必填 `input_fbx`、`output_gdb`、整数 `wkid`、三个有限数值 `origin`。可选 `feature_class`、`profile`、`missing_textures`、`texture_dirs` 和 `report_path`。不会把字符串 WKID 或布尔值默认为有效整数；工具箱脚本应显式转换类型。要素类名称使用字母开头、最长 64 位的 ASCII 字母 / 数字 / 下划线。输出后缀为小写 `.gdb`。

`ConversionResult` 包含规范化 `request`、`output_gdb`、`feature_class_path`、`report_path`、`feature_count`、完整 `diagnostics`、`stdout_tail`、`stderr_tail`、`version` 和 `backend`。要素类路径供 GIS 使用；它不是 GDB 目录中的普通文件，不能用 `Path.is_file()` 判断要素类存在。

库显式传入 writer，不读取 `GMB_NATIVE_WRITER`，避免外部环境改变实际使用的程序。需要其他 writer 位置时使用 `Engine(..., writer=...)`，仍要求版本匹配。库不修改当前工作目录、环境变量、`arcpy.env` 或地图状态；导入本身不会运行 EXE。

## 消息与错误

`on_message` 接收 `Message(level, code, text, count)`。阶段代码为 `CHECKING_ENGINE`、`CONVERTING`、`VERIFIED`；成功后的诊断按 severity / code 汇总，例如 `MISSING_TEXTURE_FALLBACK`、`NORMALS_REPAIRED`。完整逐项内容保留在结果和报告中。

回调在调用线程执行，可以由未来工具箱脚本映射到 GIS 消息函数。这里提供阶段消息，转换期间不连续推送日志，也不报告完成百分比。失败通过异常的 `diagnostics` 返回。回调应保持简单且不抛异常；回调异常原样传播，完成回调发生时 GDB 可能已经提交，不应据此重试覆盖同一路径。

所有业务失败继承 `GeoModelBridgeError`，包含 `code`、可空 `exit_code`、可空 `report_path`、`diagnostics`、两路日志尾部。参数错误为 `ValidationError`；进程失败或报告不能核验为 `ConversionError`。

| `error.code` | 含义 |
| --- | --- |
| `INVALID_PATH` / `INVALID_REQUEST` | 参数、输入或输出父目录不符合要求 |
| `PATH_EXISTS` | 输出 GDB 或报告已经存在，未开始转换 |
| `ENGINE_UNAVAILABLE` / `LAUNCH_FAILED` | 找不到或无法启动程序 |
| `VERSION_MISMATCH` / `BACKEND_UNAVAILABLE` | 组件版本或写入端运行库检查未通过 |
| `PROCESS_FAILED` | CLI 非零退出，原始退出码和可用诊断随异常返回 |
| `INVALID_REPORT` | 即使退出码为 0，报告或输出仍不能确认成功 |

成功要求：新的 GDB 目录存在；报告的源 FBX、输出、版本、后端、要素类、profile、缺图策略和投影 WKID 均匹配；包含正数要素数量及关闭重开后的几何 / 材质 / UV / 纹理验证；没有错误诊断。`missing_textures="error"` 不能接受缺图回退。报告不是安全签名，这些检查用于发现失败、组件混用和结果错配。

两路子进程输出重定向到本次调用拥有的临时文件，避免管道阻塞；每路返回最后 256 KiB，临时日志随正常调用结束清理。磁盘日志仍随输出量增长，完整模型诊断以 JSON 报告为准。库最多读取 64 MiB 报告；超限会明确报错，保留生成的成果与报告供核查。库不删除转换失败后留下的 GDB 或报告。

## 后续 ATBX 的接入约定

后续工具箱负责参数、消息和结果输出，入库逻辑复用本库。`.atbx` 脚本可用 [GetParameterAsText](https://pro.arcgis.com/en/pro-app/3.6/arcpy/functions/getparameterastext.htm) 获取文本参数，在脚本层转换 WKID / XYZ 数值并构造请求。成功后可将 `str(result.feature_class_path)` 传给 [SetParameterAsText](https://pro.arcgis.com/en/pro-app/3.6/arcpy/functions/setparameterastext.htm) 作为派生输出。消息回调可映射 `info` / `warning` / `error`，详细错误从异常提取。

当前边界：

- **创建新的 GDB 与要素类**。不向已有 GDB 追加、不覆盖已有成果；即使未来 `arcpy.env.overwriteOutput=True` 也不改变此契约。若以后需要追加，应另行定义数据库锁、冲突和回滚行为。
- **同步调用**。本版不提供后台任务、超时或进程树取消接口。ArcGIS 取消按钮不能被描述为已能安全终止整条 EXE / writer 链路；工具箱取消、结果登记以及中断后的清理需在实际接入时处理。[Esri 的取消机制](https://pro.arcgis.com/en/pro-app/3.6/arcpy/geoprocessing_and_python/understanding-cancellation-behavior-in-script-tools.htm) 说明默认在当前代码行结束后停止，`autoCancelling=False` 可交由脚本检查 `isCancelled`。
- **调用库与转换 EXE 独立于 Pro**。后续 `.atbx` 宿主自身需要 ArcGIS，但不会因此让本库或原生入库内核依赖 Pro。
- **数据库核验与显示验收分开**。真实 GDB 测试不等于已完成 ATBX 参数界面、地图加载、三维外观或取消行为的验收。

## 回归入口

```powershell
python tests/python_client_test.py
python tests/python_client_integration_test.py --install-dir releases/V0.1.9 --work artifacts/new-python-client-check
```

第一项在 CTest 中自动运行；第二项使用安装后的库和真实写入端，测试中文 / 空格路径、贴图、缺图策略、法线兼容、已有成果保护，并对三份 GDB 分别复制后独立回读。测试目录必须是新路径。Windows / Ubuntu CI 都已配置此入口；托管运行结果以 GitHub Actions 实际状态为准。
