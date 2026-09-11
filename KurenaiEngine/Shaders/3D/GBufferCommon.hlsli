// GBuffer.hlsl/Water.hlslが共有する頂点シェーダー・構造体・cbuffer宣言。
//
// 両者は「頂点変換・G-Bufferへの書き込み方の枠組み」が完全に同一で、違いはPSMain内の
// 法線の求め方とMaterial.aへ書くマテリアルIDだけ(水面マテリアル基盤)。
// そのため頂点シェーダーVSMain・入出力構造体・cbuffer宣言をこのヘッダーへ括り出し、
// GBuffer.hlsl/Water.hlslはそれぞれのPSMainだけを持つようにしている。
#ifndef KURENAI_GBUFFER_COMMON_HLSLI
#define KURENAI_GBUFFER_COMMON_HLSLI

#include "Bindless.hlsli"
#include "Meshlet.hlsli"
#include "NormalEncoding.hlsli"
#include "TangentFrame.hlsli"
#include "Samplers.hlsli"

// G-BufferのMaterial.aに書き込むマテリアル種別ID。0=通常マテリアル(GBuffer.hlslのPSMainが
// そのまま0.0fを書く)、1=水面。将来SSR.hlsl側で水面の反射統合を行う際にもこの値を
// 参照する予定のため、値を変える場合は参照側(SSR.hlsl)も必ず同時に直すこと
static const float kMaterialIDWater = 1.0f;

#include "ShaderInterop/FrameConstants.hlsli"

#include "ObjectConstants.hlsli"

// 画面座標から作る決定的なノイズ(0〜1)。Tonemap.hlslが同名の関数を持つが、あちらは
// バンディングを散らすためのもので用途が別。ここはLOD切替のクロスディザ専用
float LODDitherNoise(float2 position)
{
    return frac(52.9829189f * frac(dot(position, float2(0.06711056f, 0.00583715f))));
}

// LOD切替のクロスディザで捨てる画素をclipする。DitherFadeが1.0(既定)なら何もしない。
//
// 【深度プリパスとG-Bufferで必ず同じものを呼ぶこと】片方だけが捨てると、
// 深度は書かれているのに色が書かれない画素ができて穴が開く
void ApplyLODDither(float2 screenPosition)
{
    if (DitherFade < 1.0f)
    {
        const float noise = LODDitherNoise(screenPosition);
        // 【境界を半開区間にする】clipは負のときだけ捨てるので、素直に書くと
        // noise == |DitherFade| ちょうどの画素で「先」と「元」の両方が生き残り、
        // その画素だけZファイティングになる。「元」の側をわずかに厳しくして、
        // 先: noise <= f / 元: noise > f と分けきる。
        // ずらす量は画素の分け前を目に見えて変えない大きさにしてある
        const float kBoundaryEpsilon = 1e-5f;
        clip(DitherFade >= 0.0f ? (DitherFade - noise) : (noise + DitherFade - kBoundaryEpsilon));
    }
}

// Assets::GpuMaterial(80バイト、Source/Library/Assets/Model.h)と1対1で対応。
// **並びとサイズを一致させること。**
//
// 【構造化バッファは詰めて並ぶ】定数バッファと違い、StructuredBuffer<T>のTは
// C++と同じ「メンバの型のアラインメントに従った詰めた配置」になる
// (GBufferMeshlet.hlslのMeshVertexのコメントと同じ)
struct GpuMaterial
{
    float4 BaseColorFactor;
    float3 EmissiveFactor;
    float MetallicFactor;
    float RoughnessFactor;
    float AlphaCutoff;
    float OcclusionStrength;
    float Translucency;
    uint BaseColorTextureIndex;
    uint NormalTextureIndex;
    uint MetallicRoughnessTextureIndex;
    uint EmissiveTextureIndex;
    uint OcclusionTextureIndex;
    uint BentNormalTextureIndex;
    uint Flags;
    uint Padding;
};

// PSInput::MaterialIndexに入る「マテリアルテーブルを使わない」ことを表す番号。
// 頂点シェーダー経路(VSMain)がこれを書く
static const uint kInvalidMaterialIndex = 0xFFFFFFFFu;

Texture2D BaseColorTexture : register(t0);
Texture2D NormalTexture : register(t1);
Texture2D MetallicRoughnessTexture : register(t2);
Texture2D EmissiveTexture : register(t3);
// ベイク済みアンビエントオクルージョン(遮蔽マップ)。赤チャンネルが遮蔽率(1=遮蔽なし)。
// t4はTransparent.hlsl/ProbeCapture.hlslがカスケードシャドウマップ配列に使っているため、
// マテリアルテクスチャを読む3パスで共通して空いている最初のスロットがt5になる
Texture2D OcclusionTexture : register(t5);

