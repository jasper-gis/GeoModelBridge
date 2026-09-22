# 纯命令行与连续调用 EXE · V0.2.2

[首页](../README.md) · [构建和依赖排错](build-and-release.md) · [FileGDB API 调用与依赖说明](filegdb-api.md)

## 入口与一次转换

Windows 使用 `dist/bin/geomodelbridge.exe`；Linux 使用 `dist/bin/geomodelbridge`。两者均为独立控制台程序，不启动 GUI、不需要 .NET、ArcPy 或 ArcGIS Pro。`geomodelbridgeGUI.exe` 才是 Windows GUI。主 CLI 解析 FBX，启动原生 writer 子进程并等待其完成，核对成功报告后退出。

在 PowerShell 中，从仓库根目录执行（测试定位值必须替换为模型实际坐标）：

```powershell
.\dist\bin\geomodelbridge.exe -h
.\dist\bin\geomodelbridge.exe convert -h
.\dist\bin\geomodelbridge.exe doctor
.\dist\bin\native-filegdb\GeoModelBridge.NativeWriter.exe --probe
.\dist\bin\geomodelbridge.exe convert ".\dist\bin\demo\textured_quad.fbx" --output ".\new-demo.gdb" --wkid 32650 --origin 500000 3000000 100 --feature-class Models --report ".\new-demo.json"
if ($LASTEXITCODE -ne 0) { throw "转换失败，退出码 $LASTEXITCODE" }
```

最后一次调用产生 `new-demo.gdb/Models` 和 `new-demo.json`。不写 `--report` 时默认为 `<输出GDB路径>.report.json`。输出 GDB 与报告均必须不存在，报告必须位于 GDB 目录外。不能追加要素类、覆盖已有 GDB，也没有内置 `--batch` / 通配符参数。一个进程接收一个 FBX；一个 FBX 中多个 mesh 可形成多个要素。

`doctor` 的 `writer_present=true` 只说明可执行文件存在；`--probe` 才实际加载 SDK 和检查坐标系目录。完整写入检查须运行上面的演示转换。普通 FBX 调用主 CLI，不把 FBX 或 `FileGDBAPI.dll` 传给 writer 的 `--input` / CLI 的 `--writer`。

## 参数表

| 参数 / 命令 | 含义与默认行为 |
| --- | --- |
| `convert INPUT.fbx` | 转换为新 FileGDB，写入后关闭、重新打开并核验 |
| `inspect INPUT.fbx` | 解析和校验，只有指定 `--report` 才输出报告，不生成 GDB |
| `prepare INPUT.fbx` | 生成含 `scene.json`、纹理和 `report.json` 的中间包，不生成 GDB |
| `fixture NAME\|all` | 生成合成测试中间包；不是 FBX 批量入口 |
| `-h` / `--help` | 总帮助；五个子命令也支持 `COMMAND -h` / `COMMAND --help` |
| `--version` | 当前引擎版本 |
| `-o PATH` / `--output PATH` | convert、prepare、fixture 必填；convert 路径以 `.gdb` 结尾 |
| `--wkid N` | convert 必填，SDK 支持的米制投影坐标系 ID |
| `--origin X Y Z` | convert 必填，有限数值，单位米；已定位模型也要显式传 `0 0 0` |
| `--feature-class NAME` | convert 的要素类名称，默认 `Models` |
| `--report NEW.json` | 新报告路径；convert 缺省为 `<output>.report.json`，其他命令可选；`fixture all` 不接受外部报告 |
| `--texture-dir DIR` | FBX 的附加贴图搜索目录，可重复指定 |
| `--profile strict\|gis-static` | 默认 `strict`；`gis-static` 显式使用保存的静态姿态、允许有记录的材质简化 / 退化面移除 / 无效法线修复 |
| `--missing-textures material-color\|error` | 默认真正缺图时保留颜色及标量透明度并告警；error 改为拒绝；损坏、不可读或非普通文件仍失败 |
| `--backend native-filegdb` | convert 默认值，也是唯一支持的后端 |
| `--writer PATH` | convert 的原生可执行文件路径；优先于 `GMB_NATIVE_WRITER` 环境变量，再次为 CLI 同目录下 `native-filegdb/GeoModelBridge.NativeWriter[.exe]` |

WKID 赋值和 origin 平移不执行重投影。输入的节点变换和单位规范化在 FBX 读取阶段完成，不要在外部再重复应用。实际 GIS 兼容调整及缺图回退见报告的诊断项；退出 0 可以伴随警告。

参数值不能为空；除可重复的 `--texture-dir` 外，同一选项只能出现一次，`-o` 与 `--output` 视为同一选项。WKID 必须为正十进制整数，不接受小数、指数或超出 32 位有符号整数范围的写法。

报告路径必须在输出目录之外，Windows 比较路径组件时不区分大小写，Linux 区分大小写。Windows 路径组件不能以点或空格结尾（正常的 `.` / `..` 路径片段除外）；这类不明确的路径会在写出前拒绝。

