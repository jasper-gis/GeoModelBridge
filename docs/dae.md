# DAE / COLLADA 输入

当前开发版本接受 `.dae`（大小写均可）的 COLLADA **1.4.1** 静态场景，直接进入 Windows/Linux 共用 C++ 核心、Scene Bundle 和原生 FileGDB 后端。

```sh
geomodelbridge inspect model.dae --report new-inspect.json
geomodelbridge prepare model.dae --output new-bundle
geomodelbridge convert model.dae --output new-model.gdb --wkid 32650 --origin 500000 3000000 100
```

GUI 文件选择/拖放及标准库 Python 客户端的 `ConversionRequest.input_fbx` 同样接受 DAE；参数保留旧名称以兼容调用方。`asset` 决定单位及 X/Y/Z 向上轴，缺省为右手 Y-up、米制。变换按文档顺序组合，`matrix` 按 COLLADA 的行序文本解码，父子变换、实例、单位及轴向转换各应用一次；镜像实例同时校正绕序和法线。`DAE_COORDINATE_CONVENTION` 记录源坐标约定。WKID 是空间参考赋值，不执行重投影。

支持范围：

- `triangles`、`polylist` 和无孔 `polygons`；多边形须为有限、平面、无自交的简单面，每面最多 4096 角点，每个展开网格最多 1000 万角点。独立位置、法线和 UV 索引及 accessor 的 stride/offset 保留角点边界。
- 选定 visual scene、父子 node 和本文件内 `instance_node` / `instance_geometry`，以及 translate、rotate、scale、仿射 matrix；拒绝循环引用、奇异或非有限变换。
- `profile_COMMON` 的 Lambert、Phong、Blinn 漫反射颜色或 PNG/JPEG 纹理，以及 `constant` 的 emission 作为无光照基色；后者报告 `UNLIT_SHADING_MAPPED`，目标光照可能有差异。
- `instance_material` 符号绑定，sampler → surface → image 链和显式 `bind_vertex_input` 的 TEXCOORD 集。保留漫反射使用的 UV 集；无纹理或缺图绑定不可用时保留现有编号最小的 UV 集，其他 UV 集仍校验并报告。保留原图片字节，重复采样；显式过滤设置、surface 格式覆盖、UV 变换、投影、夹取或其他无法保留的采样语义均拒绝。
- `transparent` 的颜色与 `transparency` 标量，按 1.4.1 的 `A_ONE` / 灰度 `RGB_ZERO` 公式计算不透明度；透明度纹理只接受 `A_ONE` 且使用完全相同的漫反射 sampler 和 UV 绑定，保留图片 Alpha 与标量透明度的乘积。含 Alpha / tRNS 的 PNG 必须显式绑定到该透明度通道，避免把 diffuse Alpha 猜测为透明度；即使该 PNG 可能全部不透明，也保守执行这一检查。非灰度 `RGB_ZERO` 需要独立 RGB 混合因子，不能归约为标量透明度；它与漫反射 color 的非 1 alpha、独立透明度贴图及其他透明模式均明确拒绝。
- effect / profile_COMMON 上 GOOGLEEARTH、MAX3D、MAYA 扩展的 `double_sided`；其他扩展明确拒绝。

严格策略拒绝活动环境光、高光、反射与发光等无法保留的材质通道。显式 `--profile gis-static` 可以省略环境光、高光与反射，并逐项报告 `MATERIAL_CHANNEL_OMITTED`；发光材质仍须烘焙（constant 无光照基色除外）。GIS 静态策略还可移除有限零面积三角形和重建零值/非有限法线，各自记录计数。未提供法线时生成平面法线并报告 `DAE_DEFAULT_NORMALS_GENERATED`。

缺图默认仅移除不可用的图片绑定，保留已有漫反射颜色和标量不透明度，报告 `MISSING_TEXTURE_FALLBACK`；DAE 的 diffuse 只能二选一 color 或 texture，texture 通道没有额外颜色时沿用白色因子。`--missing-textures error` 恢复拒绝。访问错误、非普通文件、无效父目录或损坏图片不会按缺图回退。图片只允许模型目录/显式贴图目录内的相对 URI；拒绝网络、外部 DAE、目录逃逸、DTD 和外部实体。

当前不支持 COLLADA 1.5、动画、skin/morph/controller、关节、灯光/相机实例、顶点颜色、曲线、线段、带孔或非平面多边形、嵌套坐标约定、shader 参数覆盖或自定义渲染扩展；这些内容须先在来源软件中导出受支持的静态网格。

解析文件上限 512 MiB，单纹理上限 256 MiB，XML 与实例图均有深度/数量限制。`p` 索引和 `vcount` 面角点数逐面读取，不构造全量索引浮点数组或逐面副本；这些列表必须使用整数文本，小数、指数、非有限值、负索引、超限或不完整列表均拒绝，未绑定的 offset 槽也会校验。源 XML 文档及最终 Scene 几何仍保留在内存中。输出仍由共享独占创建机制保护，已有 GDB、Bundle 或报告不会被覆盖。

文本按 XML 注释与 CDATA 的内容规则读取，完整检查分片后的数字、索引和变换，不忽略后续文本。固定版本 TinyXML-2 会丢弃相邻标记之间的纯空白；如果因此无法判定数字是否被分隔，或无法还原图片路径等字符串的内部空白，返回 `UNSUPPORTED_DAE_XML`。遇到此诊断时，将值写在一个普通文本/CDATA 区段内，或为数字列表在文本区段中保留明确的空白分隔。

数字字符引用须符合 XML 1.0 字符范围；NUL、代理项、越界或非法数字文本在 XML 解码前拒绝，避免解码后的字符串截断。注释、CDATA 和处理指令中的引用样式文本保持字面含义。

语义依据：[Khronos COLLADA 1.4.1 规范](https://www.khronos.org/files/collada_spec_1_4.pdf)、[W3C XML 1.0 的注释与 CDATA 规则](https://www.w3.org/TR/REC-xml/#sec-comments)。测试使用项目自制 fixture；外部导出器及目标 GIS 视觉验收须另外执行。
