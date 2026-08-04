// レイトレーシング反射(RT反射)パス。SSR.hlslのレイトレーシング版で、役割はまったく同じ。
//
// 【SSRとの関係】SSRと同じく反射色を「加算」せず、Lightingパスが適用した鏡面IBLの
// 環境の放射輝度だけを差し替える(20章):
//   出力 = SceneColor + (RTが得た放射輝度 - Lightingが使った放射輝度) * SpecularIBLWeight(...) * 確信度
// 係数SpecularIBLWeightと環境の放射輝度SampleEnvironmentはReflectionProbe.hlsliで
// DeferredLighting.hlsl / SSR.hlslと共有しているため、「足した覚えのない値を引く」ことは起きない。
//
// 【SSRに対する利点】画面に映っていないものも反射に映る。SSRはレイが画面外へ出た時点で
// 打ち切って確信度0(=プローブ任せ)にするしかなかったが、RTはシーン全体のBLAS/TLASを
// 走査するため、カメラの背後にある建物も足元の地面も正しく反射できる。
//
// 【コンピュートシェーダーである理由】インラインレイトレーシング(RayQuery)自体は
// ピクセルシェーダーでも使えるが、このエンジンで高速化構造をバインドできるのは
// IRHICommandList::SetComputeAccelerationStructure(コンピュート用ルートシグネチャのt枠)だけ。
// SSRのようなフルスクリーン三角形+ピクセルシェーダーではなく、UAVへ書くディスパッチにしている。
//
// 【この段階の制約】docs/Architecture.html 25章にまとめてある。要点:
//   - ヒット面のテクスチャを読めない(bindlessが要る)。色はマテリアルの定数値のみ
//   - 1本の鏡面レイのみ。粗い面はSSRと同じくラフネスでフェードしてプローブ/IBLへ戻す
//   - すべての三角形を不透明として扱う(RAY_FLAG_FORCE_OPAQUE)
#include "NormalEncoding.hlsli"
#include "Samplers.hlsli"

// レイの始点を法線方向へ押し出す量(ワールド単位)。自己交差(アクネ)を防ぐ。
// ヒット距離に比例させる項も足して、遠方でも相対的な押し出し量を保つ
static const float kRayOriginBias = 0.01f;
static const float kRayOriginBiasSlope = 1e-4f;
// 影レイの最大距離(ワールド単位)。太陽は平行光なので本来は無限だが、
// シーン外まで飛ばしても当たるものは無いので上限を設ける
static const float kShadowRayMaxDistance = 1.0e4f;

static const float kPI = 3.14159265359f;

// 反射プローブの環境ソースと鏡面IBLの重み(DeferredLighting.hlsl / SSR.hlslと共有)。
// 拡散イラディアンスは専用マップ経路を使わないため、拡散側のレジスタは定義しない
// (ヒット面のアンビエントはプリフィルタ済み鏡面の最終ミップを直接引く。後述)
#define KURENAI_GLOBAL_PREFILTERED_REGISTER t8
#define KURENAI_PROBE_PREFILTERED_REGISTER t9
#define KURENAI_PROBE_BUFFER_REGISTER t10

