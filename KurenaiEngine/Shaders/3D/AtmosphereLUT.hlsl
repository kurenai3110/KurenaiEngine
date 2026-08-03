// 大気散乱のLUT生成(P14a)。Hillaire, "A Scalable and Production Ready Sky and Atmosphere
// Rendering Technique" (EGSR 2020) の構成に従う。
//
// 【なぜPreethamから乗り換えるのか】P13bで参考写真と突き合わせた結果、空の色がPreethamの
// 限界に当たっていることが実測で確定した。写真の最も青い空はB/R=4.84だが、Preethamは論文の
// 係数から実装とは独立に計算しても1.34〜1.74しか出さない(実装の実測もこの範囲内でモデルに忠実)。
// Rayleigh散乱はλ^-4に比例するので、物理から始めればB/Rは散乱係数の時点で5.70になる。
// つまりP13bで SkySaturation=1.9 という非物理のつまみで埋めた穴は、物理ベースのモデルなら
// 自然に埋まる性質のものである。地平線がマゼンタに寄る問題(Preethamは仰角0.5度で緑の
// 落ち込みが-7.6)も、Rayleigh/Mieを分けて持てば構造的に起きない。
//
// 【焼くもの】
//   CSTransmittance   … 256x64。高度×視線天頂角。大気圏上端までの消散(Rayleigh+Mie+オゾン)
//   CSMultiScattering …  32x32。高度×太陽天頂角。多重散乱を等方近似で1枚に畳んだもの
//                        (これがHillaireの中核。地平線の白さはこの項が作る)
//   CSSkyView         … 192x108。カメラ周りの空。太陽が動くたびに焼き直す
//
// TransmittanceとMultiScatteringは大気パラメータだけで決まるので起動後に一度だけ焼く
// (BRDF積分LUT・雲の3Dノイズと同じ「一度だけ焼く」作法)。
//
// 【単位はkm】Hillaireの参照実装と同じくkmで統一する。散乱係数もkm^-1。
// メートルで書くと地球半径6360000のような値が出てfloat32の桁が苦しくなる。
#include "Samplers.hlsli"

// ============================================================================
// 大気パラメータ
//
// 【出典】Hillaire (2020) の付録およびBruneton-Neyret (2008) が使う地球大気の標準値。
// 見た目からの調整値は1つも含まない。ここを変えるとLUTを焼き直す必要がある
// (KurenaiEngine3D::m_AtmosphereLUTBaked)。
// ============================================================================

static const float kBottomRadiusKm = 6360.0f;   // 地表
static const float kTopRadiusKm = 6460.0f;      // 大気圏上端(厚さ100km)

// Rayleigh散乱。λ^-4に比例するのでB/R = 33.100/5.802 = 5.70。
// **空が青い理由そのもの**で、Preethamのフィットが再現しきれていなかったのがこの比
static const float3 kRayleighScattering = float3(0.005802f, 0.013558f, 0.033100f); // 1/km
static const float kRayleighScaleHeightKm = 8.0f;

// Mie散乱(エアロゾル)。波長依存がほぼ無いので白っぽい霞になる。
// 吸収があるため散乱係数と消散係数が異なる
static const float kMieScattering = 0.003996f;  // 1/km
static const float kMieExtinction = 0.004440f;  // 1/km
static const float kMieScaleHeightKm = 1.2f;
static const float kMiePhaseG = 0.8f;

// オゾンの吸収。**省いてはいけない**。省くと薄明の色と天頂の青が両方おかしくなる
// (オゾンは緑〜赤を吸うので、これが無いと空が緑がかる)。
// 高度分布は指数ではなく高度25kmを中心・半幅15kmのテント形
static const float3 kOzoneAbsorption = float3(0.000650f, 0.001881f, 0.000085f); // 1/km
static const float kOzoneCenterKm = 25.0f;
static const float kOzoneHalfWidthKm = 15.0f;

// 地表のアルベド。多重散乱LUTで地面からの照り返しを見込むのに使う
static const float3 kGroundAlbedo = float3(0.3f, 0.3f, 0.3f);

