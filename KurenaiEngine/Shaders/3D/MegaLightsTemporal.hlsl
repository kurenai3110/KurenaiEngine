// MegaLights の時間再利用。前フレームのリザーバを速度ベクトルで再投影して結合する。
//
// 【何のためにあるか】空間再利用が「隣の画素」から借りるのに対し、こちらは「前フレームの
// 自分」から借りる。実効サンプル数がフレーム方向に積み上がるので、レイを1本も増やさずに
// 収束が速くなる。借りるのは「どの灯か」だけで、寄与は必ず現フレームの面で評価し直す。
//
// 【結合は空間再利用と同じ形】候補は2つ(自分と履歴)だけで、
// 選択は w_i = p^(y_i) * W_i * M_i、最後に「選ばれた灯を生成しえた候補の M の合計」で割る。
// **空リザーバでも M は数える** ―― 数え落とすと分母が過小になり明るい側へ偏る
// (これは実際に空間再利用で踏んだ。docs/ImplementationDetail.md 61.7)。
//
// 【Mのクランプが要る】履歴のMは放っておくと際限なく増え、新しいサンプルが採用される確率が
// 1/(M+1) まで落ちる。そうなると灯を消しても明かりと影が残り続ける(ゴースト)。
//
// 【プリ露出の補正は要らない。TAA/DDGIから式を写してはいけない】あちらは履歴の*色*を
// 持ち回るので露出比を掛ける必要があるが、リザーバのWは露出に対して不変(比なので約分される)。
// 写すと露出が動いた瞬間だけ4倍ずれる。詳しい理由と実測は下の結合部のコメント。
//
// 【再投影はTAAとまったく同じ引き方をする】historyUv = uv - velocity。
// 違う引き方をすると1画素未満の系統的なずれが出て、照らされた面が静止時に微振動する。
//
// 【最近傍で引く。バイリニアにしてはいけない】リザーバはライト番号という離散値を持つので
// 補間できない。4近傍を見て、幾何が一致する最初のものを採る。
//
// 【時間検証レイ ―― 履歴の固着を防ぐ要】履歴のサンプルは前のフレームの抽選結果で、
// この画素からの可視性は検証されていない。検証せずに使うと、遮蔽された灯を掴んだ画素が
// M上限ぶんの慣性で黒いまま固まり、外れた瞬間に明るく戻る ―― これが「遅い明滅」として
// 見える(実測: 検証なし・M上限640で、256フレーム蓄積しても厳密に0のままの画素が
// 画面の47%に達した)。初期可視レイが有効なときは、採用した履歴のサンプルへ
// 1本撃ち直し、遮蔽されていたら殺す(どの灯を殺したかは残す)。これで
// 「生きているリザーバは、このフレーム・この画素で可視」という不変条件が全パスで成り立ち、
// 空間再利用の可視性込みの分母(Z)の前提も守られる。
//
// RayQuery(SM 6.5)を使うため、DX12 かつ DXR Tier 1.1 のときだけ生成される
// (KurenaiShaderPacker の kSkipDxbc50Files に登録済み)。
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
    // 【宣言はここで止めている】読むのは ShadowParams まで
};

cbuffer MegaLightsStochasticConstants : register(b1)
{
    // x=出力幅, y=出力高, z=初期候補数M(このパスでは未使用), w=影レイを撃つか(未使用)
    uint4 Params0;
    // x=タイル数X, y=タイルの1辺のピクセル数, z=1タイルあたりの候補数K, w=フレーム番号
    uint4 Params1;
    // xyz=空間再利用用(未使用)、w=初期可視レイの有無(未使用)。
    // 【途中を飛ばして宣言してはいけない】飛ばすと誤ったオフセットを読み、
    // コンパイルは通り絵もそれらしく出るため気付けない
    uint4 Params2;
    // x=射影行列の(0,0)成分(未使用), y=同(1,1)成分(未使用),
    // z=未使用(かつてプリ露出の補正倍率を入れていたが、Wは露出に不変と実測で分かった。
    //   理由は下の「プリ露出の補正は要らない」を参照), w=履歴のMの上限
    float4 Params3;
    // x=履歴が使えるか(0なら履歴を読まない), yzw=未使用。
    // 【0のときは読むこと自体をやめる】解像度が変わった直後などは、履歴バッファに
    // 前の解像度のままの内容が残っている。RHIにバッファのクリアが無いため、
    // 混ぜる割合を0にするだけでは足りない(添字の意味が変わっているので中身は別画素のもの)
    uint4 Params4;
};

RaytracingAccelerationStructure SceneTLAS : register(t0);

