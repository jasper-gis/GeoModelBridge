# V0.1.10 验证记录

2026-09-21，在 Windows x64（Python 3.11.15）和未安装 Pro 的 Ubuntu 24.04.4 x86_64 虚拟机（Python 3.12.3）验证。本版重点为日志资源控制、消息回调失败后的结果保留，以及报告定位参数的记录和核对。两平台仍使用原生 FileGDB API 1.5.5。

## 最终结果

| 检查 | Windows | Ubuntu |
| --- | --- | --- |
| CMake/CTest | 8/8 | 8/8 |
| Python 契约测试 | 26 项：25 通过，1 项跳过 | 26/26 |
| 安装后的 Python 真实调用 | 7/7，4 份 GDB 独立复制回读 | 同左 |
| 原生集成、拒绝/清理/散列检查 | 37 项通过，含 21 万角点流式输入 | 同左 |
| 可搬迁部署，含独立 Python 导入与转换 | 通过 | 通过 |
| 完整 GDB 样例与复制回读 | 14/14 | 14/14 |
| 版本、依赖散列和发布检查 | 通过 | 通过 |
| GUI 服务与真实转换 | 161/161 | 不适用 |

Windows 符号链接构造测试因当前账户权限而跳过；对应检查在 Ubuntu 通过。发布检查使用 `--check-only`，没有生成归档。新增完整程序目录 `releases/V0.1.10`，旧程序、用户模型、数据库与报告保留。

## 新增验证

- Python 子进程分别向 stdout / stderr 输出 9,000,000 字节无换行内容后再输出中文尾部；两路均排空、尾部正确、截断标志为真，父 Python 进程的 `tracemalloc` 峰值小于 4 MiB。此数值只描述该日志测试的 Python 分配，不是模型转换的总内存测量。
- 短日志与空日志保持原样；版本或 probe 输出被截断时不得接受为完整响应。进程启动失败、第二个日志线程无法启动时，不遗留读取线程；线程准备失败不启动转换程序。
- CTest 使用真实 C++ 模拟 writer 输出 12 MiB 内容后以错误退出。CLI 返回最后的 `FINAL_WRITER_ERROR`、明确标记截断，stderr 小于 68,000 字节，失败报告保留最终错误，清理本次拥有的暂存目录。
- 成功与失败报告保存 WKID / XYZ。CLI 解析已完成但源文件缺失、FBX 内容损坏或模型校验被拒绝时，报告仍保留 `gis-static`、缺图策略及 `(-12.25, 0, 100)`。CLI、GUI 和 Python 拒绝缺失或不匹配的成功定位报告。
- Python 的完成回调故意抛异常：`CallbackError.result` 保留有效结果，进程退出码为 0；使用该结果直接复制 GDB 并独立回读，原点为 `(-12.25, 0, 123.125)`。告警回调异常同样保留结果，开始前回调异常不启动转换。
- 七个真实调用场景覆盖贴图、缺图回退、严格拒绝、静态法线修复、已有成果保护和完成消息恢复。四份成功成果均独立复制回读，原输入散列不变，Python 未导入 `arcpy`。

GUI 首轮为 157/161：四个模拟引擎场景尚未把 `--origin` 参数填入新增的报告元数据，导致测试桩在写报告前退出。已修正测试桩的参数传递，重新执行整套测试得到 161/161；真实原生转换在两轮中均通过。保留[首轮记录](evidence/V0.1.10/windows-gui-initial.json)和[最终记录](evidence/V0.1.10/windows-gui.json)。

## 证据与范围

日志及 JSON 摘要位于 [evidence/V0.1.10](evidence/V0.1.10)，本机用户 / 工作目录已替换为占位符。

- Windows：[最终 CTest](evidence/V0.1.10/windows-ctest-final.log)、[Python 契约](evidence/V0.1.10/windows-python-unit.log)、[真实调用](evidence/V0.1.10/windows-python-client.json)、[独立部署](evidence/V0.1.10/windows-deployment.json)。
- Ubuntu：[最终 CTest](evidence/V0.1.10/ubuntu-ctest-final.log)、[Python 契约](evidence/V0.1.10/ubuntu-python-unit.log)、[真实调用](evidence/V0.1.10/ubuntu-python-client.json)、[独立部署](evidence/V0.1.10/ubuntu-deployment.json)。
- 构建、原生后端、样例与发布清单见同目录中的对应日志和摘要。

未制作或测试 ATBX，未实现进程树取消、追加已有 GDB、重投影或新的材质兼容策略。数据库回读不代表三维外观验收；Python 3.9 的 CI 矩阵配置仍不等于本地或托管 3.9 已验证。旧大模型结果仍属于 [V0.1.8](validation-v0.1.8.md)，Python 调用库首版结果见 [V0.1.9](validation-v0.1.9.md)。
