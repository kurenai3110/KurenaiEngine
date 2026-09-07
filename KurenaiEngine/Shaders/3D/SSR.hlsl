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
// 適用したプローブ/グローバルIBLをそのまま残す。プローブが画面外の情報を持っているため、
// 「何もしない=プローブに任せる」が正しい答えになる。
//
// このエンジンにはPSOのブレンドステートが無いため、既存のSSAO/SSILと同じ
// フルスクリーン三角形+ピクセルシェーダーのパターンで実装し、合成もこのシェーダー内で直接行う。
#include "NormalEncoding.hlsli"
#include "Samplers.hlsli"
// 水面の解析空フォールバック用。DeferredLighting.hlslが背景の解析評価に使っている
// のと同じ空モデル定義を共有する。cbufferに依存しないヘッダーで、PIも定義しないため
// (Sky.hlsli冒頭のコメント参照)、このファイルがPIを定義していない現状と衝突しない
// ボリュメトリック積雲が引く3Dノイズのレジスタ。Sky.hlsliはcbufferにもレジスタにも
// 依存しない方針なので、DDGI.hlsliと同じくインクルードする側がマクロで指定する。
// 定義しないシェーダー(SkyGenerate/AerialPerspective/PlanarReflection)ではボリュームの
// 経路がコンパイルされず、従来の平面の経路だけが残る
// SkyView LUT。日中の空はこのLUTを引く。**定義しないと日中の空が黒くなる**ので、
// SkyColorUpperUnitを呼ぶシェーダーは全員定義すること(Sky.hlsliのSkyViewセクション参照)
#define KURENAI_SKYVIEW_REGISTER t15
#define KURENAI_CLOUD_SHAPE_REGISTER t13
#define KURENAI_CLOUD_DETAIL_REGISTER t14
// 焼いたウェザーマップ(H3)。DeferredLighting.hlsl 側のコメント参照
#define KURENAI_CLOUD_WEATHER_REGISTER t17
#include "Sky.hlsli"

static const int kSSRStepCount = 32;
static const int kSSRBinaryStepCount = 6;
static const float kSSREdgeFadeDistance = 0.1f;
// 水面のマテリアルID(G-BufferのMaterial.a)。GBufferCommon.hlsliのkMaterialIDWaterと
// **同じ値でなければならない**。GBufferCommon.hlsliはcbuffer/テクスチャの宣言を含み
// このパスへはインクルードできないため、値をここに複製している
static const float kSSRWaterMaterialID = 1.0f;

// 反射プローブの環境ソースと鏡面IBLの重み(DeferredLighting.hlslと共有)。
// 拡散イラディアンスは使わないため、拡散側のレジスタは定義しない
#define KURENAI_GLOBAL_PREFILTERED_REGISTER t7
#define KURENAI_PROBE_PREFILTERED_REGISTER t8
#define KURENAI_PROBE_BUFFER_REGISTER t9
// 距離キューブ(19.12節)。DeferredLighting.hlslと同じ条件でコンパイルしないと、
// SSRが「Lightingが使ったのとは違う放射輝度」を引き算することになるため必ず定義する
#define KURENAI_PROBE_DISTANCE_REGISTER t10

#include "ShaderInterop/FrameConstants.hlsli"

