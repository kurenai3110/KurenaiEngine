#pragma once

#include <cstdint>

// KurenaiEngine専用モデルパッケージ形式(.kmodel / .kgeom / .ktex)の定義。
// KurenaiPacker(オフラインのアセットビルドツール)とランタイム(ModelLoader.cpp)の
// 両方から参照される、フォーマットの単一の正とするヘッダー。ヘッダオンリーで
// KURENAI_API(DLLエクスポート)は不要(値はコンパイル時定数、構造体はPOD)。
//
// 設計方針: 実行時の初回読み込みで重い前処理(assimp解析・WICデコード・ミップ生成・
// GPU BC7圧縮)を行っていた従来のディスクキャッシュ(.kmodelcache/.ktexcache、
// ソースファイルの更新日時+サイズで自動失効する派生物)を廃止し、KurenaiPacker.exeが
// 事前に生成する配布可能なアセットへ置き換える。そのため.ktexは.ktexcacheと異なり
// 元画像のタイムスタンプ/サイズを持たない(元画像が無くても成立する成果物のため)。
//
// バイト列はすべてリトルエンディアン。#pragma packは使わない
// (各構造体は自然アラインメントのままパディングが生じない配置に設計済みで、
// static_assertでサイズを固定している。フィールドの追加・削除・並び替えを行う場合は
// このstatic_assertも必ず更新し、ランタイム側のVersion検証に頼って互換性を保つこと)。
//
// 文字列の規約(.kmodel内のStringPool、.kscene内のパスも同様):
// UTF-8、NUL終端なし(長さで管理)、パス区切りは'/'に正規化。
// 空白・日本語・記号を含みうるため、char*のまま操作せずwstringへ変換してから扱うこと
// (Core/StringUtil.hのWideToUtf8/Utf8ToWideを使う)。

namespace Kurenai::Assets
{
    // === .kmodel (マニフェスト) ===
    //
    // ファイルレイアウト:
    //   [PackageHeader]
    //   [TextureEntry  × TextureCount]
    //   [MaterialEntry × MaterialCount]   (v10で追加。テクスチャ番号を参照するのでテクスチャの後ろ)
    //   [MeshEntry     × MeshCount]
    //   [LightEntry    × LightCount]
    //   [StringPool (StringPoolSize bytes)]
    //
    // StringPool中のパスはすべて「この.kmodel自身のあるディレクトリ」からの相対パス。
    // (.kscene内の[Model]Pathが「Assetsルート」からの相対パスであるのとは基準が異なる点に注意)

