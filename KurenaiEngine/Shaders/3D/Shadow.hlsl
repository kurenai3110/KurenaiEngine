// シャドウパス: ライト視点から深度のみを描画する(頂点位置以外は不要)。
// カスケードシャドウマップ(CSM)のため、カスケードごとに1回ずつこのパスを実行し、
// その都度CascadeConstantsを該当カスケードのビュー・プロジェクション行列で更新して呼び出す。
// 共有のFrameConstantsとは別の専用バッファ(このシェーダはFrameConstantsを一切使わない)
#include "Bindless.hlsli"
#include "Samplers.hlsli"

cbuffer CascadeConstants : register(b0)
{
    float4x4 ViewProj;
};

#include "ObjectConstants.hlsli"

// Assets::GpuMaterial(80バイト)と1対1で対応。GBufferCommon.hlsliの同名の宣言と
// **並びとサイズを必ず一致させること**
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

// メッシュ単位の描画でバインドされるベースカラー。1モデル1ドローの経路では
// bindless番号で引くためこれは使わない(GBufferCommon.hlsliのt0と同じスロット)
Texture2D BaseColorTexture : register(t0);

// PSInput::MaterialIndexに入る「マテリアルテーブルを使わない」ことを表す番号。
// GBufferCommon.hlsliのkInvalidMaterialIndexと同じ値にすること
static const uint kShadowInvalidMaterialIndex = 0xFFFFFFFFu;

struct VSInput
{
    float3 Position : POSITION;
};

struct PSInput
{
    float4 Position : SV_POSITION;
};

PSInput VSMain(VSInput input, uint instanceID : SV_InstanceID)
{
    PSInput output;
    // このインスタンスの変換。インスタンシングが無効なドローでは
    // ObjectConstantsのWorld/NormalMatrix/TangentSignFlipがそのまま返る
    const ModelInstanceRecord instance = FetchModelInstance(instanceID);
    float3 worldPos = mul(float4(input.Position, 1.0f), instance.World).xyz;
    output.Position = mul(float4(worldPos, 1.0f), ViewProj);
    return output;
}

void PSMain(PSInput input)
{
    // 深度のみを書き込むためカラー出力は不要
}

// --- アルファカットアウト(glTFのalphaMode=MASK)用 -------------------------------------
//
// 【なぜ要るのか】このパスは長らくアルファを一切見ておらず、葉や柵のように
// テクスチャで切り抜く前提のマテリアルが、切り抜き前の板ポリゴンのまま影を落としていた。
// 木の下に矩形の影ができる、といった形で絵に出る。
//
// 【頂点シェーダーもメッシュシェーダーもここへ来る】入力構造体は
// ShadowMeshlet.hlslのShadowPSInputと同じ並び・同じセマンティクスにしてあり、
// 1本のピクセルシェーダーで両方の経路を賄う(G-Bufferと同じ考え方)。
// 頂点シェーダー経路はMaterialIndexにkShadowInvalidMaterialIndexを書き、
// 従来どおりt0と定数バッファから読む。
//
// 【判定式はGBuffer.hlslのPSMainと同一でなければならない】ここで抜いた部分と
// G-Bufferで抜く部分がずれると、葉の隙間から光が漏れたり、逆に無いはずの影が落ちたりする

struct VSInputCutout
{
    float3 Position : POSITION;
    float2 UV : TEXCOORD0;
};

struct CutoutPSInput
{
    float4 Position : SV_POSITION;
    float2 UV : TEXCOORD0;
    nointerpolation uint MaterialIndex : TEXCOORD1;
};

CutoutPSInput VSMainCutout(VSInputCutout input, uint instanceID : SV_InstanceID)
{
    CutoutPSInput output;
    // 【VSMainと同じくインスタンシングに対応させること】カットアウトのメッシュも
    // 不透明ぶんと同じバッチのまま DrawIndexed(..., instanceCount) で描かれる。
    // ここだけ定数バッファのWorldを使うと、バッチ内の全個体の影が
    // 代表インスタンスの位置に重なって落ちる
    const ModelInstanceRecord instance = FetchModelInstance(instanceID);
    float3 worldPos = mul(float4(input.Position, 1.0f), instance.World).xyz;
    output.Position = mul(float4(worldPos, 1.0f), ViewProj);
    output.UV = input.UV;
    // この経路はマテリアルテーブルを使わない(t0と定数バッファから読む)
    output.MaterialIndex = kShadowInvalidMaterialIndex;
    return output;
}

void PSMainCutout(CutoutPSInput input)
{
    float4 baseColorFactor = BaseColorFactor;
    float alphaCutoff = AlphaCutoff;
    float4 baseColorSample = BaseColorTexture.Sample(MaterialSampler, input.UV);

#if defined(KURENAI_BINDLESS)
    if (input.MaterialIndex != kShadowInvalidMaterialIndex && MaterialTableIndex != kInvalidBindlessIndex)
    {
        StructuredBuffer<GpuMaterial> materials = KURENAI_BINDLESS_BUFFER(MaterialTableIndex);
        const GpuMaterial material = materials[input.MaterialIndex];
        baseColorFactor = material.BaseColorFactor;
        alphaCutoff = material.AlphaCutoff;
        baseColorSample =
            BindlessSample(material.BaseColorTextureIndex, MaterialSampler, input.UV, baseColorSample);
    }
#endif

    clip(baseColorSample.a * baseColorFactor.a - alphaCutoff);
}
