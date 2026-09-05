// MegaLights クアッド共有(手法3)の解決パス。2x2 クアッドの4画素がそれぞれ引いた標本を、
// **自分の面で評価し直して単純平均**する。レイは1本も撃たない。
//
// 【何を解こうとしているか】手法2(ReSTIR DI)は、厳密な不偏性を保ったまま近傍のサンプルを
// 再利用するために可視レイとバイアス補正レイを撃つ。実測(BistroExteriorNight 107灯 /
// 1280x720 / RTX 4070 Ti)で MegaLights 合計 4.26ms のうち MegaLightsSpatial だけで 2.64ms
// を占め、**全灯総当たりの参照実装(4.21ms)と同じコストになっていた**。
// レイの本数を灯数から切り離すという手法の主張が成立していない。
//
// UE5 の MegaLights は「候補ごとに可視性レイが要る」という理由で ReSTIR を採らず、
// 固定本数のレイ + 重要度サンプリング + 時間フィードバック + デノイザで解いている。
// このパスはその構造を採る。1画素が撃つ影レイは Initial の1本だけで、
// 再利用のための追加レイは**1本も撃たない**。
//
// 【推定量】画素 x について、x を含む 2x2 クアッド Q の4標本 y_j を
//
//     L(x) = (1/n) * sum_{j in Q, 幾何ゲート通過} f_x(y_j) * V_j * W_j
//
// で合成する。f_x は **x 自身の面**での寄与(隣で良かった灯が x で良いとは限らない)、
// V_j は**仲間 j が撃ったレイの結果**、W_j は j のリザーバが持つ不偏寄与重みである。
//
// 【なぜ不偏なのか】RIS の性質 E[g(y_j) * W_j] = sum_i g(i) は**任意の g** について成り立つ。
// g = f_x * V(x, ・) と置けば各項が sum_i f_x(i) V(x,i) の不偏推定量になる。n は幾何だけで
// 決まる(標本の中身に依存しない)ので平均も不偏。**Z も MIS も補正レイも要らない。**
// 2x2 クアッドは16画素タイルを跨がないので4画素は同じ候補プールを見るが、跨いでも
// 各 W_j は自分のプールに対して厳密なので問題にならない。
//
// 【受け入れている偏り】V_j は本来 V(x, y_j) であるべきところを V(x_j, y_j) で代用している。
// 影の境界がクアッドを横切る画素でだけ食い違い、硬い影の縁が最大1画素(対角 sqrt(2))
// ぼける。これは 2x2 の箱フィルタと同じで、**箱フィルタは積分を保存するので総和比には出ず、
// 影の縁の帯の |相対誤差| にだけ出る**。UE の DownsampleFactor=2 と同じ種類の近似である。
// 実測(MegaLightsNoiseCheck / 900枚 / デノイザOFF): 共有を入れても総和比は
// 0.99927 → 0.99842 と -0.09% しか動かない。一方、影の縁の帯の中央値は平坦部の4倍になる。
//
// 【総和の欠損はこのパスではなくデノイザから来る】デノイザまで通した総和比は 0.98883 で、
// 欠損のほぼ全部(約1.0%)は SVGF の輝度エッジ停止が 1/p の重い裾の明るいタップを
// 優先的に弾くことによる(手法2でも同じ性質が -0.5% 出ている)。
// **「負側の画素が51%だから偏りが無い」とは言えない** ―― それは画素ごとの誤差の符号に
// ついてしか言えず、総エネルギーの欠損を否定しない。
//
// 【ゲートは幾何だけで決めること】深度・法線・材質で仲間を採否する。
// **y_j や V_j や f の値を見て採否してはいけない** ―― 見た瞬間に採用集合が標本の中身に
// 依存し、静かに偏る。同じ理由で、空のリザーバ・殺されたリザーバは
// **項0として n に数える**(数え落とすと分母が過小になり明るい側の系統誤差になる。
// docs/ImplementationHistory.md 67.6 の M=0 バグとまったく同じ型)。
//
// レイを撃たないので RayQuery を使わず、3バリアント(SM 5.0 / 6.5 / 6.6)すべてで焼ける
// (KurenaiShaderPacker の kSkipDxbc50Files には入れない)。ただし手法3自体は
// Initial が RayQuery を使うので DX12 + DXR Tier 1.1 でしか走らない。
#include "NormalEncoding.hlsli"
#include "SpecularEnergy.hlsli"

