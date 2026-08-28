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

// GBuffer.hlslのObjectConstantsと同じレイアウト(不透明の描画ではWorldしか使わないが、
// 同じルートシグネチャ/定数バッファを共有するため並び順を合わせる)。
//
// 【AlphaCutoff以降まで宣言を伸ばしてある】アルファカットアウトのマテリアルは
// 切り抜きを反映しないと影の形が実物と食い違うため、下のPSMainCutoutが
// AlphaCutoff / BaseColorFactor と、1モデル1ドローの経路ではマテリアルテーブルの
// 番号まで読む。**並びを1つでもずらすと別の値を読む**ので、
// GBufferCommon.hlsli側を直したらここも直すこと
cbuffer ObjectConstants : register(b1)
{
    float4x4 World;
    float4x4 NormalMatrix;
    float MetallicFactor;
    float RoughnessFactor;
    float TangentSignFlip;
    float AlphaCutoff;
    float3 EmissiveFactor;
    float OcclusionStrength;
    float4 BaseColorFactor;
    float MaterialID;
    uint MeshletOffset;
    uint MeshletBufferIndex;
    uint MeshletVertexBufferIndex;
    uint MeshletTriangleBufferIndex;
    uint MeshletCount;
    float Translucency;
    uint MaterialTableIndex;
    uint MeshletFilterReject;
    uint MeshletFilterRequire;
    float EmissiveIntensity;
    float OcclusionMapScale;
};

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

PSInput VSMain(VSInput input)
{
    PSInput output;
    float3 worldPos = mul(float4(input.Position, 1.0f), World).xyz;
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

CutoutPSInput VSMainCutout(VSInputCutout input)
{
    CutoutPSInput output;
    float3 worldPos = mul(float4(input.Position, 1.0f), World).xyz;
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
