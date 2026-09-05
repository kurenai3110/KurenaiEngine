// MegaLights の空間再利用。近傍の画素が選んだ灯を借りて、自分の面で評価し直して結合する。
// 【レイは増える】当初は「借りるのは"どの灯か"だけなので影レイは増えない」と書いていたが、
// 現行は増える ―― 目標関数に可視性を入れるために標的ごとに自分の面から1本撃ち、
// 不偏化の分母(Z)でも可視性が不明な近傍にバイアス補正レイを撃つ。
// そのぶん勝者は可視が証明済みになるので Shade 側の影レイを省ける(下記)。
//
// 【何を直すためにあるのか】候補プール(MegaLightsTilePool.hlsl)の重みは**設計上、法線を見ない**。
// タイル内で画素ごとに法線が違うため、法線に依存させると「代表法線からは見えないが、
// ある画素からは見える」灯を落としてしまうからである。その結果、
// **法線が候補集合と噛み合わない面では候補の半分が背向きになり、提案分布が外れる。**
// 実測(ManyLightsTest / N=256 / |相対誤差|中央値)では、球で 0.0395 に対し床は 0.0052。
// これは曲面に固有ではなく、**平らな床でも法線を候補集合と噛み合わない向きに固定すると
// 0.0052 → 0.0220 と 4.2 倍悪化する**。つまり「その画素の法線から見て候補のうち何割が
// 背向きか」が効いている。
//
// 近傍から借りれば、法線の近い画素が既に引き当てた良い灯を使える。プールの提案が
// 法線を見られないことを、選んだあとで埋め合わせる形になる。
//
// 【結合は2通り持っている】
//   0 = confidence(M)で重み付ける標準形(Bitterli 2020 Alg.4)。単純だが**不偏ではない** ――
//       近傍が自分と違う候補集合(違うタイル)から引いている可能性を無視するため。
//       実測で総和の相対差が +2.2% 出た。
//   1 = 不偏(Bitterli 2020 Alg.6)。候補の選び方は同じで、**最後に割る数だけが違う**。
//       ΣM ではなく Z ―― 「選ばれたサンプルを実際に生成しえた候補の confidence の合計」で割る。
//       「生成しえた」は「その灯がその候補のタイルの候補集合に入る」かつ
//       「その候補の面で寄与が正」の両方。前者を厳密に判定できるのは、候補プールが
//       タイルの深度スラブをヘッダに書いているため。
//       判定は**選ばれた1つについてだけ**なので、候補ごとにMIS重みを作る書き方より桁違いに軽い。
//
// **切り替えられるようにしてあるのは、長時間平均を比べて差が出ることを確かめるため。**
// 差が出なければどちらかが実装されていない。
//
// 【初期可視レイが有効なときは、Z の判定を可視性まで含めて行う(バイアス補正レイ)】
// 殺しが入ると各画素のストリームは「可視な灯しか配れない」形に変わる。Z が可視性を
// 見ずに M を数えると、殺しの起きる画素の周囲(=影の縁)だけ分母が太り、
// **影が太く・濃くなる**系統誤差になる(実測 -3.6%。一様ではなく縁に集中するので
// 見た目に出る)。前提は「全ストリームが可視フィルタ済み」であること ――
// 現フレームは初期可視レイ、履歴は時間検証レイ(MegaLightsTemporal.hlsl)が保証する。
// 検証されていない履歴が混ざる構成でこの判定を行うと、遮蔽された灯を正当に運ぶ候補を
// 誤って外し、参照の1万倍級のファイアフライになる(実測: 総和+17.5%。61.7f)。
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
    // x=候補プールの有効タイル数X(格子ジッター有効時だけ+1)、
    // y=タイルの1辺のピクセル数, z=1タイルあたりの候補数K, w=フレーム番号
    uint4 Params1;
    // x=借りる近傍の数, y=探す半径(ピクセル),
    // z=結合の方式(0=confidence重み, 1=不偏化のZ),
    // w=初期可視レイでリザーバを殺すか(Initialが読む。このパスでは未使用)
    uint4 Params2;
    // x=射影行列の(0,0)成分, y=同(1,1)成分, zw=未使用。
    // MIS重みが「その灯が隣のタイルへ届くか」を判定するのに、隣のタイルの錐台を組み立て直す
    float4 Params3;
    // x=時間再利用の履歴が有効か。可視性込みのZを使ってよいかの判定に要る(下記)、
    // y=空間再利用の反復番号(0起点)。近傍の型板の種に混ぜて、反復ごとに別の近傍を選ばせる、
    // zw=未使用
    uint4 Params4;
    // Params5はInitial/Resolveが使う1画素あたりの標本数。このパスでは未使用だが、
    // 末尾のParams6を正しいオフセットで読むため途中を飛ばさず宣言する
    uint4 Params5;
    // xy=候補プールのタイル格子オフセット(画素、各0〜15)、zw=未使用
    uint4 Params6;
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
// 「その灯が隣のタイルへ届くか」の判定。MIS重みの分母で定義域を厳密に扱うのに要る
#include "TileLightCulling.hlsli"
#include "MegaLightsCommon.hlsli"

