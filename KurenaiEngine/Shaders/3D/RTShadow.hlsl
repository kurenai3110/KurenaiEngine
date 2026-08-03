// レイトレーシングシャドウ(RTシャドウ)パス。カスケードシャドウマップ(CSM)の代わりに
// 太陽の可視率を求め、単チャンネルのテクスチャ(R32_Float、0=完全に影 〜 1=完全に光が当たる)へ
// 書き出す。DirectLighting.hlslがこのテクスチャを読み、CSMのComputeCascadedShadowFactorの
// 戻り値と同じ位置で使う(詳細はdocs/Architecture.html 26章)。
//
// 【CSMに対する利点】
//   - カスケードの境界が無い。CSMは深度でシャドウマップを切り替えるため、遠景で解像度が
//     落ちる・境界でにじみ方が変わるといった問題があったが、RTには段が存在しない
//   - ピーターパン(接地部の影の浮き)が起きない。CSMは深度比較のバイアスでアクネと
//     ピーターパンを綱引きさせるしかなかったが、RTは実際の交差判定なので、押し出し量は
//     自己交差を避けるための微小な値で足りる
//   - シャドウマップの解像度に縛られないので、石壁の目地のような細かい接触影が出る
//     (2048x2048のCSMでは1テクセルに埋もれて消えていた)
//   - 半影(ペナンブラ)が距離に応じて正しく広がる。CSMのPCSSは正射影のシャドウマップから
//     半影幅を近似していたが、RTは太陽の見かけの大きさ(円盤)を直接サンプルする
//
// 【この段階の制約】docs/Architecture.html 26章。要点:
//   - 対象は太陽(平行光)のみ。ポイント/スポットライトの影は従来どおり
//     スクリーンスペースシャドウ(ScreenSpaceShadow.hlsli)が担当する
//   - 不透明サーフェス(G-Buffer)のみ。半透明(Transparent.hlsl)と反射プローブの
//     キャプチャ(ProbeCapture.hlsl)はカメラ視点の画面空間テクスチャを使えないため、
//     RTシャドウ選択時もCSMを描き続けてそちらを使う
//   - すべての三角形を不透明として扱う(RAY_FLAG_FORCE_OPAQUE)。アルファテスト付きの
//     葉などは板ポリのまま影を落とす(RTReflection.hlslと同じ理由)
//   - デノイザを持たないため、太陽を大きく(角半径を上げて)柔らかい影にするほど
//     サンプル数を増やさないとノイズが出る
//   - 太陽がほぼ真横から当たる面(NdotLが0に近い面)では可視率がピクセル単位で激しく
//     ばらつく。レイが面とほぼ平行に進むため、目地や庇のわずかな凹凸で当たる/当たらないが
//     決まってしまうためで、押し出し量では解消しない(実測で確認済み)。ただしその領域は
//     太陽の寄与自体が NdotL 倍でほぼ0になるため、最終的な絵には出ない
//     (デバッグ表示「RTシャドウ (太陽の可視率)」では見える)
#include "NormalEncoding.hlsli"

// レイの始点を法線方向へ押し出す量(ワールド単位)。深度バッファから復元したワールド座標は
// 遠方ほど誤差が大きいため、カメラからの距離に比例する項も足す(RTReflection.hlslと同じ扱い)
static const float kRayOriginBias = 0.01f;
static const float kRayOriginBiasSlope = 1e-4f;
// 押し出し量を1/NdotLでスケールするときの下限。0.1(入射角約84度)より浅い角度では
// これ以上増やさない。NdotLが小さいほど太陽の寄与自体が小さくなるため、
// 押し出しすぎて本来の影が抜けるほうが害が大きい
static const float kMinSlopeScaleNdotL = 0.1f;
// 影レイの最大距離(ワールド単位)。太陽は平行光なので本来は無限だが、シーン外まで飛ばしても
// 当たるものは無いので上限を設ける(RTReflection.hlslのkShadowRayMaxDistanceと同じ値)
static const float kSunRayMaxDistance = 1.0e4f;

