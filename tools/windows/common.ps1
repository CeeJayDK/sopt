# Shared by run-test.ps1 and run-bench.ps1 (dot-sourced): where things are, ReShade, the run folder, and how ReShade
# is put in front of each graphics API. Windows PowerShell 5.1 or later.
$ErrorActionPreference = "Stop"

# The download keeps the programs in bin\ next to the launcher (owner, 2026-10-05: only the menu and the guide in the
# main folder); results, the run folder, the Effects folder and a ReShade setup go in the main folder.
$script:Bin = $PSScriptRoot
$script:Here = $Bin
if ((Split-Path -Leaf $Bin) -ieq "bin") { $script:Here = Split-Path -Parent $Bin }

# The APIs sopt-host runs, in menu order.
$script:ApiNames = [ordered]@{
  dx9 = "Direct3D 9"; dx10 = "Direct3D 10"; dx11 = "Direct3D 11"; dx12 = "Direct3D 12"; vulkan = "Vulkan"; gl = "OpenGL"
}

function Fail($msg) {
  Write-Host ""
  Write-Host "ERROR: $msg" -ForegroundColor Red
  exit 1
}

# "all", "dx9,dx11" (one argument from a batch file) or several arguments -> a list of known API names.
function Resolve-Apis([string[]]$apis) {
  $list = @()
  foreach ($a in $apis) { $list += $a -split "[,; ]+" | Where-Object { $_ } }
  if ($list -contains "all") { return @($ApiNames.Keys) }
  foreach ($a in $list) { if (-not $ApiNames.Contains($a.ToLower())) { Fail "Unknown API '$a' (use $($ApiNames.Keys -join ', ') or all)." } }
  return @($list | ForEach-Object { $_.ToLower() })
}

# ReShade64.dll with full add-on support: -ReShade, a ReShade64.dll in the main folder or in bin\, or taken out of a
# ReShade_Setup_*_Addon.exe in the main folder (the setup is an executable with a zip archive appended).
function Find-ReShade([string]$given) {
  if ($given) {
    if (Test-Path $given) { return (Resolve-Path $given).Path }
    Fail "ReShade DLL '$given' not found."
  }
  foreach ($c in @((Join-Path $Here "ReShade64.dll"), (Join-Path $Bin "ReShade64.dll"))) { if (Test-Path $c) { return $c } }
  $setup = Get-ChildItem -Path $Here -Filter "ReShade_Setup_*Addon*.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($setup) {
    $dll = Join-Path $Bin "ReShade64.dll"
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    try {
      $zip = [System.IO.Compression.ZipFile]::OpenRead($setup.FullName)
      $entry = $zip.Entries | Where-Object { $_.Name -eq "ReShade64.dll" } | Select-Object -First 1
      if ($entry) { [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $dll, $true) }
      $zip.Dispose()
      if (Test-Path $dll) { return $dll }
    } catch { }
  }
  Fail ("ReShade64.dll not found. Put ReShade64.dll (ReShade with full add-on support) or the " +
        "ReShade_Setup_..._Addon.exe installer from https://reshade.me in this folder.")
}

# The run folder (run\ in the main folder): sopt-host, the sopt-timer add-on, ReShade and its Vulkan layer manifest.
# ReShade.ini and ReShade.log live there too, so no ReShade installation and no game is needed.
function New-RunFolder([string]$reshade) {
  foreach ($f in @("sopt-host.exe", "sopt-timer.addon64")) {
    if (-not (Test-Path (Join-Path $Bin $f))) { Fail "$f is missing from $Bin." }
  }
  $run = Join-Path $Here "run"
  New-Item -ItemType Directory -Force -Path $run | Out-Null
  Copy-Item (Join-Path $Bin "sopt-host.exe") $run -Force
  Copy-Item (Join-Path $Bin "sopt-timer.addon64") $run -Force
  Copy-Item $reshade (Join-Path $run "ReShade64.dll") -Force
  # The Vulkan layer manifest: the loader finds it through VK_ADD_LAYER_PATH, VK_INSTANCE_LAYERS enables it for
  # sopt-host only. Its own name: an installed ReShade registers an implicit layer called VK_LAYER_reshade (in
  # C:\ProgramData\ReShade, often a build with limited add-on support), and with the same name the loader used that
  # one instead (owner's GTX 1660 run, 2026-10-06: sopt-timer skipped, the window never closed).
  $json = @"
{
  "file_format_version": "1.0.0",
  "layer": {
    "name": "VK_LAYER_sopt_reshade",
    "type": "GLOBAL",
    "library_path": ".\\ReShade64.dll",
    "api_version": "1.3.268",
    "implementation_version": "1",
    "description": "ReShade (test host)"
  }
}
"@
  Set-Content -Path (Join-Path $run "ReShade64.json") -Value $json -Encoding ASCII
  Disable-ReShade $run
  return $run
}

