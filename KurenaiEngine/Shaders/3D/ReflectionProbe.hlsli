// 反射プローブ(リフレクションプローブ、19章)の共有ヘッダー。
//
// DeferredLighting.hlsl(鏡面IBLを適用する側)とSSR.hlsl(その鏡面IBLをスクリーンスペースの
// 反射で差し替える側)の両方が、まったく同じ環境ソースとまったく同じBRDF重みを使う必要がある。
// 片方だけ式が変わると、SSRが「自分が引いた覚えのない値」を引き算することになって破綻するため、
// プローブの選択・視差補正・ブレンドと鏡面IBLの重み計算はこのヘッダーに1か所だけ置く。
//
// リソースのレジスタ番号は2つのシェーダーで異なる(それぞれ既に使っているスロットが違う)ため、
// インクルードする側が以下のマクロを定義してからインクルードする。
//
//   KURENAI_GLOBAL_PREFILTERED_REGISTER   必須。スカイボックス由来のプリフィルタ済み鏡面(TextureCube)
//   KURENAI_PROBE_PREFILTERED_REGISTER    必須。プローブのプリフィルタ済み鏡面(TextureCubeArray)
//   KURENAI_PROBE_BUFFER_REGISTER         必須。プローブの影響範囲(StructuredBuffer)
//   KURENAI_GLOBAL_IRRADIANCE_REGISTER    任意。拡散イラディアンスも要る場合のみ
//   KURENAI_PROBE_IRRADIANCE_REGISTER     任意。同上(プローブ側)
//
// 拡散側の2つを定義しなければ拡散イラディアンスのサンプルはコンパイルされず、
// SampleEnvironmentは常にirradiance=0を返す(SSRは鏡面しか要らないためこちらを使う)。
//
// このヘッダーはFrameConstants(b0)の ProbeParams / ShadowParams / AmbientColor / IBLParams を
// 参照する(IBLParamsは拡散側のマクロを定義した場合のみ)。インクルードする側はこれらを含む形で
// FrameConstantsを宣言しておく必要がある。cbufferのレイアウトは宣言順で決まるため、
// 途中のフィールドを飛ばさずC++側 KurenaiEngine3D.cpp の FrameConstants と並びを一致させること。
#ifndef KURENAI_REFLECTION_PROBE_HLSLI
#define KURENAI_REFLECTION_PROBE_HLSLI

#include "SpecularEnergy.hlsli"

// --- リソース ---
TextureCube PrefilteredEnvTexture : register(KURENAI_GLOBAL_PREFILTERED_REGISTER);
TextureCubeArray ProbePrefilteredTexture : register(KURENAI_PROBE_PREFILTERED_REGISTER);

#ifdef KURENAI_GLOBAL_IRRADIANCE_REGISTER
TextureCube IrradianceTexture : register(KURENAI_GLOBAL_IRRADIANCE_REGISTER);
#endif
#ifdef KURENAI_PROBE_IRRADIANCE_REGISTER
TextureCubeArray ProbeIrradianceTexture : register(KURENAI_PROBE_IRRADIANCE_REGISTER);
#endif

// プローブ1つぶんの影響範囲。C++側 KurenaiEngine3D.cpp の GPUReflectionProbe と
// 並び・ストライド(48バイト)を一致させる必要がある
struct GPUReflectionProbe
{
    float4 PositionRadius; // xyz=ワールド座標(Box形状では箱の中心), w=Sphere形状の影響半径
    float4 BoxExtents;     // xyz=Box形状の各軸の半径(ハーフエクステント), w=ブレンド距離
    float4 ShapeParams;    // x=形状(0=Sphere,1=Box), y=sin(Yaw), z=cos(Yaw), w=未使用
};
StructuredBuffer<GPUReflectionProbe> ReflectionProbes : register(KURENAI_PROBE_BUFFER_REGISTER);

// ワールド空間のベクトルをプローブのローカル空間(Yaw回転を打ち消した空間)へ移す。
// 回転はY軸まわりだけなので、逆回転は-Yawの回転(sinの符号反転)で足りる
float3 WorldToProbeLocal(float3 v, float sinYaw, float cosYaw)
{
    return float3(v.x * cosYaw - v.z * sinYaw, v.y, v.x * sinYaw + v.z * cosYaw);
}