static const float kTwoPI = 6.28318530718f;
// 黄金比の小数部。サンプルごとに方位角をずらす低食い違い量列(Rank-1格子)に使う
static const float kGoldenRatioFrac = 0.61803398875f;

cbuffer FrameConstants : register(b0)
{
    // このシェーダでは未使用(オフセット合わせのためだけに宣言する)
    float4x4 ViewProj;
    float4x4 InvViewProj;
    // このシェーダでは未使用(オフセット合わせのためだけに宣言する)
    float4x4 CascadeViewProj[4];
    float4 CameraPosition;
    // xyz=太陽の進行方向(光が飛んでいく向き)。太陽へ向かうベクトルは -LightDirection.xyz
    float4 LightDirection;
    // このシェーダでは未使用(オフセット合わせのためだけに宣言する)
    float4 LightColor;
    float4x4 View;
    float4x4 Proj;
    float4 AmbientColor;
    float4 CascadeSplits;
    float4 ShadowParams;
    float4 ActiveLightCount;
    float4 IBLParams;
    float4 ProbeParams;
    // 【宣言はここで止めている】このシェーダーが読むのはProbeParamsまでで、それより後ろは使わない。
    // C++側のFrameConstantsはこの後ろにTimeParams・Sky*・Cloud*・PlanarReflectionPlane・
    // Fog*・WaterBodyColorを持つが、cbufferは宣言順レイアウトなので、途中を飛ばして末尾だけを
    // 宣言すると誤ったオフセットを読む。しかもコンパイルは通り絵も「それらしく」出るため気付けない。
    // これらが必要になったら、C++の並びどおりに間のフィールドをすべて宣言すること
};

cbuffer RTShadowConstants : register(b1)
{
    // xy: 出力サイズ(ピクセル), z: 太陽の見かけの半径(ラジアン), w: 1ピクセルあたりのレイ本数
    float4 Params0;
};

// --- レイトレーシング資源 ---
RaytracingAccelerationStructure SceneTLAS : register(t0);

// --- G-Buffer ---
Texture2D NormalTexture : register(t1);
Texture2D DepthTexture : register(t2);

// 太陽の可視率。0=完全に影, 1=完全に光が当たる。
// R32_Floatのため型付きUAV読み書きが保証されている(AutoExposure.hlsl冒頭のコメント参照)
RWTexture2D<float> VisibilityOutput : register(u0);

float3 ReconstructWorldPos(float2 uv, float depth)
{
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float4 worldPos = mul(float4(ndc, depth, 1.0f), InvViewProj);
    return worldPos.xyz / worldPos.w;
}

// PCG系の整数ハッシュ。ピクセルごとにサンプル位置を散らすためだけに使う
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

