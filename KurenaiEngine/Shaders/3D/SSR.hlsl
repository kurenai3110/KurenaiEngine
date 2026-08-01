// スクリーンスペースリフレクション(SSR)パス。
// Lightingパスで完成したSceneColor(HDR、トーンマップ前)を「反射先の環境色」として
// 簡易的に再利用し、G-Buffer(Normal/Material/Depth)を使ってワールド空間でレイマーチングする。
// HDRのまま扱うため、1.0を超える輝度(明るい光源の反射など)も正しく合成できる。
// トーンマッピングはこのパスより後段のTonemap.hlsl(Present直前)でまとめて行う。
//
// このパスは反射色を「加算」しない(20章)。Lightingパスは既に鏡面IBL
//   鏡面IBL = 環境の放射輝度(プローブ+グローバルIBLの合成) * SpecularIBLWeight(...)
// をSceneColorへ書き込んでいるため、SSRの結果をそのまま足すと同じ反射を二重に計上してしまう
// (14.9.5節。White Furnace TestがSSRを切っているのはこれが目に見える形で出るため)。
// 代わりにSSRは「環境の放射輝度だけを差し替える」:
//   出力 = SceneColor + (SSRが得た放射輝度 - Lightingが使った放射輝度) * SpecularIBLWeight(...) * 確信度
// 確信度が0なら出力はSceneColorと厳密に一致し、1ならSSRの放射輝度が鏡面IBLを完全に置き換える。
// 係数SpecularIBLWeightと環境の放射輝度SampleEnvironmentはReflectionProbe.hlsliで
// DeferredLighting.hlslと共有しており、「足した覚えのない値を引く」ことが起きないようにしている。
//
// レイが画面外に外れた場合や最大距離まで判定がつかなかった場合は確信度0とし、Lightingパスが
// 適用したプローブ/グローバルIBLをそのまま残す。プローブ導入以前は「その先に何があるか不明なため
// 何も足さない」という判断だったが、いまはプローブが画面外の情報を持っているため、
// 「何もしない=プローブに任せる」が正しい答えになった。
//
// このエンジンにはPSOのブレンドステートが無いため、既存のSSAO/SSILと同じ
// フルスクリーン三角形+ピクセルシェーダーのパターンで実装し、合成もこのシェーダー内で直接行う。
#include "NormalEncoding.hlsli"
#include "Samplers.hlsli"

static const int kSSRStepCount = 32;
static const int kSSRBinaryStepCount = 6;
static const float kSSREdgeFadeDistance = 0.1f;

// 反射プローブの環境ソースと鏡面IBLの重み(DeferredLighting.hlslと共有)。
// 拡散イラディアンスは使わないため、拡散側のレジスタは定義しない
#define KURENAI_GLOBAL_PREFILTERED_REGISTER t7
#define KURENAI_PROBE_PREFILTERED_REGISTER t8
#define KURENAI_PROBE_BUFFER_REGISTER t9
// 距離キューブ(19.12節)。DeferredLighting.hlslと同じ条件でコンパイルしないと、
// SSRが「Lightingが使ったのとは違う放射輝度」を引き算することになるため必ず定義する
#define KURENAI_PROBE_DISTANCE_REGISTER t10

cbuffer FrameConstants : register(b0)
{
    float4x4 ViewProj;
    float4x4 InvViewProj;
    // カスケードシャドウマップ用(このシェーダでは未使用。オフセット合わせのためだけに宣言する)
    float4x4 CascadeViewProj[4];
    float4 CameraPosition;
    float4 LightDirection;
    float4 LightColor;
    float4x4 View;
    float4x4 Proj;
    // このシェーダでは未使用(オフセット合わせのためだけに宣言する)。
    // a=昼度はかつて鏡面IBLの重みに含めていたが、手続き空の導入で不要になった(21.4節)
    float4 AmbientColor;
    // このシェーダでは未使用(オフセット合わせのためだけに宣言する)
    float4 CascadeSplits;
    // y: プリフィルタ済み鏡面マップの最大ミップレベル、z: IBL強度倍率、
    // w: スペキュラのマルチスキャッタリング・エネルギー補正のトグル
    float4 ShadowParams;
    // このシェーダでは未使用(オフセット合わせのためだけに宣言する)
    float4 ActiveLightCount;
    // 拡散イラディアンスの取得元切り替え。このシェーダは鏡面しか扱わないため未使用だが、
    // 後続のProbeParamsのオフセットを合わせるために宣言だけしている
    float4 IBLParams;
    // 反射プローブ用。ReflectionProbe.hlsliのプローブ選択・ブレンドが読む
    float4 ProbeParams;
    // 距離キューブ用(19.12節)。同じくReflectionProbe.hlsliが読む
    float4 ProbeParams2;
    // 【以下2つはこのシェーダーでは使わないが宣言だけ必要】cbufferは宣言順レイアウトなので、
    // 末尾のOcclusionParamsを正しいオフセットで読むには途中のフィールドを飛ばせない。
    // C++側のFrameConstantsと並びを必ず一致させること
    float4x4 PrevViewProj;
    float4 TAAParams;
    // bent normalによる遮蔽(25章)。DeferredLighting.hlslと必ず同じ値を読むこと
    float4 OcclusionParams;
};