Texture2D NormalTexture : register(t1);
Texture2D DepthTexture : register(t2);
Texture2D AlbedoTexture : register(t3);
Texture2D MaterialTexture : register(t4);
Texture2D BRDFLUTTexture : register(t5);

#define KURENAI_PUNCTUAL_LIGHT_REGISTER t6
#define KURENAI_PUNCTUAL_LIGHTING_BRDF
#include "PunctualLighting.hlsli"
#include "MegaLightsCommon.hlsli"

StructuredBuffer<MegaLightsReservoir> InputReservoirs : register(t7);
StructuredBuffer<MegaLightsReservoir> HistoryReservoirs : register(t8);
StructuredBuffer<MegaLightsHistoryGuide> HistoryGuide : register(t9);
// G-Bufferパスが書いたモーションベクター(UV単位)。TAAが使っているものと同じ
Texture2D VelocityTexture : register(t10);

RWStructuredBuffer<MegaLightsReservoir> OutputReservoirs : register(u0);
RWStructuredBuffer<MegaLightsHistoryGuide> OutputGuide : register(u1);

// 履歴を採用する条件。空間再利用(MegaLightsSpatial.hlsl)と同じ3つを同じしきい値で見る。
// **深度は必ずView空間の線形値で比べること**(Reverse-Zの生値で比べてはいけない)
static const float kMaxRelativeDepthDiff = 0.05f;
static const float kMinNormalDot = 0.9f;
static const float kMaxMaterialDiff = 0.1f;

float3 ReconstructWorldPos(float2 uv, float depth)
{
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float4 worldPos = mul(float4(ndc, depth, 1.0f), InvViewProj);
    return worldPos.xyz / worldPos.w;
}

uint HashUint(uint x)
{
    x ^= x >> 17;
    x *= 0xed5ad4bbu;
    x ^= x >> 11;
    x *= 0xac4c1b51u;
    x ^= x >> 15;
    x *= 0x31848babu;
    x ^= x >> 14;
    return x;
}

float NextRandom(inout uint state)
{
    state = HashUint(state);
    return float(state) * 2.3283064365e-10f;
}

