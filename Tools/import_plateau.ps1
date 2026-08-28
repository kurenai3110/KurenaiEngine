<#
.SYNOPSIS
    Project PLATEAU 東京都23区の建築物モデル(LOD1)を KurenaiEngine のアセットへ変換する。

.DESCRIPTION
    ダウンロード → bldg/lod1 の展開 → 共通原点の算出 → 671タイルのパック →
    .kscene の生成と配置 → 出力フォルダへ配布 までを行う。
    既にあるものは飛ばすので、何度実行しても構わない。

    出典: 国土交通省 Project PLATEAU「3D都市モデル(Project PLATEAU)東京都23区」
          https://www.geospatial.jp/ckan/dataset/plateau-tokyo23ku
    ライセンス: 公共データ利用規約(PDL1.0) / CC BY 4.0 互換。商用利用可、**要出典表示**。
    著作権は各地方公共団体に帰属する。

.PARAMETER Configuration
    配布先のビルド構成。既定は Debug と Release の両方へ配る。

.PARAMETER Force
    既存の .kmodel があっても作り直す。

.PARAMETER Parallel
    同時に走らせる KurenaiPacker.exe の数。既定8。

    既定値は推測ではなく掃引して決めた。150タイルの実測(28論理コア)で
    1→29.9秒 / 4→7.9秒 / 8→4.8秒 / 12→4.4秒 / 16→4.7秒 / 24→4.7秒。
    8で頭打ちになり、12以降は誤差の範囲。同時に生きるプロセスの
    ワーキングセット合計は最大でも330MB程度で、メモリの制約にはならない。

    【なぜプロセスを分けるのか】LOD1 のタイルはテクスチャを1枚も持たないため、
    パッカーは GPU も内部のワーカースレッドも使わず、1タイルの処理は実質1コアで走る。
    28論理コアの機械で1コアしか使っていないので、プロセスを並べれば素直に効く。
    (テクスチャの多いモデルでは事情が逆で、GPU の BC7 圧縮が飽和しているため
     プロセスを分けても速くならない。docs/ImplementationDetail.md 50.6節)

    1にすると従来どおりの直列実行になる。

.EXAMPLE
    Tools\import_plateau.ps1
    Tools\import_plateau.ps1 -Configuration Release
    Tools\import_plateau.ps1 -Parallel 1        # 直列で回す(切り分け用)
#>

param(
    [ValidateSet("Debug", "Release", "Both")]
    [string]$Configuration = "Both",
    [switch]$Force,
    [ValidateRange(1, 64)]
    [int]$Parallel = 8
)

$ErrorActionPreference = "Stop"

# Windows PowerShell 5.1 はネイティブexeのstderrを ErrorRecord として扱うため、
# $ErrorActionPreference = "Stop" のままだと KurenaiPacker の警告1行で停止する。
# ネイティブ呼び出しの間だけ Continue にし、成否は終了コードで判定する
function Invoke-Native
{
    param(
        [Parameter(Mandatory = $true)][string]$Exe,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$What
    )
    $previous = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try
    {
        & $Exe @Arguments
        $code = $LASTEXITCODE
    }
    finally
    {
        $ErrorActionPreference = $previous
    }
    if ($code -ne 0)
    {
        Write-Error "[ERROR] $What に失敗しました(終了コード: $code)"
        exit 1
    }
}

$repo    = Split-Path -Parent $PSScriptRoot
$packer  = "$repo\Tools\KurenaiPacker\Build\Bin\x64\Release\KurenaiPacker.exe"
$zipPath = "$repo\ThirdParty\SourceModels\PLATEAU_tokyo23ku_fbx.zip"
$srcRoot = "$repo\ThirdParty\SourceModels\PLATEAU_tokyo23ku"
$lod1Dir = "$srcRoot\bldg\lod1"
$outDir  = "$repo\Assets\Packed\Plateau"
$scene   = "$repo\Scenes\PlateauTokyo23ku.kscene"
$sceneOut = "$repo\Assets\Packed\Scenes\PlateauTokyo23ku.kscene"

if (-not (Test-Path -LiteralPath $packer -PathType Leaf))
{
    Write-Error "[ERROR] KurenaiPacker.exe が見つかりません: $packer (build-run スキルの手順でビルドしてください)"
    exit 1
}

