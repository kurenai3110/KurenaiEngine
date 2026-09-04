// ライトの距離減衰の**唯一の定義**。
//
// 【なぜ独立したヘッダーなのか】この式は以前 4つのファイル(PunctualLighting.hlsli /
// ProbeShading.hlsli / Transparent.hlsl / PlanarReflection.hlsl)に別々に書かれていた。
// どれも字面まで同一だったが、**新しいライトの種類を1本にだけ足すと残り3本が
// それを素のポイントライトとして評価する**。減衰の分母は max(distSq, 0.0001f) で
// 下限が入っているだけなので、光源の位置にサーフェスがある場合(面光源をプロキシ化すると起きる)
// には 1e4 倍の照度になり、しかも**絵は出る**。
//
// とくに ProbeShading.hlsli は DDGI のプローブ焼き込みと反射プローブが通る道で、
// DDGI はヒステリシスで時間収束するため「起動直後は正常で、数秒かけて白く飽和していく」
// という追いにくい壊れ方をする。式を1か所にまとめて構造的に防ぐ。
//
// 【構造体にもレジスタにも依存させないこと】ProbeShading.hlsli と PunctualLighting.hlsli は
// どちらも同じ struct GPULight を宣言しており、同時にインクルードできない
// (PunctualLighting.hlsli 冒頭の注意を参照)。そのため**このヘッダーは GPULight を受け取らず、
// スカラだけを受け取る**。これが両方から読める唯一の形になっている。
//
// このヘッダーは他の何もインクルードしない(組み込みの max / saturate しか使わない)。

#ifndef KURENAI_LIGHT_ATTENUATION_HLSLI
#define KURENAI_LIGHT_ATTENUATION_HLSLI

// --- 影のフラグ(GPULight.Params.y)---
//
// 【1つの値に2つの意味を持たせない】以前は「影を落とすか」の真偽値1つで、
// スクリーンスペースシャドウとレイトレース影レイの**両方**を止めていた。
// エミッシブから起こした光源プロキシは数百灯になりうるのに、
// スクリーンスペースシャドウは画素あたりのレイ数に上限(既定4灯)がある。
// 1つのフラグのままだと「プロキシに影を出す」と決めた瞬間に、その予算を
// プロキシが食い尽くして手置きライトの接触影が消える。
//
// bit0 と bit1 を分ければ、プロキシは bit1 だけを立てられる。
// 既存のライトは 1.0 から 3.0(両方)へ変えれば挙動が変わらない。
static const uint kLightShadowScreenSpace = 1u; // スクリーンスペースシャドウを撃つ
static const uint kLightShadowRaytraced   = 2u; // MegaLights のレイトレース影レイを撃つ

bool LightCastsScreenSpaceShadow(float packedFlags)
{
    return (((uint)(packedFlags + 0.5f)) & kLightShadowScreenSpace) != 0u;
}

bool LightCastsRaytracedShadow(float packedFlags)
{
    return (((uint)(packedFlags + 0.5f)) & kLightShadowRaytraced) != 0u;
}

// Karis 2013 / Frostbite の打ち切り窓の**唯一の定義**(まだ二乗していない素の窓)。
// Range で厳密に0になり、打ち切り境界のハードエッジが出ない。
//
// 【切り出してある理由】この窓は punctual・エミッシブプロキシ・その上界の3か所で
// 使われており、段階2の三角形メッシュライト(MeshLighting.hlsli)も影響半径 R で
// 同じ形で切る。**打ち切りの形がずれると定義域がずれる** ―― 参照実装と確率的
// サンプリングで「どこまで届くか」が食い違い、絵は出るが期待値がずれる。
// 二乗する前で返すのは、呼び出し側の (window * window) の掛け順を変えないため
float LightRangeWindow(float distSq, float range)
{
    float factor = distSq / max(range * range, 1e-4f); // (d/r)^2
    return saturate(1.0f - factor * factor);           // 1 - (d/r)^4
}

// Karis 2013 / Frostbite の windowed inverse-square。Range を超えると厳密に0になり、
// 打ち切り境界でのハードエッジが出ない
float DistanceAttenuation(float distSq, float range)
{
    float window = LightRangeWindow(distSq, range);
    // 光源に極端に近づいたときの発散を抑える。定数1.0を足す実装はシーンスケール依存になるため、
    // 最小距離二乗でのクランプにする
    return (window * window) / max(distSq, 0.0001f);
}

