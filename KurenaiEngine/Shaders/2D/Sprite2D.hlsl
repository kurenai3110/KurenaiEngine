// KurenaiEngine2D(2D公開API)が内部で使うスプライト描画シェーダ。
// 定数バッファはb0(フレーム共通)とb1(スプライト単位)、テクスチャ/サンプラーはt0/s0のみを使用する。

cbuffer FrameConstants : register(b0)
{
    float4x4 ViewProj;
};

cbuffer ObjectConstants : register(b1)
{
    float4x4 World;
    float4 Color;
    // xy=UVオフセット, zw=UVスケール。DrawText(フォントアトラスの1文字ぶんの矩形)専用で、
    // それ以外(DrawSprite/DrawCircle/DrawRoundedRect)は(0, 0, 1, 1)の恒等変換で呼ぶ
    float4 UVOffsetScale;
    // DrawRoundedRect専用。xy=半幅・半高さ(ピクセル), z=角丸半径(ピクセル), w=枠線太さ(ピクセル)
    float4 ShapeParams;
    // DrawRoundedRect専用。枠線の色(borderThicknessPixels<=0のときは未使用)
    float4 BorderColor;
};

Texture2D SpriteTexture : register(t0);
SamplerState DefaultSampler : register(s0);

struct VSInput
{
    float3 Position : POSITION;
    float2 UV : TEXCOORD0;
};

struct PSInput
{
    float4 Position : SV_POSITION;
    float2 UV : TEXCOORD0;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    float4 worldPos = mul(float4(input.Position, 1.0f), World);
    output.Position = mul(worldPos, ViewProj);
    output.UV = UVOffsetScale.xy + input.UV * UVOffsetScale.zw;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float4 texColor = SpriteTexture.Sample(DefaultSampler, input.UV);
    return texColor * Color;
}

// DrawCircle用。テクスチャを使わず、UV(0〜1)を中心からの距離に変換した円形マスクで
// ふちをアンチエイリアスしながら塗りつぶす(KurenaiEngine2D::DrawCircle参照)
float4 PSCircle(PSInput input) : SV_TARGET
{
    const float2 centered = input.UV * 2.0f - 1.0f; // UV(0〜1) -> 中心が原点の-1〜1
    const float dist = length(centered);
    const float edge = fwidth(dist);
    const float coverage = 1.0f - smoothstep(1.0f - edge, 1.0f + edge, dist);
    return float4(Color.rgb, Color.a * coverage);
}

// DrawRoundedRect用。角丸矩形の符号付き距離関数(SDF)でふちをアンチエイリアスしながら塗りつぶし、
// borderThicknessPixels>0のときは内側にBorderColorで枠線を重ねる(KurenaiEngine2D::DrawRoundedRect参照)
float4 PSRoundedRect(PSInput input) : SV_TARGET
{
    const float2 halfSize = ShapeParams.xy; // 半幅・半高さ(ピクセル)
    const float radius = min(ShapeParams.z, min(halfSize.x, halfSize.y));
    const float border = ShapeParams.w;

    const float2 centeredPixels = (input.UV * 2.0f - 1.0f) * halfSize; // UV(0..1) -> ピクセル単位のローカル座標(中心原点)
    const float2 q = abs(centeredPixels) - (halfSize - radius);
    const float dist = length(max(q, 0.0f)) + min(max(q.x, q.y), 0.0f) - radius; // 角丸矩形までの符号付き距離(内側が負)

    const float edge = fwidth(dist);
    const float fillCoverage = 1.0f - smoothstep(-edge, edge, dist);

    float4 result = float4(Color.rgb, Color.a * fillCoverage);
    if (border > 0.0f)
    {
        const float borderCoverage = smoothstep(-edge, edge, dist + border) * fillCoverage;
        result.rgb = lerp(result.rgb, BorderColor.rgb, borderCoverage * BorderColor.a);
    }
    return result;
}