# --- 1. 取得 -------------------------------------------------------------
if (-not (Test-Path -LiteralPath $zipPath -PathType Leaf))
{
    Write-Host "[1/5] ダウンロード中(約2.99GB)..."
    & curl.exe -L --retry 3 --retry-delay 5 -o $zipPath `
        "https://gic-plateau.s3.ap-northeast-1.amazonaws.com/2020/13100_tokyo23-ku_2020_fbx_3_op.zip"
    if ($LASTEXITCODE -ne 0)
    {
        Write-Error "[ERROR] ダウンロードに失敗しました(終了コード: $LASTEXITCODE)"
        exit 1
    }
}
else
{
    Write-Host "[1/5] ZIPは取得済み: $zipPath"
}

$expectedBytes = 2989693483
$actualBytes = (Get-Item -LiteralPath $zipPath).Length
if ($actualBytes -ne $expectedBytes)
{
    Write-Warning "[WARN] ZIPのサイズが実測値と違います(期待 $expectedBytes / 実際 $actualBytes)。配布物が更新された可能性があります"
}

# --- 2. 展開(建築物のLOD1だけ) -------------------------------------------
#
# 【dem(地形)とtran(道路)は入れない】これらは6桁=2次メッシュ(約10km四方)で、
# bldgの3次メッシュ(約1km四方)とは分割単位が違う。1枚入れるだけでシーンAABBの対角が
# 14km級になり、遠クリップ面(farZ = max(100, 対角x4))が56kmまで伸びてカスケードシャドウが
# 破綻する。
# 【LOD2も入れない】LOD2整備済み80タイルのメッシュコードは全てLOD1側にも存在するため、
# 同時に読むと同じ建物が二重になりZファイティングを起こす。
if (-not (Test-Path -LiteralPath $lod1Dir -PathType Container))
{
    Write-Host "[2/5] bldg/lod1 を展開中(約3.19GB / 671ファイル)..."
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::OpenRead($zipPath)
    try
    {
        New-Item -ItemType Directory -Force -Path $lod1Dir | Out-Null
        $targets = $archive.Entries | Where-Object { $_.FullName -match 'bldg/lod1/.*\.fbx$' }
        foreach ($e in $targets)
        {
            [System.IO.Compression.ZipFileExtensions]::ExtractToFile($e, (Join-Path $lod1Dir $e.Name), $true)
        }
        Write-Host "        $($targets.Count) ファイルを展開しました"
    }
    catch
    {
        Write-Error "[ERROR] 展開に失敗しました: $_"
        exit 1
    }
    finally { $archive.Dispose() }
}
else
{
    Write-Host "[2/5] 展開済み: $lod1Dir"
}

# --- 3. 共通原点の算出 ---------------------------------------------------
#
# 【全タイルで同じ値を引く必要がある】PLATEAUのFBXは平面直角座標系 第9系(EPSG:6677)の
# 絶対座標で、原点から北へ最大52km離れている。頂点はfloat32で、.kmodelのAABBもそのまま
# 巨大になるため原点付近へ寄せるが、タイルごとに「自分のAABBの中心」で寄せると
# タイル同士の相対位置が壊れて街が崩れる。
Write-Host "[3/5] 共通原点を算出中..."
$originLine = & python "$repo\Tools\plateau_mesh.py" origin $lod1Dir | Select-String -Pattern '^\s*--origin\s+(\S+)'
if (-not $originLine)
{
    Write-Error "[ERROR] 共通原点を算出できませんでした"
    exit 1
}
$origin = $originLine.Matches[0].Groups[1].Value
Write-Host "        --origin $origin"

# --- 4. パック -----------------------------------------------------------
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$files = Get-ChildItem $lod1Dir -Filter *.fbx | Sort-Object Name
Write-Host "[4/5] $($files.Count) タイルをパック中..."
$sw = [Diagnostics.Stopwatch]::StartNew()
$skipped = 0

# パックが要るものだけをキューへ積む
$queue = [System.Collections.Queue]::new()
foreach ($f in $files)
{
    $code = $f.BaseName.Split('_')[0]
    $out = Join-Path $outDir "$code.kmodel"
    if ((-not $Force) -and (Test-Path -LiteralPath $out))
    {
        $skipped++
        continue
    }
    $queue.Enqueue([pscustomobject]@{ Code = $code; Src = $f.FullName; Out = $out })
}

# 子プロセスの出力はコンソールへ出さずタイルごとのファイルへ分ける。
# 並列で走らせると行が混ざって読めなくなるため(親が出す進捗だけをコンソールへ出す)
$logDir = Join-Path $env:TEMP ("kurenai_plateau_" + (Get-Date -Format "yyyyMMdd_HHmmss"))
New-Item -ItemType Directory -Force -Path $logDir | Out-Null

$running = [System.Collections.ArrayList]::new()
$failures = [System.Collections.ArrayList]::new()
$packed = 0
$total = $queue.Count

while ($queue.Count -gt 0 -or $running.Count -gt 0)
{
    while ($running.Count -lt $Parallel -and $queue.Count -gt 0)
    {
        $job = $queue.Dequeue()
        # Start-Process は -ArgumentList を空白で連結するだけで要素ごとの引用はしない。
        # パスに空白があると引数が割れるのでこちらで引用する
        $arguments = @("`"$($job.Src)`"", "-o", "`"$($job.Out)`"", "--origin", $origin,
                       "--log-suffix", "_$($job.Code)")
        # -RedirectStandardOutput と -RedirectStandardError に同じパスは渡せない(PS5.1)
        $p = Start-Process -FilePath $packer -ArgumentList $arguments -WorkingDirectory $repo `
                -NoNewWindow -PassThru `
                -RedirectStandardOutput (Join-Path $logDir "$($job.Code).out.log") `
                -RedirectStandardError  (Join-Path $logDir "$($job.Code).err.log")
        # 【.Handle を触っておく】これを落とすと WaitForExit 後に ExitCode が空になり、
        # 失敗が静かに消える
        $null = $p.Handle
        [void]$running.Add([pscustomobject]@{ Job = $job; Process = $p })
    }

    $finished = @($running | Where-Object { $_.Process.HasExited })
    foreach ($r in $finished)
    {
        $r.Process.WaitForExit()
        $packed++
        if ($r.Process.ExitCode -ne 0)
        {
            [void]$failures.Add($r.Job)
        }
        $running.Remove($r)
    }
    if ($finished.Count -eq 0)
    {
        Start-Sleep -Milliseconds 15
    }
    elseif ($packed % 50 -eq 0)
    {
        Write-Host ("        $packed/$total ...")
    }
}

# 【失敗はその場で止めず、最後に直列でやり直す】671件のうち1件が落ちたときに
# 残りを捨てるのは無駄が大きい。一時的なファイルの競合はこのやり直しで吸収される
if ($failures.Count -gt 0)
{
    Write-Host ("        失敗 $($failures.Count) 件を直列でやり直します...")
    $stillFailed = 0
    foreach ($job in $failures)
    {
        $previous = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        try
        {
            & $packer $job.Src "-o" $job.Out "--origin" $origin 2>&1 | Out-Null
            $code = $LASTEXITCODE
        }
        finally { $ErrorActionPreference = $previous }
        if ($code -ne 0)
        {
            $stillFailed++
            Write-Host "[ERROR] $($job.Code) のパックに失敗しました(ログ: $logDir\$($job.Code).err.log)"
        }
    }
    if ($stillFailed -gt 0)
    {
        Write-Error "[ERROR] $stillFailed 件のパックに失敗しました"
        exit 1
    }
}

$sw.Stop()
Write-Host ("        新規 $packed / スキップ $skipped / 並列 $Parallel / {0:N1}分" -f $sw.Elapsed.TotalMinutes)
Write-Host ("        タイルごとのログ: $logDir")

# --- 5. .kscene の生成 ---------------------------------------------------
& python "$repo\Tools\plateau_scene.py" $outDir $scene
if ($LASTEXITCODE -ne 0)
{
    Write-Error "[ERROR] .ksceneの生成に失敗しました"
    exit 1
}

Invoke-Native -Exe $packer -Arguments @("--scene", $scene, "-o", $sceneOut) -What ".ksceneの検証・配置"

# --- 配布 ---------------------------------------------------------------
# Sample3D.vcxproj の PostBuildEvent は Sample3D 自体が再ビルドされないと走らない
$configs = if ($Configuration -eq "Both") { @("Debug", "Release") } else { @($Configuration) }
foreach ($config in $configs)
{
    $dir = "$repo\Samples\Sample3D\Build\Bin\x64\$config"
    if (-not (Test-Path -LiteralPath $dir -PathType Container))
    {
        Write-Warning "[WARN] 出力フォルダが無いので配布を飛ばします: $dir"
        continue
    }
    Write-Host "[5/5] $config へ配布中..."
    & xcopy /Y /I /E /D /Q "$repo\Assets\Packed" "$dir\Assets\" | Out-Null
    if ($LASTEXITCODE -gt 1)
    {
        Write-Error "[ERROR] 配布に失敗しました(xcopy 終了コード: $LASTEXITCODE)"
        exit 1
    }
}

Write-Host ""
Write-Host "完了しました。起動して確認するには:"
Write-Host "  Samples\Sample3D\Build\Bin\x64\Release\Sample3D.exe -scene PlateauTokyo23ku"