// 影響範囲の内側で正、境界でちょうど0、外側で0になる重み。境界からBlendDistanceだけ内側へ
// 入った時点で1に達する。BlendDistanceが0に近いほど「境界で突然切り替わる」挙動に近づく
float ProbeInfluenceWeight(GPUReflectionProbe probe, float3 worldPos)
{
    const float blendDistance = max(probe.BoxExtents.w, 1e-4f);

    if (probe.ShapeParams.x > 0.5f)
    {
        // Box(OBB): ローカル空間で各軸の面までの距離を取り、最も近い面までの距離で重みを決める。
        // 1軸でも箱の外に出ていればその軸の距離が負になり、minを通してsaturateで0になる
        const float3 local = WorldToProbeLocal(worldPos - probe.PositionRadius.xyz, probe.ShapeParams.y, probe.ShapeParams.z);
        const float3 toBoundary = probe.BoxExtents.xyz - abs(local);
        const float distanceToBoundary = min(min(toBoundary.x, toBoundary.y), toBoundary.z);
        return saturate(distanceToBoundary / blendDistance);
    }

    // Sphere: 中心からの距離が半径を超えれば負になり、同様に0となる
    const float distanceToCenter = length(worldPos - probe.PositionRadius.xyz);
    return saturate((probe.PositionRadius.w - distanceToCenter) / blendDistance);
}

// ワールド座標を影響範囲に含むプローブのうち、中心が最も近いものの番号を返す(無ければ-1)。
// ブレンド無効時の選択に使う。境界に継ぎ目が出るのはこの方式の性質
int SelectNearestProbe(float3 worldPos)
{
    int selected = -1;
    float bestDistSq = 3.402823466e+38f; // FLT_MAX

    const uint probeCount = (uint)ProbeParams.x;
    [loop]
    for (uint i = 0; i < probeCount; ++i)
    {
        if (ProbeInfluenceWeight(ReflectionProbes[i], worldPos) <= 0.0f) continue;

        const float3 toProbe = ReflectionProbes[i].PositionRadius.xyz - worldPos;
        const float distSq = dot(toProbe, toProbe);
        if (distSq < bestDistSq)
        {
            bestDistSq = distSq;
            selected = (int)i;
        }
    }

    return selected;
}

// 視差補正(box projection)。プローブのキューブマップは「プローブ位置1点から見た景色」なので、
// プローブ位置から離れたピクセルで反射ベクトルRをそのまま引くと、映る像の位置が実際の反射位置と
// ずれる(壁際なのに部屋の反対側が映る等)。Rをプローブの箱(部屋の壁に合わせて置く)と交差させ、
// その交点をプローブ中心から見た方向へ引き直すことでずれを打ち消す。
// Sphere形状には交差させる箱が無いため適用しない(Box形状にする動機のひとつ)
float3 ParallaxCorrectDirection(GPUReflectionProbe probe, float3 worldPos, float3 R)
{
    const float sinYaw = probe.ShapeParams.y;
    const float cosYaw = probe.ShapeParams.z;
    const float3 localPos = WorldToProbeLocal(worldPos - probe.PositionRadius.xyz, sinYaw, cosYaw);
    const float3 localR = WorldToProbeLocal(R, sinYaw, cosYaw);

    // 軸に平行な成分は対応する面と交差しない。0除算のinfをそのまま使うと後段のmaxで0*inf=NaNに
    // なり得るため、符号を保ったまま絶対値に下限を与えてから逆数を取る
    const float3 safeR = max(abs(localR), 1e-5f) * ((localR < 0.0f) ? -1.0f : 1.0f);
    const float3 invR = 1.0f / safeR;
    const float3 planeNegative = (-probe.BoxExtents.xyz - localPos) * invR;
    const float3 planePositive = (probe.BoxExtents.xyz - localPos) * invR;
    // 軸ごとに「Rの進む向きにある面」までの距離を取り、その最小値が箱から抜け出る点になる
    const float3 exitDistance = max(planeNegative, planePositive);
    const float hitDistance = min(min(exitDistance.x, exitDistance.y), exitDistance.z);

    // 交点をプローブ中心から見た方向。キューブマップはワールド軸で焼かれているのでワールド空間で返す
    return (worldPos + R * max(hitDistance, 0.0f)) - probe.PositionRadius.xyz;
}