cbuffer SSRConstants : register(b1)
{
    // w: 水面の解析空フォールバックを使うか(1=使う)。C++側で
    // m_WaterAnalyticSkyReflection && usingProceduralSky の両方が立っているときだけ1になる
    // (手続き空が無効=.ksceneがDDSスカイボックスを明示するシーンでは、このトグルの値に
    // 関わらず必ず0にする。DDSは任意の絵でPerezモデルとは無関係なため解析評価できない)
    float4 Params0; // x: 最大レイ距離(ワールド単位), y: ヒット判定の厚み, z: ラフネスカットオフ, w: 水面の解析空フォールバック
    // 平面反射(末尾に追加)。x: このフレームで平面反射パスを実行したか(1=有効。
    // C++側でm_PlanarReflectionEnabled && 水面インスタンスが存在するときのみ1になる)、
    // y: 波の法線による画面UVのずらし量(m_PlanarReflectionDistortion)、zw: 未使用
    float4 Params1;
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
// t11は平面反射が使うためt16に置く(34章)
Texture2D BentNormalTexture : register(t16);

// プリフィルタ済み鏡面(t7)・プローブのキューブマップ配列(t8)・プローブの影響範囲バッファ(t9)・
// プローブの距離キューブ(t10)の宣言と、プローブの選択・視差補正・ブレンド・鏡面IBLの重みは
// ReflectionProbe.hlsliが持つ
#include "ReflectionProbe.hlsli"

// 平面反射。KurenaiEngine3D::Renderが鏡映カメラで描いたPlanarReflection.hlslの結果
// (m_RenderTargets.PlanarReflectionColor)。t0〜t10は上ですべて埋まっているためt11を使う
Texture2D PlanarReflectionTexture : register(t11);
// SkyIntegrate.hlslが書いた空パラメータ。ティント4本と正規化済みの天頂輝度が入る。
// t0〜t11が既に使用済みのためt12を使う
StructuredBuffer<GPUSkyParameters> SkyParametersBuffer : register(t12);

#include "ShaderInterop/FullscreenTriangle.hlsli"

#include "ShaderInterop/Common.hlsli"

#define KURENAI_SKY_WITH_STARS
#include "ShaderInterop/SkyFrameParameters.hlsli"

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

// 平面反射を解析空フォールバックより優先して使う。呼び出し側(useWaterAnalyticSky
// が立っている水面画素)からのみ呼ばれる想定。
//
// 平面反射はSSRのレイマーチとは完全に別経路――鏡映カメラで景色を描き直したPlanarReflection.hlsl
// の結果(m_RenderTargets.PlanarReflectionColor)を、反射ベクトルを再投影せず同じ画面UV(input.UV)でそのまま
// サンプルするだけでよい(平面鏡の反射は鏡映カメラで撮り直すことと数学的に等価なため。
// 詳細はPlanarReflection.hlsl冒頭のコメント参照)。波の法線でその画面UVを少しだけずらすことで、
// 波打つ水面らしい歪みを付ける。
//
// 【画面端の扱い】reflUVが画面端に近いほど平面反射の信頼度を落とすが、confidence
// (roughnessFade)ではなくここで解析空とのlerpとして表現する。confidenceを落とすと
// Lightingパスが適用したプローブ/グローバルIBLへ戻ってしまい、
// 「水面にIBLしか映らない」状態が画面端で出るため
//
// 【ジオメトリが無い方向の扱い】平面反射パスは不透明メッシュしか描かないため、
// 反射先に何も無い方向(=島の鏡像以外のほとんどの向き)のテクセルはクリア値のまま残る。
// **ここを区別せずlerpしてはいけない** ―― 水面のほぼ全面がクリア色(黒)で塗り潰され、
// 20.6節の解析空が見えなくなる(SSRを有効にすると水面が一様な暗色になる)。
// レンダーターゲットはアルファ0でクリアされ、PlanarReflection.hlslのPSMainは
// float4(color, 1.0f)を返すので、アルファがそのまま「ジオメトリが描かれたか」の
// カバレッジになる(ブレンドはOpaqueなのでアルファは加工されずに書き込まれる)。
// これを使って、描かれていない方向は解析空へ戻す
float3 ApplyPlanarReflection(float3 analyticSky, float2 screenUV, float3 N)
{
    // Params1.x <= 0.5fは「このフレームで平面反射パスを実行していない」ケース
    // (m_PlanarReflectionEnabled=falseか、シーンに水面インスタンスが無い)。
    // この分岐に入ると解析空のみの経路を通る
    if (Params1.x <= 0.5f)
    {
        return analyticSky;
    }

    const float2 reflUV = screenUV + N.xz * Params1.y;
    if (reflUV.x < 0.0f || reflUV.x > 1.0f || reflUV.y < 0.0f || reflUV.y > 1.0f)
    {
        // 画面外に出た場合は平面反射を使わず解析空のみにする
        return analyticSky;
    }

    const float4 planarSample = PlanarReflectionTexture.Sample(ColorSampler, reflUV);
    const float2 edgeDist = min(reflUV, float2(1.0f, 1.0f) - reflUV);
    const float edgeFade = saturate(min(edgeDist.x, edgeDist.y) / kSSREdgeFadeDistance);

    // 事前乗算済みアルファのover合成。
    // 【なぜlerp(analyticSky, planarSample.rgb, edgeFade * planarSample.a)ではないのか】
    // クリア値が(0,0,0,0)でジオメトリが(color, 1)を書くため、バイリニア補間された
    // テクセルのrgbは既にアルファが掛かった値(a * color)になっている。素のlerpだと
    // ジオメトリの輪郭でアルファがもう一度掛かって二重に暗くなる。
    // この形なら a=0 で解析空そのもの、a=1 かつ edgeFade=1 で平面反射そのものになり、
    // 中間でも輪郭に暗い縁が出ない
    return analyticSky * (1.0f - edgeFade * planarSample.a) + planarSample.rgb * edgeFade;
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
    // .aに水面のマテリアルID(kMaterialIDWater)が入っているため、rgbとaを1回のサンプルで
    // まとめて読む
    float4 materialSample = MaterialTexture.Sample(DataSampler, input.UV);
    float3 material = materialSample.rgb;
    float metallic = material.r;
    float roughness = material.g;
    float materialAO = material.b; // マテリアルの遮蔽マップ(GBuffer.hlslでstrength適用済み)
    // DataSamplerはPoint+Clamp(Samplers.hlsli参照)なので、水面と通常マテリアルの境界でIDが
    // バイリニア補間により中間値化することは無い。それでも==ではなく閾値で比較しているのは、
    // 浮動小数点の等値比較そのものを避ける一般的な安全策のため(実際に出現する値は0.0か1.0のみ)。
    // kSSRWaterMaterialIDの半分をしきい値にしているのは、0.5fと決め打つよりIDの値と連動させておき、
    // 将来kMaterialIDWater/kSSRWaterMaterialIDが1.0f以外に変わってもここを直し忘れないようにするため
    const bool isWater = materialSample.a > kSSRWaterMaterialID * 0.5f;

    const float maxDistance = Params0.x;
    const float thickness = Params0.y;
    const float roughnessCutoff = Params0.z;
    // 水面の解析空フォールバックが有効か。水面タグ(isWater)とC++側のフラグの両方が
    // 立っているときだけtrueになる。非水面画素では常にfalseになるため、以降の分岐は
    // このフィールドが存在しなかったときとまったく同じコードパスを通る
    const bool useWaterAnalyticSky = isWater && (Params0.w > 0.5f);

    // スクリーンスペースのレイマーチはヒット色を1点サンプルするだけで、粗い面に必要な
    // 円錐状のぼかしを持たない。そのため粗い面ほどSSRの結果を信用しない
    float roughnessFade = 1.0f - smoothstep(0.0f, roughnessCutoff, roughness);
    if (roughnessFade <= 0.0f)
    {
        // SSRを信用しない=Lightingパスが適用したプローブ/グローバルIBLをそのまま残す。
        // このシーンの水面メッシュはroughnessFactor=0.03(Tools/generate_water_plane.pyの
        // WATER_ROUGHNESS)で焼かれており、Water.hlslのPSMainが下限0.045へクランプするため
        // G-Bufferには0.045が入る。既定のroughnessCutoff(0.6)より十分低いのでここを通過するが、
        // ユーザーがroughnessCutoffをそれ以下まで下げると水面もここで弾かれ、
        // 下の解析空フォールバックも一緒に無効になる。
        // 「粗すぎる面ではSSRを信用しない」という設計は水面かどうかで変えていない
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
    // 0 = Frostbite近似 / 1 = 球冠交差 / 2 = 球面ガウス(34.11節)。
    // DeferredLighting.hlslとまったく同じ読み方をすること(段差防止)
    const int soMode = (int)(OcclusionParams.y + 0.5f);
    const float3 brdf = BRDFLUTTexture.Sample(ColorSampler, float2(NdotV, roughness)).rgb;
    const float3 specularWeight =
        SpecularIBLWeight(F0, NdotV, roughness, soMode, bent, N, reflectDir, materialAO, ssao, brdf,
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
        if (useWaterAnalyticSky)
        {
            // 水面: プリフィルタ済み鏡面(128pxベースのキューブマップをラフネス由来の
            // ミップで引く)の代わりに、Perez分布を画面解像度でそのまま評価した解析空を使う。
            // 水面はroughnessが低く(このroughnessCutoffのゲートを通過している時点でそう)、
            // 低ミップの128pxを直接引くと空に映る太陽・地平線の勾配が色斑としてにじむため、
            // 解析評価のほうが実際の見え方に近い。
            // reflectDirが水平線より下を向く場合(強い波で反射ベクトルが下向きになったとき)は
            // 地平線下の接地色へのフェード(Sky.hlsli kGroundFadeStartY/EndY)でそのまま処理でき、
            // ここで別扱いする必要はない。
            // 平面反射(P6)はこの解析空よりさらに優先する(ApplyPlanarReflection参照)。
            //
            // 【P17: レイの起点を水面にする】SkyColor(dir, params)は起点を視点(カメラ)と
            // みなすため、反射レイまでカメラから出ているものとして雲を評価していた。これが
            // **水面に雲が映らなかった直接の原因**で、水面すれすれの反射レイは
            // 「カメラの真上にある雲層」の地平線際——撤去前のフェードで消される領域——へ
            // 丸ごと入っていた。起点を水面のワールド座標にすることで、水面から空を見上げる
            // 本来のレイとして雲層との交差が解ける
            newRadiance = ApplyPlanarReflection(
                SkyColorWithRay(worldPos, reflectDir, kCloudBackgroundRayDistance, MakeSkyParameters(input.Position.xy)),
                input.UV, N);
        }
        else
        {
            // 生のスカイボックスではなくプリフィルタ済み鏡面をラフネス→ミップで引く
            // (生のスカイボックスにはミップ選択が無く、粗い面でも鮮鋭な鏡像が返ってしまう)
            newRadiance = PrefilteredEnvTexture.SampleLevel(MaterialSampler, reflectDir, mipLevel).rgb;
        }
        confidence = roughnessFade;
    }
    else if (useWaterAnalyticSky)
    {
        // 画面外に外れた、または最大距離まで判定がつかなかった場合。非水面はこの分岐が無く
        // confidence = 0 のまま(Lightingパスが適用したプローブ/グローバルIBLをそのまま残す。
        // プローブが画面外を知っているため、20.3節のとおりこれが正しい答え)。
        //
        // 水面だけここで解析空を使う理由: このシーンの水面は4000m四方あり、かつ
        // SSRMaxDistance(既定5.0m)に対して反射レイはほぼ確実に数ステップで画面外へ抜けるか
        // 最大距離まで判定がつかない。つまり水面ではこの分岐が「レアケース」ではなく
        // ほぼ常時通る経路になり、confidence=0のままだと水面の映り込みが実質いつも死んで
        // プリフィルタ済み鏡面IBL(低解像度でにじむ)しか見えなくなる。
        // 水平な水面を上から見たとき反射ベクトルは必ず上向き(空側)を向くため、
        // 空で埋めるのは常に妥当な近似になる(上のskyHit分岐と同じ理由)。
        // 屋根の下の水たまりのような反例は、.kscene側で[Model]Water=trueと明示的にタグ付けした
        // 面にしかこの経路が適用されない(オプトイン)ため、影響範囲がそこに閉じている。
        // 平面反射(P6)はこの解析空よりさらに優先する(ApplyPlanarReflection参照)。
        // 【P17】起点を水面にする理由は上のskyHit分岐と同じ。**水面の大半はこちらの分岐を
        // 通る**(SSRMaxDistance 5.0mに対し4,000m四方の水面では、ほぼ常に画面外へ抜けるか
        // 最大距離まで判定がつかない)ため、水面へ雲が映るかどうかは実質ここで決まる
        newRadiance = ApplyPlanarReflection(
            SkyColorWithRay(worldPos, reflectDir, kCloudBackgroundRayDistance, MakeSkyParameters(input.Position.xy)),
            input.UV, N);
        confidence = roughnessFade;
    }
    // 非水面が画面外に外れた、または最大距離まで判定がつかなかった場合は confidence = 0 のまま。
    // Lightingパスが適用したプローブ/グローバルIBLをそのまま残す(プローブが画面外を知っている)

    const float3 composited = baseColor + (newRadiance - envRadiance) * specularWeight * confidence;

    // 半透明サーフェスのピクセルではG-Bufferが「ガラスの奥にある不透明面」の値を持つため、
    // ここで引く鏡面IBLがSceneColor(ガラスで上書き済み)に含まれておらず負へ振れうる。
    // 半透明パスがSSRの対象外である以上この不一致は避けられないので、負の輝度だけは止めておく
    return float4(max(composited, float3(0.0f, 0.0f, 0.0f)), 1.0f);
}
