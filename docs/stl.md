# STL 输入

接受 `.stl`（扩展名大小写均可）的 ASCII 与 little-endian binary 三角面模型，接入共享 Scene Bundle 与原生 FileGDB 后端。ASCII 支持多个完整 `solid`；每个 solid 保留独立网格。二进制头即使以 `solid` 开始，也按精确长度 `84 + 50 × facet_count` 判断。

```sh
geomodelbridge inspect model.stl --report new-inspect.json
geomodelbridge prepare model.stl --output new-bundle --mesh-up-axis Y --mesh-unit-meters 0.001
geomodelbridge convert model.stl --output new-model.gdb --wkid 32650 --origin 500000 3000000 100
```

STL 没有通用的单位、向上轴或空间参考元数据。缺省假定右手 Z-up、米制，可用 `--mesh-up-axis Y|Z` 和正数 `--mesh-unit-meters` 指定来源约定；Y-up `(x,y,z)` 转为 Z-up `(x,-z,y)`。单位与轴向只应用一次，`STL_COORDINATE_ASSUMPTION` 记录采用值；随后设置的 origin 只应用一次，WKID 只赋值，不重投影。GUI 和 Python 客户端使用同一组来源设置。

标准 STL 不携带材质、贴图、UV 或透明度，采用不透明白色双面材质并报告 `STL_MATERIAL_ASSUMPTION`。有色 STL 的约定互不兼容：所有非零二进制 facet attribute 字段，以及头中的 `COLOR=`、`MATERIAL=`、`TEXTURE=` 扩展，均明确拒绝，两个 profile 一致。无法解释的 ASCII 指令也拒绝，不会静默丢弃颜色或纹理。

每个三角形保留三个独立角点，保留来源绕序和各 facet 的法线方向。有限非零法线归一化，不按位置合并角点，不自动改向或平滑。严格策略拒绝零值、非有限法线及零面积三角形。显式 `--profile gis-static` 可从有效三角形边重建零值/非有限法线，报告 `NORMALS_REPAIRED` 的角点/三角形数量；可移除有限且真正零面积的面，报告 `DEGENERATE_TRIANGLES_REMOVED`，随面移除的无效法线另记 `DEGENERATE_NORMALS_DISCARDED`。非有限位置、变换溢出、非零坐标缩放下溢为零、不可表示的非零面积始终失败；不会借 GIS 静态策略移除这些几何。

ASCII 必须完整闭合 `facet / outer loop / endloop / endfacet` 和 `solid / endsolid`；每面恰好三个顶点，若 endsolid 提供名称，必须与 solid 相同。只接受 ASCII 字符，不支持 UTF-8 BOM、二进制尾部附加数据、颜色扩展、未知命令或压缩 STL。不执行封闭流形、邻接一致性或制造可打印性认证。

输入文件上限 512 MiB，每网格最多 1000 万角点（包含被移除面的解码计数），每文件最多 10 万个 solid，名称最多 4096 字节、数字/关键字 token 最多 256 字节。模型字节和最终 Scene 在内存中保留；二进制的计数和长度在展开前核验。已有 GDB、Bundle 或报告由共享独占创建机制保护。

格式依据：[Assimp 作者维护的 STL 读取实现](https://github.com/assimp/assimp/blob/master/code/AssetLib/STL/STLLoader.cpp)（二进制布局、little-endian、颜色扩展）、[Ennex 保存的 3D Systems STL 接口说明](https://www.fabbers.com/tech/STL_Format)（三角面与法线约定）。实现为项目独立编写，测试使用自制模型。来源软件样本与目标 GIS 的外观验收仍须另行执行。
