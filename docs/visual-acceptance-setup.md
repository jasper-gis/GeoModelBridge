# ArcGIS Pro 图形验收工程

`scripts/create_visual_project.py` 使用本机已授权 ArcGIS Pro 的 ArcPy 创建一个新的 `.aprx`，并导出固定视角的 PNG。它只读源 GDB，不重写几何、颜色、UV、贴图或透明度。工程准备成功不等于图形验收通过；必须查看实际渲染并记录结果。

## 运行

在工程根目录执行，`--output` 必须是尚不存在的目录：

```powershell
& 'C:\Program Files\ArcGIS\Pro\bin\Python\envs\arcgispro-py3\python.exe' `
  scripts/create_visual_project.py `
  --examples examples/V0.1.1 `
  --output examples/V0.1.1/visual-rerun `
  --render-output docs/evidence/V0.1.1/visual-rerun/renders
```

参数：

- `--examples`：含 `filegdb/`、`standalone/` 的样例目录。
- `--output`：新工程及证据目录；拒绝覆盖已有目录。
- `--render-output`：独立的新 PNG 目录；省略时为 `--output` 下的 `renders/`。
- `--no-export`：只创建工程、布局和书签，不导出 PNG。
- `--only color-cube uv-plane`：仅创建指定样例的场景。
- `--extra-gdb native-uv-plane=C:\data\uv.gdb`：追加一个含 `Models` 要素类的 GDB；名称含 `plane` 时采用顶视图。
- `--projection`：可选 `Perspective`（默认）或 `Isometric`；此设置不能代替全局立体模式开关。
- `--template`：显式指定空白 `.aprx` 模板；默认使用 ArcGIS Pro 安装目录内 `Resources/ArcToolBox/Services/routingservices/data/Blank.aprx`，不会搜索或借用其他用户项目。

输出含 `.aprx`、各场景 `.mapx`、`renders/`、记录数据路径/坐标范围/相机参数的 `visual-project.json`，以及独立的空白默认 GDB。保存或移动整个交付目录后，应检查图层是否显示断链；跨目录复制 `.aprx` 不保证其相对数据路径仍可解析。

`examples/V0.1.0` 仅用于历史问题复现，其中纹理方向采用修复前的约定；当前验收必须使用重新生成的 `examples/V0.1.1`。

## 场景配置

| 项目 | 设置 |
| --- | --- |
| 坐标系统 | EPSG:32650，本地场景；样例位置 500000 / 3000000 / 100 米 |
| 高程 | `ABSOLUTE_HEIGHT`、`Shape.Z`、米、偏移 0、夸张 1 |
| 地面 | 清除地面/自定义高程面引用，无底图，无远程高程源 |
| 模型符号 | Mesh Symbol，白色 `Multiply`；保留要素内嵌材质 |
| 背景 | RGB 235 / 240 / 246；便于识别透明区域 |
| 范围 | 从 GDB 中每条实际几何的三维包围盒计算 |
| 视角 | 两个立方体采用斜俯视、对侧斜仰视、顶视；混合材质采用斜俯视和顶视；平面与独立副本采用顶视 |

每个布局的 `CIMMapView.viewingMode` 显式设为 `SceneLocal`，并断言 `camera.mode == 'LOCAL'`。本机 Pro 3.6.2 的 `createMapFrame` 初始相机为 `MAP`，只创建 Scene 仍不足以保证布局采用三维相机；省略此项会得到灰色带交叉线的二维几何投影，不能用作材质验收。

## 查看及核对

用 ArcGIS Pro 打开生成的 `.aprx`，在目录窗格中展开“地图”或“布局”，双击所需项目。地图初始相机及书签均保存固定视角。无需重新“缩放至图层”；该命令可能只依据二维范围，不能保证距离 100 米绝对高程处的米级模型足够近。

检查 `color-cube` 两个相反斜视角是否合计覆盖六面；检查 `seam-cube` 各面贴图方向及面边界；检查 `mixed-materials` 色块和贴图是否分别保留；将 `uv-plane` 与源方向纹理比较，确认 `TL/TR/BL/BR` 与 U/V 箭头；比较 `alpha-plane` 和 `standalone-copy` 的透明区域及颜色。

正式检查颜色前确认 ArcGIS Pro“项目 → 选项 → 显示 → 立体模式”为“无”。红/青眼镜模式会产生双影并改变通道显示，不能据此判断源颜色；脚本不会擅自修改用户的全局显示设置。若临时更改显示选项，应记录原值及检查后的恢复情况。

## 官方依据

- [ArcGISProject](https://pro.arcgis.com/en/pro-app/3.6/arcpy/mapping/arcgisproject-class.htm)：引用空白工程、创建地图/布局及 `saveACopy`。
- [MapFrame](https://pro.arcgis.com/en/pro-app/3.6/arcpy/mapping/mapframe-class.htm)：布局相机、书签及 PNG 导出；三维导出不适用世界文件。
- [Camera](https://pro.arcgis.com/en/pro-app/3.6/arcpy/mapping/camera-class.htm)：`LOCAL` / `GLOBAL` / `MAP` 区分及 X/Y/Z、方向、俯仰控制。
- [LayerElevation](https://pro.arcgis.com/en/pro-app/3.6/arcpy/mapping/layerelevation-class.htm)：绝对高程及 `Shape.Z`。
- [Mesh symbols](https://doc.esri.com/en/arcgis-pro/latest/help/mapping/layer-properties/mesh-symbols.html)：Multiply、Tint、Replace 语义；Replace 会覆盖原始纹理与颜色。
- [Esri CIMMap specification](https://github.com/Esri/cim-spec/blob/main/docs/v3/CIMMap.md) 与 [CIMDocument specification](https://github.com/Esri/cim-spec/blob/main/docs/v3/CIMDocument.md)：场景查看模式、默认相机及视图相机结构。
- [Set display options](https://doc.esri.com/en/arcgis-pro/latest/help/mapping/properties/display-options.html)：立体显示模式与红/青眼镜渲染。

本机实际接口以 ArcGIS Pro 3.6.2 自带的 `arcpy/_mp.py` 与 `arcpy/cim/` 定义复核。该版本不存在较新文档中的 `arcpy.mp.CreateArcGISProject`，因此使用已安装空白工程加 `saveACopy`。

## 本次实际结果

[V0.1.1 图形证据对照页](evidence/V0.1.1/visual/index.html) 并列显示原始 PNG/JPEG 与两个后端写出的 GDB 的实际渲染，并提供原图及 SHA-256。工程位于 `examples/V0.1.1/visual/GeoModelBridge-visual-V0.1.1.aprx`，包括 9 个场景、14 个布局；9 条 GDB 数据连接均核对为完整交付目录内的相对路径。

PNG 与专门制作的非对称 JPEG 均已与源图逐项比较：U 向右、V 向上、上方 TL/TR、下方 BL/BR，两个后端均一致。JPEG 仅在测试源制作时由方向 PNG 转存，产品转换使用该 JPEG 输入。PNG 双后端、JPEG 双后端以及 Alpha 原库/独立副本各自导出的 PNG 字节一致；这些一致性检查是补充证据，不替代源图对照。

导出图仍存在红/青通道双影，外观符合官方的 anaglyph 描述；没有可靠读取到用户 UI 中的实际立体模式值，因此未推断未知存储字段或修改应用设置。切换布局为 `Isometric` 未消除双影。完整色彩、透明度和六面接缝图形验收保持未通过，详见 [assessment.json](evidence/V0.1.1/visual/assessment.json)。