    constexpr char kPackageMagic[4] = { 'K', 'M', 'D', 'L' };
    // v2: MeshEntryへAlphaCutoff/EmissiveFactor/EmissiveTextureIndexを追加し、
    // LightEntry(モデルファイル埋め込みのライト。glTFのKHR_lights_punctual/FBXライトノード由来)
    // を追加したため加算。
    // v3: MeshEntryへFlags(bit0=半透明。glTFのalphaMode=BLEND)を追加したため加算。
    // v4: MeshEntryへBaseColorFactor[4](glTFのpbrMetallicRoughness.baseColorFactor)を追加したため
    // 加算。テクスチャを持たずbaseColorFactorのみで色/不透明度を表現するマテリアル(ガラス等でよくある
    // パターン)がBaseColorTextureIndex=-1(白1x1プレースホルダー、alpha=1)へ機械的にフォールバックし、
    // 意図した色・アルファと異なる見た目になっていたため追加した(14章参照)。
    // v5: MeshEntryへOcclusionTextureIndex(遮蔽マップ)とOcclusionStrength(glTFの
    // occlusionTexture.strength)を追加したため加算。ベイク済みのアンビエントオクルージョンを
    // マテリアルの5枚目のテクスチャとして扱えるようにした。SSAO/SSILはスクリーンスペース由来で
    // ジオメトリの実解像度を下回る遮蔽(布の折り目・狭い隙間など)を拾えないため、それを
    // アセット側のベイク結果で補う(14章参照)。
    // v6: 頂点フォーマットへライトマップUV(Vertex::UV1、TEXCOORD1)を追加したため加算。
    // 遮蔽マップをKurenaiPackerで焼く(--bake-occlusion)には重なりの無い専用UVが必要で、
    // タイリング前提のTEXCOORD0は流用できないため(22章)。VertexStrideの検証だけでも
    // 読み込みは拒否されるが、理由を明示するためVersionも上げる。
    // v7: MeshEntryへBentNormalTextureIndex(bent normal、正規化しない可視方向の平均)を
    // 追加したため加算。遮蔽マップが「どれだけ隠れているか」しか持たないのに対し、
    // bent normalは「どの方向が開いているか」を持つ。消費側がこれを軸・aoB・aoNの3つへ
    // 分解し、スペキュラ遮蔽の方向依存とディフューズの方向バイアスを扱えるようにした(34章参照)。
    // v8: bent normalの格納空間をモデル空間から接空間へ変更したため加算。構造体レイアウトは
    // v7と同一で、bent normalテクスチャの中身の意味だけが変わる。モデル空間では「遮蔽なし」が
    // 法線Nそのものになるため、曲面上では遮蔽が無くても隣り合うテクセルの向きが違い、
    // ミップ生成やバイリニア補間で平均すると打ち消し合って長さが縮む。消費側はその長さを
    // aoB(遮蔽率)として読むので、縮小するほど暗くなり細かい黒い点が出ていた。
    // 接空間なら遮蔽なしは曲率によらず常に(0,0,1)で、平均しても長さ1のまま保たれる(34.9)。
    // 【レイアウトが同じなのでVersionでしか判別できない】上げないと古いアセットを黙って
    // 誤って解釈することになる。
    // v9: MeshEntryへメッシュレット(MeshletOffset/MeshletCount ほか6フィールド)を追加し、
    // .kgeomのペイロードへメッシュレット3ブロックを足したため加算(kGeometryVersionも同時にv3へ)。
    // メッシュシェーダーがメッシュレット単位で錐台・法線コーンのカリングを行えるようにするため。
    // あわせてインデックスバッファの並びがメッシュレット順になった点も、この版からの変更。
    // 以前の.kmodelはVersion不一致で読み込み拒否され、KurenaiPackerの再実行で再生成される
    // v10: 次の4つをまとめて追加したため加算(.kgeomも同時にv4へ)。
    // (a) MeshEntryへメッシュ単位のAABB(BoundsMin/BoundsMax)。従来はモデル全体のAABBしか無く、
    //     「1モデルに多数のメッシュを持つアセットではフラスタムカリングで1つも間引けない」
    //     状態だった(PLATEAUのタイルは1枚が1.1km四方ある)。
    // (b) MaterialEntryを新設し、MeshEntryが持っていた材質(テクスチャ番号6つと係数)を切り出した。
    //     MeshEntryはMaterialIndexで引く形になる。bindless経路が、メッシュではなく
    //     実行時の番号でマテリアルを引けるようにするため。
    // (c) MeshletEntryへMaterialIndexとLODLevelを追加(48→64バイト)。モデル全体の
    //     メッシュレット総覧を1回のDispatchMeshで回すとき、メッシュレット自身が
    //     どの材質で描かれるかを知っている必要がある。
    // (d) メッシュレットの離散LOD。MeshEntryが段ごとの範囲(MeshletLODOffsets/Counts)を持ち、
    //     .kgeomのメッシュレット3ブロックが全段を連結して持つ。
    //
    // 【なぜ4つを1回のバージョン上げにまとめたのか】バージョンを上げるたびに全アセットの
    // 再パックが要る。PLATEAU東京23区の847モデルは変換だけで1〜3時間かかるため、
    // 段階的に上げると同じ変換を何度も回すことになる。フォーマットの変更は一度で決める
    constexpr uint32_t kPackageVersion = 10;