static const float kPI = 3.14159265359f;

// ============================================================================
// 媒質
// ============================================================================

// 高度[km]における散乱係数と消散係数
void SampleMedium(float altitudeKm, out float3 scattering, out float3 extinction)
{
    const float rayleighDensity = exp(-max(altitudeKm, 0.0f) / kRayleighScaleHeightKm);
    const float mieDensity = exp(-max(altitudeKm, 0.0f) / kMieScaleHeightKm);
    // テント形。中心から半幅ぶん離れると0になる
    const float ozoneDensity =
        max(0.0f, 1.0f - abs(altitudeKm - kOzoneCenterKm) / kOzoneHalfWidthKm);

    const float3 rayleighS = kRayleighScattering * rayleighDensity;
    const float mieS = kMieScattering * mieDensity;
    const float mieE = kMieExtinction * mieDensity;
    const float3 ozoneA = kOzoneAbsorption * ozoneDensity;

    scattering = rayleighS + mieS;
    // オゾンは吸収のみ(散乱しない)なので消散にだけ入る
    extinction = rayleighS + mieE + ozoneA;
}

// 原点からdir方向のレイと、中心が原点・半径radiusの球との交点までの距離。
// 交わらなければ負を返す。posは地球中心を原点とした位置
float RaySphereNearest(float3 pos, float3 dir, float radius)
{
    const float b = dot(pos, dir);
    const float c = dot(pos, pos) - radius * radius;
    const float discriminant = b * b - c;
    if (discriminant < 0.0f)
    {
        return -1.0f;
    }
    const float sqrtD = sqrt(discriminant);
    const float t0 = -b - sqrtD;
    const float t1 = -b + sqrtD;
    if (t1 < 0.0f)
    {
        return -1.0f;
    }
    return (t0 < 0.0f) ? t1 : t0;
}

// ============================================================================
// Transmittance LUT
//
// パラメータ化はBruneton-Neyret (2008) の標準形。(r, mu) を [0,1]^2 へ写す。
//   r  … 地球中心からの距離
//   mu … 視線と天頂方向のなす角の余弦
// 地平線を跨ぐところで分解能が要るため、単純な線形ではなく
// 「大気圏上端までの距離」を最小値・最大値で正規化する形を使う
// ============================================================================

void TransmittanceUvToRMu(float2 uv, out float r, out float mu)
{
    const float H = sqrt(max(kTopRadiusKm * kTopRadiusKm - kBottomRadiusKm * kBottomRadiusKm, 0.0f));
    const float rho = H * uv.y;
    r = sqrt(max(rho * rho + kBottomRadiusKm * kBottomRadiusKm, 0.0f));

    const float dMin = kTopRadiusKm - r;
    const float dMax = rho + H;
    const float d = dMin + uv.x * (dMax - dMin);
    mu = (d == 0.0f) ? 1.0f : clamp((H * H - rho * rho - d * d) / (2.0f * r * d), -1.0f, 1.0f);
}

float2 TransmittanceRMuToUv(float r, float mu)
{
    const float H = sqrt(max(kTopRadiusKm * kTopRadiusKm - kBottomRadiusKm * kBottomRadiusKm, 0.0f));
    const float rho = sqrt(max(r * r - kBottomRadiusKm * kBottomRadiusKm, 0.0f));
    const float discriminant = r * r * (mu * mu - 1.0f) + kTopRadiusKm * kTopRadiusKm;
    const float d = max(-r * mu + sqrt(max(discriminant, 0.0f)), 0.0f);
    const float dMin = kTopRadiusKm - r;
    const float dMax = rho + H;
    return float2((d - dMin) / max(dMax - dMin, 1e-6f), rho / max(H, 1e-6f));
}

