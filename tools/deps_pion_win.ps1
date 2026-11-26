# Fetches pinned pure-Pion native dependencies for Windows x64 (MSVC ABI) into deps/:
#   - AFK-surf/invar (OpenType shaping, Skia/pixels/dmath raster, D3D11/DirectWrite/WIC/WebView2 host)
#   - Microsoft.Web.WebView2 1.0.4258.31 NuGet SDK (header + WebView2LoaderStatic.lib)
#   - lexiforest/curl-impersonate v2.3.2 x86_64-win32 (Chrome TLS fingerprint libcurl + BoringSSL)
#   - metacall/libnode v24.10.0 (libnode.dll + filtered V8/Node/UV import lib) + Node.js v24.10.0 headers & LICENSE
param(
  [switch]$Force
)
$ErrorActionPreference = 'Stop'

# Load pinned versions from tools/deps.env (shared with tools/deps_pion.sh and CI)
$depsEnv = @{}
$depsEnvPath = Join-Path $PSScriptRoot 'deps.env'
foreach ($line in Get-Content -Path $depsEnvPath) {
  $t = $line.Trim()
  if (-not $t -or $t.StartsWith('#')) { continue }
  $kv = $t -split '=', 2
  if ($kv.Length -eq 2) { $depsEnv[$kv[0].Trim()] = $kv[1].Trim() }
}

function Assert-Sha256([string]$Path, [string]$Expected) {
  $actual = (Get-FileHash -Path $Path -Algorithm SHA256).Hash.ToLowerInvariant()
  if ($actual -ne $Expected.ToLowerInvariant()) {
    throw "deps_pion_win: SHA-256 mismatch for ${Path}: got $actual, expected $Expected"
  }
}

function Download-File([string]$Url, [string]$OutFile) {
  $parent = Split-Path -Parent $OutFile
  if ($parent -and -not (Test-Path $parent)) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
  if (Get-Command curl.exe -ErrorAction SilentlyContinue) {
    & curl.exe -fSL --retry 3 --retry-delay 2 $Url -o $OutFile
    if ($LASTEXITCODE -eq 0) { return }
  }
  Invoke-WebRequest -Uri $Url -OutFile $OutFile -UseBasicParsing
}

$tmpRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("fv-deps-win-" + $PID)
New-Item -ItemType Directory -Force -Path $tmpRoot | Out-Null

