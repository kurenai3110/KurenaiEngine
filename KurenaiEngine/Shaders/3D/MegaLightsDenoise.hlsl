// MegaLights のデノイザ。時間累積 + エッジ停止付き à-trous(SVGF; Schied 2017 の構成)。
//
// 【何を落とすのか】確率的サンプリングは1画素1本の影レイで推定するので、
// 半影や遮蔽の縁では可視率が0か1のどちらかしか引けず、1枚の絵では強いノイズになる。
// 時空間再利用(段階3・4)は**リザーバ**を混ぜて実効サンプル数を増やすが、
// それでも残る裾をここで落とす。**両者は別物で、混同しないこと** ――
// あちらは「どの灯を選ぶか」を改善し、こちらは「出た色」を空間・時間へならす。
//
// 【TAAと二重に掛けない】このエンジンは後段にTAAを持つ。TAAはYCoCgの近傍分散でクリップ
// するので、ノイズを「正当な信号の広がり」と解釈して履歴を毎フレーム棄却する ――
// ノイズもAAも両方失う。だからノイズはTAAへ渡す前にここで落とす。
// 逆にここで長く累積しすぎるとTAAのゴーストと重なって二重に尾を引くので、
// 時間累積は上限32フレーム(TAAより短く)で止める。
//
// 【アルベド復調】フィルタの前に「その画素の反射率」で割り、後で掛け戻す。
// 割らずにぼかすと、明るい面と暗い面の境界で色が滲む(テクスチャの模様が影へ漏れる)。
// 係数はG-Bufferから両側で同一に再計算するので保存は要らない。
// **拡散と鏡面を1本で復調するのは近似**で、強い鏡面ハイライトでは復調しきれない。
// 分けるには出力を2枚に増やす必要があるため、まず1本で出して残る破綻を見てから判断する。
//
// レイを撃たないので3バリアントすべてでコンパイルされる。
#include "NormalEncoding.hlsli"
#include "Samplers.hlsli"

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
    float4 ShadowParams;
    // 【宣言はここで止めている】読むのは ShadowParams まで
};

cbuffer MegaLightsDenoiseConstants : register(b1)
{
    // x=出力幅, y=出力高, z=履歴が使えるか(0なら履歴を読まない), w=à-trousの段(0起点)
    uint4 Params0;
    // x=à-trousのステップ幅(1,2,4,8...), y=時間累積の上限フレーム数,
    // z=輝度のエッジ停止の強さ, w=法線のエッジ停止の指数
    float4 Params1;
    // x=深度のエッジ停止の強さ, yzw=未使用
    float4 Params2;
};

Texture2D NormalTexture : register(t1);
Texture2D DepthTexture : register(t2);
Texture2D AlbedoTexture : register(t3);
Texture2D MaterialTexture : register(t4);
// G-Bufferパスが書いたモーションベクター(UV単位)。TAAと同じものを同じ引き方で使う
Texture2D VelocityTexture : register(t5);
// 入力。時間累積では MegaLights の生出力、à-trous では前段の出力
Texture2D InputTexture : register(t6);
// 前フレームの累積結果(rgb=復調済みの色, a=これまでに累積したフレーム数)
Texture2D HistoryTexture : register(t7);
// 前フレームのモーメント(x=輝度の1次, y=2次, z=履歴の長さ, w=未使用)
Texture2D HistoryMomentsTexture : register(t8);

RWTexture2D<float4> OutputTexture : register(u0);
RWTexture2D<float4> OutputMomentsTexture : register(u1);

// 履歴を採用する条件。時空間再利用(MegaLightsTemporal/Spatial)と同じ3つを同じしきい値で。
// **深度はView空間の線形値で比べること**(Reverse-Zの生値で比べてはいけない)
static const float kMaxRelativeDepthDiff = 0.05f;
static const float kMinNormalDot = 0.9f;
static const float kMaxMaterialDiff = 0.1f;

// 復調に使う反射率の下限。0で割ると黒い面で発散する
static const float kMinDemodulation = 0.05f;

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

