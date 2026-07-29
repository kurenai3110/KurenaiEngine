// 最終合成パス。DirectLightingパスで計算済みの直接光(拡散+鏡面反射、シャドウ適用済み)を
// サンプルし、IBL(拡散イラディアンス+プリフィルタ済み鏡面、AO/SSILの遮蔽率を適用)・
// 間接拡散光(SSIL使用時)を加算する。PBRのライティング計算自体はDirectLighting.hlsl側/
// このパスのEvaluateIBLで行うため、SceneColorへの書き込みはバッファの合成として行う。
// 出力はHDR(SceneColor、1.0を超える輝度を保持)のままで、トーンマッピングは行わない。
// SSRパスがこのHDR値を反射元として参照するため、ここでLDRへ落とすとSSRの反射色が
// 1.0を超えられずエネルギー保存が破れる。トーンマッピングはPresent直前のTonemap.hlslで行う
#include "NormalEncoding.hlsli"
// スペキュラのマルチスキャッタリング・エネルギー補正(14.9節)
#include "SpecularEnergy.hlsli"

static const float PI = 3.14159265359f;

cbuffer FrameConstants : register(b0)
{
    float4x4 ViewProj;
    float4x4 InvViewProj;
    // カスケードシャドウマップ用(このシェーダでは未使用。オフセット合わせのためだけに宣言する)
    float4x4 CascadeViewProj[4];
    float4 CameraPosition;
    float4 LightDirection;
    float4 LightColor;
    float4x4 View;
    float4x4 Proj;
    // 昼夜サイクル用。rgb=環境光の色(m_AmbientScale乗算済み、KurenaiEngine3D::Render側の
    // constants.AmbientColor代入部を参照)、a=昼度(0=夜,1=昼)。IBLプリフィルタマップ自体は
    // スカイボックス(昼固定のテクスチャ)から焼いたものなので、夜の減光は背景スカイの表示と
    // 同様にAmbientColor.aでIBL全体を減衰させて近似する(KurenaiEngine3D::EvaluateIBL呼び出し側)。
    // Enable IBL無効時はIBL導入以前と同じ、rgbをそのまま定数色アンビエントとして使う(PSMain参照)
    float4 AmbientColor;
    // このシェーダでは未使用(オフセット合わせのためだけに宣言する)
    float4 CascadeSplits;
    // y: プリフィルタ済み鏡面マップの最大ミップレベル(ミップ数-1)。ラフネス[0,1]をミップ番号へ
    // 変換するのに使う(EvaluateIBL参照)。z: IBL強度倍率(m_IBLEnabled=falseなら0.0f。
    // PSMain側でこれが0以下の場合はEvaluateIBLの代わりにAmbientColor.rgbの定数色アンビエントへ
    // フォールバックする)。x/wはこのシェーダでは未使用
    float4 ShadowParams;
    // このシェーダでは未使用(オフセット合わせのためだけに宣言する。半透明パス専用)
    float4 ActiveLightCount;
    // 反射プローブ用。x=有効プローブ数(0ならプローブは一切使わずグローバルIBLのみ)、
    // y=影響範囲のデバッグ表示フラグ(1以上でプローブごとの色分け表示に切り替える)、
    // z=視差補正(box projection)の有効フラグ、w=プローブ間ブレンドの有効フラグ。
    // z/wはPhase 1(補正なし・ブレンドなし)との見比べのためにImGuiから切り替えられる
    float4 ProbeParams;
};

