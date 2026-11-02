# MAX 入库适配器（V0.6.0）

## 状态与依赖

已接入 CLI、Windows GUI 和无第三方依赖的 Python 客户端。`.max` 由用户指定的 **Windows 3ds Max Batch 2022.2 或更新版本**打开；需要有效授权及场景所需插件。GeoModelBridge 不附带、安装或代为授权 Autodesk 软件。

**已在中文 3ds Max 2024 实测加载和导出真实 MAX，并完成 50 组 Scanline 材质对照；该大型样本仍未完整入库。** “南Ⅰ-1联合站”样本的 32 个带贴图网格没有源 UV，部分 JPEG 也不符合原生保真写入条件，工具明确拒绝，不生成不完整的 GDB。详见 [本次验证记录](validation-max-nanyi-2026-11-02.md)。协议替身测试、实际渲染对照和完整 MAX 入库验收是不同证据，不能相互替代。

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
- Standard Blinn / Phong（使用数值 shaderType，兼容中文名称）及嵌套 Multi/Sub Standard。每层均用原始面 ID 按最大槽位 ID 循环解析，再展开为明确的叶材质绑定。未知、未指定、禁用材质槽仍拒绝；只有保存的 Scanline 渲染器在 `gis-static` 下允许将 ID 空洞显式表示为其实际渲染的黑色，记录涉及的原面 ID 和数量。
- 漫反射 RGB、标量不透明度、100% 强度 Bitmap。普通 PNG/JPEG 保留原始字节；以下有损或重采样策略只在 `gis-static` 中接受并记录，不修改源图。
- 位图使用显式 UV 通道 1，支持正有限 U/V 缩放及真实尺寸映射，补偿普通缩放的中心偏移；仍拒绝源偏移/旋转/镜像/噪声、输出曲线及未知映射。缺少源 UV 的带贴图网格列出源节点名并拒绝，不补造 UV，也不自动删除有效贴图。
- 对非交错 8/16 位 RGB、RGBA、灰度 Alpha PNG，按 Standard 的 diffuse alpha 与底色合成规则生成数值 RGBA8 表面图；保留原图时不改字节。只有同一 PNG、相同映射/裁剪/输出、100% 强度且标量 opacity=100 的图像 Alpha opacity 绑定可成为表面 Alpha。漫反射 Alpha 本身不表示表面透明；CRC、解压流、长度均校验。tRNS、APNG、Exif 方向等未支持容器仍拒绝。
- PNG 的内部矩形裁剪以原生裁剪像素密度、像素中心双线性采样输出，再保留 UV 重复；记录裁剪参数和原/新图哈希。Max 内置 Bricks 仅在没有子贴图和独立 opacity 时，使用 `renderMap` 生成 2048×2048 RGBA8 颜色图，明确记录有限分辨率、量化与哈希，不烘焙场景光照。
- 全白的顶点颜色/Alpha/照明通道为恒等值，可验证后从私有快照移除并记录；非恒等顶点颜色、自发光、彩色透射、透明衰减、线框、面贴图、隐藏/不可渲染网格、XRef、未知渲染材质及其他独立 opacity、bump、normal、displacement、emission、refraction 贴图仍拒绝。Physical、V-Ray、Corona、Arnold 等材质没有通用自动烘焙。

`gis-static` 对允许的静态修改器和动画时间采样为当前帧网格；省略环境光、高光、反射及非网格节点，并逐项记录。单面材质按明确记录的双面 Multipatch 策略输出；strict 要求原材质已为双面。严格模式拒绝动画和非网格节点。两种策略均采用数值 RGB；除上述明确报告的表面图处理外保留原图字节，渲染器曝光、色调映射、色彩管理及像素过滤不作烘焙，记录 `MAX_COLOR_POLICY`。

Max 的 100% 漫反射图替代漫反射 RGB；中间 FBX 使用白色乘数并保留标量 opacity，记录 `MAX_DIFFUSE_REPLACEMENT`。缺图默认恢复原材质色和 opacity，记录 `MISSING_TEXTURE_FALLBACK`；`--missing-textures error` 禁止回退。路径访问错误、非普通文件及无效父路径均失败。

贴图按声明路径、MAX 同目录同名文件、显式 `--texture-dir` 的顺序查找。旧电脑的绝对路径已不存在时，可以直接使用随 MAX 一起交付的图片；已选路径的访问错误、无效父目录或非普通文件不会被后续同名文件掩盖。

## 流程与报告

1. 核验源文件、运行环境路径、匹配版本的 worker，并在系统临时目录独占创建本次工作目录。
2. 以 JSON 保存源文件 SHA-256、采样帧、策略和额外图像目录。所有路径是数据，不拼接进 MAXScript。
3. 启动独立 Max Batch 进程。缺少插件 / XRef 会拒绝。唯一例外：保存的 Scanline 渲染器、`gis-static`，且缺项精确为 `mrmaterialattribs.gup - mental ray: material custom attribute`，可忽略这个不参与 Scanline 表面的旧属性扩展并记录；不放宽未知着色器、几何或修改器。普通图像缺失留给明确的贴图策略处理。
4. 预检材质和几何，生成命名静态快照并仅导出快照选择集；关闭动画、Skin、Shape、PointCache 和贴图转码。原 MAX 文件不保存。
5. 共享 FBX 读取器读取原始图片并完整校验。适配器核对源/FBX 哈希、逐网格三角形数和米制世界包围盒、面材质分配数量、颜色/opacity/单双面及图片绑定。核验使用已报告的零面积面清理之前的三角形、面材质数量及角点包围盒；最终几何报告仍只统计保留的面。无面引用的有限顶点单独记录且仍检查非有限值；无面引用的已验证材质和图片记录在 `unreferenced_materials` / `unreferenced_textures`，不再误判为导出丢失。所有实际面引用的清单仍须完整且不重复；未知或不支持的未使用材质仍拒绝。任何不一致停止，不创建 GDB。
6. 通过既有 Scene Bundle / FileGDB SDK 写入、关闭后重开读回校验。最终成功报告仍必须为 `written_and_readback_verified`。

