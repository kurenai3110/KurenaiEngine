---
name: build-run
description: KurenaiEngine をビルドして起動するときに使う。叩くslnの使い分け、初回セットアップ(submodule・assimp・DirectXTex・fxcのPATH)、シェーダもビルド対象であること、起動後のログ確認を含む。Use when asked to build, rebuild, or "ビルドして起動して".
effort: low
context: fork
background: false
---

# ビルドと起動

## 0. この手順は fork されたサブエージェント文脈で走る

frontmatter に `context: fork` を指定してあるので、**この手順はサブエージェントの中で実行され、
MSBuild の出力は呼び出し元の会話に入らない**。数千行のビルドログでコンテキストを潰さないため。

そのぶん、**呼び出し元へ返す内容がここで決まる。** 最後に必ずこの形で返すこと。

```
成否: 成功 / 失敗
構成: <sln の絶対パス> / <Debug|Release> / x64
出力: <実行ファイルのあるフォルダの絶対パス>
シェーダ: <.kshader を焼き直したか。「すべて最新です」で終わったならそう書く>
失敗した場合: エラー行をそのまま <ファイル>:<行> つきで。最初の3件まで
起動: <Run*.bat の絶対パス、または exe と引数>
ログ: <KurenaiEngine_DX11.log / DX12.log の絶対パス>
```

**起動・スクリーンショット・絵の良し悪しの判断はここでやらない。** ウィンドウは共有資源で、
撮った画像はサブエージェントの文脈に閉じてしまう。ビルドまでで止めて、上の形で返す。
起動は呼び出し元が `verify-app` で行う。

## MSBuild

```powershell
$msbuild = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" `
             -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe |
           Select-Object -First 1
& $msbuild "$env:CLAUDE_PROJECT_DIR\Samples\Sample3D\Sample3D.sln" `
    /p:Configuration=Debug /p:Platform=x64 /m /nologo /v:m
```

worktreeで作業中は**必ず自分のworktreeのslnを指すこと**(`$env:CLAUDE_PROJECT_DIR` を使う)。

## どのslnを叩くか

slnが5つあり、目的で使い分ける。**「動かして確認したい」なら Sample3D.sln で、
`KurenaiEngine.sln` ではない。**

| やりたいこと | 叩くsln | 備考 |
|---|---|---|
| 動かして絵を見る(3D) | `Samples\Sample3D\Sample3D.sln` | Library+3D+ShaderPackerをプロジェクト参照。DLLもシェーダーも一緒にビルドされる |
| 動かして絵を見る(2D) | `Samples\Sample2D\Sample2D.sln` | Library+2D+ShaderPackerのみ。3DもAssetsも要らない |
| 3つのDLLが通るかだけ見る | `KurenaiEngine.sln` | ビルド確認用。実行ファイルは出ない(シェーダーは焼かれる) |
| アセットを変換する | `Tools\KurenaiPacker\KurenaiPacker.sln` | assimp・DirectXTexの事前ビルドが要る |
| ドローンショーを編集する | `Tools\KurenaiShowEditor\KurenaiShowEditor.sln` | エンジンの2DLLに依存 |

## 構成の使い分け: 測るときはRelease、追うときはDebug

`/p:Configuration=Release` で切り替える。**性能を語る数値は必ずReleaseで取る**
(`docs/ImplementationHistory.md` にはDebug計測の値も残っているので、過去の数値と比べるときは
どちらで測ったかを確認する)。ブレークポイントを置くならDebug。

## 初回セットアップ(クローン直後・worktree作成直後は必ず失敗する)

そのままMSBuildを叩くと失敗する。この順で用意する。
**手順2・3はKurenaiPacker(アセット変換ツール)を使う場合にだけ必要**で、
エンジン本体とサンプルをビルドするだけなら要らない。

### 1. submoduleを取る(必須)

```powershell
git -C "$env:CLAUDE_PROJECT_DIR" submodule update --init --recursive
```

`ThirdParty/` に imgui・DirectXTex・assimp・xatlas・meshoptimizer が入る。
このうち **xatlas と meshoptimizer は別途ビルドが不要**(KurenaiPackerが必要なソースを
直接コンパイルする)。

### 2. assimp をビルドする(KurenaiPackerを使う場合のみ)

**assimpはエンジンのどのDLLにもリンクされない。** KurenaiPacker.exe のビルドにだけ必要。
インポータはglTF/FBX/OBJだけ有効にした静的ライブラリとして作る。

