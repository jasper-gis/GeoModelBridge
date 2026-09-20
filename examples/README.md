# 参考成果

`V0.1.0/` 是本机实际运行生成的参考成果。含五组合成网格、八个自造 ASCII FBX 和一个 Blender 二进制 FBX 的 GDB，转换报告，以及脱离原中间包路径后的 GDB 副本与核验报告。

```powershell
python .\scripts\generate_examples.py --output .\examples\new-run
```

需要先运行 `scripts/build.ps1 -WithPro`，且本机 ArcGIS Pro 许可可用。脚本拒绝已存在的目标目录。生成的 GDB 不进入源码 Git，但包含在完整 V0.1.0 交付 ZIP 中。

所有模型的位置 `EPSG:32650 / origin 500000,3000000,100` 均为测试参数，没有真实地理含义。`filegdb/*.gdb` 中的要素类名为 `Models`。

`verification-summary.json` 汇总实际数据核验；`reports/` 保存材質量化、UV、法线及纹理读回的详细结果。`graphical_acceptance` 为待人工验收，不应把数据通过等同于已完成 GIS 显示验证。
