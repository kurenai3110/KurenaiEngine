// DDGIのプローブへ入れる放射輝度・距離を、ラスタライズではなくDXR(インラインRayQuery)で集める。
//
// 【何を置き換えるのか】従来はプローブ1個につきシーンを6回描き(ProbeCapture.hlsl)、
// その結果をキューブへ写していた(IBLConvolve.hlsl の CSCopyCaptureToCubeFace)。
// このシェーダーは**同じ形のスクラッチキューブ2本を直接埋める**。
// 出力の規約を一切変えないので、更新CS(DDGIProbeUpdate.hlsl)・アトラス・境界複製・
// 更新モードは1行も変わらない。A/B比較の差分の原因が「レイの取得」だけに限定される。
//
// 【レイの向き】CubeFace.hlsli の CubeFaceDirection で、従来の16x16x6のキューブ面と
// **同じ方向**を撃つ。ここを変えると比較が「経路の差」でなくなる。
//
// 【ラスタ経路との既知の差】どれもDXR側の制約で、絵が変わる方向まで含めて分かっている:
//   - 法線マップが乗らない。RTVertexAttributeに接線が無く、FetchHitSurfaceが
//     法線マップを適用できないため(25章の制約)
//   - ベイク済みAO・bent normalが引けない。ヒット面にライトマップUVが無いため、
//     遮蔽は「無し」(materialAO = 1、bent normal無効)として扱う
//   - アルファテストが効かない。全ヒットを不透明として扱う(RaytracingScene.hlsliの制約)
//   - 逆に、**太陽の影はカスケードシャドウマップではなく影レイで求める**。
//     ProbeCapture.hlsl冒頭が書いている「カメラから遠いプローブはCSMの範囲外で
//     影が落ちない」という制約はこの経路には無い(影が増える方向へ変わる)
//
// 【裏面ヒット】裏面カリングはしない。壁の内部に落ちたプローブを検出できるようにするため
// (プローブ分類で使う)。現状は放射輝度0・距離は正のヒット距離として書く。
// ラスタ経路は裏面カリングの結果「何も描かれない→深度0→空」となり明るく化けるので、
// ここは意図的に違う振る舞いであり、光漏れが暗くなる方向へ変わる。
//
// 【露出の規約】LightColor も Lights[] も SampleDDGIIrradiance の戻り値も既に露出適用済み。
// このシェーダーでは何も割らない。割り戻すのは更新CSの Params2.w だけ、という規約を守ること。
#include "NormalEncoding.hlsli"
#include "SpecularEnergy.hlsli"
#include "Samplers.hlsli"
#include "CubeFace.hlsli"

static const float PI = 3.14159265359f;

// ProbeCapture.hlsl と同じ並びで宣言する。cbufferのレイアウトは宣言順で決まり、
// 途中のフィールドを飛ばせないため、読まないフィールドもオフセット合わせのために並べる
// (C++側 KurenaiEngine3D.cpp の FrameConstants と一致させること)
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
    // x=t7のライトリストの有効数
    float4 ActiveLightCount;
    float4 IBLParams;
    float4 ProbeParams;
    float4 ProbeParams2;
    float4x4 PrevViewProj;
    float4 TAAParams;
    // DDGI(22章)。多重バウンスのために前フレームのイラディアンスを引くのに使う
    float4 DDGIParams0;
    float4 DDGIParams1;
    float4 DDGIParams2;
    float4 DDGIParams3;
    // x=このフレームの実効プリ露出(アトラスは露出非依存で持つため読み出し時に掛け戻す)
    float4 DDGIParams4;
    // bent normalによる遮蔽(34章)
    float4 OcclusionParams;
    // これ以降はこのシェーダーでは読まないため宣言しない
};

// C++側 KurenaiEngine3D.cpp の DDGITraceConstants と一致させること
cbuffer DDGITraceConstants : register(b1)
{
    // xyz = いま焼いているプローブのワールド座標、w = 処理対象の面(D3Dのキューブ標準順)
    float4 TraceParams0;
    // x = キャプチャキューブの1面の解像度、y = エミッシブ強度(ラスタ経路の
    //     ObjectConstants.EmissiveFactorに掛かっているのと同じ倍率)、
    // z = 太陽の影レイを撃つか(0/1。対照実験用のつまみ)、w = 未使用
    float4 TraceParams1;
};

RaytracingAccelerationStructure SceneTLAS : register(t0);

