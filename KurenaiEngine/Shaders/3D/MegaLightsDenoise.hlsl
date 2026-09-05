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
// 前フレームの幾何(MegaLightsHistoryGuide)。時間再利用が毎フレーム書いているものを
// 履歴の妥当性判定に使う。リソースは宣言していないヘッダなので取り込んでも束縛は増えない
#include "MegaLightsCommon.hlsli"

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
    // x=深度のエッジ停止の強さ, y=ファイアフライのクランプ強さ(0で無効),
    // z=前フレームの幾何(履歴ガイド)が使えるか(0なら現フレームのG-Bufferで代用), w=未使用
    float4 Params2;
};

// 前フレームの幾何。時間再利用(MegaLightsTemporal)が毎フレーム全画素へ書いている。
// 【使わないフレームでも必ずバインドする】DX12は宣言したリソースが未バインドだと壊れる
StructuredBuffer<MegaLightsHistoryGuide> HistoryGuide : register(t0);

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
// モーメント(x=輝度の1次, y=2次, z=履歴の長さ, w=輝度の分散)。
// 【.w の分散は à-trous が段ごとにフィルタして次段へ渡す】時間累積が最初の値を作り、
// 各段が重みの二乗で畳んで書き戻す(本家SVGFの構成)
Texture2D HistoryMomentsTexture : register(t8);