// --- エミッシブ光源プロキシ(LightType 3)の減衰 ---
//
// 【何を近似しているか】面積 A・放射輝度 L の平らでない発光体を1点へ潰したもの。
// ランバート放射面の遠方場は指向性 κ = |Σ A_i n_i| / Σ A_i で決まり、
//
//     I(θ) = L * A * [ (1-κ)/4 + κ * max(0, cosθ) ]      θ は発光面の法線から
//
// の形にすると**κ によらず全光束が π*L*A に一致する**(∫I dω = L*A*[(1-κ)/4*4π + κ*π])。
// 両端も厳密で、κ=0(閉じた電球)は等方の L*A/4 ―― 凸閉曲面の全方向平均投影面積が A/4 という
// Cauchy の投影面積定理そのもの ―― κ=1(平らな片面)はランバートの L*A*cosθ になる。
//
// **等方で済ませて I = L*A*(1+κ)/4 とはしない。** κ=1 で光束が真値の2倍になり、
// 半分がパネルの裏へ漏れる。「壁に貼ってあるから裏は見えない」という前提に頼ることになり、
// 自立した看板や吊り下げパネルで破綻する。
//
// 【分母が d^2 ではなく d^2 + R^2 なのはなぜか】半径 R の円板の軸上照度には閉じた式があり、
//
//     厳密   : π*L*R^2 / (R^2 + d^2)
//     1/d^2  : π*L*R^2 / d^2            ← 1 + (R/d)^2 倍だけ過大
//
// なので、分母を d^2 + R^2 にすると**κ=1 の軸上は厳密に一致する**。
// これ1つで (1) 近傍での 1/d^2 の発散 (2) 発光面が自分自身を照らす問題
// (面の上では d≈0 かつ cosθ≈0 なので寄与がほぼ0に落ちる) (3) 半影の広がりを決める R との整合
// が同時に片付く。R は Params.z(面積等価の円板半径)で、球光源の SourceRadius と同じ枠。
//
// 【余弦は枝の中で求める】呼び出し側で正規化してから渡す形にすると、
// 型0/1/2 でも rsqrt が走る。ここで求めれば既存のライトの経路は1命令も増えない。
float EmissiveProxyAttenuation(
    float3 toLight,        // 受光点 → 光源中心。正規化しない
    float distSq,
    float range,
    float sourceRadius,    // Params.z
    float3 lightDirection, // DirectionAngle.xyz = 発光面の平均法線
    float directionality)  // Params.w = κ
{
    // 打ち切りの窓は punctual と同じ形。Range の境界で厳密に0になり、ハードエッジが出ない
    float window = LightRangeWindow(distSq, range);

    // 発光面から見て受光点がどちら側にあるか。-toLight が「光源から受光点へ」の向き
    float cosLightSide = -dot(lightDirection, toLight) * rsqrt(max(distSq, 1e-12f));
    float lobe = (1.0f - directionality) * 0.25f + directionality * saturate(cosLightSide);

    return lobe * (window * window) / max(distSq + sourceRadius * sourceRadius, 0.0001f);
}

// ライト1灯の距離減衰。**種類による分岐をここへ閉じ込める。**
//
// 【呼び出し側に枝を置かない】減衰の式を1か所へ集めたのに分岐を各ライトループへ配ると、
// 「同期が要る箇所」が式から分岐へ移るだけで何も解決しない。
float LightAttenuation(
    uint lightType, float3 toLight, float distSq, float range, float sourceRadius,
    float3 lightDirection, float directionality)
{
    if (lightType == 3u) // EmissiveProxy(Assets::LightType と一致させること)
    {
        return EmissiveProxyAttenuation(
            toLight, distSq, range, sourceRadius, lightDirection, directionality);
    }
    return DistanceAttenuation(distSq, range);
}

// 受光点の向きが分からない場所で使う、方向によらない上界。
//
// 【なぜ要るのか】MegaLights の候補プールはタイル単位で重みを作るので、画素ごとの向きを
// 使えない(タイルの中で法線も位置も違う)。しかもあそこは「届く灯を取りこぼさない」ことが
// 正しさの条件なので、**過小に見積もってはいけない**。
// 余弦ローブの最大は cosθ=1 のときの (1-κ)/4 + κ なので、それを掛ける。
float LightAttenuationUpperBound(
    uint lightType, float distSq, float range, float sourceRadius, float directionality)
{
    if (lightType == 3u)
    {
        float window = LightRangeWindow(distSq, range);
        float lobeMax = (1.0f - directionality) * 0.25f + directionality;
        return lobeMax * (window * window) / max(distSq + sourceRadius * sourceRadius, 0.0001f);
    }
    return DistanceAttenuation(distSq, range);
}

#endif // KURENAI_LIGHT_ATTENUATION_HLSLI
