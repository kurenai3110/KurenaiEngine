// MegaLights の初期サンプリング。候補プールから M 個引いて RIS で1灯へ絞り、
// 結果を**リザーバとして**書き出す(色は作らない。シェードは MegaLightsShade.hlsl)。
//
// 【なぜ色ではなくリザーバを書くのか】時間・空間の再利用は「どの灯を選んだか」を
// 持ち回って現フレームで評価し直す形で行う。色を持ち回ると、遮蔽物が動いたときに
// 古い明るさが残り続ける。分けておけば、再利用の段が Initial と Shade の間に入るだけで済む。
//
// 【なぜこれで全灯評価と同じ答えになるのか】RIS は「粗い提案分布 p で M 個引き、
// 目標関数 p̂ に比例する重みで1つ選び、最後に 1/p̂ と Σw/M を掛け戻す」形の推定量で、
// 期待値が Σ_i f_i(全灯の合計)に一致する。ノイズは乗るが偏りは無い。
//
// 【提案分布 p はどこから来るか】候補プール(MegaLightsTilePool.hlsl)がタイルごとに
// K 個のスロットを持ち、各スロットは p_i = w_i / SumW から独立同分布に引かれている。
// スロットを一様に1つ選べば、それは p からの1サンプルになる。
//
// 【1フレームだけ見ると偏る】プールの K スロットは1タイル内の全ピクセルで共有され、
// 届いているのにどのスロットにも入らなかった灯はそのフレームでは選ばれない。
// プールの種にフレーム番号を混ぜて毎フレーム引き直しているため時間平均では消えるが、
// **混ぜるのをやめると偏ったまま収束しなくなる。**
//
// 【初期可視レイは空間再利用と組で使う(どちらも既定で有効)】選んだサンプルへ影レイを
// 1本撃ち、遮蔽されていたらリザーバごと殺す(RTXDI系では標準の段)。
// 殺しの意味は「遮蔽で0になるサンプルを近傍へ配らない」ことなので、
// **空間再利用が無いと絵が1bitも変わらない**(殺されるサンプルはシェード側のレイでも
// どうせ0)。逆に空間再利用は殺しが無いと実測でほぼ効かない。
// 【殺すときはどの灯を殺したかをリザーバへ残す】殺された画素のストリームは
// 「可視な灯しか配れない」形に変わるため、空間再利用の不偏化の分母(Z)は可視性まで
// 含めて数える必要がある。番号を残せばZ側で確定情報として使え、不明な近傍にだけ
// バイアス補正レイを撃てば済む(詳細は MegaLightsSpatial.hlsl。
// 残さない実装は -3.6% 暗く偏った。docs/ImplementationDetail.md 61.7f)。
// 【影を二重に掛けてはいけない】撃つ場合もここは「サンプルを殺す」だけで、影の階調は
// シェード側の1本が決める(殺されたサンプルはそもそもシェードへ渡らない)。
//
// DX12 かつ DXR Tier 1.1 のときだけ生成される(RayQuery は SM 6.5 の機能)。
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
    // x=出力幅, y=出力高, z=1ピクセルあたりの初期候補数M, w=影レイを撃つか
    uint4 Params0;
    // x=候補プールの有効タイル数X(格子ジッター有効時だけ+1)、
    // y=タイルの1辺のピクセル数, z=1タイルあたりの候補数K, w=フレーム番号
    uint4 Params1;
    // xyz=空間再利用用(このパスでは未使用)、w=初期可視レイでリザーバを殺すか。
    // 【途中のフィールドを飛ばしてはいけない】wだけ欲しくてもxyzごと宣言する
    // (飛ばすと誤ったオフセットを読み、コンパイルは通り絵もそれらしく出るため気付けない)
    uint4 Params2;
    // x=射影(0,0), y=射影(1,1), z=未使用, w=履歴Mの上限(このパスでは未使用)
    float4 Params3;
    // x=時間再利用の履歴が有効か(殺しのヒントを読んでよいか)、y=空間再利用の反復番号(未使用)、
    // z=クアッド共有(Resolveが読む。このパスでは未使用)、
    // w=クアッドで候補スロットを分けて引くか(手法3の層化)
    uint4 Params4;
    // x=1画素あたりの標本数(リザーバの本数)。手法3だけが1より大きくなる。
    // 【末尾へ足すこと】途中へ挿すと Shade / Temporal / Spatial のオフセットがずれる
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
#include "MegaLightsCommon.hlsli"

