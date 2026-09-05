# RenderDoc の Python モジュール(renderdoc.pyd)を、このPC用にビルドして配置する。
#
# 【なぜ要るのか】配布版のRenderDocには renderdoc.pyd が入っていない
# (qrenderdoc.exe に内蔵されているだけ)。これが無いとGUI無しでキャプチャを読めない。
# 根拠と引っかかった点は docs/ImplementationDetail.md 63.10。
#
# 【なぜバイナリをリポジトリに入れないのか】できあがる .pyd は
# 「このPCのPythonのバージョン」と「このPCに入っているRenderDocのバージョン」の
# 両方に縛られる。commitしても他のPCでは動かず、Git履歴に残るだけになる。
# **代わりにこのスクリプトを置く。** 別PCではこれを走らせれば同じ状態になる。
#
# 使い方:
#     powershell -NoProfile -ExecutionPolicy Bypass -File Tools\renderdoc_setup.ps1
#     powershell ... -File Tools\renderdoc_setup.ps1 -KeepSource   # ソースを消さない
#
# 出力先は %LOCALAPPDATA%\KurenaiEngine\renderdoc\renderdoc.pyd(-Destination で変えられる)。
# **ユーザー名を含む絶対パスをどこにも書かない**ため、環境変数から組み立てている。

[CmdletBinding()]
param(
    # renderdoc.pyd の置き場所。既定は %LOCALAPPDATA% の下(PCが変わっても同じ式で決まる)
    [string]$Destination = (Join-Path $env:LOCALAPPDATA "KurenaiEngine\renderdoc"),
    # ソースとビルドの作業場所。既定は %TEMP% の下
    [string]$WorkDir = (Join-Path $env:TEMP "renderdoc-build"),
    # ビルド後にソース(数GB)を消さない
    [switch]$KeepSource
)

$ErrorActionPreference = "Stop"

function Write-Step($text) { Write-Host "==> $text" -ForegroundColor Cyan }
function Fail($text) { throw $text }

# --- 1. インストール済みの RenderDoc を見つけ、バージョンを読む -----------------------
# **ソースのタグはインストール版と一致させること。**
# .pyd は renderdoc.dll のABIに依存するので、違うバージョンで作ると読み込みで落ちる
Write-Step "インストール済みの RenderDoc を探す"
$rdDir = $null
foreach ($d in @("$env:ProgramFiles\RenderDoc", "${env:ProgramFiles(x86)}\RenderDoc")) {
    if (Test-Path (Join-Path $d "renderdoc.dll")) { $rdDir = $d; break }
}
if (-not $rdDir) { Fail "RenderDoc が見つかりません。https://renderdoc.org からインストールしてください" }

$rdVersion = (Get-Item (Join-Path $rdDir "qrenderdoc.exe")).VersionInfo.FileVersion
$tag = "v" + (($rdVersion -split '\.')[0..1] -join '.')
Write-Host "    $rdDir  version=$rdVersion  -> タグ $tag"

# --- 2. 使える Python を探す -------------------------------------------------------
# python.props が要求するのは3つ: include\Python.h / pythonXY.zip / libs\pythonXY.lib。
# **pythonXY.zip はインストーラ版に入っていない**ので下で Lib\ から作る。
# ストア版やembeddable版は include/libs を持たないため対象外
Write-Step "ビルドに使える Python を探す"
$pyCandidates = @()
foreach ($line in (& py -0p 2>$null)) {
    if ($line -match '^\s*-V:(\d+)\.(\d+)') {
        $major = [int]$Matches[1]; $minor = [int]$Matches[2]
        $exe = ($line -split '\s+')[-1]
        if (Test-Path $exe) {
            $prefix = Split-Path $exe -Parent
            $tagXY = "$major$minor"
            if ((Test-Path (Join-Path $prefix "include\Python.h")) -and
                (Test-Path (Join-Path $prefix "libs\python$tagXY.lib"))) {
                $pyCandidates += [pscustomobject]@{ Exe = $exe; Prefix = $prefix; XY = $tagXY; Minor = $minor }
            }
        }
    }
}
if ($pyCandidates.Count -eq 0) {
    Fail @"
include\Python.h と libs\pythonXY.lib を持つ Python が見つかりません。
python.org のインストーラ版が要ります(ストア版・embeddable版は開発用ファイルを持たない)。
インストール時に "Download debug binaries"/"Include development files" を有効にしてください。
"@
}
# python.props が対応するのは 3.4〜3.18。複数あれば古いほうが安全(新しすぎるとSWIGの生成が通らない)
$py = $pyCandidates | Sort-Object Minor | Select-Object -First 1
Write-Host "    $($py.Exe)  (python$($py.XY))"

