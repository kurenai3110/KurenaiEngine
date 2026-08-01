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
//   KURENAI_PROBE_DISTANCE_REGISTER       任意。距離キューブ(19.12節)を使う場合のみ
//
// 拡散側の2つを定義しなければ拡散イラディアンスのサンプルはコンパイルされず、
// SampleEnvironmentは常にirradiance=0を返す(SSRは鏡面しか要らないためこちらを使う)。
//
// KURENAI_PROBE_DISTANCE_REGISTERを定義しない場合、視差補正は箱との交差のみ、遮蔽判定は無しで
// コンパイルされる。ただし「LightingパスとSSRパスがまったく同じ環境ソースを見る」ことが
// 20章の前提なので、この2つは必ず同じ条件でコンパイルすること(片方だけ距離キューブを使うと、
// SSRが自分の足した覚えのない値を引き算することになる)。
//
// このヘッダーはFrameConstants(b0)の ProbeParams / ProbeParams2 / ShadowParams / AmbientColor /
// IBLParams を参照する(IBLParams.xは拡散側のマクロを定義した場合のみ。.zは常に参照する)。
// インクルードする側はこれらを含む形でFrameConstantsを宣言しておく必要がある。
// cbufferのレイアウトは宣言順で決まるため、
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
#ifdef KURENAI_PROBE_DISTANCE_REGISTER
// プローブ位置から各方向の被写体までのワールド距離(19.12節)。ジオメトリが無かった方向には
// 十分大きな値(IBLConvolve.hlslのkProbeSkyDistance)が入っている。
// サンプラーがDataSampler(Point)なのは、これが「色」ではなく「データ」だからである。
// 補間するとシルエットを跨いだタップが実在しない中間距離を作り、そこから求めた交点も遮蔽判定も
// どのジオメトリにも対応しない偽の値になる(Samplers.hlsliの区分に従う)
TextureCubeArray ProbeDistanceTexture : register(KURENAI_PROBE_DISTANCE_REGISTER);
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

// worldPosからR方向へ進んだとき、プローブの箱から抜け出るまでの距離(スラブ法)。
// 箱による視差補正の交点そのものであり、距離キューブ版(下)の探索範囲の上限にもなる
float ProbeBoxExitDistance(GPUReflectionProbe probe, float3 worldPos, float3 R)
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
    return max(min(min(exitDistance.x, exitDistance.y), exitDistance.z), 0.0f);
}

// 視差補正(box projection)。プローブのキューブマップは「プローブ位置1点から見た景色」なので、
// プローブ位置から離れたピクセルで反射ベクトルRをそのまま引くと、映る像の位置が実際の反射位置と
// ずれる(壁際なのに部屋の反対側が映る等)。Rをプローブの箱(部屋の壁に合わせて置く)と交差させ、
// その交点をプローブ中心から見た方向へ引き直すことでずれを打ち消す。
// Sphere形状には交差させる箱が無いため適用しない(Box形状にする動機のひとつ)
float3 ParallaxCorrectDirection(GPUReflectionProbe probe, float3 worldPos, float3 R)
{
    // 交点をプローブ中心から見た方向。キューブマップはワールド軸で焼かれているのでワールド空間で返す
    return (worldPos + R * ProbeBoxExitDistance(probe, worldPos, R)) - probe.PositionRadius.xyz;
}

#ifdef KURENAI_PROBE_DISTANCE_REGISTER
// 距離キューブを比較するときの許容誤差(19.12節)。
// キューブの1面はProbeParams2.zテクセルで[-1,1]をカバーするので、プローブから距離dの位置での
// 1テクセルの幅はおよそ 2d / size になる。シルエットを跨いだテクセルや量子化で記録距離が
// わずかに手前/奥へずれるぶんを吸収するため、2テクセルぶんを許容量とする。
// 下限があるのは、プローブのすぐ近く(d→0)で許容量が0に潰れて自己交差してしまうのを防ぐため
float ProbeDistanceBias(float distanceFromProbe)
{
    const float faceSize = max(ProbeParams2.z, 1.0f);
    return max(0.1f, distanceFromProbe * (4.0f / faceSize));
}

