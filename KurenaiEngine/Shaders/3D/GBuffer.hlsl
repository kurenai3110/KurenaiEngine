#include "NormalEncoding.hlsli"

cbuffer FrameConstants : register(b0)
{
    float4x4 ViewProj;
    float4x4 InvViewProj;
    // カスケードシャドウマップ用(このシェーダでは未使用。CameraPosition等のオフセットを
    // C++側のFrameConstantsに合わせるためだけに同じ配列サイズで宣言する)
    float4x4 CascadeViewProj[4];
    float4 CameraPosition;
    float4 LightDirection;
    float4 LightColor;
};

// メッシュ単位(将来的にはシーン上のモデルインスタンス単位)の情報。
// DX12のルートシグネチャがCBVをb0/b1の2枠しか持たないため、モデル行列もここへ同居させている
cbuffer ObjectConstants : register(b1)
{
    float4x4 World;
    // Worldの3x3部分の逆転置(4x4に格納)。回転+非一様スケールで法線が歪むのを防ぐため、
    // 位置と同じWorldではなくこちらを法線の変換に使う(Architecture.html「法線マッピングの
    // 接線ベクトル計算」参照)
    float4x4 NormalMatrix;
    float MetallicFactor;
    float RoughnessFactor;
    // Worldの行列式が負(ミラーリングを含む非一様スケール)の場合は-1。従法線の向きが
    // 反転するため、頂点接線のw成分(従法線の向き)に掛け合わせて補正する
    float TangentSignFlip;
    // 0以下ならアルファカットアウト無効(常に不透明として扱う)。glTFのalphaMode=MASKの
    // マテリアルのみalphaCutoff(既定0.5)が設定される
    float AlphaCutoff;
    float3 EmissiveFactor;
    float ObjectPadding;
};

Texture2D BaseColorTexture : register(t0);
Texture2D NormalTexture : register(t1);
Texture2D MetallicRoughnessTexture : register(t2);
Texture2D EmissiveTexture : register(t3);
SamplerState DefaultSampler : register(s0);

struct VSInput
{
    float3 Position : POSITION;
    float3 Normal : NORMAL;
    float2 UV : TEXCOORD0;
    float4 Tangent : TANGENT;
};

struct PSInput
{
    float4 Position : SV_POSITION;
    float3 Normal : NORMAL;
    float3 WorldPos : TEXCOORD1;
    float2 UV : TEXCOORD0;
    float4 Tangent : TANGENT;
};

struct PSOutput
{
    float4 Albedo : SV_TARGET0;
    float2 Normal : SV_TARGET1;
    float4 Material : SV_TARGET2;
    float4 Emissive : SV_TARGET3;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    float3 worldPos = mul(float4(input.Position, 1.0f), World).xyz;
    output.Position = mul(float4(worldPos, 1.0f), ViewProj);
    output.Normal = mul(input.Normal, (float3x3)NormalMatrix);
    output.WorldPos = worldPos;
    output.UV = input.UV;
    // 接線は面上の方向ベクトルなので、法線と異なりinverse-transposeではなく
    // Worldの3x3部分そのままで変換する(位置と同じ変換)
    output.Tangent = float4(mul(input.Tangent.xyz, (float3x3)World), input.Tangent.w * TangentSignFlip);
    return output;
}

// 頂点接線(xyz)と従法線の向き(w = +1/-1)からTBN行列を構築する。
// UV/位置の画面空間微分(ddx/ddy)から近似する手法は、UV継ぎ目(シームがある円筒状展開の
// グラス類など)でピクセルクアッドがトポロジー的に不連続になり法線が破綻するため使用しない
float3x3 ComputeTangentFrame(float3 N, float4 tangent)
{
    // 頂点補間でTとNの直交性が崩れるため、ピクセル単位でGram-Schmidt再直交化する
    float3 T = normalize(tangent.xyz - N * dot(N, tangent.xyz));
    float3 B = cross(N, T) * tangent.w;
    return float3x3(T, B, N);
}

PSOutput PSMain(PSInput input)
{
    float4 baseColorSample = BaseColorTexture.Sample(DefaultSampler, input.UV);

    // AlphaCutoff<=0(アルファカットアウト無効)の場合、alpha(0〜1)は常にAlphaCutoff以上になるため
    // clipは発火しない。AlphaCutoff>0の場合のみ、alphaがそれを下回るピクセルを破棄する
    clip(baseColorSample.a - AlphaCutoff);

    float3 geometricNormal = normalize(input.Normal);

    // BC5(2チャンネル、X/Yのみ)圧縮された法線マップはB/Aチャンネルにデータを持たず、
    // サンプリング時にハードウェアがB=0を返すため、Bをそのまま使うとタンジェント空間Zが
    // 常に-1(裏向き)になってしまう。単位ベクトルである前提でX/YからZを再構成する
    // (通常の3チャンネル法線マップに対しても正しく機能する)
    float2 normalXY = NormalTexture.Sample(DefaultSampler, input.UV).xy * 2.0f - 1.0f;
    float normalZ = sqrt(saturate(1.0f - dot(normalXY, normalXY)));
    float3 normalSample = float3(normalXY, normalZ);
    float3x3 tbn = ComputeTangentFrame(geometricNormal, input.Tangent);
    float3 N = normalize(mul(normalSample, tbn));

    float3 metallicRoughnessSample = MetallicRoughnessTexture.Sample(DefaultSampler, input.UV).rgb;
    float metallic = saturate(MetallicFactor * metallicRoughnessSample.b);
    // RoughnessFactorが負の場合はソースデータにラフネス係数が無かったことを表す
    // (Assets::kInvalidMaterialFactor)。パッカーが勝手な既定値を埋めない方針のため、
    // ここで係数1.0=テクスチャの値をそのまま使う、と解釈する
    float roughnessFactor = (RoughnessFactor < 0.0f) ? 1.0f : RoughnessFactor;
    float roughness = clamp(roughnessFactor * metallicRoughnessSample.g, 0.045f, 1.0f);

    float3 emissive = EmissiveTexture.Sample(DefaultSampler, input.UV).rgb * EmissiveFactor;

    PSOutput output;
    output.Albedo = float4(baseColorSample.rgb, 1.0f);
    output.Normal = OctEncode(N);
    output.Material = float4(metallic, roughness, 0.0f, 0.0f);
    output.Emissive = float4(emissive, 1.0f);
    return output;
}