# --- 3. Python の上書き用プレフィクスを作る -----------------------------------------
Write-Step "Python の上書き用プレフィクスを用意する"
$prefix = Join-Path $WorkDir "py$($py.XY)-prefix"
New-Item -ItemType Directory $prefix -Force | Out-Null
if (-not (Test-Path (Join-Path $prefix "include\Python.h"))) {
    Copy-Item (Join-Path $py.Prefix "include") (Join-Path $prefix "include") -Recurse -Force
}
New-Item -ItemType Directory (Join-Path $prefix "libs") -Force | Out-Null
Copy-Item (Join-Path $py.Prefix "libs\python$($py.XY).lib") (Join-Path $prefix "libs") -Force
Copy-Item (Join-Path $py.Prefix "python$($py.XY).dll") $prefix -Force -ErrorAction SilentlyContinue
if (-not (Test-Path (Join-Path $prefix "DLLs"))) {
    Copy-Item (Join-Path $py.Prefix "DLLs") (Join-Path $prefix "DLLs") -Recurse -Force -ErrorAction SilentlyContinue
}

$zip = Join-Path $prefix "python$($py.XY).zip"
if (-not (Test-Path $zip)) {
    # 【インストーラ版には pythonXY.zip が無い】Lib\ に非圧縮で置かれている。
    # python.props はこの .zip の存在を「有効なPython構成である」印として使うので、自分で作る
    $stage = Join-Path $WorkDir "_libstage"
    if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
    Copy-Item (Join-Path $py.Prefix "Lib") $stage -Recurse -Force
    foreach ($x in @("test", "site-packages", "idlelib", "lib2to3\tests")) {
        Remove-Item (Join-Path $stage $x) -Recurse -Force -ErrorAction SilentlyContinue
    }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [System.IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip)
    Remove-Item $stage -Recurse -Force
}
Write-Host "    $prefix"