    struct PackageHeader
    {
        char     Magic[4];                // 'K','M','D','L' (kPackageMagic)
        uint32_t Version;                 // kPackageVersion。不一致なら読み込み拒否
        uint32_t VertexStride;            // sizeof(Vertex)。不一致なら読み込み拒否
        uint32_t IndexStride;             // sizeof(uint32_t)。不一致なら読み込み拒否
        // このモデルのAABB(パック時に確定したローカル空間)。
        // 【オフセット16から動かさないこと】ストリーミングやシーン生成スクリプトが
        // 「先頭だけ読んでAABBを得る」ために、ここの位置に依存している
        float    BoundsMin[3];
        float    BoundsMax[3];
        uint32_t MeshCount;
        uint32_t MaterialCount;           // v10で追加
        uint32_t TextureCount;
        uint32_t LightCount;
        uint32_t GeometryPathOffset;      // StringPool内オフセット。対になる.kgeomの相対パス
        uint32_t GeometryPathLength;
        uint32_t StringPoolSize;
        uint32_t Reserved;                // 0固定
    };
    static_assert(sizeof(PackageHeader) == 72, "PackageHeaderのレイアウトは72バイト固定");

    struct TextureEntry
    {
        uint32_t PathOffset;              // StringPool内オフセット。.ktexへの相対パス
        uint32_t PathLength;
        uint32_t Flags;                   // bit0: sRGB(検証用。実体は.ktexのDXGIフォーマットが持つ)
        uint32_t Reserved;                // 0固定
    };
    static_assert(sizeof(TextureEntry) == 16, "TextureEntryのレイアウトは16バイト固定");

    constexpr uint32_t kTextureEntryFlagSRGB = 1u << 0;

    // メッシュレットの離散LODの段数の上限(LOD0を含む)。
    //
    // 【4にした理由】段を1つ進めるごとに三角形を半分にするので、4段で1/8まで落ちる。
    // これはモデルLOD(.ksceneのLODPath)の上限4段と揃えた値でもある。
    // それ以上に粗くしたいものは、そもそもメッシュレット単位ではなくモデル単位で
    // 別の.kmodelへ差し替えるべき粒度になる。
    // MeshEntryが段ごとの範囲を固定長配列で持つため、増やすと全アセットの再パックが要る
    constexpr uint32_t kMaxMeshletLODCount = 4;

    // === MaterialEntry (v10で追加) ===
    //
    // 従来 MeshEntry が直接持っていた材質(テクスチャ番号6つと係数)をここへ切り出したもの。
    // MeshEntry は MaterialIndex でこの配列を引く。
    //
    // 【なぜ分けたのか】メッシュシェーダー経路では、モデル全体のメッシュレット総覧を
    // 1回の DispatchMesh で回して「1モデル = 1ドロー」にしたい。このときピクセルシェーダーは
    // 「今描いている三角形がどの材質か」をメッシュ単位の定数バッファからは知れないため、
    // 材質を番号で引ける表として持つ必要がある。
    //
    // 【mesh と material は 1 対 1 である】KurenaiPacker はマテリアル単位でメッシュを結合する
    // (ModelSource.cpp の meshesByMaterial)ので、出力されるメッシュ数は必ず
    // 「ジオメトリを持つマテリアルの数」に一致し、MaterialCount == MeshCount になる。
    // **--bake-occlusion が焼く遮蔽マップと bent normal はメッシュ単位の成果物**で、
    // その番号をこの MaterialEntry へ書いているため、この 1 対 1 に依存している。
    // 将来マテリアルを複数メッシュで共有させるなら、ベイク結果の持ち方を先に決め直すこと
    struct MaterialEntry
    {
        float    MetallicFactor;
        float    RoughnessFactor;
        // 0以下ならアルファカットアウト無効(常に不透明)。glTFのalphaMode=MASKのマテリアルのみ
        // alphaCutoff(既定0.5)が設定される
        float    AlphaCutoff;
        float    EmissiveFactor[3];
        // glTFのpbrMetallicRoughness.baseColorFactor(RGBA、既定[1,1,1,1])。テクスチャの有無に
        // 関わらず常に設定され、BaseColorTextureIndex=-1の場合の白1x1プレースホルダーと乗算される
        // ことで、テクスチャを持たずbaseColorFactorのみで色/不透明度を表現するマテリアル(ガラス等)を
        // 正しく再現する(14章参照)
        float    BaseColorFactor[4];
        // glTFのocclusionTexture.strength(既定1.0)。シェーダーはlerp(1, ao, strength)で適用する。
        // strengthはglTF仕様で既定値が1.0と明記されているため、ラフネス係数のような
        // kInvalidMaterialFactor(負値)方式は取らず、ソースに無ければ1.0を書き出す
        float    OcclusionStrength;
        // 透過率(0=不透明、1=完全に透ける)。葉・花弁のような薄い被写体が、裏から当たった光を
        // 透かして表側を光らせる量。DeferredLightingの透過項が使う(45章)
        float    Translucency;

