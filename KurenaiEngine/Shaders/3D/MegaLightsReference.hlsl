// MegaLights の参照実装(グラウンドトゥルース)。ポイント/スポットライトを**全灯そのまま**
// 評価し、届いた1灯ごとにTLASへ影レイを撃って、直接光の寄与をHDRで書き出す。
//
// 【何のためにあるか】MegaLights 本体は「候補から確率的に1灯選び、選んだ確率で割り戻す」
// 手法で、出てくる絵は必ずノイズを含む。**目視では正しさを判定できない。**
// そこで「ノイズのない真値」をこのパスで作り、以降の段階はすべてこれとの
// 相対誤差で評価する(N フレーム平均が 1/√N で真値へ寄るか、で不偏性を見る)。
// 出力先は MegaLights 本体とまったく同じテクスチャにしてあるので、
// 同じ表示経路・同じ後段のままA/Bが撮れる。
//
// 【恒等テスト】Params0.z(影レイ本数)を 0 にすると影を一切撃たず、可視率を常に1として
// 評価する。この状態の出力は、**スクリーンスペースシャドウを切った既存の直接光パス
// (DirectLighting.hlsl の t8 ライトループ)と数値的に一致するはず**である。
// BRDF・距離減衰・スポット円錐・プリ露出・G-Bufferの読み方をまとめて検証できる唯一の機会なので、
// 以降の段階へ進む前に必ずこれを通すこと。ここがずれていると後続の検証がすべて無意味になる。
//
// 【恒等テストが成り立つ前提を外さないこと】次のどれかが崩れると、実装が正しくても一致しない:
//   - スクリーンスペースシャドウが無効であること(既定はOFF)。あちらはライトループの内側で
//     影を掛けるため、有効だと比較にならない
//   - **タイルライトカリングの容量(kLightTileCapacity)を超えたタイルが1つも無いこと。**
//     容量を超えるとあちらは打ち切るが、このパスは全灯を回す。1タイルへライトが集中する
//     カメラでは原理的に一致しない。デバッグ表示「ライトタイル」のマゼンタで超過を確認できる
//     (ManyLightsTest は66灯 = 容量64より多く、起動時に警告が出る。固定カメラでは超過0)
//   - 太陽が無効か、太陽の寄与が両者で同じであること(太陽はこのパスの対象外)
//
// 【DirectLighting.hlsl とループを共有していない理由】あちらの EvaluateLight は
// スクリーンスペースシャドウ(画面空間のレイマーチ)を内側で呼んでおり、
// 影の求め方がこのパスと根本的に違う。式の共有は PunctualLighting.hlsli(BRDF・減衰・透過)
// までとし、**early-out の並びが食い違っていないことは上の恒等テストで担保する。**
//
// 【太陽は対象外】太陽は b0 の LightDirection/LightColor で別枠に扱われ、CSM か RTShadow が
// 影を持っている。MegaLights へ取り込まない理由は docs 側に書く(要点: 屋外では太陽の寄与が
// 他を何桁も上回るため、寄与に比例して1灯選ぶ確率的サンプリングに混ぜると
// ほぼ全ピクセルで太陽が当選し、「多数のライトを扱う」という機能自体が死ぬ)。
//
// DX12 かつ DXR Tier 1.1 のときだけ生成される(RayQuery は SM 6.5 の機能)。
#include "NormalEncoding.hlsli"
// Smith可視性項とスペキュラのエネルギー補正。BRDF積分LUTと同じ可視性項を使うことが
// エネルギー補正の前提になるため、DirectLighting.hlsl と定義を共有する
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
    // w にスペキュラのエネルギー補正のモードが入っている(MakeSpecularEnergyContextへ渡す)。
    // DirectLighting.hlsl と同じ値を使わないとエネルギーがずれる
    float4 ShadowParams;
    // 【宣言はここで止めている】読むのは ShadowParams まで。cbufferは宣言順レイアウトなので、
    // 途中を飛ばして末尾だけを宣言すると誤ったオフセットを読む。しかもコンパイルは通り
    // 絵も「それらしく」出るため気付けない(DirectLighting.hlsl と同じ注意)
};

