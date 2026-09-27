# MAX 入库适配器（V0.6.0）

## 状态与依赖

已接入 CLI、Windows GUI 和无第三方依赖的 Python 客户端。`.max` 由用户指定的 **Windows 3ds Max Batch 2022.2 或更新版本**打开；需要有效授权及场景所需插件。GeoModelBridge 不附带、安装或代为授权 Autodesk 软件。

**当前没有完成真实 `.max → FileGDB` 实机验收。** 开发环境未发现 Max Batch，也未收到真实 MAX 样本。自动测试中的进程替身只证明参数、隔离、超时、清单校验和后续原生 GDB 链路，不能证明 Max API 在某个实际版本上的兼容性。正式使用前必须执行下述实机验收。

Linux 明确拒绝 `.max` 预处理；可在 Windows 上准备 Scene Bundle，或从 Max 导出静态 FBX 后交给同一 Linux 引擎。其他格式及原生 FileGDB 后端不依赖 Max。

## 调用

```powershell
geomodelbridge convert "D:\models\building.max" `
  --max-batch "C:\Program Files\Autodesk\3ds Max 2025\3dsmaxbatch.exe" `
  --max-frame 0 --max-timeout 600 --profile gis-static `
  --texture-dir "D:\models\textures" `
  --output "D:\results\building-new.gdb" `
  --wkid 32650 --origin 500000 3000000 100
```

替换示例坐标。采样帧必须显式填写，范围为 -1000000..1000000；超时为 1..86400 秒，默认 600。`inspect` 和 `prepare` 支持相同 MAX 参数。严格策略仍为 CLI 默认；普通 Standard 材质通常需要 `gis-static` 以显式省略光照。

GUI 中选择 MAX 后，在“转换选项 → MAX 预处理”填入程序路径、采样帧和超时。

```python
from geomodelbridge import Engine, ConversionRequest

result = Engine(r"D:\GeoModelBridge\bin\geomodelbridge.exe").convert(
    ConversionRequest(
        input_fbx=r"D:\models\building.max",  # 保留已有 API 字段名
        output_gdb=r"D:\results\building-new.gdb",
        wkid=32650, origin=(500000, 3000000, 100),
        profile="gis-static",
        max_batch=r"C:\Program Files\Autodesk\3ds Max 2025\3dsmaxbatch.exe",
        max_frame=0, max_timeout=600,
    )
)
```

## 当前接受边界

- 静态 Editable Mesh / Editable Poly 和常见内置几何体：Box、Sphere、GeoSphere、Plane、Cylinder、Cone、Torus、Teapot、Pyramid、Tube。
- 可采样的静态修改器白名单：UVWMap、Unwrap UVW、Smooth、Normal、XForm、Bend、Taper、Twist、Edit Mesh、Edit Poly。实际类名由运行时检查；不认识的类明确失败。Skin、Morpher、Cloth、粒子和第三方几何修改器不接受。
- Standard Blinn / Phong 及一层 Multi/Sub Standard。所有槽位和面材质 ID 必须明确有效，不能包含未指定、禁用或嵌套 Multi/Sub 材质。
- 漫反射 RGB、标量不透明度、100% 强度的 Bitmap 漫反射贴图。仅保留 PNG/JPEG 原始字节，不转码。
- 位图使用显式 UV 通道 1、UV 方向、单位缩放、无偏移/旋转/镜像/噪声/真实世界缩放、无裁剪与输出颜色曲线。其他映射需要用户先烘焙。
- 目前拒绝带 alpha 通道或 tRNS 的 PNG，即使全部 alpha 实际为不透明；不把漫反射 alpha 擅自解释为材质透明度。透明效果可使用支持的标量 opacity。独立 opacity、bump、normal、displacement、emission、refraction 贴图均拒绝。
- 拒绝顶点颜色、顶点 alpha、自发光、彩色透射、透明衰减、线框、面贴图、隐藏/不可渲染网格、XRef 依赖和未知渲染材质。Physical、V-Ray、Corona、Arnold 等材质应先在建模端显式烘焙/转换；本版本没有自动烘焙。

`gis-static` 对允许的静态修改器和动画时间采样为当前帧网格；省略环境光、高光、反射及非网格节点，并逐项记录。单面材质按明确记录的双面 Multipatch 策略输出；strict 要求原材质已为双面。严格模式拒绝动画和非网格节点。两种策略均保留数值 RGB 和原图字节，渲染器曝光、色调映射、色彩管理及像素过滤不作烘焙，记录 `MAX_COLOR_POLICY`。

Max 的 100% 漫反射图替代漫反射 RGB；中间 FBX 使用白色乘数并保留标量 opacity，记录 `MAX_DIFFUSE_REPLACEMENT`。缺图默认恢复原材质色和 opacity，记录 `MISSING_TEXTURE_FALLBACK`；`--missing-textures error` 禁止回退。路径访问错误、非普通文件及无效父路径均失败。

## 流程与报告

1. 核验源文件、运行环境路径、匹配版本的 worker，并在系统临时目录独占创建本次工作目录。
2. 以 JSON 保存源文件 SHA-256、采样帧、策略和额外图像目录。所有路径是数据，不拼接进 MAXScript。
3. 启动独立 Max Batch 进程。缺少 DLL / XRef 会终止加载；普通图像缺失留给明确的贴图策略处理。
4. 预检材质和几何，生成命名静态快照并仅导出快照选择集；关闭动画、Skin、Shape、PointCache 和贴图转码。原 MAX 文件不保存。
5. 共享 FBX 读取器读取原始图片并完整校验。适配器核对源/FBX 哈希、逐网格三角形数和米制世界包围盒、面材质分配数量、颜色/opacity/单双面及图片绑定。任何不一致停止，不创建 GDB。
6. 通过既有 Scene Bundle / FileGDB SDK 写入、关闭后重开读回校验。最终成功报告仍必须为 `written_and_readback_verified`。

报告 `reader_diagnostics` 中的 `MAX_ADAPTER_PROVENANCE` 包含 JSON 清单：协议/引擎版本、请求标识、原 MAX 路径/哈希、采样帧和帧率、Max 版本、已加载插件路径和文件版本（包括 FBX 导出插件）、FBX 文件版本和参数、单位、源节点矩阵和导出统计、图像源/哈希、调整诊断及进程日志。GUI / Python 额外校验原 MAX 路径、Batch 程序和帧号。矩阵仅为追溯信息，不重复应用；WKID 与 origin 的规则不变。

每条日志流保留最多 256 KiB 原始尾部，并公开截断标志；无效 UTF-8 日志替换显示字符。超过超时，Windows Job Object 会结束整个进程树。外部终止 CLI 也会关闭该 Job 并结束 MAX 子进程；强制结束可能遗留本次临时目录，不会清理其他任务目录。GUI 仍沿用等待转换完成的交互，没有单独取消按钮。

Max 进程使用当前用户权限，不是安全沙箱。对于会运行脚本的 MAX 场景，应使用独立低权限 Windows 账户/隔离工作机，并配置对应 Max 版本的场景脚本安全策略。适配器不操作用户已打开的 Max 会话。

## 实机验收

```powershell
python tests/max_real_integration_test.py `
  --install-dir releases/V0.6.0 `
  --max-batch "C:\Program Files\Autodesk\3ds Max 2025\3dsmaxbatch.exe" `
  --input "D:\models\supported-textured.max" --frame 0 `
  --wkid 32650 --origin 500000 3000000 100 `
  --work "D:\results\max-acceptance-new"
```

