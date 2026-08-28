# HLSL bulk validation for KurenaiEngine.
#
# Shaders are read from the output folder at RUNTIME, so a green C++ build proves
# nothing about them. This compiles every engine shader the way each backend does.
#
# TWO BACKENDS, TWO COMPILERS -- both have to pass:
#   - DX11 : fxc, vs_5_0 / ps_5_0 / cs_5_0  (D3DCompileFromFile in DX11Device.cpp)
#   - DX12 : dxc, *_6_6                     (DX12Device::CreateShader routes EVERY stage
#            through DX12ShaderCompiler when dxcompiler.dll loads. D3DCompileFromFile
#            with *_5_0 is only the fallback for machines without dxc / below SM 6.0.)
#
# Checking the DX12 side with fxc 5_0 is NOT equivalent and will miss real breakage:
# dxc 1.7+ defaults to HLSL 2021, which rejects things HLSL 2018 accepted (a ternary
# with a vector condition, && / || on vectors, ...). fxc never sees any of it.
# It also misses register overlaps that only appear once all includes are combined.
#
# The DX12 pass runs twice, with and without -D KURENAI_BINDLESS=1, because the engine
# only defines it when the device supports SM 6.6 -- both configurations ship.
#
# Entry points are discovered structurally, not by a fixed name list, because the
# engine uses suffixed entries (CSDownsample, PSMainBlur, CSClearHistogram, ...):
#   mesh         : function carrying an [outputtopology(...)] attribute
#   amplification: [numthreads(...)] function whose body calls DispatchMesh()
#   compute      : any other function carrying a [numthreads(...)] attribute
#   pixel        : VS/PS-prefixed function with an SV_ return semantic or a PSOutput return
#   vertex       : VS-prefixed function with SV_ in its parameters or a struct return
#
# Amplification/mesh shaders exist only on the bindless path (they read geometry through
# ResourceDescriptorHeap), so they are compiled by dxc only, and only with the define.
#
# Exit code 0 = all good, 1 = at least one shader failed.

[CmdletBinding()]
param(
    [string]$ShaderRoot,
    [string]$SdkBin = "C:\Program Files (x86)\Windows Kits\10\bin",
    # DX12ShaderCompiler がデバイス対応の範囲で 6.6 を上限に使うため、それに合わせる
    [string]$DxcModel = "6_6"
)

$ErrorActionPreference = 'Stop'

if (-not $ShaderRoot) {
    $repo = if ($env:CLAUDE_PROJECT_DIR) { $env:CLAUDE_PROJECT_DIR } else { (Get-Location).Path }
    $ShaderRoot = Join-Path $repo 'KurenaiEngine\Shaders'
}
if (-not (Test-Path $ShaderRoot)) {
    Write-Error "シェーダのルートが見つかりません: $ShaderRoot"
    exit 1
}

function Find-Tool([string]$name) {
    $c = Get-ChildItem $SdkBin -Recurse -Filter $name -ErrorAction SilentlyContinue |
         Where-Object { $_.DirectoryName -like '*\x64' } |
         Sort-Object FullName -Descending | Select-Object -First 1
    if ($null -eq $c) { return $null }
    return $c.FullName
}

