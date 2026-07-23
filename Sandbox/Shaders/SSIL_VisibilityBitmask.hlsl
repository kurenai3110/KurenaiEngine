// SSIL(Screen Space Indirect Lighting) - Visibility Bitmaskパス。
// Olivier Therrien et al. "Screen Space Indirect Lighting with Visibility Bitmask"(2023)の
// アイデアを参考にした実装: GTAO/HBAOと同様に法線周りを複数の「スライス」(視線ベクトルVと
// 画面空間方向Dが張る平面)に分割し、各スライスでスクリーン空間の水平線サーチを行うが、
// 遮蔽を2本の水平線角度ではなく32個のセクタを持つビットマスク(uint)で表現する。
// これにより「手前の1点が遮蔽している角度範囲」だけをビットとして立てられるため、
// 薄いオブジェクトの裏に光が回り込む(Thickness Heuristic)表現ができ、
// 遠くの遮蔽物によって近くの隙間が誤って塗りつぶされる問題(GTAOの弱点)も緩和される。
//
// 間接光(GI)は、各サンプルで新規に隠れたビット数(=このサンプルが担う可視立体角の割合)を重みとして、
// サンプル地点のG-Buffer(Albedo/Normal)から簡易的に計算した直接光+環境光を間接反射光として積算する。
// (本来は前フレームのライティング結果を再利用する実装が多いが、本エンジンはSSILパスがライティング
// パスより前に実行されるため、サンプル地点で簡易的に直接光を計算し直す近似を採用している)
//
// PSMain: G-BufferのAlbedo/Normal/Depthから遮蔽率(AO, alpha)と間接拡散光(GI, rgb)を計算する
// PSMainBlur: SSAOパスと共有する汎用RGBAボックスブラー(SSAO.hlsl側)を使い回すため、ここには実装しない
static const float PI = 3.14159265359f;
static const float HALF_PI = 1.57079632679f;
static const float TWO_PI = 6.28318530718f;
static const uint kSectorCount = 32u;

cbuffer FrameConstants : register(b0)
{
    float4x4 ViewProj;
    float4x4 InvViewProj;
    float4x4 LightViewProj;
    float4 CameraPosition;
    float4 LightDirection;
    float4 LightColor;
    float4x4 View;
    float4x4 Proj;
    float4 AmbientColor;
};

cbuffer SSILConstants : register(b1)
{
    float4 Params0; // x: 半径(ワールド空間), y: 厚み(Thickness Heuristic), z: 間接光の強さ, w: AOのコントラスト(べき乗)
    uint4 Params1;  // x: スライス数, y: スライスあたりのステップ数, z/w: 未使用
};

// Texture0=Albedo, Texture1=World Normal, Texture2=Depth (すべてG-Buffer、レンダー解像度と同じ)
Texture2D Texture0 : register(t0);
Texture2D Texture1 : register(t1);
Texture2D Texture2 : register(t2);
SamplerState DefaultSampler : register(s0);

struct PSInput
{
    float4 Position : SV_POSITION;
    float2 UV : TEXCOORD0;
};

// 頂点バッファなしで画面全体を覆う三角形を1枚だけ生成する定番のテクニック
PSInput VSMain(uint vertexID : SV_VertexID)
{
    PSInput output;
    output.UV = float2((vertexID << 1) & 2, vertexID & 2);
    output.Position = float4(output.UV.x * 2.0f - 1.0f, 1.0f - output.UV.y * 2.0f, 0.0f, 1.0f);
    return output;
}

float3 ReconstructWorldPos(float2 uv, float depth)
{
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 clipPos = float4(ndc, depth, 1.0f);
    float4 worldPos = mul(clipPos, InvViewProj);
    return worldPos.xyz / worldPos.w;
}