cbuffer MegaLightsConstants : register(b1)
{
    // x=出力幅, y=出力高, z=1灯あたりに撃つ影レイの本数(**0なら影を撃たず可視率1**。恒等テスト用),
    // w=有効ライト数
    uint4 Params0;
};

RaytracingAccelerationStructure SceneTLAS : register(t0);

Texture2D NormalTexture : register(t1);
Texture2D DepthTexture : register(t2);
Texture2D AlbedoTexture : register(t3);
Texture2D MaterialTexture : register(t4);
// split-sum近似の第2項、BRDF積分LUT。EvaluateDirectBRDF がエネルギー補正で引く
Texture2D BRDFLUTTexture : register(t5);

// GPULight・距離減衰・スポット減衰・Cook-Torrance・透過。
// PI・SpecularEnergy.hlsli・BRDFLUTTexture・ColorSampler をすべて宣言したあとでインクルードする
#define KURENAI_PUNCTUAL_LIGHT_REGISTER t6
#define KURENAI_PUNCTUAL_LIGHTING_BRDF
#include "PunctualLighting.hlsli"

// 直接光(拡散+鏡面、影と透過を適用済み)。DirectLighting.hlsl が t7 で読んで加算する
RWTexture2D<float4> MegaLightsOutput : register(u0);

// レイの始点を法線方向へ押し出す量(ワールド単位)。深度バッファから復元したワールド座標は
// 遠方ほど誤差が大きいため、カメラからの距離に比例する項も足す(RTShadow.hlsl と同じ扱い)
static const float kRayOriginBias = 0.01f;
static const float kRayOriginBiasSlope = 1e-4f;
// 押し出し量を1/NdotLでスケールするときの下限(RTShadow.hlsl と同じ)
static const float kMinSlopeScaleNdotL = 0.1f;

float3 ReconstructWorldPos(float2 uv, float depth)
{
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float4 worldPos = mul(float4(ndc, depth, 1.0f), InvViewProj);
    return worldPos.xyz / worldPos.w;
}

