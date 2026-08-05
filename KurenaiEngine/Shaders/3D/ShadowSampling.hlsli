// カスケードシャドウマップのサンプリング(PCSS)共通処理。
// DirectLighting.hlsl(ディファードの直接光)・Transparent.hlsl(半透明フォワード)から#includeする。
// NormalEncoding.hlsli・SpecularEnergy.hlsli・Samplers.hlsliに次ぐ4つ目の共有ヘッダー。
//
// 【インクルードの前提】このファイルは以下が宣言済みであることを前提にしているため、
// FrameConstants cbufferとSamplers.hlsliの後ろで#includeすること
// (既存の共有ヘッダーと同様、インクルードガードは持たない):
//   - Samplers.hlsli の DataSampler(s2、Point+Clamp)。既存の2ファイルはいずれも
//     SpecularEnergy.hlsli経由で間接的にインクルードしている
//   - FrameConstants の ShadowParams / CascadeSplits / CascadeViewProj
//
// 【なぜ共通化しているか】DirectLighting.hlslとTransparent.hlslにPCSS実装を複製すると、
// 「片方だけ直すと半透明と不透明で影が食い違う」事故が起きる。実装を1箇所に集約することで、
// 両者が構造的にずれないようにしている

// 全カスケードの深度を1枚にまとめたテクスチャ配列。スライス番号がカスケード番号に対応する
// (エンジン側はKurenaiEngine3D::m_ShadowCascadeArray。CreateDepthTextureArrayで生成)。
// カスケードごとにTexture2Dを1枚ずつ宣言せずテクスチャ配列に統合してあるため、t4の1本で済む
Texture2DArray ShadowMapArray : register(t4);

// PCSS(Percentage Closer Soft Shadows)。ライト視点のクリップ空間へ変換した上で、
// (1)近傍のブロッカー(受光点より手前=光源側にある遮蔽物)の平均深度を探し、
// (2)受光点との深度差から半影(ペナンブラ)の広さを推定し、
// (3)その広さでPCF(複数タップの深度比較平均)を行う。
// ブロッカーが見つからない場合は完全に光が当たるとみなしPCFをスキップする(コスト削減も兼ねる)。
// 戻り値は0(完全に影)〜1(完全に光が当たる)の連続値。シャドウマップの範囲外は影を落とさない。
// cascadeIndexでサンプルする配列スライス(=カスケード)を選ぶ。
//
// 本来のPCSS(Fernando 2005)は透視投影のライトを前提に「受光点までの距離」で半影の広さを
// スケールするが、このエンジンの平行光は正射影のシャドウマップを使うため、代わりに
// 正規化された深度値([0,1])同士の比をそのまま使う近似で代用している
float ComputeShadowFactor(uint cascadeIndex, float4x4 cascadeViewProj, float3 worldPos, float NdotL)
{
    float4 lightClipPos = mul(float4(worldPos, 1.0f), cascadeViewProj);
    float3 lightNdc = lightClipPos.xyz / lightClipPos.w;

    if (abs(lightNdc.x) > 1.0f || abs(lightNdc.y) > 1.0f || lightNdc.z < 0.0f || lightNdc.z > 1.0f)
    {
        return 1.0f;
    }

    float2 shadowUV = float2(lightNdc.x * 0.5f + 0.5f, 1.0f - (lightNdc.y * 0.5f + 0.5f));
    float receiverDepth = lightNdc.z;

    // シャドウアクネ対策のバイアス。斜入射(NdotLが小さい)ほどアクネが出やすいため傾斜に応じて大きくする
    const float kShadowBiasMin = 0.0005f;
    const float kShadowBiasMax = 0.0025f;
    const float bias = lerp(kShadowBiasMax, kShadowBiasMin, NdotL);
    const float compareDepth = receiverDepth - bias;

    // シャドウマップの1テクセル分のUVサイズ(KurenaiEngine3D::kShadowMapSizeと合わせる)
    const float kTexelSize = 1.0f / 2048.0f;
    const float lightSize = max(ShadowParams.x, kTexelSize);

    // --- (1) ブロッカーサーチ: lightSizeの範囲を5x5タップでサンプルし、受光点より光源側にある
    //     (=深度がより小さい)テクセルの平均深度を求める ---
    const int kBlockerTaps = 5;
    const int kBlockerHalf = kBlockerTaps / 2;
    float blockerDepthSum = 0.0f;
    int blockerCount = 0;

    [unroll]
    for (int by = -kBlockerHalf; by <= kBlockerHalf; ++by)
    {
        [unroll]
        for (int bx = -kBlockerHalf; bx <= kBlockerHalf; ++bx)
        {
            const float2 offset = float2(bx, by) * (lightSize / float(kBlockerTaps));
            const float sampleDepth = ShadowMapArray.Sample(DataSampler, float3(shadowUV + offset, cascadeIndex)).r;
            if (sampleDepth < compareDepth)
            {
                blockerDepthSum += sampleDepth;
                blockerCount += 1;
            }
        }
    }

    if (blockerCount == 0)
    {
        return 1.0f;
    }

    const float avgBlockerDepth = blockerDepthSum / float(blockerCount);

    // --- (2) 半影サイズの推定 ---
    const float penumbraRatio = (receiverDepth - avgBlockerDepth) / max(avgBlockerDepth, 1e-5f);
    const float filterRadius = clamp(penumbraRatio * lightSize, kTexelSize, lightSize);

    // --- (3) 推定した半径でPCF(5x5タップの深度比較平均) ---
    const int kPCFTaps = 5;
    const int kPCFHalf = kPCFTaps / 2;
    float shadowSum = 0.0f;

    [unroll]
    for (int py = -kPCFHalf; py <= kPCFHalf; ++py)
    {
        [unroll]
        for (int px = -kPCFHalf; px <= kPCFHalf; ++px)
        {
            const float2 offset = float2(px, py) * (filterRadius / float(kPCFTaps));
            const float sampleDepth = ShadowMapArray.Sample(DataSampler, float3(shadowUV + offset, cascadeIndex)).r;
            shadowSum += (sampleDepth < compareDepth) ? 0.0f : 1.0f;
        }
    }

    return shadowSum / float(kPCFTaps * kPCFTaps);
}

// ピクセルのView空間深度からカスケード番号(0=カメラに近い方)を選び、そのスライスをサンプルする。
// シャドウマップをTexture2DArrayに統合してあるため、「HLSLはリソース(Texture2D)を
// 動的添字の配列として扱えない」制約(=カスケードごとの分岐)を受けない。
// 配列スライスとcbuffer配列(CascadeViewProj)はいずれも動的添字で選べる
float ComputeCascadedShadowFactor(float3 worldPos, float viewDepth, float NdotL)
{
    uint cascadeIndex = 0;
    if (viewDepth > CascadeSplits.x) cascadeIndex = 1;
    if (viewDepth > CascadeSplits.y) cascadeIndex = 2;
    if (viewDepth > CascadeSplits.z) cascadeIndex = 3;

    return ComputeShadowFactor(cascadeIndex, CascadeViewProj[cascadeIndex], worldPos, NdotL);
}