Texture2D AlbedoTexture : register(t0);
Texture2D DirectLightTexture : register(t1);
Texture2D MaterialTexture : register(t2);
Texture2D DepthTexture : register(t3);
TextureCube SkyboxTexture : register(t4);
// SSAO/SSIL(Visibility Bitmask)共通のAO/GIバッファ。rgb=間接拡散光(加算)、a=遮蔽率(乗算)
Texture2D AOTexture : register(t5);
// G-Bufferのエミッシブ(自発光)バッファ。AO/シャドウの影響を受けず常に加算する
Texture2D EmissiveTexture : register(t6);
// G-Bufferの法線(オクタヘドラルエンコード)。IBLの方向依存項(拡散イラディアンスのサンプル方向、
// 鏡面の反射方向)を求めるのに必要
Texture2D NormalTexture : register(t7);
// 拡散イラディアンス(本物のTextureCube。IBLConvolve.hlsl CSIrradianceが面ごとに書き込む)
TextureCube IrradianceTexture : register(t8);
// プリフィルタ済み鏡面(本物のTextureCube、ラフネスに応じたミップチェーン。CSPrefilterで生成)
TextureCube PrefilteredEnvTexture : register(t9);
// split-sum近似の第2項、BRDF積分LUT(x=NdotV, y=ラフネス。BRDFLUT.hlslで生成、方向性を持たない
// (NdotV, ラフネス)の2Dルックアップテーブルのため、これだけは通常のTexture2Dのまま)
Texture2D BRDFLUTTexture : register(t10);
// 反射プローブ(15章)。プローブごとにキャプチャ・畳み込んだ拡散イラディアンスとプリフィルタ済み鏡面を、
// TextureCubeArrayとして1枚にまとめて持つ。HLSLは別々のTextureCubeリソースを動的に添字参照できないため、
// ピクセルごとに異なるプローブを選ぶには配列でなければならない(カスケードシャドウマップがShadowMap0〜3を
// 個別スロットに分けて分岐しているのと同じ制約。プローブは数が可変なので分岐では書けない)
TextureCubeArray ProbeIrradianceTexture : register(t11);
TextureCubeArray ProbePrefilteredTexture : register(t12);

// プローブ1つぶんの影響範囲。C++側 KurenaiEngine3D.cpp の GPUReflectionProbe と
// 並び・ストライド(48バイト)を一致させる必要がある
struct GPUReflectionProbe
{
    float4 PositionRadius; // xyz=ワールド座標(Box形状では箱の中心), w=Sphere形状の影響半径
    float4 BoxExtents;     // xyz=Box形状の各軸の半径(ハーフエクステント), w=ブレンド距離
    float4 ShapeParams;    // x=形状(0=Sphere,1=Box), y=sin(Yaw), z=cos(Yaw), w=未使用
};
StructuredBuffer<GPUReflectionProbe> ReflectionProbes : register(t13);

// ワールド空間のベクトルをプローブのローカル空間(Yaw回転を打ち消した空間)へ移す。
// 回転はY軸まわりだけなので、逆回転は-Yawの回転(sinの符号反転)で足りる
float3 WorldToProbeLocal(float3 v, float sinYaw, float cosYaw)
{
    return float3(v.x * cosYaw - v.z * sinYaw, v.y, v.x * sinYaw + v.z * cosYaw);
}

// 影響範囲の内側で正、境界でちょうど0、外側で0になる重み。境界からBlendDistanceだけ内側へ
// 入った時点で1に達する。BlendDistanceが0に近いほどPhase 1の「境界で突然切り替わる」挙動に近づく
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
// ブレンド無効時(Phase 1相当)の選択に使う。境界に継ぎ目が出るのはこの方式の性質
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

// 環境ソース(拡散イラディアンス・プリフィルタ済み鏡面)を求める。影響下のプローブを重み付きで
// 合成し、重みの合計が1に満たない残りをスカイボックス由来のグローバルIBLで埋める。
// これによりプローブの影響範囲の外へ出るとき、境界で切り替わるのではなく徐々にグローバルIBLへ
// 戻っていく。プローブが1つも効いていない場合は従来どおり完全にグローバルIBLになる
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

            accumulatedIrradiance += ProbeIrradianceTexture.Sample(MaterialSampler, float4(N, i)).rgb * weight;
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
        // ブレンド無効(Phase 1相当): 影響範囲に含む最も近いプローブ1つだけを重み1で使う
        const int nearest = SelectNearestProbe(worldPos);
        if (nearest >= 0)
        {
            const GPUReflectionProbe probe = ReflectionProbes[nearest];
            const float3 sampleR = (parallaxEnabled && probe.ShapeParams.x > 0.5f)
                ? ParallaxCorrectDirection(probe, worldPos, R)
                : R;

            accumulatedIrradiance = ProbeIrradianceTexture.Sample(MaterialSampler, float4(N, nearest)).rgb;
            accumulatedPrefiltered = ProbePrefilteredTexture.SampleLevel(MaterialSampler, float4(sampleR, nearest), mipLevel).rgb;
            totalWeight = 1.0f;
        }
    }

    const float globalWeight = 1.0f - totalWeight;
    if (globalWeight > 0.0f)
    {
        accumulatedIrradiance += IrradianceTexture.Sample(MaterialSampler, N).rgb * globalWeight;
        accumulatedPrefiltered += PrefilteredEnvTexture.SampleLevel(MaterialSampler, R, mipLevel).rgb * globalWeight;
    }

    irradiance = accumulatedIrradiance;
    prefiltered = accumulatedPrefiltered;
}

