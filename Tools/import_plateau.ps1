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

.EXAMPLE
    Tools\import_plateau.ps1
    Tools\import_plateau.ps1 -Configuration Release
#>

param(
    [ValidateSet("Debug", "Release", "Both")]
    [string]$Configuration = "Both",
    [switch]$Force
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
$packed = 0
$skipped = 0
foreach ($f in $files)
{
    $code = $f.BaseName.Split('_')[0]
    $out = Join-Path $outDir "$code.kmodel"
    if ((-not $Force) -and (Test-Path -LiteralPath $out))
    {
        $skipped++
        continue
    }
    Invoke-Native -Exe $packer -Arguments @($f.FullName, "-o", $out, "--origin", $origin) -What "$code のパック" | Out-Null
    $packed++
}
$sw.Stop()
Write-Host ("        新規 $packed / スキップ $skipped / {0:N1}分" -f $sw.Elapsed.TotalMinutes)

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
