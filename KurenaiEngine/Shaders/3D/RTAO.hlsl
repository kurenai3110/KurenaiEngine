// レイトレーシングAO/GIパス。SSAO.hlsl / SSIL_VisibilityBitmask.hlslのレイトレーシング版で、
// 出力フォーマット(rgb=間接拡散光, a=遮蔽率)も後段の扱いもまったく同じ。
// このパスの結果はAOBlurパスでブラーされ、DeferredLighting.hlslが1枚読むだけになる
// (詳細はdocs/Architecture.html 27章)。
//
// 【スクリーンスペース手法に対する利点】
//   - 画面に映っていない遮蔽物・反射面も効く。SSAO/SSILは深度バッファに写っている面しか
//     遮蔽物として扱えず、画面外や手前の面に隠れた物は無視されていた
//   - 遮蔽の判定が実際の交差判定になる。SSILの「厚み(Thickness Heuristic)」のような、
//     深度バッファの1点にどれだけ奥行きがあると仮定するかの調整が要らない
//   - 半球を余弦重みでサンプルするだけなので、遮蔽率がそのままモンテカルロ積分の推定値になる
//
// 【この段階の制約】docs/Architecture.html 27章。要点:
//   - バウンス面が画面に映っていればDirectLightingパスの結果(全ライトぶん、影付き)を使うが、
//     映っていない場合は太陽の直接光だけを解析的に求めた簡略版へ落ちる。そのフォールバックでは
//     バウンス面のテクスチャも読めない(bindlessが要る)ため色はマテリアルの定数値になる
//   - 1バウンスのみ。バウンス面が受ける光に環境光は含めない
//     (SSILと同じ方針。DeferredLighting側のアンビエントと二重計上しないため)
//   - すべての三角形を不透明として扱う(RAY_FLAG_FORCE_OPAQUE)
//   - デノイザを持たないため、サンプル数が少ないとノイズが出る(後段のAOBlurが均す)
#include "NormalEncoding.hlsli"

// レイの始点を法線方向へ押し出す量(ワールド単位)。RTShadow.hlslと同じ扱いで、
// 深度から復元したワールド座標の誤差が遠方ほど大きいためカメラ距離に比例する項も足す
static const float kRayOriginBias = 0.01f;
static const float kRayOriginBiasSlope = 1e-4f;
// バウンス面から太陽へ撃つ影レイの最大距離(ワールド単位)
static const float kShadowRayMaxDistance = 1.0e4f;
// バウンス面が画面のその画素に映っているとみなす位置ずれの許容量。カメラからの距離への比率
// (1画素が覆うワールド空間の大きさは遠いほど大きくなるため)。TryFetchScreenRadiance参照
static const float kScreenMatchTolerance = 0.02f;

static const float kPI = 3.14159265359f;
static const float kTwoPI = 6.28318530718f;
// 黄金比の小数部。サンプルごとに方位角をずらす低食い違い量列(Rank-1格子)に使う
static const float kGoldenRatioFrac = 0.61803398875f;

cbuffer FrameConstants : register(b0)
{
    // バウンス面が画面のどこに映っているかを求めるのに使う
    float4x4 ViewProj;
    float4x4 InvViewProj;
    // このシェーダでは未使用(オフセット合わせのためだけに宣言する)
    float4x4 CascadeViewProj[4];
    float4 CameraPosition;
    // xyz=太陽の進行方向(光が飛んでいく向き)。太陽へ向かうベクトルは -LightDirection.xyz
    float4 LightDirection;
    // rgb=太陽の放射輝度(露出適用済み)。太陽が無効なシーンでは0が入る
    float4 LightColor;
    // このシェーダでは未使用(オフセット合わせのためだけに宣言する)
    float4x4 View;
    float4x4 Proj;
    float4 AmbientColor;
    float4 CascadeSplits;
    float4 ShadowParams;
    // 【宣言はここで止めている】このシェーダーが読むのはShadowParamsまでで、それより後ろは使わない。
    // C++側のFrameConstantsはこの後ろにTimeParams・Sky*・Cloud*・PlanarReflectionPlane・
    // Fog*・WaterBodyColorを持つが、cbufferは宣言順レイアウトなので、途中を飛ばして末尾だけを
    // 宣言すると誤ったオフセットを読む。しかもコンパイルは通り絵も「それらしく」出るため気付けない。
    // これらが必要になったら、C++の並びどおりに間のフィールドをすべて宣言すること
};