try {
  # 1. AFK-surf/invar (pinned)
  $invarRev = $depsEnv['INVAR_REV']
  $invarDir = 'deps/invar'
  $invarReady = $false
  if (-not $Force -and (Test-Path "$invarDir/.git")) {
    $cur = (& git -C $invarDir rev-parse HEAD 2>$null)
    if ($LASTEXITCODE -eq 0 -and $cur.Trim() -eq $invarRev) {
      Write-Host "deps_pion_win: $invarDir already at $invarRev"
      $invarReady = $true
    }
  }
  if (-not $invarReady) {
    if (Test-Path $invarDir) { Remove-Item -Recurse -Force $invarDir }
    New-Item -ItemType Directory -Force -Path 'deps' | Out-Null
    & git clone -q -c core.autocrlf=false --depth 1 https://github.com/AFK-surf/invar.git $invarDir
    if ($LASTEXITCODE -ne 0) { throw 'git clone AFK-surf/invar failed' }
    $cur = (& git -C $invarDir rev-parse HEAD).Trim()
    if ($cur -ne $invarRev) {
      & git -C $invarDir fetch -q --depth 1 origin $invarRev
      if ($LASTEXITCODE -ne 0) { throw 'git fetch invar rev failed' }
      & git -C $invarDir -c core.autocrlf=false checkout -q $invarRev
      if ($LASTEXITCODE -ne 0) { throw 'git checkout invar rev failed' }
    }
    $actual = (& git -C $invarDir rev-parse HEAD).Trim()
    if ($actual -ne $invarRev) { throw "invar rev mismatch: got $actual, expected $invarRev" }
    Write-Host "deps_pion_win: $invarDir ready at $($actual.Substring(0, 8))"
  }

  # 2. Microsoft.Web.WebView2 NuGet SDK (required by invar's Windows host)
  $wv2Ver = $depsEnv['WEBVIEW2_VER']
  $wv2Sha = '56f7f4b8bf9aee4b8efefbbdd4f67d5f74ebd1b100ed0806da71bf76af481aa9'
  $wv2Dir = "deps/invar/packages/host/native/webview2/$wv2Ver"
  if (-not $Force -and (Test-Path "$wv2Dir/build/native/x64/WebView2LoaderStatic.lib") -and (Test-Path "$wv2Dir/build/native/include/WebView2.h")) {
    Write-Host "deps_pion_win: WebView2 SDK $wv2Ver already ready"
  } else {
    $wv2Zip = Join-Path $tmpRoot "webview2-$wv2Ver.zip"
    Download-File "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/$wv2Ver" $wv2Zip
    Assert-Sha256 $wv2Zip $wv2Sha
    if (Test-Path $wv2Dir) { Remove-Item -Recurse -Force $wv2Dir }
    New-Item -ItemType Directory -Force -Path $wv2Dir | Out-Null
    Expand-Archive -Path $wv2Zip -DestinationPath $wv2Dir -Force
    Write-Host "deps_pion_win: WebView2 SDK $wv2Ver ready (sha256 verified)"
  }

  # 3. lexiforest/curl-impersonate x86_64-win32
  $ciVer = $depsEnv['CURL_IMPERSONATE_VER']
  $ciArch = 'x86_64-win32'
  $ciSha = '4c416b5e50716199c4f6541b660fdada9a049cd0bdb72e92bdee4d4da27d39db'
  $ciStamp = "$ciVer-$ciArch-$ciSha"
  $ciDir = 'deps/curl-impersonate'
  $ciVersionFile = "$ciDir/.version"
  if (-not $Force -and (Test-Path "$ciDir/lib/libcurl-impersonate.lib") -and (Test-Path "$ciDir/LICENSE") -and (Test-Path $ciVersionFile) -and ((Get-Content $ciVersionFile -Raw).Trim() -eq $ciStamp)) {
    Write-Host "deps_pion_win: $ciDir already ready ($ciVer $ciArch)"
  } else {
    $ciTgz = Join-Path $tmpRoot "libcurl-impersonate-$ciVer.$ciArch.tar.gz"
    Download-File "https://github.com/lexiforest/curl-impersonate/releases/download/$ciVer/libcurl-impersonate-$ciVer.$ciArch.tar.gz" $ciTgz
    Assert-Sha256 $ciTgz $ciSha
    if (Test-Path $ciDir) { Remove-Item -Recurse -Force $ciDir }
    New-Item -ItemType Directory -Force -Path $ciDir | Out-Null
    & tar.exe -xzf $ciTgz -C $ciDir
    if ($LASTEXITCODE -ne 0) { throw 'tar extract curl-impersonate failed' }
    [System.IO.File]::WriteAllText((Resolve-Path $ciDir).Path + '/.version', "$ciStamp`n")
    Write-Host "deps_pion_win: $ciDir ready ($ciVer $ciArch, sha256 verified)"
  }

  # 4. Node.js 24 libnode (metacall/libnode) + official headers & LICENSE
  $nodeVer = $depsEnv['NODE_WIN_VER']
  $libnodeSha = '930a3b8e0ed433239e44b92ff5b8f05f64f33c932173fd53e26e38300f7a5e2a'
  $hdrSha = 'fde04b7be93bea3cf20348eaa90c4c375a7b3a303d4838a149eb5d506aca1a67'
  $nodeStamp = "$nodeVer-win-x64-$libnodeSha-$hdrSha-v8only"
  $nodeDir = 'deps/node'
  $nodeVersionFile = "$nodeDir/.version"
  if (-not $Force -and (Test-Path "$nodeDir/lib/libnode.dll") -and (Test-Path "$nodeDir/lib/libnode.lib") -and (Test-Path "$nodeDir/include/node/v8.h") -and (Test-Path "$nodeDir/LICENSE") -and (Test-Path $nodeVersionFile) -and ((Get-Content $nodeVersionFile -Raw).Trim() -eq $nodeStamp)) {
    Write-Host "deps_pion_win: $nodeDir already ready ($nodeVer)"
  } else {
    $lnZip = Join-Path $tmpRoot 'libnode-amd64-windows.zip'
    $hdrTgz = Join-Path $tmpRoot "node-$nodeVer-headers.tar.gz"
    Download-File "https://github.com/metacall/libnode/releases/download/$nodeVer/libnode-amd64-windows.zip" $lnZip
    Assert-Sha256 $lnZip $libnodeSha
    Download-File "https://nodejs.org/dist/$nodeVer/node-$nodeVer-headers.tar.gz" $hdrTgz
    Assert-Sha256 $hdrTgz $hdrSha

    if (Test-Path $nodeDir) { Remove-Item -Recurse -Force $nodeDir }
    New-Item -ItemType Directory -Force -Path "$nodeDir/lib", "$nodeDir/include" | Out-Null

    $lnExtract = Join-Path $tmpRoot 'ln_ext'
    Expand-Archive -Path $lnZip -DestinationPath $lnExtract -Force
    $dll = Get-ChildItem -Path $lnExtract -Recurse -Filter 'libnode.dll' | Select-Object -First 1
    $lib = Get-ChildItem -Path $lnExtract -Recurse -Filter 'libnode.lib' | Select-Object -First 1
    if (-not $dll -or -not $lib) { throw 'libnode.dll or libnode.lib missing in archive' }
    Copy-Item $dll.FullName "$nodeDir/lib/libnode.dll" -Force

    # Filter libnode.lib to export only V8 / Node / libuv symbols so libnode's bundled OpenSSL/zlib
    # exports never collide with curl-impersonate's static BoringSSL/zlib archives at link time.
    $origLib = Join-Path $tmpRoot 'libnode.orig.lib'
    Copy-Item $lib.FullName $origLib -Force
    $defPath = Join-Path $tmpRoot 'libnode.def'
    $bytes = [System.IO.File]::ReadAllBytes($origLib)
    $pos = 8
    $seen = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
    $defLines = [System.Collections.Generic.List[string]]::new()
    $defLines.Add('LIBRARY libnode.dll')
    $defLines.Add('EXPORTS')
    while ($pos -lt $bytes.Length) {
      if (($pos % 2) -ne 0) { $pos++ }
      if ($pos + 60 -gt $bytes.Length) { break }
      $sizeStr = [System.Text.Encoding]::ASCII.GetString($bytes, $pos + 48, 10).Trim()
      $sz = [int]$sizeStr
      $bodyOff = $pos + 60
      $pos = $bodyOff + $sz
      if ($sz -ge 20) {
        $sig1 = [BitConverter]::ToUInt16($bytes, $bodyOff)
        $sig2 = [BitConverter]::ToUInt16($bytes, $bodyOff + 2)
        if ($sig1 -eq 0 -and $sig2 -eq 0xFFFF) {
          $typ = ([BitConverter]::ToUInt16($bytes, $bodyOff + 18)) -band 3
          $nameEnd = $bodyOff + 20
          while ($nameEnd -lt ($bodyOff + $sz) -and $bytes[$nameEnd] -ne 0) { $nameEnd++ }
          $sym = [System.Text.Encoding]::ASCII.GetString($bytes, $bodyOff + 20, $nameEnd - ($bodyOff + 20))
          if ($sym.StartsWith('?') -or $sym.StartsWith('v8') -or $sym.StartsWith('node_') -or $sym.StartsWith('napi_') -or $sym.StartsWith('uv_')) {
            if ($seen.Add($sym)) {
              if ($typ -eq 1) { $defLines.Add("  $sym DATA") } else { $defLines.Add("  $sym") }
            }
          }
        }
      }
    }
    [System.IO.File]::WriteAllText($defPath, ($defLines -join "`n") + "`n")
    $outLibAbs = Join-Path (Resolve-Path "$nodeDir/lib").Path 'libnode.lib'
    $dlltool = Get-Command llvm-dlltool.exe -ErrorAction SilentlyContinue
    $msLib = Get-Command lib.exe -ErrorAction SilentlyContinue
    if ($dlltool) {
      & $dlltool.Source -m i386:x86-64 -d $defPath -l $outLibAbs
      if ($LASTEXITCODE -ne 0) { throw 'llvm-dlltool failed generating filtered libnode.lib' }
    } elseif ($msLib) {
      & $msLib.Source /nologo /machine:x64 "/def:$defPath" "/out:$outLibAbs"
      if ($LASTEXITCODE -ne 0) { throw 'lib.exe failed generating filtered libnode.lib' }
    } else {
      throw 'Neither llvm-dlltool.exe nor MSVC lib.exe found in PATH to build filtered libnode.lib'
    }

    $hdrExtract = Join-Path $tmpRoot 'hdr_ext'
    New-Item -ItemType Directory -Force -Path $hdrExtract | Out-Null
    & tar.exe -xzf $hdrTgz -C $hdrExtract
    if ($LASTEXITCODE -ne 0) { throw 'tar extract node headers failed' }
    $hdrTop = Get-ChildItem -Path $hdrExtract -Directory | Select-Object -First 1
    Copy-Item -Recurse -Force (Join-Path $hdrTop.FullName 'include/node') "$nodeDir/include/node"
    $licSrc = Join-Path $hdrTop.FullName 'LICENSE'
    if (Test-Path $licSrc) {
      Copy-Item $licSrc "$nodeDir/LICENSE" -Force
    } else {
      Download-File "https://raw.githubusercontent.com/nodejs/node/$nodeVer/LICENSE" "$nodeDir/LICENSE"
    }
    [System.IO.File]::WriteAllText((Resolve-Path $nodeDir).Path + '/.version', "$nodeStamp`n")
    Write-Host "deps_pion_win: $nodeDir ready ($nodeVer, $($seen.Count) V8/Node/UV exports, sha256 verified)"
  }
} finally {
  Remove-Item -Recurse -Force $tmpRoot -ErrorAction SilentlyContinue
}