        int32_t  BaseColorTextureIndex;           // -1 = 指定なし → 白1x1
        int32_t  NormalTextureIndex;              // -1 = 指定なし → フラット法線
        int32_t  MetallicRoughnessTextureIndex;   // -1 = 指定なし → 白1x1
        // -1 = 指定なし → 白1x1(EmissiveFactorが0ならどのみち結果は黒になるため、
        // BaseColor等と同様に白のプレースホルダーへフォールバックしてよい)
        int32_t  EmissiveTextureIndex;
        // ベイク済みアンビエントオクルージョン(遮蔽マップ)。glTFのocclusionTextureに対応し、
        // 赤チャンネルを遮蔽率(1=遮蔽なし、0=完全遮蔽)として読む。
        // -1 = 指定なし → 白1x1(=遮蔽なし。他のテクスチャと同様、シェーダー側に分岐を持たせず
        // プレースホルダーへフォールバックする方式)
        int32_t  OcclusionTextureIndex;
        // bent normal(正規化しない可視方向の平均、RGBA16F)。.rgb = bRaw、.a = 有効フラグ。
        // -1 = 指定なし → 黒1x1。
        //
        // 【白1x1ではなく黒1x1へ落とす】bent normalは「遮蔽なし」を定数テクスチャで表現できない。
        // 遮蔽なしのbRawは法線Nそのものでピクセルごとに違うため。かといって長さ0を遮蔽なしと
        // 解釈すると完全遮蔽と区別がつかなくなるので、.aを明示的な有効フラグにして
        // 曖昧さを消し、無効なら消費側でaxis=N・aoB=1へ落とす(34章参照)
        int32_t  BentNormalTextureIndex;

        uint32_t Flags;                    // bit0: 半透明(kMeshEntryFlagTransparent。glTFのalphaMode=BLEND)
        uint32_t Reserved;                 // 0固定
    };
    static_assert(sizeof(MaterialEntry) == 80, "MaterialEntryのレイアウトは80バイト固定");

    struct MeshEntry
    {
        uint64_t VertexOffset;            // .kgeomのペイロード先頭からのバイトオフセット(16B境界)
        uint64_t IndexOffset;             // 同上
        uint32_t VertexCount;
        uint32_t IndexCount;

        // === メッシュ単位のAABB(v10で追加) ===
        //
        // このメッシュだけを包むAABB(モデルのローカル空間)。PackageHeaderのAABBが
        // モデル全体を包むのに対し、こちらはメッシュ1つぶん。
        //
        // 【何のためにあるか】1つのモデルが多数のメッシュを持つアセット(街のタイル、
        // 大規模な屋外シーン)では、モデル全体のAABBが視錐台と交差する限り
        // 全メッシュを描くしかなく、フラスタムカリングが1つも間引けない。
        // メッシュごとのAABBがあれば、モデル単位の判定を通ったあとにもう一段間引ける。
        //
        // 【bindlessが使えない環境ではこれが唯一の削減手段】DX11とメッシュシェーダー
        // 非対応のGPUでは、GPU駆動カリングもメッシュレットカリングも効かない
        float    BoundsMin[3];
        float    BoundsMax[3];

        // MaterialEntry配列への番号。材質はすべてそちらにある。
        // パッカーがマテリアル単位でメッシュを結合するため、実際には常に自分のメッシュ番号と
        // 同じ値になる(MaterialEntryのコメント参照)が、消費側はその一致に依存してはいけない
        int32_t  MaterialIndex;
        uint32_t Reserved;                 // 0固定。uint64_tメンバによる8バイト境界へ揃えるため