报告 `reader_diagnostics` 中的 `MAX_ADAPTER_PROVENANCE` 包含 JSON 清单：协议/引擎版本、请求标识、原 MAX 路径/哈希、采样帧和帧率、Max 版本、已加载插件路径和文件版本（包括 FBX 导出插件）、FBX 文件版本和参数、单位、源节点矩阵和导出统计、图像源/哈希、调整诊断及进程日志。GUI / Python 额外校验原 MAX 路径、Batch 程序和帧号。矩阵仅为追溯信息，不重复应用；WKID 与 origin 的规则不变。

每条日志流保留最多 256 KiB 原始尾部，并公开截断标志；无效 UTF-8 日志替换显示字符。超过超时，Windows Job Object 会结束整个进程树。外部终止 CLI 也会关闭该 Job 并结束 MAX 子进程；强制结束可能遗留本次临时目录，不会清理其他任务目录。GUI 仍沿用等待转换完成的交互，没有单独取消按钮。

已完成 FBX 导出后，即使共享读取器拒绝，仍保存 `MAX_ADAPTER_PROVENANCE`，用于定位源节点和材质。清单中的 `exported` 只表示导出阶段完成，不能作为入库成功证据。只有原生后端的 `written_and_readback_verified` 才表示完成入库回读。

预检发现缺失或不完整的 UV 时，逐节点 `MAX_MISSING_UV` 之外还提供 `MAX_REPAIR_MANIFEST`。其 JSON 消息列出原文件/哈希、帧号、是否完成全场景评估，以及每个节点的总面数、带贴图面数、所需 UV 通道、原材质名、原图路径和哈希。中途遇到其他错误时，`scene_evaluated=false` 表示清单尚不完整。它只提供修复依据，不补造 UV 或解除图片绑定。

可选的系统/监听日志文件无法读取时，报告记录 `MAX_LOG_READ_ERROR` 警告及对应 `runtime_logs` 的 `read_error`、`message`。超时仍报告 `MAX_TIMEOUT`，worker 的原始拒绝原因仍保留；成功结果继续以完整导出清单及原生 GDB 回读为依据。

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

独立的渲染语义对照可设置 `GMB_MAX_POLICY_WORK` 为一个不存在的短路径目录，用 Max Batch 执行 `tests/max_runtime_policy_probe.py`。它使用真实 Scanline 渲染检查 Alpha 合成、Multi/Sub 循环/空洞/嵌套、真实尺寸 UV 原点及裁剪，输出 `runtime-policy.json`。另设 `GMB_MAX_POLICY_ENGINE` 为已安装 CLI 路径，可对 Max 生成的 FBX 核对 UV/RGBA，并写入和独立复制回读真实 GDB；它不代表用户完整 MAX 入库已通过。

## 官方 API 依据

- [3ds Max Batch 参数与 Python 脚本](https://help.autodesk.com/cloudhelp/2018/ENU/3DSMax-Batch/files/GUID-48A78515-C24B-4E46-AC5F-884FBCF40D59.htm)
- [loadMaxFile / exportFile](https://help.autodesk.com/cloudhelp/2024/ENU/MAXScript-Help/files/MAXScript-Tools-and-Interaction/File-Access/3ds-Max-Scene-Files-Access/GUID-624D3D05-B15D-4A97-9F15-DA35CDB0DDD2.html)
- [FBX 参数](https://help.autodesk.com/cloudhelp/2015/ENU/MAXScript-Help/files/GUID-54B1F140-B304-48A2-9829-C68B345E2044.htm)
- [Standard 材质与按 Shader 区分的通道表](https://help.autodesk.com/cloudhelp/2016/ENU/MAXScript-Help/files/GUID-57F5EBBA-5F54-4CD4-8993-0B07A3571293.htm)
- [静态快照](https://help.autodesk.com/cloudhelp/2016/ENU/MAXScript-Help/files/GUID-0532C071-4401-4846-8450-3DA5510A3883.htm)
- [UV 参数](https://help.autodesk.com/cloudhelp/2017/ENU/MAXScript-Help/files/GUID-BD6C5537-7314-4A1B-940C-32E471045621.htm) / [贴图输出颜色曲线](https://help.autodesk.com/cloudhelp/2022/ENU/MAXScript-Help/files/3ds-Max-Objects-and-Interfaces/TextureMap-Material/TextureMap-Shared-Classes/GUID-F37AE1A1-0747-4DC9-83CC-B8F036375DC3.html)
- [Multi/Sub 材质 ID 循环](https://help.autodesk.com/cloudhelp/2021/ENU/Max-Developer-Help/developer/3ds_max_sdk_features/rendering/materials_textures_and_maps/accessing_material_properties.html) / [Bitmap Alpha 属性](https://help.autodesk.com/cloudhelp/2019/ENU/3DSMax-MAXScript/files/GUID-A3231AD9-2FCB-483B-9F43-3914BD1CE3D6.htm)
- [mental ray Connection 的作用范围](https://help.autodesk.com/cloudhelp/2016/ENU/3DSMax/files/GUID-17ED203D-D363-4EF8-B9F0-ADFE38F53645.htm) / [renderMap](https://help.autodesk.com/cloudhelp/2022/ENU/MAXScript-Help/files/3ds-Max-Objects-and-Interfaces/TextureMap-Material/GUID-43EC5EED-F275-4325-8D99-819C51C11380.html)