cbuffer FrameConstants : register(b0)
{
    float4x4 ViewProj;
    float4x4 InvViewProj;
    // カスケードシャドウマップ用(このシェーダでは未使用。オフセット合わせのためだけに宣言する)
    float4x4 CascadeViewProj[4];
    float4 CameraPosition;
    // xyz=太陽の進行方向(光が飛んでいく向き)。太陽へ向かうベクトルは -LightDirection.xyz
    float4 LightDirection;
    // rgb=太陽の放射輝度(露出適用済み)。太陽が無効なシーンでは0が入る
    float4 LightColor;
    float4x4 View;
    // このシェーダでは未使用(オフセット合わせのためだけに宣言する)
    float4x4 Proj;
    float4 AmbientColor;
    // このシェーダでは未使用(オフセット合わせのためだけに宣言する)
    float4 CascadeSplits;
    // y: プリフィルタ済み鏡面マップの最大ミップレベル、z: IBL強度倍率、
    // w: スペキュラのマルチスキャッタリング・エネルギー補正のトグル
    float4 ShadowParams;
    // このシェーダでは未使用(オフセット合わせのためだけに宣言する)
    float4 ActiveLightCount;
    // 拡散イラディアンスの取得元切り替え。ReflectionProbe.hlsliが参照する
    float4 IBLParams;
    // 反射プローブ用。ReflectionProbe.hlsliのプローブ選択・ブレンドが読む
    float4 ProbeParams;
    // 反射プローブの距離キューブ用。w=焼いた時点の実効プリ露出から現在の実効プリ露出への
    // 換算倍率(19.14節)で、ReflectionProbe.hlsliのBlendReflectionProbesが読む。x〜zはこの
    // シェーダでは未使用(視差補正・遮蔽判定のフラグと距離キューブの解像度)
    float4 ProbeParams2;
    // 【以下はこのシェーダーでは使わないが宣言だけ必要】cbufferは宣言順レイアウトなので、
    // 末尾のOcclusionParamsを正しいオフセットで読むには途中のフィールドを飛ばせない。
    // C++側のFrameConstantsと並びを必ず一致させること
    float4x4 PrevViewProj;
    float4 TAAParams;
    float4 DDGIParams0;
    float4 DDGIParams1;
    float4 DDGIParams2;
    float4 DDGIParams3;
    float4 DDGIParams4;
    // bent normalによる遮蔽(34章)。y=スペキュラ遮蔽の方式を読む。
    // DeferredLighting.hlsl・SSR.hlslとまったく同じ読み方をすること(段差防止)
    float4 OcclusionParams;
    // これ以降(TimeParams / Sky* / Cloud* / PlanarReflectionPlane / Fog* / WaterBodyColor)は
    // このシェーダーでは一切読まないため宣言しない
};

cbuffer RTReflectionConstants : register(b1)
{
    // xy: 出力サイズ(ピクセル), z: 最大レイ距離(ワールド単位), w: ラフネスカットオフ
    float4 Params0;
    // x: ヒット面へ影レイを撃つか(1で撃つ)
    // y: メッシュレットのデバッグ表示(1で、反射に映る面を陰影の代わりにメッシュレット色で塗る)。
    //    ラスタ側の色分け(GBufferMeshlet.hlslのPSMainMeshletDebug)と同じ色になるので、
    //    両者が同じ塊分けを見ていることを目視で確かめられる
    // zw: 未使用
    float4 Params1;
};

// --- レイトレーシング資源 ---
RaytracingAccelerationStructure SceneTLAS : register(t0);

// --- G-Buffer / 中間バッファ ---
Texture2D SceneColorTexture : register(t1);
Texture2D NormalTexture : register(t2);
Texture2D MaterialTexture : register(t3);
Texture2D DepthTexture : register(t4);
Texture2D AlbedoTexture : register(t5);
// SSAO/SSILのAO/GIバッファ。a=遮蔽率。スペキュラオクルージョンに使う
Texture2D AOTexture : register(t6);
// split-sum近似の第2項、BRDF積分LUT
Texture2D BRDFLUTTexture : register(t7);
// bent normal(GBuffer.hlslがSV_TARGET5へ書いた接空間のbRaw)。t0〜t15が埋まっているためt16。
// これに合わせてDX12のkComputeSrvSlotCountを16→17へ上げてある(34章)
Texture2D BentNormalTexture : register(t16);

// プリフィルタ済み鏡面(t8)・プローブのキューブマップ配列(t9)・プローブの影響範囲バッファ(t10)の
// 宣言と、プローブの選択・視差補正・ブレンド・鏡面IBLの重みはReflectionProbe.hlsliが持つ
#include "ReflectionProbe.hlsli"

