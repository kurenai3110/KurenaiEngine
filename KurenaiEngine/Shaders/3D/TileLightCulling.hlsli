// 「そのライトはこのタイルに届くか」の判定を1か所へ集めた共有ヘッダー。
// タイルライトカリング(LightCulling.hlsl)と MegaLights の候補プール
// (MegaLightsTilePool.hlsl)が**同じ判定**を使うためのもの。
//
// 【なぜ共有しなければならないのか】MegaLights の候補プールは「タイルに届くライトの集合」を
// 定義域とし、その中から寄与に比例した確率で1灯を選ぶ。**定義域が従来のカリングとずれると、
// 片方にしか入らないライトが出て、確率的サンプリングにバイアスが乗る。**
// しかも絵は「それらしく」出るため気付けない。定義を1つにして構造的に保証する。
//
// 【インクルードする側の責務】このヘッダーより前に、次を用意しておくこと。
//   - struct GPULight (PunctualLighting.hlsli)
// 定数バッファには依存しない ―― 必要な値はすべて引数で受け取る。
// LightCulling.hlsl と MegaLightsTilePool.hlsl でレジスタもcbufferの並びも違うため

#ifndef KURENAI_TILE_LIGHT_CULLING_HLSLI
#define KURENAI_TILE_LIGHT_CULLING_HLSLI

// タイルの1辺のピクセル数。スレッドグループのサイズ(numthreads)と一致させること。
// C++側 KurenaiEngine3D::kLightTileSize と必ず同じ値にする
static const uint kTileSize = 16u;
static const uint kTileThreadCount = kTileSize * kTileSize;

// タイルの視錐台(側面4枚)と、タイル内サーフェスの深度スラブ
struct TileFrustum
{
    // 内向き法線。距離項は常に0(側面はいずれもカメラ原点を通るため)
    float3 Planes[4];
    // View空間Z。nearestが最も手前のサーフェス、farthestが最も奥
    float NearestViewZ;
    float FarthestViewZ;
};

// Reverse-Zの深度値からView空間Z(値が大きいほど遠い)を復元する。
// a/bは射影行列から作った深度リニアライズ定数(viewZ = b / (depth - a))
float TileViewZFromDepth(float depth, float a, float b)
{
    return b / (depth - a);
}

// タイルの視錐台を組み立てる。
// 射影行列(行ベクトル規約)は clip.x = viewX * P00、clip.w = viewZ なので ndc.x = viewX * P00 / viewZ。
// タイルのNDC範囲に入る条件をそのまま平面にする
TileFrustum MakeTileFrustum(
    uint2 tileCoord, uint2 renderSize, float p00, float p11, float nearestViewZ, float farthestViewZ)
{
    const float2 tileMin = float2(tileCoord * kTileSize);
    const float2 tileMax = min(tileMin + float2(kTileSize, kTileSize), float2(renderSize));
    const float2 invRenderSize = 1.0f / float2(renderSize);

    const float ndcMinX = tileMin.x * invRenderSize.x * 2.0f - 1.0f;
    const float ndcMaxX = tileMax.x * invRenderSize.x * 2.0f - 1.0f;
    // 画面Yは下向き、NDC Yは上向きなので上下が入れ替わる
    const float ndcMaxY = 1.0f - tileMin.y * invRenderSize.y * 2.0f;
    const float ndcMinY = 1.0f - tileMax.y * invRenderSize.y * 2.0f;

    TileFrustum frustum;
    // 左面: P00*viewX - ndcMinX*viewZ >= 0、右面: ndcMaxX*viewZ - P00*viewX >= 0(上下も同様)
    frustum.Planes[0] = normalize(float3(p00, 0.0f, -ndcMinX));
    frustum.Planes[1] = normalize(float3(-p00, 0.0f, ndcMaxX));
    frustum.Planes[2] = normalize(float3(0.0f, p11, -ndcMinY));
    frustum.Planes[3] = normalize(float3(0.0f, -p11, ndcMaxY));
    frustum.NearestViewZ = nearestViewZ;
    frustum.FarthestViewZ = farthestViewZ;
    return frustum;
}

