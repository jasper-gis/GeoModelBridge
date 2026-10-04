# 三维模型格式选择

下表描述当前源码的输入范围。CLI、Windows GUI 与 Python 客户端共用读取器和 Scene Bundle；MAX 另有 Windows 运行环境要求。格式名称表示接受的静态子集，完整边界以链接文档为准。

| 格式 | 保留内容与适用来源 | 主要边界 |
| --- | --- | --- |
| [FBX](compatibility.md) | ASCII / 二进制静态网格、层级变换、漫反射颜色、PNG/JPEG、UV 与法线 | strict 拒绝活动动画和无法保留的材质；gis-static 可按报告使用保存姿态、简化指定光照。不求值骨骼/蒙皮/形变。 |
| [OBJ](command-line.md) | 网格及 MTL 的漫反射色、标量透明度、漫反射贴图 | 只接受模型目录内一个声明的 MTL；未知渲染指令拒绝。缺省 Z-up、米，可用 OBJ 来源参数覆盖。 |
| [GLB / glTF](glb.md) | glTF 2.0 静态三角形、节点实例、基色图片/颜色、UV、法线与受支持透明度 | strict 接受 unlit；普通 PBR 基色需 gis-static 并报告光照省略。拒绝蒙皮、形变、MASK、压缩网格和未知扩展等。 |
| [WRL](wrl.md) | VRML97 的静态 IndexedFaceSet 场景、层级变换、颜色及纹理 | 限定节点/字段集；不执行脚本、动画或外部场景。按 VRML97 的颜色、纹理与透明度规则解释。 |
| [DAE / COLLADA](dae.md) | COLLADA 1.4.1 静态网格、文件内实例、材质绑定、选定 UV 与纹理 | 限定 profile_COMMON 材质及透明度组合；拒绝 1.5、动画、controller、外部 DAE 和未知扩展。 |
| [PLY（本轮新增）](ply.md) | 扫描网格交换：PLY 1.0 ASCII、大小端二进制；逐面 RGB / Alpha、可选法线 | 顶点颜色须在每个原始面内一致；保留不同面的材质边界。拒绝颜色渐变、贴图属性及未知元素；不接受仅有点的点云。 |
| [STL（本轮新增）](stl.md) | CAD / 制造交换中的无色三角面：ASCII、little-endian binary、多 solid | 不含材质/UV/图片，明确使用白色材质；有色 STL 扩展拒绝。严格模式拒绝零/非有限法线；不作可打印性认证。 |
| [X3D（本轮新增）](x3d.md) | X3D 3.0–3.3 XML 静态场景，复用 WRL 转换器，保留支持的纹理、UV、实例与变换 | 限定 IndexedFaceSet 等节点；不接受脚本、动画、外部 Inline 或任意 profile 全集。材质和图片 Alpha 组合采用文档列出的保守边界。 |
| [MAX](max-adapter.md) | Windows 可选 Batch 适配器，指定采样帧后经受限静态网格/Standard 材质检查导出 FBX | 需要已授权的 3ds Max Batch 及场景依赖；Linux 不直接读取。已接入适配流程，真实 MAX 实机兼容性仍未验收。 |

本轮优先补齐 PLY、STL、X3D，是为了覆盖三类不同来源：PLY 增加带颜色的扫描网格交换，STL 增加 CAD / 制造三角面交换，X3D 则利用已有 WRL 场景处理能力扩展 XML 编码的贴图场景。选择依据是功能覆盖及与现有引擎的契合程度，不是市场占有率排名。

PLY / STL 默认按右手 Z-up、米制解释；实际为 Y-up 或毫米模型时，用 `--mesh-up-axis Y --mesh-unit-meters 0.001`，或 Python 的 `mesh_up_axis`、`mesh_unit_meters`。OBJ 使用独立的 `--obj-*` 参数；其他格式按各自规则读取，X3D 3.3 可包含长度单位声明。所有格式在读取阶段统一坐标约定，之后才应用用户原点；WKID 赋值不执行重投影。

可先用 `geomodelbridge inspect model.ply --report new-check.json` 或 GUI“检查模型”查看数量、米制范围与诊断。检查通过、GDB 写入并回读通过、目标 GIS 外观验收是不同结论。当前格式表不替代真实来源软件样本及视觉验收；结果与报告始终使用新路径，详见[命令行](command-line.md)、[GUI](gui.md)和[Python 客户端](python-client.md)。
