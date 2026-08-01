// DDGI(Dynamic Diffuse Global Illumination、22章)のサンプリング側。
//
// DDGIProbeUpdate.hlslが焼いたオクタヘドラルアトラスから、ワールド座標と法線を与えて
// 拡散の間接光(イラディアンス)を取り出す。
//
// 【20章の単一定義規則との関係】ここが返すのは拡散イラディアンスだけで、鏡面
// (ReflectionProbe.hlsliのprefiltered / SpecularIBLWeight)には一切触れない。
// したがって「SSRはDeferredLightingが足した鏡面IBLと厳密に同じ量を引く」という20章の
// 不変条件はDDGIを入れても自動的に保たれる。DDGIをSSRのパスへ持ち込んではならない。
//
// インクルードする側は以下のマクロを定義してからインクルードすること。
//   KURENAI_DDGI_IRRADIANCE_REGISTER   イラディアンスアトラス(Texture2D)
//   KURENAI_DDGI_DISTANCE_REGISTER     距離モーメントアトラス(Texture2D)
//
// FrameConstants(b0)の DDGIParams0〜3 を参照する。C++側 KurenaiEngine3D.cpp の
// FrameConstantsと並びを一致させること。
#ifndef KURENAI_DDGI_HLSLI
#define KURENAI_DDGI_HLSLI

#include "Samplers.hlsli"

Texture2D DDGIIrradianceAtlas : register(KURENAI_DDGI_IRRADIANCE_REGISTER);
Texture2D DDGIDistanceAtlas : register(KURENAI_DDGI_DISTANCE_REGISTER);

// 全ての重みが極小になったとき(格子の隅など)に真っ暗にならないための下限。
// RTXGIが同じ理由で置いているクランプに相当する
static const float kDDGIMinWeight = 0.05f;

// --- オクタヘドラル投影 ---
// DDGIProbeUpdate.hlslのDirectionToOctahedralと1文字も違わない実装でなければならない。
// ここがずれると「焼いた方向」と「引く方向」が食い違い、間接光が見当違いの方向から来る
float2 DDGIDirectionToOctahedral(float3 direction)
{
    const float3 d = direction / max(abs(direction.x) + abs(direction.y) + abs(direction.z), 1e-8f);
    if (d.y >= 0.0f)
    {
        return float2(d.x, d.z);
    }
    return float2(
        (1.0f - abs(d.z)) * (d.x >= 0.0f ? 1.0f : -1.0f),
        (1.0f - abs(d.x)) * (d.z >= 0.0f ? 1.0f : -1.0f));
}

// プローブ番号と方向から、アトラス上のUVを求める。
//
// アトラスの並びは 列 = Cx*Cy、行 = Cz(C++側 RecreateDDGIAtlases と一致)。
// セルの内側(境界を除いた texels x texels)へ収まるようスケールしてから、境界ぶんだけずらす。
// バイリニア補間はセルの縁で境界テクセルを拾い、それは対辺から複製されているので
// 折り返しが正しく効く
float2 DDGIProbeAtlasUV(uint probeIndex, float3 direction, uint3 probeCounts, float texels, float border)
{
    const uint slice = probeCounts.x * probeCounts.y;
    const uint row = probeIndex / slice;
    const uint column = probeIndex - row * slice;

    const float cell = texels + border * 2.0f;
    const float2 atlasSize = float2((float)slice * cell, (float)probeCounts.z * cell);

    // [-1,1] を [0,texels] のテクセル座標へ
    const float2 oct = DDGIDirectionToOctahedral(direction);
    const float2 inCell = (oct * 0.5f + 0.5f) * texels;

    const float2 texel = float2((float)column, (float)row) * cell + border + inCell;
    return texel / atlasSize;
}

// 格子座標(整数)からワールド座標を求める。C++側 ComputeDDGIProbePosition と一致させること
float3 DDGIProbePosition(uint3 gridCoord, float3 origin, float3 spacing)
{
    return origin + float3(gridCoord) * spacing;
}

uint DDGIProbeIndex(uint3 gridCoord, uint3 probeCounts)
{
    return gridCoord.x + gridCoord.y * probeCounts.x + gridCoord.z * probeCounts.x * probeCounts.y;
}

