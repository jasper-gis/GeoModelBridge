# V0.1.3 转换策略

FileGDB Multipatch 可承载几何、常规漫反射颜色、透明度、UV 和图片，但本工具不会自动烘焙完整的三维渲染材质。V0.1.3 将输入检查与显式兼容处理分开：GUI 默认显示“GIS 静态兼容”，命令行保持默认严格模式。

| 内容 | `strict` | `gis-static` |
|---|---|---|
| 中性的标准默认材质字段 | 明确为零且未绑定活动纹理时接受 | 相同 |
| 没有曲线或动画属性的空动画容器 | 提示并接受 | 相同 |
| 真实动画曲线 | 拒绝 | 使用文件保存的节点静态姿态，记录 `STATIC_POSE_USED` |
| 活动环境光、高光、反射及其专用纹理 | 拒绝 | 省略这些光照/反射效果，逐材质记录 `MATERIAL_CHANNEL_OMITTED` |
| 严格零面积且坐标有限的三角形 | 拒绝 | 删除并按网格汇总 `DEGENERATE_TRIANGLES_REMOVED` |
| 缺少 JFIF 标识、且能明确安全补齐的 Adobe YCbCr JPEG | 拒绝并提示需要封装修复 | 仅插入 18 字节 JFIF APP0，逐贴图记录 `JPEG_CONTAINER_NORMALIZED` 与处理前后 SHA-256；不重新压缩图像 |
| 缺失的漫反射贴图、无效 UV、非有限几何、未知材质语义、活动位移、PBR、自发光、骨骼/形变 | 拒绝 | 仍拒绝 |

保存的静态姿态不等同于第 0 帧，不等同于源软件当前播放帧，也不包含骨骼、蒙皮和形变求值。需要指定时间的动画快照或烘焙光照时，应先在源建模软件中完成。

JPEG 封装修复只适用于经检查的 8 位、三分量 baseline、明确 Adobe YCbCr 的缺少 JFIF 情况。未知颜色空间、旋转方向或无法确认安全的元数据不会自动猜测；已有非首位 JFIF 不做重排。修复在中间包副本中完成，原 JPEG 的其余全部字节保留，源 FBX 和外置图片不修改。图像解码有效性仍由后端检查。JFIF 标识的位置和颜色空间规则依据 [JFIF 1.02 规范](https://www.w3.org/Graphics/JPEG/jfif.pdf)。

兼容处理不会把贴图失败的材质替换成白模，不会放宽数据库回读核验，也不会更改源 FBX。非有限或过小但非零的三角形不会被当作零面积而删除；如果删掉零面积面后整个网格为空，仍然失败。

报告的 `conversion_profile` 记录实际策略；兼容处理标记 `compatibility_adjustments` 必须和具体诊断一起阅读。输入报告中的 `validation_passed` 表示处理后的模型通过检查，`strict_validation_passed` 只在严格模式无错时为真。GDB 的成功状态仍必须为 `written_and_readback_verified`，而实际三维外观仍需目标软件验收。

失败报告保留后，可以在 GUI 点“换个新名称”生成未被占用的输出名，再按新策略重试。该操作不会覆盖数据库或报告，也不会自动开始转换。

命令行示例：

```powershell
.\dist\bin\geomodelbridge.exe inspect model.fbx --profile gis-static --report new-inspection.json
.\dist\bin\geomodelbridge.exe convert model.fbx --profile gis-static --backend native-filegdb --output new-model.gdb --wkid 3857 --origin 0 0 0
```

示例空间参考和原点必须替换为模型实际参数；它们不会完成重投影或地理配准。