// worldPosがプローブから見て「記録された面より奥」にあるなら真。つまりそのピクセルは
// プローブの位置からは見えない(壁の向こう側にある)
bool ProbeIsBehindRecordedSurface(float3 toPoint, uint probeIndex)
{
    const float distanceFromProbe = length(toPoint);
    const float recorded = ProbeDistanceTexture.SampleLevel(DataSampler, float4(toPoint, probeIndex), 0.0f).r;
    return distanceFromProbe > recorded + ProbeDistanceBias(distanceFromProbe);
}

// 距離キューブを使った視差補正(19.12節)。
//
// 箱による視差補正(ParallaxCorrectDirection)は「部屋が直方体である」という仮定に立っているため、
// 実際の形状が箱からずれているほど反射像がずれる。距離キューブがあれば実形状と当てられる。
//
// 反射ベクトルRに沿って点を進めながら「プローブからその点までの距離」と「その方向にプローブが
// 記録している距離」を比べ、後者を追い越した区間を二分探索で詰める(キューブマップ版の
// リリーフマッピング)。探索範囲の上限には箱との交点をそのまま使う。箱は部屋に合わせて
// 置かれているので、その外側まで探しても意味が無いうえ、交差が見つからなかったときの
// フォールバックが自然に「従来の箱による補正」になるという利点がある
float3 ParallaxCorrectDirectionDepth(GPUReflectionProbe probe, float3 worldPos, float3 R, uint probeIndex)
{
    const float3 probeCenter = probe.PositionRadius.xyz;
    const float boxExitDistance = ProbeBoxExitDistance(probe, worldPos, R);

    const int kLinearSteps = 8;
    const int kRefineSteps = 4;
    const float stepSize = boxExitDistance / (float)kLinearSteps;

    float tNear = 0.0f;             // まだ記録面より手前だと分かっている位置
    float tFar = boxExitDistance;   // 交差が見つからなければ箱の交点をそのまま使う
    bool hit = false;

    [loop]
    for (int i = 1; i <= kLinearSteps; ++i)
    {
        const float t = stepSize * (float)i;
        if (ProbeIsBehindRecordedSurface((worldPos + R * t) - probeCenter, probeIndex))
        {
            tFar = t;
            hit = true;
            break;
        }
        tNear = t;
    }

    if (hit)
    {
        [loop]
        for (int j = 0; j < kRefineSteps; ++j)
        {
            const float tMid = 0.5f * (tNear + tFar);
            if (ProbeIsBehindRecordedSurface((worldPos + R * tMid) - probeCenter, probeIndex))
            {
                tFar = tMid;
            }
            else
            {
                tNear = tMid;
            }
        }
    }

    return (worldPos + R * tFar) - probeCenter;
}

// プローブから見てそのピクセルが見えているか(0=完全に隠れている、1=見えている)。
// 影響範囲の重みへ乗算することで、仕切り壁の向こう側の明るさが漏れてくるのを抑える。
// 硬い0/1ではなく1テクセルぶんの幅で滑らかに落とすのは、記録距離の量子化がそのまま
// 遮蔽の輪郭のちらつきになるのを防ぐため。
//
// 判定点は面の法線方向へbiasぶん浮かせる(DDGIのnormal biasと同じ考え方、Majercik et al. 2019)。
// これが無いと「プローブから見えている面が自分自身に遮蔽される」誤判定が起きる:
// 記録距離はテクセル中心の方向の値なので、そのテクセルが張る幅のぶんだけ手前へずれ得るためで、
// 実測ではこれが画面全体をわずかに暗くする形で現れた。
// 法線方向へ浮かせるとプローブ側から見て手前へ動くので、この誤判定だけが解消される
// (プローブが面の裏側にある場合は逆に遠ざかり、遮蔽が強まる。これは正しい挙動)
float ProbeVisibility(GPUReflectionProbe probe, float3 worldPos, float3 N, uint probeIndex)
{
    const float rawDistance = length(worldPos - probe.PositionRadius.xyz);
    const float bias = ProbeDistanceBias(rawDistance);

    const float3 toPoint = (worldPos + N * bias) - probe.PositionRadius.xyz;
    const float distanceFromProbe = length(toPoint);
    const float recorded = ProbeDistanceTexture.SampleLevel(DataSampler, float4(toPoint, probeIndex), 0.0f).r;

    // 記録面より手前(bias以内も含む)なら1。そこからbiasぶん奥へ行くまでに0へ落とす
    const float depthBehind = distanceFromProbe - recorded;
    return saturate(1.0f - (depthBehind - bias) / bias);
}
#endif // KURENAI_PROBE_DISTANCE_REGISTER