StructuredBuffer<MegaLightsReservoir> InputReservoirs : register(t7);
// 候補プール。ヘッダに入っているタイルの深度スラブから、隣のタイルの錐台を組み立て直す
StructuredBuffer<uint> TilePool : register(t8);
// 今フレームの初期リザーバ(時間再利用を挟む前)。
// 【殺しの持ち回りを読むために別途要る】時間再利用は履歴が勝つと現フレームの殺しを
// 捨てるため、InputReservoirs だけでは「この画素からその灯は見えない」という
// 確定情報が届かない。影の縁では近傍が遮蔽された支配光を正当に持っており、
// この情報無しで借りると毎フレーム影レイを無駄にして黒い斑点になる
StructuredBuffer<MegaLightsReservoir> InitialReservoirs : register(t9);
// 画素ごとの「遮蔽が確定した灯」のキャッシュ(MegaLightsInitialSample.hlsl が維持する)。
// 殺しの持ち回りより寿命が長く、初期RISが別の灯を引いたフレームでも効く
StructuredBuffer<uint> BlockedLights : register(t10);
RWStructuredBuffer<MegaLightsReservoir> OutputReservoirs : register(u0);

// 結合に使う候補の最大数(自分 + 近傍)。MIS重みの分母は候補数の2乗で効くため上限を置く
static const uint kMaxSpatialCandidates = 9u;

// 近傍を採用する条件。**深度は必ずView空間の線形値で比べること** ――
// Reverse-Zの生値で比べると、遠景では常に「一致」・近景では常に「不一致」になる
static const float kMaxRelativeDepthDiff = 0.05f;
// 法線の一致(約25度)。ここを緩めるとノイズは減るがシルエットで光が滲む
static const float kMinNormalDot = 0.9f;
// 金属度・粗さの一致。別の材質の画素から借りると、そこで良かった灯がここで良いとは限らない
static const float kMaxMaterialDiff = 0.1f;

static const float kTwoPI = 6.28318530718f;
static const float kGoldenRatioFrac = 0.61803398875f;

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
// (違う値にすると、殺す判定と Z の判定が食い違って縁に細いバイアスが残る)
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

// 1画素ぶんのサーフェス。MIS重みの分母は「隣の画素の面で評価した目標関数」を要るので、
// 近傍の面も同じ形で読めるようにしてある
struct SpatialSurface
{
    float3 WorldPos;
    float3 N;
    float3 V;
    float3 Albedo;
    float NdotV;
    float Metallic;
    float Roughness;
    float Translucency;
    SpecularEnergyContext Energy;
    bool Valid;
};

SpatialSurface LoadSurface(uint2 p, uint2 outputSize)
{
    SpatialSurface s = (SpatialSurface)0;
    const float2 uv = (float2(p) + 0.5f) / float2(outputSize);
    const float depth = DepthTexture.SampleLevel(DataSampler, uv, 0).r;
    if (depth <= 0.0f)
    {
        s.Valid = false;
        return s;
    }

    s.WorldPos = ReconstructWorldPos(uv, depth);
    const float4 albedoSample = AlbedoTexture.SampleLevel(ColorSampler, uv, 0);
    s.Albedo = albedoSample.rgb;
    s.Translucency = albedoSample.a;
    s.N = OctDecode(NormalTexture.SampleLevel(DataSampler, uv, 0).xy);
    const float2 material = MaterialTexture.SampleLevel(DataSampler, uv, 0).rg;
    s.Metallic = material.r;
    s.Roughness = material.g;
    s.V = normalize(CameraPosition.xyz - s.WorldPos);
    s.NdotV = saturate(dot(s.N, s.V)) + 1e-5f;

    const float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), s.Albedo, s.Metallic);
    const float3 brdf = BRDFLUTTexture.SampleLevel(ColorSampler, float2(s.NdotV, s.Roughness), 0).rgb;
    s.Energy = MakeSpecularEnergyContext(F0, brdf, s.Roughness, ShadowParams.w);
    s.Valid = true;
    return s;
}

