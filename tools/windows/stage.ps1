# Stages the three Windows downloads, used by CI (artifacts) and release.yml (zips), so both have the same layout.
# Owner, 2026-10-05: the main folder holds only the launcher and the guide (README.html); the programs go in bin\,
# the other pages, the stylesheet and the licenses in Docs\.
#   SweetOpt\       SweetOpt.bat, README.html; bin\ sopt.exe, sopt-fx.exe; Docs\ style.css, LICENSE.txt
#   GPU-Blueprint\  GPU-Blueprint.bat, README.html; bin\ OpBench.exe, TexBench.exe, ShaderInfo.exe, zip-reports.ps1;
#                   Docs\ OpBench.html, TexBench.html, ShaderInfo.html, style.css, LICENSE.txt
#   Test-Host\      Test-Host.bat, README.html; bin\ sopt-host.exe, sopt-timer.addon64 / .addon32, sopt-fxc.exe,
#                   common.ps1, run-test.ps1, run-bench.ps1, timings.py, ReShade64.dll; Effects\ the test effects
#                   (sopt_IEEE754.fx, sopt_MipTest.fx); Docs\ style.css, LICENSE.txt, ReShade-LICENSE.md, IEEE754.md
param(
  [string]$Out = "stage",
  [string]$Build = "build/Release",
  [string]$Build32 = "build32/Release",
  [string]$ReShadeBin = "reshade-bin"
)
$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "../..")
$win = Join-Path $root "tools/windows"

function Stage($name, $launcher, $readme, $bins, $docs) {
  $dir = Join-Path $Out $name
  New-Item -ItemType Directory -Force -Path (Join-Path $dir "bin"), (Join-Path $dir "Docs") | Out-Null
  Copy-Item $launcher $dir
  Copy-Item $readme (Join-Path $dir "README.html")
  Copy-Item $bins (Join-Path $dir "bin")
  Copy-Item $docs (Join-Path $dir "Docs")
  Copy-Item (Join-Path $root "LICENSE") (Join-Path $dir "Docs/LICENSE.txt")
}

Stage "SweetOpt" (Join-Path $win "SweetOpt.bat") (Join-Path $root "docs/sweetopt/README.html") `
  @("$Build/sopt.exe", "$Build/sopt-fx.exe") `
  @((Join-Path $win "docs/style.css"))

Stage "GPU-Blueprint" (Join-Path $win "GPU-Blueprint.bat") (Join-Path $win "docs/README.html") `
  @("$Build/OpBench.exe", "$Build/TexBench.exe", "$Build/ShaderInfo.exe", (Join-Path $win "zip-reports.ps1")) `
  @((Join-Path $win "docs/OpBench.html"), (Join-Path $win "docs/TexBench.html"), (Join-Path $win "docs/ShaderInfo.html"), (Join-Path $win "docs/style.css"))

# Test Host (owner, 2026-10-06: one test host for every API ReShade supports, a menu, test effects in Effects\).
Stage "Test-Host" (Join-Path $win "Test-Host.bat") (Join-Path $win "docs/TestHost.html") `
  @("$Build/sopt-host.exe", "$Build/sopt-timer.addon64", "$Build32/sopt-timer.addon32", "$Build/sopt-fxc.exe", (Join-Path $win "common.ps1"), (Join-Path $win "run-test.ps1"), (Join-Path $win "run-bench.ps1"), (Join-Path $win "timer/timings.py"), "$ReShadeBin/ReShade64.dll") `
  @((Join-Path $win "docs/style.css"), "$ReShadeBin/ReShade-LICENSE.md", (Join-Path $root "tools/reshade/IEEE754.md"))
$fx = Join-Path $Out "Test-Host/Effects"
New-Item -ItemType Directory -Force -Path $fx | Out-Null
Copy-Item (Join-Path $root "tools/reshade/sopt_IEEE754.fx"), (Join-Path $root "tools/reshade/sopt_MipTest.fx") $fx

Get-ChildItem -Recurse $Out | Where-Object { -not $_.PSIsContainer } | ForEach-Object { $_.FullName.Substring((Resolve-Path $Out).Path.Length + 1) }
