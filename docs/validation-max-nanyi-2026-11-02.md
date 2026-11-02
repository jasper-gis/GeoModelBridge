# MAX 真实样本验证（2026-11-02）

## 结论

中文 3ds Max 2024（26.0.0.940）可以加载“南Ⅰ-1联合站”目录中的 `nanyiyilianhezhan2012.max`，并导出 2,774 个网格。**这个用户样本尚未完整入库，不存在通过验收的整站 GDB。** 文件名中的“2012”不是实际保存版本；同目录的 2026 文件在已安装的 2024 运行时加载失败。

源数据阻断项如下：

- 32 个带贴图网格、5,424 个贴图三角形缺少源 UV 通道。已回查原 Editable Poly 与 `snapshotAsMesh` / `snapshot`，并非 FBX 导出丢失。最后一次转换以 `MAX_MISSING_UV` 列出全部源节点，在 GDB 创建前拒绝，没有补造 UV 或删除有效贴图。
- 单独检查目录全部 51 张 JPEG：21 张因方向/色度元数据或非基线编码不满足保真写入条件而拒绝，22 张符合已有的保守 JFIF 补标记策略，其余保持原始字节。没有对不明确的 JPEG 自动转码或移除元数据。
- Max Scene Security Tools 报告 ALC/CRP 并在独立进程的内存场景内自动清理。没有保存回原 MAX。转换前后 SHA-256 均为 `adc5a28328f4c9009da1424835f3ed886754ac9e679556061d45c424ffde5622`。

完整节点名、各网格三角形数、JPEG 文件名与原因保留在本地验证产物 `问题清单.md`、`compatibility-summary.json`、`source-final-report.json` 中；不将用户模型、图像或生成 GDB 上传仓库。测试用 WKID 3857 / origin (0,0,0) 不代表模型的实际地理定位。

## 适配器修改

- 数值 shaderType 替代本地化显示名称比较；FBX 枚举 getter 的 MAXScript Name 转为文本后核验，仍拒绝未接受的参数。
- 仅对保存为 Scanline、显式 gis-static、缺失项精确匹配的旧 mental ray 材质属性扩展设置忽略策略；其他缺插件、未知材质和修改器仍拒绝。
- 按实测规则解析 Multi/Sub 循环 ID 和嵌套图；每层都使用原面 ID。Scanline 的 ID 空洞以显式黑色材质记录，不移除三角形。不存在或禁用的材质对象不属于此例外。
- 标准库 PNG 解码检查 CRC、全部解压数据和长度。支持所列 8/16 位容器的数值 Alpha/底色合成、严格匹配的 PNG opacity 绑定及裁剪；记录源/输出哈希、量化和重采样策略。独立图片库对目录内 41 张 PNG 的 RGBA8 解码逐像素对照一致。
- 内置、无子贴图的 Bricks 采用明确的 2048×2048 数值颜色图策略；全白顶点颜色/Alpha/照明通道验证后仅从私有快照移除，非恒等通道仍拒绝。
- Max 普通 tiling 以 UV 中心为原点，真实尺寸映射以零为原点。实测还发现 Max FBX 输出的纹理 scaling 会在共享读取器中反向解释，因此改为按材质在网格 UV 角点上计算映射，输出单位纹理变换。以原 map vertex 和变换共同索引，保留 UV 接缝，不改位置或重复应用节点矩阵。
- MAX 清单验证识别共享读取器已经核验的 JFIF 补标记前哈希；只接受这个内部生成的来源关系，不信任 worker 任意宣称的改图。不改变 Scene Bundle JSON。
- 共享 FBX 读取器拒绝时保留导出来源清单。`exported` 不代表 GDB 成功，最终状态和回读要求保持不变。

## 验证证据的范围

`tests/max_runtime_policy_probe.py` 在真实 Scanline 下完成 50 项对照：8 项 Alpha/预乘/opacity 组合、28 项连续与稀疏 ID、9 项嵌套 ID、3 项真实尺寸 UV 和 2 项裁剪。UV 与裁剪均比较处理前后的实际渲染像素。

设置 `GMB_MAX_POLICY_ENGINE` 后，同一测试还通过真实 Max FBX 导出 3 个贴图平面。共享读取器核对三种 UV 范围（0..0.25、-0.5..1.5、0..0.01）、RGBA 像素、双面标志和白色乘数；原生后端写出真实 GDB，状态为 `written_and_readback_verified`，复制到另一目录后独立回读得到 `standalone_copy_verified`。**这是 Max 生成 FBX 的局部集成证据，不是该用户 MAX 的完整验收。** GIS 软件视觉验收未执行。

已将本次提交内容独立检出，排除工作区原有、未纳入本提交的原生性能修改；Windows 与 Ubuntu 24.04 x86_64（Docker 用户空间）根 CMake/CTest 均为 **22/22**。两平台原生集成（87 项无效输入/清理/哈希反例）、部署、安装后标准库 Python（17 项转换、22 项检查、12 份 GDB 独立复制回读）通过，Windows GUI 服务 **379/379**。Ubuntu 再以非 root 用户执行贴图路径测试，**29 项**通过，包含访问拒绝。依赖哈希和版本检查通过。

独立提交快照还重新执行了上述 50 项真实 Scanline 对照及生成 FBX 的 GDB 写入/复制回读，全部通过。两份原 MAX 与目录内 92 张原图共 94 个文件的哈希均与初始审计一致。完整用户 MAX 的状态仍为拒绝，不能用这些局部与回归结果替代整站验收。

运行时对照命令（目录必须不存在）：

```powershell
$env:GMB_MAX_POLICY_WORK = 'C:\gmb-check\max-new'
$env:GMB_MAX_POLICY_ENGINE = 'C:\GeoModelBridge\bin\geomodelbridge.exe'
& 'C:\Program Files\Autodesk\3ds Max 2024\3dsmaxbatch.exe' `
  tests/max_runtime_policy_probe.py -dm on -listenerLog 'C:\gmb-check\listener-new.log'
```

已有 `tests/max_real_integration_test.py` 仍作为完整 `.max → 安装后 Python 客户端 → 原生 GDB → 独立复制回读` 的验收入口。用户样本需先解决源 UV 和不支持的图像表达，再重新运行；不能把本次拒绝或局部小样本通过改写为整站已验证。