// その面で、その灯の「遮蔽を含まない寄与の明るさ」を求める。
// **借りたサンプルも必ず評価し直す** ―― 隣で良かった灯がここで良いとは限らない
float TargetPdfOn(SpatialSurface s, uint lightIndex)
{
    if (!s.Valid)
    {
        return 0.0f;
    }
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

// その灯が、その画素の属するタイルの候補集合に入りうるか。
//
// 【MIS重みの分母にこれが要る】生成化バランスヒューリスティックは
// 「隣の画素がそのサンプルを生成しえた確率」で重み付ける。隣が**そもそも引けない**灯を
// 分母へ入れると、その分だけ重みが小さくなって暗い側へ偏る。逆に、引けるのに入れなければ
// 明るい側へ偏る。候補プールと**まったく同じ判定**(TileLightCulling.hlsli)で決めること
bool LightInTileDomain(uint lightIndex, uint2 pixelCoord, uint2 outputSize)
{
    const uint tileSize = max(Params1.y, 1u);
    const uint2 tileCoord = (pixelCoord + Params6.xy) / tileSize;
    const uint candidateCount = Params1.z;
    const uint base = MegaLightsTilePoolBase(tileCoord, Params1.x, candidateCount);

    if (TilePool[base + 2u] == 0u)
    {
        return false;
    }
    // 深度スラブは候補プールがヘッダへ書いている(そのタイルを走査しないと分からないため)
    const float nearestViewZ = asfloat(TilePool[base + 4u]);
    const float farthestViewZ = asfloat(TilePool[base + 5u]);

    // MISの分母も候補プールを書いたタイルの画素範囲で判定しなければならない
    const int2 tilePixelOrigin = int2(tileCoord * tileSize) - int2(Params6.xy);
    const TileFrustum frustum = MakeTileFrustumFromPixelOrigin(
        tilePixelOrigin, outputSize, Params3.x, Params3.y, nearestViewZ, farthestViewZ);

    float3 viewCenter;
    float radius;
    return IsLightVisibleInTile(Lights[lightIndex], View, frustum, viewCenter, radius);
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

    const SpatialSurface self = LoadSurface(pixel, outputSize);
    if (!self.Valid)
    {
        // 背景。【必ず書くこと】RHIにバッファのクリアが無い
        OutputReservoirs[index] = MegaLightsMakeEmptyReservoir();
        return;
    }

    const float viewZ = mul(float4(self.WorldPos, 1.0f), View).z;
    // 【乱数にフレーム番号を混ぜない ―― Initial とは逆の選択】近傍の取り方と抽選を
    // 毎フレーム引き直すと、影の縁で「明るい側から借りたか/影側から借りたか」が
    // フレームごとに揺れ、半影帯が塊で明滅する(boiling。実測では時間stdの上位帯が
    // 影の縁に沿って幅広く出た)。画素固定の型板にすれば揺れは静的な空間パターンに
    // 変わり、そちらはデノイザの a-trous が消せる。新しい情報は Initial(こちらは
    // フレーム番号を混ぜる)から毎フレーム流れ込むので、収束は止まらない
    // 【フレーム番号は混ぜない】混ぜると近傍の型板が毎フレーム変わり、ノイズが塊で
    // 蠢く(boiling)。画素ごとに固定した型板にしてある。
    // 反復番号だけは混ぜる ―― 同じ型板で2回借りると同じ近傍から借り直すだけになる
    uint rngState =
        HashUint(pixel.x + pixel.y * outputSize.x + 0x27D4EB2Du + Params4.y * 0x9E3779B9u);

    // --- 候補を集める(0番は自分) ---
    uint2 candidateCoord[kMaxSpatialCandidates];
    MegaLightsReservoir candidate[kMaxSpatialCandidates];
    candidateCoord[0] = pixel;
    candidate[0] = InputReservoirs[index];
    uint candidateCount = 1u;

    const uint neighborCount = min(Params2.x, kMaxSpatialCandidates - 1u);
    const float radius = float(max(Params2.y, 1u));
    // 半径方向は層化し、方位角は黄金比で回す(RTShadow.hlsl と同じ低食い違い量列)。
    // 画素ごとに位相をずらして、近傍の取り方が画面全体で揃わないようにする
    const float angleOffset = NextRandom(rngState);
    const float radiusOffset = NextRandom(rngState);

    [loop]
    for (uint n = 0u; n < neighborCount; ++n)
    {
        const float r = radius * sqrt((float(n) + radiusOffset) / float(max(neighborCount, 1u)));
        const float phi = kTwoPI * frac(angleOffset + float(n) * kGoldenRatioFrac);
        const int2 offset = int2(round(r * cos(phi)), round(r * sin(phi)));
        const int2 neighborPixel = int2(pixel) + offset;

        if (neighborPixel.x < 0 || neighborPixel.y < 0 ||
            neighborPixel.x >= int(outputSize.x) || neighborPixel.y >= int(outputSize.y) ||
            (offset.x == 0 && offset.y == 0))
        {
            continue;
        }

        const uint2 np = uint2(neighborPixel);
        const float2 nuv = (float2(np) + 0.5f) / float2(outputSize);

        const float nDepth = DepthTexture.SampleLevel(DataSampler, nuv, 0).r;
        if (nDepth <= 0.0f)
        {
            continue;
        }

        // 【深度はView空間の線形値で比べる】Reverse-Zの生値で比べてはいけない
        const float3 nWorldPos = ReconstructWorldPos(nuv, nDepth);
        const float nViewZ = mul(float4(nWorldPos, 1.0f), View).z;
        if (abs(nViewZ - viewZ) > kMaxRelativeDepthDiff * max(abs(viewZ), 1e-3f))
        {
            continue;
        }

        const float3 nN = OctDecode(NormalTexture.SampleLevel(DataSampler, nuv, 0).xy);
        if (dot(self.N, nN) < kMinNormalDot)
        {
            continue;
        }

        const float2 nMaterial = MaterialTexture.SampleLevel(DataSampler, nuv, 0).rg;
        if (abs(nMaterial.r - self.Metallic) > kMaxMaterialDiff ||
            abs(nMaterial.g - self.Roughness) > kMaxMaterialDiff)
        {
            continue;
        }

        candidateCoord[candidateCount] = np;
        candidate[candidateCount] = InputReservoirs[np.y * outputSize.x + np.x];
        ++candidateCount;
    }

    const bool useMIS = (Params2.z != 0u);

    // --- 結合 ---
    // 候補の選び方は両方式で同じ(w_i = p̂_q(y_i) * W_i * M_i)。**違うのは最後に割る数だけ**。
    //   confidence重み … ΣM で割る(単純だが不偏ではない)
    //   MIS            … Z で割る。Z は「選ばれたサンプルを生成しえた候補の confidence の合計」
    // Bitterli 2020 Alg.6 の形。定義域の判定が**選ばれた1つについてだけ**で済むので、
    // 候補ごとにMIS重みを作る書き方(候補数の2乗)より桁違いに軽い
    float weightSum = 0.0f;
    float confidenceSum = 0.0f;
    uint selectedLight = kMegaLightsInvalidLight;
    float selectedTargetPdf = 0.0f;
    uint selectedSampleUV = 0u;

    // 自分の画素で「この灯を可視レイで殺した」と確定している標的。
    // 【影の縁の黒い斑点の対策】遮蔽の境界の画素は、隣(明るい側)がその支配光を
    // 正当に持っているため、borrow のたびに自分では見えない灯を選び直して
    // 影レイを無駄にし、黒く沈む。自分の殺しは V=0 の証明なので、その標的の
    // 実効的な目標関数は 0 ―― 選択から外せば、届く別の灯が選ばれる。
    // 寄与が0と証明済みの候補を混ぜないだけなので、期待値は変わらない。
    // 【今フレームの初期リザーバから読む】時間再利用の出力(candidate[0])は履歴が
    // 勝つと殺しを捨てるため、初期パスの出力を直接見る。両方に殺しがあれば初期を優先
    uint selfKilledLight = kMegaLightsInvalidLight;
    uint selfKilledSampleUV = 0u;
    if (candidate[0].W <= 0.0f && !MegaLightsUnpackVisible(candidate[0].IndexAndFlags))
    {
        selfKilledLight = MegaLightsUnpackLight(candidate[0].IndexAndFlags);
        selfKilledSampleUV = candidate[0].SampleUV;
    }
    {
        const MegaLightsReservoir initial = InitialReservoirs[index];
        const uint initialLight = MegaLightsUnpackLight(initial.IndexAndFlags);
        if (initialLight != kMegaLightsInvalidLight && initial.W <= 0.0f &&
            !MegaLightsUnpackVisible(initial.IndexAndFlags))
        {
            selfKilledLight = initialLight;
            selfKilledSampleUV = initial.SampleUV;
        }
    }
    // キャッシュされた遮蔽の確定情報(点光源のみ。持続するので毎フレーム効く)
    const uint blockedLight = BlockedLights[index];

    // --- 目標関数に可視性を入れる準備 ---
    // 【なぜ入れるのか】借りた灯が自分から遮蔽されているのを、従来はシェードの影レイで
    // 初めて知り、そのフレームの画素は黒になっていた(デノイザ前の暗黒点の主因。
    // 実測で1フレームあたり点灯画素の3.6%)。選択の時点で標的ごとに自分の面から
    // 1本レイを撃てば、見える灯だけが選ばれる。選ばれた勝者は可視が証明済みになるので
    // シェード側の影レイを省け、レイの総数はほぼ相殺される。
    // 分母(Z)は既に可視性込みで数えているので、これで分子と分母の定義が一致する。
    // 【重複する標的はレイを共有する】候補は同じ支配光を持っていることが多い
    const bool visibilityTargetAware = (Params0.w != 0u) && (Params2.w != 0u);
    uint visTargetLight[kMaxSpatialCandidates];
    uint visTargetUV[kMaxSpatialCandidates];
    float visTargetValue[kMaxSpatialCandidates];
    uint visTargetCount = 0u;

    [loop]
    for (uint i = 0u; i < candidateCount; ++i)
    {
        // 空のリザーバでも confidence は数える(数えないと、寄与0の画素から借りたぶんだけ
        // 重みが持ち上がる)
        confidenceSum += candidate[i].M;
        if (MegaLightsReservoirIsEmpty(candidate[i]))
        {
            continue;
        }

        const uint lightI = MegaLightsUnpackLight(candidate[i].IndexAndFlags);
        // 自分から見えないことが確定している標的は選ばない(理由は selfKilledLight の定義)。
        // 球光源はサンプル点まで一致した場合だけ確定と見なす
        if (lightI == selfKilledLight &&
            (Lights[lightI].Params.z <= 0.0f || candidate[i].SampleUV == selfKilledSampleUV))
        {
            continue;
        }
        // キャッシュ側(点光源のみ記録される)
        if (lightI == blockedLight)
        {
            continue;
        }
        // 【借りた灯は必ず自分の面で評価し直す】隣で良かった灯がここで良いとは限らない
        const float pdfSelf = TargetPdfOn(self, lightI);
        if (pdfSelf <= 0.0f)
        {
            continue;
        }

        // --- 可視性(自分の面から標的へ)。重複はレイを共有する ---
        if (visibilityTargetAware && LightCastsRaytracedShadow(Lights[lightI].Params.y))
        {
            float vis = -1.0f;
            [loop]
            for (uint t = 0u; t < visTargetCount; ++t)
            {
                if (visTargetLight[t] == lightI && visTargetUV[t] == candidate[i].SampleUV)
                {
                    vis = visTargetValue[t];
                    break;
                }
            }
            if (vis < 0.0f)
            {
                const float3 samplePos = MegaLightsLightSamplePosition(
                    Lights[lightI].PositionType.xyz, Lights[lightI].Params.z,
                    Lights[lightI].DirectionAngle.xyz, (uint)Lights[lightI].PositionType.w,
                    MegaLightsUnpackSampleUV(candidate[i].SampleUV));
                const float3 toSample = samplePos - self.WorldPos;
                const float sampleDist = length(toSample);
                const float slopeScale =
                    1.0f / max(dot(self.N, toSample / max(sampleDist, 1e-6f)), kMinSlopeScaleNdotL);
                const float originBias =
                    (kRayOriginBias + length(self.WorldPos - CameraPosition.xyz) * kRayOriginBiasSlope) *
                    slopeScale;
                vis = (sampleDist > originBias)
                          ? TraceLightVisibility(
                                self.WorldPos + self.N * originBias, toSample / sampleDist, originBias,
                                sampleDist)
                          : 1.0f;
                if (visTargetCount < kMaxSpatialCandidates)
                {
                    visTargetLight[visTargetCount] = lightI;
                    visTargetUV[visTargetCount] = candidate[i].SampleUV;
                    visTargetValue[visTargetCount] = vis;
                    ++visTargetCount;
                }
            }
            if (vis <= 0.0f)
            {
                continue;
            }
        }

        const float w = pdfSelf * candidate[i].W * candidate[i].M;
        if (w <= 0.0f)
        {
            continue;
        }

        weightSum += w;
        if (NextRandom(rngState) < w / weightSum)
        {
            selectedLight = lightI;
            selectedTargetPdf = pdfSelf;
            // 【球面上のどこを狙ったかも一緒に引き継ぐ】落とすと借りたサンプルが
            // 中心を狙い直してしまい、再利用した画素だけ半影が消える
            selectedSampleUV = candidate[i].SampleUV;
        }
    }

    // 【0除算のガードは必須】NaNが出ると直接光→SceneColor→TAAの履歴まで壊れる
    if (selectedLight == kMegaLightsInvalidLight || selectedTargetPdf <= 0.0f || weightSum <= 0.0f ||
        confidenceSum <= 0.0f)
    {
        OutputReservoirs[index] = MegaLightsMakeEmptyReservoir();
        return;
    }

    // --- 割る数を決める ---
    // confidence重みは ΣM で割る。これは「どの候補もこのサンプルを生成しえた」と仮定した形で、
    // **生成しえない候補まで数に入れるぶん、割りすぎて暗く/割らなすぎて明るくなる**。
    //
    // MIS(Bitterli 2020 Alg.6)は、**選ばれたサンプルを実際に生成しえた候補だけ**の
    // confidence を合計した Z で割る。「生成しえた」は
    //   ・その灯がその候補のタイルの候補集合に入る(候補プールと同じ判定)
    //   ・その候補の面で寄与が正
    // の両方。これで期待値が全灯評価に一致する
    float denominator = confidenceSum;
    if (useMIS)
    {
        // 【初期可視レイが有効なときは、Z も可視性まで含めて判定する】殺しが入ると
        // 各候補のストリームは「可視な灯しか配れない」形に変わる。選ばれた灯が見えない
        // 候補の M を数えると、殺しの起きる画素の周囲だけ分母が太り、暗い側の系統誤差に
        // なる(実測 -3.6%。docs/ImplementationDetail.md 61.7f)。
        // この前提が成り立つのは、**履歴も時間検証レイで検証されている**から
        // (MegaLightsTemporal.hlsl)。検証しない構成でここを有効にすると、履歴由来の
        // 「遮蔽された灯を正当に運ぶ」候補をレイで分母から外してしまい、
        // 参照の1万倍級のファイアフライが出る(実測: 総和+17.5%。61.7f.7)。
        // 可視性の決め方は3通りで、レイは「分からないとき」しか撃たない:
        //   1. 候補自身が同じ標的(同じ灯・同じサンプル点)を生き残らせている → 可視が確定
        //   2. 同じ標的を殺している → 遮蔽が確定(分母から外す)
        //   3. 別の灯を選んでいた/空だった → 分からないので、その候補の面から
        //      選ばれたサンプル点へバイアス補正レイを1本撃つ
        // 自分(j=0)にはレイを撃たない ―― 自分から見えないサンプルは Shade の影レイが
        // どのみち0にするので、ここで数え過ぎても結果に効かない
        const bool visibilityAware = (Params0.w != 0u) && (Params2.w != 0u) &&
                                     LightCastsRaytracedShadow(Lights[selectedLight].Params.y);
        const float selectedRadius = Lights[selectedLight].Params.z;
        const float3 selectedSamplePos = MegaLightsLightSamplePosition(
            Lights[selectedLight].PositionType.xyz, selectedRadius,
            Lights[selectedLight].DirectionAngle.xyz, (uint)Lights[selectedLight].PositionType.w,
            MegaLightsUnpackSampleUV(selectedSampleUV));

        float z = 0.0f;
        [loop]
        for (uint j = 0u; j < candidateCount; ++j)
        {
            if (candidate[j].M <= 0.0f)
            {
                continue;
            }
            if (!LightInTileDomain(selectedLight, candidateCoord[j], outputSize))
            {
                continue;
            }
            // 【三項演算子は使えない】HLSLの条件演算子は数値のスカラー/ベクトル/行列にしか
            // 使えず、構造体を返そうとすると error X3020 / G600E2A1F になる
            SpatialSurface surfaceJ;
            if (j == 0u)
            {
                surfaceJ = self;
            }
            else
            {
                surfaceJ = LoadSurface(candidateCoord[j], outputSize);
            }
            if (TargetPdfOn(surfaceJ, selectedLight) <= 0.0f)
            {
                continue;
            }

            // 【可視性の判定は全候補に掛ける ―― ただし時間検証レイが前提】
            // どの候補のストリームも「このフレーム・その画素で可視検証済みの灯しか
            // 配れない」形になっている(現フレームは初期可視レイ、履歴は時間検証レイ。
            // MegaLightsTemporal.hlsl)。だから「選ばれた灯がその候補の面から見えない」
            // ことが分かった候補は、実際にその灯を配れない ―― 分母から外すのが正しい。
            // 【検証されていない履歴が混ざる構成でこれをやってはいけない】遮蔽された灯を
            // 正当に運ぶ候補を誤って外し、分子に残った寄与が小さな分母で割られて
            // 参照の1万倍級のファイアフライになる(実測: 総和+17.5%。61.7f)。
            // 勝者自身は自分のサンプルについて可視検証済みなので必ず分母に残り、
            // W = Σw/(Z・p̂) の上界は変わらない(Z ≥ 勝者のM)。
            if (visibilityAware)
            {
                const uint candLight = MegaLightsUnpackLight(candidate[j].IndexAndFlags);
                const bool sameTarget = (candLight == selectedLight) &&
                    (selectedRadius <= 0.0f || candidate[j].SampleUV == selectedSampleUV);
                if (sameTarget && MegaLightsUnpackVisible(candidate[j].IndexAndFlags))
                {
                    // 可視が確定(このフレーム・その画素で検証済み)。レイ不要で数える
                }
                else if (sameTarget && candidate[j].W <= 0.0f)
                {
                    // 遮蔽が確定(殺しの持ち回り)。分母から外す
                    continue;
                }
                else if (j != 0u)
                {
                    // 分からない。バイアス補正レイで確かめる
                    const float slopeScale =
                        1.0f / max(dot(surfaceJ.N, normalize(selectedSamplePos - surfaceJ.WorldPos)),
                                   kMinSlopeScaleNdotL);
                    const float originBias =
                        (kRayOriginBias + length(surfaceJ.WorldPos - CameraPosition.xyz) * kRayOriginBiasSlope) *
                        slopeScale;
                    const float3 toSample = selectedSamplePos - surfaceJ.WorldPos;
                    const float sampleDist = length(toSample);
                    if (sampleDist > originBias &&
                        TraceLightVisibility(
                            surfaceJ.WorldPos + surfaceJ.N * originBias, toSample / sampleDist, originBias,
                            sampleDist) <= 0.0f)
                    {
                        continue;
                    }
                }
                // j==0(自分)で可視性が分からない場合は数える ――
                // 自分から見えないサンプルは Shade の影レイがどのみち0にする
            }
            z += candidate[j].M;
        }
        // 選ばれたサンプルを出した候補は必ず1つ以上あるはずだが、
        // 浮動小数の丸めで0になった場合に備える(0で割るとNaNが伝播する)
        if (z <= 0.0f)
        {
            OutputReservoirs[index] = MegaLightsMakeEmptyReservoir();
            return;
        }
        denominator = z;
    }

    MegaLightsReservoir result;
    // 【可視フラグ = このフレーム・この画素で可視を証明済みか】目標関数に可視性を
    // 入れているとき、勝者は自分の面からのレイを通過している ―― シェードは
    // このフラグを見て影レイを省く
    result.IndexAndFlags = MegaLightsPackLightAndFlags(selectedLight, visibilityTargetAware);
    result.SampleUV = selectedSampleUV;
    result.W = weightSum / (denominator * selectedTargetPdf);
    result.M = confidenceSum;
    OutputReservoirs[index] = result;
}