// --- シーンジオメトリの統合バッファ(Assets::RaytracingScene) ---
// 構造体の写し・索引の辿り方(FetchHitSurface)・遮蔽レイはRaytracingScene.hlsliが持つ。
// RTAO.hlslとまったく同じ辿り方でなければならないため共有している
#define KURENAI_RT_ATTRIBUTE_REGISTER t11
#define KURENAI_RT_INDEX_REGISTER t12
#define KURENAI_RT_MESHINFO_REGISTER t13
#define KURENAI_RT_INSTANCEINFO_REGISTER t14
#define KURENAI_RT_MATERIAL_REGISTER t15
// メッシュレット表。t8〜t10はReflectionProbe.hlsliが、t16はbent normalが使っているため、
// このシェーダーで空いているのはt17が最初。
// これに合わせてDX12のkComputeSrvSlotCountを17→18へ上げてある。
// KurenaiEngine3D.cppのRT反射パスのバインド(スロット17)と一致させること
#define KURENAI_RT_MESHLET_REGISTER t17
#include "RaytracingScene.hlsli"

RWTexture2D<float4> OutputTexture : register(u0);

float3 ReconstructWorldPos(float2 uv, float depth)
{
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float4 worldPos = mul(float4(ndc, depth, 1.0f), InvViewProj);
    return worldPos.xyz / worldPos.w;
}

// 指定位置から太陽へ影レイを撃ち、遮られていなければ1、遮られていれば0を返す
float TraceSunShadow(float3 position, float3 normal, float3 toSun)
{
    if (Params1.x <= 0.5f)
    {
        return 1.0f;
    }

    const bool occluded = TraceOcclusionRay(
        SceneTLAS, position + normal * kRayOriginBias, toSun, kRayOriginBias, kShadowRayMaxDistance);
    return occluded ? 0.0f : 1.0f;
}

