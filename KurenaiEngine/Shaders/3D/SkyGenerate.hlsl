// 空のキューブマップをGPUで手続き生成する。Perez et al. "All-Weather Model for Sky Luminance
// Distribution" (1993) / Preetham, Shirley, Smits (SIGGRAPH 1999) のCIE快晴空モデル。
//
// === なぜオフラインDDSではなくGPU生成なのか ===
// Perez分布は太陽の位置に依存するため、時刻が変わると空の輝度分布の「形」そのものが変わる
// (circumsolarの明るい領域が太陽と一緒に動く)。これはオフラインで焼いた2枚のキューブマップの
// 線形補間では表現できない。
// 逆に言えば「昼夜2枚をブレンドするだけ」なら畳み込みが線形演算である以上
// prefilter(lerp(昼,夜,t)) == lerp(prefilter(昼),prefilter(夜),t) が厳密に成立するので、
// 動的な再ベイクは純粋な無駄になる。再ベイクが意味を持つのは、この非線形な変化を追うときだけ。
//
// このシェーダーは Tools/generate_sky_cubemap.py の移植であり、両者は同じ絵を出す必要がある
// (あちらはオフラインの参照実装 兼 手続き空を無効にしたときのフォールバック用)。
// 係数・定数を変える場合は必ず両方を同時に直すこと。
#include "Samplers.hlsli"

static const float PI = 3.14159265359f;

cbuffer SkyBakeConstants : register(b0)
{
    // 処理対象の面(D3Dのキューブマップ標準順: +X=0,-X=1,+Y=2,-Y=3,+Z=4,-Z=5)
    uint Face;
    // 天頂輝度のスケール。Perezの相対輝度にこれを掛けたものが最終的な輝度になる
    float ZenithLuminance;
    float2 SkyPadding0;
    // 太陽が「ある」向き(正規化済み)。光が進む向きとは符号が逆なので注意
    // (KurenaiEngine3D.cpp ComputeSunLighting の sunDirection と同じ向き)
    float4 SunDirection;
    // --- 空の色味。太陽高度に応じてCPU側(ComputeSkyTint)が決めた値。xyzのみ使う ---
    // 【なぜCPUで決めるのか】この色味は、ここでの描画と CPU 側の照度正規化
    // (ComputeSkyZenithScale。積分の重みに色味の輝度成分が入る)の両方で完全に一致して
    // いなければならない。CPUで1度決めて配れば、両者がずれることが構造的に起きなくなる
    float4 ZenithTint;
    float4 HorizonTint;
    float4 GroundTint;
    // xyz=夕焼け・朝焼けの暖色、w=その強さ(太陽の仰角0度で1、±15度で0)
    float4 SunGlowTint;
};

RWTexture2DArray<float4> SkyOut : register(u0);

// --- generate_sky_cubemap.py と一致させる定数 ---
// 地平線より下は空モデルの適用範囲外。プラトー色から暗い接地色へフェードさせる。
// ゼロにしないのは、IBLの拡散イラディアンス積分で下半球が完全な暗黒にならないようにするため
static const float kGroundFadeStartY = -0.02f;
static const float kGroundFadeEndY = -0.6f;
// CIE快晴空係数(circumsolar項 c=10, d=-3)は反太陽側の水平線で輝度が天頂の0.2倍程度まで落ちる。
// 実際の大気は多重散乱で暗部が持ち上がるためゼロにはしないが、以前ここを0.45にしていたときは
// 輝度の勾配がほぼ消えて空全体が一様なスレートグレーになっていた(実測: 彩度0.26で時刻不変)。
// 勾配が残る値まで下げてある。KurenaiEngine3D.cpp の kSkyRelativeLuminanceFloor と同じ値であること
static const float kRelativeLuminanceFloor = 0.12f;

// キューブマップの1面上のUV([0,1]^2)から方向を求める。
// IBLConvolve.hlsl の CubeFaceDirection および generate_sky_cubemap.py の face_direction_grid と
// 完全に同一の規約でなければならない(ずれると空とIBLで方向が食い違う)
float3 CubeFaceDirection(uint face, float2 uv)
{
    float2 ndc = uv * 2.0f - 1.0f;
    float u = ndc.x;
    float v = ndc.y;

    float3 dir;
    if (face == 0)      dir = float3(1.0f, -v, -u);   // +X
    else if (face == 1) dir = float3(-1.0f, -v, u);   // -X
    else if (face == 2) dir = float3(u, 1.0f, v);     // +Y
    else if (face == 3) dir = float3(u, -1.0f, -v);   // -Y
    else if (face == 4) dir = float3(u, -v, 1.0f);    // +Z
    else                dir = float3(-u, -v, -1.0f);  // -Z

    return normalize(dir);
}

