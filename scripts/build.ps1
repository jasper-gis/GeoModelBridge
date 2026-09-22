[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')][string]$Configuration = 'Release',
    [switch]$WithNative,
    [switch]$WithGui,
    [string]$FileGDBApiRoot = $env:FILEGDB_API_ROOT,
    [switch]$IncludeFileGDBRuntime,
    [switch]$ExcludeFileGDBRuntime,
    [string]$NativeBuildDirectory = '',
    [string]$BuildDirectory = '',
    [string]$InstallDirectory = ''
)
$ErrorActionPreference = 'Stop'
if ($IncludeFileGDBRuntime -and $ExcludeFileGDBRuntime) { throw 'Choose only one of -IncludeFileGDBRuntime and -ExcludeFileGDBRuntime.' }
if (($IncludeFileGDBRuntime -or $ExcludeFileGDBRuntime) -and -not $WithNative) { throw 'FileGDB runtime options require -WithNative.' }
if ($WithNative) {
    if (-not $FileGDBApiRoot) { throw '-WithNative requires -FileGDBApiRoot or FILEGDB_API_ROOT. Run python scripts/fetch_filegdb_sdk.py --output build/filegdb-sdk first.' }
    if (-not (Get-Command cl -ErrorAction SilentlyContinue)) { throw 'Build the native backend in an x64 Visual Studio C++ developer terminal (cl.exe must be on PATH).' }
    if ($Configuration -eq 'Debug') { throw 'The pinned native FileGDB SDK requires Release; use core-only builds for Debug.' }
}
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $projectRoot "build\$($Configuration.ToLowerInvariant())" }
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
# Use a VS developer terminal for MSVC, or a PATH containing MinGW GCC and Ninja.
Invoke-Checked cmake @('-S', $projectRoot, '-B', $BuildDirectory, '-G', 'Ninja', "-DCMAKE_BUILD_TYPE=$Configuration", '-DGMB_BUILD_TESTS=ON', '-DGMB_BUILD_NATIVE=OFF')
Invoke-Checked cmake @('--build', $BuildDirectory, '--parallel')
Invoke-Checked ctest @('--test-dir', $BuildDirectory, '--output-on-failure')
Invoke-Checked cmake @('--install', $BuildDirectory, '--prefix', $InstallDirectory)
if ($WithNative) {
    if (-not $NativeBuildDirectory) { $NativeBuildDirectory = Join-Path $projectRoot 'build\native-filegdb' }
    $nativeSource = Join-Path $projectRoot 'backends\native-filegdb'
    $runtimeOption = if ($ExcludeFileGDBRuntime) { 'OFF' } else { 'ON' }
    Invoke-Checked cmake @('-S', $nativeSource, '-B', $NativeBuildDirectory, '-G', 'Ninja', '-DCMAKE_CXX_COMPILER=cl', "-DCMAKE_BUILD_TYPE=$Configuration", "-DFILEGDB_API_ROOT=$FileGDBApiRoot", "-DGMB_INSTALL_FILEGDB_RUNTIME=$runtimeOption")
    Invoke-Checked cmake @('--build', $NativeBuildDirectory, '--parallel')
    if (-not $ExcludeFileGDBRuntime) {
        Invoke-Checked python @((Join-Path $projectRoot 'tests/native_runtime_test.py'), '--writer', (Join-Path $NativeBuildDirectory 'GeoModelBridge.NativeWriter.exe'))
    }
    Invoke-Checked cmake @('--install', $NativeBuildDirectory, '--prefix', $InstallDirectory)
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
if ($WithNative) {
    Write-Host "Native writer: $(Join-Path $InstallDirectory 'bin\native-filegdb\GeoModelBridge.NativeWriter.exe') (runtime bundling: $runtimeOption)"
} else {
    Write-Warning 'Core-only build: no native writer or FileGDBAPI.dll was built. For GDB conversion use -WithNative -FileGDBApiRoot <SDK>.'
}
Write-Host 'CLI help: geomodelbridge.exe convert -h; sequential calling examples: docs/command-line.md'
