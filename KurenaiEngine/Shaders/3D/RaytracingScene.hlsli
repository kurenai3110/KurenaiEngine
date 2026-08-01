// レイトレーシング用シーンジオメトリ(Assets::RaytracingScene)の共有ヘッダー。
//
// RayQueryが返すのは InstanceID / GeometryIndex / PrimitiveIndex / 重心座標 だけなので、
// そこから法線・マテリアルへたどり着くには統合バッファを何段か辿る必要がある
// (引き方の全体像はSource/Library/Assets/RaytracingScene.hの冒頭コメント)。
// この索引の辿り方とC++側の構造体の写しは、レイを撃つすべてのシェーダーで同一でなければ
// ならない(ずれるとまったく別の三角形の法線やマテリアルを読む)ため、
// ShadowSampling.hlsli / ReflectionProbe.hlsli と同じ方針で1か所に集約する。
//
// リソースのレジスタ番号はシェーダーごとに空きが違うため、インクルードする側が
// 以下のマクロを定義してからインクルードする(すべて必須):
//
//   KURENAI_RT_ATTRIBUTE_REGISTER     頂点属性(StructuredBuffer<RTVertexAttribute>)
//   KURENAI_RT_INDEX_REGISTER         インデックス(StructuredBuffer<uint>)
//   KURENAI_RT_MESHINFO_REGISTER      メッシュ情報(StructuredBuffer<RTMeshInfo>)
//   KURENAI_RT_INSTANCEINFO_REGISTER  インスタンス情報(StructuredBuffer<RTInstanceInfo>)
//   KURENAI_RT_MATERIAL_REGISTER      マテリアル(StructuredBuffer<RTMaterial>)
//
// このヘッダーはNormalEncoding.hlsliのOctDecodeを使うため、その後で#includeすること。
#ifndef KURENAI_RAYTRACING_SCENE_HLSLI
#define KURENAI_RAYTRACING_SCENE_HLSLI

// --- C++側の構造体の写し。並びとサイズを一致させること ---

// Assets::RaytracingVertexAttribute(16バイト)と1対1で対応
struct RTVertexAttribute
{
    float2 UV;
    uint PackedNormal; // オクタヘドラル+half2。NormalEncoding.hlsliのOctEncodeと同じ方式
    uint Padding;
};

// Assets::RaytracingMeshInfo(16バイト)
struct RTMeshInfo
{
    uint AttributeOffset;
    uint IndexOffset;
    uint MaterialIndex;
    uint Padding;
};

// Assets::RaytracingInstanceInfo(80バイト)
struct RTInstanceInfo
{
    float4x4 NormalMatrix; // モデルのローカル空間の法線 → ワールド空間
    uint MeshInfoOffset;
    uint3 Padding;
};

// Assets::RaytracingMaterial(48バイト)
struct RTMaterial
{
    float4 BaseColorFactor;
    float3 EmissiveFactor;
    float MetallicFactor;
    float RoughnessFactor;
    float AlphaCutoff;
    uint Flags;
    uint Padding;
};

StructuredBuffer<RTVertexAttribute> RTAttributes : register(KURENAI_RT_ATTRIBUTE_REGISTER);
StructuredBuffer<uint> RTIndices : register(KURENAI_RT_INDEX_REGISTER);
StructuredBuffer<RTMeshInfo> RTMeshInfos : register(KURENAI_RT_MESHINFO_REGISTER);
StructuredBuffer<RTInstanceInfo> RTInstanceInfos : register(KURENAI_RT_INSTANCEINFO_REGISTER);
StructuredBuffer<RTMaterial> RTMaterials : register(KURENAI_RT_MATERIAL_REGISTER);

// half2へ詰めたオクタヘドラル法線を復元する。CPU側のAssets::OctEncodeNormalの逆変換
float3 RTUnpackNormal(uint packed)
{
    return OctDecode(float2(f16tof32(packed & 0xFFFFu), f16tof32(packed >> 16)));
}

// ヒットした三角形の法線(ワールド空間)とマテリアルを取り出す。
// 法線は3頂点を重心座標で補間したもの(スムーズシェーディング)
void FetchHitSurface(uint instanceID, uint geometryIndex, uint primitiveIndex, float2 barycentrics,
                     out float3 worldNormal, out RTMaterial material)
{
    const RTInstanceInfo instanceInfo = RTInstanceInfos[instanceID];
    const RTMeshInfo meshInfo = RTMeshInfos[instanceInfo.MeshInfoOffset + geometryIndex];

    const uint indexBase = meshInfo.IndexOffset + primitiveIndex * 3u;
    const uint i0 = RTIndices[indexBase + 0u];
    const uint i1 = RTIndices[indexBase + 1u];
    const uint i2 = RTIndices[indexBase + 2u];

    const float3 n0 = RTUnpackNormal(RTAttributes[meshInfo.AttributeOffset + i0].PackedNormal);
    const float3 n1 = RTUnpackNormal(RTAttributes[meshInfo.AttributeOffset + i1].PackedNormal);
    const float3 n2 = RTUnpackNormal(RTAttributes[meshInfo.AttributeOffset + i2].PackedNormal);

    // RayQueryが返す重心座標は2成分(1つ目の頂点の重みは 1 - x - y)
    const float3 weights = float3(1.0f - barycentrics.x - barycentrics.y, barycentrics.x, barycentrics.y);
    const float3 localNormal = n0 * weights.x + n1 * weights.y + n2 * weights.z;

    // BLASの頂点はモデルのローカル空間のまま登録してあるため、法線もワールドへ移す必要がある。
    // NormalMatrixは転置済みでHLSLへ渡ってくるのでmul(vector, matrix)の順で掛ける
    worldNormal = normalize(mul(float4(localNormal, 0.0f), instanceInfo.NormalMatrix).xyz);
    material = RTMaterials[meshInfo.MaterialIndex];
}

// 遮蔽レイを1本撃ち、何かに当たれば true を返す。何に当たったかは問わないので
// 最初のヒットで打ち切ってよい(ACCEPT_FIRST_HIT_AND_END_SEARCH)。
// アルファテスト付きのジオメトリも不透明として扱う(RAY_FLAG_FORCE_OPAQUE。
// 正しく抜くにはヒット候補ごとにベースカラーをサンプルする必要があり bindless が要る)
bool TraceOcclusionRay(RaytracingAccelerationStructure tlas, float3 origin, float3 direction, float tMin, float tMax)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = tMin;
    ray.TMax = tMax;

    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
    query.TraceRayInline(tlas, RAY_FLAG_NONE, 0xFFu, ray);
    query.Proceed();

    return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

#endif // KURENAI_RAYTRACING_SCENE_HLSLI