cbuffer RTAOConstants : register(b1)
{
    // xy: 出力サイズ(ピクセル), z: レイの最大距離(ワールド単位), w: 遮蔽率のコントラスト(べき乗)
    float4 Params0;
    // x: 1ピクセルあたりのレイ本数, y: 間接光の強さ, z: バウンス面へ影レイを撃つか(1で撃つ), w: 未使用
    float4 Params1;
};

// --- レイトレーシング資源 ---
RaytracingAccelerationStructure SceneTLAS : register(t0);

// --- G-Buffer ---
Texture2D NormalTexture : register(t1);
Texture2D DepthTexture : register(t2);
// DirectLighting.hlslが計算済みの直接光(シャドウ適用済み、環境光は含まない)。
// バウンス面が画面に映っていればこの値をそのまま再放射の放射輝度として使う(ShadeBounceSurface参照)
Texture2D DirectLightTexture : register(t8);

// シーンジオメトリの統合バッファ。構造体の写しと索引の辿り方はRTReflection.hlslと共有する
#define KURENAI_RT_ATTRIBUTE_REGISTER t3
#define KURENAI_RT_INDEX_REGISTER t4
#define KURENAI_RT_MESHINFO_REGISTER t5
#define KURENAI_RT_INSTANCEINFO_REGISTER t6
#define KURENAI_RT_MATERIAL_REGISTER t7
#include "RaytracingScene.hlsli"

// rgb=間接拡散光(イラディアンス), a=遮蔽率。SSAO/SSILの出力と同じ意味・同じフォーマット
RWTexture2D<float4> OutputTexture : register(u0);

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

// 法線周りの正規直交基底(Duff et al. 2017 の分岐なし版)
void BuildOrthonormalBasis(float3 n, out float3 tangent, out float3 bitangent)
{
    const float sign = (n.z >= 0.0f) ? 1.0f : -1.0f;
    const float a = -1.0f / (sign + n.z);
    const float b = n.x * n.y * a;
    tangent = float3(1.0f + sign * n.x * n.x * a, sign * b, -sign * n.x);
    bitangent = float3(b, sign + n.y * n.y * a, -n.y);
}

// 法線周りの半球を余弦重み(cosθ/π)でサンプルする。
// この重みで撃つと「遮蔽されたレイの割合」がそのまま余弦重み付き遮蔽率の推定値になり、
// 間接光も E = π/N * ΣL で放射照度(イラディアンス)の推定値になる
float3 SampleCosineHemisphere(float3 n, float3 tangent, float3 bitangent, float2 u)
{
    const float radius = sqrt(u.x);
    const float phi = kTwoPI * u.y;
    const float z = sqrt(max(0.0f, 1.0f - u.x));
    return normalize(tangent * (radius * cos(phi)) + bitangent * (radius * sin(phi)) + n * z);
}