// ビュー空間の点を画面UVへ投影する(SSAOパスの流儀と同じくmul(vec,Proj)してw除算する)
float2 ProjectToUV(float3 viewPos)
{
    float4 clipPos = mul(float4(viewPos, 1.0f), Proj);
    float w = abs(clipPos.w) > 1e-4f ? clipPos.w : 1e-4f;
    float2 ndc = clipPos.xy / w;
    return float2(ndc.x * 0.5f + 0.5f, 1.0f - (ndc.y * 0.5f + 0.5f));
}

// ピクセル座標から[0,1)の疑似乱数を得るハッシュ関数(Dave Hoskinsのhash12)。
// スライスの向きを毎ピクセルわずかに回転させ、バンディングを高周波ノイズに変換する(後段でブラーする)
float Hash12(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031f);
    p3 += dot(p3, p3.yzx + 33.33f);
    return frac((p3.x + p3.y) * p3.z);
}

// -pi..pi にラップする(2つのatan2角度の差分をとった際に周期をまたぐケースに対応)
float WrapAngle(float a)
{
    return atan2(sin(a), cos(a));
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float depth = Texture2.Sample(DefaultSampler, input.UV).r;
    if (depth >= 1.0f)
    {
        // 背景(スカイ)は遮蔽なし・間接光なし
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    }

    const float radius = Params0.x;
    const float thickness = Params0.y;
    const float intensity = Params0.z;
    const float aoPower = Params0.w;
    const uint sliceCount = max(Params1.x, 1u);
    const uint stepCount = max(Params1.y, 1u);

    float3 worldPos = ReconstructWorldPos(input.UV, depth);
    float3 normalWorld = normalize(Texture1.Sample(DefaultSampler, input.UV).xyz * 2.0f - 1.0f);

    float3 P = mul(float4(worldPos, 1.0f), View).xyz;
    float3 N = normalize(mul(normalWorld, (float3x3)View));
    float3 V = normalize(-P);

    // ワールド半径を、現在の画素の深度における画面UV半径に変換する(視点から遠いほどUV上では小さくなる)
    float2 uvAtRadius = ProjectToUV(P + float3(radius, 0.0f, 0.0f));
    float screenRadiusUV = clamp(length(uvAtRadius - input.UV), 0.0f, 0.5f);
    if (screenRadiusUV < 1e-5f)
    {
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    }

    float jitter = Hash12(input.Position.xy);

    float ao = 0.0f;
    float3 gi = float3(0.0f, 0.0f, 0.0f);

    [loop]
    for (uint i = 0; i < sliceCount; ++i)
    {
        float angle = (float(i) + jitter) / float(sliceCount) * TWO_PI;
        float2 dir2D = float2(cos(angle), sin(angle));
        float3 D = normalize(float3(dir2D, 0.0f));

        // スライス平面(DとVが張る平面)の法線と、その平面内でVに直交するタンジェント方向を求める。
        // これを基準に、法線N・各サンプルへのオフセットを平面内の角度(atan2)として測る
        float3 planeNormal = cross(D, V);
        float planeNormalLen = length(planeNormal);
        if (planeNormalLen < 1e-5f)
        {
            continue;
        }
        planeNormal /= planeNormalLen;
        float3 e1 = normalize(cross(V, planeNormal));
        float3 e2 = V;

        float normalAngle = atan2(dot(N, e2), dot(N, e1));

        uint sliceMask = 0u;

        [loop]
        for (uint s = 1; s <= stepCount; ++s)
        {
            float t = float(s) / float(stepCount + 1u);
            float2 sampleUV = input.UV + dir2D * (t * screenRadiusUV);
            if (sampleUV.x < 0.0f || sampleUV.x > 1.0f || sampleUV.y < 0.0f || sampleUV.y > 1.0f)
            {
                continue;
            }

            float sampleDepth = Texture2.Sample(DefaultSampler, sampleUV).r;
            if (sampleDepth >= 1.0f)
            {
                continue;
            }

            float3 sampleWorldPos = ReconstructWorldPos(sampleUV, sampleDepth);
            float3 sampleViewPos = mul(float4(sampleWorldPos, 1.0f), View).xyz;

            float3 offsetFront = sampleViewPos - P;
            float distFront = length(offsetFront);
            if (distFront < 1e-5f || distFront > radius)
            {
                continue;
            }

            // Thickness Heuristic: サンプル点は無限に薄い点ではなく、視線方向にthicknessぶん奥行きを
            // 持つ板とみなす。表(front)と裏(back)、両方の水平線角度をとりビットマスクの範囲にする
            float3 viewDirToSample = normalize(sampleViewPos);
            float3 backViewPos = sampleViewPos + viewDirToSample * thickness;
            float3 offsetBack = backViewPos - P;

            float angleFront = WrapAngle(atan2(dot(offsetFront, e2), dot(offsetFront, e1)) - normalAngle);
            float angleBack = WrapAngle(atan2(dot(offsetBack, e2), dot(offsetBack, e1)) - normalAngle);

            // 法線の接平面より下(可視半球の外)は寄与しないので範囲外として扱う
            if (angleFront < -HALF_PI && angleBack < -HALF_PI)
            {
                continue;
            }
            if (angleFront > HALF_PI && angleBack > HALF_PI)
            {
                continue;
            }

            angleFront = clamp(angleFront, -HALF_PI, HALF_PI);
            angleBack = clamp(angleBack, -HALF_PI, HALF_PI);

            float theta01Min = (min(angleFront, angleBack) + HALF_PI) / PI;
            float theta01Max = (max(angleFront, angleBack) + HALF_PI) / PI;

            uint startBit = min((uint)floor(saturate(theta01Min) * float(kSectorCount)), kSectorCount - 1u);
            uint bitSpan = (uint)ceil(saturate(theta01Max - theta01Min) * float(kSectorCount));
            bitSpan = clamp(bitSpan, 1u, kSectorCount - startBit);
            uint sampleMaskBits = (bitSpan >= kSectorCount) ? 0xFFFFFFFFu : (((1u << bitSpan) - 1u) << startBit);

            // このサンプルで新規に隠れたビット数を、サンプルが担う可視立体角の割合とみなす。
            // 既に手前のサンプルで隠れていた区間は二重にカウントしない(=多重遮蔽物の裏に光が回り込む)
            uint newlySetBits = sampleMaskBits & ~sliceMask;
            uint newlySetCount = countbits(newlySetBits);
            if (newlySetCount > 0u)
            {
                float3 sampleAlbedo = Texture0.Sample(DefaultSampler, sampleUV).rgb;
                float3 sampleNormalWorld = normalize(Texture1.Sample(DefaultSampler, sampleUV).xyz * 2.0f - 1.0f);

                float3 lightDirWorld = normalize(sampleWorldPos - worldPos);
                float receiverNdotL = saturate(dot(normalWorld, lightDirWorld));
                float emitterNdotL = saturate(dot(sampleNormalWorld, -lightDirWorld));

                // サンプル地点の簡易直接光(ライティングパスの前に実行されるため、ここで簡易的に計算し直す近似)
                float sampleDirectNdotL = saturate(dot(sampleNormalWorld, -LightDirection.xyz));
                float3 sampleRadiance = sampleAlbedo * (sampleDirectNdotL * LightColor.rgb + AmbientColor.rgb);

                float weight = float(newlySetCount) / float(kSectorCount);
                gi += sampleRadiance * weight * receiverNdotL * emitterNdotL;
            }

            sliceMask |= sampleMaskBits;
            if (sliceMask == 0xFFFFFFFFu)
            {
                // このスライスの全セクタが既に埋まった: これ以上新しい区間は増えないため打ち切る
                break;
            }
        }

        ao += 1.0f - float(countbits(sliceMask)) / float(kSectorCount);
    }

    ao = saturate(ao / float(sliceCount));
    ao = pow(ao, aoPower);
    gi = saturate(gi / float(sliceCount) * intensity);

    return float4(gi, ao);
}
