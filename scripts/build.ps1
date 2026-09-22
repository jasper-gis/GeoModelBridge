[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')][string]$Configuration = 'Release',
    [switch]$WithNative,
    [switch]$CoreOnly,
    [switch]$WithGui,
    [string]$FileGDBApiRoot = $env:FILEGDB_API_ROOT,
    [switch]$IncludeFileGDBRuntime,
    [switch]$ExcludeFileGDBRuntime,
    [string]$BuildDirectory = '',
    [string]$InstallDirectory = ''
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$buildNative = -not $CoreOnly
if ($CoreOnly -and ($WithNative -or $WithGui)) { throw '-CoreOnly cannot be combined with -WithNative or -WithGui.' }
if ($IncludeFileGDBRuntime -and $ExcludeFileGDBRuntime) { throw 'Choose only one of -IncludeFileGDBRuntime and -ExcludeFileGDBRuntime.' }
if (($IncludeFileGDBRuntime -or $ExcludeFileGDBRuntime) -and $CoreOnly) { throw 'FileGDB runtime options cannot be used with -CoreOnly.' }
if ($buildNative) {
    if (-not $FileGDBApiRoot) { $FileGDBApiRoot = Join-Path $projectRoot 'build\filegdb-sdk' }
    $FileGDBApiRoot = [IO.Path]::GetFullPath($FileGDBApiRoot)
    if (-not (Test-Path -LiteralPath (Join-Path $FileGDBApiRoot 'include\FileGDBAPI.h') -PathType Leaf)) { throw "SDK not found at $FileGDBApiRoot. Run python scripts/fetch_filegdb_sdk.py --output build/filegdb-sdk or supply -FileGDBApiRoot <SDK>. For inspect/prepare only, use -CoreOnly." }
    if (-not (Get-Command cl -ErrorAction SilentlyContinue)) { throw 'Build the native backend in an x64 Visual Studio C++ developer terminal (cl.exe must be on PATH).' }
    if ($Configuration -eq 'Debug') { throw 'The pinned native FileGDB SDK requires Release; use -CoreOnly -Configuration Debug for core debugging.' }
}
if (-not $BuildDirectory) {
    $buildName = if ($CoreOnly) { "core-$($Configuration.ToLowerInvariant())" } else { $Configuration.ToLowerInvariant() }
    $BuildDirectory = Join-Path $projectRoot "build\$buildName"
}
if (-not $InstallDirectory) { $InstallDirectory = Join-Path $projectRoot 'dist' }
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$InstallDirectory = [IO.Path]::GetFullPath($InstallDirectory)
if (Test-Path -LiteralPath (Join-Path $InstallDirectory 'bin\arcgis-pro')) {
    throw 'The install directory contains a removed backend. Use a new -InstallDirectory; existing installations are preserved.'
}

function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
}
Invoke-Checked python @((Join-Path $PSScriptRoot 'verify_dependencies.py'), '--install-dir', $InstallDirectory)
Invoke-Checked python @((Join-Path $PSScriptRoot 'check_version.py'))
# Both native executables share one root configure/build/install and toolchain.
$nativeOption = if ($buildNative) { 'ON' } else { 'OFF' }
$runtimeOption = if ($ExcludeFileGDBRuntime) { 'OFF' } else { 'ON' }
$configureArguments = @('-S', $projectRoot, '-B', $BuildDirectory, '-G', 'Ninja', "-DCMAKE_BUILD_TYPE=$Configuration", '-DGMB_BUILD_TESTS=ON', "-DGMB_BUILD_NATIVE=$nativeOption")
if ($buildNative) {
    $configureArguments += @('-DCMAKE_C_COMPILER=cl', '-DCMAKE_CXX_COMPILER=cl', "-DFILEGDB_API_ROOT=$FileGDBApiRoot", "-DGMB_INSTALL_FILEGDB_RUNTIME=$runtimeOption")
}
Invoke-Checked cmake $configureArguments
Invoke-Checked cmake @('--build', $BuildDirectory, '--parallel')
Invoke-Checked ctest @('--test-dir', $BuildDirectory, '--output-on-failure')
Invoke-Checked cmake @('--install', $BuildDirectory, '--prefix', $InstallDirectory)
if ($buildNative) {
    $previousNativePath = $env:PATH
    try {
        if ($ExcludeFileGDBRuntime) {
            $env:PATH = (Join-Path $FileGDBApiRoot 'bin64') + ';' + $env:PATH
            Write-Warning 'SDK runtime not bundled. Probe uses the development SDK; customer deployment must supply its own DLL. Existing copied DLLs are not removed.'
        }
        Invoke-Checked (Join-Path $InstallDirectory 'bin\native-filegdb\GeoModelBridge.NativeWriter.exe') @('--probe')
    } finally { $env:PATH = $previousNativePath }
}
if ($WithGui) {
    $guiProject = Join-Path $projectRoot 'apps\GeoModelBridge.Gui\GeoModelBridge.Gui.csproj'
    $guiOutput = Join-Path $InstallDirectory 'bin'
    Invoke-Checked dotnet @('publish', $guiProject, '-c', $Configuration, '-o', $guiOutput)
    $guiExecutable = Join-Path $guiOutput 'geomodelbridgeGUI.exe'
    if (-not (Test-Path -LiteralPath $guiExecutable -PathType Leaf)) { throw "GUI build did not produce $guiExecutable" }
    Invoke-Checked python @((Join-Path $PSScriptRoot 'install_dotnet_licenses.py'), '--assets', (Join-Path $projectRoot 'apps\GeoModelBridge.Gui\obj\project.assets.json'), '--output', (Join-Path $InstallDirectory 'licenses\dotnet'))
    Write-Host "GUI: $guiExecutable"
}
Invoke-Checked python @((Join-Path $PSScriptRoot 'verify_dependencies.py'), '--install-dir', $InstallDirectory)
Write-Host "GeoModelBridge built and tested. CLI: $(Join-Path $InstallDirectory 'bin\geomodelbridge.exe')"
if ($buildNative) {
    Write-Host "Native writer: $(Join-Path $InstallDirectory 'bin\native-filegdb\GeoModelBridge.NativeWriter.exe') (runtime bundling: $runtimeOption)"
} else {
    Write-Warning 'Core-only build: no native writer or FileGDBAPI.dll was built. For GDB conversion omit -CoreOnly and supply the SDK.'
}
Write-Host 'CLI help: geomodelbridge.exe convert -h; sequential calling examples: docs/command-line.md'
