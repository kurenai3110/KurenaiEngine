// タイルベースのライトカリング。画面を16x16ピクセルのタイルに分け、タイルごとに
// 「そのタイルに届くライト」だけを集めたインデックスリストを作る。
// 直接光パス(DirectLighting.hlsl)はこのリストだけをループするため、シーン全体のライト数ではなく
// タイル内のライト数がピクセルあたりのコストになる。
//
// カリングは見た目を変えてはならない(スクリーンスペースシャドウと違い、これは純粋な最適化)。
// 検証では「カリング有無で最終画像が一致すること」を確認する(docs/Architecture.html 18章)。
//
// 【出力バッファのレイアウト】1本のRWStructuredBuffer<uint>に、タイルごとの固定長ブロックで詰める。
//   base = tileIndex * (1 + kMaxLightsPerTile)
//   LightTiles[base + 0]           = そのタイルに届いたライト数(容量超過時は超えた数がそのまま入る)
//   LightTiles[base + 1 + n]       = n番目のライトの、ライトリスト(t0)側でのインデックス
// タイルごとに固定容量を割り当てることで、グローバルなアトミックカウンタも
// 「カウンタを毎フレーム0クリアするパス」も不要になる。このエンジンにはバッファを
// クリアする手段(ClearUnorderedAccessViewUint相当)がRHIに無いため、これは実装上の要件でもある。
//
// 【個数を飽和させずに書く理由】容量を超えた場合も「実際に届いた数」をそのまま書き、
// 読み手側(DirectLighting.hlsl)がkMaxLightsPerTileで打ち切る。こうしておくと
// デバッグ表示(DebugView::LightTiles)が容量超過を検出でき、静かに欠落しない。

// タイルの1辺のピクセル数(kTileSize)とスレッド数(kTileThreadCount)、および
// 「そのライトがタイルへ届くか」の判定は TileLightCulling.hlsli にある
// (MegaLightsの候補プールと同じ判定を使うための共有ヘッダー。GPULightの宣言より後で読む)。

// 1タイルが保持できるライト数の上限。groupshared配列のサイズに使うためコンパイル時定数である必要がある。
// C++側 KurenaiEngine3D.cpp の kLightTileCapacity、および DirectLighting.hlsl の同名の定数と
// 必ず同じ値にすること(バッファのストライドがこの値で決まる)
static const uint kMaxLightsPerTile = 64u;

// C++側 KurenaiEngine3D.cpp の LightCullingConstants と並びを一致させること
cbuffer LightCullingConstants : register(b0)
{
    // ワールド座標をView空間へ変換する行列(ライトの位置をタイル錐台と同じ空間へ持ち込むため)
    float4x4 View;
    // x=タイル数X, y=タイル数Y, z=有効ライト数, w=1タイルあたりの容量(kMaxLightsPerTileと同じ値)
    uint4 TileParams;
    // x=レンダー解像度の幅, y=同 高さ, zw=未使用
    uint4 RenderSize;
    // x=射影行列の(0,0)成分, y=同(1,1)成分(NDC→View空間の錐台平面を組み立てるのに使う)、
    // z=深度リニアライズ定数a, w=同b(viewZ = b / (depth - a))
    float4 ProjParams;
};

// ライト1灯ぶんのデータ(struct GPULight)とライトリストの宣言は PunctualLighting.hlsli にある。
// このパスが要るのは構造体だけで、BRDFは使わないため KURENAI_PUNCTUAL_LIGHTING_BRDF は定義しない
// (定義するとPI・SpecularEnergy.hlsli・BRDFLUTTexture・ColorSamplerがすべて要求される)
#define KURENAI_PUNCTUAL_LIGHT_REGISTER t0
#include "PunctualLighting.hlsli"
// タイル錐台の組み立てと、ライトがタイルへ届くかの判定。GPULightを使うのでこの順で読む
#include "TileLightCulling.hlsli"
Texture2D<float> DepthTexture : register(t1);

RWStructuredBuffer<uint> LightTiles : register(u0);

// タイル内の深度範囲。Reverse-Zの深度値は[0,1]の非負floatなので、ビットパターンのまま
// uintとして比較しても大小関係が保たれる(InterlockedMin/Maxがfloatを扱えないための定番の手法)
groupshared uint gsMinDepthBits;
groupshared uint gsMaxDepthBits;
groupshared uint gsLightCount;
// 追加順(スレッドの実行順に依存するため非決定的)のライトインデックス
groupshared uint gsLightIndices[kMaxLightsPerTile];
// gsLightIndicesをライト番号の昇順へ並べ替えたもの。これを最終出力にする
groupshared uint gsSortedLightIndices[kMaxLightsPerTile];