## PowerShell 连续调用

下例可保存为 `convert-many.ps1`。每项独立配置输入和定位；使用新的结果目录，碰到失败即停止。把示例输入与坐标改成实际值。在 PowerShell 中，变量表示的 EXE 必须用 `&` 调用；控制台 EXE 是同步执行的，不需要 `Start-Process`。示例成果写到系统临时目录的独立子目录，正式使用时可改成较短的成果根路径；Windows 过深的输出路径可能使内部中间贴图超过文件路径限制。

```powershell
$ErrorActionPreference = 'Stop'
$exe = (Resolve-Path '.\dist\bin\geomodelbridge.exe').Path
$out = Join-Path ([IO.Path]::GetTempPath()) ('gmb-results-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $out -ErrorAction Stop | Out-Null
$jobs = @(
    @{ Input = '.\dist\bin\demo\textured_quad.fbx'; Name = 'first'; Wkid = 32650; Origin = @(500000, 3000000, 100) },
    @{ Input = '.\dist\bin\demo\textured_quad.fbx'; Name = 'second'; Wkid = 3857; Origin = @(100, 200, 300) }
)
foreach ($job in $jobs) {
    $gdb = Join-Path $out ($job.Name + '.gdb')
    $report = $gdb + '.report.json'
    $cliArgs = @('convert', $job.Input, '--output', $gdb,
                 '--wkid', $job.Wkid, '--origin') + $job.Origin +
               @('--feature-class', 'Models', '--report', $report)
    & $exe @cliArgs
    $code = $LASTEXITCODE
    if ($code -ne 0) { throw "转换 $($job.Input) 失败，退出码 $code；查看 $report（若已生成）" }
    $r = Get-Content -LiteralPath $report -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($r.status -ne 'written_and_readback_verified' -or
        $r.backend -ne 'native-filegdb' -or
        $r.verification.geometry_material_uv_texture_readback -ne $true -or
        $r.coordinate_system.wkid -ne $job.Wkid -or
        $r.coordinates.wkid -ne $job.Wkid -or
        $r.coordinates.origin_explicit -ne $true -or
        ($r.coordinates.origin -join ',') -ne ($job.Origin -join ',')) {
        throw "成功报告或定位参数不匹配：$report"
    }
    Write-Host "已完成：$gdb；报告：$report"
}
```

需要宿主程序严格校验版本、路径、坐标和报告结构时，使用已有的 [Python 标准库客户端](python-client.md)。直接启动 EXE 的 C# / 其他语言也应传参数数组、等待退出、同时排空 stdout 与 stderr，再读取报告；不要只判断目标目录存在。

## cmd.exe 与 Linux Bash

Windows 批处理文件 `.cmd` 中，每条命令执行后立即检查退出码（不要直接粘贴到 PowerShell）：

```bat
@echo off
set "EXE=%~dp0dist\bin\geomodelbridge.exe"
"%EXE%" convert "%~dp0dist\bin\demo\textured_quad.fbx" --output "%~dp0new-a.gdb" --wkid 32650 --origin 500000 3000000 100
if errorlevel 1 exit /b %errorlevel%
"%EXE%" convert "%~dp0dist\bin\demo\textured_quad.fbx" --output "%~dp0new-b.gdb" --wkid 3857 --origin 100 200 300
if errorlevel 1 exit /b %errorlevel%
```

Bash 中，对同一定位下多个 FBX 连续调用（脚本运行目录包含 `models/*.fbx`）：

```bash
#!/usr/bin/env bash
set -euo pipefail
shopt -s nullglob
exe="$PWD/dist/bin/geomodelbridge"
files=(models/*.fbx)
((${#files[@]} > 0)) || { echo '没有找到 FBX' >&2; exit 2; }
out=$(mktemp -d "$PWD/results-XXXXXXXX")
for input in "${files[@]}"; do
  name=$(basename "$input" .fbx)
  "$exe" convert "$input" --output "$out/$name.gdb" \
    --wkid 32650 --origin 500000 3000000 100
done
```

三种方式都不复用同名 GDB。重试时换新输出目录；已成功的成果保留。若模型有不同坐标系或原点，应逐项传递正确定位，不要共用演示值。

## 退出码与失败诊断

| 退出码 | 含义 |
| --- | --- |
| 0 | 命令成功；convert 已核对写入回读报告；doctor 仅完成发现检查 |
| 2 | 参数错误、已有输出 / 报告冲突 |
| 3 | 模型或转换策略拒绝 |
| 4 | 不支持的后端或 writer 文件不存在 |
| 5 | 已启动的 writer 失败，含 DLL 加载失败导致的进程异常 |
| 6 | 解析、IO、无法启动进程、报告核验或其他操作异常 |

失败先看 stderr；报告仅在可安全创建且处理已到报告阶段时生成，不能假定每次参数错误都有报告。成功 GDB 的三维外观验收仍是单独步骤。
