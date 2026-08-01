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
    //   [TextureEntry × TextureCount]
    //   [MeshEntry    × MeshCount]
    //   [LightEntry   × LightCount]
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
    // v5以前の.kmodelはVersion不一致で読み込み拒否され、KurenaiPackerの再実行で再生成される
    constexpr uint32_t kPackageVersion = 6;

    struct PackageHeader
    {
        char     Magic[4];                // 'K','M','D','L' (kPackageMagic)
        uint32_t Version;                 // kPackageVersion。不一致なら読み込み拒否
        uint32_t VertexStride;            // sizeof(Vertex)。不一致なら読み込み拒否
        uint32_t IndexStride;             // sizeof(uint32_t)。不一致なら読み込み拒否
        float    BoundsMin[3];            // このモデルのAABB(パック時に確定したローカル空間)
        float    BoundsMax[3];
        uint32_t MeshCount;
        uint32_t TextureCount;
        uint32_t LightCount;
        uint32_t GeometryPathOffset;      // StringPool内オフセット。対になる.kgeomの相対パス
        uint32_t GeometryPathLength;
        uint32_t StringPoolSize;
    };
    static_assert(sizeof(PackageHeader) == 64, "PackageHeaderのレイアウトは64バイト固定");

    struct TextureEntry
    {
        uint32_t PathOffset;              // StringPool内オフセット。.ktexへの相対パス
        uint32_t PathLength;
        uint32_t Flags;                   // bit0: sRGB(検証用。実体は.ktexのDXGIフォーマットが持つ)
        uint32_t Reserved;                // 0固定
    };
    static_assert(sizeof(TextureEntry) == 16, "TextureEntryのレイアウトは16バイト固定");

    constexpr uint32_t kTextureEntryFlagSRGB = 1u << 0;

    struct MeshEntry
    {
        uint64_t VertexOffset;            // .kgeomのペイロード先頭からのバイトオフセット(16B境界)
        uint64_t IndexOffset;             // 同上
        uint32_t VertexCount;
        uint32_t IndexCount;
        float    MetallicFactor;
        float    RoughnessFactor;
        // 0以下ならアルファカットアウト無効(常に不透明)。glTFのalphaMode=MASKのマテリアルのみ
        // alphaCutoff(既定0.5)が設定される
        float    AlphaCutoff;
        float    EmissiveFactor[3];
        int32_t  BaseColorTextureIndex;           // -1 = 指定なし → 白1x1
        int32_t  NormalTextureIndex;              // -1 = 指定なし → フラット法線
        int32_t  MetallicRoughnessTextureIndex;   // -1 = 指定なし → 白1x1
        // -1 = 指定なし → 白1x1(EmissiveFactorが0ならどのみち結果は黒になるため、
        // BaseColor等と同様に白のプレースホルダーへフォールバックしてよい)
        int32_t  EmissiveTextureIndex;
        uint32_t Flags;                    // bit0: 半透明(kMeshEntryFlagTransparent。glTFのalphaMode=BLEND)
        uint32_t Reserved;                 // 0固定(uint64_tメンバがあるため構造体全体が8バイト境界に
                                            // アラインされ、Flagsだけでは68バイトになり暗黙のパディングが
                                            // 発生してしまうため、明示的なフィールドとして72バイトに揃える)
        // glTFのpbrMetallicRoughness.baseColorFactor(RGBA、既定[1,1,1,1])。テクスチャの有無に
        // 関わらず常に設定され、BaseColorTextureIndex=-1の場合の白1x1プレースホルダーと乗算される
        // ことで、テクスチャを持たずbaseColorFactorのみで色/不透明度を表現するマテリアル(ガラス等)を
        // 正しく再現する(14章参照)
        float    BaseColorFactor[4];
        // ベイク済みアンビエントオクルージョン(遮蔽マップ)。glTFのocclusionTextureに対応し、
        // 赤チャンネルを遮蔽率(1=遮蔽なし、0=完全遮蔽)として読む。
        // -1 = 指定なし → 白1x1(=遮蔽なし。他のテクスチャと同様、シェーダー側に分岐を持たせず
        // プレースホルダーへフォールバックする方式)
        int32_t  OcclusionTextureIndex;
        // glTFのocclusionTexture.strength(既定1.0)。シェーダーはlerp(1, ao, strength)で適用する。
        // strengthはglTF仕様で既定値が1.0と明記されているため、ラフネス係数のような
        // kInvalidMaterialFactor(負値)方式は取らず、ソースに無ければ1.0を書き出す
        float    OcclusionStrength;
    };
    static_assert(sizeof(MeshEntry) == 96, "MeshEntryのレイアウトは96バイト固定");

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
    // ペイロードはMeshEntryの並び順に[頂点ブロック][インデックスブロック]を連結したもの。
    // 各ブロックの先頭は16バイト境界(パディングは0埋め)。頂点はVertex.hの48バイトレイアウト
    // そのまま、インデックスはuint32_t生配列そのままで、読み込み後の加工は一切不要
    // (memcpy相当でそのままGPUバッファへ渡せる)。圧縮は行わない。

    constexpr char kGeometryMagic[4] = { 'K', 'G', 'E', 'O' };
    // v2: Vertex(Vertex.h)へライトマップUV(UV1)を追加し、頂点ストライドが48→56バイトへ
    // 変わったため加算
    constexpr uint32_t kGeometryVersion = 2;

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

    // ジオメトリブロック(頂点ブロック・インデックスブロックそれぞれ)を整列させる境界バイト数
    constexpr uint64_t kGeometryBlockAlignment = 16;

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
    // .ktexcacheとの決定的な違い: SourceFileTime/SourceFileSizeを持たない。
    // .ktexcacheは元画像から派生する自動失効キャッシュだったが、.ktexは元画像が無くても
    // 成立する配布可能なアセットである。無効化は「パッカーを再実行するか」という
    // ビルド上の判断に一本化される。

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