[numthreads(16, 16, 1)]
void CSMain(uint3 groupID : SV_GroupID, uint3 dispatchThreadID : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex)
{
    const uint tileCountX = TileParams.x;
    const uint tileCountY = TileParams.y;
    const uint lightCount = TileParams.z;
    const uint tileCapacity = TileParams.w;

    if (groupID.x >= tileCountX || groupID.y >= tileCountY)
    {
        return;
    }

    const uint tileIndex = groupID.y * tileCountX + groupID.x;
    const uint tileBase = tileIndex * (1u + tileCapacity);

    if (groupIndex == 0u)
    {
        gsMinDepthBits = 0xFFFFFFFFu;
        gsMaxDepthBits = 0u;
        gsLightCount = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    // --- タイル内の深度範囲を求める ---
    // 背景(深度<=0、Reverse-Zの遠平面)はジオメトリが無いので範囲に含めない
    if (dispatchThreadID.x < RenderSize.x && dispatchThreadID.y < RenderSize.y)
    {
        const float depth = DepthTexture.Load(int3(dispatchThreadID.xy, 0));
        if (depth > 0.0f)
        {
            const uint depthBits = asuint(depth);
            InterlockedMin(gsMinDepthBits, depthBits);
            InterlockedMax(gsMaxDepthBits, depthBits);
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // タイル全体が背景ならライトは1灯も要らない
    if (gsMaxDepthBits == 0u)
    {
        if (groupIndex == 0u)
        {
            LightTiles[tileBase] = 0u;
        }
        return;
    }

    // Reverse-Zなので「深度値が大きい=手前」。最大深度が最も近いサーフェス、最小深度が最も遠いサーフェス
    const float nearestViewZ = TileViewZFromDepth(asfloat(gsMaxDepthBits), ProjParams.z, ProjParams.w);
    const float farthestViewZ = TileViewZFromDepth(asfloat(gsMinDepthBits), ProjParams.z, ProjParams.w);

    // タイルの視錐台(側面4枚+深度スラブ)。組み立ても判定もTileLightCulling.hlsliにあり、
    // MegaLightsの候補プールとまったく同じものを使う
    const TileFrustum frustum = MakeTileFrustum(
        groupID.xy, RenderSize.xy, ProjParams.x, ProjParams.y, nearestViewZ, farthestViewZ);

    // --- ライトをタイルへ振り分ける ---
    // スレッドgroupIndexが groupIndex, groupIndex+256, ... 番のライトを担当する
    for (uint lightIndex = groupIndex; lightIndex < lightCount; lightIndex += kTileThreadCount)
    {
        const GPULight light = Lights[lightIndex];

        float3 viewCenter;
        float radius;
        const bool visible = IsLightVisibleInTile(light, View, frustum, viewCenter, radius);

        if (visible)
        {
            uint writeIndex;
            InterlockedAdd(gsLightCount, 1u, writeIndex);
            if (writeIndex < kMaxLightsPerTile)
            {
                gsLightIndices[writeIndex] = lightIndex;
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();

    const uint rawCount = gsLightCount;
    const uint storedCount = min(rawCount, kMaxLightsPerTile);

    // --- 追加順をライト番号の昇順へ並べ替える ---
    // InterlockedAddの戻り値はスレッドの実行順に依存するため、並べ替えないとタイルごと・
    // フレームごとにリストの順序が変わる。順序が変わると、スクリーンスペースシャドウの
    // 「ピクセルあたりのシャドウレイ数の上限」でどのライトが選ばれるかが揺れて影がちらつく。
    // 要素数が最大64と少ないので、各要素の順位(自分より小さい要素の個数)を数える単純な方法で足りる
    // (要素は必ず相異なるライト番号なので順位も一意になる)
    for (uint sortIndex = groupIndex; sortIndex < storedCount; sortIndex += kTileThreadCount)
    {
        const uint value = gsLightIndices[sortIndex];
        uint rank = 0u;
        for (uint compareIndex = 0u; compareIndex < storedCount; ++compareIndex)
        {
            if (gsLightIndices[compareIndex] < value)
            {
                ++rank;
            }
        }
        gsSortedLightIndices[rank] = value;
    }
    GroupMemoryBarrierWithGroupSync();

    // --- 書き出し ---
    for (uint writeIndex = groupIndex; writeIndex < storedCount; writeIndex += kTileThreadCount)
    {
        LightTiles[tileBase + 1u + writeIndex] = gsSortedLightIndices[writeIndex];
    }
    if (groupIndex == 0u)
    {
        // 飽和させずに実際の数を書く(読み手が容量で打ち切る)。デバッグ表示が容量超過を検出できる
        LightTiles[tileBase] = rawCount;
    }
}
