// 空の天頂輝度スケールを、上半球の余弦重み積分が目標照度に一致するよう正規化して求める
// コンピュートシェーダー(P9)。以前はKurenaiEngine3D.cppのComputeSkyZenithScaleがCPUで
// θ64分割×φ256分割=16,384サンプルの積分を行い、その結果(天頂輝度・ティント4本)を
// FrameConstants/SkyBakeConstants経由で配っていた。この式はSky.hlsli側にも同じものが
// 二重実装されており、「片方を直したら必ずもう片方も直す」という規約でしか整合が保てなかった。
// P9でこの積分をGPU側(このファイル)へ一本化し、CPUミラーは削除した。
//
// 【なぜ正規化が必要か】従来は zenith_luminance = 空光の照度[lx] をそのまま天頂輝度として
// 使っていた。照度E[lx]と輝度L[cd/m^2]は E = ∫L・cosθ dω の関係にあるので、この扱いだと
// 実際に届く照度は「積分値の分だけ」ずれる。しかもPerez分布の形は太陽高度で変わるため、
// そのずれ自体が時刻とともに動く。
//
// 正規化前は、Perez分布の形が太陽高度で変わるぶんだけ空光の照度が1.8倍も勝手に変動して
// いた(輝度フロア0.45・旧ティストでの実測。太陽高度90度で積分1.080、45度で1.898)。
// ここで正規化すると常に目標値ちょうどになり、時刻による空の明るさは薄明係数のように
// 意図した係数だけで制御できるようになる。
// 積分値そのものはフロアとティントを変えると当然変わるが、正規化しているので
// 最終的な照度は変わらない(だから上の実測値は現在の設定のものではない)。
//
// 補足: 「一様な空なら L = E/π なので従来はπ倍明るかった」という説明は誤り。
// 積分にはティントの輝度成分(Rec.709)も入るため、単位球の積分はπ(3.14)には遠く
// 及ばない。正午での補正は数%〜十数%の範囲にとどまる。
//
// 【GPU実装の注意】HLSLのInterlockedAddはSM6.6未満では整数専用でfloatを加算できないため、
// groupshared配列へ各スレッドの部分和を書き、GroupMemoryBarrierWithGroupSync()を挟んだ
// ツリー加算(ペアワイズ和、256→128→…→1)で集約する。CPU版はdoubleで累積していたが、
// HLSLにdoubleの実用的な手段はないためfloatのツリー加算で行う(16,384個の正の項に対する
// 相対誤差はオーダーとしては小さい見込みだが、これは見込みであって測定値ではない)。
//
// スレッド構成は1グループ×256スレッド、Dispatch(1,1,1)固定。スレッドi(i=0..255)がφ
// インデックスiを担当し、θを64ステップぶん積む(1スレッドあたり64サンプル、合計16,384で
// CPU版と厳密に同じサンプル位置)
#include "Sky.hlsli"

cbuffer SkyIntegrateConstants : register(b0)
{
    // xyz=太陽が「ある」向き(正規化済み)、w=未使用
    float4 SunDirection;
    // x=目標照度[lx](SunLighting::SkyIlluminanceLux)、y=実効プリ露出(effectiveExposure)、zw=未使用
    float4 IntegrateParams;
};

RWStructuredBuffer<GPUSkyParameters> SkyParametersOut : register(u0);

static const uint kThetaSteps = 64;
static const uint kPhiSteps = 256;
static const float kSkyIntegratePI = 3.14159265359f;

// 各スレッド(=φインデックス)の部分和をここへ書き、ツリー加算で集約する
groupshared float s_Partial[256];