// 視差補正の入口。距離キューブが使える場合はProbeParams2.xで箱版と深度版を切り替える
// (ImGuiの Parallax: Box / Box + Depth に対応)。使えない場合は箱版だけがコンパイルされる
float3 ProbeParallaxDirection(GPUReflectionProbe probe, float3 worldPos, float3 R, uint probeIndex)
{
#ifdef KURENAI_PROBE_DISTANCE_REGISTER
    if (ProbeParams2.x > 0.5f)
    {
        return ParallaxCorrectDirectionDepth(probe, worldPos, R, probeIndex);
    }
#endif
    return ParallaxCorrectDirection(probe, worldPos, R);
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
            float weight = ProbeInfluenceWeight(probe, worldPos);
            if (weight <= 0.0f) continue;

#ifdef KURENAI_PROBE_DISTANCE_REGISTER
            // プローブから見えない位置(壁の向こう)のピクセルは重みを落とす(19.12節)。
            // 落ちたぶんは他のプローブとグローバルIBLが埋める
            if (ProbeParams2.y > 0.5f)
            {
                weight *= ProbeVisibility(probe, worldPos, N, i);
                if (weight <= 0.0f) continue;
            }
#endif

            // 拡散イラディアンスは低周波で位置による差が小さく、視差補正しても得られるものが
            // ほとんど無い一方で交差計算のコストは掛かるため、鏡面の反射ベクトルにのみ適用する
            const float3 sampleR = (parallaxEnabled && probe.ShapeParams.x > 0.5f)
                ? ProbeParallaxDirection(probe, worldPos, R, i)
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
                ? ProbeParallaxDirection(probe, worldPos, R, (uint)nearest)
                : R;

            float weight = 1.0f;
#ifdef KURENAI_PROBE_DISTANCE_REGISTER
            // ブレンド無効でも遮蔽は効かせる。落ちたぶんはグローバルIBLが埋める
            if (ProbeParams2.y > 0.5f)
            {
                weight = ProbeVisibility(probe, worldPos, N, (uint)nearest);
            }
#endif

#ifdef KURENAI_PROBE_IRRADIANCE_REGISTER
            accumulatedIrradiance = SampleProbeIrradiance(N, (uint)nearest) * weight;
#endif
            accumulatedPrefiltered = ProbePrefilteredTexture.SampleLevel(MaterialSampler, float4(sampleR, nearest), mipLevel).rgb * weight;
            totalWeight = weight;
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
//   brdf              BRDF積分LUTの値(x=A, y=B, z=Eavg)
//   compensationMode  ShadowParams.w(エネルギー補正の方式。0=無効/1=Linear/2=Series/3=Kulla-Conty)
//   iblIntensity      ShadowParams.z(IBL強度倍率。0ならIBL自体が無効)
//
// 【環境光の鏡面倍率(IBLParams.z)は引数で受けずここで直接読む】iblIntensityのように引数に
// すると、DeferredLightingとSSRが別々の値を渡してしまう余地が残る。この係数は上記のとおり
// 「両者が定義上必ず一致する」ことが存在理由なので、外から差し込める口を増やさない
//
// 【この係数が受け持つのは単一散乱(鏡面)ローブだけ】Kulla-Conty方式が足す加算ローブは
// プリフィルタ済み鏡面ではなく拡散イラディアンスに掛かるうえ、ほぼ拡散に近い広がりを持つため
// スクリーンスペース反射で差し替える対象ではない。よってそちらは
// SpecularIBLMultiScatterWeightとして分けてあり、SSRは触らない(14.9節)。
// 乗算型(Linear/Series)ではSpecularEnergyCompensationが倍率を返し加算項は0になるので、
// 従来どおりこの係数だけで完結する。
//
// かつてはここで昼度(AmbientColor.a)による夜間減衰も掛けていたが、手続き空の導入で
// 空自体が太陽高度に応じて暗くなるようになったため撤廃した(21.4節)。
// 掛けたままだと夜が二重に暗くなる
//
// 【遮蔽の引数について】materialAO(遮蔽マップのスカラー)とssao(スクリーンスペース側)を
// 分けて受け取り、bentとあわせてSpecularEnergy.hlsliのComposeSpecularOcclusionで合成する。
// soMode = 0なら従来どおりmaterialAO * ssaoを1回Frostbite近似へ通すだけになる。
//
// 【DeferredLightingとSSRへは必ず同じ値を渡すこと】この2つが定義上一致することが
// この関数の存在理由で、ズレるとSSRの適用領域と非適用領域の境界に段差が出る
float3 SpecularIBLWeight(float3 F0, float NdotV, float roughness,
                         int soMode, BentOcclusion bent, float3 N, float3 R,
                         float materialAO, float ssao, float3 brdf,
                         float compensationMode, float iblIntensity)
{
    // マルチスキャッタリング・エネルギー補正(SpecularEnergy.hlsli、14.9節)
    const int mode = (int)(compensationMode + 0.5f);
    const float3 splitSum = (F0 * brdf.x + brdf.y) * SpecularEnergyCompensation(F0, brdf, mode);

    // スペキュラオクルージョン。式はSpecularEnergy.hlsliに1つだけ置いてある
    // (半透明パス・プローブ焼き込みからも同じものを使うため)
    const float so = ComposeSpecularOcclusion(soMode, bent, N, R, NdotV, roughness, materialAO, ssao);
    return splitSum * so * iblIntensity * IBLParams.z;
}

// Kulla-Conty方式の加算ローブに掛かる係数。呼び出し側で拡散イラディアンスを乗算する:
//
//   マルチスキャッタぶん = 拡散イラディアンス * SpecularIBLMultiScatterWeight(...)
//
// 方式0/1/2ではSpecularMultiScatterIBLが0を返すため、この項は完全に消える。
// SSRはこの項を差し替えない(上のSpecularIBLWeightのコメント参照)。
//
// 【この加算ローブは鏡面倍率(IBLParams.z)の側に入れる】掛かる相手が拡散イラディアンスなので
// 拡散側に見えるが、これは鏡面BRDFが単散乱で取りこぼしたエネルギーを戻す項であって
// 拡散反射ではない。拡散側に入れると、鏡面倍率を0にしても鏡面由来の光が残ってしまう
float3 SpecularIBLMultiScatterWeight(float3 F0, float NdotV, float roughness,
                                     int soMode, BentOcclusion bent, float3 N, float3 R,
                                     float materialAO, float ssao, float3 brdf,
                                     float compensationMode, float iblIntensity)
{
    const int mode = (int)(compensationMode + 0.5f);
    const float3 FssEss = F0 * brdf.x + brdf.y;
    const float Ess = brdf.x + brdf.y;
    const float3 multiScatter = SpecularMultiScatterIBL(F0, FssEss, Ess, mode);

    const float so = ComposeSpecularOcclusion(soMode, bent, N, R, NdotV, roughness, materialAO, ssao);
    return multiScatter * so * iblIntensity * IBLParams.z;
}

#endif // KURENAI_REFLECTION_PROBE_HLSLI