```powershell
cmake -S ThirdParty/assimp -B ThirdParty/assimp/build -G "Visual Studio 17 2022" -A x64 `
  -DBUILD_SHARED_LIBS=OFF -DASSIMP_BUILD_TESTS=OFF -DASSIMP_BUILD_ASSIMP_TOOLS=OFF `
  -DASSIMP_INSTALL=OFF -DASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT=OFF `
  -DASSIMP_BUILD_GLTF_IMPORTER=ON -DASSIMP_BUILD_FBX_IMPORTER=ON -DASSIMP_BUILD_OBJ_IMPORTER=ON `
  -DASSIMP_BUILD_ZLIB=ON -DASSIMP_NO_EXPORT=ON -DASSIMP_WARNINGS_AS_ERRORS=OFF
cmake --build ThirdParty/assimp/build --config Debug   --target assimp
cmake --build ThirdParty/assimp/build --config Release --target assimp
```

**Debug/Release の両方をビルドすること。** 構成ごとに別のlibを見に行くので、
片方しか無い状態でもう片方を叩くとリンクで落ちる。

### 3. DirectXTex をビルドする(KurenaiPackerを使う場合のみ)

**罠: DirectXTexのビルド前処理が `Shaders\CompileShaders.cmd` を実行し、その中で
`fxc.exe` を PATH 経由で呼ぶ。** 先にWindows SDKの `bin\<SDKバージョン>\x64` を
PATHへ追加しないと、ここで失敗する。

```powershell
$sdkBin = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" -Directory |
          Where-Object { Test-Path "$($_.FullName)\x64\fxc.exe" } |
          Sort-Object Name -Descending | Select-Object -First 1
if (-not $sdkBin) { throw "fxc.exe が見つかりません。Windows SDK を確認すること" }
$env:PATH = "$($sdkBin.FullName)\x64;$env:PATH"

& $msbuild "$env:CLAUDE_PROJECT_DIR\ThirdParty\DirectXTex\DirectXTex\DirectXTex_Desktop_2022.vcxproj" `
    /p:Configuration=Debug   /p:Platform=x64 /nologo /v:m
& $msbuild "$env:CLAUDE_PROJECT_DIR\ThirdParty\DirectXTex\DirectXTex\DirectXTex_Desktop_2022.vcxproj" `
    /p:Configuration=Release /p:Platform=x64 /nologo /v:m
```

## 出力先

```
Build\Bin\x64\<Config>\<プロジェクト名>\          3つのDLL + 焼いた Shaders\*.kshader
Samples\Sample3D\Build\Bin\x64\<Config>\          Sample3D.exe + DLL + Shaders\*.kshader + Assets\(Packed の中身)
Samples\Sample2D\Build\Bin\x64\<Config>\          Sample2D.exe + DLL + Shaders\*.kshader
Tools\KurenaiPacker\Build\Bin\x64\Release\        KurenaiPacker.exe
Tools\KurenaiShaderPacker\Build\Bin\x64\<Config>\ KurenaiShaderPacker.exe + dxcompiler.dll / dxil.dll
```

DLL・Shaders・Assets のコピーはPostBuildEventで走るので、各出力フォルダはそれだけで完結して動く。

**`KurenaiShaderPacker` はエンジンの2つのDLLよりも先に建つ**(`KurenaiEngine3D` / `KurenaiEngine2D`
からプロジェクト参照が張ってあり、4つの sln すべてに登録されている)。
Windows SDK の `bin\<SDKバージョン>\x64` から `dxcompiler.dll` / `dxil.dll` を持ってくるのも
このツールのPostBuildEventで、**エンジンの配布物には入らない**。

## ビルド後に必ず確認するもの

```powershell
$config = "Debug"   # Release を見るならここを変える
Get-ChildItem "$env:CLAUDE_PROJECT_DIR\Samples\Sample3D\Build\Bin\x64\$config" |
    Select-Object Name, LastWriteTime
```