# Native exe + "2>&1" makes Windows PowerShell 5.1 wrap each stderr line in an
# ErrorRecord (NativeCommandError) and flips $? even on exit code 0.
# Redirect to files via Start-Process instead.
function Invoke-Compiler([string]$exe, [string[]]$argList) {
    $so = [System.IO.Path]::GetTempFileName()
    $se = [System.IO.Path]::GetTempFileName()
    try {
        $p = Start-Process -FilePath $exe -ArgumentList $argList -NoNewWindow -Wait -PassThru `
                           -RedirectStandardOutput $so -RedirectStandardError $se
        $text = ''
        try { $text += [System.IO.File]::ReadAllText($se) } catch {}
        try { $text += [System.IO.File]::ReadAllText($so) } catch {}
        return @{ Code = $p.ExitCode; Text = $text }
    } finally {
        Remove-Item $so, $se -Force -ErrorAction SilentlyContinue
    }
}

function Get-Entries([string]$src) {
    $entries = New-Object System.Collections.ArrayList

    # mesh shader: carries [outputtopology(...)]. Must be matched before [numthreads],
    # because a mesh shader carries both attributes.
    foreach ($m in [regex]::Matches($src, '\[outputtopology\s*\([^)]*\)\][\s\S]{0,200}?void\s+(\w+)\s*\(')) {
        [void]$entries.Add(@{ Name = $m.Groups[1].Value; Stage = 'ms' })
    }
    foreach ($m in [regex]::Matches($src, '\[numthreads\s*\([^)]*\)\]\s*\w+\s+(\w+)\s*\(')) {
        $name = $m.Groups[1].Value
        if ($entries | Where-Object { $_.Name -eq $name }) { continue }
        # amplification shaders are [numthreads] functions that end in DispatchMesh()
        $stage = if ($src.Substring($m.Index) -match 'DispatchMesh\s*\(') { 'as' } else { 'cs' }
        [void]$entries.Add(@{ Name = $name; Stage = $stage })
    }
    # pixel: "<type> PSxxx(...) : SV_Target"  or  "PSOutput PSxxx(...)"
    foreach ($m in [regex]::Matches($src, '(?m)^\s*\w+\s+(PS\w*)\s*\([^)]*\)\s*:\s*SV_\w+')) {
        [void]$entries.Add(@{ Name = $m.Groups[1].Value; Stage = 'ps' })
    }
    foreach ($m in [regex]::Matches($src, '(?m)^\s*PSOutput\s+(PS\w*)\s*\(')) {
        [void]$entries.Add(@{ Name = $m.Groups[1].Value; Stage = 'ps' })
    }
    # shadow pass has "void PSMain(PSInput input)" with no render target
    foreach ($m in [regex]::Matches($src, '(?m)^\s*void\s+(PS\w*)\s*\(')) {
        [void]$entries.Add(@{ Name = $m.Groups[1].Value; Stage = 'ps' })
    }
    # vertex: "<struct> VSxxx(...)"
    foreach ($m in [regex]::Matches($src, '(?m)^\s*(\w+)\s+(VS\w*)\s*\(([^)]*)\)')) {
        $ret = $m.Groups[1].Value
        $prm = $m.Groups[3].Value
        if ($ret -eq 'void') { continue }
        if ($prm -match 'SV_' -or $ret -match 'Input|Output') {
            [void]$entries.Add(@{ Name = $m.Groups[2].Value; Stage = 'vs' })
        }
    }

    $seen = @{}
    $uniq = New-Object System.Collections.ArrayList
    foreach ($e in $entries) {
        $k = "$($e.Stage):$($e.Name)"
        if ($seen.ContainsKey($k)) { continue }
        $seen[$k] = $true
        [void]$uniq.Add($e)
    }
    return $uniq
}

$fxc = Find-Tool 'fxc.exe'
$dxc = Find-Tool 'dxc.exe'
if (-not $fxc) { Write-Error "fxc.exe が見つかりません ($SdkBin 配下)"; exit 1 }
Write-Host "fxc : $fxc"
Write-Host "dxc : $(if ($dxc) { $dxc } else { '(なし - DXRシェーダはスキップ)' })"
Write-Host "root: $ShaderRoot`n"

# SDK同梱の dxc.exe は BOM 無しUTF-8 のファイルをANSIとして読み、日本語コメントで
# "dxc failed : error code 0x80070459" (Unicode変換不可) になる。fxc は逆に BOM を
# 受け付けないため、リポジトリ側は BOM 無しのままにし、DXRシェーダのときだけ
# シェーダツリー全体を BOM 付きで一時展開してそこからコンパイルする。
# (インクルードも同じ問題を起こすため、1ファイルではなくツリーごと複製する必要がある)
$stageDir = $null
function Get-StagedTree {
    if ($script:stageDir) { return $script:stageDir }
    $d = Join-Path ([System.IO.Path]::GetTempPath()) ("kurenai_shdstage_" + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $d -Force | Out-Null
    Get-ChildItem $ShaderRoot -Recurse -Include *.hlsl, *.hlsli | ForEach-Object {
        $rel = $_.FullName.Substring($ShaderRoot.Length).TrimStart('\')
        $dst = Join-Path $d $rel
        $dd  = Split-Path $dst
        if (-not (Test-Path $dd)) { New-Item -ItemType Directory -Path $dd -Force | Out-Null }
        $t = [System.IO.File]::ReadAllText($_.FullName, [System.Text.Encoding]::UTF8)
        [System.IO.File]::WriteAllText($dst, $t, (New-Object System.Text.UTF8Encoding($true)))
    }
    $script:stageDir = $d
    return $d
}

# .hlsli はインクルード専用でエントリポイントを持たないため対象外
$files = Get-ChildItem $ShaderRoot -Recurse -Filter *.hlsl | Sort-Object FullName
Write-Host "対象: $($files.Count) ファイル`n"

$failures = New-Object System.Collections.ArrayList
$okCount = 0
$noEntry = New-Object System.Collections.ArrayList

foreach ($f in $files) {
    $rel = $f.FullName.Substring($ShaderRoot.Length).TrimStart('\')

    # BOMチェック: fxc は BOM を error X3000 (illegal character) で弾く
    $bytes = [System.IO.File]::ReadAllBytes($f.FullName)
    if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
        [void]$failures.Add([PSCustomObject]@{ File=$rel; Entry='(BOM)'; Error='UTF-8 BOM が付いています。fxc が error X3000 で弾きます' })
        Write-Host "  [BOM]  $rel" -ForegroundColor Red
        continue
    }

    $src = [System.IO.File]::ReadAllText($f.FullName, [System.Text.Encoding]::UTF8)
    # SM 5.0 に無い機能を使うファイルは fxc(DX11経路)の対象外。DX11はそもそもこれらを描かない。
    #
    # 【KURENAI_BINDLESS を含むかどうかで判定してはいけない】そのマクロで**分岐している**だけの
    # ファイルは、マクロ未定義なら SM 5.0 で完全にコンパイルできる。Shadow.hlsl がまさにそれで、
    # DX11 は実行時に D3DCompileFromFile でこのファイルを読む。含むだけで外すと、
    # **DX11 経路が未検証のまま「失敗0」で通る**(しかも shadow の CreateShader は
    # try/catch に入っていないので、SM 5.0 で落ちる変更が入ると DX11 は起動時に死ぬ)。
    #
    # 判定したいのは「マクロ未定義に展開しても SM 5.0 の機能しか使わないか」である。
    # これは字面では決まらないため、フォールバックを持たない(=bindless 専用の)シェーダーには
    # ファイル先頭へ KURENAI_SHADER_BINDLESS_ONLY と書いてもらい、それを見る。
    # **付け忘れたら fxc に掛かって失敗する**ので、黙って検証から漏れることはない
    $sm6Only  = $src -match 'RayQuery|TraceRay|RaytracingAccelerationStructure|ResourceDescriptorHeap|KURENAI_SHADER_BINDLESS_ONLY'
    $entries  = Get-Entries $src

    if ($entries.Count -eq 0) { [void]$noEntry.Add($rel); continue }
    if (-not $dxc) { [void]$noEntry.Add("$rel (DX12経路: dxc なし)") }

    foreach ($e in $entries) {
        $jobs = New-Object System.Collections.ArrayList

        # DX11経路: fxc / SM 5.0
        if (-not $sm6Only -and $e.Stage -ne 'as' -and $e.Stage -ne 'ms') {
            [void]$jobs.Add(@{ Tool = 'fxc'; Profile = "$($e.Stage)_5_0"; Bindless = $false })
        }
        # DX12経路: dxc / SM 6.6。bindless の定義有無で2通りともコンパイルされ得る
        if ($dxc) {
            [void]$jobs.Add(@{ Tool = 'dxc'; Profile = "$($e.Stage)_$DxcModel"; Bindless = $true })
            if ($e.Stage -ne 'as' -and $e.Stage -ne 'ms') {
                [void]$jobs.Add(@{ Tool = 'dxc'; Profile = "$($e.Stage)_$DxcModel"; Bindless = $false })
            }
        }

        foreach ($j in $jobs) {
            if ($j.Tool -eq 'dxc') {
                $staged = Join-Path (Get-StagedTree) $rel
                # -HV 2018 はエンジンに合わせる。DX12ShaderCompiler::Compile が
                # 無条件に `-HV 2018` を渡している(DX12ShaderCompiler.cpp。dxcが将来
                # 既定を2021以降へ上げても、DLLの差し替えだけでシェーダーの意味が
                # 変わらないようにするため)。ここで渡さないと、dxc 1.7+ の既定である
                # HLSL 2021 で検証してしまい、**実際には動くコードを失敗と報告する**
                # (実例: ReflectionProbe.hlsli のベクタ条件の三項演算子で14件の偽陽性)
                $argList = @('-T', $j.Profile, '-HV', '2018', '-E', $e.Name, $staged, '-Fo', 'NUL')
                if ($j.Bindless) { $argList += @('-D', 'KURENAI_BINDLESS=1') }
                $label = "$($j.Profile)$(if ($j.Bindless) { ' +bindless' })"
                $r = Invoke-Compiler $dxc $argList
            } else {
                $label = $j.Profile
                $r = Invoke-Compiler $fxc @('/T', $j.Profile, '/E', $e.Name, '/I', $f.DirectoryName, '/Fo', 'NUL', '/nologo', "`"$($f.FullName)`"")
            }

            if ($r.Code -ne 0) {
                $lines = $r.Text -split "`r?`n" | Where-Object { $_ -match 'error' } | Select-Object -First 3
                if (-not $lines) { $lines = $r.Text -split "`r?`n" | Where-Object { $_.Trim() } | Select-Object -First 3 }
                [void]$failures.Add([PSCustomObject]@{ File=$rel; Entry="$($e.Name) ($label)"; Error=($lines -join ' / ') })
                Write-Host "  [FAIL] $rel :: $($e.Name) ($label)" -ForegroundColor Red
            } else {
                $okCount++
                Write-Host "  [ ok ] $rel :: $($e.Name) ($label)" -ForegroundColor DarkGray
            }
        }
    }
}

if ($script:stageDir -and (Test-Path $script:stageDir)) {
    Remove-Item $script:stageDir -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host ""
Write-Host "成功: $okCount エントリ / 失敗: $($failures.Count)"
if ($noEntry.Count -gt 0) {
    Write-Host "エントリ未検出($($noEntry.Count)件): $($noEntry -join ', ')" -ForegroundColor Yellow
}

if ($failures.Count -gt 0) {
    Write-Host ""
    Write-Host "=== 失敗一覧 ===" -ForegroundColor Red
    foreach ($x in $failures) {
        Write-Host ("{0}  [{1}]" -f $x.File, $x.Entry) -ForegroundColor Red
        Write-Host ("    {0}" -f $x.Error)
    }
    exit 1
}

Write-Host "すべてのシェーダがコンパイルできました。" -ForegroundColor Green
exit 0
