# sopt bench: times every orig / sopt pair of a test package with sopt-host + ReShade + the
# sopt-timer add-on, on DX11 and Vulkan (-Apis: any of dx9, dx10, dx11, dx12, vulkan, gl), saves a screenshot per
# preset and zips the results. Started from Test-Host.bat. Everything stays inside this folder.
#
# Needs, in this folder (or given as parameters):
#   - ReShade64.dll from ReShade *with full add-on support* (or the ReShade_Setup_*_Addon.exe
#     installer itself; the DLL is taken out of it),
#   - the test package (a folder with sopt-presets\ and reshade-shaders\, e.g. the extracted
#     sopt-compare-*.zip),
#   - sopt-host.exe and sopt-timer.addon64 (in bin\, from the Test Host download).
param(
  [string]$ReShade = "",
  [string]$Package = "",
  [string[]]$Apis = @("dx11", "vulkan"),
  [int]$Width = 3840,
  [int]$Height = 2160,
  [int]$Frames = 300
)
. (Join-Path $PSScriptRoot "common.ps1")
$ReShade = Find-ReShade $ReShade

if (-not $Package) {
  $cand = @($here) + @(Get-ChildItem -Path $here -Directory | ForEach-Object { $_.FullName })
  foreach ($c in $cand) {
    if ((Test-Path (Join-Path $c "sopt-presets")) -and (Test-Path (Join-Path $c "reshade-shaders"))) { $Package = $c; break }
  }
}
if (-not $Package) { Fail "No test package found: extract the sopt-compare-*.zip next to Test-Host.bat." }
$presets = Get-ChildItem -Path (Join-Path $Package "sopt-presets") -Filter "sopt-*.ini" | Sort-Object Name
if ($presets.Count -eq 0) { Fail "The test package has no sopt-*.ini presets." }

# --- a run folder with its own ReShade.ini ----------------------------------------------
$stamp = Get-Date -Format "yyyy-MM-dd_HHmm"
$results = Join-Path $here "Results\bench-$stamp"
$shots = Join-Path $results "screenshots"
$run = New-RunFolder $ReShade
New-Item -ItemType Directory -Force -Path $results, $shots | Out-Null

$shaders = Join-Path $Package "reshade-shaders\Shaders"
$textures = Join-Path $Package "reshade-shaders\Textures"
Write-ReShadeIni $run $shaders $textures $presets[0].FullName $shots $false @{
  SOPT_TIMER = [ordered]@{ AutoRun = 1; ExitWhenDone = 1; Screenshots = 1; Frames = $Frames }
}

# --- the runs ---------------------------------------------------------------------------
$summary = @()
foreach ($api in (Resolve-Apis $Apis)) {
  Write-Host ""
  Write-Host "=== $api : $($presets.Count) presets, $Frames frames each (the window closes by itself) ===" -ForegroundColor Cyan
  $csv = Join-Path $run "sopt-timer.csv"
  Remove-Item $csv -ErrorAction SilentlyContinue
  Enable-ReShade $run $api
  $p = Start-Process -FilePath (Join-Path $run "sopt-host.exe") -WorkingDirectory $run -PassThru -Wait `
    -ArgumentList @("--api", $api, "--bench", "--width", $Width, "--height", $Height)
  Disable-ReShade $run
  $log = Join-Path $run "ReShade.log"
  if (Test-Path $log) { Copy-Item $log (Join-Path $results "ReShade-$api.log") -Force }
  if (Test-Path $csv) {
    Move-Item $csv (Join-Path $results "sopt-timer-$api.csv") -Force
    $summary += "$api : OK"
  } else {
    $summary += "$api : no sopt-timer.csv (exit code $($p.ExitCode)); see ReShade-$api.log"
  }
  # Screenshots of this API in their own folder.
  $apiShots = Join-Path $shots $api
  New-Item -ItemType Directory -Force -Path $apiShots | Out-Null
  Get-ChildItem -Path $shots -File | Move-Item -Destination $apiShots -Force
}

# --- results ----------------------------------------------------------------------------
Get-GpuInfo | Set-Content (Join-Path $results "gpu.txt")
$summary | Set-Content (Join-Path $results "summary.txt")
$zip = "$results.zip"
Compress-Archive -Path "$results\*" -DestinationPath $zip -Force
Write-Host ""
$summary | ForEach-Object { Write-Host $_ }
Write-Host ""
Write-Host "Done. Send this file: $zip" -ForegroundColor Green
