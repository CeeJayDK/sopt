# Test Host: runs one effect in sopt-host + ReShade on each graphics API, takes a screenshot per API and zips them
# with ReShade's logs (owner, 2026-10-06: one test host for every API ReShade supports). Started by Test-Host.bat.
#
#   run-test.ps1 [-Apis all|dx9,dx10,dx11,dx12,vulkan,gl] [-Effect sopt_IEEE754.fx | -Effect ?] [-Width 1920]
#                [-Height 1080] [-Frames 30] [-ReShade path\ReShade64.dll]
#   run-test.ps1 -Interactive -Apis dx11     (opens the host with every effect available; press Home for the overlay)
#
# Effects come from the Effects folder next to Test-Host.bat (subfolders too: a whole reshade-shaders folder can go
# there). A test enables every technique of the effect; the add-on (sopt-timer, shot mode) waits until ReShade has
# rendered them for -Frames frames and its banner is gone, saves a screenshot and closes the window.
param(
  [string[]]$Apis = @("all"),
  [string]$Effect = "sopt_IEEE754.fx",
  [switch]$Interactive,
  [int]$Width = 1920,
  [int]$Height = 1080,
  [int]$Frames = 30,
  [string]$ReShade = ""
)
. (Join-Path $PSScriptRoot "common.ps1")

$effects = Join-Path $Here "Effects"
if (-not (Test-Path $effects)) { Fail "The Effects folder is missing next to Test-Host.bat." }
$apiList = Resolve-Apis $Apis
$dll = Find-ReShade $ReShade
$run = New-RunFolder $dll
$shots = Join-Path $run "shots"
New-Item -ItemType Directory -Force -Path $shots | Out-Null

function Start-Host([string]$api, [int]$timeoutSeconds) {
  Enable-ReShade $run $api
  $p = Start-Process -FilePath (Join-Path $run "sopt-host.exe") -WorkingDirectory $run -PassThru `
    -ArgumentList @("--api", $api, "--width", $Width, "--height", $Height)
  $ok = $true
  if ($timeoutSeconds -gt 0) {
    if (-not $p.WaitForExit($timeoutSeconds * 1000)) {
      Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
      $ok = $false
    }
  } else {
    $p.WaitForExit()
  }
  Disable-ReShade $run
  return $ok
}

# --- interactive: one API, every effect available, nothing measured ------------------------
if ($Interactive) {
  $api = $apiList[0]
  $preset = Join-Path $run "MyPreset.ini"
  if (-not (Test-Path $preset)) { Set-Content -Path $preset -Value "" -Encoding ASCII }
  Write-ReShadeIni $run $effects $effects $preset $shots $false $null
  Remove-Item Env:SOPT_TIMER_SHOT -ErrorAction SilentlyContinue
  Write-Host ""
  Write-Host "Opening sopt-host on $($ApiNames[$api]). Press Home in its window for ReShade's overlay;" -ForegroundColor Cyan
  Write-Host "your effect choices are kept in run\MyPreset.ini. Close the window when done." -ForegroundColor Cyan
  Start-Host $api 0 | Out-Null
  exit 0
}

# --- a test: the effect on each API, one screenshot each --------------------------------------
$fxFiles = @(Get-ChildItem -Path $effects -Filter "*.fx" -Recurse | Sort-Object Name)
if ($Effect -eq "?") {
  if ($fxFiles.Count -eq 0) { Fail "There are no .fx files in the Effects folder." }
  Write-Host ""
  for ($i = 0; $i -lt $fxFiles.Count; ++$i) { Write-Host ("  {0,3}  {1}" -f ($i + 1), $fxFiles[$i].Name) }
  Write-Host ""
  $n = Read-Host "  Number of the effect to test (Enter = back)"
  if (-not $n) { exit 0 }
  $k = 0
  if (-not [int]::TryParse($n, [ref]$k) -or $k -lt 1 -or $k -gt $fxFiles.Count) { Fail "No effect number $n." }
  $fx = $fxFiles[$k - 1]
} else {
  $fx = $fxFiles | Where-Object { $_.Name -ieq $Effect } | Select-Object -First 1
  if (-not $fx) { Fail "$Effect is not in the Effects folder." }
}

# Its techniques, from the code (comments and strings removed).
$code = Get-Content -Raw $fx.FullName
$code = [regex]::Replace($code, '/\*.*?\*/', ' ', 'Singleline')
$code = [regex]::Replace($code, '//[^\r\n]*', ' ')
$code = [regex]::Replace($code, '"(\\.|[^"\\])*"', '""')
$techs = @([regex]::Matches($code, '(?m)^\s*technique\s+([A-Za-z_][A-Za-z0-9_]*)') | ForEach-Object { $_.Groups[1].Value })
if ($techs.Count -eq 0) { Fail "$($fx.Name) has no technique." }
$list = ($techs | ForEach-Object { "$_@$($fx.Name)" }) -join ","
$preset = Join-Path $run "test-preset.ini"
Set-Content -Path $preset -Value @("Techniques=$list", "TechniqueSorting=$list") -Encoding ASCII
Write-ReShadeIni $run $effects $effects $preset $shots $true $null

$stamp = Get-Date -Format "yyyy-MM-dd_HHmm"
$stem = [IO.Path]::GetFileNameWithoutExtension($fx.Name)
$results = Join-Path $Here "Results\$stem-$stamp"
New-Item -ItemType Directory -Force -Path $results | Out-Null
$env:SOPT_TIMER_SHOT = "$Frames"
$summary = @("$($fx.Name), $Width x $Height")
foreach ($api in $apiList) {
  Write-Host ""
  Write-Host "=== $($ApiNames[$api]): $($fx.Name) (the window closes by itself) ===" -ForegroundColor Cyan
  Get-ChildItem -Path $shots -File -ErrorAction SilentlyContinue | Remove-Item -Force
  $log = Join-Path $run "ReShade.log"
  Remove-Item $log -ErrorAction SilentlyContinue
  $finished = Start-Host $api 180
  $status = "OK"
  if (Test-Path $log) {
    Copy-Item $log (Join-Path $results "ReShade-$api.log") -Force
    $text = Get-Content -Raw $log
    if ($text -match "Failed to compile") { $status = "the effect did not compile (see ReShade-$api.log)" }
    elseif ($text -match "Skipped loading add-on") {
      $status = "another ReShade without full add-on support was loaded, so no automatic screenshot (see ReShade-$api.log)"
    }
  } else {
    $status = "ReShade did not start"
  }
  $shot = Get-ChildItem -Path $shots -Filter "*.png" -File -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($shot) {
    Move-Item $shot.FullName (Join-Path $results "$stem-$api.png") -Force
  } elseif ($status -eq "OK") {
    $status = "no screenshot"
  }
  if (-not $finished) { $status += " (the window did not close within 3 minutes)" }
  $line = "{0,-12} {1}" -f $ApiNames[$api], $status
  $summary += $line
  Write-Host $line -ForegroundColor $(if ($status -eq "OK") { "Green" } else { "Yellow" })
}
Remove-Item Env:SOPT_TIMER_SHOT -ErrorAction SilentlyContinue

Get-GpuInfo | Set-Content (Join-Path $results "gpu.txt")
$summary | Set-Content (Join-Path $results "summary.txt")
$zip = "$results.zip"
Compress-Archive -Path "$results\*" -DestinationPath $zip -Force
Write-Host ""
$summary | ForEach-Object { Write-Host $_ }
Write-Host ""
Write-Host "Screenshots and logs: Results\$stem-$stamp\ (zipped: Results\$stem-$stamp.zip)" -ForegroundColor Green
