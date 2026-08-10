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
    // DrawRoundedRect/DrawCircle専用。xy=半幅・半高さ(ピクセル), z=角丸半径(ピクセル),
    // w=枠線太さ(ピクセル)。DrawCircleはxy・zすべてに半径を入れる(= 角丸半径が半幅・半高さと
    // 等しい角丸矩形は円そのもの、という関係だが、円は専用の距離関数のほうが安いのでPSは分けてある)
    float4 ShapeParams;
    // DrawRoundedRect/DrawCircle専用。枠線の色(borderThicknessPixels<=0のときは未使用)
    float4 BorderColor;
};

Texture2D SpriteTexture : register(t0);
// 役割はKurenaiEngine/Shaders/3D/Samplers.hlsliのMaterialSamplerと同じ(s0固定)。
// 2Dは3D側の共有ヘッダーに依存させないため、ここで単独に宣言している。
// KurenaiEngine2Dはこのスロットへ異方性16x + Wrapのサンプラーを1つだけ持つセットをバインドする
SamplerState MaterialSampler : register(s0);

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
    float4 texColor = SpriteTexture.Sample(MaterialSampler, input.UV);
    return texColor * Color;
}

// 符号付き距離(内側が負)から、塗り+枠線の最終色を作るDrawCircle/DrawRoundedRect共通の処理。
//
// 【枠線を「塗りの上へのアルファ合成」として扱う理由】以前はresult.rgbをlerpするだけで
// アルファは塗り側のままだった。この方式だと塗りa=0(塗りなし)にすると枠線ごと透明になり、
// 射程円のような「中が完全に透明なリング」が表現できない。枠線を独立したレイヤーとして
// 通常のアルファ合成で重ねることで、塗りa=0+枠線ありがそのままリングになる。
// 塗りが不透明(a=1)の場合の結果は従来と一致する
float4 CompositeFillAndBorder(float dist, float border)
{
    const float edge = fwidth(dist);
    const float fillCoverage = 1.0f - smoothstep(-edge, edge, dist);

    float4 result = float4(Color.rgb, Color.a * fillCoverage);
    if (border > 0.0f)
    {
        // 図形の内側かつ、ふちからborderピクセル以内の帯
        const float borderCoverage = smoothstep(-edge, edge, dist + border) * fillCoverage;
        const float borderAlpha = BorderColor.a * borderCoverage;
        const float outAlpha = borderAlpha + result.a * (1.0f - borderAlpha);
        // ストレートアルファのまま合成するため、色は合成後のアルファで割り戻す
        result.rgb = outAlpha > 0.0f
            ? (BorderColor.rgb * borderAlpha + result.rgb * result.a * (1.0f - borderAlpha)) / outAlpha
            : result.rgb;
        result.a = outAlpha;
    }
    return result;
}

// DrawCircle用。テクスチャを使わず、UV(0〜1)を中心からの距離に変換した円の符号付き距離関数で
// ふちをアンチエイリアスしながら塗りつぶし、borderThicknessPixels>0のときは内側に
// BorderColorで枠線を重ねる(KurenaiEngine2D::DrawCircle参照)
float4 PSCircle(PSInput input) : SV_TARGET
{
    const float radiusPixels = ShapeParams.x; // 半径(ピクセル)
    const float border = ShapeParams.w;

    // 枠線の太さをピクセルで指定できるよう、距離もピクセル単位で求める
    const float2 centeredPixels = (input.UV * 2.0f - 1.0f) * radiusPixels;
    const float dist = length(centeredPixels) - radiusPixels; // 円までの符号付き距離(内側が負)
    return CompositeFillAndBorder(dist, border);
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
    return CompositeFillAndBorder(dist, border);
}
