# sopt bench: times every orig / sopt pair of a test package with sopt-host + ReShade + the
# sopt-timer add-on, on DX11 and Vulkan, saves a screenshot per preset and zips the results.
# Start it with run-bench.bat (double-click). Everything stays inside this folder.
#
# Needs, in this folder (or given as parameters):
#   - ReShade64.dll from ReShade *with full add-on support* (or the ReShade_Setup_*_Addon.exe
#     installer itself; the DLL is taken out of it),
#   - the test package (a folder with sopt-presets\ and reshade-shaders\, e.g. the extracted
#     sopt-compare-*.zip),
#   - sopt-host.exe and sopt-timer.addon64 (from the sopt-windows-tools CI artifact).
param(
  [string]$ReShade = "",
  [string]$Package = "",
  [string[]]$Apis = @("dx11", "vulkan"),
  [int]$Width = 3840,
  [int]$Height = 2160,
  [int]$Frames = 300
)
$ErrorActionPreference = "Stop"
$here = $PSScriptRoot

function Fail($msg) {
  Write-Host ""
  Write-Host "ERROR: $msg" -ForegroundColor Red
  exit 1
}

# --- the pieces -------------------------------------------------------------------------
foreach ($f in @("sopt-host.exe", "sopt-timer.addon64")) {
  if (-not (Test-Path (Join-Path $here $f))) { Fail "$f is missing next to this script." }
}

if (-not $ReShade) {
  $dll = Join-Path $here "ReShade64.dll"
  if (Test-Path $dll) {
    $ReShade = $dll
  } else {
    # The ReShade setup is an executable with a zip archive appended: take the DLL out of it.
    $setup = Get-ChildItem -Path $here -Filter "ReShade_Setup_*Addon*.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($setup) {
      Add-Type -AssemblyName System.IO.Compression.FileSystem
      try {
        $zip = [System.IO.Compression.ZipFile]::OpenRead($setup.FullName)
        $entry = $zip.Entries | Where-Object { $_.Name -eq "ReShade64.dll" } | Select-Object -First 1
        if ($entry) { [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $dll, $true); $ReShade = $dll }
        $zip.Dispose()
      } catch { }
    }
  }
}
if (-not $ReShade -or -not (Test-Path $ReShade)) {
  Fail ("ReShade64.dll not found. Put ReShade64.dll (ReShade with full add-on support) or the " +
        "ReShade_Setup_..._Addon.exe installer from https://reshade.me next to this script.")
}

if (-not $Package) {
  $cand = @($here) + @(Get-ChildItem -Path $here -Directory | ForEach-Object { $_.FullName })
  foreach ($c in $cand) {
    if ((Test-Path (Join-Path $c "sopt-presets")) -and (Test-Path (Join-Path $c "reshade-shaders"))) { $Package = $c; break }
  }
}
if (-not $Package) { Fail "No test package found: extract the sopt-compare-*.zip next to this script." }
$presets = Get-ChildItem -Path (Join-Path $Package "sopt-presets") -Filter "sopt-*.ini" | Sort-Object Name
if ($presets.Count -eq 0) { Fail "The test package has no sopt-*.ini presets." }

# --- a run folder with its own ReShade.ini ----------------------------------------------
$stamp = Get-Date -Format "yyyy-MM-dd_HHmm"
$results = Join-Path $here "results-$stamp"
$shots = Join-Path $results "screenshots"
$run = Join-Path $here "run"
New-Item -ItemType Directory -Force -Path $results, $shots, $run | Out-Null
Copy-Item (Join-Path $here "sopt-host.exe") $run -Force
Copy-Item (Join-Path $here "sopt-timer.addon64") $run -Force
Copy-Item $ReShade (Join-Path $run "ReShade64.dll") -Force

$shaders = Join-Path $Package "reshade-shaders\Shaders"
$textures = Join-Path $Package "reshade-shaders\Textures"
$ini = @"
[GENERAL]
EffectSearchPaths=$shaders\**
TextureSearchPaths=$textures\**
PresetPath=$($presets[0].FullName)
NoReloadOnInit=0

[OVERLAY]
TutorialProgress=4
ShowFPS=0
ShowScreenshotMessage=0
ShowPresetTransitionMessage=0

[SCREENSHOT]
SavePath=$shots
FileFormat=1
FileNaming=%AppName%

[ADDON]
AddonPath=$run

[SOPT_TIMER]
AutoRun=1
ExitWhenDone=1
Screenshots=1
Frames=$Frames
"@
Set-Content -Path (Join-Path $run "ReShade.ini") -Value $ini -Encoding UTF8

# The Vulkan layer manifest, so that no ReShade installation is needed: the loader finds it
# through VK_ADD_LAYER_PATH and VK_INSTANCE_LAYERS enables it for sopt-host only.
$json = @"
{
  "file_format_version": "1.0.0",
  "layer": {
    "name": "VK_LAYER_reshade",
    "type": "GLOBAL",
    "library_path": ".\\ReShade64.dll",
    "api_version": "1.3.268",
    "implementation_version": "1",
    "description": "ReShade (sopt bench)"
  }
}
"@
Set-Content -Path (Join-Path $run "ReShade64.json") -Value $json -Encoding ASCII

# --- the runs ---------------------------------------------------------------------------
$summary = @()
foreach ($api in $Apis) {
  Write-Host ""
  Write-Host "=== $api : $($presets.Count) presets, $Frames frames each (the window closes by itself) ===" -ForegroundColor Cyan
  $csv = Join-Path $run "sopt-timer.csv"
  Remove-Item $csv -ErrorAction SilentlyContinue
  $dxgi = Join-Path $run "dxgi.dll"
  if ($api -eq "dx11") {
    Copy-Item (Join-Path $run "ReShade64.dll") $dxgi -Force
    Remove-Item Env:VK_INSTANCE_LAYERS -ErrorAction SilentlyContinue
  } else {
    Remove-Item $dxgi -ErrorAction SilentlyContinue
    $env:VK_ADD_LAYER_PATH = $run
    $env:VK_INSTANCE_LAYERS = "VK_LAYER_reshade"
  }
  $p = Start-Process -FilePath (Join-Path $run "sopt-host.exe") -WorkingDirectory $run -PassThru -Wait `
    -ArgumentList @("--api", $api, "--bench", "--width", $Width, "--height", $Height)
  Remove-Item $dxgi -ErrorAction SilentlyContinue
  Remove-Item Env:VK_INSTANCE_LAYERS -ErrorAction SilentlyContinue
  Remove-Item Env:VK_ADD_LAYER_PATH -ErrorAction SilentlyContinue
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
$sys = Get-CimInstance Win32_VideoController | Select-Object -ExpandProperty Name
$sys | Set-Content (Join-Path $results "gpu.txt")
$summary | Set-Content (Join-Path $results "summary.txt")
$zip = "$results.zip"
Compress-Archive -Path "$results\*" -DestinationPath $zip -Force
Write-Host ""
$summary | ForEach-Object { Write-Host $_ }
Write-Host ""
Write-Host "Done. Send this file: $zip" -ForegroundColor Green