[numthreads(256, 1, 1)]
void CSIntegrateSky(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const uint phiIndex = dispatchThreadID.x;

    // 呼び出し側の慣習(SkyGenerate.hlsl等)に合わせ、受け取った向きはここで正規化する
    const float3 sunPosition = normalize(SunDirection.xyz);
    const float thetaSun = acos(clamp(sunPosition.y, -1.0f, 1.0f));
    const float cosThetaSun = max(cos(thetaSun), 1e-3f);

    const float dTheta = (kSkyIntegratePI * 0.5f) / float(kThetaSteps);
    const float dPhi = (kSkyIntegratePI * 2.0f) / float(kPhiSteps);

    // 色味はθ・φに依存しないため1度だけ求める(CPU版ComputeSkyZenithScaleと同じく
    // 呼び出し元が1度だけ決めてループへ渡す構造)
    const SkyTintSet tintSet = ComputeSkyTintSet(sunPosition.y);

    const float phi = (float(phiIndex) + 0.5f) * dPhi;

    // 中点則でθ64ステップぶん積む(1スレッドあたり64サンプル)
    float partialSum = 0.0f;
    [loop]
    for (uint ti = 0; ti < kThetaSteps; ++ti)
    {
        const float theta = (float(ti) + 0.5f) * dTheta;
        const float cosThetaRaw = cos(theta);
        const float sinTheta = sin(theta);
        // SkyColorUpperと同じクランプ(水平線でPerezが発散するため)
        const float cosTheta = clamp(max(cosThetaRaw, cos(radians(89.5f))), 1e-3f, 1.0f);

        const float3 dir = float3(sinTheta * cos(phi), cosThetaRaw, sinTheta * sin(phi));
        const float cosGamma = clamp(dot(dir, sunPosition), -1.0f, 1.0f);
        const float gamma = acos(cosGamma);

        // 照度は測光的な輝度で測るので、ティントの輝度成分(Rec.709)を重みに掛ける
        const float3 tint = SkyTintFromSet(cosTheta, cosGamma, tintSet);
        const float tintLuminance = 0.2126f * tint.x + 0.7152f * tint.y + 0.0722f * tint.z;

        float relative = max(PerezRelativeLuminance(cosTheta, gamma, cosThetaSun, thetaSun), 0.0f);
        // フロア適用後の値(SkyColorUpperと同じ式)
        relative = kRelativeLuminanceFloor + (1.0f - kRelativeLuminanceFloor) * relative;

        // dω = sinθ dθ dφ、余弦重みはcosθ(クランプ前のcosThetaRawを使う。CPU版と同じ)
        partialSum += relative * tintLuminance * cosThetaRaw * sinTheta * dTheta * dPhi;
    }

    s_Partial[phiIndex] = partialSum;
    GroupMemoryBarrierWithGroupSync();

    // ツリー加算(ペアワイズ和)。256要素を128→64→…→1まで畳み込む
    [unroll]
    for (uint stride = 128; stride > 0; stride >>= 1)
    {
        if (phiIndex < stride)
        {
            s_Partial[phiIndex] += s_Partial[phiIndex + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    // スレッド0が最終値をまとめてバッファへ書く
    if (phiIndex == 0)
    {
        const float integral = s_Partial[0];
        const float targetIlluminance = IntegrateParams.x;
        const float effectiveExposure = IntegrateParams.y;

        // 積分がゼロ近傍になることは無い想定だが、ゼロ除算だけは防いでおく
        // (CPU版ComputeSkyZenithScaleの同じ判定。警告ログはHLSLから出せないため、
        // 検証は積分値そのもの(Luminance.y)をバッファへ残すことで行う)
        const float zenithLuminance =
            (integral < 1e-6f ? targetIlluminance : targetIlluminance / integral) * effectiveExposure;

        GPUSkyParameters result;
        result.ZenithTint = float4(tintSet.Zenith, 0.0f);
        result.HorizonTint = float4(tintSet.Horizon, 0.0f);
        result.GroundTint = float4(tintSet.Ground, 0.0f);
        result.SunGlowTint = float4(tintSet.SunGlow, tintSet.SunGlowStrength);
        result.Luminance = float4(zenithLuminance, integral, 0.0f, 0.0f);

        SkyParametersOut[0] = result;
    }
}