// 候補プール。レイアウトは MegaLightsTilePool.hlsl 冒頭を参照
StructuredBuffer<uint> TilePool : register(t7);

RWStructuredBuffer<MegaLightsReservoir> Reservoirs : register(u0);
// 画素ごとの「遮蔽が確定した灯」のキャッシュ(0xFFFFFFFFで無し)。
// 【なぜ持続させるのか】殺しの持ち回り(リザーバ)はそのフレームに殺しが起きた
// 画素にしか無い。初期RISが別の灯を引いたフレームには知識が消え、その隙に
// 空間再利用が遮蔽された支配光を近傍から借りて影レイを無駄にする ――
// 影の縁に暗い粒のフリンジが残る原因。キャッシュなら毎フレーム効く。
// 【新鮮さ】殺しで記録し、同じ灯が可視レイを通ったら即消す。支配的な灯は
// RISがほぼ毎フレーム引き直すので、遮蔽が解けた次のフレームには消える。
// 履歴が無効なフレーム(解像度変更直後など)は読まずに上書きだけする
RWStructuredBuffer<uint> BlockedLights : register(u1);

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

// 呼ぶたびに状態を進める乱数。RIS はスロットの抽選と採用判定で2回引く
float NextRandom(inout uint state)
{
    state = HashUint(state);
    return float(state) * 2.3283064365e-10f; // uintの最大値で割って[0,1)へ
}

