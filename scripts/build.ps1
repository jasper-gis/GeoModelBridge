[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')][string]$Configuration = 'Release',
    [switch]$WithPro,
    [switch]$WithNative,
    [switch]$WithGui,
    [string]$FileGDBApiRoot = $env:FILEGDB_API_ROOT,
    [switch]$IncludeFileGDBRuntime,
    [string]$NativeBuildDirectory = '',
    [string]$ArcGISProInstallDir = 'C:\Program Files\ArcGIS\Pro',
    [string]$BuildDirectory = '',
    [string]$InstallDirectory = ''
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $projectRoot "build\$($Configuration.ToLowerInvariant())" }
if (-not $InstallDirectory) { $InstallDirectory = Join-Path $projectRoot 'dist' }
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$InstallDirectory = [IO.Path]::GetFullPath($InstallDirectory)

function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
}
Invoke-Checked python @((Join-Path $PSScriptRoot 'verify_dependencies.py'))
Invoke-Checked python @((Join-Path $PSScriptRoot 'check_version.py'))
# Use a VS developer terminal for MSVC, or a PATH containing MinGW GCC and Ninja.
Invoke-Checked cmake @('-S', $projectRoot, '-B', $BuildDirectory, '-G', 'Ninja', "-DCMAKE_BUILD_TYPE=$Configuration", '-DGMB_BUILD_TESTS=ON')
Invoke-Checked cmake @('--build', $BuildDirectory, '--parallel')
Invoke-Checked ctest @('--test-dir', $BuildDirectory, '--output-on-failure')
Invoke-Checked cmake @('--install', $BuildDirectory, '--prefix', $InstallDirectory)
if ($WithPro) {
    $adapter = Join-Path $projectRoot 'backends\arcgis-pro\GeoModelBridge.ArcGISPro.csproj'
    $writerOutput = Join-Path $InstallDirectory 'bin\arcgis-pro'
    Invoke-Checked dotnet @('build', $adapter, '-c', $Configuration, "-p:ArcGISProInstallDir=$ArcGISProInstallDir", '-o', $writerOutput)
    $previousProPath = $env:ARCGIS_PRO_INSTALL_DIR
    try {
        $env:ARCGIS_PRO_INSTALL_DIR = $ArcGISProInstallDir
        Invoke-Checked dotnet @((Join-Path $writerOutput 'GeoModelBridge.ProWriter.dll'), '--probe')
    } finally { $env:ARCGIS_PRO_INSTALL_DIR = $previousProPath }
}
if ($WithNative) {
    if (-not $FileGDBApiRoot) { throw '-WithNative requires -FileGDBApiRoot or FILEGDB_API_ROOT.' }
    if (-not (Get-Command cl -ErrorAction SilentlyContinue)) { throw 'Build the native backend in an x64 Visual Studio C++ developer terminal (cl.exe must be on PATH).' }
    if (-not $NativeBuildDirectory) { $NativeBuildDirectory = Join-Path $projectRoot 'build\native-filegdb' }
    $nativeSource = Join-Path $projectRoot 'backends\native-filegdb'
    $runtimeOption = if ($IncludeFileGDBRuntime) { 'ON' } else { 'OFF' }
    Invoke-Checked cmake @('-S', $nativeSource, '-B', $NativeBuildDirectory, '-G', 'Ninja', '-DCMAKE_CXX_COMPILER=cl', "-DCMAKE_BUILD_TYPE=$Configuration", "-DFILEGDB_API_ROOT=$FileGDBApiRoot", "-DGMB_INSTALL_FILEGDB_RUNTIME=$runtimeOption")
    Invoke-Checked cmake @('--build', $NativeBuildDirectory, '--parallel')
    Invoke-Checked cmake @('--install', $NativeBuildDirectory, '--prefix', $InstallDirectory)
    $previousNativePath = $env:PATH
    try {
        $env:PATH = (Join-Path $FileGDBApiRoot 'bin64') + ';' + $env:PATH
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
Write-Host "GeoModelBridge built and tested. CLI: $(Join-Path $InstallDirectory 'bin\geomodelbridge.exe')"
