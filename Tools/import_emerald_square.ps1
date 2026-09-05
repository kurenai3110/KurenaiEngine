<#
.SYNOPSIS
    NVIDIA Emerald Square (ORCA) を取得して KurenaiEngine のアセットへ変換する。

.DESCRIPTION
    ダウンロード → 展開 → KurenaiPackerでパック → .ksceneの検証・配置 → 出力フォルダへ配布
    までを一括で行う。既にあるものは飛ばすので、何度実行しても構わない。

    アセットのライセンスは CC BY-NC-SA 3.0 Unported(非商用のみ・継承あり)。
    出典: NVIDIA ORCA - NVIDIA Emerald Square v4.1
          Nicholas Hull, Kate Anderson, Nir Benty (2017)
          https://developer.nvidia.com/orca/nvidia-emerald-square
    植生は SpeedTree の ORCA アセット。

.PARAMETER Configuration
    配布先のビルド構成。既定は Debug と Release の両方へ配る。

.PARAMETER Force
    既存の .ktex があっても再圧縮する。ソース側のテクスチャを描き直したときだけ必要で、
    通常は不要(このアセットは配布物が不変なため)。

.PARAMETER Inspect
    パックの前に --inspect を走らせ、単位系・軸・ノード数・バウンズ・テクスチャスロットを印字する。
    --scale や --specular-as-orm の判断根拠を取り直したいときに使う。

.EXAMPLE
    Tools\import_emerald_square.ps1
    Tools\import_emerald_square.ps1 -Inspect -Configuration Release
#>

param(
    [ValidateSet("Debug", "Release", "Both")]
    [string]$Configuration = "Both",
    [switch]$Force,
    [switch]$Inspect
)

$ErrorActionPreference = "Stop"

# Windows PowerShell 5.1 は、ネイティブexeがstderrへ書いた行を ErrorRecord として扱う。
# $ErrorActionPreference = "Stop" のままだと、KurenaiPackerが出す警告1行(フォールバック等)で
# スクリプトが停止してしまう ―― 実際に「ブロック圧縮が4x4未満」の警告で止まった。
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
$zipPath = "$repo\ThirdParty\SourceModels\EmeraldSquare_v4_1.zip"
$srcDir  = "$repo\ThirdParty\SourceModels\EmeraldSquare_v4_1"
$fbx     = "$srcDir\EmeraldSquare_Day.fbx"
$outModel = "$repo\Assets\Packed\EmeraldSquare\Day.kmodel"
$scene    = "$repo\Scenes\EmeraldSquare.kscene"
$sceneOut = "$repo\Assets\Packed\Scenes\EmeraldSquare.kscene"

if (-not (Test-Path -LiteralPath $packer -PathType Leaf)) {
    Write-Error "[ERROR] KurenaiPacker.exe が見つかりません: $packer (build-run スキルの手順でビルドしてください)"
    exit 1
}

# --- 1. 取得 -------------------------------------------------------------
# ダウンロードURLは302で署名付きURL(__token__付き)へ飛ぶ。トークンは短時間で失効するので
# 直リンクを書かず、必ず /emerald-square からリダイレクトを辿る。認証は不要
if (-not (Test-Path -LiteralPath $zipPath -PathType Leaf)) {
    Write-Host "[1/5] ダウンロード中(約589MB)..."
    & curl.exe -L --retry 3 --retry-delay 5 -o $zipPath "https://developer.nvidia.com/emerald-square"
    if ($LASTEXITCODE -ne 0) {
        Write-Error "[ERROR] ダウンロードに失敗しました(終了コード: $LASTEXITCODE)"
        exit 1
    }
} else {
    Write-Host "[1/5] ZIPは取得済み: $zipPath"
}

# 実測値。壊れたZIPを掴んだまま先へ進まないための検査
$expectedBytes = 617345248
$actualBytes = (Get-Item -LiteralPath $zipPath).Length
if ($actualBytes -ne $expectedBytes) {
    Write-Warning "[WARN] ZIPのサイズが実測値と違います(期待 $expectedBytes / 実際 $actualBytes)。配布物が更新された可能性があります"
}

# --- 2. 展開 -------------------------------------------------------------
if (-not (Test-Path -LiteralPath $fbx -PathType Leaf)) {
    Write-Host "[2/5] 展開中(約1.54GB)..."
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    try {
        [System.IO.Compression.ZipFile]::ExtractToDirectory($zipPath, (Split-Path -Parent $srcDir))
    }
    catch {
        Write-Error "[ERROR] 展開に失敗しました: $_"
        exit 1
    }
} else {
    Write-Host "[2/5] 展開済み: $srcDir"
}

# --- 3. inspect(任意) ----------------------------------------------------
if ($Inspect) {
    Write-Host "[3/5] --inspect でソースの構造を印字します"
    Invoke-Native -Exe $packer -Arguments @($fbx, "--inspect") -What "--inspect"
} else {
    Write-Host "[3/5] --inspect は省略(-Inspect で実行できます)"
}

