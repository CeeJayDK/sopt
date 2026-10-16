# Prints "<largest reference drift, whole %>|<program>|<GPU>" for each OpBench and TexBench report in Reports\, for
# GPU-Blueprint.bat's send screen (owner, 2026-10-11: tell people their drift before they send).
param([string]$Root = (Split-Path -Parent $PSScriptRoot))
$ErrorActionPreference = "SilentlyContinue"
foreach ($f in Get-ChildItem -LiteralPath (Join-Path $Root "Reports") -File | Where-Object { $_.Name -match '^(opbench|texbench|stagebench-(ps|vs))-.+\.csv$' }) {
  $program = if ($f.Name -like "opbench-*") { "OpBench" } elseif ($f.Name -like "texbench-*") { "TexBench" } else { "StageBench-" + $f.Name.Substring(11, 2) }
  $gpu = $f.BaseName
  $drift = -1.0
  foreach ($line in Get-Content -LiteralPath $f.FullName -TotalCount 40) {
    if ($line -match '^# gpu: (.+)$') { $gpu = $Matches[1].Trim() }
    if ($line -match '^# reference drift[\w ]*: ([\d.]+)%') { $drift = [math]::Max($drift, [double]$Matches[1]) }
  }
  if ($drift -ge 0) { "{0}|{1}|{2}" -f [int][math]::Round($drift), $program, $gpu }
}