// 影響範囲のデバッグ表示用の色。ブレンド有効時は重み付き平均の色になるため、プローブ同士が
// 混ざり合う遷移帯がグラデーションとして見える(ブレンド無効時は単色の塗り分けのまま)
float3 ProbeInfluenceDebugColor(float3 worldPos)
{
    // 番号を3bitとみなしてRGBへ散らす。隣り合う番号が必ず別の色になればよく、色自体に意味は無い
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

// IBL(split-sum近似、Karis 2013)による環境光の評価。ao(SSAO/SSILの遮蔽率)は拡散項へそのまま、
// 鏡面項へはLagarde & de Rousiers 2014のスペキュラオクルージョン近似を通してから適用する
// (拡散用のAOをそのまま鏡面に使うと、粗い面で鏡面ハイライトまで過剰に暗くなってしまうため)。
// 環境ソース(プローブとグローバルIBLの重み付き合成)はSampleEnvironmentが返す。プローブと
// グローバルIBLはどちらも同じ手順で焼かれており(IBLConvolve.hlslを共有している)、解像度・
// ミップ構成も揃えてあるため、式そのものは同一で引くキューブマップだけが変わる
float3 EvaluateIBL(float3 N, float3 V, float3 worldPos, float3 albedo, float metallic, float roughness, float ao)
{
    const float NdotV = saturate(dot(N, V));
    const float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);

    const float3 R = reflect(-V, N);
    // ShadowParams.y = プリフィルタ済み鏡面マップの最大ミップレベル(ミップ数-1、KurenaiEngine3D側で設定)
    const float mipLevel = roughness * ShadowParams.y;

    float3 irradiance;
    float3 prefiltered;
    SampleEnvironment(worldPos, N, R, mipLevel, irradiance, prefiltered);

    // --- 拡散IBL ---
    // ラフネスを考慮したFresnel-Schlick(Lagarde, "Moving Frostbite to PBR")。粗い面ほど
    // 視線に対するフレネルの立ち上がりが緩やかになる近似で、鏡面に回らない分をkdへ反映する
    const float3 fresnelRoughness = F0 + (max(float3(1.0f - roughness, 1.0f - roughness, 1.0f - roughness), F0) - F0) * pow(saturate(1.0f - NdotV), 5.0f);
    const float3 kd = (1.0f - fresnelRoughness) * (1.0f - metallic);
    const float3 diffuseIBL = kd * albedo * irradiance;

    // --- 鏡面IBL(split-sum近似) ---
    const float2 brdf = BRDFLUTTexture.Sample(ColorSampler, float2(NdotV, roughness)).rg;
    // マルチスキャッタリング・エネルギー補正(SpecularEnergy.hlsli、14.9節)。
    // ShadowParams.w = ImGuiトグル(0で無効=倍率1.0)
    const float3 specularIBL =
        prefiltered * (F0 * brdf.x + brdf.y) * SpecularEnergyCompensation(F0, brdf, ShadowParams.w);

    // スペキュラオクルージョン(Lagarde & de Rousiers, "Moving Frostbite to Physically Based
    // Rendering 3.0", 2014)。ラフネスが高いほど指数を1に近づけ、AOの効きを弱める
    const float specularOcclusionExponent = exp2(-16.0f * roughness - 1.0f);
    const float specularOcclusion = saturate(pow(NdotV + ao, specularOcclusionExponent) - 1.0f + ao);

    // 夜は空を暗い紺色へ落とし込む(背景スカイと同じ扱い)ため、IBL全体もAmbientColor.aで減衰させる。
    // プリフィルタマップ・イラディアンスマップ自体は昼固定のスカイボックスから焼いたものなので、
    // 夜間はこの係数だけが唯一の減光手段になる
    return (diffuseIBL * ao + specularIBL * specularOcclusion) * AmbientColor.a;
}

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

