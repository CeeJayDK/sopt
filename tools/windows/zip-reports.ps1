# Zips the reports for GPU-Blueprint.bat and prints the zip's file name.
# Owner, 2026-10-05: the zip is named after the graphics cards in the reports, short ("less verbose": brand words
# left out), e.g. Reports-GTX-1660+UHD-630.zip, so uploads tell each other apart.
# Only the files directly in Reports\ go in (not Reports\Shaders\, not the hidden marker .sent); an older zip is
# replaced, so there is only ever one.
param([string]$Root = (Split-Path -Parent $PSScriptRoot))
$ErrorActionPreference = "Stop"
$reports = Join-Path $Root "Reports"
$files = @(Get-ChildItem -LiteralPath $reports -File -ErrorAction SilentlyContinue)
if ($files.Count -eq 0) { exit 1 }

# Card names from the report file names (opbench-<GPU>.csv, texbench-<GPU>[-order...].csv / .png,
# shaderinfo-<GPU>.txt), where the programs turned every character but letters and digits into '_'.
$gpus = @()
foreach ($f in $files) {
  if ($f.Name -match '^(?:opbench|texbench|shaderinfo)-(.+?)(?:-order(?:-zoom)?)?\.(?:csv|txt|png)$') {
    $words = @($Matches[1] -split '_+' | Where-Object { $_ -and $_ -notmatch '^(R|TM)$' })
    $short = @($words | Where-Object { $_ -notmatch '^(NVIDIA|GeForce|AMD|ATI|Radeon|Intel|Graphics|GPU)$' })
    if ($short.Count -eq 0) { $short = @($words | Where-Object { $_ -notmatch '^(Graphics|GPU)$' }) }  # "AMD Radeon(TM) Graphics"
    $g = $short -join '-'
    if ($g -and ($gpus -notcontains $g)) { $gpus += $g }
  }
}
$name = "Reports"
if ($gpus.Count -gt 0) { $name += "-" + ($gpus -join "+") }
if ($name.Length -gt 150) { $name = $name.Substring(0, 150) }
$zip = Join-Path $Root "$name.zip"

Get-ChildItem -LiteralPath $Root -Filter "Reports*.zip" -File -ErrorAction SilentlyContinue |
  Where-Object { $_.FullName -ne $zip } | Remove-Item -Force
Compress-Archive -LiteralPath $files.FullName -DestinationPath $zip -Force
"$name.zip"
