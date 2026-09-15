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
//   - LightAttenuationUpperBound (LightAttenuation.hlsli。PunctualLighting.hlsli が
//     無条件に読むので、上を満たせば自動的に揃う)
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

// 画素原点からタイルの視錐台を組み立てる。原点は格子ジッターで負になりうるためint2。
// はみ出した範囲を画面内へクランプし、実際に深度を集めた画素範囲より錐台を狭めない
// 射影行列(行ベクトル規約)は clip.x = viewX * P00、clip.w = viewZ なので ndc.x = viewX * P00 / viewZ。
// タイルのNDC範囲に入る条件をそのまま平面にする
TileFrustum MakeTileFrustumFromPixelOrigin(
    int2 pixelOrigin, uint2 renderSize, float p00, float p11, float nearestViewZ, float farthestViewZ)
{
    const float2 unclampedTileMin = float2(pixelOrigin);
    const float2 renderMax = float2(renderSize);
    const float2 tileMin = clamp(unclampedTileMin, float2(0.0f, 0.0f), renderMax);
    const float2 tileMax = clamp(
        unclampedTileMin + float2(kTileSize, kTileSize), float2(0.0f, 0.0f), renderMax);
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

// 従来のライトカリングは格子をずらさない。既存の呼び出しと浮動小数点の入力を保つ薄いラッパ
TileFrustum MakeTileFrustum(
    uint2 tileCoord, uint2 renderSize, float p00, float p11, float nearestViewZ, float farthestViewZ)
{
    return MakeTileFrustumFromPixelOrigin(
        int2(tileCoord * kTileSize), renderSize, p00, p11, nearestViewZ, farthestViewZ);
}

// タイルの視錐台スラブを包むView空間のAABBを求める。
//
// 【何のためにあるか】MegaLights の候補プールは「そのライトがタイルへどれだけ届くか」を
// 重みにするが、錐台そのものとの最短距離は求めるのが面倒で、しかも保守的でなくてよい。
// 錐台を包むAABBで代用すると**距離を過小評価する = 重みを過大評価する**側に倒れる。
// 重みの過大評価はサンプリングの効率を落とすだけで正しさには影響しないが、
// 過小評価は「届くのに選ばれない」= バイアスになる。必ずこの向きに倒すこと。
void TileViewSpaceAABBFromPixelOrigin(
    int2 pixelOrigin, uint2 renderSize, float p00, float p11, float nearestViewZ, float farthestViewZ,
    out float3 aabbMin, out float3 aabbMax)
{
    const float2 unclampedTileMin = float2(pixelOrigin);
    const float2 renderMax = float2(renderSize);
    const float2 tileMin = clamp(unclampedTileMin, float2(0.0f, 0.0f), renderMax);
    const float2 tileMax = clamp(
        unclampedTileMin + float2(kTileSize, kTileSize), float2(0.0f, 0.0f), renderMax);
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

// 既存のライトカリング向け。画素原点版へ従来どおり tileCoord * kTileSize を渡す
void TileViewSpaceAABB(
    uint2 tileCoord, uint2 renderSize, float p00, float p11, float nearestViewZ, float farthestViewZ,
    out float3 aabbMin, out float3 aabbMax)
{
    TileViewSpaceAABBFromPixelOrigin(
        int2(tileCoord * kTileSize), renderSize, p00, p11, nearestViewZ, farthestViewZ, aabbMin, aabbMax);
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

// 境界球を求めたあとの判定だけを切り出したもの。平行光は距離減衰を持たず画面全体に届くため常に真。
//
// 【分けてある理由】候補プールの確率的バイリニア参照(MegaLightsInitialSample.hlsl)は
// **1灯につき4タイルぶん**判定する。境界球はタイルに依らないので、外で1回求めて使い回す。
// 分けずに IsLightVisibleInTile を4回呼ぶと、行列積を含む ComputeLightBoundingSphere を
// 4回やり直すことになる
bool IsLightSphereVisibleInTile(uint lightType, float3 viewCenter, float radius, TileFrustum frustum)
{
    if (lightType == 0u) // Directional
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

// そのライトがタイルへ届くか。
// viewCenter / radius は呼び出し側でも使えるようoutで返す(境界球を2回求めないため)
bool IsLightVisibleInTile(GPULight light, float4x4 view, TileFrustum frustum, out float3 viewCenter, out float radius)
{
    ComputeLightBoundingSphere(light, view, viewCenter, radius);
    return IsLightSphereVisibleInTile((uint)light.PositionType.w, viewCenter, radius, frustum);
}

// 届いたライトの重みに与える下限。
//
// 【これは効率の調整ではなく正しさの要件】重みが厳密に0になった灯は、そのタイルでは
// **どのピクセルからも決して選ばれない**。届いているのに選ばれない灯があると、
// 期待値が全灯評価と一致しなくなる(=バイアス)。距離減衰は Range の境界で厳密に0へ
// 落ちるため、AABBで距離を過大に見積もった場合などに0が出うる。届くと判定した灯には
// 必ず正の重みを与える
static const float kMinTileCandidateWeight = 1e-8f;

// 候補プールの重み w_j(y)。届かない灯は 0。
//
// 【この式は絶対に2か所へ書かないこと】書き手(MegaLightsTilePool.hlsl)がこれで重みを作り、
// 読み手(MegaLightsInitialSample.hlsl の確率的バイリニア参照)が**隣のタイルについて
// 同じ式を再計算**して提案分布 q_j(y) を組み立てる。ずれた瞬間に割り戻しが実際の抽出確率と
// 食い違い、絵は「それらしく」出たまま静かにバイアスが乗る。
// プールが invPdf ではなく w_j と SumW を別々に持っているのは、この再計算のため
// (MegaLightsTilePool.hlsl 冒頭)。
//
// 【法線を使ってはいけない】タイルの中でピクセルごとに法線が違うため、法線に依存した
// 重みにすると「代表法線からは見えないが、あるピクセルからは見える」灯を落としてしまう。
// ここで使ってよいのは、そのタイルのどのピクセルにも共通する量だけ。
//
// 【呼ぶ前に ComputeLightBoundingSphere で viewCenter / radius を求めておくこと】
float TileLightCandidateWeight(
    GPULight light, float3 viewCenter, float radius, TileFrustum frustum, float3 aabbMin, float3 aabbMax)
{
    const uint lightType = (uint)light.PositionType.w;
    if (!IsLightSphereVisibleInTile(lightType, viewCenter, radius, frustum))
    {
        return 0.0f;
    }

    // 相対輝度。ライトの色から「どれくらい効きそうか」を1つの数にする。
    // 【Luminance() を呼ばずに展開してあるのは include 順に依存させないため】
    // このヘッダーは GPULight にしか依存しないという契約を持っており、
    // Luminance を各 .hlsl が自前で定義している現状では呼び出せない
    const float intensity = dot(light.ColorRange.rgb, float3(0.2126f, 0.7152f, 0.0722f));

    float atten = 1.0f;
    if (lightType != 0u)
    {
        // タイルを包むAABB上で最も光源に近い点までの距離。錐台そのものより近く出る
        // (= 減衰を強めに見積もる)ので、届く灯を取りこぼす方向には倒れない
        const float3 closest = clamp(viewCenter, aabbMin, aabbMax);
        const float3 toLight = viewCenter - closest;
        // 【向きを使えないので上界を取る】ここは View 空間で、しかもタイルの中で
        // 画素ごとに位置も法線も違う。エミッシブ光源の余弦ローブは方向に依存するため、
        // 最大値((1-κ)/4 + κ)を掛ける ―― 過小に見積もると届く灯を取りこぼす
        atten = LightAttenuationUpperBound(
            lightType, dot(toLight, toLight), light.ColorRange.w, light.Params.z, light.Params.w);
    }

    return max(intensity * atten, kMinTileCandidateWeight);
}

#endif // KURENAI_TILE_LIGHT_CULLING_HLSLI
