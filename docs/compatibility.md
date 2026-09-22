# 转换策略

FileGDB Multipatch 可承载几何、常规漫反射颜色、透明度、UV 和图片，但本工具不会自动烘焙完整的三维渲染材质。GUI 默认显示“GIS 静态兼容”，命令行默认严格渲染模式。V0.1.7 起，缺失图片使用独立策略，两种 profile 默认均允许回退为材质颜色。

| 内容 | `strict` | `gis-static` |
|---|---|---|
| 中性的标准默认材质字段 | 明确为零且未绑定活动纹理时接受 | 相同 |
| 没有曲线或动画属性的空动画容器 | 提示并接受 | 相同 |
| 真实动画曲线 | 拒绝 | 使用文件保存的节点静态姿态，记录 `STATIC_POSE_USED` |
| 活动环境光、高光、反射及其专用纹理 | 拒绝 | 省略这些光照/反射效果，逐材质记录 `MATERIAL_CHANNEL_OMITTED` |
| 严格零面积且坐标有限的三角形 | 拒绝 | 删除并按网格汇总 `DEGENERATE_TRIANGLES_REMOVED` |
| 有效三角形上的零值或非有限法线 | 拒绝 | 用变换后三角形的面法线替换无效角点，记录 `NORMALS_REPAIRED` |
| 缺少 JFIF 标识、且能明确安全补齐的 Adobe YCbCr JPEG | 拒绝并提示需要封装修复 | 仅插入 18 字节 JFIF APP0，逐贴图记录 `JPEG_CONTAINER_NORMALIZED` 与处理前后 SHA-256；不重新压缩图像 |
| 缺失的图片文件 | 默认保留材质颜色与标量透明度，警告并继续；`--missing-textures error` 可要求完整图片 | 相同 |
| 保留贴图的 `PremultiplyAlpha` 为 true 或属性类型无效 | 拒绝，报告 `UNSUPPORTED_PREMULTIPLIED_ALPHA`；包括纹理模板继承值 | 相同 |
| 保留颜色的非中性第四分量，或漫反射/透明度因子的多维值 | 拒绝；RGB 或第四分量明确为 1 的颜色可接受，标量属性必须为一维 | 相同 |
| 有效贴图缺少 UV、非有限位置、无法形成有效面的几何、损坏或不可读的现有图片、未知材质语义、活动位移、PBR、自发光、骨骼/形变 | 拒绝 | 仍拒绝 |

保存的静态姿态不等同于第 0 帧，不等同于源软件当前播放帧，也不包含骨骼、蒙皮和形变求值。需要指定时间的动画快照或烘焙光照时，应先在源建模软件中完成。

保留的 Lambert 漫反射/透明度颜色按 RGB、因子按标量解释，依据 [Autodesk FBX Lambert 属性定义](https://help.autodesk.com/cloudhelp/2019/ENU/FBX-Developer-Help/cpp_ref/class_fbx_surface_lambert.html)。如果导出文件使用额外 alpha，且其值不是中性的 1，报告 `UNSUPPORTED_MATERIAL_ALPHA`；维度不匹配报告 `INVALID_MATERIAL_DIMENSIONS`。工具不会猜测额外 alpha 与独立透明度因子的组合公式。

V0.1.8 的法线修复发生在轴/单位、节点变换及镜像绕序处理之后。根据有限三角形边向量计算单位面法线，仅替换无效角点，保留已有有效法线、位置、UV 和材质边界，不跨面平滑或按位置合并角点。无法计算有限非零面法线时仍拒绝。零面积面已被明确删除时，其无效法线另以 `DEGENERATE_NORMALS_DISCARDED` 计数；这不会额外删除有效几何。局部光照可能变硬，修复不等于还原原作者的平滑法线。已生成 Bundle 的法线仍由 writer 严格核验。

JPEG 封装修复只适用于经检查的 8 位、三分量 baseline、明确 Adobe YCbCr 的缺少 JFIF 情况。未知颜色空间、旋转方向或无法确认安全的元数据不会自动猜测；已有非首位 JFIF 不做重排。修复在中间包副本中完成，原 JPEG 的其余全部字节保留，源 FBX 和外置图片不修改。图像解码有效性仍由后端检查。JFIF 标识的位置和颜色空间规则依据 [JFIF 1.02 规范](https://www.w3.org/Graphics/JPEG/jfif.pdf)。

缺图回退先搜索输入目录、附加 `--texture-dir` 目录和内嵌图片；只有文件找不到且没有内嵌字节时，才解除对应图片绑定。保留 FBX 漫反射颜色（含颜色因子）和标量不透明度，不生成纯白占位图片，不猜测透明度遮罩。缺失文件在透明度等通道中的别名也逐材质记录；同一材质中其他有效图片继续按原规则校验。回退后没有图片的材质无需该图片的 UV，已有角点数据仍保留。`MISSING_TEXTURE_FALLBACK` 记录图片原路径与材质，上游补齐图片后可重新转换恢复贴图。

该策略仅在读取 FBX 时执行：已生成的 Bundle 如丢失图片，writer 仍拒绝写入。现有图片损坏、读取失败或格式不支持不会当作“缺图”放行。数据库回读核验和源文件保护保持生效。非有限或过小但非零的三角形不会被当作零面积而删除；如果删掉零面积面后整个网格为空，仍然失败。

V0.1.12 修正路径检查的边界：在允许的搜索目录中，若选中的贴图路径是目录、特殊文件，父路径被普通文件占用，或检查时遇到访问拒绝 / 循环链接，报告 `TEXTURE_READ_ERROR` 并停止写入。即使附加目录存在同名图片，也不会用它掩盖先命中的无效路径；应先修正 FBX 路径、目录或权限。真正不存在的图片仍可按原策略回退，找得到的有效外置 / 内嵌图片仍照常保留。一次 FBX 读取内缓存贴图的路径查询结果，并去除重复候选路径；读取图片字节时仍检查文件类型、大小和内容，后续转换重新查询。

报告的 `conversion_profile` 与 `missing_texture_policy` 分别记录渲染策略和缺图策略；兼容处理标记 `compatibility_adjustments` 必须和具体诊断一起阅读。输入报告中的 `validation_passed` 表示处理后的模型通过检查，`strict_validation_passed` 只在严格模式无错且未发生兼容回退时为真。GDB 的成功状态仍必须为 `written_and_readback_verified`，而实际三维外观仍需目标软件验收。

失败报告保留后，可以在 GUI 点“换个新名称”生成未被占用的输出名，再按新策略重试。该操作不会覆盖数据库或报告，也不会自动开始转换。

命令行示例：

```powershell
.\dist\bin\geomodelbridge.exe inspect model.fbx --profile gis-static --report new-inspection.json
.\dist\bin\geomodelbridge.exe convert model.fbx --profile gis-static --backend native-filegdb --output new-model.gdb --wkid 3857 --origin 0 0 0
```

示例空间参考和原点必须替换为模型实际参数；它们不会完成重投影或地理配准。