// 高度rから天頂角cosがmuの向きへ、大気圏上端まで進んだときの光学的深さ
static const int kTransmittanceSteps = 40;
float3 ComputeOpticalDepthToTop(float r, float mu)
{
    const float3 pos = float3(0.0f, r, 0.0f);
    const float3 dir = float3(sqrt(max(1.0f - mu * mu, 0.0f)), mu, 0.0f);
    const float distanceToTop = RaySphereNearest(pos, dir, kTopRadiusKm);
    if (distanceToTop <= 0.0f)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    const float dt = distanceToTop / float(kTransmittanceSteps);
    float3 opticalDepth = float3(0.0f, 0.0f, 0.0f);
    [loop]
    for (int i = 0; i < kTransmittanceSteps; ++i)
    {
        // 中点則
        const float t = (float(i) + 0.5f) * dt;
        const float3 samplePos = pos + dir * t;
        const float altitude = length(samplePos) - kBottomRadiusKm;
        float3 scattering, extinction;
        SampleMedium(altitude, scattering, extinction);
        opticalDepth += extinction * dt;
    }
    return opticalDepth;
}

RWTexture2D<float4> TransmittanceOut : register(u0);

[numthreads(8, 8, 1)]
void CSTransmittance(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint width, height;
    TransmittanceOut.GetDimensions(width, height);
    if (dispatchThreadID.x >= width || dispatchThreadID.y >= height)
    {
        return;
    }

    const float2 uv = (float2(dispatchThreadID.xy) + 0.5f) / float2(width, height);
    float r, mu;
    TransmittanceUvToRMu(uv, r, mu);

    const float3 transmittance = exp(-ComputeOpticalDepthToTop(r, mu));
    TransmittanceOut[dispatchThreadID.xy] = float4(transmittance, 1.0f);
}

// ============================================================================
// MultiScattering LUT
//
// Hillaireの中核。多重散乱を「等方的に散らばった光」として1枚のLUTへ畳む。
// 各テクセルで球面上のN方向へレイマーチし、1次散乱の寄与と「その場に等方的に
// 入ってくる光」の2つを積む。最後に無限次までの等比級数へ畳む:
//   F_multi = L_2nd / (1 - f_ms)
// これが地平線際の白さを作る項で、これを省くと空が暗く彩度過剰になる
// (Nishita 1993が地平線で破綻するのはこの項が無いため)。
// ============================================================================

static const int kMultiScatteringDirections = 64;   // 球面サンプル数
static const int kMultiScatteringSteps = 20;        // 1方向あたりのレイマーチ段数

Texture2D<float4> TransmittanceIn : register(t0);
RWTexture2D<float4> MultiScatteringOut : register(u0);

float3 SampleTransmittance(float r, float mu)
{
    const float2 uv = TransmittanceRMuToUv(r, mu);
    return TransmittanceIn.SampleLevel(ColorSampler, uv, 0.0f).rgb;
}