// その画素の「反射率」。フィルタの前にこれで割り、後で掛け戻す。
// **時間累積側とà-trous側で必ず同じ式を使うこと** ―― ずれると掛け戻したときに色が変わる
float3 DemodulationFactor(float2 uv)
{
    const float3 albedo = AlbedoTexture.SampleLevel(ColorSampler, uv, 0).rgb;
    const float metallic = MaterialTexture.SampleLevel(DataSampler, uv, 0).r;
    // 金属は拡散を持たず反射色がアルベドになる。誘電体は拡散アルベド + 4%の鏡面。
    // 拡散と鏡面を分けない近似(冒頭のコメント参照)
    const float3 f0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
    const float3 factor = lerp(albedo, float3(0.0f, 0.0f, 0.0f), metallic) + f0;
    return max(factor, float3(kMinDemodulation, kMinDemodulation, kMinDemodulation));
}

float TileViewZ(float2 uv, float depth)
{
    return mul(float4(ReconstructWorldPos(uv, depth), 1.0f), View).z;
}

// ---------------------------------------------------------------------------
// 段1: 時間累積。速度ベクトルで再投影し、指数移動平均で混ぜる
// ---------------------------------------------------------------------------
[numthreads(8, 8, 1)]
void CSTemporalAccum(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadID.xy;
    const uint2 outputSize = Params0.xy;
    if (pixel.x >= outputSize.x || pixel.y >= outputSize.y)
    {
        return;
    }

    const float2 uv = (float2(pixel) + 0.5f) / float2(outputSize);
    const float depth = DepthTexture.SampleLevel(DataSampler, uv, 0).r;
    const float3 raw = InputTexture.Load(int3(pixel, 0)).rgb;

    if (depth <= 0.0f)
    {
        // 背景。【必ず書くこと】RHIにUAVのクリアが無く、書かずにreturnすると前フレームが残る
        OutputTexture[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        OutputMomentsTexture[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    // --- 復調してから混ぜる ---
    const float3 demod = DemodulationFactor(uv);
    const float3 current = raw / demod;
    const float lum = Luminance(current);

    const float3 N = OctDecode(NormalTexture.SampleLevel(DataSampler, uv, 0).xy);
    const float viewZ = TileViewZ(uv, depth);
    const float2 material = MaterialTexture.SampleLevel(DataSampler, uv, 0).rg;

    // --- 再投影。TAAとまったく同じ引き方(historyUv = uv - velocity) ---
    const float2 velocity = VelocityTexture.SampleLevel(DataSampler, uv, 0).rg;
    const float2 historyUv = uv - velocity;

    float3 historyColor = float3(0.0f, 0.0f, 0.0f);
    float2 historyMoments = float2(0.0f, 0.0f);
    float historyLength = 0.0f;
    bool historyValid = false;

    if (Params0.z != 0u && all(historyUv >= 0.0f) && all(historyUv <= 1.0f))
    {
        // 【幾何の判定は再投影先の現フレームの値で行う】前フレームの幾何は保存していないが、
        // 深度・法線は1フレームでは大きく動かないので、現フレームのG-Bufferを
        // 再投影先で引いて代用する。**動きの速い物では判定が甘くなる**ぶん、
        // 下の輝度クリップ(近傍の分散でクランプ)が受け皿になる
        const float hDepth = DepthTexture.SampleLevel(DataSampler, historyUv, 0).r;
        if (hDepth > 0.0f)
        {
            const float hViewZ = TileViewZ(historyUv, hDepth);
            const float3 hN = OctDecode(NormalTexture.SampleLevel(DataSampler, historyUv, 0).xy);
            const float2 hMaterial = MaterialTexture.SampleLevel(DataSampler, historyUv, 0).rg;
            if (abs(hViewZ - viewZ) <= kMaxRelativeDepthDiff * max(abs(viewZ), 1e-3f) &&
                dot(N, hN) >= kMinNormalDot &&
                abs(hMaterial.r - material.r) <= kMaxMaterialDiff &&
                abs(hMaterial.g - material.g) <= kMaxMaterialDiff)
            {
                const float4 h = HistoryTexture.SampleLevel(ColorSampler, historyUv, 0);
                const float4 hm = HistoryMomentsTexture.SampleLevel(ColorSampler, historyUv, 0);
                historyColor = h.rgb;
                historyMoments = hm.xy;
                historyLength = hm.z;
                historyValid = true;
            }
        }
    }

    // --- 混ぜる ---
    // α = 1/min(履歴の長さ+1, 上限)。上限で止めるのは、止めないと動く物に追従できなくなるため。
    // 上限をTAAより短くするのは冒頭の「二重に掛けない」の通り
    const float maxFrames = max(Params1.y, 1.0f);
    const float newLength = historyValid ? min(historyLength + 1.0f, maxFrames) : 1.0f;
    const float alpha = 1.0f / newLength;

    const float3 blended = historyValid ? lerp(historyColor, current, alpha) : current;
    const float2 moments = historyValid
                               ? lerp(historyMoments, float2(lum, lum * lum), alpha)
                               : float2(lum, lum * lum);

    // 分散。履歴が短いうちは時間方向の分散が信用できないので、
    // à-trous 側で空間の分散を代用する(ここでは長さを渡すだけ)
    OutputTexture[pixel] = float4(blended, 1.0f);
    OutputMomentsTexture[pixel] = float4(moments, newLength, 0.0f);
}

// ---------------------------------------------------------------------------
// 段2: エッジ停止付き à-trous。段ごとにステップ幅を倍にして広い範囲をならす
// ---------------------------------------------------------------------------
static const float kAtrousKernel[3] = { 3.0f / 8.0f, 1.0f / 4.0f, 1.0f / 16.0f };

[numthreads(8, 8, 1)]
void CSAtrous(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadID.xy;
    const uint2 outputSize = Params0.xy;
    if (pixel.x >= outputSize.x || pixel.y >= outputSize.y)
    {
        return;
    }

    const float2 uv = (float2(pixel) + 0.5f) / float2(outputSize);
    const float depth = DepthTexture.SampleLevel(DataSampler, uv, 0).r;
    const float4 center = InputTexture.Load(int3(pixel, 0));

    if (depth <= 0.0f)
    {
        OutputTexture[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        OutputMomentsTexture[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    const float3 N = OctDecode(NormalTexture.SampleLevel(DataSampler, uv, 0).xy);
    const float viewZ = TileViewZ(uv, depth);
    const float4 centerMoments = HistoryMomentsTexture.Load(int3(pixel, 0));

    // 輝度の分散。E[l^2] - E[l]^2。履歴が短いうちは負にもなるのでクランプする
    float variance = max(centerMoments.y - centerMoments.x * centerMoments.x, 0.0f);
    const float historyLength = centerMoments.z;
    if (historyLength < 4.0f)
    {
        // 【履歴が短い画素は時間方向の分散を信用しない】その場の空間分散で代用する。
        // これをやらないと、遮蔽が外れた直後(disocclusion)の画素が
        // 「分散0 = 信用できる」と誤判定され、ノイズがそのまま残る
        float m1 = 0.0f;
        float m2 = 0.0f;
        float count = 0.0f;
        [unroll]
        for (int dy = -3; dy <= 3; ++dy)
        {
            [unroll]
            for (int dx = -3; dx <= 3; ++dx)
            {
                const int2 p = clamp(int2(pixel) + int2(dx, dy), int2(0, 0), int2(outputSize) - 1);
                const float l = Luminance(InputTexture.Load(int3(p, 0)).rgb);
                m1 += l;
                m2 += l * l;
                count += 1.0f;
            }
        }
        m1 /= count;
        m2 /= count;
        variance = max(m2 - m1 * m1, 0.0f);
    }
    const float lumStdDev = sqrt(variance) + 1e-6f;
    const float centerLum = Luminance(center.rgb);

    const int step = (int)max(Params1.x, 1.0f);
    float3 sum = float3(0.0f, 0.0f, 0.0f);
    float weightSum = 0.0f;

    [unroll]
    for (int dy = -2; dy <= 2; ++dy)
    {
        [unroll]
        for (int dx = -2; dx <= 2; ++dx)
        {
            // 【画面外は折り返す】同じシェーダ内で境界の扱いが2種類あると将来混乱する。
            // 上の空間分散のフォールバックと同じ clamp に揃えてある。
            // なお打ち切っても採用したタップだけで正規化するので、どちらでも
            // 系統的な偏りにはならない(縁を30px切って測っても損失は減らなかった)
            const int2 p = clamp(int2(pixel) + int2(dx, dy) * step, int2(0, 0), int2(outputSize) - 1);
            const float2 puv = (float2(p) + 0.5f) / float2(outputSize);
            const float pDepth = DepthTexture.SampleLevel(DataSampler, puv, 0).r;
            if (pDepth <= 0.0f)
            {
                continue;
            }

            const float3 tap = InputTexture.Load(int3(p, 0)).rgb;

            // --- エッジ停止 ---
            // 深度: 別の面へ滲ませない。視線に対して斜めの面でも切れないよう相対差で見る
            const float pViewZ = TileViewZ(puv, pDepth);
            const float wZ = exp(-abs(pViewZ - viewZ) / (Params2.x * max(abs(viewZ), 1e-3f) + 1e-6f));
            // 法線: 角を丸めない
            const float3 pN = OctDecode(NormalTexture.SampleLevel(DataSampler, puv, 0).xy);
            const float wN = pow(saturate(dot(N, pN)), Params1.w);
            // 輝度: 分散で正規化する。ノイズなら分散が大きいので広く混ぜ、
            // 本物の明暗差なら分散が小さいので混ぜない ―― これがSVGFの要。
            //
            // 【この重みは原理的にエネルギーを減らす】中心から遠い値ほど弾くので、
            // 1/p の重みが作る裾の重い分布では**明るいタップのほうが強く弾かれる**。
            // 結果としてフィルタ後の平均が暗い側へ寄る。実測(ManyLightsTest / N=256)で
            // 段を重ねるごとに総和が -1.1% / -2.5% / -5.8% と積み上がった。
            // 実装ミスではなくSVGF系の既知の性質で、σ(Params1.z)を大きくすると混ぜる範囲が
            // 広がるぶん損失も増える。**平均を動かさないことを優先するなら段数を減らす。**
            // 根拠と実測は docs/ImplementationDetail.md 61.7d
            const float wL = exp(-abs(centerLum - Luminance(tap)) / (Params1.z * lumStdDev));

            const float kernel = kAtrousKernel[abs(dx)] * kAtrousKernel[abs(dy)];
            const float w = kernel * wZ * wN * wL;
            sum += tap * w;
            weightSum += w;
        }
    }

    // 【0除算のガード】全部の近傍が弾かれたら中心をそのまま返す
    const float3 filtered = (weightSum > 1e-6f) ? (sum / weightSum) : center.rgb;
    OutputTexture[pixel] = float4(filtered, center.a);
    // モーメントはそのまま次の段へ渡す。
    // 【本家SVGFからの逸脱】あちらは段ごとに分散を3x3で平滑化してから使う。
    // ここは未フィルタの時間分散を全段で使い回すので、既に平滑化された入力に対して
    // lumStdDev が過大になり、**後段ほど輝度の重みが1へ寄ってただのぼかしになる**。
    // 4段で誤差の中央値が悪化する(0.0030→0.0055)のはこれで説明がつく。
    // 既定が2段である限り実害は小さいが、段数を増やすならここを直すのが先
    OutputMomentsTexture[pixel] = centerMoments;
}

// ---------------------------------------------------------------------------
// 段3: 復調を戻して最終出力にする
// ---------------------------------------------------------------------------
[numthreads(8, 8, 1)]
void CSRemodulate(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadID.xy;
    const uint2 outputSize = Params0.xy;
    if (pixel.x >= outputSize.x || pixel.y >= outputSize.y)
    {
        return;
    }
    const float2 uv = (float2(pixel) + 0.5f) / float2(outputSize);
    const float depth = DepthTexture.SampleLevel(DataSampler, uv, 0).r;
    if (depth <= 0.0f)
    {
        OutputTexture[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }
    // 【復調に使ったのと同じ式で掛け戻す】ずれると色が変わる
    const float3 demod = DemodulationFactor(uv);
    OutputTexture[pixel] = float4(InputTexture.Load(int3(pixel, 0)).rgb * demod, 1.0f);
}
