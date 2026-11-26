# Pure-Pion self-contained Windows x64 distribution bundler: builds pion/cmd/host,
# pion/cmd/agent, and pion/cmd/app with pionc (MSVC ABI) and bundles libnode.dll plus
# complete license and third-party notice files into:
#   dist/farview-pion-windows-x64.zip  (bin/host.exe, bin/agent.exe, bin/app.exe, bin/libnode.dll, LICENSE, THIRD_PARTY_NOTICES.md, LICENSES/)
param(
  [switch]$NoApp
)
$ErrorActionPreference = 'Stop'

$pionc = if ($env:PIONC) { $env:PIONC } else { 'pionc' }
if (-not (Get-Command $pionc -ErrorAction SilentlyContinue)) {
  throw 'package_pion_win: pionc not found (set PIONC=C:\path\to\pionc.exe)'
}
if (-not (Test-Path 'deps/invar/packages/shape')) {
  throw 'package_pion_win: deps/invar missing — run powershell -File tools/deps_pion_win.ps1 first'
}
if (-not (Test-Path 'deps/node/lib/libnode.dll')) {
  throw 'package_pion_win: deps/node/lib/libnode.dll missing — run powershell -File tools/deps_pion_win.ps1 first'
}

$env:PION_WIN_ABI = 'msvc'

$pkgName = 'farview-pion-windows-x64'
$dest = Join-Path 'dist' $pkgName
if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
New-Item -ItemType Directory -Force -Path "$dest/bin" | Out-Null

& $pionc build -o "$dest/bin/host.exe" pion/cmd/host
if ($LASTEXITCODE -ne 0) { throw 'failed to build host.exe' }
& $pionc build -o "$dest/bin/agent.exe" pion/cmd/agent
if ($LASTEXITCODE -ne 0) { throw 'failed to build agent.exe' }
if (-not $NoApp) {
  & $pionc build -o "$dest/bin/app.exe" pion/cmd/app
  if ($LASTEXITCODE -ne 0) { throw 'failed to build app.exe' }
}

Copy-Item 'deps/node/lib/libnode.dll' "$dest/bin/libnode.dll" -Force

# Copy license & third-party notice files (matching tools/package_pion.sh)
Copy-Item 'LICENSE' "$dest/LICENSE" -Force
Copy-Item 'THIRD_PARTY_NOTICES.md' "$dest/THIRD_PARTY_NOTICES.md" -Force
Copy-Item -Recurse -Force 'LICENSES' "$dest/LICENSES"

foreach ($req in @(
  'deps/node/LICENSE',
  'deps/invar/NOTICE',
  'deps/invar/THIRD_PARTY_NOTICES.md',
  'deps/invar/LICENSES',
  'deps/curl-impersonate/LICENSE'
)) {
  if (-not (Test-Path $req)) { throw "package_pion_win: required license asset missing: $req" }
}
Copy-Item 'deps/node/LICENSE' "$dest/LICENSES/Node.js-LICENSE.txt" -Force
Copy-Item 'deps/invar/NOTICE' "$dest/LICENSES/invar-NOTICE.txt" -Force
Copy-Item 'deps/invar/THIRD_PARTY_NOTICES.md' "$dest/LICENSES/invar-THIRD_PARTY_NOTICES.md" -Force
Copy-Item -Recurse -Force 'deps/invar/LICENSES' "$dest/LICENSES/invar"
New-Item -ItemType Directory -Force -Path "$dest/LICENSES/curl-impersonate" | Out-Null
Copy-Item 'deps/curl-impersonate/LICENSE*' "$dest/LICENSES/curl-impersonate/" -Force

$zipPath = Join-Path 'dist' "$pkgName.zip"
if (Test-Path $zipPath) { Remove-Item -Force $zipPath }
Compress-Archive -Path $dest -DestinationPath $zipPath -CompressionLevel Optimal
$mb = [Math]::Round((Get-Item $zipPath).Length / 1MB, 1)
Write-Host "package_pion_win: $zipPath (${mb} MB)"