struct VSInput
{
    float3 Position : POSITION;
    float3 Normal : NORMAL;
    float2 UV : TEXCOORD0;
    float4 Tangent : TANGENT;
    // ライトマップUV(Assets::Vertex::UV1)。遮蔽マップ専用で、重なりが無く[0,1]に収まる
    float2 LightmapUV : TEXCOORD1;
};

struct PSInput
{
    float4 Position : SV_POSITION;
    float3 Normal : NORMAL;
    float3 WorldPos : TEXCOORD1;
    float2 UV : TEXCOORD0;
    float4 Tangent : TANGENT;
    float2 LightmapUV : TEXCOORD2;
    // モーションベクター用のクリップ空間座標。SV_POSITIONはラスタライザがw除算と
    // ビューポート変換を済ませた値になってしまい元のwが取れないため、別途そのまま渡す。
    // w除算は必ずPS側で行うこと(VS側で割ってから補間すると、遠近補正が効かず
    // 三角形の内側でずれる)
    float4 CurClip : TEXCOORD3;
    float4 PrevClip : TEXCOORD4;
    // このピクセルを出したメッシュレットの番号。メッシュシェーダー経路
    // (GBufferMeshlet.hlsl)でだけ実際の番号が入り、従来の頂点シェーダー経路では
    // kInvalidMeshletIndexになる。
    //
    // 三角形の中では一定なのでnointerpolation(補間すると意味を成さない整数が混ざる)。
    // 通常のPSMainは読まず、メッシュレットの分かれ方を目で確かめるデバッグ表示
    // (GBufferMeshlet.hlslのPSMainMeshletDebug)だけが使う
    nointerpolation uint MeshletIndex : TEXCOORD5;
    // このピクセルのマテリアル番号(Model::MaterialTableBufferの添字)。
    // メッシュシェーダー経路でだけ実際の番号が入り、従来の頂点シェーダー経路では
    // kInvalidMaterialIndexになる。
    //
    // 【プリミティブ属性ではなく頂点属性でよい】メッシュレットは必ず1つのマテリアルの
    // 三角形だけで構成される(KurenaiPackerがmeshopt_buildMeshletsをメッシュごとに
    // 呼んでいるため)。1つの塊の中では全頂点で同じ値になるので、nointerpolationが
    // 拾う先頭頂点の値がそのまま正しい。上のMeshletIndexとまったく同じ形
    nointerpolation uint MaterialIndex : TEXCOORD6;
};

struct PSOutput
{
    float4 Albedo : SV_TARGET0;
    float2 Normal : SV_TARGET1;
    float4 Material : SV_TARGET2;
    float4 Emissive : SV_TARGET3;
    // モーションベクター(この画素の中身が前フレームから今フレームまでに動いた量、UV単位)
    float2 Velocity : SV_TARGET4;
    // bent normal(正規化しない可視方向の平均、ワールド空間)。.rgb = bRaw、.a = 有効フラグ。
    // R11G11B10_Floatは使えない ―― 符号なしのため負の成分が落ちる(34章)。
    // 【Water.hlslも必ず書くこと】この構造体はGBuffer.hlslとWater.hlslで共有しており、
    // 書き残すとそのターゲットの内容が未定義になる。水面はbent normalを焼いていないので
    // 有効フラグ0(=データ無し)を書き、消費側で従来の経路へ落とす
    float4 BentNormal : SV_TARGET5;
};

// クリップ空間座標を画面UV([0,1]、左上原点)へ変換する。
// NDCのyは上が+1・下が-1なのに対しUVのvは上が0・下が1なので、yだけ符号を反転する
float2 ClipToUv(float4 clipPos)
{
    return (clipPos.xy / clipPos.w) * float2(0.5f, -0.5f) + 0.5f;
}

PSInput VSMain(VSInput input, uint instanceID : SV_InstanceID)
{
    PSInput output;
    // このインスタンスの変換。インスタンシングが無効なドローでは
    // ObjectConstantsのWorld/NormalMatrix/TangentSignFlipがそのまま返る
    const ModelInstanceRecord instance = FetchModelInstance(instanceID);
    float3 worldPos = mul(float4(input.Position, 1.0f), instance.World).xyz;
    output.Position = mul(float4(worldPos, 1.0f), ViewProj);
    output.Normal = mul(input.Normal, (float3x3)instance.NormalMatrix);
    output.WorldPos = worldPos;
    output.UV = input.UV;
    output.LightmapUV = input.LightmapUV;
    // 接線は面上の方向ベクトルなので、法線と異なりinverse-transposeではなく
    // Worldの3x3部分そのままで変換する(位置と同じ変換)
    output.Tangent =
        float4(mul(input.Tangent.xyz, (float3x3)instance.World), input.Tangent.w * instance.TangentSignFlip);

    output.CurClip = output.Position;
    // 前フレームの投影位置。現在のシーンは全インスタンスが静的(ModelInstance::Worldは
    // 読み込み時に確定し以降変わらない)なので、前フレームのワールド座標は今と同じでよく、
    // 違うのはカメラ由来のビュー射影行列だけになる。動的オブジェクトを入れる場合は
    // ObjectConstantsへPrevWorldを追加し、input.Positionをそちらで変換してからここへ渡すこと
    output.PrevClip = mul(float4(worldPos, 1.0f), PrevViewProj);
    // この経路はメッシュレットを経由していない(GBufferCommon.hlsliのPSInput参照)
    output.MeshletIndex = kInvalidMeshletIndex;
    // マテリアルテーブルも使わない。t0〜t6と定数バッファから読む従来経路
    output.MaterialIndex = kInvalidMaterialIndex;
    return output;
}

