#pragma once

#include <memory>
#include <string>
#include <vector>

#include "RHI/IRHIBuffer.h"
#include "RHI/IRHITexture.h"

#include "RaytracingGeometry.h"

namespace Kurenai::Assets
{
    struct Mesh
    {
        std::unique_ptr<RHI::IRHIBuffer> VertexBuffer;
        std::unique_ptr<RHI::IRHIBuffer> IndexBuffer;
        uint32_t IndexCount = 0;
        // VertexBufferに入っている頂点数。描画はインデックス経由なので不要だが、
        // BLASの構築(D3D12_RAYTRACING_GEOMETRY_DESC::Triangles.VertexCount)に必要
        uint32_t VertexCount = 0;
        // レイトレーシング用: Model::RaytracingAttributes / RaytracingIndices の中で
        // このメッシュのデータが始まる位置。RaytracingSceneがシーン全体の統合バッファを
        // 組み立てる際の連結元として使う。
        // デバイスがレイトレーシング非対応の場合は両配列とも空で、この値は意味を持たない
        uint32_t RaytracingAttributeOffset = 0;
        uint32_t RaytracingIndexOffset = 0;
        // 同じくModel::RaytracingMeshletTriangleOffsets内でこのメッシュのデータが始まる位置。
        // 要素数はMeshletCount(メッシュレットを持たない.kmodelでは0)
        uint32_t RaytracingMeshletOffset = 0;

        // --- メッシュレット(メッシュシェーダー用) ---------------------------------------
        //
        // KurenaiPackerが焼いた分割情報(Assets::MeshletEntry)と、その2段の間接参照テーブル。
        // 増幅シェーダーがMeshletBufferのバウンディング球・法線コーンでカリングし、
        // メッシュシェーダーが生き残ったメッシュレットの頂点/三角形を組み立てる。
        //
        // 3本ともBufferUsage::StructuredImmutable。シーン読み込み時に一度書いたら
        // 変わらないため、ステージングリングを持たないこのUsageがそのまま当てはまる。
        //
        // 【空になる条件】(1) デバイスがメッシュシェーダー非対応、(2) .kmodelが
        // --no-meshletsで焼かれている、のいずれか。
        // 描画側はMeshletCountではなくMeshletBufferの有無で経路を選ぶこと ――
        // MeshletCountはアセットが持つメッシュレット数そのもので、メッシュシェーダー
        // 非対応の環境でも(レイトレーシング側が使うため)0にはならない
        std::unique_ptr<RHI::IRHIBuffer> MeshletBuffer;
        std::unique_ptr<RHI::IRHIBuffer> MeshletVertexBuffer;
        std::unique_ptr<RHI::IRHIBuffer> MeshletTriangleBuffer;
        // .kmodelが持つメッシュレット数。GPUバッファの有無とは独立
        uint32_t MeshletCount = 0;
        RHI::IRHITexture* BaseColorTexture = nullptr;
        RHI::IRHITexture* NormalTexture = nullptr;
        RHI::IRHITexture* MetallicRoughnessTexture = nullptr;
        RHI::IRHITexture* EmissiveTexture = nullptr;
        // ベイク済みアンビエントオクルージョン(遮蔽マップ)。glTFのocclusionTexture由来で、
        // 赤チャンネルを遮蔽率(1=遮蔽なし、0=完全遮蔽)として読む。未指定時は白1x1(=遮蔽なし)。
        // SSAO/SSILがスクリーンスペースの制約で拾えない、布の折り目や狭い隙間のような
        // 細部の遮蔽を補う。間接光(IBL・SSILの間接拡散光)にのみ効き、直接光と自発光には掛からない
        RHI::IRHITexture* OcclusionTexture = nullptr;
        // bent normal(正規化しない可視方向の平均、RGBA16F)。ライトマップUV空間で、
        // .rgb = bRaw、.a = 有効フラグ。未指定時は黒1x1(=.a が0 = データ無し)。
        //
        // 遮蔽マップが「どれだけ隠れているか」しか持たないのに対し、こちらは
        // 「どの方向が開いているか」を持つ。消費側が軸 normalize(bRaw)・aoB = length(bRaw)・
        // aoN = dot(N, bRaw) の3つへ分解し、スペキュラ遮蔽の方向依存(壁を向いた反射だけを
        // 暗くする)とディフューズの方向バイアスを扱う(34章)
        RHI::IRHITexture* BentNormalTexture = nullptr;
        float MetallicFactor = 0.0f;
        // ソースデータに値が無い場合はkInvalidMaterialFactor(負値)が入る。シェーダー側は
        // 負値を「係数の指定なし」とみなし1.0(テクスチャの値をそのまま使う)として扱う
        float RoughnessFactor = 0.0f;
        float EmissiveFactor[3] = { 0.0f, 0.0f, 0.0f };
        // 0以下ならアルファカットアウト無効(常に不透明)。glTFのalphaMode=MASKのマテリアルのみ
        // alphaCutoff(既定0.5)が設定される
        float AlphaCutoff = 0.0f;
        // 透過率(0=不透明、1=完全に透ける)。葉や花弁のように薄いものが、裏から当たった光を
        // 透かして表側が明るく見える量。0なら従来どおりの不透明な陰影になる。
        // KurenaiPackerの --translucent <マテリアル名>=<値> で設定する(45章)
        float Translucency = 0.0f;
        // glTFのalphaMode=BLENDのマテリアルのみtrue。GBufferパス(不透明)には描画されず、
        // 専用のTransparentパス(KurenaiEngine3D::Render参照)でカメラから遠い順にアルファブレンド
        // 合成される。AlphaCutoffとは排他(glTF仕様上alphaModeはOPAQUE/MASK/BLENDのいずれか1つ)
        bool IsTransparent = false;
        // glTFのpbrMetallicRoughness.baseColorFactor(RGBA、既定[1,1,1,1])。BaseColorTextureと
        // 乗算して使う。テクスチャを持たずbaseColorFactorのみで色/不透明度を表現するマテリアル
        // (ガラス等でよくあるパターン)は、これが無いとBaseColorTexture=nullptr時の白1x1
        // プレースホルダー(alpha=1)にフォールバックし、意図した色・アルファと異なる見た目になる。
        // 現状このフォールバック乗算はTransparentパス(Transparent.hlsl)のみが行い、GBufferパス
        // (不透明・MASK)は既存動作を変えないため引き続きテクスチャの色をそのまま使う
        float BaseColorFactor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        // glTFのocclusionTexture.strength(既定1.0)。シェーダーはlerp(1, ao, strength)で適用する。
        // 既定値はModelPackage.hのkDefaultOcclusionStrengthと同じ1.0だが、このヘッダーは
        // ディスク形式の定義に依存させたくないため定数を参照せず直接書いている
        float OcclusionStrength = 1.0f;
    };