cbuffer SSRConstants : register(b1)
{
    float4 Params0; // x: 最大レイ距離(ワールド単位), y: ヒット判定の厚み, z: ラフネスカットオフ, w: 未使用
};

Texture2D SceneColorTexture : register(t0);
Texture2D NormalTexture : register(t1);
Texture2D MaterialTexture : register(t2);
Texture2D DepthTexture : register(t3);
Texture2D AlbedoTexture : register(t4);
// SSAO/SSILのAO/GIバッファ。a=遮蔽率。スペキュラオクルージョンに使う
// (Lightingパスが適用した鏡面IBLの重みを再現するために必要)
Texture2D AOTexture : register(t5);
// split-sum近似の第2項、BRDF積分LUT
Texture2D BRDFLUTTexture : register(t6);
// bent normal(GBuffer.hlslがSV_TARGET5へ書いたワールド空間のbRaw)。
// このパスはt0〜t10を使っているためt11(25章)
Texture2D BentNormalTexture : register(t11);

// プリフィルタ済み鏡面(t7)・プローブのキューブマップ配列(t8)・プローブの影響範囲バッファ(t9)・
// プローブの距離キューブ(t10)の宣言と、プローブの選択・視差補正・ブレンド・鏡面IBLの重みは
// ReflectionProbe.hlsliが持つ
#include "ReflectionProbe.hlsli"

struct PSInput
{
    float4 Position : SV_POSITION;
    float2 UV : TEXCOORD0;
};

// 頂点バッファなしで画面全体を覆う三角形を1枚だけ生成する定番のテクニック
PSInput VSMain(uint vertexID : SV_VertexID)
{
    PSInput output;
    output.UV = float2((vertexID << 1) & 2, vertexID & 2);
    output.Position = float4(output.UV.x * 2.0f - 1.0f, 1.0f - output.UV.y * 2.0f, 0.0f, 1.0f);
    return output;
}

float3 ReconstructWorldPos(float2 uv, float depth)
{
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 clipPos = float4(ndc, depth, 1.0f);
    float4 worldPos = mul(clipPos, InvViewProj);
    return worldPos.xyz / worldPos.w;
}


// ワールド座標を画面UVとView空間Z(カメラからの距離。値が大きいほど遠い)へ投影する。
// カメラ背後、または画面外に出た場合はfalseを返す
bool ProjectToScreen(float3 worldPos, out float2 uv, out float viewZ)
{
    float4 clipPos = mul(float4(worldPos, 1.0f), ViewProj);
    if (clipPos.w <= 0.0f)
    {
        uv = float2(0.0f, 0.0f);
        viewZ = 0.0f;
        return false;
    }

    float3 ndc = clipPos.xyz / clipPos.w;
    uv = float2(ndc.x * 0.5f + 0.5f, 1.0f - (ndc.y * 0.5f + 0.5f));
    viewZ = mul(float4(worldPos, 1.0f), View).z;
    return (uv.x >= 0.0f && uv.x <= 1.0f && uv.y >= 0.0f && uv.y <= 1.0f);
}