float Luminance(float3 c)
{
    return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

// レイ原点のバイアス。MegaLightsInitialSample.hlsl / MegaLightsShade.hlsl と同じ値を使う
// (違う値にすると、殺す判定と影の階調が縁で食い違う)
static const float kRayOriginBias = 0.01f;
static const float kRayOriginBiasSlope = 1e-4f;
static const float kMinSlopeScaleNdotL = 0.1f;

float TraceLightVisibility(float3 rayOrigin, float3 L, float originBias, float distanceToLight)
{
    RayDesc ray;
    ray.Origin = rayOrigin;
    ray.Direction = L;
    ray.TMin = originBias;
    // 光源までの距離で打ち切る(省くと光源の向こう側のジオメトリが遮蔽物になる)
    ray.TMax = max(distanceToLight - originBias, originBias);

    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
    query.TraceRayInline(SceneTLAS, RAY_FLAG_NONE, 0xFFu, ray);
    query.Proceed();

    return (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ? 0.0f : 1.0f;
}

// 現フレームのこの画素の面。借りた灯は必ずここで評価し直す
struct TemporalSurface
{
    float3 WorldPos;
    float3 N;
    float3 V;
    float3 Albedo;
    float NdotV;
    float Metallic;
    float Roughness;
    float Translucency;
    float ViewZ;
    SpecularEnergyContext Energy;
};

// その面で、その灯の「遮蔽を含まない寄与の明るさ」
float TargetPdfOn(TemporalSurface s, uint lightIndex)
{
    const GPULight light = Lights[lightIndex];
    const PunctualGeometry geometry = EvaluatePunctualGeometry(light, s.WorldPos, s.N, s.Translucency);
    if (!geometry.Contributes)
    {
        return 0.0f;
    }
    const float3 unshadowed = EvaluatePunctualContribution(
        light, geometry, s.N, s.V, s.NdotV, s.Albedo, s.Metallic, s.Roughness, s.Translucency, s.Energy, 1.0f);
    return Luminance(unshadowed);
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
        // 背景。【必ず両方書くこと】RHIにバッファのクリアが無く、書かずにreturnすると
        // 前フレームの残骸が残り、次フレームがそれを履歴として読む
        OutputReservoirs[index] = MegaLightsMakeEmptyReservoir();
        OutputGuide[index] = MegaLightsMakeEmptyGuide();
        return;
    }

    TemporalSurface self;
    self.WorldPos = ReconstructWorldPos(uv, depth);
    const float4 albedoSample = AlbedoTexture.SampleLevel(ColorSampler, uv, 0);
    self.Albedo = albedoSample.rgb;
    self.Translucency = albedoSample.a;
    self.N = OctDecode(NormalTexture.SampleLevel(DataSampler, uv, 0).xy);
    const float2 material = MaterialTexture.SampleLevel(DataSampler, uv, 0).rg;
    self.Metallic = material.r;
    self.Roughness = material.g;
    self.V = normalize(CameraPosition.xyz - self.WorldPos);
    self.NdotV = saturate(dot(self.N, self.V)) + 1e-5f;
    self.ViewZ = mul(float4(self.WorldPos, 1.0f), View).z;

    const float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), self.Albedo, self.Metallic);
    const float3 brdf = BRDFLUTTexture.SampleLevel(ColorSampler, float2(self.NdotV, self.Roughness), 0).rgb;
    self.Energy = MakeSpecularEnergyContext(F0, brdf, self.Roughness, ShadowParams.w);

    // 現フレームのガイドは、履歴を採れたかどうかに関わらず必ず書く
    // (次フレームから見れば「前フレームの幾何」であり、リザーバの中身とは独立)
    MegaLightsHistoryGuide guide;
    guide.NormalOct = MegaLightsPackNormalOct(OctEncode(self.N));
    guide.ViewZ = self.ViewZ;
    guide.Material = MegaLightsPackMaterial(self.Metallic, self.Roughness);
    OutputGuide[index] = guide;

    // --- 履歴を再投影して探す ---
    // 【TAAとまったく同じ引き方】historyUv = uv - velocity
    const float2 velocity = VelocityTexture.SampleLevel(DataSampler, uv, 0).rg;
    const float2 historyUv = uv - velocity;

    MegaLightsReservoir history = MegaLightsMakeEmptyReservoir();
    bool historyFound = false;

    if (Params4.x != 0u && all(historyUv >= 0.0f) && all(historyUv <= 1.0f))
    {
        // 最近傍で引く(リザーバはライト番号という離散値を持つので補間できない)。
        // 4近傍を見て、幾何が一致する最初のものを採る
        const float2 historyPixelF = historyUv * float2(outputSize) - 0.5f;
        const int2 base = int2(floor(historyPixelF));

        [unroll]
        for (uint n = 0u; n < 4u; ++n)
        {
            if (historyFound)
            {
                continue;
            }
            const int2 offset = int2(n & 1u, n >> 1u);
            const int2 hp = base + offset;
            if (hp.x < 0 || hp.y < 0 || hp.x >= int(outputSize.x) || hp.y >= int(outputSize.y))
            {
                continue;
            }

            const uint historyIndex = uint(hp.y) * outputSize.x + uint(hp.x);
            const MegaLightsHistoryGuide g = HistoryGuide[historyIndex];
            // ViewZ == 0 は「前フレームはそこが背景だった」を表す
            if (g.ViewZ == 0.0f)
            {
                continue;
            }
            // 【線形深度で比べる】Reverse-Zの生値ではない
            if (abs(g.ViewZ - self.ViewZ) > kMaxRelativeDepthDiff * max(abs(self.ViewZ), 1e-3f))
            {
                continue;
            }
            const float3 historyN = OctDecode(MegaLightsUnpackNormalOct(g.NormalOct));
            if (dot(self.N, historyN) < kMinNormalDot)
            {
                continue;
            }
            float historyMetallic;
            float historyRoughness;
            MegaLightsUnpackMaterial(g.Material, historyMetallic, historyRoughness);
            if (abs(historyMetallic - self.Metallic) > kMaxMaterialDiff ||
                abs(historyRoughness - self.Roughness) > kMaxMaterialDiff)
            {
                continue;
            }

            history = HistoryReservoirs[historyIndex];
            historyFound = true;
        }
    }

    // --- 結合 ---
    const MegaLightsReservoir current = InputReservoirs[index];

    if (historyFound)
    {
        // 【プリ露出の補正は要らない ―― TAAやDDGIと事情が違う】
        // あちらが補正するのは履歴の*色*で、色はプリ露出に比例するから比を掛ける必要がある。
        // こちらが持ち回るのはリザーバのWで、
        //     W = Σw / (M * p̂(y)),  w = p̂ / p_source
        // 分子も分母も p̂ に比例し、p_source は正規化された確率なので露出に依存しない。
        // **露出が約分されるので W は露出に対して不変**であり、掛けるべき係数は 1。
        //
        // 【両方向を実測して確かめた】履歴のWへ「今/前」(=1/4)を掛けると露出+2段の直後に
        // 4倍暗くなり、「前/今」(=4)を掛けると4倍明るいまま居座った。掛けないときだけ
        // 正解(履歴を持たない経路の値)と一致する。**静止画では絶対に気付けない誤り**なので、
        // 露出を跳ばす摂動(-megalightsperturb 2)で測ること
        // 【Mのクランプ】無いと新しいサンプルが採用されなくなり、灯を消しても残る
        history.M = min(history.M, max(Params3.w, 1.0f));

        // --- 時間検証レイ: 採用した履歴のサンプルがこの画素から見えるかを撃ち直す ---
        // 検証しないと、遮蔽された灯を掴んだ画素がM上限ぶんの慣性で黒く固まり、
        // 「遅い明滅」になる(冒頭のコメント)。殺すときはどの灯を殺したかを残す
        // (空間再利用の分母がこの確定情報を使う。MegaLightsInitialSample.hlsl と同じ)
        if (Params0.w != 0u && Params2.w != 0u && !MegaLightsReservoirIsEmpty(history))
        {
            const uint historyLight = MegaLightsUnpackLight(history.LightAndFlags);
            const GPULight hLight = Lights[historyLight];
            if (hLight.Params.y > 0.5f)
            {
                const PunctualGeometry hGeometry =
                    EvaluatePunctualGeometry(hLight, self.WorldPos, self.N, self.Translucency);
                if (hGeometry.Contributes)
                {
                    const float slopeScale =
                        1.0f / max(dot(self.N, hGeometry.L), kMinSlopeScaleNdotL);
                    const float originBias =
                        (kRayOriginBias + length(self.WorldPos - CameraPosition.xyz) * kRayOriginBiasSlope) *
                        slopeScale;
                    const float3 samplePos = MegaLightsLightSamplePosition(
                        hLight.PositionType.xyz, hLight.Params.z,
                        MegaLightsUnpackSampleUV(history.SampleUV));
                    const float3 toSample = samplePos - self.WorldPos;
                    const float sampleDist = length(toSample);
                    if (sampleDist > originBias &&
                        TraceLightVisibility(
                            self.WorldPos + self.N * originBias, toSample / sampleDist, originBias,
                            sampleDist) <= 0.0f)
                    {
                        // 殺す。番号とサンプル点は残し、W=0・可視フラグfalseにする
                        history.LightAndFlags = MegaLightsPackLightAndFlags(historyLight, false);
                        history.W = 0.0f;
                    }
                    else
                    {
                        // このフレーム・この画素で可視を検証済み
                        history.LightAndFlags = MegaLightsPackLightAndFlags(historyLight, true);
                    }
                }
            }
        }
    }
    else
    {
        history = MegaLightsMakeEmptyReservoir();
    }

    // 【ここは判定が実質1回なのでブルーノイズ位相を使える】候補は現フレームと履歴の2つで、
    // 1つ目は必ず採用される(w/weightSum = 1)。効くのは「履歴へ乗り換えるか」の1回だけで、
    // WRS の独立性を使わないため、位相を隣どうしで離しても選択確率は厳密に保たれる。
    // どちらの灯を映すかが画素ごとに決まる場所なので、ここを白色にしておくと
    // 見た目の粒がそのまま白色ノイズになる
    const float switchPhase = MegaLightsPixelPhase(pixel, Params1.w, 1u);
    uint acceptCount = 0u;

    float weightSum = 0.0f;
    float confidenceSum = 0.0f;
    uint selectedLight = kMegaLightsInvalidLight;
    float selectedTargetPdf = 0.0f;
    uint selectedSampleUV = 0u;
    // 選ばれたサンプルが「このフレーム・この画素で可視レイを通過した」証明を持つか。
    // 現フレーム(i=0)の勝者だけが持ちうる。履歴の勝者は前のフレームの証明なので持たない
    bool selectedVerified = false;

    // 候補は2つだけ(0=現フレーム、1=履歴)。空間再利用と同じ形で回す
    [unroll]
    for (uint i = 0u; i < 2u; ++i)
    {
        // 【三項演算子は使えない】HLSLの条件演算子は数値のスカラー/ベクトル/行列にしか
        // 使えず、構造体を返そうとすると error X3020 になる
        MegaLightsReservoir candidate;
        if (i == 0u)
        {
            candidate = current;
        }
        else
        {
            candidate = history;
        }

        // 【空でも M は数える】数え落とすと分母が過小になり、明るい側の系統誤差になる
        confidenceSum += candidate.M;
        if (MegaLightsReservoirIsEmpty(candidate))
        {
            continue;
        }

        const uint lightI = MegaLightsUnpackLight(candidate.LightAndFlags);
        // 【借りた灯は必ず現フレームの面で評価し直す】前で良かった灯が今も良いとは限らない
        const float pdfSelf = TargetPdfOn(self, lightI);
        if (pdfSelf <= 0.0f)
        {
            continue;
        }

        const float w = pdfSelf * candidate.W * candidate.M;
        if (w <= 0.0f)
        {
            continue;
        }

        weightSum += w;
        const float acceptRandom = (acceptCount == 0u) ? switchPhase
                                                       : MegaLightsLowDiscrepancy1D(acceptCount, switchPhase);
        ++acceptCount;
        if (acceptRandom < w / weightSum)
        {
            selectedLight = lightI;
            selectedTargetPdf = pdfSelf;
            // 【球面上のどこを狙ったかも一緒に引き継ぐ】落とすと借りたサンプルが
            // 中心を狙い直してしまい、再利用した画素だけ半影が消える
            selectedSampleUV = candidate.SampleUV;
            // 現フレームは初期可視レイ、履歴は時間検証レイが立てたフラグをそのまま使う
            selectedVerified = MegaLightsUnpackVisible(candidate.LightAndFlags);
        }
    }

    // 【0除算のガードは必須】NaNが出ると直接光→SceneColor→TAAの履歴まで壊れる
    if (selectedLight == kMegaLightsInvalidLight || selectedTargetPdf <= 0.0f || weightSum <= 0.0f ||
        confidenceSum <= 0.0f)
    {
        // 【Mは残すが、1フレームぶんの空振りで頭打ちにする】「候補を検討したが外れた」
        // という情報は分母に要る(M=0にすると明るく偏る。61.7の教訓)。
        // 一方で空のまま confidenceSum を積み上げると、殺され続けた画素のMが履歴上限まで
        // 肥え太り、やっと当たった弱い灯の W が 8/(8+M) 倍に押し潰される ――
        // 回復に数百フレームかかり、影の縁に黒い斑点として残る(実測)。
        // 空のリザーバは「今フレームの空振り」以上の確信を主張してはいけない
        // 【現フレームが可視レイで殺されていたら、その番号と可視フラグも引き継ぐ】
        // 消してしまうと、後段の空間再利用の分母(Z)が「この画素はその灯を配れなかった」
        // ことを知れず、暗い側の系統誤差が時間再利用経由で戻ってくる
        // (詳細は MegaLightsInitialSample.hlsl の殺し側のコメント)
        MegaLightsReservoir rejected = MegaLightsMakeEmptyReservoir();
        if (MegaLightsUnpackLight(current.LightAndFlags) != kMegaLightsInvalidLight &&
            !MegaLightsUnpackVisible(current.LightAndFlags))
        {
            rejected.LightAndFlags = current.LightAndFlags;
            rejected.SampleUV = current.SampleUV;
        }
        rejected.M = min(confidenceSum, float(max(Params0.z, 1u)));
        OutputReservoirs[index] = rejected;
        return;
    }

    // --- 割る数を決める ---
    // 【定義域の判定は要らない】空間再利用では候補ごとにタイルが違うため「その灯を
    // 生成しえたか」を判定する必要があったが、時間再利用の候補はどちらも**同じ画素**で、
    // 履歴は幾何の一致判定を通っている(=同じ面)。同じ面なら候補集合も提案分布も同じなので、
    // 両方の候補が常に「生成しえた」側に入る。よって分母は素直に ΣM でよい
    // 【分母は素直に ΣM でよい】候補は2つとも同じ画素のストリームで、定義域(タイル)も
    // 可視性(この画素からの遮蔽)も共有する。勝者は可視レイか時間検証レイを通過して
    // いるので、その灯は両ストリームから生成されえた ―― どちらの M も数えるのが正しい。
    // (かつて「現フレームが選ばれた灯を殺していたら M を引く」補正を入れていたが、
    //  それは履歴を検証しない構成での近似で、検証レイが入った今は逆に偏らせる)
    const float denominator = confidenceSum;

    MegaLightsReservoir result;
    // 【可視フラグは証明があるときだけ true】空間再利用の Z がこのフラグを
    // 「レイを省いてよいか」の判定に使う。履歴の勝者に true を付けると、
    // 検証していない可視性を確定扱いしてしまう
    result.LightAndFlags = MegaLightsPackLightAndFlags(selectedLight, selectedVerified);
    result.SampleUV = selectedSampleUV;
    result.W = weightSum / (denominator * selectedTargetPdf);
    result.M = confidenceSum;
    OutputReservoirs[index] = result;
}
