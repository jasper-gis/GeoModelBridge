# PLY、STL、X3D 扩展验证 — 2026-10-04

本轮从 `master` 的 `cac02ae` 开始，版本保持 `0.6.0`，新增功能列入 Unreleased。按来源覆盖和现有能力复用优先选择 PLY、STL、X3D：分别补齐带颜色扫描网格交换、CAD / 制造三角面交换、XML 静态贴图场景。完整格式表见 [model-formats.md](model-formats.md)。不引入新第三方依赖，不改变 Scene Bundle 或原生 FileGDB 编码。

## 实现与保留边界

- **PLY 1.0**：ASCII、binary_little_endian、binary_big_endian，类型/数量严格校验，简单凹凸多边形三角化，显式顶点或面法线，8 位 RGB 与可选 Alpha。每个原始面内顶点颜色须一致，面/顶点颜色同时存在时须一致；明确拒绝渐变、未知元素/属性、贴图扩展和仅有点的点云。缺失法线生成平面法线并记录。
- **STL**：ASCII、多 solid、标准 little-endian binary，精确核对 `84 + 50 × facet_count`。保留每面独立角点及有限非零法线方向；默认白色、不透明、双面材质并记录约定。非零 facet attribute 与已知头部颜色/材质扩展均拒绝，避免丢失或误解颜色。
- **X3D XML 3.0–3.3**：通过已有 tinyxml2 解码为 WRL 共用静态节点图，支持限定节点、实例、层级变换、独立角点绑定、贴图和标量透明度；3.3 接受长度单位。未知/动态节点、DTD/外部实体、非法字符及不明确的材质/纹理组合拒绝。RGB 图片需中性白色颜色因子，带 Alpha 图片需标量不透明度 1；具体例外与灰度规则见 [X3D 说明](x3d.md)。
- CLI、标准库 Python、WPF 检查与转换、浏览/拖放、安装演示及打包清单均识别三种格式。PLY/STL 默认右手 Z-up、米制，新增 `--mesh-up-axis` / `--mesh-unit-meters`，Python 对应 `mesh_up_axis` / `mesh_unit_meters`；源假设写入报告。GUI 复用来源轴/单位控件并新增中文诊断说明。

PLY/STL 对非零坐标单位缩放下溢为零明确拒绝。共享 WRL/X3D 修复四项实际复现问题：有限非单位法线不再被 GIS 静态策略改成面法线（两个 profile 均拒绝）；检查未引用的无效法线；使用幂次缩放和 FMA 残差区别严格零面积与浮点叉积舍入为零的非零三角形；变换后零面积时核对原始面，拒绝单位/变换导致的非零几何消失。只有符合既定策略的零/非有限法线和有限精确零面积面允许报告后修复/移除。

没有修改源模型、原纹理或历史成果。节点变换、单位及源轴向只执行一次，随后应用显式 origin；WKID 赋值不执行重投影。所有输出仍使用共享独占创建和禁止替换操作。

## 双平台验证

Windows x64 使用 MSVC 14.44.35207、Python 3.11.15、.NET 8；Ubuntu 24.04.4 LTS x86_64 使用本机 QEMU TCG 虚拟机（3 vCPU、3 GiB）、GCC 13.3.0、Python 3.12.3。完整根工程构建与安装分别使用 `scripts/build.ps1 -WithGui` 和 `scripts/build.py`，官方 FileGDB API 1.5.5 后端无需桌面 GIS。

| 检查 | Windows | Ubuntu 24.04 |
| --- | --- | --- |
| 根 CMake / 完整 CTest | 21/21 | 21/21 |
| PLY / STL / X3D 专项解析与策略用例 | 78 / 80 / 92，通过 | 78 / 80 / 92，通过 |
| 原生 FileGDB 集成异常/保护检查 | 83 项通过 | 84 项通过 |
| 最小部署、九种网格格式写入及独立复制回读 | 通过 | 通过 |
| 安装后 Python 转换 | 17 场景、12 份 GDB 独立复制回读 | 17 场景、12 份 GDB 独立复制回读 |
| 安装后 Python 模型检查 | 22 场景 | 22 场景 |
| Python 契约 | 55 项，4 项平台/权限跳过，其余通过 | 55 项，3 项 Windows MAX 测试跳过，其余通过 |
| WPF 编译、GUI 服务及真实进程调用 | 379/379 | 不适用 |
| 依赖散列、安装许可、版本与打包检查 | 通过 | 通过 |

Windows Python 跳过 1 项 POSIX FIFO 和 3 项当前权限不允许的符号链接用例；目录联接替换实际检查通过。Ubuntu 对应 POSIX 与符号链接用例实际通过。改动的 C++、Python、C#、XAML、项目/CMake 文件及新 fixture 在两平台测试时 SHA-256 一致。

真实原生写入覆盖三种新格式、二进制 PLY 的两种端序、二进制 STL；PLY 样例核对 RGB8=`[200,80,20]` 与 Alpha 按既有目标格式量化后的 50% 透明度。X3D 变透明度图片核对源 PNG SHA-256、解码后 RGBA SHA-256 与回读字节，标量不透明度为 1；随后复制 GDB、仅凭报告独立重开核验。原有 DAE 图片 Alpha 与标量透明度叠加回归保留。Windows PLY 可选 native 用例另执行一次，总计 79 项通过。

Python/GUI 集成覆盖新格式及 PLY/STL 来源轴/单位参数；Python 检查进一步核对 Y-up 厘米变换后的精确包围盒。完整 CTest 继续覆盖并发与链接输出保护、放置来源、材质/UV/法线角点边界、流式 Scene JSON 和既有格式。检查测试在缺少 writer 的布局运行，检查成功与 GDB 写入成功严格区分。

## 环境记录与验收限制

本地旧 Windows 构建缓存的 Release 优化 flags 为空，本轮显式恢复 `/O2 /Ob2 /DNDEBUG` 后重建；Ubuntu 使用 `-O3 -DNDEBUG`。没有据此声称性能提升或重标历史测量。

Ubuntu 的首次打包检查在源文件清单阶段因同步快照没有 `.git` 元数据失败；真实 GDB 样例生成、原生测试与部署均已通过。为该测试快照初始化本地 `master` 元数据后，使用相同代码和安装目录重跑打包检查通过；未修改主仓库历史。保留首轮失败与最终成功日志。Ubuntu 本轮新生成 14 个原有基线 GDB 样例；Windows 打包检查使用既有同版本基线样例并由当前 writer 重新独立打开，不将其标记为新生成。

仍未执行第三方生产导出器样本、目标 GIS 三维外观、GUI 屏幕布局或真实 Max Batch 实机验收。GUI 为 WPF 构建、服务与真实 CLI/GDB 调用验证；使用新建短路径输出目录，未声称修复既有 Windows 深路径限制。PLY/STL 不提供贴图，X3D 仅为文档定义的静态子集；不等同于任意模型或完整 X3D profile 无损支持。

完整构建、CTest、原生/部署/Python/GUI、打包日志与源码散列见 [证据目录](evidence/mesh-formats-2026-10-04/)。历史记录未改写。本轮 Windows 可运行目录为 `releases/MeshFormats-20261004`，包含 CLI、GUI、原生 writer、SDK 运行库、Python 客户端与演示输入；这是沿用 `0.6.0` 的开发构建，没有发布新语义版本或创建 release tag。