# ReShade in front of one API: d3d9.dll (Direct3D 9), dxgi.dll (Direct3D 10 / 11 / 12), opengl32.dll (OpenGL) next
# to sopt-host, or the Vulkan layer for this process only.
function Enable-ReShade([string]$run, [string]$api) {
  Disable-ReShade $run
  $dll = Join-Path $run "ReShade64.dll"
  switch ($api) {
    "dx9" { Copy-Item $dll (Join-Path $run "d3d9.dll") -Force }
    "gl" { Copy-Item $dll (Join-Path $run "opengl32.dll") -Force }
    "vulkan" {
      $env:VK_ADD_LAYER_PATH = $run
      $env:VK_INSTANCE_LAYERS = "VK_LAYER_sopt_reshade"
      # Keeps an installed ReShade's implicit layer out (its manifest's disable_environment), so ReShade runs once.
      $env:DISABLE_VK_LAYER_reshade_1 = "1"
    }
    default { Copy-Item $dll (Join-Path $run "dxgi.dll") -Force }
  }
}

function Disable-ReShade([string]$run) {
  foreach ($n in @("d3d9.dll", "dxgi.dll", "opengl32.dll")) { Remove-Item (Join-Path $run $n) -ErrorAction SilentlyContinue }
  Remove-Item Env:VK_INSTANCE_LAYERS -ErrorAction SilentlyContinue
  Remove-Item Env:VK_ADD_LAYER_PATH -ErrorAction SilentlyContinue
  Remove-Item Env:DISABLE_VK_LAYER_reshade_1 -ErrorAction SilentlyContinue
}

# ReShade.ini for a run. $sections: extra sections as a hashtable of name -> ordered hashtable of key -> value.
function Write-ReShadeIni([string]$run, [string]$effects, [string]$textures, [string]$preset, [string]$shots,
                          [bool]$skipDisabled, $sections) {
  $lines = @(
    "[GENERAL]",
    "EffectSearchPaths=$effects\**",
    "TextureSearchPaths=$textures\**",
    "PresetPath=$preset",
    "NoReloadOnInit=0",
    "SkipLoadingDisabledEffects=$([int]$skipDisabled)",
    "",
    "[OVERLAY]",
    "TutorialProgress=4",
    "ShowFPS=0",
    "ShowClock=0",
    "ShowScreenshotMessage=0",
    "ShowPresetTransitionMessage=0",
    "",
    "[SCREENSHOT]",
    "SavePath=$shots",
    "FileFormat=1",
    "FileNaming=%AppName%",
    "",
    "[ADDON]",
    "AddonPath=$run"
  )
  if ($sections) {
    foreach ($name in $sections.Keys) {
      $lines += ""
      $lines += "[$name]"
      foreach ($k in $sections[$name].Keys) { $lines += "$k=$($sections[$name][$k])" }
    }
  }
  Set-Content -Path (Join-Path $run "ReShade.ini") -Value $lines -Encoding UTF8
}

# The graphics cards and drivers (only hardware, no user data).
function Get-GpuInfo {
  try { Get-CimInstance Win32_VideoController | ForEach-Object { "$($_.Name), driver $($_.DriverVersion)" } }
  catch { "unknown" }
}