// 1灯ぶんの可視率。punctual なので方向は1つに決まり、半影は出ない(常にハードシャドウ)。
//
// 【TMax を光源までの距離で打ち切ること】太陽(RTShadow.hlsl)は無限遠なので上限を
// 定数で置けるが、ポイント/スポットで同じことをすると**光源の向こう側のジオメトリが
// 遮蔽物になる**。症状は「壁際・天井際のライトが常に真っ暗」で、コピー元の
// RTShadow.hlsl にはこの罠が無い
float TraceLightVisibility(float3 rayOrigin, float3 L, float originBias, float distanceToLight)
{
    RayDesc ray;
    ray.Origin = rayOrigin;
    ray.Direction = L;
    ray.TMin = originBias;
    ray.TMax = max(distanceToLight - originBias, originBias);

    // 遮蔽の有無だけが分かればよいので、最初のヒットで打ち切る
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
    query.TraceRayInline(SceneTLAS, RAY_FLAG_NONE, 0xFFu, ray);
    query.Proceed();

    return (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ? 0.0f : 1.0f;
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

    // 深度もDirectLighting.hlslと同じサンプラー(DataSampler、Point)で引く。
    // uvは「そのピクセルの中心」で、PS側の補間で得られるUVと同じ位置になる
    const float2 uv = (float2(pixel) + 0.5f) / float2(outputSize);
    const float depth = DepthTexture.SampleLevel(DataSampler, uv, 0).r;
    if (depth <= 0.0f)
    {
        // 背景(スカイ)。Reverse-Zのため遠平面はNDC z=0.0付近になる。
        // 【必ず書くこと】RHIにUAVのクリアが無く、書かずにreturnすると前フレームの残骸が残る
        MegaLightsOutput[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    const float3 worldPos = ReconstructWorldPos(uv, depth);

    // 【G-Bufferの読み方をDirectLighting.hlslと厳密に揃える】あちらと同じサンプラーで、
    // 同じUVから引く。整数座標のLoadでも「同じテクセル」にはなるが、
    // ColorSampler(Linear)側は補間の重みが1ULPずれるだけで結果が動くため、
    // 恒等テストで1/255の差として現れる。読み方まで一致させておくと、
    // 差が出たときに「式が違う」以外の可能性を潰せる
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

    const uint lightCount = Params0.w;
    const uint shadowRayCount = Params0.z;

    float3 directLight = float3(0.0f, 0.0f, 0.0f);

    // 全灯を総当たりする。タイルカリングもサンプリングも通さないのが参照実装の要件で、
    // 「候補集合の作り方が間違っている」ことの影響を受けないようにするため
    [loop]
    for (uint i = 0u; i < lightCount; ++i)
    {
        const GPULight light = Lights[i];
        const uint lightType = (uint)light.PositionType.w;
        const float range = light.ColorRange.w;

        float3 L;
        float atten = 1.0f;
        float distanceToLight = 1e30f;

        if (lightType == 0u) // Directional
        {
            L = normalize(-light.DirectionAngle.xyz);
        }
        else
        {
            const float3 toLight = light.PositionType.xyz - worldPos;
            const float distSq = dot(toLight, toLight);
            if (distSq > range * range)
            {
                continue;
            }

            atten = DistanceAttenuation(distSq, range);
            if (atten <= 0.0f)
            {
                continue;
            }

            const float dist = sqrt(max(distSq, 1e-16f));
            distanceToLight = dist;
            L = toLight / dist;

            if (lightType == 2u) // Spot
            {
                const float spotAtten =
                    SpotAttenuation(light.DirectionAngle.xyz, L, light.DirectionAngle.w, light.Params.x);
                if (spotAtten <= 0.0f)
                {
                    continue;
                }
                atten *= spotAtten;
            }
        }

        // 透過するマテリアルでは NdotL<=0 の側にこそ寄与があるので打ち切らない
        // (DirectLighting.hlsl の EvaluateLight と同じ判定にすること)
        const float NdotL = dot(N, L);
        if (NdotL <= 0.0f && translucency <= 0.0f)
        {
            continue;
        }

        float shadow = 1.0f;
        // Params0.z が 0 のときは影を撃たない(恒等テスト)。ライト側の CastShadow(Params.y)が
        // 0 の灯も撃たない ―― 既存経路の扱いと揃えるため
        if (shadowRayCount > 0u && light.Params.y > 0.5f)
        {
            const float slopeScale = 1.0f / max(NdotL, kMinSlopeScaleNdotL);
            const float originBias =
                (kRayOriginBias + length(worldPos - CameraPosition.xyz) * kRayOriginBiasSlope) * slopeScale;
            // punctual なので方向が1つに決まり、何本撃っても同じ答えになる。
            // 本数を活かせるのは光源に半径が入ってから(GPULight.Params.z を使う段階)
            shadow = TraceLightVisibility(worldPos + N * originBias, L, originBias, distanceToLight);
        }

        const float3 reflected = EvaluateDirectBRDF(N, V, L, NdotV, albedo, metallic, roughness, energy);
        const float transmissionShadow = lerp(saturate(translucency * kTranslucencyShadowFloor), 1.0f, shadow);
        const float3 transmitted = EvaluateTranslucency(N, V, L, albedo, translucency) * transmissionShadow;
        directLight += reflected * shadow * light.ColorRange.rgb * atten + transmitted * light.ColorRange.rgb * atten;
    }

    MegaLightsOutput[pixel] = float4(directLight, 1.0f);
}
