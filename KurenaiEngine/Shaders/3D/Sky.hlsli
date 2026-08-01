// 空モデル(CIE快晴空、Perez分布)の共有ヘッダー。
//
// 現在このヘッダーの利用者は2つある:
//   (a) SkyGenerate.hlsl      … IBL専用のキューブマップ(256px/面、ミップ無し)をベイクする
//   (b) DeferredLighting.hlsl … 深度が書かれていない背景画素を、画面解像度で直接評価する
//       (キューブマップは256px/面のため、3840px・水平画角68度のカメラでは約20倍に拡大表示され
//       背景としては解像度が足りない。IBLは畳み込むため低解像度のままで正しい)
// 将来ここへ雲(P5)・Preethamモデルへの置き換え(P7)・大気遠近(P8)が乗る予定で、
// それらもこの1つの定義を経由することで背景・IBL・水面反射が同じ空を見ることを保証する。
//
// このファイルの式は Tools/generate_sky_cubemap.py(オフラインの参照実装 兼 手続き空を
// 無効にしたときのフォールバック)と、KurenaiEngine3D.cpp のCPUミラー
// (ComputeSkyTint / SkyRelativeLuminance / SkyTint / SunGlowWeight / ComputeSkyZenithScale)と
// 常に一致させる必要がある。係数・定数を変える場合は必ずそれらも同時に直すこと。
//
// 【cbufferに依存しない】呼び出し側ごとにcbufferのレイアウトが異なるため
// (SkyGenerate.hlslはSkyBakeConstantsから、DeferredLighting.hlslはFrameConstantsから)、
// 必要な値はすべてSkyParameters構造体で受け取る。PIも定義しない
// (DeferredLighting.hlslが既に自前でPIを定義しており、ここでも定義すると再定義エラーになるうえ、
// 空モデルの関数群はPIを使わないので不要)
#ifndef KURENAI_SKY_HLSLI
#define KURENAI_SKY_HLSLI

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

// 空モデルの評価に必要なパラメータ一式。呼び出し側が自分のcbufferから組み立てて渡す
// (SkyGenerate.hlslはSkyBakeConstantsから、DeferredLighting.hlslはFrameConstantsから)
struct SkyParameters
{
    float3 SunDirection;     // 太陽が「ある」向き(正規化済み)。光が進む向きとは符号が逆
    float  ZenithLuminance;  // 天頂輝度。実効プリ露出を掛けた後の値
    float3 ZenithTint;
    float3 HorizonTint;
    float3 GroundTint;
    float3 SunGlowTint;
    float  SunGlowStrength;  // 太陽の暖色の強さ(仰角0度で1、±15度で0)
};

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
float3 SkyTint(float cosTheta, float cosGamma, SkyParameters params)
{
    // 水平線側への寄せを3乗カーブにして、高度があるうちは天頂色をほぼ保つ
    const float horizonBlend = pow(1.0f - saturate(cosTheta), 3.0f);
    const float3 base = lerp(params.ZenithTint, params.HorizonTint, horizonBlend);
    return lerp(base, params.SunGlowTint, SunGlowWeight(cosGamma, params.SunGlowStrength));
}

// 水平線以上を仮定した空の色(呼び出し側で地面フェードと合成する)
float3 SkyColorUpper(float3 dir, SkyParameters params)
{
    // Perez分布は水平線で不安定になるため天頂角を89.5度までにクランプする
    const float clampedY = max(dir.y, cos(radians(89.5f)));
    const float cosTheta = clamp(clampedY, 1e-3f, 1.0f);

    const float thetaSun = acos(clamp(params.SunDirection.y, -1.0f, 1.0f));
    const float cosThetaSun = max(cos(thetaSun), 1e-3f);

    const float cosGamma = clamp(dot(dir, params.SunDirection), -1.0f, 1.0f);
    const float gamma = acos(cosGamma);

    float relative = max(PerezRelativeLuminance(cosTheta, gamma, cosThetaSun, thetaSun), 0.0f);
    relative = kRelativeLuminanceFloor + (1.0f - kRelativeLuminanceFloor) * relative;

    return relative * params.ZenithLuminance * SkyTint(cosTheta, cosGamma, params);
}

float3 SkyColor(float3 dir, SkyParameters params)
{
    if (dir.y >= kGroundFadeStartY)
    {
        return SkyColorUpper(dir, params);
    }

    // 水平線より下: プラトー色(kGroundFadeStartYの高さへ射影した方向の空色)から接地色へフェード
    float3 plateauDir = dir;
    plateauDir.y = kGroundFadeStartY;
    plateauDir = normalize(plateauDir);
    const float3 plateauColor = SkyColorUpper(plateauDir, params);

    const float3 groundColor = params.ZenithLuminance * params.GroundTint;
    const float groundT = saturate((dir.y - kGroundFadeStartY) / (kGroundFadeEndY - kGroundFadeStartY));
    return lerp(plateauColor, groundColor, groundT);
}

#endif // KURENAI_SKY_HLSLI