// タイルの視錐台スラブを包むView空間のAABBを求める。
//
// 【何のためにあるか】MegaLights の候補プールは「そのライトがタイルへどれだけ届くか」を
// 重みにするが、錐台そのものとの最短距離は求めるのが面倒で、しかも保守的でなくてよい。
// 錐台を包むAABBで代用すると**距離を過小評価する = 重みを過大評価する**側に倒れる。
// 重みの過大評価はサンプリングの効率を落とすだけで正しさには影響しないが、
// 過小評価は「届くのに選ばれない」= バイアスになる。必ずこの向きに倒すこと。
void TileViewSpaceAABB(
    uint2 tileCoord, uint2 renderSize, float p00, float p11, float nearestViewZ, float farthestViewZ,
    out float3 aabbMin, out float3 aabbMax)
{
    const float2 tileMin = float2(tileCoord * kTileSize);
    const float2 tileMax = min(tileMin + float2(kTileSize, kTileSize), float2(renderSize));
    const float2 invRenderSize = 1.0f / float2(renderSize);

    const float ndcMinX = tileMin.x * invRenderSize.x * 2.0f - 1.0f;
    const float ndcMaxX = tileMax.x * invRenderSize.x * 2.0f - 1.0f;
    const float ndcMaxY = 1.0f - tileMin.y * invRenderSize.y * 2.0f;
    const float ndcMinY = 1.0f - tileMax.y * invRenderSize.y * 2.0f;

    // ndc.x = viewX * p00 / viewZ より viewX = ndc.x * viewZ / p00。
    // 手前と奥それぞれの断面の4隅を取り、その全体を包む
    const float zNear = min(nearestViewZ, farthestViewZ);
    const float zFar = max(nearestViewZ, farthestViewZ);
    const float invP00 = 1.0f / max(abs(p00), 1e-6f);
    const float invP11 = 1.0f / max(abs(p11), 1e-6f);

    const float2 nearXY0 = float2(ndcMinX, ndcMinY) * zNear * float2(invP00, invP11);
    const float2 nearXY1 = float2(ndcMaxX, ndcMaxY) * zNear * float2(invP00, invP11);
    const float2 farXY0 = float2(ndcMinX, ndcMinY) * zFar * float2(invP00, invP11);
    const float2 farXY1 = float2(ndcMaxX, ndcMaxY) * zFar * float2(invP00, invP11);

    const float2 minXY = min(min(nearXY0, nearXY1), min(farXY0, farXY1));
    const float2 maxXY = max(max(nearXY0, nearXY1), max(farXY0, farXY1));

    aabbMin = float3(minXY, zNear);
    aabbMax = float3(maxXY, zFar);
}

// ライトの影響範囲を包むView空間の球を求める。
// ポイントはそのままRangeが半径。スポットは円錐の外接球を使う(円錐そのものとの厳密な判定より
// 保守的=多めに残るが、カリングは「取りこぼさない」ことが正しさの条件なので保守的側で問題ない)
void ComputeLightBoundingSphere(GPULight light, float4x4 view, out float3 viewCenter, out float radius)
{
    const uint lightType = (uint)light.PositionType.w;
    const float range = light.ColorRange.w;
    const float3 worldPosition = light.PositionType.xyz;

    if (lightType == 2u) // Spot
    {
        // DirectionAngle.w = 1 / max(0.001, cos(inner) - cos(outer))、Params.x = -cos(outer) * scale。
        // ここから cos(outer) = -Params.x / DirectionAngle.w を復元する
        // (CPU側で角度そのものを持たせず、既存のGPULightのレイアウトを変えずに済ませるため)
        const float angleScale = max(light.DirectionAngle.w, 1e-6f);
        const float cosOuter = saturate(-light.Params.x / angleScale);

        // 円錐(頂点=ライト位置、軸=Direction、母線長=Range、半頂角=outer)の外接球。
        // 半頂角が45度を超えると底面の円が最大断面になるため式が切り替わる
        float3 worldCenter;
        if (cosOuter < 0.70710678f) // outer > 45度
        {
            worldCenter = worldPosition + light.DirectionAngle.xyz * (range * cosOuter);
            radius = range * sqrt(saturate(1.0f - cosOuter * cosOuter));
        }
        else
        {
            const float sphereRadius = range / (2.0f * max(cosOuter, 1e-4f));
            worldCenter = worldPosition + light.DirectionAngle.xyz * sphereRadius;
            radius = sphereRadius;
        }
        viewCenter = mul(float4(worldCenter, 1.0f), view).xyz;
        // ポイントと同じ理由で光源そのものの半径を足す。
        // 【スポットも早期returnするので、ここにも足すこと】ポイント側だけ直すと
        // 半影が出るスポットでだけ縁が欠け、原因がスポット固有に見えて追いにくくなる
        radius += max(light.Params.z, 0.0f);
        return;
    }

    viewCenter = mul(float4(worldPosition, 1.0f), view).xyz;
    // 【光源そのものの半径を足すこと】球光源は中心から SourceRadius だけ外へ広がっており、
    // 中心が Range だけ離れた面でも球の手前側からは光が届く。足さないと光源の近くで
    // タイルから取りこぼし、球の縁が黒く欠ける(カリングは取りこぼさないことが正しさの条件)
    radius = range + max(light.Params.z, 0.0f);
}

// そのライトがタイルへ届くか。平行光は距離減衰を持たず画面全体に届くため常に真。
// viewCenter / radius は呼び出し側でも使えるようoutで返す(境界球を2回求めないため)
bool IsLightVisibleInTile(GPULight light, float4x4 view, TileFrustum frustum, out float3 viewCenter, out float radius)
{
    ComputeLightBoundingSphere(light, view, viewCenter, radius);

    if ((uint)light.PositionType.w == 0u) // Directional
    {
        return true;
    }

    // 深度スラブ(タイル内の最も近い/遠いサーフェスの間)との判定
    if (viewCenter.z + radius < frustum.NearestViewZ || viewCenter.z - radius > frustum.FarthestViewZ)
    {
        return false;
    }

    [unroll]
    for (uint p = 0; p < 4; ++p)
    {
        if (dot(frustum.Planes[p], viewCenter) < -radius)
        {
            return false;
        }
    }
    return true;
}

#endif // KURENAI_TILE_LIGHT_CULLING_HLSLI