// 太陽の見かけの円盤上の1点へ向かう方向を作る。angularRadiusは円盤の角半径(ラジアン)。
// toSunに直交する接平面上で半径angularRadiusの円板を一様サンプルし、それを方向へ足してから
// 正規化する近似(tan(r)≒r。角半径5度でも誤差0.3%未満なので太陽には十分)
float3 SampleSunDirection(float3 toSun, float3 tangent, float3 bitangent, float angularRadius, float2 u)
{
    // sqrtを掛けるのは円板上で面積一様にするため(そのままだと中心へ寄る)
    const float radius = angularRadius * sqrt(u.x);
    const float phi = kTwoPI * u.y;
    return normalize(toSun + tangent * (radius * cos(phi)) + bitangent * (radius * sin(phi)));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadID.xy;
    const uint2 outputSize = (uint2)Params0.xy;
    if (pixel.x >= outputSize.x || pixel.y >= outputSize.y)
    {
        return;
    }

    const float depth = DepthTexture[pixel].r;
    if (depth <= 0.0f)
    {
        // 背景(スカイ)。DirectLighting.hlslは背景ピクセルを先に打ち切るので実際には読まれないが、
        // デバッグ表示で「影ではない」と分かるように1で埋めておく。
        // Reverse-Zのため遠平面(=背景)はNDC z=0.0付近になる
        VisibilityOutput[pixel] = 1.0f;
        return;
    }

    const float3 toSun = normalize(-LightDirection.xyz);

    // ピクセル中心のUVからワールド座標を復元する
    const float2 uv = (float2(pixel) + 0.5f) / float2(outputSize);
    const float3 worldPos = ReconstructWorldPos(uv, depth);

    // G-Bufferの法線(=法線マップ適用後のシェーディング法線)をそのまま使う。
    // DirectLighting.hlslの太陽の打ち切りも同じ法線のNdotLで行っているため、
    // 「照らされていると判定するピクセル」が両者で完全に一致する
    const float3 N = OctDecode(NormalTexture[pixel].xy);
    const float NdotL = dot(N, toSun);
    if (NdotL <= 0.0f)
    {
        // 太陽に背を向けた面。レイを撃つまでもなく光は当たらない
        // (DirectLighting.hlsl側も sunNdotL <= 0 で太陽の寄与を打ち切る)
        VisibilityOutput[pixel] = 0.0f;
        return;
    }

    // 斜入射(NdotLが小さい)ほど自己交差(アクネ)が出やすいので押し出し量を増やす。
    // 法線方向へ d だけ持ち上げても、太陽方向へ進む間に元の面から離れる距離は d*NdotL しか
    // 稼げないため、1/NdotL で割るとどの入射角でも同じだけ面から離れることになる。
    // 完全な平行光(NdotL→0)で発散しないよう下限でクランプする
    const float slopeScale = 1.0f / max(NdotL, kMinSlopeScaleNdotL);
    const float originBias =
        (kRayOriginBias + length(worldPos - CameraPosition.xyz) * kRayOriginBiasSlope) * slopeScale;
    const float3 rayOrigin = worldPos + N * originBias;

    const float angularRadius = Params0.z;
    const uint sampleCount = max((uint)Params0.w, 1u);

    // 太陽方向に直交する基底。toSunと平行になりにくい軸を選んでからcrossする
    const float3 upAxis = (abs(toSun.z) < 0.999f) ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
    const float3 tangent = normalize(cross(upAxis, toSun));
    const float3 bitangent = cross(toSun, tangent);

    // ピクセルごとに固定の乱数(フレーム番号を混ぜない)。時間方向のジッタを入れると
    // デノイザ(蓄積)が無い今の構成ではノイズが毎フレーム動いてちらつくため、
    // 静止画としては粗くても安定するほうを選んでいる
    const uint seed = HashUint(pixel.x + pixel.y * outputSize.x + 0x9e3779b9u);
    const float randomOffset = float(seed) * 2.3283064365e-10f; // uint最大値で割って[0,1)へ
    const float randomPhase = float(HashUint(seed)) * 2.3283064365e-10f;

    uint visibleCount = 0u;

    [loop]
    for (uint i = 0u; i < sampleCount; ++i)
    {
        // 半径方向は層化(ストラティファイ)し、方位角は黄金比で回す。1本のときは
        // randomOffsetだけが効き、円盤内のランダムな1点になる
        const float2 u = float2(
            (float(i) + randomOffset) / float(sampleCount),
            frac(randomPhase + float(i) * kGoldenRatioFrac));

        RayDesc ray;
        ray.Origin = rayOrigin;
        ray.Direction = SampleSunDirection(toSun, tangent, bitangent, angularRadius, u);
        ray.TMin = originBias;
        ray.TMax = kSunRayMaxDistance;

        // 遮蔽の有無だけが分かればよいので、最初のヒットで打ち切る
        RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
        query.TraceRayInline(SceneTLAS, RAY_FLAG_NONE, 0xFFu, ray);
        query.Proceed();

        if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
        {
            visibleCount += 1u;
        }
    }

    VisibilityOutput[pixel] = float(visibleCount) / float(sampleCount);
}
