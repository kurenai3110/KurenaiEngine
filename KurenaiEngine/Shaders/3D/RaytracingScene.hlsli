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

// ヒット面のマテリアルテクスチャをbindlessで引くために必要(FetchHitSurface参照)。
// どちらもインクルードガードを持つため、消費側が先に取り込んでいても二重定義にならない
#include "Bindless.hlsli"
#include "Samplers.hlsli"

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

// Assets::RaytracingMaterial(64バイト)。
// 末尾4つはbindlessディスクリプタ番号で、kInvalidBindlessIndexなら「テクスチャ無し」
struct RTMaterial
{
    float4 BaseColorFactor;
    float3 EmissiveFactor;
    float MetallicFactor;
    float RoughnessFactor;
    float AlphaCutoff;
    uint Flags;
    uint BaseColorTextureIndex;
    // 【現状このシェーダーは読まない】法線マップを適用するには接空間の基底が要るが、
    // RTVertexAttributeはまだ接線を持っていない(16バイトに抑えるため法線とUVだけ。
    // 接線を足す場所としてPaddingが予約されている)。番号だけ先に運んでおき、
    // 接線を足したときにここを読むだけで済むようにしてある
    uint NormalTextureIndex;
    uint MetallicRoughnessTextureIndex;
    uint EmissiveTextureIndex;
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
// 法線は3頂点を重心座標で補間したもの(スムーズシェーディング)。
//
// 【マテリアルのテクスチャはここで係数へ畳み込む】戻り値のRTMaterialは、
// BaseColorFactor / MetallicFactor / RoughnessFactor / EmissiveFactor に
// 「定数の係数 × ヒット点のテクスチャの値」を入れて返す。
// こうすることで消費側(RTReflection / RTAO)は今までどおり係数を読むだけでよく、
// テクスチャの有無を意識しなくて済む。bindless非対応の環境では
// BindlessSampleLevelが白1x1相当を返すため、結果は従来と完全に同じになる
void FetchHitSurface(uint instanceID, uint geometryIndex, uint primitiveIndex, float2 barycentrics,
                     out float3 worldNormal, out RTMaterial material)
{
    const RTInstanceInfo instanceInfo = RTInstanceInfos[instanceID];
    const RTMeshInfo meshInfo = RTMeshInfos[instanceInfo.MeshInfoOffset + geometryIndex];

    const uint indexBase = meshInfo.IndexOffset + primitiveIndex * 3u;
    const uint i0 = RTIndices[indexBase + 0u];
    const uint i1 = RTIndices[indexBase + 1u];
    const uint i2 = RTIndices[indexBase + 2u];

    const RTVertexAttribute a0 = RTAttributes[meshInfo.AttributeOffset + i0];
    const RTVertexAttribute a1 = RTAttributes[meshInfo.AttributeOffset + i1];
    const RTVertexAttribute a2 = RTAttributes[meshInfo.AttributeOffset + i2];

    // RayQueryが返す重心座標は2成分(1つ目の頂点の重みは 1 - x - y)
    const float3 weights = float3(1.0f - barycentrics.x - barycentrics.y, barycentrics.x, barycentrics.y);
    const float3 localNormal = RTUnpackNormal(a0.PackedNormal) * weights.x +
                               RTUnpackNormal(a1.PackedNormal) * weights.y +
                               RTUnpackNormal(a2.PackedNormal) * weights.z;
    const float2 uv = a0.UV * weights.x + a1.UV * weights.y + a2.UV * weights.z;

    // BLASの頂点はモデルのローカル空間のまま登録してあるため、法線もワールドへ移す必要がある。
    // NormalMatrixは転置済みでHLSLへ渡ってくるのでmul(vector, matrix)の順で掛ける
    worldNormal = normalize(mul(float4(localNormal, 0.0f), instanceInfo.NormalMatrix).xyz);
    material = RTMaterials[meshInfo.MaterialIndex];

    // 【LOD 0で引く】レイのヒット点には隣接ピクセルとのUV勾配が無く、暗黙のミップ選択ができない。
    // 距離に応じたLODを推定する方法もあるが、反射・GIに映る面は元々ラフネスでぼかされるうえ、
    // 誤ったLODで縮小されるより最大解像度で引くほうが破綻が少ないため0で固定する
    const float kHitLod = 0.0f;
    const float4 kWhite = float4(1.0f, 1.0f, 1.0f, 1.0f);

    material.BaseColorFactor *= BindlessSampleLevel(material.BaseColorTextureIndex, MaterialSampler, uv, kHitLod, kWhite);
    material.EmissiveFactor *= BindlessSampleLevel(material.EmissiveTextureIndex, MaterialSampler, uv, kHitLod, kWhite).rgb;

    // metallic/roughnessはテクスチャがあるときだけ畳み込む。
    // 【なぜ分岐が要るのか】この2つの係数は「ソースデータに値が無い」ことを負値
    // (Assets::kInvalidMaterialFactor)で表しており、その場合の解釈は
    // 「係数1.0 = テクスチャの値をそのまま使う」と決まっている(ModelPackage.h)。
    // テクスチャが無いのに負値を1.0へ丸めると、データを持たないマテリアルが
    // 一律メタリック1.0になってしまい従来の見た目を壊す。テクスチャがある場合に限り
    // glTFと同じ「係数×テクスチャ」の合成を行う
    if (material.MetallicRoughnessTextureIndex != kInvalidBindlessIndex)
    {
        const float4 mr = BindlessSampleLevel(material.MetallicRoughnessTextureIndex, MaterialSampler, uv, kHitLod, kWhite);
        // glTFの規約どおり b=metallic、g=roughness
        material.MetallicFactor = (material.MetallicFactor < 0.0f ? 1.0f : material.MetallicFactor) * mr.b;
        material.RoughnessFactor = (material.RoughnessFactor < 0.0f ? 1.0f : material.RoughnessFactor) * mr.g;
    }
}

// 遮蔽レイを1本撃ち、何かに当たれば true を返す。何に当たったかは問わないので
// 最初のヒットで打ち切ってよい(ACCEPT_FIRST_HIT_AND_END_SEARCH)。
// アルファテスト付きのジオメトリも不透明として扱う(RAY_FLAG_FORCE_OPAQUE)。
//
// 【bindlessが入った今も抜いていない】正しく抜くにはRayQuery::Proceed()のループを回し、
// ヒット候補ごとにベースカラーをサンプルしてAlphaCutoffと比較する必要がある。
// bindlessが無かった頃はそもそも書けなかったが、現在は書ける。それでも入れていないのは、
// 影レイは1ピクセルあたり複数本撃つうえ「最初のヒットで打ち切る」最適化が効かなくなり、
// 葉や柵の影の精度に対して代償が大きいため。入れる場合はここを起点にする
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