# --- 4. パック -----------------------------------------------------------
# SpeedTreeの植生。マテリアル名末尾の .DoubleSided は両面描画の指定で、--inspect で
# これが付く29件を確認した。BaseColorが8bitアルファ(DXT5)を持つ26枚のうち植生18枚と対応する。
#
# 【なぜ要るのか】FBXにはglTFのalphaMode(OPAQUE/MASK/BLEND)に相当する情報が無く、
# 解析だけでは「アルファで抜く葉」を判別できない。指定しないと不透明な板として描かれる
# (Bistro(OBJ)の葉と同じ既知の破綻)
$cutoutMaterials = @(
    "Azalea_Leaves_1.DoubleSided",
    "Azalea_Leaves_1.DoubleSided1",
    "Azalea_Leaves_2.DoubleSided",
    "Azalea_Leaves_2.DoubleSided1",
    "Boston_Fern_Fiddlehead.DoubleSided",
    "Boston_Fern_Leaflets_1.DoubleSided",
    "Cards.DoubleSided",
    "EuropeanLindenLeaf.DoubleSided",
    "EuropeanLindenLeaf_2.DoubleSided",
    "Grass_blades.DoubleSided",
    "Hedge_Base.DoubleSided",
    "Hedge_Leaves_1.DoubleSided",
    "Hedge_Leaves_3.DoubleSided",
    "JapaneseMapleLeaf_1.DoubleSided",
    "JapaneseMapleLeaf_1.DoubleSided1",
    "JapaneseMapleLeaf_1.DoubleSided2",
    "JapaneseMapleLeaf_1.DoubleSided3",
    "JapaneseMapleLeaf_1.DoubleSided4",
    "JapaneseMapleLeaf_2.DoubleSided",
    "JapaneseMapleLeaf_2.DoubleSided1",
    "JapaneseMapleLeaf_2.DoubleSided2",
    "JapaneseMapleLeaf_2.DoubleSided3",
    "JapaneseMapleLeaf_2.DoubleSided4",
    "Leaves.DoubleSided",
    "Leaves_2.DoubleSided",
    "Pink_Flower.DoubleSided",
    "Pink_Flower.DoubleSided1",
    "White_Oak_Leaves_Hero_1.DoubleSided",
    "White_Oak_Leaves_Hero_3.DoubleSided"
)

# 【--scale を付けない理由】このFBXは UnitScaleFactor=100 と宣言しているが、assimpが読んだ
# 直後のルート変換行列は単位行列で、頂点はそのままメートルだった(--inspect で確認:
# バウンズ 230.1 x 113.2 x 230.2m、展望塔の高さ108.25m = 配布元資料の108.3mと一致)。
#
# 【--specular-as-orm が要る理由】ORM(R=遮蔽/G=ラフネス/B=メタリック)がFBXの
# SpecularColorスロットに入っており(同梱README.txtに規約が明記)、既定の解決では
# 220マテリアル全部が1枚も拾えない。対照実験: なし225枚 / あり336枚(+111枚)
$packArgs = @($fbx, "-o", $outModel, "--specular-as-orm")
if ($Force) { $packArgs += "--force" }
foreach ($m in $cutoutMaterials) {
    $packArgs += "--alpha-cutout"
    $packArgs += "$m=0.5"
}

Write-Host "[4/5] パック中(カットアウト指定 $($cutoutMaterials.Count)件)..."
Invoke-Native -Exe $packer -Arguments $packArgs -What "パック"

# 【既知の警告】法線マップ115枚のうち6枚が 1x1 の ATI2(BC5) で、うち3枚が実際に参照される。
# BC5はブロック圧縮で最小4x4のため、1x1はD3D11がE_INVALIDARGを返し実行時にフォールバックされる
# (Emissive_Light_2 / Emissive_Light_Inst / Painted_Metal の法線がフラット法線になる)。
# 配布物側の作りで、絵への影響は軽微

# .kscene の検証・配置(参照する .kmodel の存在とバージョンをここで確認する)
Invoke-Native -Exe $packer -Arguments @("--scene", $scene, "-o", $sceneOut) -What ".ksceneの検証・配置"

# --- 5. 出力フォルダへ配布 -----------------------------------------------
# Sample3D.vcxproj の PostBuildEvent は Sample3D 自体が再ビルドされないと走らない。
# アセットだけを更新したときはここで明示的にコピーする必要がある
$configs = if ($Configuration -eq "Both") { @("Debug", "Release") } else { @($Configuration) }
foreach ($config in $configs) {
    $outDir = "$repo\Samples\Sample3D\Build\Bin\x64\$config"
    if (-not (Test-Path -LiteralPath $outDir -PathType Container)) {
        Write-Warning "[WARN] 出力フォルダが無いので配布を飛ばします: $outDir"
        continue
    }
    Write-Host "[5/5] $config へ配布中..."
    & xcopy /Y /I /E /D /Q "$repo\Assets\Packed" "$outDir\Assets\" | Out-Null
    if ($LASTEXITCODE -gt 1) {
        Write-Error "[ERROR] 配布に失敗しました(xcopy 終了コード: $LASTEXITCODE)"
        exit 1
    }
}

Write-Host ""
Write-Host "完了しました。起動して確認するには:"
Write-Host "  Samples\Sample3D\Build\Bin\x64\Release\Sample3D.exe -scene EmeraldSquare"
