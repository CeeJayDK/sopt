# Prints "<largest reference drift, whole %>|<GPU>" for each OpBench report in Reports\, for GPU-Blueprint.bat's send
# screen (owner, 2026-10-11: tell people their drift before they send; reports with 150%+ drift were unusable).
param([string]$Root = (Split-Path -Parent $PSScriptRoot))
$ErrorActionPreference = "SilentlyContinue"
foreach ($f in Get-ChildItem -LiteralPath (Join-Path $Root "Reports") -Filter "opbench-*.csv" -File) {
  $gpu = $f.BaseName.Substring(8)
  $drift = -1.0
  foreach ($line in Get-Content -LiteralPath $f.FullName -TotalCount 40) {
    if ($line -match '^# gpu: (.+)$') { $gpu = $Matches[1].Trim() }
    if ($line -match '^# reference drift \w+: ([\d.]+)%') { $drift = [math]::Max($drift, [double]$Matches[1]) }
  }
  if ($drift -ge 0) { "{0}|{1}" -f [int][math]::Round($drift), $gpu }
}
