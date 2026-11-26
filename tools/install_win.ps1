# Builds and installs Farview (pion-host.exe, pion-app.exe, pion-agent.exe, Farview.exe, libnode.dll)
# into %LOCALAPPDATA%\Programs\Farview for local Windows testing.
#
# Usage (from an MSVC x64 developer environment with PIONC, CC, CXX set):
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools/install_win.ps1 [-InstallDir <dir>] [-Pull] [-Launch]
param(
  [string]$InstallDir = (Join-Path $env:LOCALAPPDATA 'Programs\Farview'),
  [switch]$Pull,
  [switch]$Launch
)
$ErrorActionPreference = 'Stop'

if ($Pull) {
  & git pull --ff-only
  if ($LASTEXITCODE -ne 0) { throw 'git pull failed' }
}

$pionc = if ($env:PIONC) { $env:PIONC } else { 'pionc' }
if (-not (Get-Command $pionc -ErrorAction SilentlyContinue)) {
  throw 'install_win: pionc not found (set PIONC=C:\path\to\pionc.exe)'
}
$cc = if ($env:CC) { $env:CC } else { 'clang' }
if (-not (Get-Command $cc -ErrorAction SilentlyContinue)) {
  throw 'install_win: C compiler not found (set CC=C:\path\to\clang.exe)'
}

& powershell -NoProfile -ExecutionPolicy Bypass -File tools/deps_pion_win.ps1
if ($LASTEXITCODE -ne 0) { throw 'deps_pion_win.ps1 failed' }

$env:PION_WIN_ABI = 'msvc'
if (-not (Test-Path 'out')) { New-Item -ItemType Directory -Force -Path 'out' | Out-Null }

foreach ($t in @('host', 'app', 'agent')) {
  Write-Host "install_win: building pion-$t.exe ..."
  & $pionc build -o "out/pion-$t.exe" "pion/cmd/$t"
  if ($LASTEXITCODE -ne 0) { throw "failed to build pion-$t.exe" }
}

Write-Host 'install_win: building Farview.exe launcher ...'
& $cc -O2 -D_CRT_SECURE_NO_WARNINGS tools/win_launcher.c -ladvapi32 -luser32 '-Wl,/subsystem:windows' -o out/Farview.exe
if ($LASTEXITCODE -ne 0) { throw 'failed to build Farview.exe launcher' }

New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
$running = Get-CimInstance Win32_Process | Where-Object {
  $_.ExecutablePath -and $_.ExecutablePath.StartsWith($InstallDir, [StringComparison]::OrdinalIgnoreCase)
}
foreach ($p in $running) {
  Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
}
if ($running) { Start-Sleep -Milliseconds 300 }

foreach ($f in @('out/pion-host.exe', 'out/pion-app.exe', 'out/pion-agent.exe', 'out/Farview.exe')) {
  Copy-Item -Force $f (Join-Path $InstallDir (Split-Path -Leaf $f))
}
Copy-Item -Force 'deps/node/lib/libnode.dll' (Join-Path $InstallDir 'libnode.dll')
Write-Host "install_win: installed Farview to $InstallDir"

if ($Launch) {
  & explorer.exe (Join-Path $InstallDir 'Farview.exe')
}