# --- 4. ソースを取る ---------------------------------------------------------------
Write-Step "RenderDoc $tag のソースを取る"
$src = Join-Path $WorkDir "renderdoc-$tag"
if (Test-Path (Join-Path $src "renderdoc.sln")) {
    Write-Host "    既にある: $src"
} else {
    & git clone --depth 1 --branch $tag --recurse-submodules --shallow-submodules `
        https://github.com/baldurk/renderdoc.git $src
    if ($LASTEXITCODE -ne 0) { Fail "git clone に失敗しました(タグ $tag は存在しますか)" }
}

# --- 5. ビルド ---------------------------------------------------------------------
Write-Step "ビルドする(数分かかります)"
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { Fail "Visual Studio が見つかりません" }
$msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe |
    Select-Object -First 1
if (-not $msbuild) { Fail "MSBuild が見つかりません" }

# 最新のWindows SDK。指定しないと props が 8.1 へ落ちる
$sdk = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\Include" -Directory -ErrorAction SilentlyContinue |
    Sort-Object Name -Descending | Select-Object -First 1
if (-not $sdk) { Fail "Windows 10/11 SDK が見つかりません" }

$env:RENDERDOC_PYTHON_PREFIX64 = $prefix
# 【PlatformToolset を明示する】vcxproj は v140(VS2015)を指定しているため、
# 新しいVSしか無いと "TRK0005: CL.exe が見つかりません" で落ちる
$toolsetDir = Get-ChildItem (Join-Path (Split-Path (Split-Path $msbuild -Parent) -Parent) "..\..\VC\Tools\MSVC") `
    -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending | Select-Object -First 1
$toolset = "v143"
$common = @(
    "/p:Configuration=Release", "/p:Platform=x64", "/p:SolutionDir=$src\",
    "/p:PlatformToolset=$toolset", "/p:WindowsTargetPlatformVersion=$($sdk.Name)",
    "/m", "/nologo", "/v:m")

# 【breakpad を先に建てる】renderdoc.vcxproj はこの3つの .lib を直接参照しているのに
# プロジェクト参照になっていない。単体ビルドではリンクで LNK1181 になる
$deps = @(
    "renderdoc\3rdparty\breakpad\client\windows\common.vcxproj",
    "renderdoc\3rdparty\breakpad\client\windows\crash_generation\crash_generation_client.vcxproj",
    "renderdoc\3rdparty\breakpad\client\windows\handler\exception_handler.vcxproj",
    "qrenderdoc\Code\pyrenderdoc\pyrenderdoc_module.vcxproj")

foreach ($proj in $deps) {
    $name = Split-Path $proj -Leaf
    Write-Host "    $name"
    $out = & $msbuild (Join-Path $src $proj) $common 2>&1
    if ($LASTEXITCODE -ne 0) {
        $errLines = $out | Select-String -Pattern ": (error|fatal error) " | Select-Object -First 5
        if (-not $errLines) {
            # 【空にしない】"error" を含まない失敗もある(リンカの内部エラーなど)。
            # 理由が出ないまま「失敗しました」とだけ言われると手掛かりが消える
            $errLines = $out | Select-Object -Last 8
        }
        $errLines | ForEach-Object { Write-Host "      $($_.ToString().Trim())" -ForegroundColor Red }
        Fail "$name のビルドに失敗しました"
    }
}

$pyd = Join-Path $src "x64\Release\pymodules\renderdoc.pyd"
if (-not (Test-Path $pyd)) { Fail "renderdoc.pyd ができていません: $pyd" }

# --- 6. 配置 -----------------------------------------------------------------------
Write-Step "配置する"
New-Item -ItemType Directory $Destination -Force | Out-Null
Copy-Item $pyd (Join-Path $Destination "renderdoc.pyd") -Force
Write-Host "    $(Join-Path $Destination 'renderdoc.pyd')"

# --- 7. 動くことを確かめる ----------------------------------------------------------
# **「置けた」ではなく「読める」ことまで見る。**
# バージョンが食い違っていると、ここで初めて分かる
Write-Step "読み込めるか確かめる"
$probe = Join-Path $WorkDir "probe.py"
$probeBody = @"
import os, sys
sys.path.append(r"$Destination")
os.environ["PATH"] = r"$rdDir" + os.pathsep + os.environ["PATH"]
if sys.version_info >= (3, 8):
    os.add_dll_directory(r"$rdDir")
import renderdoc as rd
rd.InitialiseReplay(rd.GlobalEnvironment(), [])
rd.ShutdownReplay()
print("OK attrs=%d" % len([a for a in dir(rd) if not a.startswith("_")]))
"@
[System.IO.File]::WriteAllText($probe, $probeBody, (New-Object System.Text.UTF8Encoding($false)))
$result = & $py.Exe $probe 2>&1
if ($LASTEXITCODE -ne 0) {
    $result | ForEach-Object { Write-Host "    $_" -ForegroundColor Red }
    Fail "renderdoc.pyd を読み込めませんでした"
}
Write-Host "    $result"

# --- 8. 後始末 ---------------------------------------------------------------------
if (-not $KeepSource) {
    Write-Step "ソースを消す(-KeepSource で残せます)"
    Remove-Item $src -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item $prefix -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host ""
Write-Host "完了。Tools\renderdoc_probe.py を使う前に、この環境変数を設定してください:" -ForegroundColor Green
Write-Host ""
Write-Host "    `$env:KURENAI_RENDERDOC_PYMODULE = `"$Destination`""
Write-Host ""
Write-Host "実行は python$($py.XY) で行うこと(ビルドに使ったのと同じバージョンでしか読めません)"