// ヒットした三角形からマテリアルまでを引くための共有ヘッダー。
// レジスタの並びはC++側のバインド順と一致させること
#define KURENAI_RT_ATTRIBUTE_REGISTER t1
#define KURENAI_RT_INDEX_REGISTER t2
#define KURENAI_RT_MESHINFO_REGISTER t3
#define KURENAI_RT_INSTANCEINFO_REGISTER t4
#define KURENAI_RT_MATERIAL_REGISTER t5
#define KURENAI_RT_MESHLET_REGISTER t6
#include "RaytracingScene.hlsli"

// プローブへ焼く1点ぶんのシェーディング。ラスタ経路(ProbeCapture.hlsl)と同じ定義を使う
#define KURENAI_PROBE_LIGHT_REGISTER t7
#define KURENAI_PROBE_IRRADIANCE_REGISTER t8
#define KURENAI_PROBE_PREFILTERED_REGISTER t9
#define KURENAI_PROBE_BRDFLUT_REGISTER t10
#define KURENAI_DDGI_IRRADIANCE_REGISTER t12
#define KURENAI_DDGI_DISTANCE_REGISTER t13
#include "ProbeShading.hlsli"

// 何にも当たらなかった方向を埋める空。ラスタ経路が
// IBLConvolve.hlsl の CSCopyCaptureToCubeFace で引いているのと同じテクスチャ・同じ引き方にする
TextureCube SourceSkybox : register(t11);

// 出力は面ごとのUAV(要素数1のTexture2DArray)。IBLConvolve.hlsl の
// CSCopyCaptureToCubeFace の出力と同じ形・同じ規約でなければならない
RWTexture2DArray<float4> ProbeRadianceOut : register(u0);
RWTexture2DArray<float> ProbeDistanceOut : register(u1);

// 空(ジオメトリ無し)を表す距離。**IBLConvolve.hlsl の kProbeSkyDistance と同じ値**にすること。
// 更新CSが MaxRayDistance でクランプする前提なので、ここを変えるとチェビシェフ判定が
// 「どこも遠い」に倒れる
static const float kProbeSkyDistance = 1.0e6f;

// プローブから撃つレイの最大距離。これを超える先は空として扱う。
// シーンの広がり(モン・サン=ミシェルで数百m)に対して十分大きく取ってある
static const float kProbeRayMaxDistance = 1.0e4f;

// 影レイの原点をずらす量。自己交差(アクネ)を避けるための最小限
static const float kRayOriginBias = 0.01f;

// 太陽の影レイ。遮蔽の有無だけが分かればよいので最初のヒットで打ち切る
float TraceProbeSunShadow(float3 position, float3 normal, float3 toSun)
{
    if (TraceParams1.z <= 0.5f)
    {
        return 1.0f;
    }

    const bool occluded = TraceOcclusionRay(
        SceneTLAS, position + normal * kRayOriginBias, toSun, kRayOriginBias, kProbeRayMaxDistance);
    return occluded ? 0.0f : 1.0f;
}