        // === メッシュレット(v9で追加。v10で離散LODを持つよう拡張) ===
        //
        // メッシュシェーダーが「メッシュレット1つ = 1スレッドグループ」で描くための分割情報。
        // 増幅シェーダーがバウンディング球と法線コーンで錐台・背面カリングを行い、
        // 生き残ったメッシュレットだけをメッシュシェーダーへ渡す。
        // メッシュ全体でしかカリングできなかった従来と比べ、ドラゴンのような
        // 「1メッシュ=数十万三角形」のモデルで画面外の三角形を大量に落とせる。
        //
        // 3つのオフセットはVertexOffset/IndexOffsetと同じく.kgeomペイロード先頭からの
        // バイトオフセット(いずれも16バイト境界)。要素番号ではない点に注意。
        // メッシュレットを生成していない(KurenaiPackerに--no-meshletsを指定した)場合は
        // Countがすべて0になり、ランタイムはメッシュシェーダー経路を使わない
        uint64_t MeshletOffset;            // MeshletEntry配列の先頭
        uint64_t MeshletVertexOffset;      // uint32_t配列(このメッシュの頂点バッファへのインデックス)の先頭
        uint64_t MeshletTriangleOffset;    // uint32_t配列(三角形1つにつき1要素)の先頭

        // 【v10でこの3つの意味が変わった】いずれも**全LOD段を合計した**ブロックの要素数で、
        // GPUバッファの確保と境界検証がこれを使う。
        // **描画に使う「LOD0のメッシュレット数」はMeshletLODCounts[0]のほう**であり、
        // MeshletCountをそのまま描くと全段が重なって描かれる
        uint32_t MeshletCount;
        uint32_t MeshletVertexCount;       // 全メッシュレットのVertexCountの総和(全段)
        uint32_t MeshletTriangleCount;     // 全メッシュレットのTriangleCountの総和(全段)

        // === メッシュレットの離散LOD(v10で追加) ===
        //
        // 段ごとに独立したメッシュレット群を持ち、増幅シェーダーが1段だけ選ぶ。
        // NaniteのようなDAG(段をまたいで継ぎ目なく混ぜられる階層)は作らない ――
        // 実装量が桁で小さく、街のタイルに要る精度にはこれで足りるため。
        //
        // **全段が同じ頂点バッファを共有する。** meshopt_simplifyは頂点を増やさず
        // インデックスを削るだけなので、段が増えても頂点は1つも増えない。
        //
        // **.kgeomのインデックスブロックはLOD0の三角形だけ**で、簡略化した段の三角形は
        // 入らない。レイトレーシングの三角形番号→メッシュレットの二分探索も、
        // メッシュシェーダーを使わない従来の描画経路も、v9とまったく同じ形のまま動く。
        //
        // MeshletLODOffsetsはMeshletEntry配列内の**要素番号**(MeshletOffsetからの相対)。
        // LOD0は必ず0から始まり、段は詳細な順に並ぶ。MeshletLODCount未満の段だけが有効
        uint32_t MeshletLODCount;          // 1〜kMaxMeshletLODCount。メッシュレット無しなら0
        uint32_t MeshletLODOffsets[kMaxMeshletLODCount];
        uint32_t MeshletLODCounts[kMaxMeshletLODCount];
    };
    static_assert(sizeof(MeshEntry) == 128, "MeshEntryのレイアウトは128バイト固定");

    constexpr int32_t kNoTextureIndex = -1;
    constexpr uint32_t kMeshEntryFlagTransparent = 1u << 0;

    // glTFのocclusionTexture.strengthの既定値。ソースデータが値を持たない場合にパッカーが書き出す
    constexpr float kDefaultOcclusionStrength = 1.0f;

    // マテリアルの係数がソースデータに存在しなかったことを表す無効値。
    // 実在する係数の値域は[0,1]なので、負値であれば「データに無い」と一意に判別できる。
    // KurenaiPackerは、ソースモデルが持っていない係数に対してもっともらしい既定値を勝手に
    // 埋めることはせず、この無効値をそのまま書き出す(何を既定値とするかはフォーマットや
    // アセットによって異なり、パッカーが決めてよい値ではないため)。
    // 無効値をどう解釈するかは消費側の責任で、シェーダーは係数1.0(テクスチャの値をそのまま使う)
    // として扱う
    constexpr float kInvalidMaterialFactor = -1.0f;