// UV位置の実際のジオメトリのView空間Zを取得する。背景(深度なし)ならfalseを返す
bool SampleSceneViewZ(float2 uv, out float viewZ)
{
    float sceneDepth = DepthTexture.Sample(DataSampler, uv).r;
    if (sceneDepth <= 0.0f)
    {
        viewZ = 0.0f;
        return false;
    }
    float3 sceneWorldPos = ReconstructWorldPos(uv, sceneDepth);
    viewZ = mul(float4(sceneWorldPos, 1.0f), View).z;
    return true;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float3 baseColor = SceneColorTexture.Sample(ColorSampler, input.UV).rgb;

    float depth = DepthTexture.Sample(DataSampler, input.UV).r;
    if (depth <= 0.0f)
    {
        // 背景(スカイ)には反射元のサーフェスがない
        return float4(baseColor, 1.0f);
    }

    float3 albedo = AlbedoTexture.Sample(ColorSampler, input.UV).rgb;
    float3 material = MaterialTexture.Sample(DataSampler, input.UV).rgb;
    float metallic = material.r;
    float roughness = material.g;
    float materialAO = material.b; // マテリアルの遮蔽マップ(GBuffer.hlslでstrength適用済み)

    const float maxDistance = Params0.x;
    const float thickness = Params0.y;
    const float roughnessCutoff = Params0.z;

    // スクリーンスペースのレイマーチはヒット色を1点サンプルするだけで、粗い面に必要な
    // 円錐状のぼかしを持たない。そのため粗い面ほどSSRの結果を信用しない
    float roughnessFade = 1.0f - smoothstep(0.0f, roughnessCutoff, roughness);
    if (roughnessFade <= 0.0f)
    {
        // SSRを信用しない=Lightingパスが適用したプローブ/グローバルIBLをそのまま残す
        return float4(baseColor, 1.0f);
    }

    float3 worldPos = ReconstructWorldPos(input.UV, depth);
    float3 N = OctDecode(NormalTexture.Sample(DataSampler, input.UV).xy);
    float3 V = normalize(CameraPosition.xyz - worldPos);
    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
    float NdotV = saturate(dot(N, V));

    float3 reflectDir = normalize(reflect(-V, N));

    // --- Lightingパスが適用した鏡面IBLを、そのときとまったく同じ式で再現する ---
    // 環境の放射輝度と、それに掛かる係数。どちらもReflectionProbe.hlsliの定義を共有しているため、
    // ここで求めた値はLightingパスがSceneColorへ足したものと定義上一致する
    // aoの合成式はDeferredLighting.hlslのPSMainとまったく同じでなければならない
    // (スクリーンスペースの遮蔽 × マテリアルの遮蔽マップ)。ズレるとSSRが適用される領域と
    // されない領域の境界に段差が出る
    const float ssao = AOTexture.Sample(ColorSampler, input.UV).a;
    // bent normalもDeferredLighting.hlslとまったく同じ引き方をすること。
    // 反射ベクトルも同じものを渡す。あちらはreflect(-V, N)でnormalizeを挟まないが、
    // VとNが単位ベクトルならreflectは長さを保つので同じ向き・同じ長さになる
    const BentOcclusion bent = DecodeBentOcclusion(BentNormalTexture.Sample(DataSampler, input.UV), N);
    const bool useBent = OcclusionParams.y > 0.5f;
    const float3 brdf = BRDFLUTTexture.Sample(ColorSampler, float2(NdotV, roughness)).rgb;
    const float3 specularWeight =
        SpecularIBLWeight(F0, NdotV, roughness, useBent, bent, N, reflectDir, materialAO, ssao, brdf,
                          ShadowParams.w, ShadowParams.z);
    // Kulla-Conty方式の加算ローブ(SpecularIBLMultiScatterWeight)はここでは扱わない。
    // あれは拡散イラディアンスに掛かるほぼ拡散のローブで、鏡面反射として差し替える対象では
    // ないため、Lightingパスが足したまま残す(14.9節)

    const float mipLevel = roughness * ShadowParams.y;
    float3 unusedIrradiance;
    float3 envRadiance;
    SampleEnvironment(worldPos, N, reflectDir, mipLevel, unusedIrradiance, envRadiance);

    // 線形マーチ: レイに沿って一定間隔でサンプルし、G-Buffer深度より奥に入った地点(ヒット)を探す
    const float stepSize = maxDistance / float(kSSRStepCount);
    bool hit = false;
    bool skyHit = false;
    float2 hitUV = float2(0.0f, 0.0f);
    float tPrev = 0.0f;
    float tCurr = 0.0f;

    [loop]
    for (int i = 1; i <= kSSRStepCount; ++i)
    {
        tPrev = tCurr;
        tCurr = stepSize * float(i);

        float3 samplePos = worldPos + reflectDir * tCurr;
        float2 sampleUV;
        float rayViewZ;
        if (!ProjectToScreen(samplePos, sampleUV, rayViewZ))
        {
            // 画面外に外れた: この先に何があるか(スカイか別のジオメトリか)分からないため打ち切る
            break;
        }

        float sceneViewZ;
        if (!SampleSceneViewZ(sampleUV, sceneViewZ))
        {
            // 画面内で背景(スカイ)ピクセルに到達したことが確定したので、以降はスカイボックスへ
            // フォールバックしてよい
            skyHit = true;
            break;
        }

        if (rayViewZ >= sceneViewZ && rayViewZ - sceneViewZ < thickness)
        {
            hit = true;
            hitUV = sampleUV;
            break;
        }
    }

    // --- 環境の放射輝度を差し替える ---
    // newRadiance が envRadiance の代わりに使う放射輝度、confidence がその信用度
    float3 newRadiance = envRadiance;
    float confidence = 0.0f;

    if (hit)
    {
        // 2分探索でヒット区間[tPrev, tCurr]を精密化し、貫通による誤差を減らす
        float tLo = tPrev;
        float tHi = tCurr;
        [unroll]
        for (int j = 0; j < kSSRBinaryStepCount; ++j)
        {
            float tMid = (tLo + tHi) * 0.5f;
            float3 samplePos = worldPos + reflectDir * tMid;
            float2 sampleUV;
            float rayViewZ;
            float sceneViewZ;
            if (ProjectToScreen(samplePos, sampleUV, rayViewZ) && SampleSceneViewZ(sampleUV, sceneViewZ) && rayViewZ >= sceneViewZ)
            {
                hitUV = sampleUV;
                tHi = tMid;
            }
            else
            {
                tLo = tMid;
            }
        }

        // 画面内に実際に映っているサーフェスの色。プローブより新しく、視差も完全に正しい
        newRadiance = SceneColorTexture.Sample(ColorSampler, hitUV).rgb;

        // 反射先が画面の縁に近いほど信用を落とす(画面外へレイが抜ける際の急な打ち切りを緩和する)。
        // 縁で確信度が0へ落ちると、その分だけプローブ/グローバルIBLへ滑らかに戻る
        float2 edgeDist = min(hitUV, float2(1.0f, 1.0f) - hitUV);
        float edgeFade = saturate(min(edgeDist.x, edgeDist.y) / kSSREdgeFadeDistance);

        confidence = roughnessFade * edgeFade;
    }
    else if (skyHit)
    {
        // 画面内で実際にスカイへ到達したことが確定した場合。プローブは屋内の壁を返しうるが、
        // このレイは確かに外へ抜けているので、空のほうが正しい答えになる。
        // 生のスカイボックスではなくプリフィルタ済み鏡面をラフネス→ミップで引く
        // (以前は生のスカイボックスを引いていたため、粗い面でも鮮鋭な鏡像が返っていた)
        newRadiance = PrefilteredEnvTexture.SampleLevel(MaterialSampler, reflectDir, mipLevel).rgb;
        confidence = roughnessFade;
    }
    // 画面外に外れた、または最大距離まで判定がつかなかった場合は confidence = 0 のまま。
    // Lightingパスが適用したプローブ/グローバルIBLをそのまま残す(プローブが画面外を知っている)

    const float3 composited = baseColor + (newRadiance - envRadiance) * specularWeight * confidence;

    // 半透明サーフェスのピクセルではG-Bufferが「ガラスの奥にある不透明面」の値を持つため、
    // ここで引く鏡面IBLがSceneColor(ガラスで上書き済み)に含まれておらず負へ振れうる。
    // 半透明パスがSSRの対象外である以上この不一致は避けられないので、負の輝度だけは止めておく
    return float4(max(composited, float3(0.0f, 0.0f, 0.0f)), 1.0f);
}