RWTexture2D<float4> OutputTexture : register(u0);
RWTexture2D<float4> OutputMomentsTexture : register(u1);
// 時間累積(CSTemporalAccum)が「次フレームの履歴」として書く先。
// 【u0/u1と別に要る】u0/u1(ping)はà-trousが上書きしていくので、
// 翌フレームが読み戻す履歴は独立したバッファに残さなければならない。
// C++側は最初からここへ履歴バッファを束縛していたが、シェーダが宣言しておらず
// **時間累積が一度も履歴を書けていなかった**(historyValidの幾何判定は現フレームの
// G-Bufferを見るので通ってしまい、長さだけがゴミ値のまま毎フレーム1へ戻っていた)。
// à-trous / Remodulate では書かない(C++はダミーとして自分の出力を重ねて束縛する)
RWTexture2D<float4> HistoryOutTexture : register(u2);
RWTexture2D<float4> HistoryMomentsOutTexture : register(u3);

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
        HistoryOutTexture[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        HistoryMomentsOutTexture[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    // --- 復調してから混ぜる ---
    const float3 demod = DemodulationFactor(uv);
    float3 current = raw / demod;

    // --- ファイアフライの抑制(近傍クランプ) ---
    // 【何を切っているのか】RISの 1/p の重みは裾の重い分布を作る。たまたま小さい確率で
    // 引かれた灯は大きな W で割り戻され、桁違いに明るい1画素になる。実測(履歴が無い
    // 状態 = カメラを動かした直後に相当)で**画素の3.0%が期待値の3倍以上、2,353画素が
    // 10倍以上**で、これが画面上で最も目につく「白い粒」だった。
    // 【上側だけ切る】暗い側は触らない。黒く沈んだ画素は影の縁の黒い斑点の原因になり、
    // そちらは別の仕組み(可視性込みの目標関数とBlockedLightsキャッシュ)で潰してある。
    // 【色相は保つ】輝度の比で全成分を縮める。成分ごとに切ると色が転ぶ。
    // 【平均+k・標準偏差では切れない】一度そう書いて外した。履歴が無い場所では近傍も
    // 同じだけノイジーで、**標準偏差がファイアフライ自身に押し上げられる**ため上限が
    // 外れ値の上に来る。実測(移動直後に相当する条件)でk=4は10倍超の画素を2,353→2,260と
    // 4%しか減らせなかった。基準には外れ値に強い量が要る。
    // 【採ったやり方】5x5の平均を取り、その8倍を超えるタップを外してもう一度平均を取る
    // (1回の刈り込み平均)。上限はその k 倍。平均は25画素で薄まるので、3%が外れ値でも
    // 基準はほとんど動かない。
    // 【バイアスが入る】切ったぶんのエネルギーは戻らないので、強さは
    // 「総和比を落とさない範囲でどこまで切れるか」で決める(EngineDefaults.h の根拠を参照)
    if (Params2.y > 0.0f)
    {
        const float invDemodLum = 1.0f / max(Luminance(demod), 1e-6f);
        float tapLum[25];
        float rawMean = 0.0f;
        [unroll]
        for (int cy = -2; cy <= 2; ++cy)
        {
            [unroll]
            for (int cx = -2; cx <= 2; ++cx)
            {
                const int2 cp = clamp(int2(pixel) + int2(cx, cy), int2(0, 0), int2(outputSize) - 1);
                const float l = Luminance(InputTexture.Load(int3(cp, 0)).rgb) * invDemodLum;
                tapLum[(cy + 2) * 5 + (cx + 2)] = l;
                rawMean += l;
            }
        }
        rawMean /= 25.0f;

        // 刈り込み平均。極端なタップを外してから取り直す
        const float trimLimit = rawMean * 8.0f;
        float trimmedSum = 0.0f;
        float trimmedCount = 0.0f;
        [unroll]
        for (uint t = 0u; t < 25u; ++t)
        {
            if (tapLum[t] <= trimLimit)
            {
                trimmedSum += tapLum[t];
                trimmedCount += 1.0f;
            }
        }
        const float robustMean = (trimmedCount > 0.0f) ? (trimmedSum / trimmedCount) : rawMean;

        const float limit = robustMean * Params2.y;
        const float curLum = Luminance(current);
        if (curLum > limit && curLum > 1e-6f)
        {
            current *= limit / curLum;
        }
    }

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
        // --- 再投影先の幾何を引く ---
        // 【前フレームの幾何そのものを見る】以前は現フレームのG-Bufferを再投影先で
        // 引いて代用していた。これは**動く細い形状で必ず失敗する** ―― 椅子の桟が
        // 毎フレーム数画素動くと、historyUv が指す現フレームの位置にはもう桟がなく
        // 背後の壁が写っているので、深度が桁違いに食い違って履歴が棄却される。
        // 棄却された画素は1サンプルの推定値がそのまま出るため、**動かしたときだけ
        // 細い形状に白い粒が乗る**という形で現れていた。
        // 時間再利用が前フレームの法線・線形深度・材質を全画素ぶん書いているので、それを読む。
        // 時間再利用を切っている場合はガイドが更新されないので、従来どおり代用する
        float hViewZ;
        float3 hN;
        float2 hMaterial;
        bool hValid;
        if (Params2.z != 0.0f)
        {
            const int2 historyPixel =
                clamp(int2(historyUv * float2(outputSize)), int2(0, 0), int2(outputSize) - 1);
            const MegaLightsHistoryGuide guide =
                HistoryGuide[historyPixel.y * outputSize.x + historyPixel.x];
            hViewZ = guide.ViewZ;
            hN = OctDecode(MegaLightsUnpackNormalOct(guide.NormalOct));
            MegaLightsUnpackMaterial(guide.Material, hMaterial.x, hMaterial.y);
            // ガイドは背景を ViewZ=0 で表す
            hValid = (guide.ViewZ != 0.0f);
        }
        else
        {
            const float hDepth = DepthTexture.SampleLevel(DataSampler, historyUv, 0).r;
            hViewZ = (hDepth > 0.0f) ? TileViewZ(historyUv, hDepth) : 0.0f;
            hN = OctDecode(NormalTexture.SampleLevel(DataSampler, historyUv, 0).xy);
            hMaterial = MaterialTexture.SampleLevel(DataSampler, historyUv, 0).rg;
            hValid = (hDepth > 0.0f);
        }

        if (hValid)
        {
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

    // --- 分散を作る。ここで1回だけ作り、à-trousは段ごとにこれを畳んで次段へ渡す ---
    // 【なぜここへ移したのか】以前はà-trousが毎段この場で時間分散を計算し直していた。
    // 段を重ねると入力は既に平滑化されているのに分散だけ生のままなので、
    // 輝度の門番の分母(σ・√分散)が過大になり、**後段ほど重みが1へ寄って
    // ただのぼかしになる**。実測でも3段以上は誤差が悪化した(2段0.0394 → 5段0.0461)
    float variance = max(moments.y - moments.x * moments.x, 0.0f);
    if (newLength < 4.0f)
    {
        // 【履歴が短い画素は時間分散を信用しない】その場の7x7で代用する。
        // これをやらないと、遮蔽が外れた直後(disocclusion)の画素が
        // 「分散0 = 信用できる」と誤判定され、ノイズがそのまま残る。
        // タップの復調は中心の係数で代用する(反射率は7x7の窓では大きく変わらない)
        const float invDemodLum = 1.0f / max(Luminance(demod), 1e-6f);
        float sm1 = 0.0f;
        float sm2 = 0.0f;
        float count = 0.0f;
        [unroll]
        for (int dy = -3; dy <= 3; ++dy)
        {
            [unroll]
            for (int dx = -3; dx <= 3; ++dx)
            {
                const int2 p = clamp(int2(pixel) + int2(dx, dy), int2(0, 0), int2(outputSize) - 1);
                const float l = Luminance(InputTexture.Load(int3(p, 0)).rgb) * invDemodLum;
                sm1 += l;
                sm2 += l * l;
                count += 1.0f;
            }
        }
        sm1 /= count;
        sm2 /= count;
        variance = max(sm2 - sm1 * sm1, 0.0f);
    }

    OutputTexture[pixel] = float4(blended, 1.0f);
    OutputMomentsTexture[pixel] = float4(moments, newLength, variance);
    // 翌フレームの履歴。pingはà-trousが上書きするので独立に残す。
    // .wは翌フレームの時間累積では読まない(分散はそのフレームで作り直す)
    HistoryOutTexture[pixel] = float4(blended, 1.0f);
    HistoryMomentsOutTexture[pixel] = float4(moments, newLength, variance);
}

// ---------------------------------------------------------------------------
// 段2: エッジ停止付き à-trous。段ごとにステップ幅を倍にして広い範囲をならす
// ---------------------------------------------------------------------------
static const float kAtrousKernel[3] = { 3.0f / 8.0f, 1.0f / 4.0f, 1.0f / 16.0f };
// 門番に渡す分散を平滑化するための3x3ガウス(中心 / 辺 / 角 = 4:2:1 の分離型)
static const float kGaussian3x3[2] = { 2.0f / 4.0f, 1.0f / 4.0f };

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

    // 輝度の分散。時間累積が作り、前段までのà-trousが畳んできた値をそのまま受け取る
    const float variance = max(centerMoments.w, 0.0f);

    // 【門番に使う分散は3x3で平滑化する(本家SVGF)】分散そのものが1画素ぶんの推定で
    // ノイジーなので、生の値で門番を作ると「たまたま分散が小さく出た画素」だけが
    // 近傍と混ざらず粒として残る。重み計算に使うのは平滑化した値、
    // 次段へ渡すのは下で重みの二乗で畳んだ値で、役割が違うことに注意
    float varSmooth = 0.0f;
    float varWeightSum = 0.0f;
    [unroll]
    for (int vy = -1; vy <= 1; ++vy)
    {
        [unroll]
        for (int vx = -1; vx <= 1; ++vx)
        {
            const int2 vp = clamp(int2(pixel) + int2(vx, vy), int2(0, 0), int2(outputSize) - 1);
            const float g = kGaussian3x3[abs(vx)] * kGaussian3x3[abs(vy)];
            varSmooth += g * max(HistoryMomentsTexture.Load(int3(vp, 0)).w, 0.0f);
            varWeightSum += g;
        }
    }
    varSmooth = (varWeightSum > 1e-6f) ? (varSmooth / varWeightSum) : variance;

    const float lumStdDev = sqrt(varSmooth) + 1e-6f;
    const float centerLum = Luminance(center.rgb);

    const int step = (int)max(Params1.x, 1.0f);
    float3 sum = float3(0.0f, 0.0f, 0.0f);
    float weightSum = 0.0f;
    // 分散は重みの二乗で畳む。Var[Σw_i x_i / Σw_i] = Σw_i^2 Var[x_i] / (Σw_i)^2
    float varSum = 0.0f;

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
            // 【中心の輝度が厳密に0の画素は、輝度の門番を外して近傍から埋める】
            // 確率的サンプリングでは「届く灯を一度も引き当てられない画素」が影の縁に
            // 黒い斑点として残る(可視レイで殺され続けるか、目標関数が遮蔽された灯に
            // 支配される画素)。輝度の門番は黒を「本物のエッジ」として守ってしまうので、
            // 中心が0のときだけ無効化する。真に照らされない画素は近傍も0なので安全
            // (0どうしの平均は0のまま)
            const float wL = (centerLum <= 0.0f)
                                 ? 1.0f
                                 : exp(-abs(centerLum - Luminance(tap)) / (Params1.z * lumStdDev));

            const float kernel = kAtrousKernel[abs(dx)] * kAtrousKernel[abs(dy)];
            const float w = kernel * wZ * wN * wL;
            sum += tap * w;
            weightSum += w;
            varSum += w * w * max(HistoryMomentsTexture.Load(int3(p, 0)).w, 0.0f);
        }
    }

    // 【0除算のガード】全部の近傍が弾かれたら中心をそのまま返す
    const float3 filtered = (weightSum > 1e-6f) ? (sum / weightSum) : center.rgb;
    const float filteredVariance =
        (weightSum > 1e-6f) ? (varSum / (weightSum * weightSum)) : variance;
    OutputTexture[pixel] = float4(filtered, center.a);
    // xyz(1次・2次モーメントと履歴の長さ)はそのまま、wだけ畳んだ分散に差し替えて次段へ渡す。
    // これで段が進むほど分散が小さくなり、輝度の門番が効き続ける
    OutputMomentsTexture[pixel] = float4(centerMoments.xyz, filteredVariance);
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