// ヒット面の陰影を求める。1バウンス目の反射に映る色なので、太陽の直接光(影レイ付き)と
// 環境からのアンビエント、エミッシブだけを足す簡略版にしている。
// ヒット面での鏡面反射(2バウンス目)は撃たない
float3 ShadeHitSurface(float3 hitPosition, float3 hitNormal, RTMaterial material, float3 rayDirection)
{
    // 裏面に当たった場合は法線を反転させる(片面ポリゴンのシーンでは普通に起きる)
    const float3 N = (dot(hitNormal, rayDirection) > 0.0f) ? -hitNormal : hitNormal;

    const float3 baseColor = material.BaseColorFactor.rgb;
    // 金属は拡散反射を持たない。この段階では鏡面を撃たないため、金属面は
    // アンビエントの鏡面成分だけで表現される(下のprefilteredEnv)
    const float3 diffuseAlbedo = baseColor * (1.0f - material.MetallicFactor);

    // --- 太陽の直接光(Lambert拡散) ---
    // 1/PIはランバートBRDFの正規化。DirectLighting.hlslのEvaluateDirectBRDF(kd*albedo/PI)と
    // スケールを揃えるために必要で、これが抜けていると反射に映る日向の面だけがπ倍明るくなる
    float3 radiance = float3(0.0f, 0.0f, 0.0f);
    const float3 toSun = normalize(-LightDirection.xyz);
    const float NdotL = saturate(dot(N, toSun));
    if (NdotL > 0.0f)
    {
        const float shadow = TraceSunShadow(hitPosition, N, toSun);
        radiance += (diffuseAlbedo / kPI) * LightColor.rgb * NdotL * shadow;
    }

    // --- アンビエント ---
    // プリフィルタ済み鏡面の最終ミップ(roughness=1)を法線方向で引いたものがイラディアンスE(N)/πに
    // 等しい(14.10節)。ヒット面ではプローブの視差補正まではやらず、グローバルIBLだけを使う
    // (レイ1本ごとにプローブ選択を回すコストに見合わないため。25章の制約)
    const float3 irradiance = PrefilteredEnvTexture.SampleLevel(MaterialSampler, N, ShadowParams.y).rgb;
    radiance += diffuseAlbedo * irradiance * ShadowParams.z;

    // 粗くない金属面のアンビエント鏡面。拡散を持たない金属が真っ黒にならないようにする最低限の項
    if (material.MetallicFactor > 0.0f)
    {
        const float3 mirrorDir = reflect(rayDirection, N);
        const float mipLevel = material.RoughnessFactor * ShadowParams.y;
        const float3 prefilteredEnv = PrefilteredEnvTexture.SampleLevel(MaterialSampler, mirrorDir, mipLevel).rgb;
        radiance += baseColor * prefilteredEnv * material.MetallicFactor * ShadowParams.z;
    }

    return radiance + material.EmissiveFactor;
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

    // ピクセル中心のUV。Load(整数座標)で読めるものはLoadで読み、
    // フィルタが要るキューブマップ/LUTだけSampleLevelを使う
    const float2 uv = (float2(pixel) + 0.5f) / float2(outputSize);

    const float3 baseSceneColor = SceneColorTexture[pixel].rgb;

    const float depth = DepthTexture[pixel].r;
    if (depth <= 0.0f)
    {
        // 背景(スカイ)には反射元のサーフェスがない
        OutputTexture[pixel] = float4(baseSceneColor, 1.0f);
        return;
    }

    const float3 materialSample = MaterialTexture[pixel].rgb;
    const float metallic = materialSample.r;
    const float roughness = materialSample.g;
    // b = マテリアルの遮蔽マップ(GBuffer.hlslでstrength適用済み。遮蔽マップを持たない
    // マテリアルは1.0)。下のSpecularIBLWeight呼び出しへmaterialAOとして渡す
    const float materialAO = materialSample.b;

    const float maxRayDistance = Params0.z;
    const float roughnessCutoff = Params0.w;

    // 1本の鏡面レイしか撃たないため、粗い面ではSSRと同じ理由(円錐状のぼかしを持たない)で
    // 結果を信用しない。粗い面はLightingパスが適用したプローブ/グローバルIBLに任せる
    const float roughnessFade = 1.0f - smoothstep(0.0f, roughnessCutoff, roughness);
    if (roughnessFade <= 0.0f)
    {
        OutputTexture[pixel] = float4(baseSceneColor, 1.0f);
        return;
    }

    const float3 albedo = AlbedoTexture[pixel].rgb;
    const float3 worldPos = ReconstructWorldPos(uv, depth);
    const float3 N = OctDecode(NormalTexture[pixel].xy);
    const float3 V = normalize(CameraPosition.xyz - worldPos);
    const float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
    const float NdotV = saturate(dot(N, V));
    const float3 reflectDir = normalize(reflect(-V, N));

    // --- Lightingパスが適用した鏡面IBLを、そのときとまったく同じ式で再現する ---
    // 遮蔽の合成(materialAOとssao、bent normalの扱い)はDeferredLighting.hlslのPSMain・
    // SSR.hlslとまったく同じでなければならない。ズレるとRT反射が適用される領域と
    // されない領域の境界に段差が出る(22章の遮蔽マップ、28.2節の差し替え構造)
    // LUTの第3成分(Eavg)はKulla-Conty方式だけが使うため.rgbで引く(DeferredLighting.hlslの
    // EvaluateIBLと同じ。SpecularIBLWeightはfloat3のbrdfを受け取る)
    const float3 brdf = BRDFLUTTexture.SampleLevel(ColorSampler, float2(NdotV, roughness), 0.0f).rgb;
    // bent normalはDeferredLighting.hlsl・SSR.hlslとまったく同じ引き方をすること。
    // このシェーダはコンピュートのためSampleではなくSampleLevelでミップ0を明示する
    const BentOcclusion bent = DecodeBentOcclusion(BentNormalTexture.SampleLevel(DataSampler, uv, 0.0f), N);
    // 0 = Frostbite近似 / 1 = 球冠交差 / 2 = 球面ガウス(34.11節)
    const int soMode = (int)(OcclusionParams.y + 0.5f);
    const float3 specularWeight =
        SpecularIBLWeight(F0, NdotV, roughness, soMode, bent, N, reflectDir, materialAO,
                          AOTexture[pixel].a, brdf, ShadowParams.w, ShadowParams.z);

    const float mipLevel = roughness * ShadowParams.y;
    float3 unusedIrradiance;
    float3 envRadiance;
    SampleEnvironment(worldPos, N, reflectDir, mipLevel, unusedIrradiance, envRadiance);

    // --- 反射レイを撃つ ---
    // 始点はカメラから遠いほど大きく押し出す。深度バッファ由来のワールド座標は遠方ほど
    // 誤差が大きく、固定バイアスのままだと遠景で自己交差(アクネ)が出るため
    const float originBias = kRayOriginBias + length(worldPos - CameraPosition.xyz) * kRayOriginBiasSlope;

    RayDesc ray;
    ray.Origin = worldPos + N * originBias;
    ray.Direction = reflectDir;
    ray.TMin = originBias;
    ray.TMax = maxRayDistance;

    // アルファテスト付きのジオメトリ(木の葉など)も不透明として扱う。RayQueryで正しく抜くには
    // ヒット候補ごとにベースカラーテクスチャをサンプルする必要があり、bindlessが要るため
    // (25章の制約)。葉が板ポリのまま映るほうが、葉をすり抜けて背景が映るより破綻が小さい
    RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
    query.TraceRayInline(SceneTLAS, RAY_FLAG_NONE, 0xFFu, ray);
    query.Proceed();

    float3 newRadiance;
    if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        float3 hitNormal;
        // 初期化しているのはdxcの警告対策。out引数として必ず書き込まれるが、
        // 構造体のout引数はdxcの未初期化検査を通らず警告になる
        RTMaterial hitMaterial = (RTMaterial)0;
        FetchHitSurface(
            query.CommittedInstanceID(),
            query.CommittedGeometryIndex(),
            query.CommittedPrimitiveIndex(),
            query.CommittedTriangleBarycentrics(),
            hitNormal,
            hitMaterial);

        const float3 hitPosition = ray.Origin + ray.Direction * query.CommittedRayT();
        if (Params1.y > 0.5f)
        {
            // メッシュレットのデバッグ表示。陰影を計算せず、当たった三角形が属する
            // メッシュレットの色をそのまま反射の色にする。
            // ラスタ側の色分けと同じ塊が同じ色で映れば、描画とレイトレーシングが
            // 同一のジオメトリ(同じ.kgeom、同じ三角形の並び)を見ていることの確認になる
            const RTInstanceInfo instanceInfo = RTInstanceInfos[query.CommittedInstanceID()];
            const RTMeshInfo meshInfo = RTMeshInfos[instanceInfo.MeshInfoOffset + query.CommittedGeometryIndex()];
            newRadiance = MeshletDebugColor(RTFindMeshlet(meshInfo, query.CommittedPrimitiveIndex()));
        }
        else
        {
            newRadiance = ShadeHitSurface(hitPosition, hitNormal, hitMaterial, ray.Direction);
        }
    }
    else
    {
        // 何にも当たらなかった=空へ抜けた。生のスカイボックスではなくプリフィルタ済み鏡面を
        // ラフネス→ミップで引く(SSRのskyHitと同じ扱い)
        newRadiance = PrefilteredEnvTexture.SampleLevel(MaterialSampler, reflectDir, mipLevel).rgb;
    }

    // --- 環境の放射輝度を差し替える ---
    // SSRと違い「画面外へ外れたので確信度0」という状態が存在しない。レイは必ずヒットするか
    // 空へ抜けるかのどちらかに決着するため、確信度はラフネスによるフェードのみ
    const float3 composited = baseSceneColor + (newRadiance - envRadiance) * specularWeight * roughnessFade;

    // 半透明サーフェスのピクセルではG-Bufferが「ガラスの奥にある不透明面」の値を持つため、
    // ここで引く鏡面IBLがSceneColor(ガラスで上書き済み)に含まれておらず負へ振れうる
    // (SSR.hlslと同じ理由)。負の輝度だけは止めておく
    OutputTexture[pixel] = float4(max(composited, float3(0.0f, 0.0f, 0.0f)), 1.0f);
}