    // === LightEntry (モデルファイル埋め込みのライト) ===
    //
    // glTFのKHR_lights_punctual拡張やFBXのライトノードなど、モデルファイル自体が持つライト情報。
    // Assets::Light/LightType(Model.h)のPOD部分と1対1対応する。TypeはAssets::LightTypeの値
    // (0=Directional, 1=Point, 2=Spot)をそのまま格納する

    struct LightEntry
    {
        uint32_t Type;
        float    Position[3];
        float    Direction[3];             // 正規化済み。Point以外で意味を持つ
        float    Color[3];                 // 線形色。最大成分が1になるよう正規化済み
        float    Intensity;                // Point/SpotはカンデラCd、Directionalはルクスlx
        float    Range;
        float    SpotInnerConeAngle;       // ラジアン。Spotのみ
        float    SpotOuterConeAngle;
        uint32_t Enabled;                  // bool(0/1)
        uint32_t NameOffset;               // StringPool内オフセット。ImGui一覧の表示名(aiLight::mName由来)
        uint32_t NameLength;
        uint32_t Reserved;                 // 0固定
    };
    static_assert(sizeof(LightEntry) == 72, "LightEntryのレイアウトは72バイト固定");

    // === .kgeom (ジオメトリ実体) ===
    //
    // ファイルレイアウト:
    //   [GeometryHeader]
    //   [Payload (PayloadSize bytes)]
    //
    // ペイロードはMeshEntryの並び順に、メッシュごとの
    // [頂点ブロック][インデックスブロック][メッシュレットブロック]
    // [メッシュレット頂点ブロック][メッシュレット三角形ブロック] を連結したもの。
    // 各ブロックの先頭は16バイト境界(パディングは0埋め)。頂点はVertex.hのレイアウト
    // そのまま、インデックスはuint32_t生配列そのままで、読み込み後の加工は一切不要
    // (memcpy相当でそのままGPUバッファへ渡せる)。圧縮は行わない。
    //
    // 【インデックスの並びはメッシュレット順】v3から、インデックスブロックの三角形は
    // メッシュレットの並び順そのものになっている。こうしておくと、レイトレーシングが
    // ヒットしたグローバル三角形番号から所属メッシュレットを二分探索だけで引ける
    // (三角形→メッシュレットの逆引きテーブルを別に持たずに済む)。
    // ラスタライズ側も、メッシュシェーダーを使わない従来経路では
    // このインデックスバッファをそのまま描くだけでよい。
    // 並べ替えの前にmeshopt_optimizeVertexCache/VertexFetchを通しているため、
    // 従来経路の頂点キャッシュ効率が落ちることはない。

    constexpr char kGeometryMagic[4] = { 'K', 'G', 'E', 'O' };
    // v2: Vertex(Vertex.h)へライトマップUV(UV1)を追加し、頂点ストライドが48→56バイトへ
    // 変わったため加算
    // v3: メッシュレットの3ブロックをペイロードへ追加し、インデックスの並びを
    // メッシュレット順へ変更したため加算(上のコメント参照)
    // v4: MeshletEntryが48→64バイトになり、メッシュレット3ブロックが離散LODの全段を
    // 連結して持つようになったため加算(kPackageVersionも同時にv10へ)。
    // **インデックスブロックはLOD0の三角形だけ**で、v3と同じ内容・同じ並びのまま
    constexpr uint32_t kGeometryVersion = 4;

    struct GeometryHeader
    {
        char     Magic[4];                // 'K','G','E','O' (kGeometryMagic)
        uint32_t Version;                 // kGeometryVersion
        uint32_t VertexStride;            // sizeof(Vertex)
        uint32_t IndexStride;             // sizeof(uint32_t)
        uint64_t PayloadSize;             // ヘッダ以降のバイト数
        uint64_t Reserved;                // 0固定
    };
    static_assert(sizeof(GeometryHeader) == 32, "GeometryHeaderのレイアウトは32バイト固定");

    // ジオメトリブロック(頂点・インデックス・メッシュレットの各ブロック)を整列させる境界バイト数
    constexpr uint64_t kGeometryBlockAlignment = 16;