float4 PSMain(PSInput input) : SV_TARGET
{
    float depth = DepthTexture.Sample(DataSampler, input.UV).r;
    if (depth <= 0.0f)
    {
        // 何も描かれなかった背景ピクセル: カメラからそのピクセル方向への視線ベクトルで
        // 空のキューブマップをサンプリングする
        // Reverse-Zのため遠平面(=背景)はNDC z=0.0付近になる
        float3 farPoint = ReconstructWorldPos(input.UV, 0.0f);
        float3 rayDir = normalize(farPoint - CameraPosition.xyz);
        float3 skyColor = SkyboxTexture.Sample(MaterialSampler, rayDir).rgb;
        // 夜は空を暗い紺色へ落とし込む(スカイボックス自体は昼のテクスチャ固定のため)
        const float3 kNightSkyColor = float3(0.01f, 0.012f, 0.02f);
        skyColor = lerp(kNightSkyColor, skyColor, AmbientColor.a);
        return float4(skyColor, 1.0f);
    }

    float3 albedo = AlbedoTexture.Sample(ColorSampler, input.UV).rgb;
    float2 material = MaterialTexture.Sample(DataSampler, input.UV).rg;
    float metallic = material.r;
    float roughness = material.g;
    float3 diffuseColor = albedo * (1.0f - metallic);

    float3 worldPos = ReconstructWorldPos(input.UV, depth);
    float3 N = OctDecode(NormalTexture.Sample(DataSampler, input.UV).xy);
    float3 V = normalize(CameraPosition.xyz - worldPos);

    float4 aoSample = AOTexture.Sample(ColorSampler, input.UV);
    float ao = aoSample.a;
    float3 indirectLight = aoSample.rgb; // SSIL(Visibility Bitmask)使用時のみ非ゼロ。周囲のサーフェスからの間接拡散光
    float3 directLight = DirectLightTexture.Sample(ColorSampler, input.UV).rgb; // DirectLighting.hlslで計算済み(シャドウ適用済み)
    float3 emissive = EmissiveTexture.Sample(ColorSampler, input.UV).rgb;

    // ShadowParams.z = IBL強度倍率(0以下ならEnable IBL無効)。無効時はIBL導入以前と同じ、
    // 定数色(昼夜サイクルで変化するAmbientColor.rgb)による簡易アンビエントにフォールバックする
    // (何もライティングしない真っ暗な状態にはしない)。
    // EvaluateIBL内のirradianceはIBLConvolve.hlsl側で1/πと積分のπを相殺済みなのでそのままでよいが、
    // このフォールバックの定数色AmbientColorはその正規化を受けていないため、DirectLighting.hlslの
    // 拡散反射(kd*albedo/PI)とスケールを揃えるべくここで明示的に/PIする
    // (以前このフォールバックだけ/PIが抜けており、環境光がπ倍(意図の20%に対し実際は約65%)
    // 明るくなっていた)
    // ProbeParams.y = 影響範囲のデバッグ表示。どのプローブがどれだけ効いているかを色で
    // 塗り分けて返す(ライティングは行わない)。プローブの配置・形状・ブレンド幅の確認用
    if (ProbeParams.y > 0.0f)
    {
        return float4(ProbeInfluenceDebugColor(worldPos), 1.0f);
    }

    float3 ambient;
    if (ShadowParams.z > 0.0f)
    {
        // 反射プローブ(15章)はEvaluateIBL内のSampleEnvironmentで環境ソースへ合成される。
        // プローブが1つも効いていない位置では従来どおりスカイボックス由来のグローバルIBLになる
        ambient = EvaluateIBL(N, V, worldPos, albedo, metallic, roughness, ao) * ShadowParams.z;
    }
    else
    {
        ambient = (diffuseColor / PI) * AmbientColor.rgb * ao;
    }

    // エミッシブは自発光のためAO/シャドウの影響を受けず常に加算する。SSILの間接拡散光も
    // 受光面のランバート反射(diffuseColor/PI、非金属分)として正規化してから加算する
    float3 color = ambient + (diffuseColor / PI) * indirectLight + directLight + emissive;

    return float4(color, 1.0f);
}