// Perezの5係数関数。cosThetaは水平線(cosθ→0)で発散するため呼び出し側でクランプ済みの前提
float PerezF(float cosTheta, float gamma, float a, float b, float c, float d, float e)
{
    const float cosGamma = cos(gamma);
    return (1.0f + a * exp(b / cosTheta)) * (1.0f + c * exp(d * gamma) + e * cosGamma * cosGamma);
}

float PerezRelativeLuminance(float cosTheta, float gamma, float cosThetaSun, float thetaSun)
{
    // CIE快晴空の標準係数(Perez et al. 1993 / Preetham et al. 1999, Table 1)
    const float a = -1.0f;
    const float b = -0.32f;
    const float c = 10.0f;
    const float d = -3.0f;
    const float e = 0.45f;
    return PerezF(cosTheta, gamma, a, b, c, d, e) / PerezF(cosThetaSun, thetaSun, a, b, c, d, e);
}

// 太陽の暖色を混ぜる重み。KurenaiEngine3D.cpp の SunGlowWeight と同じ式であること。
// 太陽から離れるほど急に落ちる4乗カーブ。太陽が地平線下にあっても、その方位の低空には
// まだ暖色が残る(実際の夕焼けの残光と同じ構造)
float SunGlowWeight(float cosGamma, float glowStrength)
{
    const float proximity = saturate(cosGamma);
    const float falloff = proximity * proximity * proximity * proximity;
    return saturate(glowStrength * falloff);
}

// 方向(天頂角と太陽との離角)に対する空の色味。
// KurenaiEngine3D.cpp の SkyTint と同じ式であること
float3 SkyTint(float cosTheta, float cosGamma)
{
    // 水平線側への寄せを3乗カーブにして、高度があるうちは天頂色をほぼ保つ
    const float horizonBlend = pow(1.0f - saturate(cosTheta), 3.0f);
    const float3 base = lerp(ZenithTint.rgb, HorizonTint.rgb, horizonBlend);
    return lerp(base, SunGlowTint.rgb, SunGlowWeight(cosGamma, SunGlowTint.w));
}

// 水平線以上を仮定した空の色(呼び出し側で地面フェードと合成する)
float3 SkyColorUpper(float3 dir, float3 sunDir)
{
    // Perez分布は水平線で不安定になるため天頂角を89.5度までにクランプする
    const float clampedY = max(dir.y, cos(radians(89.5f)));
    const float cosTheta = clamp(clampedY, 1e-3f, 1.0f);

    const float thetaSun = acos(clamp(sunDir.y, -1.0f, 1.0f));
    const float cosThetaSun = max(cos(thetaSun), 1e-3f);

    const float cosGamma = clamp(dot(dir, sunDir), -1.0f, 1.0f);
    const float gamma = acos(cosGamma);

    float relative = max(PerezRelativeLuminance(cosTheta, gamma, cosThetaSun, thetaSun), 0.0f);
    relative = kRelativeLuminanceFloor + (1.0f - kRelativeLuminanceFloor) * relative;

    return relative * ZenithLuminance * SkyTint(cosTheta, cosGamma);
}

float3 SkyColor(float3 dir, float3 sunDir)
{
    if (dir.y >= kGroundFadeStartY)
    {
        return SkyColorUpper(dir, sunDir);
    }

    // 水平線より下: プラトー色(kGroundFadeStartYの高さへ射影した方向の空色)から接地色へフェード
    float3 plateauDir = dir;
    plateauDir.y = kGroundFadeStartY;
    plateauDir = normalize(plateauDir);
    const float3 plateauColor = SkyColorUpper(plateauDir, sunDir);

    const float3 groundColor = ZenithLuminance * GroundTint.rgb;
    const float groundT = saturate((dir.y - kGroundFadeStartY) / (kGroundFadeEndY - kGroundFadeStartY));
    return lerp(plateauColor, groundColor, groundT);
}

[numthreads(8, 8, 1)]
void CSGenerateSky(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint width, height, elements;
    SkyOut.GetDimensions(width, height, elements);
    if (dispatchThreadID.x >= width || dispatchThreadID.y >= height)
    {
        return;
    }

    // テクセル中心のUVから方向を求める
    const float2 uv = (float2(dispatchThreadID.xy) + 0.5f) / float2(width, height);
    const float3 dir = CubeFaceDirection(Face, uv);

    const float3 color = SkyColor(dir, normalize(SunDirection.xyz));

    // 面ごとに要素数1のUAVを張るためスライスは常に0
    SkyOut[uint3(dispatchThreadID.xy, 0)] = float4(max(color, 0.0f), 1.0f);
}