    // === メッシュレット ===
    //
    // メッシュを「頂点64個・三角形124個まで」の塊へ分割したもの。分割はKurenaiPackerが
    // meshoptimizer(meshopt_buildMeshlets)で行い、ランタイムはそのまま読むだけ。
    //
    // 三角形はメッシュ全体の頂点バッファを直接は指さず、2段の間接参照で指す:
    //   ローカル頂点番号 = MeshletTriangleブロック[TriangleOffset + t] の下位24bitから3つ取り出す
    //   グローバル頂点番号 = MeshletVertexブロック[VertexOffset + ローカル頂点番号]
    // こうすると三角形あたりのインデックスが1バイト×3で済み(1メッシュレットの頂点は64個までなので
    // 8bitに収まる)、メッシュシェーダーが少ないバイト数でジオメトリを読める。
    //
    // HLSL側の対応(Shaders/3D/GBufferMeshlet.hlslのMeshlet):
    //   struct Meshlet { uint VertexOffset; uint TriangleOffset; uint VertexCount; uint TriangleCount;
    //                    float3 BoundsCenter; float BoundsRadius; float3 ConeAxis; float ConeCutoff;
    //                    uint MaterialIndex; uint LODLevel; uint2 Reserved; };
    struct MeshletEntry
    {
        // MeshletVertexブロック内の要素オフセット(このメッシュのブロック先頭からの相対)
        uint32_t VertexOffset;
        // MeshletTriangleブロック内の要素(=三角形)オフセット。同じくメッシュ内の相対
        uint32_t TriangleOffset;
        uint32_t VertexCount;              // ≤ kMeshletMaxVertices
        uint32_t TriangleCount;            // ≤ kMeshletMaxTriangles

        // 錐台カリング用のバウンディング球(モデルのローカル空間)
        float    BoundsCenter[3];
        float    BoundsRadius;

        // 背面カリング用の法線コーン。この塊に含まれる三角形の法線が
        // 「軸ConeAxisを中心とする半頂角acos(ConeCutoff)の円錐」に収まることを表す。
        // 平行投影なら、視線方向をvとして dot(v, ConeAxis) >= ConeCutoff ですべて背面。
        //
        // 【apexを持たない】meshopt_computeMeshletBoundsは円錐の頂点(cone_apex)も返し、
        // 透視投影ではそれを使うほうが厳密になる。ここで持たないのは、apexを使う判定が
        // 「頂点からメッシュレットへのベクトル」を要求してレジスタと計算を増やすため。
        //
        // 【apexの代わりに球中心を使うなら、半径の項が要る】meshoptimizer.h が挙げる
        // 「apexを使わず境界球で代用する式」は
        //
        //     dot(center - camera, ConeAxis) >= ConeCutoff * length(center - camera) + BoundsRadius
        //
        // で、**apex用の式に中心を差し替えただけでは成立しない。** `+ BoundsRadius` を
        // 落とすとしきい値が本来より小さくなり、まだ表を向いている面がある塊まで落ちる。
        // 抜けた項は radius/距離 なのでカメラが近いほど効き、遠景では気づけない
        // (実装史69章。Bistroの立て看板の木枠が丸ごと消えていた)
        float    ConeAxis[3];
        float    ConeCutoff;               // = cos(角度/2)

        // === v10で追加 ===

        // このメッシュレットを描く材質(MaterialEntry配列への番号)。所属メッシュのものと同じ値。
        //
        // 【メッシュレットは1つの材質の三角形だけで構成される】KurenaiPackerはマテリアル単位で
        // メッシュを結合してからメッシュごとにmeshopt_buildMeshletsを呼ぶため、
        // 1つのメッシュレットに2つの材質が混ざることは構造上起こらない。
        // それでも番号を持たせるのは、モデル全体のメッシュレット総覧を1回のDispatchMeshで
        // 回すとき、メッシュ単位の定数バッファが使えなくなるため
        uint32_t MaterialIndex;

        // 何段目のLODに属するか。0が原寸。MeshEntryのMeshletLODOffsets/Countsと必ず整合する
        // (どちらか一方だけを見ても段を特定できるようにしてある。読み込み時の検算に使える)
        uint32_t LODLevel;

