# Builds a self-contained Windows x64 desktop bundle of Farview:
#   dist/Farview-<version>-windows-x64.zip
# containing Farview.exe (GUI launcher with embedded AppIcon.ico), pion-host.exe,
# pion-app.exe, pion-agent.exe, libnode.dll, and full license/third-party notices.
#
# Usage (from an MSVC x64 developer environment with PIONC, CC set):
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools/package_win_app.ps1 [-Version 0.1.0] [-PrebuiltDir out]
param(
  [string]$Version = '0.1.0',
  [string]$PrebuiltDir = $env:FV_PREBUILT_DIR,
  [string]$IconPng = 'assets/icon.png',
  [ValidateSet('Optimal', 'Fastest', 'NoCompression')]
  [string]$CompressionLevel = 'Optimal'
)
$ErrorActionPreference = 'Stop'

$cleanVer = $Version.TrimStart('v')
if (-not $cleanVer) { $cleanVer = '0.1.0' }
if ($cleanVer -notmatch '^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$') {
  throw "package_win_app: invalid version string '$cleanVer'"
}

$pionc = if ($env:PIONC) { $env:PIONC } else { 'pionc' }
$cc = if ($env:CC) { $env:CC } else { 'clang' }
if (-not (Get-Command $cc -ErrorAction SilentlyContinue)) {
  throw 'package_win_app: C compiler not found (set CC=C:\path\to\clang.exe)'
}
if (-not (Test-Path 'deps/node/lib/libnode.dll')) {
  throw 'package_win_app: deps/node/lib/libnode.dll missing — run tools/deps_pion_win.ps1 first'
}

$env:PION_WIN_ABI = 'msvc'

$outDir = 'out'
if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Force -Path $outDir | Out-Null }

$pkgName = "Farview-$cleanVer-windows-x64"
$dest = Join-Path 'dist' $pkgName
if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
New-Item -ItemType Directory -Force -Path $dest | Out-Null

foreach ($t in @('host', 'app', 'agent')) {
  $srcBin = if ($PrebuiltDir) { Join-Path $PrebuiltDir "pion-$t.exe" } else { '' }
  if ($srcBin -and (Test-Path $srcBin)) {
    Copy-Item $srcBin (Join-Path $dest "pion-$t.exe") -Force
  } else {
    if (-not (Get-Command $pionc -ErrorAction SilentlyContinue)) {
      throw 'package_win_app: pionc not found (set PIONC or -PrebuiltDir)'
    }
    & $pionc build -o (Join-Path $dest "pion-$t.exe") "pion/cmd/$t"
    if ($LASTEXITCODE -ne 0) { throw "failed to build pion-$t.exe" }
  }
}

# Build Farview.exe GUI launcher with embedded icon and version info
$resObj = $null
if (Test-Path $IconPng) {
  $icoPath = Join-Path $outDir 'AppIcon.ico'
  & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'make_win_icon.ps1') -InputPng $IconPng -OutputIco $icoPath
  if ($LASTEXITCODE -ne 0) { throw 'make_win_icon.ps1 failed' }

  $verParts = ($cleanVer -replace '[^0-9.].*$', '').Split('.')
  $v1 = if ($verParts.Length -ge 1 -and $verParts[0] -match '^\d+$') { [int]$verParts[0] } else { 0 }
  $v2 = if ($verParts.Length -ge 2 -and $verParts[1] -match '^\d+$') { [int]$verParts[1] } else { 1 }
  $v3 = if ($verParts.Length -ge 3 -and $verParts[2] -match '^\d+$') { [int]$verParts[2] } else { 0 }

  $rcPath = Join-Path $outDir 'farview.rc'
  $resPath = Join-Path $outDir 'farview.res'
  $icoAbs = (Resolve-Path $icoPath).Path -replace '\\', '/'
  @"
1 ICON "$icoAbs"
1 VERSIONINFO
FILEVERSION $v1,$v2,$v3,0
PRODUCTVERSION $v1,$v2,$v3,0
FILEFLAGSMASK 0x3fL
FILEFLAGS 0x0L
FILEOS 0x40004L
FILETYPE 0x1L
FILESUBTYPE 0x0L
BEGIN
  BLOCK "StringFileInfo"
  BEGIN
    BLOCK "040904b0"
    BEGIN
      VALUE "CompanyName", "AFK"
      VALUE "FileDescription", "Farview Browser"
      VALUE "FileVersion", "$cleanVer"
      VALUE "InternalName", "Farview"
      VALUE "OriginalFilename", "Farview.exe"
      VALUE "ProductName", "Farview"
      VALUE "ProductVersion", "$cleanVer"
    END
  END
  BLOCK "VarFileInfo"
  BEGIN
    VALUE "Translation", 0x409, 1200
  END
END
"@ | Set-Content -Encoding ascii $rcPath

  $llvmRc = Get-Command llvm-rc.exe -ErrorAction SilentlyContinue
  $msRc = Get-Command rc.exe -ErrorAction SilentlyContinue
  if ($llvmRc) {
    & $llvmRc.Source /fo $resPath $rcPath
    if ($LASTEXITCODE -ne 0) { throw 'llvm-rc failed compiling farview.rc' }
    $resObj = $resPath
  } elseif ($msRc) {
    & $msRc.Source /nologo /fo $resPath $rcPath
    if ($LASTEXITCODE -ne 0) { throw 'rc.exe failed compiling farview.rc' }
    $resObj = $resPath
  } else {
    throw 'Neither llvm-rc.exe nor rc.exe found to compile Windows icon resource'
  }
}

$launcherOut = Join-Path $dest 'Farview.exe'
$ccArgs = @('-O2', '-D_CRT_SECURE_NO_WARNINGS', 'tools/win_launcher.c')
if ($resObj) { $ccArgs += $resObj }
$ccArgs += @('-ladvapi32', '-luser32', '-Wl,/subsystem:windows', '-o', $launcherOut)
& $cc @ccArgs
if ($LASTEXITCODE -ne 0) { throw 'failed to build Farview.exe launcher' }

Copy-Item 'deps/node/lib/libnode.dll' (Join-Path $dest 'libnode.dll') -Force

# Copy license & third-party notice files (matching tools/package_pion_win.ps1)
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
  if (-not (Test-Path $req)) { throw "package_win_app: required license asset missing: $req" }
}
Copy-Item 'deps/node/LICENSE' "$dest/LICENSES/Node.js-LICENSE.txt" -Force
Copy-Item 'deps/invar/NOTICE' "$dest/LICENSES/invar-NOTICE.txt" -Force
Copy-Item 'deps/invar/THIRD_PARTY_NOTICES.md' "$dest/LICENSES/invar-THIRD_PARTY_NOTICES.md" -Force
Copy-Item -Recurse -Force 'deps/invar/LICENSES' "$dest/LICENSES/invar"
New-Item -ItemType Directory -Force -Path "$dest/LICENSES/curl-impersonate" | Out-Null
Copy-Item 'deps/curl-impersonate/LICENSE*' "$dest/LICENSES/curl-impersonate/" -Force

$zipPath = Join-Path 'dist' "$pkgName.zip"
if (Test-Path $zipPath) { Remove-Item -Force $zipPath }
Compress-Archive -Path $dest -DestinationPath $zipPath -CompressionLevel $CompressionLevel
$mb = [Math]::Round((Get-Item $zipPath).Length / 1MB, 1)
Write-Host "package_win_app: $zipPath (${mb} MB)"