`Sample3D.exe` / `KurenaiEngineLibrary.dll` / `KurenaiEngine3D.dll` / `Shaders\` が揃っていること。
`Shaders\` の中身は **`.kshader` だけ**(`.hlsl` があったら以前のビルドの残骸なので消す)。

**`dxcompiler.dll` / `dxil.dll` は実行ファイルの隣には来ない。** シェーダーを事前コンパイルへ
移したので、dxcが要るのはビルド時の `KurenaiShaderPacker` だけになった
(そちらの出力フォルダにコピーされる)。実行時にどのバリアントが選ばれたかは、
起動ログの「事前コンパイル済みシェーダー: ...」の行で確認する。

## シェーダはビルド対象(`.hlsl` を触ったらビルドする)

`KurenaiShaderPacker.exe` が `KurenaiEngine3D` / `KurenaiEngine2D` のビルドイベントで走り、
`.hlsl` を **`.kshader`**(事前コンパイル済みパッケージ)へ焼いて `$(OutDir)Shaders\` へ置く。
**出力フォルダに `.hlsl` はコピーされない**ので、差し替えて起動し直すやり方は使えない。

- **HLSLのコンパイルエラーはビルドで落ちる。** 全 `.hlsl` の全エントリを3バリアント
  (SM 5.0 / SM 6.5 / SM 6.6+bindless)で焼くので、ビルドが通れば一括検証も済んでいる
- パッカーは更新日時で増分に動く。`.hlsl` も `.hlsli` も未変更なら「すべて最新です」と出て即終わる。
  **`.hlsli` が1本でも変わると全ファイルを焼き直す**(どの `.hlsl` がどれをインクルードしているかを
  静的に追い切れないため、安全側に倒してある)。フル1回でおよそ10秒(16並列)
- **コンパイルが通ることと絵が正しいことは別。** シェーダを触ったら起動して確かめる
- パッカー単体で叩くこともできる:

```powershell
$packer = "$env:CLAUDE_PROJECT_DIR\Tools\KurenaiShaderPacker\Build\Bin\x64\Release\KurenaiShaderPacker.exe"
& $packer --input "$env:CLAUDE_PROJECT_DIR\KurenaiEngine\Shaders\3D" `
          --output "$env:CLAUDE_PROJECT_DIR\Build\Bin\x64\Release\KurenaiEngine3D\Shaders" `
          --config Release [--force]
& $packer --dump "<なにか>.kshader"    # 中身(エントリ・バリアント・プロファイル)を印字する
```

## 罠: Assets が無くてもビルドと起動は通る

`Assets/` はGit管理外なので、clone直後は `Assets\Packed\` が空になる。
このとき **Sample3Dはビルドも起動もできるが、表示するモデルとシーンが無い**。
「ビルドが通ったか」の判定はここまでで、絵が出ないことをビルド失敗と誤認しないこと。

モデルを出すには `Assets\Packed\` を用意する必要がある(`Assets\Source\` を用意し、
KurenaiPacker.exe で変換する)。手順は [README.md](../../../README.md)「手順5. アセットの準備」。
`Assets\Source\` は Sponza を除いて `Tools\*.py` で再生成できる。

なお **`.kmodel` は v10 / `.kgeom` は v4** で、バージョン不一致は読み込みを拒否される。
古い `Assets\Packed\` が残っている場合は再パックが要る。

## 起動と、通ったかどうかの判定

**この節は呼び出し元の担当。** 0節のとおり、この手順の中では起動しない。
下の内容は「返り値に何を書くか」を決めるために置いてある。

起動・PIDの扱い・スクリーンショットは `verify-app` スキルに従うこと
(**プロセスは必ずPIDで扱う。名前指定の終了は禁止**)。

```
Samples\Sample3D\RunDX12.bat      DX12で起動
Samples\Sample3D\RunDX11.bat      DX11で起動
Sample3D.exe -dx12                直接起動する場合(引数なしならDX11)
```

`Run*.bat` は、ビルドが済んでいない場合やシーンが無い場合は**起動せずに必要なコマンドを表示して
止まる**。起動の記録は `Samples\Sample3D\Run.log` に残る。

起動したら、実行ファイルと同じフォルダのログを読む。エラーハンドリングには必ずログを出す規約なので、
失敗はここに出ている。

| ファイル | 内容 |
|---|---|
| `KurenaiEngine_DX11.log` | DX11バックエンドのログ |
| `KurenaiEngine_DX12.log` | DX12バックエンドのログ |

ログの先頭には、GPU名とDXR / メッシュシェーダー / bindless(SM6.6)の対応状況が出る。
**RT系やメッシュレットの検証をする前に、まずここを読んで実機が対応しているか確かめること**
(Intel UHD Graphics 620 のようなiGPUではいずれも非対応で、機能自体が選択肢に出ない)。

`window.ini` と `imgui.ini` も同じフォルダに作られる(ウィンドウ配置とパネル配置の記憶。
`Build\` 配下なのでGit管理外)。

**ビルドやテストが失敗したら、失敗したと出力付きで報告する。** 取り繕わない。