// バウンス面が画面のどこかに映っていれば、そのピクセルの直接光(DirectLightingパスの結果)を
// 再放射の放射輝度として返す。映っていなければ false を返す。
//
// 【なぜこれをやるか】このシーンの光源は太陽だけとは限らない。ポイント/スポットライトで
// 照らされた屋内では、太陽だけを解析的に計算しても間接光が常に0になってしまう
// (実際にBistro Interiorで発生した)。DirectLightingパスの結果には太陽もポイント/スポットも
// 影付きで入っているため、映ってさえいればこれを使うのが最も正確で最も安い。
// SSILがサンプル地点の直接光を拾っているのと同じ考え方だが、SSILは「画面に映っている面しか
// 見つけられない」のに対し、こちらは面をワールド空間のレイで見つけたうえで
// 放射輝度の取得にだけ画面を使う点が違う。
//
// 一致判定は深度値の比較ではなく、その画素の深度から復元したワールド座標との距離で行う。
// 深度はReverse-Zで非線形なため、値の差に一定のしきい値を置いても遠近で意味が変わってしまう。
// しきい値をカメラからの距離に比例させているのは、1画素が覆うワールド空間の大きさが
// 遠いほど大きくなるためである
bool TryFetchScreenRadiance(float3 hitPosition, uint2 outputSize, out float3 radiance)
{
    radiance = float3(0.0f, 0.0f, 0.0f);

    const float4 clipPos = mul(float4(hitPosition, 1.0f), ViewProj);
    if (clipPos.w <= 0.0f)
    {
        return false; // カメラの背後
    }

    const float3 ndc = clipPos.xyz / clipPos.w;
    if (abs(ndc.x) > 1.0f || abs(ndc.y) > 1.0f)
    {
        return false; // 画面外
    }

    const float2 uv = float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
    const uint2 samplePixel = min((uint2)(uv * float2(outputSize)), outputSize - uint2(1u, 1u));

    const float sampleDepth = DepthTexture[samplePixel].r;
    if (sampleDepth <= 0.0f)
    {
        return false; // そこは背景(スカイ)。バウンス面は手前の何かに隠れている
    }

    const float2 sampleUV = (float2(samplePixel) + 0.5f) / float2(outputSize);
    const float3 screenWorldPos = ReconstructWorldPos(sampleUV, sampleDepth);
    const float tolerance = kScreenMatchTolerance * length(hitPosition - CameraPosition.xyz);
    if (distance(screenWorldPos, hitPosition) > tolerance)
    {
        return false; // 別の面が手前にある
    }

    radiance = DirectLightTexture[samplePixel].rgb;
    return true;
}