    enum class LightType : uint32_t
    {
        Directional = 0,
        Point       = 1,
        Spot        = 2,
        // 3以降はエリアライト(球/チューブ/矩形)用に予約。今回は未実装
    };

    struct Light
    {
        LightType Type = LightType::Point;
        float Position[3]{ 0.0f, 0.0f, 0.0f };
        float Direction[3]{ 0.0f, -1.0f, 0.0f };  // 光が進む向き(正規化済み)。Spot/Directional で使用
        float Color[3]{ 1.0f, 1.0f, 1.0f };       // 線形色。最大成分が1になるよう正規化して保持する
        // 測光量。Point/Spot はカンデラ(cd = lm/sr)、Directional はルクス(lx = lm/m²)。
        // glTF は KHR_lights_punctual の値をそのまま格納する(物理単位として正確)。
        // FBX は物理単位を持たないため、DCC側のIntensity/100をカンデラ相当として近似する(ModelLoader参照)
        float Intensity = 1.0f;
        float Range = 10.0f;                      // 影響半径。Directional では未使用
        float SpotInnerConeAngle = 0.4f;          // ラジアン(軸からの半角)。Spot のみ
        float SpotOuterConeAngle = 0.6f;
        bool Enabled = true;
        // このライトがスクリーンスペースシャドウ(ScreenSpaceShadow.hlsli)を落とすか。
        // ピクセルあたりのシャドウレイ数には上限があるため、影を出したいライトへ予算を回せるよう
        // ライト単位で切れるようにしてある。
        //
        // 【永続化しない】このフラグは .kmodelcache の CachedLightRecord(POD)には含めない。
        // 含めるとレコードのレイアウトが変わり kPackageVersion の更新と全アセットの再パックが
        // 必要になるが、得られるのは「モデルファイルに埋め込まれたライトごとの既定値」だけで
        // 見合わない。モデル由来のライトは常にこの既定値(true)で読み込まれ、
        // .kscene の [Light] セクション(テキスト形式)と ImGui で上書きする運用とする
        bool CastShadow = true;
        std::string Name;                         // aiLight::mName 由来。ImGui 一覧の表示に使う
    };

    struct Model
    {
        std::vector<Mesh> Meshes;
        std::vector<std::unique_ptr<RHI::IRHITexture>> Textures;
        std::vector<Light> Lights;

        // レイトレーシングでヒット面の陰影を計算するための、このモデル全メッシュ分の
        // 頂点属性とインデックス(Mesh::RaytracingAttributeOffset / RaytracingIndexOffsetが
        // メッシュごとの開始位置を指す)。
        //
        // 描画用の頂点/インデックスバッファはGPU上にしか無く、シェーダーからは
        // 頂点バッファとして以外に読めない(SRVを持たない)ため、レイトレーシング側が
        // 参照できる形のコピーをModelLoaderが.kgeomの読み込み中に作る。
        //
        // 【寿命】RaytracingScene::Buildがシーン全体の統合バッファへ連結してGPUへ送った時点で
        // 用済みになるため、そこで解放される(数百万頂点のシーンでは100MB規模になるため、
        // 使い終わったCPU側コピーを抱え続けない)。
        // デバイスがレイトレーシング非対応の場合はそもそも構築されず空のまま
        std::vector<RaytracingVertexAttribute> RaytracingAttributes;
        std::vector<uint32_t> RaytracingIndices;
        // メッシュレットごとの「メッシュ内での開始三角形番号」(Mesh::RaytracingMeshletOffsetが
        // メッシュごとの開始位置を指す)。上の2つと同じくRaytracingScene::Buildが
        // シーン全体の統合バッファへ連結した時点で解放される。
        // 用途と、MeshletEntryをそのまま持たない理由はRaytracingScene::GetMeshletTriangleOffsetBuffer参照
        std::vector<uint32_t> RaytracingMeshletTriangleOffsets;

        float BoundsMin[3] = { 0.0f, 0.0f, 0.0f };
        float BoundsMax[3] = { 0.0f, 0.0f, 0.0f };
    };
}
