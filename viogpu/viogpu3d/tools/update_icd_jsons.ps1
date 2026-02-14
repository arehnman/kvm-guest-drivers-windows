param(
  [string]$MesaPrefix = $env:MESA_PREFIX,
  [string]$OutDir = (Join-Path $PSScriptRoot '..\icd')
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($MesaPrefix)) {
  throw 'Set MESA_PREFIX or pass -MesaPrefix.'
}

$sourceDir = Join-Path $MesaPrefix 'share\vulkan\icd.d'
$binDir = Join-Path $MesaPrefix 'bin'

if (-not (Test-Path $sourceDir)) {
  throw "Missing ICD directory: $sourceDir"
}
if (-not (Test-Path $binDir)) {
  throw "Missing bin directory: $binDir"
}
if (-not (Test-Path $OutDir)) {
  New-Item -ItemType Directory -Path $OutDir | Out-Null
}

$icdFiles = @(
  @{ Name = 'virtio_icd.x86_64.json'; Dll = 'libvulkan_virtio.dll' },
  @{ Name = 'lvp_icd.x86_64.json'; Dll = 'vulkan_lvp.dll' }
)

$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

foreach ($icd in $icdFiles) {
  $src = Join-Path $sourceDir $icd.Name
  $dst = Join-Path $OutDir $icd.Name
  $dllPath = Join-Path $binDir $icd.Dll

  if (-not (Test-Path $src)) {
    throw "Missing source ICD JSON: $src"
  }
  if (-not (Test-Path $dllPath)) {
    throw "Missing ICD DLL: $dllPath"
  }

  $obj = Get-Content -Raw -Path $src | ConvertFrom-Json
  if (-not $obj.ICD) {
    throw "Invalid ICD JSON (missing ICD object): $src"
  }

  $obj.ICD.library_path = $icd.Dll
  $json = $obj | ConvertTo-Json -Depth 10
  [System.IO.File]::WriteAllText($dst, $json, $utf8NoBom)
}

Write-Host "Updated ICD JSONs in $OutDir"