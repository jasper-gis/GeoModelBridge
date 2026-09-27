# GLB 输入与渲染策略（V0.4.0）

`geomodelbridge inspect/prepare/convert model.glb` 读取 glTF 2.0 GLB。GLB 使用右手 Y-up、米制；程序将默认场景中各节点的世界矩阵烘焙一次，再旋转到右手 Z-up 米制。报告保存原节点矩阵、WKID 与显式原点。WKID 赋值不执行重投影。

支持静态 `TRIANGLES`、索引或非索引顶点、稀疏属性访问器（不含稀疏索引）、节点层级与镜像实例、法线、选定的 base-color UV 集、`KHR_texture_transform`、`KHR_mesh_quantization`、PNG/JPEG 基色图片及材质颜色、标量透明度、单双面。图片可来自 GLB 内部 bufferView，或模型目录及 `--texture-dir` 下的安全相对路径。URI 的常见百分号编码可解析，路径不能离开所选目录。GLB 的 V 原点在图片顶部；Scene Bundle 与 FileGDB 编码阶段做一次相应坐标转换。

严格模式只接受 `KHR_materials_unlit`；普通 glTF PBR 材质需要显式 `--profile gis-static`。GIS 静态模式保留基色与贴图，逐项报告省略金属度、粗糙度与光照的 `MATERIAL_CHANNEL_OMITTED`。活动的节点变换动画在严格模式下拒绝；GIS 静态模式使用文件保存的节点姿态并记录 `STATIC_POSE_USED`，不选择某个动画时刻。两种模式均拒绝法线、遮蔽、发光、金属粗糙度等无法保留的图片通道，以及顶点色、alpha MASK、非默认采样器、未知扩展、形变、蒙皮、材质变体、压缩网格与非三角形图元。缺少图片的默认策略仅移除该图片绑定，保留基色与标量透明度并报告 `MISSING_TEXTURE_FALLBACK`；`--missing-textures error` 改为拒绝。损坏、不可读或格式不符的图片始终失败。

例如：

```powershell
geomodelbridge inspect model.glb --profile gis-static --report new-inspection.json
geomodelbridge convert model.glb --profile gis-static --output new-model.gdb --wkid 32650 --origin 500000 3000000 100
```

GLB 的布局与坐标约定依据 [Khronos glTF 2.0 规范](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html)。解析器版本与许可见 [第三方声明](../THIRD_PARTY_NOTICES.md)。真实 GDB 与双平台验证状态见 [V0.4.0 验证记录](validation-v0.4.0.md)。
