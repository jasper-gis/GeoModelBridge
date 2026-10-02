# DAE 读取优化验证 — 2026-10-02

本次从 `master` 的 `4915aad` 开始，版本保持 `0.6.0`，改动记入 Unreleased。优化 Windows/Linux 共用 DAE 读取器的索引解析和角点暂存；Scene Bundle、材质、坐标归一化与原生 FileGDB 编码契约保持原有行为。

## 改动和回归

`p` / `vcount` 通过源 XML 的只读视图逐面消费，移除全量浮点索引、全量面角点数和逐面索引副本。复用单面角点、三角化索引及位置暂存；位置/法线/UV 的采样返回定长数组，避免逐角点堆分配。源 XML 文档和最终 Scene 几何仍保留在内存中，现有文件、列表、单面与网格上限继续生效。

索引和面角点数按整数文本解析，拒绝小数、指数、非有限值、负值、溢出、缺项或多余项。旧实现接受非法的 `1.0` 索引，本次用原始可执行文件复现，失败日志保存在证据目录。依据为 [Khronos COLLADA 1.4.1 规范](https://www.khronos.org/files/collada_spec_1_4.pdf) 的整数索引列表要求。

新增回归覆盖最后一项错误、未绑定 offset 槽、符号零、2048 面、独立角点、多份 `polygons`、混合三角形/四边形的 `polylist` 及每面 4096 角点限制。失败场景均检查未创建 Bundle。完整 CTest 继续覆盖材质/UV/法线边界、镜像、缺图、错误资源、放置来源、并发输出和链接路径。

## 执行结果

Windows x64 使用 MSVC 14.44.35207；Ubuntu 24.04.4 LTS x86_64 在 QEMU TCG 虚拟机运行（3 vCPU、3 GiB 内存）。两平台使用固定来源的官方 FileGDB API 1.5.5。修正新增缩进警告后重新构建，最终构建无新增编译警告；共用实现与回归源码 SHA-256 一致。

| 检查 | Windows | Ubuntu 24.04 |
| --- | --- | --- |
| 根 CMake 构建与完整 CTest | 18/18 通过 | 18/18 通过 |
| 原生 FileGDB 集成 | 通过，83 个异常/保护检查 | 通过，84 个异常/保护检查 |
| 最小部署、纹理 GDB 和独立复制回读 | 通过 | 通过 |
| 安装后标准库 Python 客户端 | 14 场景通过，9 份复制 GDB 回读 | 14 场景通过，9 份复制 GDB 回读 |
| GUI 服务及实际原生转换 | 247/247 通过 | 不适用 |
| 固定依赖散列、安装许可与版本一致性 | 通过 | 通过 |

原生测试实际写出纹理 GDB，并在关闭重开及独立复制后的新进程中核验。覆盖坐标/原点、独立角点、法线量化、PNG Alpha 像素、JPEG 原始字节及颜色/透明度；含 210000 角点的 Scene JSON 流式写入用例。`native_build_layout` 继续检查实际 DAE 纹理 Alpha 和标量透明度。

双平台分别比较优化前后的六组有效输入：四边形、镜像实例、三角形、2048 面、多边形和 GIS 静态法线修复。全部采用 WKID 32650、原点 `(500000, 3000000, 100)`；每组 `scene.json`、Bundle 报告和纹理文件均逐字节相同。没有通过按位置合并角点来减少几何。

## 合成读取测量

使用新增的 `scripts/benchmark_dae_reader.py`，从项目 fixture 生成分别含 30 万三角形的 `triangles` 和 `polylist`，各包含 90 万独立角点、独立位置/法线/UV 索引和原 PNG。比较原始与优化后的同版本 Release CLI `inspect`，每类交替顺序运行三轮，以下为中位数。每次使用新报告路径，各组优化前后报告内容完全一致。

| 平台 / 输入 | 耗时：优化前 → 后 | 峰值驻留内存：优化前 → 后 |
| --- | --- | --- |
| Windows / triangles | 1.506 → 0.794 秒 | 149.09 → 118.46 MiB |
| Windows / polylist | 1.530 → 0.797 秒 | 150.24 → 119.32 MiB |
| Ubuntu 虚拟机 / triangles | 10.464 → 2.090 秒 | 135.75 → 93.55 MiB |
| Ubuntu 虚拟机 / polylist | 10.558 → 2.431 秒 | 137.96 → 94.70 MiB |

Windows 合成读取耗时下降约 47%–48%，峰值内存下降约 21%；Ubuntu 实验环境峰值内存下降约 31%。Ubuntu 使用 CPU 仿真，其耗时不能作为原生硬件性能指标；这些数值也不代表真实生产模型或 FileGDB 写入吞吐量。

复现测量时，分别保留两份可执行文件，并使用不存在的工作目录：

```sh
python scripts/benchmark_dae_reader.py --before /path/to/baseline/geomodelbridge \
  --after /path/to/optimized/geomodelbridge --work /path/to/new-benchmark \
  --faces 300000 --repeat 3
```

脚本使用 Windows 进程峰值工作集或 Linux 子进程 `ru_maxrss`；每轮在独立 Python 进程中测量，避免累计子进程峰值污染结果。原始样本、可执行文件散列、报告散列及中位数均保存在 `assessment.json`。

## 证据和边界

本次最终构建/CTest 日志、原生/部署/Python/GUI 结果、双平台测量、Bundle 一致性结果、旧版错误复现及共用源码校验见 [证据目录](evidence/dae-optimization-2026-10-02/)。历史验证文件保持原有日期和版本。

本次样本为项目自制 DAE，未执行外部导出器、用户生产样本或目标 GIS 图形视觉验收；这些验收边界沿用此前 DAE 记录。Windows GUI 测试采用本次新建的短路径工作目录，现有 Windows 长路径边界保持此前记录。