// マテリアルのテクスチャを1枚サンプルする。
//
// bindless番号が有効ならResourceDescriptorHeapから、無効なら固定スロット(t0〜t6)へ
// バインドされているテクスチャから読む。番号が無効になるのは
//   (1) 従来のメッシュ単位の経路(LoadSurfaceMaterialが無効値を入れる)
//   (2) bindless非対応環境
//   (3) bindless区画が満杯で登録に失敗した(ログにエラーが出ている)
// のいずれか。
//
// 【SampleLevelではなくSample】ラスタライズ経路には隣接ピクセルとのUV勾配があるので、
// ミップはハードウェアに選ばせる。SampleLevelでLOD 0に固定すると遠景がちらつく。
//
// 【分岐の中でSampleしてよい理由】暗黙の微分はピクセルクアッド単位で取られる。
// 1つのクアッドの4画素は必ず同じ三角形=同じメッシュレット=同じマテリアルから来るので、
// この分岐はクアッド内で必ず一様であり、勾配が壊れることはない
float4 SampleMaterialTexture(uint bindlessIndex, Texture2D fallbackTexture, float2 uv)
{
#if defined(KURENAI_BINDLESS)
    if (bindlessIndex != kInvalidBindlessIndex)
    {
        // NonUniformResourceIndexが要る理由はBindless.hlsliのコメント参照
        Texture2D<float4> tex = ResourceDescriptorHeap[NonUniformResourceIndex(bindlessIndex)];
        return tex.Sample(MaterialSampler, uv);
    }
#endif
    return fallbackTexture.Sample(MaterialSampler, uv);
}

// このピクセルのマテリアルを読み出す。
//
// 1モデル1ドローの経路(materialIndexが有効)ではマテリアルテーブルから、
// 従来のメッシュ単位の経路では定数バッファ(ObjectConstants)から読む。
// **1本のピクセルシェーダーが両方を賄う**ため、呼び出し側に#ifは要らない。
//
// テクスチャ番号は、テーブル経路ならbindless番号、従来経路では
// kInvalidBindlessIndex(=「t0〜t6を使え」の意味)になる
GpuMaterial LoadSurfaceMaterial(uint materialIndex)
{
    GpuMaterial material;

#if defined(KURENAI_BINDLESS)
    if (materialIndex != kInvalidMaterialIndex && MaterialTableIndex != kInvalidBindlessIndex)
    {
        StructuredBuffer<GpuMaterial> materials = KURENAI_BINDLESS_BUFFER(MaterialTableIndex);
        return materials[materialIndex];
    }
#endif

    material.BaseColorFactor = BaseColorFactor;
    material.EmissiveFactor = EmissiveFactor;
    material.MetallicFactor = MetallicFactor;
    material.RoughnessFactor = RoughnessFactor;
    material.AlphaCutoff = AlphaCutoff;
    material.OcclusionStrength = OcclusionStrength;
    material.Translucency = Translucency;
    // 無効番号 = 「bindlessではなく従来のt0〜t6から引け」
    material.BaseColorTextureIndex = kInvalidBindlessIndex;
    material.NormalTextureIndex = kInvalidBindlessIndex;
    material.MetallicRoughnessTextureIndex = kInvalidBindlessIndex;
    material.EmissiveTextureIndex = kInvalidBindlessIndex;
    material.OcclusionTextureIndex = kInvalidBindlessIndex;
    material.BentNormalTextureIndex = kInvalidBindlessIndex;
    material.Flags = 0;
    material.Padding = 0;
    return material;
}

// ComputeTangentFrameはTangentFrame.hlsliにある(このファイルの先頭で#includeしている)。
// 反射プローブのキャプチャ・平面反射・半透明フォワードも同じものを使う

#endif // KURENAI_GBUFFER_COMMON_HLSLI
