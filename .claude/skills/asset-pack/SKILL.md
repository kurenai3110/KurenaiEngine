---
name: asset-pack
description: KurenaiEngineのアセットを用意・変換するときに使う。Assets/SourceからKurenaiPacker.exeでAssets/Packedへ焼く経路、フォーマットのバージョン不一致で読み込みを拒否されること、取り込みでZが反転すること、material_indexのタグ付け漏れが黙って別の素材で描かれること、配置を材質IDレンダで測る方法を含む。Use when packing, repacking, or generating KurenaiEngine assets (.kmodel/.kgeom/.ktex/.kscene).
---

# アセットの変換と検証

**コマンド列は [README.md](../../../README.md) の「5. アセットの準備(KurenaiPacker)」にある。
ここに写さない。** ここに書くのは、その手順で静かに間違える箇所だけ。

## 0. 経路

```
Assets/Source/  (.gltf / .fbx / .obj。Git管理外)
      │  Tools/KurenaiPacker/Build/Bin/x64/Release/KurenaiPacker.exe
      ▼
Assets/Packed/  (.kmodel / .kgeom / .ktex / .kscene)   ← エンジンが読むのは常にこちら
```

- **`Assets/` はGit管理外**(180MB規模)。cloneやworktreeには付いてこない
- **手書きの `.kscene` だけは `Assets/` の外の `Scenes/` にあり、Git管理対象**
- KurenaiPacker自身のビルドには assimp と DirectXTex が要る(`build-run` の初回セットアップ)

## 1. バージョン不一致は「読み込み拒否」で出る

**`.kmodel` は v10 / `.kgeom` は v4**(定義: `KurenaiEngine/Source/Library/Assets/ModelPackage.h`
の `kPackageVersion`)。**フォーマットを触ったら、既存の `Assets/Packed/` は全部再パックが要る。**

バージョンを上げるたびに全アセットの再パックが要るため、**追加は1回のバージョン上げに
まとめる**設計になっている。1項目ずつ上げない。

## 2. 取り込みでZが反転する

KurenaiPackerは `aiProcess_ConvertToLeftHanded` で読み込む。**Zが反転して入る。**

- **南に置きたいものは、glTF側で Z>0 に書き出す**
- 「向きが合わない」ときに**レンダ結果の側を反転して辻褄を合わせない。**
  対症療法をすると、以後の照合(南面と北面を並べて「一致」と読む等)まで壊れる

## 3. 材質のタグ付け —— 既定値0は実在の素材

**`material_index` の既定値 0 は「未設定」ではなく、実在の素材である。**
タグを付け忘れた面は、エラーにならず**その素材で黙って描かれる**。

- 手続き生成(`Tools/blender_*.py`)では、**集合差で未タグの面を追う**。
  `bmesh.ops` は面を作り直すことがあり、`polygon.material_index` を一律0へ戻す挙動が
  実測で確認されている
- **未タグ検査を常設する。** 「未タグの面が0」を機械的な合否項目にして、
  通らないうちは見た目の判断へ進まない

## 4. 配置は材質IDレンダで測る

**見た目用アルベドの色で分類すると取り違える。** 診断用に、ベースカラーを識別しやすい
パレットへ差し替えて書き出す経路がある:

```powershell
# Blender側の書き出しオプション。パレットは MATERIAL_ID_PALETTE
blender --background --python Tools/blender_msm_island.py -- --material-id ...
```

分類の集計スクリプトは**使い捨て**でよい(`Claude/` へ置く。`.gitignore` 対象)。
ただし**書き戻して検証するまで、抽出結果を信用しない。**

## 5. 再生成できるもの

- `Assets/Source/` のうち **Sponza 以外は `Tools/*.py` で再生成できる**(検証用シーンは
  すべてスクリプト生成)。手順は [README.md](../../../README.md)「手順5. アセットの準備」
- PLATEAU / Emerald Square など外部データの取り込みは `Tools/import_*.ps1` と
  `Tools/plateau_*.py`
- **手続き生成の見た目は1枚で判断しない。** 乱数の実現値を複数取る

## 6. 焼いたあと

1. 起動してログを見る(読み込み拒否はここに出る)。`build-run` / `verify-app`
2. 配置や寸法を言うなら、**材質IDレンダなど見た目に依らない経路で測る**
3. **数値・「壊していない」・原因の断定を報告する前に `double-check`**(重い主張は2系統)

## 関連

- ビルドとKurenaiPackerのビルド: `build-run`
- 実物へ寄せる作業: `reference-match` / `reference-collection`
- 報告前の検算: `double-check`