static const float PI = 3.14159265359f;

cbuffer FrameConstants : register(b0)
{
    float4x4 ViewProj;
    float4x4 InvViewProj;
    float4x4 CascadeViewProj[4];
    float4 CameraPosition;
    float4 LightDirection;
    float4 LightColor;
    float4x4 View;
    float4x4 Proj;
    float4 AmbientColor;
    float4 CascadeSplits;
    // w にスペキュラのエネルギー補正のモードが入っている
    float4 ShadowParams;
    // 【宣言はここで止めている】読むのは ShadowParams まで。途中を飛ばして末尾だけを
    // 宣言すると誤ったオフセットを読み、コンパイルは通り絵も「それらしく」出るため気付けない
};

cbuffer MegaLightsStochasticConstants : register(b1)
{
    // x=出力幅, y=出力高, z=初期候補数M(このパスでは未使用), w=影レイを撃つか
    uint4 Params0;
    // x=タイル数X, y=タイルの1辺のピクセル数, z=1タイルあたりの候補数K, w=フレーム番号
    // (このパスでは未使用。b1 を複数パスで共有しているため並びは合わせてある)
    uint4 Params1;
    // xyz=空間再利用用(このパスでは未使用)、w=初期可視レイの有無
    uint4 Params2;
    // 空間再利用と時間再利用用(このパスでは未使用)
    float4 Params3;
    // x=履歴が有効か, y=空間再利用の反復番号(このパスでは未使用),
    // z=クアッド共有を行うか(0なら自分の標本だけを使う。陽性対照で切る),
    // w=クアッド層化(Initial が読む。このパスでは未使用)
    uint4 Params4;
    // x=1画素あたりの標本数(リザーバの本数)。Initial が同じ数だけ書いている
    uint4 Params5;
};

Texture2D NormalTexture : register(t1);
Texture2D DepthTexture : register(t2);
Texture2D AlbedoTexture : register(t3);
Texture2D MaterialTexture : register(t4);
Texture2D BRDFLUTTexture : register(t5);

#define KURENAI_PUNCTUAL_LIGHT_REGISTER t6
#define KURENAI_PUNCTUAL_LIGHTING_BRDF
#include "PunctualLighting.hlsli"
#include "MegaLightsCommon.hlsli"

StructuredBuffer<MegaLightsReservoir> Reservoirs : register(t7);

RWTexture2D<float4> MegaLightsOutput : register(u0);
// 【手法3にも履歴ガイドが要る】デノイザの履歴の妥当性判定は「前フレームの幾何」と
// 比べるべきもので、現フレームの G-Buffer を再投影先で引く代用は動く細い形状で
// 構造的に必ず失敗する(docs 61.7g.6)。手法3は時間再利用パスを持たないので、
// 本来あちらが書いていたガイドをここで書く
RWStructuredBuffer<MegaLightsHistoryGuide> OutputGuide : register(u1);

// 仲間を採用する条件。**深度は必ず View 空間の線形値で比べること** ――
// Reverse-Z の生値で比べると遠景は常に「一致」・近景は常に「不一致」になる。
// しきい値は時間再利用・空間再利用・デノイザと同じものを使う(揃えておくと、
// 「どの段で弾かれたか」を考えずに済む)
static const float kMaxRelativeDepthDiff = 0.05f;
static const float kMinNormalDot = 0.9f;
static const float kMaxMaterialDiff = 0.1f;