// バウンス面が受光面へ返す放射輝度。DeferredLighting.hlslが
// (diffuseColor / PI) * indirectLight として受け取るので、ここではランバート反射の
// 出射放射輝度 L = (albedo / PI) * E を返す。
//
// 環境光を含めないのはSSILと同じ理由: DeferredLighting.hlslが受光面へ別途アンビエントを
// 足しているため、バウンス面のアンビエントまで拾うと環境光を二重に数えることになる
float3 ShadeBounceSurface(
    float3 hitPosition, float3 hitNormal, RTMaterial material, float3 rayDirection, uint2 outputSize)
{
    // 画面に映っていればすべてのライトの寄与が入った値をそのまま使える(上の関数の説明を参照)
    float3 screenRadiance;
    if (TryFetchScreenRadiance(hitPosition, outputSize, screenRadiance))
    {
        return screenRadiance + material.EmissiveFactor;
    }

    // --- 以降は画面外・手前の面に隠れている場合のフォールバック ---
    // 太陽の直接光だけを解析的に求める。ポイント/スポットライトは、1ヒットごとに
    // 全ライトを走査すると負荷がライト数に比例してしまうためここでは扱わない
    // (画面空間のタイルライトカリングはワールド空間のヒット点には使えない)

    // 裏面に当たった場合は法線を反転させる(片面ポリゴンのシーンでは普通に起きる)
    const float3 N = (dot(hitNormal, rayDirection) > 0.0f) ? -hitNormal : hitNormal;

    // 金属は拡散反射を持たない。この段階ではバウンス面の鏡面は撃たない
    const float3 diffuseAlbedo = material.BaseColorFactor.rgb * (1.0f - material.MetallicFactor);

    float3 radiance = float3(0.0f, 0.0f, 0.0f);
    const float3 toSun = normalize(-LightDirection.xyz);
    const float NdotL = saturate(dot(N, toSun));
    if (NdotL > 0.0f)
    {
        float shadow = 1.0f;
        if (Params1.z > 0.5f)
        {
            const bool occluded = TraceOcclusionRay(
                SceneTLAS, hitPosition + N * kRayOriginBias, toSun, kRayOriginBias, kShadowRayMaxDistance);
            shadow = occluded ? 0.0f : 1.0f;
        }
        radiance += (diffuseAlbedo / kPI) * LightColor.rgb * NdotL * shadow;
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

    const float depth = DepthTexture[pixel].r;
    if (depth <= 0.0f)
    {
        // 背景(スカイ)は遮蔽なし・間接光なし(SSAO/SSILと同じ)。
        // Reverse-Zのため遠平面(=背景)はNDC z=0.0付近になる
        OutputTexture[pixel] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    }

    const float maxRayDistance = Params0.z;
    const float aoPower = Params0.w;
    const uint sampleCount = max((uint)Params1.x, 1u);
    const float intensity = Params1.y;

    const float2 uv = (float2(pixel) + 0.5f) / float2(outputSize);
    const float3 worldPos = ReconstructWorldPos(uv, depth);
    const float3 N = OctDecode(NormalTexture[pixel].xy);

    const float originBias = kRayOriginBias + length(worldPos - CameraPosition.xyz) * kRayOriginBiasSlope;
    const float3 rayOrigin = worldPos + N * originBias;

    float3 tangent;
    float3 bitangent;
    BuildOrthonormalBasis(N, tangent, bitangent);

    // ピクセルごとに固定の乱数(フレーム番号を混ぜない)。RTShadow.hlslと同じ理由で、
    // デノイザ(蓄積)が無い構成では時間ジッタを入れるとノイズが毎フレーム動いてちらつく
    const uint seed = HashUint(pixel.x + pixel.y * outputSize.x + 0x9e3779b9u);
    const float randomOffset = float(seed) * 2.3283064365e-10f; // uint最大値で割って[0,1)へ
    const float randomPhase = float(HashUint(seed)) * 2.3283064365e-10f;

    uint occludedCount = 0u;
    float3 radianceSum = float3(0.0f, 0.0f, 0.0f);

    [loop]
    for (uint i = 0u; i < sampleCount; ++i)
    {
        // 半径方向は層化(ストラティファイ)し、方位角は黄金比で回す
        const float2 u = float2(
            (float(i) + randomOffset) / float(sampleCount),
            frac(randomPhase + float(i) * kGoldenRatioFrac));

        RayDesc ray;
        ray.Origin = rayOrigin;
        ray.Direction = SampleCosineHemisphere(N, tangent, bitangent, u);
        ray.TMin = originBias;
        ray.TMax = maxRayDistance;

        // 遮蔽の有無だけでなくバウンス面の情報も要るため、最初のヒットで打ち切らず
        // 最も近いヒットを取る(TraceOcclusionRayは使えない)
        RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
        query.TraceRayInline(SceneTLAS, RAY_FLAG_NONE, 0xFFu, ray);
        query.Proceed();

        if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
        {
            // 何にも当たらなかった=空が見えている。遮蔽なし。
            // 空からの光はDeferredLighting.hlslのアンビエント(IBL)が担当するのでここでは足さない
            continue;
        }

        occludedCount += 1u;

        float3 hitNormal;
        // 初期化しているのはdxcの警告対策(構造体のout引数は未初期化検査を通らない)
        RTMaterial hitMaterial = (RTMaterial)0;
        FetchHitSurface(
            query.CommittedInstanceID(),
            query.CommittedGeometryIndex(),
            query.CommittedPrimitiveIndex(),
            query.CommittedTriangleBarycentrics(),
            hitNormal,
            hitMaterial);

        const float3 hitPosition = ray.Origin + ray.Direction * query.CommittedRayT();
        radianceSum += ShadeBounceSurface(hitPosition, hitNormal, hitMaterial, ray.Direction, outputSize);
    }

    // 余弦重みでサンプルしているので、遮蔽されたレイの割合がそのまま余弦重み付き遮蔽率になる
    float ao = 1.0f - float(occludedCount) / float(sampleCount);
    ao = pow(saturate(ao), aoPower);

    // E = π/N * ΣL(余弦重みのモンテカルロ積分)。DeferredLighting.hlslが
    // (diffuseColor / PI) * indirectLight で受けるため、ここではイラディアンスを返す。
    // 負値だけは落とす(HDRバッファへ入るとブラー・ブルームへ伝播して回復不能になるため)
    const float3 gi = max(radianceSum * (kPI / float(sampleCount)) * intensity, 0.0f);

    OutputTexture[pixel] = float4(gi, ao);
}
