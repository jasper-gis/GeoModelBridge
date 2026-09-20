# Scene Bundle v1

目录由 `scene.json` 和 `textures/<sha256>.<ext>` 组成；图片通常保持原始字节。显式 `gis-static` 策略允许对安全识别的缺少 JFIF 标识的 Adobe YCbCr JPEG 插入 18 字节 APP0；原压缩图像数据及其余字节保持不变，处理前后 SHA-256 写入 `JPEG_CONTAINER_NORMALIZED` 诊断。目录名、`sha256` 和 `byte_length` 对应实际进入中间包的字节。
根字段：`schema_version:1`, `generator:"GeoModelBridge"`, `version:"0.1.5"`, `name`, `source`, `coordinates`, `nodes`, `meshes`, `materials`, `textures`。`version` 记录生成器版本；历史包保持其原始值，格式版本仍为 1。V0.1.4 改为流式写出网格数组，格式与数值编码保持不变。
V0.1.3 增加可选 `conversion_profile:"strict"|"gis-static"`，缺省解释为 `strict`。新生成器总是写入此字段，后端将其带入最终报告，CLI/GUI 核对与请求一致。兼容策略及各诊断的具体含义见 [compatibility.md](compatibility.md)。新 Scene JSON 使用紧凑排版，值与几何精度不变。
可选 `diagnostics[]`：`{ "severity":"warning"|"error", "code":"...", "message":"...", "context":"..." }`；后端必须将其带入最终报告，有 error 时拒绝写入。C++ 生成器总是写入此字段。

`coordinates`: `{ "unit":"meter", "up_axis":"Z", "space":"local"|"referenced", "wkid":0, "origin":[0,0,0], "origin_explicit":false }`。
所有 position 已烘焙节点世界变换、统一为右手 Z-up 米，再加 origin。WKID 只是显式空间参考赋值，不重投影。

`textures[]`: `{ "name":"...", "mime_type":"image/png", "source":"...", "embedded":true, "path":"textures/<sha256>.png", "sha256":"...", "byte_length":123 }`。
`materials[]`: `{ "name":"...", "color":[1,1,1,1], "texture":-1, "double_sided":true }`。texture 为索引，-1 为无纹理；颜色为 RGB 与不透明度 alpha，0..1。
`meshes[]`: `{ "name":"...", "source_node":"...", "vertices":[...], "triangles":[...] }`。
vertex: `{ "position":[x,y,z], "normal":[x,y,z]|null, "uv":[u,v]|null }`。
`uv` 保留完成 FBX UV 变换烘焙后的源坐标，V=0 对应图片下方。V0.1.1 后端对所有有 UV 的 patch 映射为目标 `S=U, T=1-V`，未绑定纹理的材质也采用相同 UV 语义；不回写 bundle，不翻转 PNG 行序或 JPEG 图片。数值验收应比较映射后的目标 UV。
原生后端要求输入 normal 为单位向量（长度容差约 1e-5），不会静默归一化。写 GDB 后的法线分量量化及精确核验规则见 [acceptance.md](acceptance.md)。
triangle: `{ "indices":[0,1,2], "material":0 }`。索引为当前 mesh 的顶点索引；每个角点独立保留，不跨 UV/法线接缝合并。
`nodes[]`: `{ "name":"...", "source_id":"...", "parent":-1, "source_world_transform":[16 column-major doubles], "meshes":[0] }`。节点矩阵仅追溯，不能再次乘到已烘焙顶点。

Writer 必须验证 schema、索引、有限数字、图片散列、相对路径；拒绝未支持的图像格式，不能静默转码。
首版所有 triangle 按材质分组为 Multipatch triangle patches；同一 mesh 输出一个要素。
只有关闭数据库、重新打开验证通过后才允许报告写入成功。显示兼容性仍须目标软件人工验收。
