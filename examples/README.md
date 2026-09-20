# 原生 FileGDB 参考成果

先构建完整原生发布目录，再运行：

```powershell
python scripts/generate_examples.py --cli releases/V0.1.5/bin/geomodelbridge.exe --output examples/V0.1.5
```

脚本只使用原生 FileGDB 后端，从 CLI 同目录发现原生写入端，生成五组合成网格及九个 FBX 的 GDB，验证独立复制、输入拒绝及写入失败报告。输出目录必须不存在，生成的 GDB 不进入源码 Git。

`verification-summary.json` 汇总数据核验；`reports/` 保存具体结果。位置 `EPSG:32650 / origin 500000,3000000,100` 仅为测试参数。显示验收独立进行。

旧版本目录是原始历史成果，保持其版本、后端和验证来源，不代表当前依赖。
