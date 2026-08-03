<#
.SYNOPSIS
    Blenderをヘッドレスモードで起動し、指定したPythonスクリプトを実行する。

.DESCRIPTION
    Blender本体のパスは環境変数 KURENAI_BLENDER が設定されていればそれを使い、
    未設定なら既定のインストール先を使う。

    Blenderは --factory-startup を付けずに起動するとユーザープリファレンス読み込み時に
    クラッシュすることが確認されている(EXCEPTION_ACCESS_VIOLATION)ため、必ず
    --background --factory-startup --python <script> の形で起動する。

.PARAMETER Script
    Blenderに実行させるPythonスクリプトのパス。

.PARAMETER ScriptArgs
    Scriptへ渡す追加引数(Blenderの `--` の後ろに渡される)。

.EXAMPLE
    Tools\run_blender.ps1 -Script Tools\blender_msm_island.py -ScriptArgs @("--export", "Assets/Source/MontSaintMichelStudy/Island.gltf")
#>

param(
    [Parameter(Mandatory = $true)]
    [string]$Script,

    [string[]]$ScriptArgs = @()
)

$ErrorActionPreference = "Stop"

# Blender本体のパスを解決する(環境変数優先、無ければ既定のインストール先)
$defaultBlenderPath = "C:\Program Files\Blender Foundation\Blender 2.82\blender.exe"
$blenderPath = $env:KURENAI_BLENDER
if ([string]::IsNullOrEmpty($blenderPath)) {
    $blenderPath = $defaultBlenderPath
}

if (-not (Test-Path -LiteralPath $blenderPath -PathType Leaf)) {
    Write-Error "[ERROR] Blender実行ファイルが見つかりません: $blenderPath (環境変数 KURENAI_BLENDER で明示的なパスを指定できます)"
    exit 1
}

if (-not (Test-Path -LiteralPath $Script -PathType Leaf)) {
    Write-Error "[ERROR] 実行対象のPythonスクリプトが見つかりません: $Script"
    exit 1
}

# --factory-startupを必ず付ける(付けないとユーザープリファレンス読み込み時にクラッシュすることを実測済み)
$blenderArgs = @("--background", "--factory-startup", "--python", $Script)
if ($ScriptArgs.Count -gt 0) {
    $blenderArgs += "--"
    $blenderArgs += $ScriptArgs
}

Write-Host "実行: `"$blenderPath`" $($blenderArgs -join ' ')"

try {
    & $blenderPath @blenderArgs
    $exitCode = $LASTEXITCODE
}
catch {
    Write-Error "[ERROR] Blenderの起動に失敗しました: $blenderPath ($_)"
    exit 1
}

if ($exitCode -ne 0) {
    Write-Error "[ERROR] Blenderがエラー終了しました(終了コード: $exitCode)"
    exit $exitCode
}

exit 0