        uint32_t Reserved[2];              // 0固定。HLSL側のMeshletと64バイトで揃えるため
    };
    static_assert(sizeof(MeshletEntry) == 64, "HLSL側のMeshletと一致させるため64バイト固定");

    // 1メッシュレットあたりの上限。メッシュシェーダーの1スレッドグループが出力できる
    // 頂点・プリミティブの上限(D3D12はどちらも256)に収まる範囲で、GPUベンダーが推奨する値。
    //
    // 【頂点64】ローカル頂点番号が8bitに収まる(=三角形1つを3バイトで表せる)境目でもある。
    // 【三角形124】128ではなく124なのは、meshoptimizerが4の倍数を推奨しているのと、
    // メッシュレットあたりのインデックスデータを4バイト境界へ収めるため
    constexpr uint32_t kMeshletMaxVertices = 64;
    constexpr uint32_t kMeshletMaxTriangles = 124;

    // meshopt_buildMeshletsのcone_weight。0だとクラスタを詰め込むことだけを優先し、
    // 1だと法線コーンの狭さだけを優先する。0.5はmeshoptimizerのドキュメントが挙げる
    // バランスの取れた値で、背面カリングの効きとメッシュレット数の増加が釣り合う
    constexpr float kMeshletConeWeight = 0.5f;

    // MeshletTriangleブロックの1要素。ローカル頂点番号3つを下位24bitへ詰める
    // (i0 | i1<<8 | i2<<16)。上位8bitは0固定。
    //
    // 【3バイトではなく4バイトにする理由】meshoptimizerが返すのはunsigned char 3つで、
    // そのまま詰めれば三角形あたり3バイトで済む。それでも1バイト増やしているのは、
    // このエンジンのRHIがByteAddressBufferを持たずStructuredBufferしか無いため。
    // 3バイト詰めを読むにはバイト単位のアドレッシングが要り、RHI・DX11/DX12両実装・
    // ディスクリプタ周りに新しい種類のバッファを通す必要がある。
    // ドラゴン(87万三角形)で増えるのは0.9MBに過ぎず、割に合わない
    inline constexpr uint32_t PackMeshletTriangle(uint32_t i0, uint32_t i1, uint32_t i2)
    {
        return (i0 & 0xFFu) | ((i1 & 0xFFu) << 8) | ((i2 & 0xFFu) << 16);
    }

    // === .ktex (テクスチャ実体) ===
    //
    // ファイルレイアウト:
    //   [PackedTextureHeader]
    //   [Payload (PayloadSize bytes) = DDSファイル全体]
    //
    // ペイロードはDirectX::SaveToDDSMemoryが出力したDDSファイル全体('DDS 'マジック付き)。
    // ミップチェーン生成済み・BC7圧縮済み(BC7_UNORM_SRGB または BC7_UNORM)。
    // 元がDDS/TGAの場合は、既に圧縮・ミップ済みの配布形式として扱いそのままペイロードへ格納する
    // (TextureImage::LoadFromFileの拡張子判定を踏襲。BC7再圧縮・ミップ生成は行わない)。
    //
    // **SourceFileTime/SourceFileSizeのような元ファイル依存の情報は持たない。**
    // .ktexは元画像が無くても成立する配布可能なアセットであり(自動失効キャッシュではない)、
    // 無効化は「パッカーを再実行するか」というビルド上の判断に一本化される。

    constexpr char kPackedTextureMagic[4] = { 'K', 'T', 'E', 'X' };
    constexpr uint32_t kPackedTextureVersion = 1;

    struct PackedTextureHeader
    {
        char     Magic[4];                // 'K','T','E','X' (kPackedTextureMagic)
        uint32_t Version;                 // kPackedTextureVersion
        uint32_t Flags;                   // bit0: sRGB
        uint32_t Reserved;                // 0固定
        uint64_t PayloadSize;             // 以降のDDSバイト数
    };
    static_assert(sizeof(PackedTextureHeader) == 24, "PackedTextureHeaderのレイアウトは24バイト固定");

    constexpr uint32_t kPackedTextureFlagSRGB = 1u << 0;
}