// ヒット面の放射輝度。ProbeCapture.hlsl の PSMain と同じ構成
// (太陽 + ライトリスト + 環境光 + エミッシブ)で、式そのものは ProbeShading.hlsli を共有する
float3 ShadeProbeRayHit(float3 hitPosition, float3 N, RTMaterial material, float3 rayDirection)
{
    const float3 albedo = material.BaseColorFactor.rgb;
    const float metallic = saturate(material.MetallicFactor);
    // RoughnessFactorが負の場合はソースデータにラフネス係数が無かったことを表す
    // (Assets::kInvalidMaterialFactor)。ProbeCapture.hlslと同じく係数1.0として扱う。
    // マテリアルにMRテクスチャがあればFetchHitSurfaceが既に乗算済み
    const float roughnessFactor = (material.RoughnessFactor < 0.0f) ? 1.0f : material.RoughnessFactor;
    const float roughness = clamp(roughnessFactor, 0.045f, 1.0f);

    // 視線ベクトルはレイの逆向き。ラスタ版が「プローブ位置 - 描画点」で求めているのと同じ向き
    const float3 V = -rayDirection;
    const float NdotV = saturate(dot(N, V)) + 1e-5f;

    const float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
    const float3 brdf = BRDFLUTTexture.SampleLevel(ColorSampler, float2(NdotV, roughness), 0.0f).rgb;
    const SpecularEnergyContext energy = MakeSpecularEnergyContext(F0, brdf, roughness, ShadowParams.w);

    float3 color = float3(0.0f, 0.0f, 0.0f);

    // --- 太陽(影レイ付き) ---
    const float3 sunL = normalize(-LightDirection.xyz);
    const float sunNdotL = saturate(dot(N, sunL));
    if (sunNdotL > 0.0f)
    {
        const float shadow = TraceProbeSunShadow(hitPosition, N, sunL);
        color += EvaluateDirectBRDF(N, V, sunL, NdotV, albedo, metallic, roughness, energy) * LightColor.rgb * shadow;
    }

    // --- t7のライトリスト(ラスタ版と同じく影は落とさない) ---
    const uint lightCount = (uint)ActiveLightCount.x;
    [loop]
    for (uint i = 0; i < lightCount; ++i)
    {
        color += EvaluateLight(Lights[i], hitPosition, N, V, NdotV, albedo, metallic, roughness, energy);
    }

    // --- 環境光(グローバルIBL + 前バウンスのDDGI。IBL無効時は定数色へフォールバック) ---
    // ヒット面にはライトマップUVが無いので、ベイク済みAOもbent normalも引けない。
    // .a = 0 を渡すと DecodeBentOcclusion が「bent normal無し」の中立値(axis=N, aoB=1, aoN=1)を返す
    const BentOcclusion bent = DecodeBentOcclusion(float4(N, 0.0f), N);
    const float materialAO = 1.0f;
    color += EvaluateProbeEnvironment(
        N, V, hitPosition, albedo, metallic, roughness, NdotV, materialAO, bent, brdf, energy);

    // エミッシブはラスタ経路と同じ倍率を掛ける
    // (ラスタ側は MakeObjectConstants が EmissiveFactor へ強度を畳み込んでいる)
    color += material.EmissiveFactor * TraceParams1.y;

    return color;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const uint captureSize = (uint)TraceParams1.x;
    if (dispatchThreadID.x >= captureSize || dispatchThreadID.y >= captureSize)
    {
        return;
    }

    const uint face = (uint)TraceParams0.w;
    const float3 probePosition = TraceParams0.xyz;

    // ラスタ経路のキューブ面と同じ方向。テクセル中心で取るのも
    // CSCopyCaptureToCubeFace の空埋めと同じ
    const float2 uv = (float2(dispatchThreadID.xy) + 0.5f) / float2(captureSize, captureSize);
    const float3 rayDirection = CubeFaceDirection(face, uv);

    RayDesc ray;
    ray.Origin = probePosition;
    ray.Direction = rayDirection;
    ray.TMin = 0.0f;
    ray.TMax = kProbeRayMaxDistance;

    // アルファテスト付きのジオメトリも不透明として扱う(RTAO/RT反射と同じ扱い。25章の制約)。
    // 裏面はカリングしない ―― 壁の内部に落ちたプローブを見分けられなくなるため
    RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
    query.TraceRayInline(SceneTLAS, RAY_FLAG_NONE, 0xFFu, ray);
    query.Proceed();

    float3 radiance;
    float distance;

    if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        float3 hitNormal;
        // 初期化しているのはdxcの警告対策(out引数の構造体は未初期化検査を通らない)
        RTMaterial hitMaterial = (RTMaterial)0;
        FetchHitSurface(
            query.CommittedInstanceID(),
            query.CommittedGeometryIndex(),
            query.CommittedPrimitiveIndex(),
            query.CommittedTriangleBarycentrics(),
            hitNormal,
            hitMaterial);

        const float hitDistance = query.CommittedRayT();
        const float3 hitPosition = ray.Origin + ray.Direction * hitDistance;

        // 面がレイと同じ向きを向いていれば裏面。プローブが閉じたジオメトリの内部にいる場合、
        // 周囲のほとんどがこれになる
        const bool backFace = (dot(hitNormal, rayDirection) > 0.0f);
        if (backFace)
        {
            // 裏面は「光を返さない遮蔽物」として扱う。
            //
            // 【距離を負で記録する】更新CSがこの符号を数えてプローブの裏面ヒット率を求め、
            // 壁の内部に落ちたプローブを見分ける(RTXGIのプローブ分類と同じ約束)。
            // ラスタ経路は length() の非負値と空の 1e6 しか書かないので、
            // 負であることが「レイトレース経路が裏面に当てた」ことの一意な印になる。
            // 距離モーメント側は abs() を取って使うので、遮蔽の判定はこれまでどおり効く
            radiance = float3(0.0f, 0.0f, 0.0f);
            distance = -hitDistance;
        }
        else
        {
            radiance = ShadeProbeRayHit(hitPosition, hitNormal, hitMaterial, rayDirection);
            distance = hitDistance;
        }
    }
    else
    {
        // 空へ抜けた。ラスタ経路の空埋めと同じテクスチャ・同じ引き方・同じ距離にする
        radiance = SourceSkybox.SampleLevel(MaterialSampler, rayDirection, 0.0f).rgb;
        distance = kProbeSkyDistance;
    }

    ProbeRadianceOut[uint3(dispatchThreadID.xy, 0)] = float4(radiance, 1.0f);
    ProbeDistanceOut[uint3(dispatchThreadID.xy, 0)] = distance;
}
