# Runs ParticlesTest's GpuFluid tests headless on Windows (DX12 or Vulkan).
#
#   run_gallery.ps1 -Out <output dir> [-Filter <filter>] [-Renderer DX12|Vulkan] [-Config Development|Release] [-Extra <args>]
#
# Environment: W = engine worktree, PL_GPUFLUID_VIDEO=1 also records the video frames, PL_GPUFLUID_TUNE_<NAME> and
# PL_GPUFLUID_LIQ_<NAME> override look and tuning values. Changed shaders are not picked up from cached permutations,
# so every cached GpuFluid permutation is deleted first. Ends only its own process tree.
param(
  [Parameter(Mandatory = $true)][string]$Out,
  [string]$Filter = "GpuFluid Gallery",
  [string]$Renderer = "DX12",
  [string]$Config = "Development",
  [int]$Timeout = 1800,
  [string[]]$Extra = @()
)

$package = (Resolve-Path "$PSScriptRoot\..\..").Path
$engine = $env:W
if (-not $engine) { $engine = "C:\Users\drago\.t3\worktrees\PlasmaEngine2\feature-gpu-fluid-f9b626d89a90544f" }

$bin = "$engine\Binaries\Windows_$Config"
$packageBin = "$package\Bin\Windows_x64_$Config"
$exe = "$package\Bin\Tests\Windows_x64_$Config\ParticlesTest.exe"
if (-not (Test-Path $exe)) { Write-Output "No test binary at $exe - build ParticlesTest first."; exit 2 }

# The test runs next to the engine binaries so it finds the SDK root, the renderer plugins and the test data.
Copy-Item $exe "$bin\ParticlesTest.exe" -Force
Copy-Item ($exe -replace '\.exe$', '.pdb') "$bin\ParticlesTest.pdb" -Force -ErrorAction SilentlyContinue
$exe = "$bin\ParticlesTest.exe"

New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path

foreach ($cache in @("$engine\Output\ShaderCache", "$bin\ShaderCache"))
{
  if (Test-Path $cache)
  {
    Get-ChildItem $cache -Recurse -Directory -Filter GpuFluid -ErrorAction SilentlyContinue | ForEach-Object { Remove-Item $_.FullName -Recurse -Force -ErrorAction SilentlyContinue }
  }
}

$env:PATH = "$bin;$packageBin;" + $env:PATH
$env:PL_SILENT_ASSERTS = "1"

$argList = @("-run", "-close", "-noGui", "-noSave", "-all", "-assert", "0", "-renderer", $Renderer, "-outputDir", "`"$Out`"", "-json", "`"$Out\results.json`"", "-filter", "`"$Filter`"") + $Extra

Write-Output "Running '$Filter' on $Renderer ($Config) -> $Out"
$start = Get-Date
$p = Start-Process -FilePath $exe -ArgumentList $argList -WorkingDirectory $bin -PassThru -NoNewWindow `
  -RedirectStandardOutput "$Out\console.log" -RedirectStandardError "$Out\stderr.log"
$timedOut = $false
try
{
  if (-not $p.WaitForExit($Timeout * 1000)) { $timedOut = $true }
}
finally
{
  if (-not $p.HasExited) { taskkill /T /F /PID $p.Id | Out-Null }
}
$secs = [int]((Get-Date) - $start).TotalSeconds

Select-String -Path "$Out\console.log" -Pattern "Sub-Test .* (succeeded|failed)|Test .* (succeeded|failed)|Error:|N0 |N1|N2 |N5 |N6 |N7 |Tier " | Select-Object -Last 80 | ForEach-Object { $_.Line }
if ($timedOut) { Write-Output "TIMEOUT after $secs s" } else { Write-Output "exit $($p.ExitCode) after $secs s; log: $Out\console.log" }
