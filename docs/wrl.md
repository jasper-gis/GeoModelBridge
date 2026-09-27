# WRL / VRML97 入库

V0.5.0 支持 `#VRML V2.0 utf8` 的静态多边形模型，CLI、GUI 和标准库 Python 客户端共用 C++ 读取器与原生 FileGDB 后端。VRML1、X3D、压缩 WRL 不在当前读取范围。

```powershell
geomodelbridge inspect model.wrl --profile gis-static --report inspection-new.json
geomodelbridge convert model.wrl --profile gis-static --output model-new.gdb --wkid 32650 --origin 500000 3000000 100
```

## 支持范围

| 内容 | 处理方式 |
| --- | --- |
| Group、Transform、Shape | 保留层级来源；世界变换只应用一次 |
| DEF / USE | 共享定义实例化；拒绝未定义、循环与超限展开 |
| Transform 的 translation、rotation、scale、center、scaleOrientation | 组合变换后转为 Z-up；镜像修正绕序，法线逆转置 |
| IndexedFaceSet | 三角形及简单、共面凸/凹多边形；保留独立角点 |
| Coordinate、Normal、TextureCoordinate | 保留独立索引、每面或每顶点法线、UV 接缝 |
| Color | 每面颜色，以及一面内相同的顶点颜色；插值顶点色需先烘焙 |
| Material | diffuseColor、transparency；ambient/specular 需 `gis-static` 明确省略；emissive 拒绝 |
| ImageTexture | PNG/JPEG，本地相对 URL 和有序候选；默认 repeat；clamp 拒绝 |
| TextureTransform | UV 减中心、缩放、旋转、加回中心与平移 |
| solid、ccw | 单双面与正面绕序 |
| 未提供 UV | 根据局部包围盒生成 VRML 默认 UV，并报告 |
| 未提供法线 | creaseAngle=0 生成平面法线；非零 creaseAngle 要求导出显式法线 |
| WorldInfo | 明确记录未写入的非渲染元数据 |

每个多边形最多 4096 角点，每个展开后网格最多 10,000,000 角点，层级最多 128 层、解析及实例展开分别最多 100,000 节点。超限会失败，不截断几何。非共面、自交及相互接触的多边形拒绝，建议在建模软件中三角化。有限零面积三角形仅在 GIS 静态策略下删除并报告；该策略可修复无效角点法线并报告角点数、三角形数。

未知节点/字段、ROUTE、Script、PROTO、动画、传感器、灯光、相机、Inline、LOD、Switch、程序化几何、视频与像素纹理均明确失败。应先导出当前静态姿态的 IndexedFaceSet。当前功能是静态模型入库，不执行 VRML 浏览器交互。

## 坐标与材质规则

源坐标按 VRML97 右手 Y-up、米制解释，转换为右手 Z-up 米制后应用请求的 origin；报告记录坐标约定、WKID、原点及节点来源矩阵。WKID 不触发重投影。

VRML97 的 RGB 图片覆盖 diffuseColor/Color，图片 alpha 覆盖材质 transparency。读取器按图片组件类型设置白色/单位透明度因子，并分别记录 `WRL_TEXTURE_COLOR_REPLACED`、`WRL_TEXTURE_ALPHA_REPLACED`；灰度图片保留颜色调制。图片原始字节不变。这与 glTF 的基色乘法不同，遵循 [VRML97 颜色及透明度规则](https://www.web3d.org/documents/specifications/14772/V2.0/part1/concepts.html#4.14)。

缺少图片时保留原始材质颜色和标量透明度，报告 `MISSING_TEXTURE_FALLBACK`；`--missing-textures error` 恢复拒绝。存在但不可读、父路径无效、非普通文件或损坏的图片不能回退为缺图。图片路径不能越出模型目录或显式贴图目录，网络 URL 不会下载。多个 URL 只在前项确实不存在时尝试后项，选中后报告序号。

未指定 Material 的白色无光照外观映射到 FileGDB 漫反射材质，并记录 `UNLIT_SHADING_MAPPED`。目标 GIS 的光照效果仍需单独视觉验收。

节点定义参考 [Web3D VRML97 节点规范](https://www.web3d.org/documents/specifications/14772/V2.0/part1/nodesRef.html)。验收记录见 [V0.5.0](validation-v0.5.0.md)。
