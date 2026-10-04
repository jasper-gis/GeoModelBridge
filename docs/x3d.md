# X3D XML 静态模型

本次新增 `.x3d` 输入，通过根构建中的 tinyxml2 解码 XML，再进入 WRL 共用的静态场景、三角化、法线、贴图与材质转换器。无需安装 X3D 浏览器或桌面 GIS。CLI `inspect`、`prepare`、`convert`，以及 GUI 和 Python 客户端使用同一个 reader。

```powershell
geomodelbridge inspect model.x3d --report model-check.json
geomodelbridge prepare model.x3d --output model-bundle
geomodelbridge convert model.x3d --output model.gdb --wkid 32650 --origin 500000 3000000 100
```

输出目录、GDB 和报告必须是新路径。检查/准备中间包通过并不代表 GDB 写入或独立回读通过。

## 支持范围

- XML 编码的 X3D 3.0、3.1、3.2、3.3；接受 `Interchange`、`Interactive`、`Immersive`、`Full`、`CADInterchange` 声明，但实际内容仍必须属于下述静态子集，不宣称实现这些完整 profile。
- `Group`、`Transform`、`Shape`、`Appearance`、`Material`、`ImageTexture`、`TextureTransform`、`IndexedFaceSet`、`Coordinate`、`Normal`、`TextureCoordinate`、`Color`、`WorldInfo`。
- 层级变换、中心点/缩放方向、负缩放与镜像绕序；完成定义后的 `DEF/USE` 实例复用。所有几何变换只应用一次，节点矩阵只保留来源信息。
- 三角形和经过平面、自交检查的 3–4096 角多边形；独立的坐标、法线、UV 和颜色索引。保留 UV 接缝、法线硬边与材质边界，不按位置合并角点。
- 右手 Y 向上、默认米单位转换为右手 Z 向上米。X3D 3.3 可声明一个 `head/unit category="length"`，正的 `conversionFactor` 同时作用于坐标与平移，并在诊断中记录。WKID 只是坐标系赋值，原点平移仍由共同流程执行，不进行重投影。
- 材质漫反射 RGB、标量透明度；未贴图时逐面颜色或每面各角相同的顶点色。插值顶点色、`ColorRGBA` 不在支持范围内，明确拒绝。
- 本地 PNG/JPEG、URL 备选路径、额外贴图目录、独立 UV 索引和 `TextureTransform`。默认重复采样；非重复/钳制采样明确拒绝。没有 `TextureCoordinate` 时按标准生成包围盒 UV，记录 `X3D_DEFAULT_UV_GENERATED`。
- 没有显式法线且 `creaseAngle=0` 时生成平面法线并报告。非零 `creaseAngle` 要求导出显式法线。严格模式拒绝无效法线；`gis-static` 可重建零/非有限角法线、删除有限零面积三角形，分别记录修复/删除计数。有限但非单位长度的法线在两个 profile 中都拒绝，不替换其方向。仅因浮点乘积舍入而叉积为零的非零面积面不会被当成退化面删除；不能可靠处理时明确拒绝。
- `head/meta` 和 `WorldInfo` 的非渲染文本省略并报告。支持 XML 预定义实体与有效数字字符引用、注释、UTF-8 名称。

## 贴图与颜色的保守边界

X3D 3.3 官方 [Lighting §17.2.2.3](https://www.web3d.org/documents/specifications/19775-1/V3.3/Part01/components/lighting.html#Lightingon) 的正文与表 17.3 对彩色贴图/标量颜色和 Alpha 的乘算或替换描述存在差异。因此，有 `Material` 时，本 reader 只接受两种解释结果一致的组合：

- RGB/RGBA 贴图要求 `diffuseColor="1 1 1"`，并要求存在的 `Color` 值均为白色。
- 带 Alpha 的贴图要求 `transparency="0"`。贴图原始 Alpha 保留。
- 不带 Alpha 的 RGB 贴图可保留材质标量透明度。
- 灰度强度贴图可与材质/逐面颜色相乘；灰度 Alpha 贴图同样要求标量不透明度为 1。

不明确的组合在两个 profile 中都返回 `AMBIGUOUS_X3D_TEXTURE_MATERIAL`。建议在源软件中烘焙这些组合，或明确导出中性白色材质。无 `Material` 的非受光场景按标准未受光映射解释，记录 `UNLIT_SHADING_MAPPED`，提示目标光照可能不同。

缺少图片时，默认保留原材质 RGB 与标量透明度、移除缺失图片绑定，并报告 `MISSING_TEXTURE_FALLBACK`。`--missing-textures error` 恢复拒绝。存在但损坏/不可读的图片、无效父路径和目录型图片路径仍失败；不会尝试后续 URL 来掩盖读取错误。图片资源不重编码。

非零环境光/高光只在显式 `gis-static` 下省略并报告；自发光仍要求源端烘焙。

## 明确拒绝的输入

- X3D 4、Classic VRML `.x3dv`、二进制 `.x3db`、压缩文件及 VRML1；VRML97 `.wrl` 继续使用原 reader。
- `Script`、`ROUTE`、PROTO、动画、变形、GeoSpatial 节点、外部 Inline、MultiTexture、PBR/着色器、程序化贴图，以及未列出的几何节点/字段。未知内容在未使用节点中也不忽略。
- `head/component`、非 length 单位、重复或不适用版本的单位声明。
- DTD、外部实体、处理指令、未知实体、非 UTF-8、非法 XML 字符/字符引用、多个根/Scene、混入正文或非空 CDATA、重复 DEF、前向/循环 USE、错误 `containerField`、重复单节点字段。
- 非有限坐标/变换、奇异变换、错误索引、非平面或自交多边形、未知材质通道。单位/变换缩放导致非零源三角形塌缩时明确拒绝，不能将其作为零面积面删除。所有已解码法线都需验证；未引用的零/非有限法线无法从对应三角形重建，两个 profile 均拒绝。

默认源文件上限仍为 512 MiB，沿用现有几何/形状上限；XML 图最多 100,000 个节点，每个数值字段最多 30,000,000 个分量。XML 解析器还应用自身深度限制。不通过截断几何来处理超限模型。

## 规范与验证

实现依据 Web3D 的 [XML 编码规则](https://www.web3d.org/documents/specifications/19776-1/V3.3/Part01/concepts.html)、[Geometry3D](https://www.web3d.org/documents/specifications/19775-1/V3.3/Part01/components/geometry3D.html)、[Texturing](https://www.web3d.org/documents/specifications/19775-1/V3.3/Part01/components/texturing.html) 和 [Lighting](https://www.web3d.org/documents/specifications/19775-1/V3.3/Part01/components/lighting.html)。

`tests/fixtures/textured_quad.x3d` 是自制中性材质贴图样例：一个网格、两个三角形、六个角点、一个材质和一张图；未定位包围盒为 `[10,-2,1]` 至 `[12,1,1]` 米。`tests/x3d_integration_test.py` 覆盖单位、定位、嵌套镜像、实例、UV 接缝、透明度、修复策略、XML 边界及不覆盖输出。真实 GDB 数值与复制回读结果见本次总体验证记录；图形显示验收另行记录。