脚本使用安装后的无第三方依赖 Python 客户端写真实 GDB，核验 MAX 来源和原文件未修改，将 GDB 复制到另一个新路径，通过原生 SDK 独立重开验证。需要至少一个纹理面；脚本不会将“运行环境缺失”记作通过。材质/UV、镜像非均匀变换、动画帧、缺图/坏图、缺插件、XRef、复杂材质、超时、长路径等样本还应分别验收。显示效果需要目标 GIS 软件另行视觉验收。

## 官方 API 依据

- [3ds Max Batch 参数与 Python 脚本](https://help.autodesk.com/cloudhelp/2018/ENU/3DSMax-Batch/files/GUID-48A78515-C24B-4E46-AC5F-884FBCF40D59.htm)
- [loadMaxFile / exportFile](https://help.autodesk.com/cloudhelp/2024/ENU/MAXScript-Help/files/MAXScript-Tools-and-Interaction/File-Access/3ds-Max-Scene-Files-Access/GUID-624D3D05-B15D-4A97-9F15-DA35CDB0DDD2.html)
- [FBX 参数](https://help.autodesk.com/cloudhelp/2015/ENU/MAXScript-Help/files/GUID-54B1F140-B304-48A2-9829-C68B345E2044.htm)
- [Standard 材质与按 Shader 区分的通道表](https://help.autodesk.com/cloudhelp/2016/ENU/MAXScript-Help/files/GUID-57F5EBBA-5F54-4CD4-8993-0B07A3571293.htm)
- [静态快照](https://help.autodesk.com/cloudhelp/2016/ENU/MAXScript-Help/files/GUID-0532C071-4401-4846-8450-3DA5510A3883.htm)
- [UV 参数](https://help.autodesk.com/cloudhelp/2017/ENU/MAXScript-Help/files/GUID-BD6C5537-7314-4A1B-940C-32E471045621.htm) / [贴图输出颜色曲线](https://help.autodesk.com/cloudhelp/2022/ENU/MAXScript-Help/files/3ds-Max-Objects-and-Interfaces/TextureMap-Material/TextureMap-Shared-Classes/GUID-F37AE1A1-0747-4DC9-83CC-B8F036375DC3.html)
