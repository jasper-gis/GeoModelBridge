# PLY 输入

支持 `.ply` 的 PLY 1.0 ASCII、binary_little_endian 与 binary_big_endian 编码。文件由头部声明元素数量、属性类型及记录顺序；这是单个多边形对象格式，不是带层级变换的场景格式。实现依据 [Greg Turk 的 PLY 格式资料与原作者工具包](https://sites.cc.gatech.edu/projects/large_models/ply.html)中 `PLY_FILES.txt`，并参考 [Stanford 3D Scanning Repository 的格式说明](https://graphics.stanford.edu/data/3Dscanrep/)。解析器为项目内独立实现，没有引入或复制外部 PLY 解析代码。

```powershell
.\geomodelbridge.exe inspect model.ply --report new-ply-inspection.json
.\geomodelbridge.exe convert model.ply --output new-model.gdb --wkid 32650 --origin 500000 3000000 100
.\geomodelbridge.exe prepare centimeters.ply --output new-bundle --mesh-up-axis Y --mesh-unit-meters 0.01
```

模型默认按右手 Z 向上、每单位一米解释；PLY 没有统一的单位或向上轴声明。`--mesh-up-axis Z|Y` 与 `--mesh-unit-meters` 可显式覆盖约定，报告记录 `PLY_COORDINATE_ASSUMPTION`。轴与单位转换仅执行一次，之后沿用显式原点平移；WKID 赋值不执行重投影。

## 支持范围

- 一个 `vertex` 元素和一个 `face` 元素；二者可按任意顺序声明及存储。属性也可重排。顶点必须包含 `x y z`，面必须包含整型列表 `vertex_indices` 或 `vertex_index`。
- 标量类型支持 `char/uchar/short/ushort/int/uint/float/double`，及对应固定宽度名称 `int8/uint8/int16/uint16/int32/uint32/float32/float64`。整数必须精确且在所声明类型范围内；ASCII 记录按行读取，禁止额外字段。支持 LF、CRLF、CR 头部行结束符，同一头部须一致。
- 三角形及简单、近似共面的凹/凸多边形。多边形先检查边相交与共面性，再使用已有 ufbx 依赖三角化；不接受孔洞、自相交、重叠边、重复角点或无法可靠三角化的面。共面检查采用归一化范围的 `1e-10` 阈值；非共面面须先在建模工具中三角化。
- 顶点或面上完整的 `nx ny nz`；方向归一化后按角点保留。二者同时声明时因优先关系不明确而拒绝。未声明法线时生成平面法线并记录 `PLY_DEFAULT_NORMALS_GENERATED`。所有已解码位置、法线均检查，包括未引用顶点。
- `uchar/uint8 red green blue`，以及可选 `alpha`。颜色值直接除以 255，映射为漫反射颜色与标量透明度；每个原始面的顶点颜色必须完全相同。不同面可有不同材质，边界处保持独立角点。面颜色与顶点颜色同时存在时必须一致。顶点渐变、绑定冲突和其他颜色类型均拒绝，不平均、不烘焙、不丢弃。
- 没有颜色时使用不透明白色漫反射材质；材质按双面显示。这些解释分别记录 `PLY_DEFAULT_MATERIAL` 或 `PLY_COLOR_MAPPED`。未引用但有效的顶点不形成渲染几何，数量记录为 `PLY_UNUSED_VERTICES`。

## 拒绝与兼容策略

未知元素或属性直接拒绝，包括边、材质扩展、纹理坐标、纹理索引、独立透明度/反射属性；即使元素未被引用也不会静默跳过。当前 PLY 入口不支持贴图，因此 `TextureFile`、纹理/材质/矩阵/变换等可识别头部提示也会拒绝，不能通过缺图颜色回退绕过。普通 `comment` 与 `obj_info` 被明确视为描述元数据，按数量记录 `PLY_METADATA_OMITTED`；不会把它们解释成位置或渲染指令。

严格策略拒绝零或非有限显式法线及零面积三角形。显式 `gis-static` 可从保留三角形的有限有效边重建无效法线，并记录 `NORMALS_REPAIRED`；仅删除确认有限且精确零面积的三角形，记录 `DEGENERATE_TRIANGLES_REMOVED`。随这些三角形移除的无效法线计入 `DEGENERATE_NORMALS_DISCARDED`。未引用的无效法线没有可供修复的三角形，在两种策略下均拒绝。过小而无法可靠表示的非零面不会被当作零面积面删除。

当前限制：源文件默认最多 512 MiB；头部最多 1 MiB，每行最多 64 KiB；顶点/面声明各不超过 10,000,000；三角化后每个模型最多 10,000,000 角点；单面最多 1,024 角点；累计多边形边配对检查最多 50,000,000 次；最多 100,000 种不同 RGBA 材质。超限会明确失败，不截断或抽稀。长度、数量、列表、二进制端序、截断和尾部字节均检查。单位换算使有限坐标溢出，或使非零坐标下溢为零，也会拒绝。

检查报告仅证明模型读取与校验通过。GDB 成功仍要求原生 FileGDB 后端完成写入和独立回读；颜色、透明度和接缝外观需要单独验收。输出及报告必须使用新路径。
