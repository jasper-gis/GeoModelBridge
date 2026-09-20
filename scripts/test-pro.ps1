[CmdletBinding()]
param([string]$Writer = '', [string]$WorkDirectory = '')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $Writer) { $Writer = Join-Path $projectRoot 'dist\bin\arcgis-pro\GeoModelBridge.ProWriter.exe' }
if (-not $WorkDirectory) { $WorkDirectory = Join-Path $projectRoot 'build\pro-integration' }
& python (Join-Path $projectRoot 'backends\arcgis-pro\tests\integration.py') --writer $Writer --work $WorkDirectory
if ($LASTEXITCODE -ne 0) { throw "Pro integration tests failed with exit code $LASTEXITCODE" }