// worldPos の拡散間接光。ボリュームの外や未初期化時は呼び出し側が DDGIParams0.w で弾く。
//
// N        シェーディングする面の法線
// V        面からカメラへ向かう単位ベクトル
float3 SampleDDGIIrradiance(float3 worldPos, float3 N, float3 V)
{
    const float3 origin = DDGIParams0.xyz;
    const float3 spacing = DDGIParams1.xyz;
    const uint3 probeCounts = uint3((uint)DDGIParams2.x, (uint)DDGIParams2.y, (uint)DDGIParams2.z);
    const float normalBias = DDGIParams1.w;
    const float viewBias = DDGIParams2.w;
    const float irradianceTexels = DDGIParams3.x;
    const float distanceTexels = DDGIParams3.y;
    const float border = DDGIParams3.w;

    // 【照会点(query point)】遮蔽判定にだけ使う、少しずらした位置。
    //
    // worldPos をそのまま使うと「面が、自分を直接照らしているプローブから見えていない」と
    // 誤判定する(自己遮蔽)。記録されている距離 r̄ はコーンの加重平均なので、面が曲がって
    // いたり斜めだったりすると容易に distance(probe, worldPos) より僅かに小さく出て、
    // チェビシェフ判定が境界上で1を下回るためである。
    // これはほぼ全ての面で一様に起きるので画面全体が均一に暗くなり、視点に依存しないため
    // ちらつかず、「GIが弱いだけ」に見えてバグと気づきにくい。ゲインで持ち上げると
    // 今度は本当に遮蔽されるべき場所の光漏れが増えるので、バイアスでしか直らない。
    //
    // 反射プローブのProbeVisibility(19.12節)へ入れたのとまったく同じ対処である
    const float3 biasedPos = worldPos + N * normalBias + V * viewBias;

    // 格子内の連続座標。floorが手前側のプローブ、fracがトライリニアの重み
    const float3 gridSpace = (biasedPos - origin) / spacing;
    const int3 baseCoord = (int3)floor(gridSpace);
    const float3 trilinear = saturate(gridSpace - float3(baseCoord));

    float3 accumulated = float3(0.0f, 0.0f, 0.0f);
    float totalWeight = 0.0f;

    [unroll]
    for (uint i = 0; i < 8; ++i)
    {
        // 000〜111 の8近傍
        const uint3 offset = uint3(i & 1u, (i >> 1u) & 1u, (i >> 2u) & 1u);
        const int3 coord = clamp(baseCoord + int3(offset), int3(0, 0, 0), int3(probeCounts) - 1);
        const uint3 gridCoord = (uint3)coord;
        const uint probeIndex = DDGIProbeIndex(gridCoord, probeCounts);

        const float3 probePos = DDGIProbePosition(gridCoord, origin, spacing);
        const float3 toProbe = probePos - biasedPos;
        const float distanceToProbe = length(toProbe);
        const float3 dirToProbe = (distanceToProbe > 1e-6f) ? (toProbe / distanceToProbe) : N;

        // (1) トライリニアの重み
        const float3 axisWeight = lerp(1.0f - trilinear, trilinear, float3(offset));
        float weight = axisWeight.x * axisWeight.y * axisWeight.z;

        // (2) backface weight。プローブが面の裏側にある場合を落とす。
        //     ハードな0/1ではなく (dot+1)/2 の二乗にするのは、格子が疎なときに
        //     境界が階段状に出るのを避けるため
        const float backface = (dot(dirToProbe, N) + 1.0f) * 0.5f;
        weight *= backface * backface;

        // (3) チェビシェフ可視性。プローブが記録した「その方向の面までの距離」の
        //     平均と分散から、距離 t の点が見えている確率の上界を求める(分散シャドウマップと
        //     同じ式)。低解像度でも遮蔽の輪郭がガタつかないのが確率にする利点
        const float2 momentUV = DDGIProbeAtlasUV(probeIndex, -dirToProbe, probeCounts, distanceTexels, border);
        const float2 moments = DDGIDistanceAtlas.SampleLevel(ColorSampler, momentUV, 0.0f).rg;
        const float meanDistance = moments.x;
        const float variance = max(moments.y - meanDistance * meanDistance, 0.0f);

        if (distanceToProbe > meanDistance)
        {
            const float delta = distanceToProbe - meanDistance;
            weight *= variance / (variance + delta * delta);
        }

        // 全てのプローブが落ちきって真っ暗にならないよう下限を置く
        weight = max(weight, kDDGIMinWeight * axisWeight.x * axisWeight.y * axisWeight.z);
        if (weight <= 0.0f)
        {
            continue;
        }

        // イラディアンスは法線方向で引く(距離は「プローブから見た向き」で引くのに対し、
        // こちらは「面が向いている向き」であることに注意)
        const float2 irradianceUV = DDGIProbeAtlasUV(probeIndex, N, probeCounts, irradianceTexels, border);
        accumulated += DDGIIrradianceAtlas.SampleLevel(ColorSampler, irradianceUV, 0.0f).rgb * weight;
        totalWeight += weight;
    }

    if (totalWeight <= 0.0f)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    // アトラスは露出非依存の物理量で持っている(理由はC++側 FrameConstants::DDGIParams4 の
    // コメント参照)。ここでこのフレームの実効プリ露出を掛けて、他のライティングと同じ
    // 表示レンジへ戻す。DDGIParams3.z は強度倍率
    return (accumulated / totalWeight) * DDGIParams3.z * DDGIParams4.x;
}

#endif // KURENAI_DDGI_HLSLI