// 拡散イラディアンスの取得元。既定(IBLParams.x = 0)ではプリフィルタ済み鏡面の最終ミップ
// (roughness=1、ShadowParams.yがそのミップ番号)を法線方向で引く。CSPrefilterはV=R=Nを仮定して
// いるため、roughness=1(α=1)ではGGXインポータンスサンプリングの実効カーネルがコサイン畳み込みへ
// 厳密に退化し、格納値もCSIrradianceと同じE(N)/πになる。近似ではなく等価であり、専用の
// イラディアンスマップを焼く必要がない(14.10節)。IBLParams.x=1のときだけ従来の専用マップを引く。
// この規則はグローバルIBLとプローブの両方へまったく同じように適用する
#ifdef KURENAI_GLOBAL_IRRADIANCE_REGISTER
float3 SampleGlobalIrradiance(float3 N)
{
    return (IBLParams.x > 0.5f)
        ? IrradianceTexture.Sample(MaterialSampler, N).rgb
        : PrefilteredEnvTexture.SampleLevel(MaterialSampler, N, ShadowParams.y).rgb;
}
#endif
#ifdef KURENAI_PROBE_IRRADIANCE_REGISTER
float3 SampleProbeIrradiance(float3 N, uint probeIndex)
{
    return (IBLParams.x > 0.5f)
        ? ProbeIrradianceTexture.Sample(MaterialSampler, float4(N, probeIndex)).rgb
        : ProbePrefilteredTexture.SampleLevel(MaterialSampler, float4(N, probeIndex), ShadowParams.y).rgb;
}
#endif

// 環境ソース(拡散イラディアンス・プリフィルタ済み鏡面)を求める。影響下のプローブを重み付きで
// 合成し、重みの合計が1に満たない残りをスカイボックス由来のグローバルIBLで埋める。
// これによりプローブの影響範囲の外へ出るとき、境界で切り替わるのではなく徐々にグローバルIBLへ
// 戻っていく。プローブが1つも効いていない場合は従来どおり完全にグローバルIBLになる。
// 拡散側のレジスタが未定義の場合、irradianceは常に0が返る(鏡面しか要らない呼び出し側向け)
void SampleEnvironment(float3 worldPos, float3 N, float3 R, float mipLevel,
                       out float3 irradiance, out float3 prefiltered)
{
    float3 accumulatedIrradiance = float3(0.0f, 0.0f, 0.0f);
    float3 accumulatedPrefiltered = float3(0.0f, 0.0f, 0.0f);
    float totalWeight = 0.0f;

    const uint probeCount = (uint)ProbeParams.x;
    const bool parallaxEnabled = ProbeParams.z > 0.5f;

    if (ProbeParams.w > 0.5f)
    {
        // ブレンド有効: 影響下の全プローブを重み付きで加算する。プローブ数の上限が8と小さいため、
        // 上位N個を選ぶソートは置かず素直に全走査する
        [loop]
        for (uint i = 0; i < probeCount; ++i)
        {
            const GPUReflectionProbe probe = ReflectionProbes[i];
            const float weight = ProbeInfluenceWeight(probe, worldPos);
            if (weight <= 0.0f) continue;

            // 拡散イラディアンスは低周波で位置による差が小さく、視差補正しても得られるものが
            // ほとんど無い一方で交差計算のコストは掛かるため、鏡面の反射ベクトルにのみ適用する
            const float3 sampleR = (parallaxEnabled && probe.ShapeParams.x > 0.5f)
                ? ParallaxCorrectDirection(probe, worldPos, R)
                : R;

#ifdef KURENAI_PROBE_IRRADIANCE_REGISTER
            accumulatedIrradiance += SampleProbeIrradiance(N, i) * weight;
#endif
            accumulatedPrefiltered += ProbePrefilteredTexture.SampleLevel(MaterialSampler, float4(sampleR, i), mipLevel).rgb * weight;
            totalWeight += weight;
        }

        // プローブが深く重なっている領域では合計が1を超える。そのまま加算すると環境光が過剰に
        // 明るくなるので、超えた分は正規化して1に収める
        if (totalWeight > 1.0f)
        {
            accumulatedIrradiance /= totalWeight;
            accumulatedPrefiltered /= totalWeight;
            totalWeight = 1.0f;
        }
    }
    else
    {
        // ブレンド無効: 影響範囲に含む最も近いプローブ1つだけを重み1で使う
        const int nearest = SelectNearestProbe(worldPos);
        if (nearest >= 0)
        {
            const GPUReflectionProbe probe = ReflectionProbes[nearest];
            const float3 sampleR = (parallaxEnabled && probe.ShapeParams.x > 0.5f)
                ? ParallaxCorrectDirection(probe, worldPos, R)
                : R;

#ifdef KURENAI_PROBE_IRRADIANCE_REGISTER
            accumulatedIrradiance = SampleProbeIrradiance(N, (uint)nearest);
#endif
            accumulatedPrefiltered = ProbePrefilteredTexture.SampleLevel(MaterialSampler, float4(sampleR, nearest), mipLevel).rgb;
            totalWeight = 1.0f;
        }
    }

    const float globalWeight = 1.0f - totalWeight;
    if (globalWeight > 0.0f)
    {
#ifdef KURENAI_GLOBAL_IRRADIANCE_REGISTER
        accumulatedIrradiance += SampleGlobalIrradiance(N) * globalWeight;
#endif
        accumulatedPrefiltered += PrefilteredEnvTexture.SampleLevel(MaterialSampler, R, mipLevel).rgb * globalWeight;
    }

    irradiance = accumulatedIrradiance;
    prefiltered = accumulatedPrefiltered;
}

