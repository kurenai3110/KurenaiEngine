// MegaLights の空間再利用。近傍の画素が選んだ灯を借りて、自分の面で評価し直して結合する。
// レイは1本も増えない ―― 借りるのは「どの灯か」だけで、影レイは後段の Shade が1本撃つだけ。
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
// レイを撃たないので3バリアントすべてでコンパイルされる。
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
    // x=借りる近傍の数, y=探す半径(ピクセル),
    // z=結合の方式(0=confidence重み, 1=生成化バランスヒューリスティック), w=未使用
    uint4 Params2;
    // x=射影行列の(0,0)成分, y=同(1,1)成分, zw=未使用。
    // MIS重みが「その灯が隣のタイルへ届くか」を判定するのに、隣のタイルの錐台を組み立て直す
    float4 Params3;
};

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
    const uint2 tileCoord = pixelCoord / tileSize;
    const uint candidateCount = Params1.z;
    const uint base = MegaLightsTilePoolBase(tileCoord, Params1.x, candidateCount);

    if (TilePool[base + 2u] == 0u)
    {
        return false;
    }
    // 深度スラブは候補プールがヘッダへ書いている(そのタイルを走査しないと分からないため)
    const float nearestViewZ = asfloat(TilePool[base + 4u]);
    const float farthestViewZ = asfloat(TilePool[base + 5u]);

    const TileFrustum frustum =
        MakeTileFrustum(tileCoord, outputSize, Params3.x, Params3.y, nearestViewZ, farthestViewZ);

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
    uint rngState = HashUint(pixel.x + pixel.y * outputSize.x + Params1.w * 0x85EBCA6Bu + 0x27D4EB2Du);

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

        const uint lightI = MegaLightsUnpackLight(candidate[i].LightAndFlags);
        // 【借りた灯は必ず自分の面で評価し直す】隣で良かった灯がここで良いとは限らない
        const float pdfSelf = TargetPdfOn(self, lightI);
        if (pdfSelf <= 0.0f)
        {
            continue;
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
    result.LightAndFlags = MegaLightsPackLightAndFlags(selectedLight, true);
    result.SampleUV = 0u;
    result.W = weightSum / (denominator * selectedTargetPdf);
    result.M = confidenceSum;
    OutputReservoirs[index] = result;
}