[numthreads(8, 8, 1)]
void CSMultiScattering(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint width, height;
    MultiScatteringOut.GetDimensions(width, height);
    if (dispatchThreadID.x >= width || dispatchThreadID.y >= height)
    {
        return;
    }

    const float2 uv = (float2(dispatchThreadID.xy) + 0.5f) / float2(width, height);
    // x=太陽天頂角の余弦([-1,1]へ)、y=高度([地表,上端]へ)
    const float sunCosZenith = uv.x * 2.0f - 1.0f;
    const float r = lerp(kBottomRadiusKm, kTopRadiusKm, uv.y);
    const float3 sunDir = float3(sqrt(max(1.0f - sunCosZenith * sunCosZenith, 0.0f)), sunCosZenith, 0.0f);
    const float3 pos = float3(0.0f, r, 0.0f);

    // 【等方近似】多重散乱は方向依存を持たないと仮定するので、位相関数は
    // 等方(1/4π)を使い、球面上を一様にサンプルして平均を取る
    const float uniformPhase = 1.0f / (4.0f * kPI);

    float3 secondOrder = float3(0.0f, 0.0f, 0.0f);  // 2次散乱の寄与
    float3 multiScatterAs1 = float3(0.0f, 0.0f, 0.0f); // 「1で照らしたときに返る量」= 等比級数の公比

    [loop]
    for (int d = 0; d < kMultiScatteringDirections; ++d)
    {
        // フィボナッチ球で一様に方向を撒く
        const float idx = float(d) + 0.5f;
        const float cosTheta = 1.0f - 2.0f * idx / float(kMultiScatteringDirections);
        const float sinTheta = sqrt(max(1.0f - cosTheta * cosTheta, 0.0f));
        const float phi = idx * kPI * (3.0f - sqrt(5.0f)); // 黄金角
        const float3 rayDir = float3(sinTheta * cos(phi), cosTheta, sinTheta * sin(phi));

        // このレイが大気を抜けるまで(または地面に当たるまで)の距離
        float tMax = RaySphereNearest(pos, rayDir, kTopRadiusKm);
        const float tGround = RaySphereNearest(pos, rayDir, kBottomRadiusKm);
        bool hitGround = false;
        if (tGround > 0.0f)
        {
            tMax = min(tMax, tGround);
            hitGround = true;
        }
        if (tMax <= 0.0f)
        {
            continue;
        }

        const float dt = tMax / float(kMultiScatteringSteps);
        float3 throughput = float3(1.0f, 1.0f, 1.0f);
        float3 l = float3(0.0f, 0.0f, 0.0f);
        float3 lf = float3(0.0f, 0.0f, 0.0f);

        [loop]
        for (int s = 0; s < kMultiScatteringSteps; ++s)
        {
            const float t = (float(s) + 0.5f) * dt;
            const float3 samplePos = pos + rayDir * t;
            const float sampleR = length(samplePos);
            const float altitude = sampleR - kBottomRadiusKm;

            float3 scattering, extinction;
            SampleMedium(altitude, scattering, extinction);

            const float3 stepTransmittance = exp(-extinction * dt);
            // 太陽から見たこの点の透過率(地面に遮られていれば0)
            const float3 up = samplePos / max(sampleR, 1e-6f);
            const float sunCos = dot(up, sunDir);
            const float3 sunTransmittance = SampleTransmittance(sampleR, sunCos);

            // 積分の解析形: ∫ T(t) σs dt を1ステップぶん閉形式で積む(Hillaireと同じ)
            const float3 safeExtinction = max(extinction, 1e-7f);
            const float3 integratedScattering =
                (scattering - scattering * stepTransmittance) / safeExtinction;

            l += throughput * integratedScattering * sunTransmittance * uniformPhase;
            // 「1で照らしたとき」の量。位相関数を掛けないのが等方近似の要点
            lf += throughput * integratedScattering;

            throughput *= stepTransmittance;
        }

        if (hitGround)
        {
            // 地面での反射。ランバート面なので cos / π
            const float3 hitPos = pos + rayDir * tMax;
            const float3 up = hitPos / max(length(hitPos), 1e-6f);
            const float sunCos = saturate(dot(up, sunDir));
            l += throughput * kGroundAlbedo * sunCos * SampleTransmittance(kBottomRadiusKm, sunCos) / kPI;
        }

        secondOrder += l;
        multiScatterAs1 += lf;
    }

    const float invDirections = 1.0f / float(kMultiScatteringDirections);
    // 球面全体の立体角4πを掛け戻す(一様サンプルの平均 × 4π)。
    // uniformPhase(=1/4π)と打ち消し合うので、ここは平均だけでよい
    secondOrder *= invDirections * 4.0f * kPI * uniformPhase;
    multiScatterAs1 *= invDirections * 4.0f * kPI * uniformPhase;

    // 無限次までの等比級数。公比が1に達することは物理的に無いが、
    // 数値誤差で1を超えると発散するのでクランプする
    const float3 ratio = min(multiScatterAs1, float3(0.9999f, 0.9999f, 0.9999f));
    const float3 multiScattering = secondOrder / (1.0f - ratio);

    MultiScatteringOut[dispatchThreadID.xy] = float4(multiScattering, 1.0f);
}