// 影響範囲のデバッグ表示用の色。ブレンド有効時は重み付き平均の色になるため、プローブ同士が
// 混ざり合う遷移帯がグラデーションとして見える(ブレンド無効時は単色の塗り分けのまま)
float3 ProbeInfluenceDebugColor(float3 worldPos)
{
    const float3 noProbeColor = float3(0.15f, 0.15f, 0.15f); // どのプローブも効いていない(グローバルIBL)

    const uint probeCount = (uint)ProbeParams.x;
    if (ProbeParams.w <= 0.5f)
    {
        const int nearest = SelectNearestProbe(worldPos);
        if (nearest < 0) return noProbeColor;
        return float3(
            ((nearest + 1) & 1) ? 1.0f : 0.25f,
            ((nearest + 1) & 2) ? 1.0f : 0.25f,
            ((nearest + 1) & 4) ? 1.0f : 0.25f);
    }

    float3 accumulated = float3(0.0f, 0.0f, 0.0f);
    float totalWeight = 0.0f;
    [loop]
    for (uint i = 0; i < probeCount; ++i)
    {
        const float weight = ProbeInfluenceWeight(ReflectionProbes[i], worldPos);
        if (weight <= 0.0f) continue;

        // 番号を3bitとみなしてRGBへ散らす。隣り合う番号が必ず別の色になればよく、色自体に意味は無い
        const int index = (int)i + 1;
        accumulated += float3(
            (index & 1) ? 1.0f : 0.25f,
            (index & 2) ? 1.0f : 0.25f,
            (index & 4) ? 1.0f : 0.25f) * weight;
        totalWeight += weight;
    }

    if (totalWeight > 1.0f)
    {
        accumulated /= totalWeight;
        totalWeight = 1.0f;
    }
    return accumulated + noProbeColor * (1.0f - totalWeight);
}

// 鏡面IBLの「放射輝度に掛かる係数」をまとめて返す。
//
//   鏡面IBL = 環境の放射輝度 * SpecularIBLWeight(...)
//
// という形に分解しておくことで、SSRは放射輝度だけを差し替えて
//   出力 = SceneColor + (SSRの放射輝度 - 環境の放射輝度) * SpecularIBLWeight(...)
// と書ける。DeferredLightingが適用した量とSSRが引き算する量が定義上必ず一致するように、
// この係数の定義はここ1か所しか持たない(20章)。
//
//   brdf                       BRDF積分LUT(split-sum近似の第2項)の値
//   energyCompensationEnabled  ShadowParams.w(マルチスキャッタリング補正のトグル)
//   iblIntensity               ShadowParams.z(IBL強度倍率。0ならIBL自体が無効)
//
// かつてはここで昼度(AmbientColor.a)による夜間減衰も掛けていたが、手続き空の導入で
// 空自体が太陽高度に応じて暗くなるようになったため撤廃した(21.4節)。
// 掛けたままだと夜が二重に暗くなる
float3 SpecularIBLWeight(float3 F0, float NdotV, float roughness, float ao, float2 brdf,
                         float energyCompensationEnabled, float iblIntensity)
{
    // マルチスキャッタリング・エネルギー補正(SpecularEnergy.hlsli、14.9節)
    const float3 splitSum = (F0 * brdf.x + brdf.y) * SpecularEnergyCompensation(F0, brdf, energyCompensationEnabled);

    // スペキュラオクルージョン。式はSpecularEnergy.hlsliに1つだけ置いてある
    // (半透明パス・プローブ焼き込みからも同じものを使うため)
    return splitSum * SpecularOcclusion(NdotV, roughness, ao) * iblIntensity;
}

#endif // KURENAI_REFLECTION_PROBE_HLSLI
