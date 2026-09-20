# V0.1.1 验证记录

本文件记录当前小迭代，不覆盖 [V0.1.0 历史记录](validation-results.md)。V0.1.0 的数据读回通过不代表它的纹理朝向通过图形验收；本次继续工作已发现需要修正的上下方向问题。

验证日期：2026-09-20。结果为 **4 组 CTest、双后端各 14 例写入与独立复制、14 例跨后端读取、Pro 19 项与原生 29 项失败处理回归通过；PNG/JPEG 方向显示通过，完整色彩、Alpha 和六面/接缝图形验收未通过。**

## 当前状态

| 项目 | 状态 | 证据与边界 |
|---|---|---|
| 版本维护 | 通过 | 10 处必需版本声明（含原生 CMake）及运行时版本文案检查一致，当前为 0.1.1 |
| C++ 与 CLI 回归 | 通过 | core、cli、reader_integration、binary_fbx 共 4 组 CTest；见 [core-tests.log](evidence/V0.1.1/core-tests.log) |
| 原生后端开发环境 | 通过 | MSVC x64 14.44.35207、Windows SDK 10.0.22621.0；C++17 filesystem 与 WIC 编译、链接及 COM 实例化运行通过 |
| Pro 后端完整样例 | 通过 | 本版重新生成 14 例 GDB，均完成写入、关闭和重开读回；见 [当前样例汇总](../examples/V0.1.1/verification-summary.json) |
| Pro 独立复制 | 通过 | 本版 GDB 在源 bundle 路径不可用时，以新进程仅读副本和预期报告核验通过 |
| Pro 非法输入与失败处理 | 通过 | 19 项拒绝、清理、覆盖保护及错误散列回归通过；见 [pro-integration.json](evidence/V0.1.1/pro-integration.json) |
| 原生非法输入与失败处理 | 通过 | 29 项拒绝、极端数值、清理、覆盖保护及错误散列回归通过；见 [native-integration.json](evidence/V0.1.1/native-integration.json) |
| 原生 FileGDB 基础写入 | 通过 | 已完成纯 C++ 实际写库和关库重开读回，转换进程不调用 ArcGIS Pro |
| 原生完整样例及独立复制 | 通过 | 14 例真实 C++ 直写和关库读回通过，成果位于 `examples/V0.1.1/native/`；原 bundle 不可用时的副本核验通过 |
| 跨后端核验 | 通过，14/14 | 独立 Pro 读取原生 GDB，与同源 Pro 成果签名精确比较；覆盖坐标、法线、UV、材质绑定、RGB/透明度/culling、纹理尺寸和全部字节、属性及 WKID；见 [summary.json](evidence/V0.1.1/native-independent-pro/summary.json) |
| PNG/JPEG 目标纹理方向 | 通过参考图 | U 向右、V 向上，TL/TR 位于 BL/BR 上方；Pro 与 native 对应的 PNG 导出文件完全一致；见 [图形对照](evidence/V0.1.1/visual/index.html) 与 [assessment.json](evidence/V0.1.1/visual/assessment.json) |
| 独立副本实际渲染 | 通过一致性对照 | Alpha 样本独立副本的目标导出与原库完全一致；不等于 Alpha 外观已验收 |
| 完整目标图形显示 | 尚未通过 | 完整色彩、Alpha 混合以及六面/接缝外观仍待验收。当前导出存在红青双影，符合红青眼镜立体显示，但未读取实际 StereoscopicMode 设置，原因尚待确认 |

Microsoft 开发组件从本机已安装 Visual Studio 的 catalog 取得官方下载地址，实际下载包逐一核对 SHA-256，编译器、链接器及 SDK 工具验证了 Microsoft 有效签名；组件仅解压在开发工作目录，没有全局安装，也不进入交付包。开发环境烟测不能替代原生 FileGDB 后端的验证。

## 纹理方向问题及修复策略

旧版数据库的 SDK 读回数值可以完全匹配源 UV，同时目标渲染仍可能上下颠倒。本次以方向参考图发现该差异；因此保留 Scene Bundle 的源 UV，在所有有 UV 的 Multipatch patch 中写入 `S=U, T=1-V`，而图片行顺序、JPEG 压缩字节和 PNG 解码像素不倒置。未绑定纹理的材质也采用相同 UV 语义。

两后端的数值验收比较映射后的目标 UV。图形验收另用四角 `TL/TR/BL/BR`、U/V 箭头及上下非对称图案确认方向。修复后的真实 Pro 导出已确认方向正确，且两个后端的对应成果渲染一致；此项不扩展为颜色空间、透明排序或所有模型已经验收。

为了避免外部旧程序绕过修复，CLI 在接受成功报告前强制核对 `backend`、`version` 和 `feature_class`；旧 V0.1.0 writer 不可作为本版成功路径。打包还验证 Pro 程序的实际二进制版本。

## 图形验收条件

按 [验收说明](acceptance.md) 核对五组模型及独立副本，按 [显示工程说明](visual-acceptance-setup.md) 创建或打开本地场景。核验之前确认真实三维相机、绝对高程和保留内嵌材质的 Mesh Symbol。当前导出观察到红青双影，符合红青眼镜显示的外观；尚未读取实际 `StereoscopicMode`，因此不将原因写为已证实的全局设置。带此现象的调查图不能作为最终颜色通过证据。

正式记录应包含目标软件完整版本、成果路径、显示模式、固定视角、源资源可访问性、逐模型结果和截图路径。数据回读报告中的 `graphical_acceptance` 保持待验收语义，图形结论以独立记录为准。

本次目标为 ArcGIS Pro 3.6.2，保留了 14 张未经翻转、调色或后处理的 MapFrame PNG。打开 [图形对照页](evidence/V0.1.1/visual/index.html) 可并排查看源 PNG/JPEG 和两个后端的实际导出；[APRX](../examples/V0.1.1/visual/GeoModelBridge-visual-V0.1.1.aprx) 中 9 处 GDB 连接已核对为交付目录内部相对路径。全局显示设置和管理员策略没有修改。

完整样例可在新目录复现：`python scripts/generate_examples.py --backend native-filegdb --output <新目录>`；改用 `--backend arcgis-pro` 可生成 Pro 成果。重建命令仍需满足相应后端的运行条件，历史 V0.1.0 目录不会被覆盖。

## 未覆盖范围

- 用户真实业务 FBX、大规模内存和吞吐性能。
- GeoScene Pro、其他 ArcGIS Pro/FileGDB API 版本以及其他操作系统。
- PBR、多通道材质、动画、自动烘焙、地理配准或重投影。
- 所有渲染器逐像素一致或所有属性逐位无损。
