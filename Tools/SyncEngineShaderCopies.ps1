# Copies the package's particle shaders over the engine's copies in Data/Base/Shaders/Particles.
#
# The package's Assets/Shaders/Particles files are the masters: with the package installed they shadow the engine's,
# so every edit happens here. Run this before an engine release so the engine's copies match and an engine shader
# build without the package compiles the same code. Files only the engine has (the GPU simulation shaders, the
# shared constant and data headers) stay as they are.
#
# Usage: .\SyncEngineShaderCopies.ps1 [-Engine E:\PlasmaTech\PlasmaEngine2] [-WhatIf]

param(
  [string]$Engine = "E:\PlasmaTech\PlasmaEngine2",
  [switch]$WhatIf
)

$source = Join-Path $PSScriptRoot "..\Assets\Shaders\Particles"
$target = Join-Path $Engine "Data\Base\Shaders\Particles"

if (-not (Test-Path $source)) { throw "Package shader folder not found: $source" }
if (-not (Test-Path $target)) { throw "Engine shader folder not found: $target" }

$changed = 0
foreach ($file in Get-ChildItem $source -File)
{
  $destination = Join-Path $target $file.Name
  $same = (Test-Path $destination) -and ((Get-FileHash $file.FullName).Hash -eq (Get-FileHash $destination).Hash)
  if ($same) { continue }

  $changed++
  if ($WhatIf)
  {
    Write-Output "would copy $($file.Name)"
  }
  else
  {
    Copy-Item $file.FullName $destination -Force
    Write-Output "copied $($file.Name)"
  }
}

Write-Output "$changed file(s) differed."