float Luminance(float3 c)
{
    return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

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

// 1画素ぶんの標本すべてへ同じリザーバを書く(背景・候補なしの早期脱出用)。
// 【1本だけ書いて帰ってはいけない】RHIにバッファのクリアが無いので、
// 書かなかったスロットには前フレームの残骸が残り、Resolveがそれを平均に混ぜる
void WriteAllReservoirs(uint base, uint count, MegaLightsReservoir value)
{
    [loop]
    for (uint s = 0u; s < count; ++s)
    {
        Reservoirs[base + s] = value;
    }
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

    // 【リザーバは1画素にN本、遮蔽キャッシュは1画素に1つ】キャッシュは
    // 「この画素からこの灯は見えない」という画素の性質で、標本ごとには持たない
    const uint samplesPerPixel = max(Params5.x, 1u);
    const uint reservoirIndex = pixel.y * outputSize.x + pixel.x;
    const uint reservoirBase = reservoirIndex * samplesPerPixel;

    const float2 uv = (float2(pixel) + 0.5f) / float2(outputSize);
    const float depth = DepthTexture.SampleLevel(DataSampler, uv, 0).r;
    if (depth <= 0.0f)
    {
        // 背景。【必ず書くこと】RHIにバッファのクリアが無く、書かずにreturnすると
        // 前フレームの残骸が残り、シェード側が存在しないサンプルを引く
        WriteAllReservoirs(reservoirBase, samplesPerPixel, MegaLightsMakeEmptyReservoir());
        BlockedLights[reservoirIndex] = 0xFFFFFFFFu;
        return;
    }

    const float3 worldPos = ReconstructWorldPos(uv, depth);
    const float4 albedoSample = AlbedoTexture.SampleLevel(ColorSampler, uv, 0);
    const float3 albedo = albedoSample.rgb;
    const float translucency = albedoSample.a;
    const float3 N = OctDecode(NormalTexture.SampleLevel(DataSampler, uv, 0).xy);
    const float2 material = MaterialTexture.SampleLevel(DataSampler, uv, 0).rg;
    const float metallic = material.r;
    const float roughness = material.g;

    const float3 V = normalize(CameraPosition.xyz - worldPos);
    const float NdotV = saturate(dot(N, V)) + 1e-5f;

    const float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
    const float3 brdf = BRDFLUTTexture.SampleLevel(ColorSampler, float2(NdotV, roughness), 0).rgb;
    const SpecularEnergyContext energy = MakeSpecularEnergyContext(F0, brdf, roughness, ShadowParams.w);

    // --- このピクセルが属するタイルの候補プールを引く ---
    const uint tileSize = max(Params1.y, 1u);
    // 書き手の [tile*16-offset, tile*16-offset+16) と逆写像になる同じ格子規約
    const uint2 tileCoord = (pixel + Params6.xy) / tileSize;
    const uint candidateCount = Params1.z;
    const uint tileBase = MegaLightsTilePoolBase(tileCoord, Params1.x, candidateCount);

    const float sumW = asfloat(TilePool[tileBase + 0u]);
    // 混合抽出(一様枝 + 重み枝)の割り戻しに要る(MegaLightsTilePool.hlsl)
    const uint reachableCount = TilePool[tileBase + 1u];
    const uint validCandidates = TilePool[tileBase + 2u];
    const uint sampleCount = max(Params0.z, 1u);
    if (sumW <= 0.0f || validCandidates == 0u)
    {
        // 【空でも M は候補数を持たせる】M はこの画素が「何個の候補を検討したか」で、
        // 結果が空だったかどうかとは独立。ここを0にすると、空間再利用の分母
        // (confidenceSum と Z)から「引いたが外した」画素が消えて分母が過小になり、
        // **明るい側の系統誤差**になる(詳細は選択失敗側のコメント)
        MegaLightsReservoir empty = MegaLightsMakeEmptyReservoir();
        empty.M = float(sampleCount);
        WriteAllReservoirs(reservoirBase, samplesPerPixel, empty);
        // 何も分からなかったフレーム。キャッシュは維持(履歴が無効なら信用できないので消す)
        if (Params4.x == 0u)
        {
            BlockedLights[reservoirIndex] = 0xFFFFFFFFu;
        }
        return;
    }

    // --- 遮蔽が確定した灯を目標関数から外す(キャッシュ) ---
    // 影の縁では目標関数を支配する灯が自分からは遮蔽されていることがあり、RISは
    // 可視性を知らないのでその灯を毎フレーム選んでは殺される ―― デノイザ前の
    // 暗黒点の主因(実測で1フレームあたり点灯画素の3.6%)。BlockedLights は
    // その灯の遮蔽が可視レイで確定した画素にだけ入っているので、目標関数を0として
    // 扱えば抽選が「届く別の灯」へ向かう。
    // 【16フレームに1回だけ再検証を許す】除外し続けると殺しが起きなくなり、
    // 遮蔽が解けたことを知る機会(可視レイ)が失われる。位相は画素ごとにずらす。
    // 静止シーンではキャッシュは常に正しく、期待値は変わらない(目標関数の変更は
    // RISでは自由で、定義域の縮小は空間再利用の分母のレイ判定が正しく数える)。
    // 動的シーンでは解けた遮蔽に平均8フレームで気づく(その間はその灯を選ばない
    // だけで、他の灯の寄与は正しいまま)
    uint blockedLight = 0xFFFFFFFFu;
    if (Params4.x != 0u)
    {
        const bool retest = ((Params1.w + pixel.x * 3u + pixel.y * 7u) & 15u) == 0u;
        if (!retest)
        {
            blockedLight = BlockedLights[reservoirIndex];
        }
    }

    // --- 1画素あたり samplesPerPixel 本を独立に引く ---
    //
    // 【なぜ本数を増やせるようにしたか】クアッド共有は2x2の4本を平均するので、
    // 1画素1本でも実効的には4標本ある。それでも動いている間はノイズが目に見える。
    // 内訳は2つで、どちらも標本数を増やすことでしか減らない:
    //   1. どの灯を選ぶか(RISの分散)
    //   2. 選んだ灯の可視性(球光源の球面上の1点への1本。ここが大きい ――
    //      参照実装を1本と32本で比べると|相対誤差|のp90が0.37あった)
    // UE5の MegaLights も r.MegaLights.NumSamplesPerPixel を 2/4/16 から選ぶ形で、
    // **最小でも2**である。1本しか撃たないのはこちらの予算の決め方の問題だった。
    //
    // 【N本は互いに独立に引く ―― 層化はクアッドの中だけ】種も位相も標本番号で分ける。
    // 期待値は1本のときと同じで、平均の分散が 1/N になる。
    //
    // 【BlockedLights を触るのは0番の標本だけ】キャッシュは画素に1つしかないので、
    // N本が競って書くと「最後に書いた者勝ち」になり、どの標本の判断が残ったのか
    // 追えなくなる。0番の判断に固定しておけば N を変えても挙動が動かない
    [loop]
    for (uint sampleSlot = 0u; sampleSlot < samplesPerPixel; ++sampleSlot)
    {
        const bool ownsCache = (sampleSlot == 0u);

        // --- RIS: 候補プールから M 個引いて、寄与の大きさに比例する重みで1つ残す ---
        // 【スロットの抽選だけ低食い違い量列にする】画素ごとの位相をブルーノイズ的に配り、
        // 同じ画素の中では M 個が均等に散るようにする。周辺分布は一様のままなので
        // 割り戻しも期待値も変わらない(MegaLightsCommon.hlsli の説明を参照)。
        // 採用判定は白色のまま ―― あちらは M 回の判定の独立性を使っている。
        // 標本番号を位相の次元として渡し、N本が同じ列を引かないようにする
        const float slotPhase = MegaLightsPixelPhase(pixel, Params1.w, sampleSlot);
        uint rngState = HashUint(pixel.x + pixel.y * outputSize.x + Params1.w * 0x9E3779B9u +
                                 sampleSlot * 0xB5297A4Du);

        // --- クアッド層化(手法3。Params4.w) ---
        // 2x2クアッドの4画素へ候補スロットを1/4ずつ割り当て、**クアッド全体でK個のスロットを
        // 重複なく列挙させる**。手法3は4画素の標本を平均するので、4人が同じ灯を引いてしまうと
        // 実効的な標本数が減る。
        //
        // 【周辺分布は変わらないので割り戻しはそのまま厳密】プールのK個のスロットは
        // 混合分布(一様枝+重み枝)からの **i.i.d. 抽出** である(MegaLightsTilePool.hlsl)。
        // スロットの中身を見ずに番号だけで選ぶ限り、どのスロットを引いても得られる灯の分布は
        // 同じ混合分布のままで、下の sourcePdf の式は変わらない。
        // 【MegaLightsCommon.hlsli が禁じている層化とは別物】あちらが禁じているのは
        // 「1つのスロット列の中で (m + phase)/M と等間隔に取る」形で、周辺分布が層の中に
        // 閉じてしまうために提案と割り戻しが食い違う。こちらは層の中で一様に引いている。
        // 【レーンの割り当てはクアッドごと・フレームごとに回す】固定すると
        // 「左上の画素はいつも先頭8スロットから引く」形になり、2画素周期の模様が焼き付く。
        // 標本番号ぶんもずらして、同じ画素のN本が同じ層に固まらないようにする
        const bool quadStratify = (Params4.w != 0u);
        uint stratumBase = 0u;
        uint stratumCount = validCandidates;
        if (quadStratify && validCandidates >= 4u)
        {
            const uint2 quad = pixel >> 1u;
            const uint lane = (pixel.x & 1u) | ((pixel.y & 1u) << 1u);
            const uint rotation = HashUint(quad.x + quad.y * 0x9E3779B9u + Params1.w * 0x85EBCA6Bu) & 3u;
            const uint stratum = (lane + rotation + sampleSlot) & 3u;
            const uint width = validCandidates >> 2u;
            stratumBase = stratum * width;
            // 最後の層は端数を引き受ける(K=32なら割り切れるが、Kを変えても定義域が欠けないように)
            stratumCount = (stratum == 3u) ? (validCandidates - stratumBase) : width;
        }

        float risWeightSum = 0.0f;
        uint selectedLightIndex = 0xFFFFFFFFu;
        float selectedTargetPdf = 0.0f;

        [loop]
        for (uint m = 0u; m < sampleCount; ++m)
        {
            const float slotRandom = MegaLightsLowDiscrepancy1D(m, slotPhase);
            // 層化しているときは自分の層の中だけを引く(層化していなければ全スロットが自分の層)
            const uint slot =
                stratumBase + min((uint)(slotRandom * float(stratumCount)), stratumCount - 1u);
            const uint lightIndex = TilePool[tileBase + kMegaLightsTilePoolHeader + 2u * slot + 0u];
            const float candidateWeight = asfloat(TilePool[tileBase + kMegaLightsTilePoolHeader + 2u * slot + 1u]);
            // 採用判定の乱数は候補が無効でも必ず引いて状態を進める
            // (引く回数がループの中身で変わると、ピクセルごとに乱数列の位相がずれる)
            const float acceptRandom = NextRandom(rngState);

            if (lightIndex == 0xFFFFFFFFu || candidateWeight <= 0.0f)
            {
                continue;
            }
            // 遮蔽が確定している灯は目標関数0として扱う(= 選ばない)。提案分布は
            // 変えていないので「引いたが目標0で外れた」という正当な棄却で、期待値は不変
            if (lightIndex == blockedLight)
            {
                continue;
            }
            const GPULight light = Lights[lightIndex];
            const PunctualGeometry geometry = EvaluatePunctualGeometry(light, worldPos, N, translucency);
            if (!geometry.Contributes)
            {
                continue;
            }

            // 目標関数。遮蔽は含めない(含めるにはレイを撃つことになりRISの意味が無くなる)
            const float3 unshadowed = EvaluatePunctualContribution(
                light, geometry, N, V, NdotV, albedo, metallic, roughness, translucency, energy, 1.0f);
            const float targetPdf = Luminance(unshadowed);
            if (targetPdf <= 0.0f)
            {
                continue;
            }

            // 提案分布の確率密度。プールは「一様枝 + 重み枝」の混合で引いている
            // (MegaLightsTilePool.hlsl)ので、割り戻しも同じ混合式で行う。
            // プールが w_i / SumW / 届いた灯数 を別々に持っているので厳密に再現できる
            const float sourcePdf = kMegaLightsUniformMixFraction / float(max(reachableCount, 1u)) +
                                    (1.0f - kMegaLightsUniformMixFraction) * (candidateWeight / sumW);
            const float risWeight = targetPdf / sourcePdf;

            risWeightSum += risWeight;
            if (acceptRandom < risWeight / risWeightSum)
            {
                selectedLightIndex = lightIndex;
                selectedTargetPdf = targetPdf;
            }
        }

        // 【0除算のガードは必須】どの候補も寄与しないピクセルでここを割るとNaNが出て、
        // 直接光→SceneColor→TAAの履歴まで壊れて復帰しなくなる
        if (selectedLightIndex == 0xFFFFFFFFu || selectedTargetPdf <= 0.0f || risWeightSum <= 0.0f)
        {
            // 【M=0 で書いてはいけない ―― 空間再利用の明るい側の系統誤差の原因だった】
            // 「M個引いて全部外した(全候補が背向き等)」は、遮蔽で殺した場合と同じく
            // 「M個の候補を検討して寄与0だった」という正当な結果である。ここを M=0 にすると、
            // 結合の分母(confidenceSum と、不偏化方式の Z)からこの画素の分だけが消える。
            // 全部外すのは背向き候補率の高い画素に集中して起きるため、その周囲だけ分母が
            // 系統的に過小になり、期待値が明るい側へ偏る(不偏性の条件は
            // 「分母 = 選ばれた灯を生成しえた候補の M の合計」であり、
            // 外した画素も生成しえた=確率が正だった以上、M ごと数えなければならない)
            MegaLightsReservoir rejected = MegaLightsMakeEmptyReservoir();
            rejected.M = float(sampleCount);
            Reservoirs[reservoirBase + sampleSlot] = rejected;
            if (ownsCache && Params4.x == 0u)
            {
                BlockedLights[reservoirIndex] = 0xFFFFFFFFu;
            }
            continue;
        }

        // --- 球光源: 狙う点を抽選してリザーバへ持たせる ---
        // 【選択ループの外で引くこと】ループ内で引くと、候補が無効だった回数で乱数列の位相が
        // ずれて画素ごとに相関が出る。半径0なら使われないが、引く回数は常に同じにしておく
        const float2 sampleUV = float2(NextRandom(rngState), NextRandom(rngState));

        // --- 初期可視レイ: 遮蔽されていたらここで殺す ---
        // 殺すと「遮蔽で真っ黒になる灯」が近傍へ配られなくなる(RTXDI系の標準の段)。
        // 【ただし空間再利用の不偏化(Z)とは両立しない】殺された画素の実効的な定義域は
        // p̂ から p̂・可視率 へ変わるが、Zはレイを撃たずに可視率を判定できないため、
        // 影の縁に暗い側の系統誤差が残る。Params2.w で切って測れるようにしてある。
        // 【レイはここで標本ごとに1本ずつ撃つ】1画素あたりの影レイの本数は
        // samplesPerPixel そのものになる
        bool visible = true;
        if (Params0.w != 0u && Params2.w != 0u)
        {
            const GPULight selectedLight = Lights[selectedLightIndex];
            if (LightCastsRaytracedShadow(selectedLight.Params.y))
            {
                const PunctualGeometry geometry =
                    EvaluatePunctualGeometry(selectedLight, worldPos, N, translucency);
                if (geometry.Contributes)
                {
                    const float slopeScale = 1.0f / max(dot(N, geometry.L), kMinSlopeScaleNdotL);
                    const float originBias =
                        (kRayOriginBias + length(worldPos - CameraPosition.xyz) * kRayOriginBiasSlope) * slopeScale;
                    // シェード側と同じ点へ撃つ(違う点を狙うと、殺す判断と影の階調が食い違う)
                    const float3 samplePos = MegaLightsLightSamplePosition(
                        selectedLight.PositionType.xyz, selectedLight.Params.z,
                        selectedLight.DirectionAngle.xyz, (uint)selectedLight.PositionType.w, sampleUV);
                    const float3 toSample = samplePos - worldPos;
                    const float sampleDist = length(toSample);
                    if (sampleDist > originBias)
                    {
                        visible = TraceLightVisibility(
                                      worldPos + N * originBias, toSample / sampleDist, originBias, sampleDist) > 0.0f;
                    }
                }
            }
        }

        if (!visible)
        {
            // 【どの灯を殺したかを残す ―― 全部消して書いてはいけない】
            // W=0 なので結合の選択からは外れる(IsEmptyがtrue)が、
            // 「この画素はこの灯への可視レイが遮蔽された(V=0 が確定した)」という事実を
            // ライト番号と可視フラグで持ち回る。空間再利用の不偏化の分母(Z)は
            // 「その候補が選ばれた灯を生成しえたか」を数えるが、殺された灯は
            // その候補からは決して出て来られない。番号を消すと Z がそれを知れずに
            // M を数え、殺しの起きる画素の周囲だけ分母が太って**暗い側の系統誤差**になる
            // (実測 -3.6%。docs/ImplementationDetail.md 61.7f)。
            // M は残す ―― 「M個の候補を検討した」ことは事実で、他の灯の Z には数えるべき
            MegaLightsReservoir killed = MegaLightsMakeEmptyReservoir();
            killed.LightAndFlags = MegaLightsPackLightAndFlags(selectedLightIndex, false);
            killed.SampleUV = MegaLightsPackSampleUV(sampleUV);
            killed.M = float(sampleCount);
            Reservoirs[reservoirBase + sampleSlot] = killed;
            // 【点光源だけキャッシュする】球光源の殺しは球面上の1点への判定で、
            // 灯そのものの遮蔽の証明にならない
            if (ownsCache)
            {
                if (Lights[selectedLightIndex].Params.z <= 0.0f)
                {
                    BlockedLights[reservoirIndex] = selectedLightIndex;
                }
                else if (Params4.x == 0u)
                {
                    BlockedLights[reservoirIndex] = 0xFFFFFFFFu;
                }
            }
            continue;
        }

        // 可視レイを通った(または影を撃たない灯を選んだ)。キャッシュの灯と同じなら
        // 「遮蔽が解けた」ことの証明なので消す。違う灯ならキャッシュは維持
        if (ownsCache && (Params4.x == 0u || BlockedLights[reservoirIndex] == selectedLightIndex))
        {
            BlockedLights[reservoirIndex] = 0xFFFFFFFFu;
        }

        MegaLightsReservoir reservoir;
        // ライト番号は16bitへ詰める(kMaxLights = 1024 なので収まる)
        reservoir.LightAndFlags = MegaLightsPackLightAndFlags(selectedLightIndex, true);
        // 球面上のどこを狙ったか。時空間再利用がこの点ごと持ち回るので、借りた側も同じ点へ撃つ
        // (半径0なら中心になり、点光源と完全に一致する)
        reservoir.SampleUV = MegaLightsPackSampleUV(sampleUV);
        // 不偏寄与重み W = (1/p̂(y)) * (1/M) * Σw
        reservoir.W = risWeightSum / (float(sampleCount) * selectedTargetPdf);
        reservoir.M = float(sampleCount);
        Reservoirs[reservoirBase + sampleSlot] = reservoir;
    }
}