// Rec.709 の輝度。標本ごとの分散を出すのに使う
// (エンジン側 Luminance() と同じ係数。各パスがそれぞれ持っているのに倣う)
float Luminance(float3 c)
{
    return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

float3 ReconstructWorldPos(float2 uv, float depth)
{
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float4 worldPos = mul(float4(ndc, depth, 1.0f), InvViewProj);
    return worldPos.xyz / worldPos.w;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadID.xy;
    const uint2 outputSize = Params0.xy;
    if (pixel.x >= outputSize.x || pixel.y >= outputSize.y)
    {
        return;
    }

    const uint index = pixel.y * outputSize.x + pixel.x;
    const float2 uv = (float2(pixel) + 0.5f) / float2(outputSize);
    const float depth = DepthTexture.SampleLevel(DataSampler, uv, 0).r;
    if (depth <= 0.0f)
    {
        // 背景(スカイ)。Reverse-Z のため遠平面は NDC z=0.0 付近になる。
        // 【必ず書くこと】RHI に UAV のクリアが無く、書かずに return すると前フレームの残骸が残る
        MegaLightsOutput[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        OutputGuide[index] = MegaLightsMakeEmptyGuide();
        return;
    }

    // --- 自分の面 ---
    const float3 worldPos = ReconstructWorldPos(uv, depth);
    const float4 albedoSample = AlbedoTexture.SampleLevel(ColorSampler, uv, 0);
    const float3 albedo = albedoSample.rgb;
    const float translucency = albedoSample.a;
    const float3 N = OctDecode(NormalTexture.SampleLevel(DataSampler, uv, 0).xy);
    const float2 material = MaterialTexture.SampleLevel(DataSampler, uv, 0).rg;
    const float metallic = material.r;
    const float roughness = material.g;
    const float viewZ = mul(float4(worldPos, 1.0f), View).z;

    const float3 V = normalize(CameraPosition.xyz - worldPos);
    const float NdotV = saturate(dot(N, V)) + 1e-5f;
    const float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
    const float3 brdf = BRDFLUTTexture.SampleLevel(ColorSampler, float2(NdotV, roughness), 0).rgb;
    const SpecularEnergyContext energy = MakeSpecularEnergyContext(F0, brdf, roughness, ShadowParams.w);

    // 履歴ガイドは標本の中身に関わらず必ず書く(次フレームから見れば「前フレームの幾何」)
    MegaLightsHistoryGuide guide;
    guide.NormalOct = MegaLightsPackNormalOct(OctEncode(N));
    guide.ViewZ = viewZ;
    guide.Material = MegaLightsPackMaterial(metallic, roughness);
    OutputGuide[index] = guide;

    // --- クアッドの4標本を集めて平均する ---
    // クアッドは2画素境界に整列している(16画素タイルを跨がない)
    const uint2 quadBase = pixel & ~1u;
    const bool shareEnabled = (Params4.z != 0u);

    // 1画素あたりの標本数。Initial が同じ数だけリザーバを書いている。
    // クアッドの項の数は 採用した仲間の数 × この数 になる
    const uint samplesPerPixel = max(Params5.x, 1u);

    float3 sum = float3(0.0f, 0.0f, 0.0f);
    // 【n は幾何だけで決まる】標本の中身(灯番号・可視性・寄与)を見て増減させてはいけない
    uint acceptedCount = 0u;
    // 項の輝度の和と二乗和。デノイザへ渡す分散の種にする(出力の .a)。
    // 【配列で持たない】項の数が 4×標本数まで増えるので固定長では受けきれない。
    // 逐次で足すと「実際に数えた項」だけが分散に入り、数と中身が必ず一致する
    float lumSum = 0.0f;
    float lumSquaredSum = 0.0f;

    [unroll]
    for (uint j = 0u; j < 4u; ++j)
    {
        const uint2 mate = quadBase + uint2(j & 1u, j >> 1u);
        if (mate.x >= outputSize.x || mate.y >= outputSize.y)
        {
            continue;
        }
        const bool isSelf = all(mate == pixel);
        if (!isSelf && !shareEnabled)
        {
            // 陽性対照(共有OFF)。自分の標本だけを使う ―― これは RIS の M=1 相当ではなく、
            // 「M=8 の RIS で選んだ1灯を自分の面で評価する」形で、手法2から時間再利用と
            // 空間再利用を外した構成と画素単位で一致するはず
            continue;
        }

        if (!isSelf)
        {
            // --- 幾何ゲート(標本の中身は見ない) ---
            const float2 mateUv = (float2(mate) + 0.5f) / float2(outputSize);
            const float mateDepth = DepthTexture.SampleLevel(DataSampler, mateUv, 0).r;
            if (mateDepth <= 0.0f)
            {
                // 背景の仲間は標本を持たない。n にも数えない
                continue;
            }
            const float3 mateWorldPos = ReconstructWorldPos(mateUv, mateDepth);
            const float mateViewZ = mul(float4(mateWorldPos, 1.0f), View).z;
            if (abs(mateViewZ - viewZ) > kMaxRelativeDepthDiff * max(abs(viewZ), 1e-3f))
            {
                continue;
            }
            const float3 mateN = OctDecode(NormalTexture.SampleLevel(DataSampler, mateUv, 0).xy);
            if (dot(N, mateN) < kMinNormalDot)
            {
                continue;
            }
            const float2 mateMaterial = MaterialTexture.SampleLevel(DataSampler, mateUv, 0).rg;
            if (abs(mateMaterial.r - metallic) > kMaxMaterialDiff ||
                abs(mateMaterial.g - roughness) > kMaxMaterialDiff)
            {
                continue;
            }
        }

        // ここから先は「採用した仲間」。その仲間が持つ標本すべてを項として数える。
        // **どの経路を通っても n には必ず数える**(空でも殺されていても項0として数える)
        const uint mateBase = (mate.y * outputSize.x + mate.x) * samplesPerPixel;
        [loop]
        for (uint s = 0u; s < samplesPerPixel; ++s)
        {
            ++acceptedCount;

            float3 term = float3(0.0f, 0.0f, 0.0f);
            const MegaLightsReservoir reservoir = Reservoirs[mateBase + s];
            // 空(候補が無かった)か、可視レイで殺された標本は寄与0のまま
            if (!MegaLightsReservoirIsEmpty(reservoir))
            {
                const uint lightIndex = MegaLightsUnpackLight(reservoir.IndexAndFlags);
                const GPULight light = Lights[lightIndex];
                // 【借りた灯は必ず自分の面で評価し直す】隣で良かった灯がここで良いとは限らない。
                // スポットの円錐外・Range 外・背向きなら寄与は0(それが真の値)
                const PunctualGeometry geometry =
                    EvaluatePunctualGeometry(light, worldPos, N, translucency);
                if (geometry.Contributes)
                {
                    // 【可視性は仲間のレイの結果を借りる】これが受け入れた偏りの本体。
                    // 影レイを撃たない構成(Params0.w == 0。恒等テスト)や、レイトレース影を
                    // 落とさない灯では Initial が可視フラグを立てているので V=1 になる
                    const float visibility =
                        MegaLightsUnpackVisible(reservoir.IndexAndFlags) ? 1.0f : 0.0f;
                    term = EvaluatePunctualContribution(
                               light, geometry, N, V, NdotV, albedo, metallic, roughness, translucency,
                               energy, visibility) *
                           reservoir.W;
                }
            }

            sum += term;
            const float termLum = Luminance(term);
            lumSum += termLum;
            lumSquaredSum += termLum * termLum;
        }
    }

    if (acceptedCount == 0u)
    {
        // 自分が背景でない以上ここへは来ないはずだが、0除算のガードは必須
        // (NaN が出ると直接光→SceneColor→TAA の履歴まで壊れる)
        MegaLightsOutput[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    const float invCount = 1.0f / float(acceptedCount);
    const float3 resolved = sum * invCount;

    // 【平均の分散を .a へ載せる】デノイザは今のところ .rgb しか読まないので無害。
    // 履歴が短い画素の分散の種として使えるかは段階Cで測ってから決める。
    // n=1 では分散を推定しようがないので0
    float variance = 0.0f;
    if (acceptedCount > 1u)
    {
        const float n = float(acceptedCount);
        // Σ(x - x̄)^2 = Σx^2 - (Σx)^2/n。標本平均の分散なので n(n-1) で割る。
        // 丸めで負に振れることがあるので0で止める
        const float squaredSum = max(lumSquaredSum - lumSum * lumSum / n, 0.0f);
        variance = squaredSum / (n * (n - 1.0f));
    }

    MegaLightsOutput[pixel] = float4(resolved, variance);
}
