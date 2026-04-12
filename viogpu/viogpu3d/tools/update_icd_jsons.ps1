param(
  [string]$MesaPrefix = $env:MESA_PREFIX,
  [string]$OutDir = (Join-Path $PSScriptRoot '..\icd'),
  [string]$MesaArch = ''
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

if ([string]::IsNullOrWhiteSpace($MesaArch)) {
  if (Test-Path (Join-Path $sourceDir 'virtio_icd.aarch64.json')) {
    $MesaArch = 'aarch64'
  } elseif (Test-Path (Join-Path $sourceDir 'virtio_icd.x86_64.json')) {
    $MesaArch = 'x86_64'
  } elseif (Test-Path (Join-Path $sourceDir 'virtio_icd.x86.json')) {
    $MesaArch = 'x86'
  } elseif (Test-Path (Join-Path $sourceDir 'virtio_icd.i686.json')) {
    $MesaArch = 'x86'
  } else {
    throw 'Unable to infer Mesa ICD arch. Pass -MesaArch (x86_64/aarch64/x86).'
  }
}

switch ($MesaArch) {
  'x86_64' {
    $icdFiles = @(
      @{ Name = 'virtio_icd.x86_64.json'; Legacy = @(); Dll = 'libvulkan_virtio.dll' },
      @{ Name = 'lvp_icd.x86_64.json'; Legacy = @(); Dll = 'vulkan_lvp.dll' }
    )
  }
  'aarch64' {
    $icdFiles = @(
      @{ Name = 'virtio_icd.aarch64.json'; Legacy = @(); Dll = 'libvulkan_virtio.dll' },
      @{ Name = 'lvp_icd.aarch64.json'; Legacy = @(); Dll = 'vulkan_lvp.dll' }
    )
  }
  'x86' {
    $icdFiles = @(
      @{ Name = 'virtio_icd.x86.json'; Legacy = @('virtio_icd.i686.json'); Dll = 'libvulkan_virtio.dll' },
      @{ Name = 'lvp_icd.x86.json'; Legacy = @('lvp_icd.i686.json'); Dll = 'vulkan_lvp.dll' }
    )
  }
  default {
    throw "Unsupported MesaArch: $MesaArch"
  }
}

$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

foreach ($icd in $icdFiles) {
  $src = Join-Path $sourceDir $icd.Name
  if (-not (Test-Path $src)) {
    foreach ($legacyName in $icd.Legacy) {
      $legacySrc = Join-Path $sourceDir $legacyName
      if (Test-Path $legacySrc) {
        $src = $legacySrc
        break
      }
    }
  }
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