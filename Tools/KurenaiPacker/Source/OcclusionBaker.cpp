#include "OcclusionBaker.h"

#include <Windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <xatlas.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <numeric>
#include <stdexcept>

#include "Core/StringUtil.h"

using Kurenai::Assets::Vertex;
using Kurenai::Core::WideToUtf8;
using Microsoft::WRL::ComPtr;

namespace KurenaiPacker
{
    namespace
    {
        void Warn(const std::string& message)
        {
            std::cerr << "[KurenaiPacker][Warning] " << message << "\n";
        }

        void Info(const std::string& message)
        {
            std::cout << "[KurenaiPacker] " << message << "\n";
        }

        // === BVH ===============================================================
        //
        // 中央値分割の単純なBVH。SAHは使わない ―― ベイクは1アセットにつき数回しか走らない
        // オフライン処理で、構築時間よりも実装の単純さ(=検証しやすさ)を優先したため。
        // レイあたりの交差判定回数は実測でも問題にならなかった。

        struct Float3
        {
            float x = 0.0f, y = 0.0f, z = 0.0f;
        };

        Float3 Sub(const Float3& a, const Float3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
        Float3 Min3(const Float3& a, const Float3& b) { return { std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z) }; }
        Float3 Max3(const Float3& a, const Float3& b) { return { std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z) }; }

        // GPUへ渡す三角形。交差判定(Möller–Trumbore)がそのまま使えるよう辺ベクトルで持つ
        struct GpuTriangle
        {
            float V0[3];
            float E1[3];
            float E2[3];
        };
        static_assert(sizeof(GpuTriangle) == 36, "GpuTriangleはHLSL側の構造体と36バイトで一致させること");

        // count > 0 ならリーフで、LeftFirstは三角形配列の開始位置。
        // count == 0 なら内部ノードで、LeftFirstは左の子のノード番号(右の子は左+1)
        struct GpuBvhNode
        {
            float BoundsMin[3];
            uint32_t LeftFirst;
            float BoundsMax[3];
            uint32_t Count;
        };
        static_assert(sizeof(GpuBvhNode) == 32, "GpuBvhNodeはHLSL側の構造体と32バイトで一致させること");

        struct BvhBuildTriangle
        {
            Float3 V0, V1, V2;
            Float3 Centroid;
            Float3 BoundsMin, BoundsMax;
        };

        constexpr uint32_t kBvhLeafSize = 4;

        void BuildBvhRecursive(
            std::vector<BvhBuildTriangle>& triangles,
            std::vector<uint32_t>& order,
            std::vector<GpuBvhNode>& nodes,
            uint32_t nodeIndex,
            uint32_t first,
            uint32_t count)
        {
            GpuBvhNode& node = nodes[nodeIndex];

            Float3 bmin{ FLT_MAX, FLT_MAX, FLT_MAX };
            Float3 bmax{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
            for (uint32_t i = 0; i < count; ++i)
            {
                const BvhBuildTriangle& t = triangles[order[first + i]];
                bmin = Min3(bmin, t.BoundsMin);
                bmax = Max3(bmax, t.BoundsMax);
            }
            node.BoundsMin[0] = bmin.x; node.BoundsMin[1] = bmin.y; node.BoundsMin[2] = bmin.z;
            node.BoundsMax[0] = bmax.x; node.BoundsMax[1] = bmax.y; node.BoundsMax[2] = bmax.z;

            if (count <= kBvhLeafSize)
            {
                node.LeftFirst = first;
                node.Count = count;
                return;
            }

            // 最も広がっている軸で中央値分割する
            const Float3 extent = Sub(bmax, bmin);
            int axis = 0;
            if (extent.y > extent.x) { axis = 1; }
            if (extent.z > (axis == 0 ? extent.x : extent.y)) { axis = 2; }

            const auto centroidAxis = [&](uint32_t triIndex) -> float
            {
                const Float3& c = triangles[triIndex].Centroid;
                return axis == 0 ? c.x : (axis == 1 ? c.y : c.z);
            };

            const uint32_t mid = count / 2;
            std::nth_element(
                order.begin() + first,
                order.begin() + first + mid,
                order.begin() + first + count,
                [&](uint32_t a, uint32_t b) { return centroidAxis(a) < centroidAxis(b); });

            const uint32_t leftChild = static_cast<uint32_t>(nodes.size());
            nodes.emplace_back();
            nodes.emplace_back();

            // nodesのreallocでnodeの参照が無効になるため、書き込みは添字経由で行う
            nodes[nodeIndex].LeftFirst = leftChild;
            nodes[nodeIndex].Count = 0;

            BuildBvhRecursive(triangles, order, nodes, leftChild, first, mid);
            BuildBvhRecursive(triangles, order, nodes, leftChild + 1, first + mid, count - mid);
        }

        struct Bvh
        {
            std::vector<GpuBvhNode> Nodes;
            std::vector<GpuTriangle> Triangles;
        };

        // モデル全体(全メッシュ)の三角形からBVHを構築する。
        // メッシュ単位ではなくモデル全体で作るのは、あるメッシュのAOに別のメッシュ
        // (床に対する壁など)の遮蔽が要るため
        Bvh BuildBvh(const SourceModel& model)
        {
            std::vector<BvhBuildTriangle> build;
            for (const SourceMesh& mesh : model.Meshes)
            {
                for (size_t i = 0; i + 2 < mesh.Indices.size(); i += 3)
                {
                    const Vertex& a = mesh.Vertices[mesh.Indices[i + 0]];
                    const Vertex& b = mesh.Vertices[mesh.Indices[i + 1]];
                    const Vertex& c = mesh.Vertices[mesh.Indices[i + 2]];

                    BvhBuildTriangle t;
                    t.V0 = { a.Position[0], a.Position[1], a.Position[2] };
                    t.V1 = { b.Position[0], b.Position[1], b.Position[2] };
                    t.V2 = { c.Position[0], c.Position[1], c.Position[2] };
                    t.Centroid = {
                        (t.V0.x + t.V1.x + t.V2.x) / 3.0f,
                        (t.V0.y + t.V1.y + t.V2.y) / 3.0f,
                        (t.V0.z + t.V1.z + t.V2.z) / 3.0f };
                    t.BoundsMin = Min3(Min3(t.V0, t.V1), t.V2);
                    t.BoundsMax = Max3(Max3(t.V0, t.V1), t.V2);
                    build.push_back(t);
                }
            }

            Bvh bvh;
            if (build.empty())
            {
                return bvh;
            }

            std::vector<uint32_t> order(build.size());
            std::iota(order.begin(), order.end(), 0u);

            bvh.Nodes.reserve(build.size() * 2);
            bvh.Nodes.emplace_back();
            BuildBvhRecursive(build, order, bvh.Nodes, 0, 0, static_cast<uint32_t>(build.size()));

            // リーフが参照する順序どおりに三角形を並べ替えて、GPU側は連番で読めるようにする
            bvh.Triangles.resize(build.size());
            for (size_t i = 0; i < order.size(); ++i)
            {
                const BvhBuildTriangle& t = build[order[i]];
                GpuTriangle& g = bvh.Triangles[i];
                g.V0[0] = t.V0.x; g.V0[1] = t.V0.y; g.V0[2] = t.V0.z;
                g.E1[0] = t.V1.x - t.V0.x; g.E1[1] = t.V1.y - t.V0.y; g.E1[2] = t.V1.z - t.V0.z;
                g.E2[0] = t.V2.x - t.V0.x; g.E2[1] = t.V2.y - t.V0.y; g.E2[2] = t.V2.z - t.V0.z;
            }
            return bvh;
        }

        // === ライトマップUVの生成(xatlas) =====================================

        // xatlasの進捗・ログはこのツールの出力形式に合わせて捨てる(既定ではstdoutへ出る)
        int SilentPrint(const char*, ...)
        {
            return 0;
        }

        // メッシュ1つを展開し、Vertices/IndicesをUV1付きで置き換える。
        // 展開できなかった場合はfalseを返し、メッシュは元のまま(UV1=UVのコピー)で残す
        bool UnwrapMesh(SourceMesh& mesh, uint32_t resolution)
        {
            if (mesh.Indices.empty() || mesh.Vertices.empty())
            {
                return false;
            }

            xatlas::Atlas* atlas = xatlas::Create();
            if (atlas == nullptr)
            {
                return false;
            }

            struct AtlasGuard
            {
                xatlas::Atlas* A;
                ~AtlasGuard() { xatlas::Destroy(A); }
            } guard{ atlas };

            xatlas::MeshDecl decl;
            decl.vertexCount = static_cast<uint32_t>(mesh.Vertices.size());
            decl.vertexPositionData = mesh.Vertices.data()->Position;
            decl.vertexPositionStride = sizeof(Vertex);
            decl.vertexNormalData = mesh.Vertices.data()->Normal;
            decl.vertexNormalStride = sizeof(Vertex);
            // 元のUVはチャート分割のヒントとしてのみ渡す(useInputMeshUvsは立てない。
            // タイリングされたUVをそのままチャートに使うと重なりが残ってしまうため)
            decl.vertexUvData = mesh.Vertices.data()->UV;
            decl.vertexUvStride = sizeof(Vertex);
            decl.indexCount = static_cast<uint32_t>(mesh.Indices.size());
            decl.indexData = mesh.Indices.data();
            decl.indexFormat = xatlas::IndexFormat::UInt32;

            if (xatlas::AddMesh(atlas, decl) != xatlas::AddMeshError::Success)
            {
                return false;
            }

            xatlas::ComputeCharts(atlas);

            xatlas::PackOptions packOptions;
            packOptions.resolution = resolution;
            packOptions.padding = 2;        // バイリニア補間で隣のチャートを拾わないための余白
            packOptions.bilinear = true;
            packOptions.blockAlign = true;  // BC4の4x4ブロックとチャート境界を揃える
            xatlas::PackCharts(atlas, packOptions);

            // 1枚に収まらず複数アトラスへ分かれた場合、UVだけではどのアトラスかを表現できない。
            // texelsPerUnitを下げて(=チャートを小さくして)1枚に収まるまで詰め直す
            for (int attempt = 0; attempt < 4 && atlas->atlasCount > 1; ++attempt)
            {
                packOptions.texelsPerUnit = atlas->texelsPerUnit * 0.7f;
                xatlas::PackCharts(atlas, packOptions);
            }
            if (atlas->atlasCount > 1)
            {
                return false;
            }
            if (atlas->meshCount == 0 || atlas->width == 0 || atlas->height == 0)
            {
                return false;
            }

            const xatlas::Mesh& out = atlas->meshes[0];

            std::vector<Vertex> newVertices(out.vertexCount);
            for (uint32_t i = 0; i < out.vertexCount; ++i)
            {
                const xatlas::Vertex& ov = out.vertexArray[i];
                // xrefは展開前の頂点番号。位置・法線・UV・接線はそこからそのまま引き継ぐ
                newVertices[i] = mesh.Vertices[ov.xref];
                newVertices[i].UV1[0] = ov.uv[0] / static_cast<float>(atlas->width);
                newVertices[i].UV1[1] = ov.uv[1] / static_cast<float>(atlas->height);
            }

            std::vector<uint32_t> newIndices(out.indexCount);
            std::memcpy(newIndices.data(), out.indexArray, out.indexCount * sizeof(uint32_t));

            mesh.Vertices = std::move(newVertices);
            mesh.Indices = std::move(newIndices);
            return true;
        }

        // === ライトマップUV空間のラスタライズ(CPU) ============================
        //
        // 各テクセルが表す面上の位置と法線を求める。GPUでラスタライズしてもよいが、
        // 解像度が512程度でメッシュ数も高々数百なのでCPUで十分速く、
        // レンダーターゲットの往復が不要なぶん実装も検証も単純になる。
        // 重いのはこの後のレイキャスト(テクセル数 × レイ本数)であり、そちらをGPUへ載せる

        struct BakeTexel
        {
            float Position[3];
            float Normal[3];
        };

        // 有効なテクセルはvalidに1が入る
        void RasterizeLightmapSpace(
            const SourceMesh& mesh,
            uint32_t resolution,
            std::vector<BakeTexel>& outTexels,
            std::vector<uint8_t>& outValid)
        {
            outTexels.assign(static_cast<size_t>(resolution) * resolution, BakeTexel{});
            outValid.assign(static_cast<size_t>(resolution) * resolution, 0);

            const float res = static_cast<float>(resolution);

            for (size_t i = 0; i + 2 < mesh.Indices.size(); i += 3)
            {
                const Vertex& a = mesh.Vertices[mesh.Indices[i + 0]];
                const Vertex& b = mesh.Vertices[mesh.Indices[i + 1]];
                const Vertex& c = mesh.Vertices[mesh.Indices[i + 2]];

                // テクセル中心を基準にするため-0.5しておく
                const float ax = a.UV1[0] * res - 0.5f, ay = a.UV1[1] * res - 0.5f;
                const float bx = b.UV1[0] * res - 0.5f, by = b.UV1[1] * res - 0.5f;
                const float cx = c.UV1[0] * res - 0.5f, cy = c.UV1[1] * res - 0.5f;

                const float area = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay);
                if (std::fabs(area) < 1e-12f)
                {
                    continue;
                }
                const float invArea = 1.0f / area;

                int minX = static_cast<int>(std::floor(std::min({ ax, bx, cx })));
                int maxX = static_cast<int>(std::ceil(std::max({ ax, bx, cx })));
                int minY = static_cast<int>(std::floor(std::min({ ay, by, cy })));
                int maxY = static_cast<int>(std::ceil(std::max({ ay, by, cy })));
                minX = std::max(minX, 0); minY = std::max(minY, 0);
                maxX = std::min(maxX, static_cast<int>(resolution) - 1);
                maxY = std::min(maxY, static_cast<int>(resolution) - 1);

                for (int y = minY; y <= maxY; ++y)
                {
                    for (int x = minX; x <= maxX; ++x)
                    {
                        const float px = static_cast<float>(x);
                        const float py = static_cast<float>(y);

                        float w0 = ((bx - px) * (cy - py) - (cx - px) * (by - py)) * invArea;
                        float w1 = ((cx - px) * (ay - py) - (ax - px) * (cy - py)) * invArea;
                        float w2 = 1.0f - w0 - w1;

                        // チャートの縁でテクセル中心が三角形の外に落ちることがある。
                        // そこを空けると継ぎ目に黒い筋が出るため、わずかな外側までは拾う
                        constexpr float kEdgeTolerance = -0.02f;
                        if (w0 < kEdgeTolerance || w1 < kEdgeTolerance || w2 < kEdgeTolerance)
                        {
                            continue;
                        }

                        const size_t index = static_cast<size_t>(y) * resolution + x;
                        if (outValid[index])
                        {
                            continue; // 先に書かれた三角形を優先(チャート境界での上書きを避ける)
                        }

                        BakeTexel& texel = outTexels[index];
                        for (int k = 0; k < 3; ++k)
                        {
                            texel.Position[k] = a.Position[k] * w0 + b.Position[k] * w1 + c.Position[k] * w2;
                            texel.Normal[k] = a.Normal[k] * w0 + b.Normal[k] * w1 + c.Normal[k] * w2;
                        }
                        const float len = std::sqrt(
                            texel.Normal[0] * texel.Normal[0] +
                            texel.Normal[1] * texel.Normal[1] +
                            texel.Normal[2] * texel.Normal[2]);
                        if (len > 1e-8f)
                        {
                            texel.Normal[0] /= len; texel.Normal[1] /= len; texel.Normal[2] /= len;
                            outValid[index] = 1;
                        }
                    }
                }
            }
        }

        // === 継ぎ目のにじみ対策(ダイレーション) ================================
        //
        // 無効テクセル(チャートの外)は初期値0=真っ黒。そのままバイリニア補間やミップ生成に
        // かけるとチャートの縁が黒く沈むため、有効テクセルの値を外側へ広げておく
        void Dilate(std::vector<uint8_t>& values, std::vector<uint8_t> valid, uint32_t resolution, uint32_t pixels)
        {
            const int res = static_cast<int>(resolution);
            for (uint32_t pass = 0; pass < pixels; ++pass)
            {
                std::vector<uint8_t> nextValid = valid;
                std::vector<uint8_t> nextValues = values;
                bool changed = false;

                for (int y = 0; y < res; ++y)
                {
                    for (int x = 0; x < res; ++x)
                    {
                        const size_t index = static_cast<size_t>(y) * res + x;
                        if (valid[index])
                        {
                            continue;
                        }
                        int sum = 0;
                        int count = 0;
                        for (int dy = -1; dy <= 1; ++dy)
                        {
                            for (int dx = -1; dx <= 1; ++dx)
                            {
                                const int nx = x + dx, ny = y + dy;
                                if (nx < 0 || ny < 0 || nx >= res || ny >= res)
                                {
                                    continue;
                                }
                                const size_t n = static_cast<size_t>(ny) * res + nx;
                                if (valid[n])
                                {
                                    sum += values[n];
                                    ++count;
                                }
                            }
                        }
                        if (count > 0)
                        {
                            nextValues[index] = static_cast<uint8_t>(sum / count);
                            nextValid[index] = 1;
                            changed = true;
                        }
                    }
                }

                values.swap(nextValues);
                valid.swap(nextValid);
                if (!changed)
                {
                    break;
                }
            }

            // 最後まで埋まらなかったテクセル(チャートから遠い空白)は「遮蔽なし」にしておく。
            // 黒のままだとミップの上位段で周囲へ滲み出て、面全体が不必要に暗くなる
            for (size_t i = 0; i < values.size(); ++i)
            {
                if (!valid[i])
                {
                    values[i] = 255;
                }
            }
        }

        // === GPU(DirectCompute)によるレイキャスト ==============================

        const char* kBakeComputeShader = R"(
struct Triangle { float3 V0; float3 E1; float3 E2; };
struct BvhNode  { float3 BoundsMin; uint LeftFirst; float3 BoundsMax; uint Count; };
struct Texel    { float3 Position; float3 Normal; };

StructuredBuffer<Triangle> Triangles : register(t0);
StructuredBuffer<BvhNode>  Nodes     : register(t1);
StructuredBuffer<Texel>    Texels    : register(t2);
StructuredBuffer<uint>     Valid     : register(t3);
RWStructuredBuffer<float>  Result    : register(u0);

cbuffer BakeConstants : register(b0)
{
    uint TexelCount;
    uint RayCount;
    float RayLength;
    float NormalOffset;
};

// Möller–Trumbore。遮蔽の有無だけが要るので交差位置は返さない
bool IntersectTriangle(float3 origin, float3 dir, Triangle tri, float maxT)
{
    const float3 pv = cross(dir, tri.E2);
    const float det = dot(tri.E1, pv);
    if (abs(det) < 1e-8f) { return false; }
    const float invDet = 1.0f / det;
    const float3 tv = origin - tri.V0;
    const float u = dot(tv, pv) * invDet;
    if (u < 0.0f || u > 1.0f) { return false; }
    const float3 qv = cross(tv, tri.E1);
    const float v = dot(dir, qv) * invDet;
    if (v < 0.0f || u + v > 1.0f) { return false; }
    const float t = dot(tri.E2, qv) * invDet;
    return t > 1e-4f && t < maxT;
}

bool IntersectAabb(float3 origin, float3 invDir, float3 bmin, float3 bmax, float maxT)
{
    const float3 t0 = (bmin - origin) * invDir;
    const float3 t1 = (bmax - origin) * invDir;
    const float3 tsmall = min(t0, t1);
    const float3 tbig = max(t0, t1);
    const float tmin = max(max(tsmall.x, tsmall.y), tsmall.z);
    const float tmax = min(min(tbig.x, tbig.y), tbig.z);
    return tmax >= max(tmin, 0.0f) && tmin < maxT;
}

// 遮蔽されていれば true。スタックを明示的に持つ非再帰トラバーサル
bool Occluded(float3 origin, float3 dir, float maxT)
{
    const float3 invDir = 1.0f / (dir + (abs(dir) < 1e-8f ? 1e-8f : 0.0f));

    uint stack[32];
    int stackSize = 0;
    stack[stackSize++] = 0;

    [loop]
    while (stackSize > 0)
    {
        const BvhNode node = Nodes[stack[--stackSize]];
        if (!IntersectAabb(origin, invDir, node.BoundsMin, node.BoundsMax, maxT))
        {
            continue;
        }
        if (node.Count > 0)
        {
            for (uint i = 0; i < node.Count; ++i)
            {
                if (IntersectTriangle(origin, dir, Triangles[node.LeftFirst + i], maxT))
                {
                    return true;
                }
            }
        }
        else if (stackSize < 30)
        {
            stack[stackSize++] = node.LeftFirst;
            stack[stackSize++] = node.LeftFirst + 1;
        }
    }
    return false;
}

// Hammersley列。低食い違い量列なので、同じ本数でも一様乱数よりノイズが少ない
float2 Hammersley(uint i, uint n)
{
    uint bits = i;
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float2(float(i) / float(n), float(bits) * 2.3283064365386963e-10f);
}

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const uint index = id.x;
    if (index >= TexelCount) { return; }

    if (Valid[index] == 0)
    {
        Result[index] = 1.0f; // チャート外は遮蔽なし扱い(ダイレーションで上書きされる)
        return;
    }

    const Texel texel = Texels[index];
    const float3 N = normalize(texel.Normal);

    // 法線まわりの正規直交基底
    const float3 up = abs(N.z) < 0.999f ? float3(0, 0, 1) : float3(1, 0, 0);
    const float3 T = normalize(cross(up, N));
    const float3 B = cross(N, T);

    // 自分自身との交差を避けるため、法線方向へわずかに浮かせる
    const float3 origin = texel.Position + N * NormalOffset;

    uint occluded = 0;
    for (uint i = 0; i < RayCount; ++i)
    {
        const float2 xi = Hammersley(i, RayCount);
        // コサイン重点サンプリング。AOの定義(コサイン項つきの可視率の積分)に対して
        // 重みが打ち消し合うため、ヒット数を数えるだけで正しい推定値になる
        const float r = sqrt(xi.x);
        const float phi = 6.2831853f * xi.y;
        const float3 local = float3(r * cos(phi), r * sin(phi), sqrt(max(0.0f, 1.0f - xi.x)));
        const float3 dir = normalize(T * local.x + B * local.y + N * local.z);

        if (Occluded(origin, dir, RayLength))
        {
            ++occluded;
        }
    }

    Result[index] = 1.0f - float(occluded) / float(RayCount);
}
)";

        struct BakeConstants
        {
            uint32_t TexelCount;
            uint32_t RayCount;
            float RayLength;
            float NormalOffset;
        };

        void ThrowIfFailed(HRESULT hr, const char* what)
        {
            if (FAILED(hr))
            {
                char buffer[64];
                sprintf_s(buffer, " (HRESULT: 0x%08X)", static_cast<unsigned int>(hr));
                throw std::runtime_error(std::string(what) + buffer);
            }
        }

        // 遮蔽マップのベイクに使うD3D11デバイス一式。ベイク全体で1つ作って使い回す
        class BakeDevice
        {
        public:
            BakeDevice()
            {
                UINT flags = 0;
                const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
                D3D_FEATURE_LEVEL obtained{};
                // ハードウェアが使えない環境ではWARP(ソフトウェアラスタライザ)へ落とす。
                // 遅くはなるがベイク自体は成立し、CIのようなGPU無し環境でも動く
                HRESULT hr = D3D11CreateDevice(
                    nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                    levels, 1, D3D11_SDK_VERSION, &m_Device, &obtained, &m_Context);
                if (FAILED(hr))
                {
                    Warn("遮蔽マップのベイク用GPUデバイスを作成できなかったため、WARP(ソフトウェア)で続行します");
                    hr = D3D11CreateDevice(
                        nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
                        levels, 1, D3D11_SDK_VERSION, &m_Device, &obtained, &m_Context);
                }
                ThrowIfFailed(hr, "遮蔽マップのベイク用D3D11デバイスの作成に失敗しました");

                ComPtr<ID3DBlob> code;
                ComPtr<ID3DBlob> errors;
                hr = D3DCompile(
                    kBakeComputeShader, std::strlen(kBakeComputeShader), "OcclusionBake.hlsl",
                    nullptr, nullptr, "CSMain", "cs_5_0", 0, 0, &code, &errors);
                if (FAILED(hr))
                {
                    const std::string detail = errors
                        ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize())
                        : std::string();
                    throw std::runtime_error("遮蔽マップのベイク用コンピュートシェーダーのコンパイルに失敗しました: " + detail);
                }
                ThrowIfFailed(
                    m_Device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &m_Shader),
                    "遮蔽マップのベイク用コンピュートシェーダーの作成に失敗しました");

                D3D11_BUFFER_DESC cbDesc{};
                cbDesc.ByteWidth = sizeof(BakeConstants);
                cbDesc.Usage = D3D11_USAGE_DYNAMIC;
                cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                ThrowIfFailed(m_Device->CreateBuffer(&cbDesc, nullptr, &m_Constants), "定数バッファの作成に失敗しました");
            }

            ID3D11Device* Device() const { return m_Device.Get(); }
            ID3D11DeviceContext* Context() const { return m_Context.Get(); }
            ID3D11ComputeShader* Shader() const { return m_Shader.Get(); }
            ID3D11Buffer* Constants() const { return m_Constants.Get(); }

        private:
            ComPtr<ID3D11Device> m_Device;
            ComPtr<ID3D11DeviceContext> m_Context;
            ComPtr<ID3D11ComputeShader> m_Shader;
            ComPtr<ID3D11Buffer> m_Constants;
        };

        ComPtr<ID3D11ShaderResourceView> CreateStructuredSrv(
            ID3D11Device* device, const void* data, uint32_t stride, uint32_t count, ComPtr<ID3D11Buffer>& outBuffer)
        {
            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = stride * count;
            desc.Usage = D3D11_USAGE_IMMUTABLE;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            desc.StructureByteStride = stride;

            D3D11_SUBRESOURCE_DATA init{};
            init.pSysMem = data;
            ThrowIfFailed(device->CreateBuffer(&desc, &init, &outBuffer), "構造化バッファの作成に失敗しました");

            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = DXGI_FORMAT_UNKNOWN;
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            srvDesc.Buffer.NumElements = count;

            ComPtr<ID3D11ShaderResourceView> srv;
            ThrowIfFailed(device->CreateShaderResourceView(outBuffer.Get(), &srvDesc, &srv), "SRVの作成に失敗しました");
            return srv;
        }
    }

    OcclusionBakeResult BakeOcclusion(SourceModel& sourceModel, const OcclusionBakeOptions& options)
    {
        OcclusionBakeResult result;
        result.Resolution = options.Resolution;
        result.MeshTextures.resize(sourceModel.Meshes.size());

        if (sourceModel.Meshes.empty())
        {
            return result;
        }

        xatlas::SetPrint(SilentPrint, false);

        // === 1. 全メッシュのライトマップUVを生成する ===
        // BVHは展開後の頂点で組む(展開は頂点を複製するだけで形状は変えないが、
        // 同じ配列を使うほうが対応関係を追いやすい)
        std::vector<uint8_t> unwrapped(sourceModel.Meshes.size(), 0);
        for (size_t i = 0; i < sourceModel.Meshes.size(); ++i)
        {
            unwrapped[i] = UnwrapMesh(sourceModel.Meshes[i], options.Resolution) ? 1 : 0;
            if (!unwrapped[i])
            {
                Warn("メッシュ[" + std::to_string(i) + "]のライトマップUVを生成できなかったため、遮蔽マップのベイクをスキップします");
                ++result.SkippedMeshCount;
            }
        }

        // === 2. モデル全体のBVHを構築する ===
        const Bvh bvh = BuildBvh(sourceModel);
        if (bvh.Triangles.empty())
        {
            return result;
        }

        // レイの長さの自動決定。モデルのバウンズ対角の10%を「近傍」とみなす
        float rayLength = options.RayLength;
        if (rayLength <= 0.0f)
        {
            const float dx = sourceModel.BoundsMax[0] - sourceModel.BoundsMin[0];
            const float dy = sourceModel.BoundsMax[1] - sourceModel.BoundsMin[1];
            const float dz = sourceModel.BoundsMax[2] - sourceModel.BoundsMin[2];
            const float diagonal = std::sqrt(dx * dx + dy * dy + dz * dz);
            rayLength = diagonal > 0.0f ? diagonal * 0.1f : 1.0f;
        }

        BakeDevice device;

        ComPtr<ID3D11Buffer> triangleBuffer;
        ComPtr<ID3D11Buffer> nodeBuffer;
        const ComPtr<ID3D11ShaderResourceView> triangleSrv = CreateStructuredSrv(
            device.Device(), bvh.Triangles.data(), sizeof(GpuTriangle), static_cast<uint32_t>(bvh.Triangles.size()), triangleBuffer);
        const ComPtr<ID3D11ShaderResourceView> nodeSrv = CreateStructuredSrv(
            device.Device(), bvh.Nodes.data(), sizeof(GpuBvhNode), static_cast<uint32_t>(bvh.Nodes.size()), nodeBuffer);

        Info("遮蔽マップをベイクします (三角形 " + std::to_string(bvh.Triangles.size())
            + " / BVHノード " + std::to_string(bvh.Nodes.size())
            + " / 解像度 " + std::to_string(options.Resolution)
            + " / レイ " + std::to_string(options.RayCount) + "本)");

        const uint32_t texelCount = options.Resolution * options.Resolution;

        // === 3. メッシュごとにラスタライズ → GPUでレイキャスト ===
        for (size_t meshIndex = 0; meshIndex < sourceModel.Meshes.size(); ++meshIndex)
        {
            if (!unwrapped[meshIndex])
            {
                continue;
            }

            std::vector<BakeTexel> texels;
            std::vector<uint8_t> valid;
            RasterizeLightmapSpace(sourceModel.Meshes[meshIndex], options.Resolution, texels, valid);

            const size_t validCount = std::count(valid.begin(), valid.end(), static_cast<uint8_t>(1));
            if (validCount == 0)
            {
                Warn("メッシュ[" + std::to_string(meshIndex) + "]はライトマップUV空間に有効なテクセルが無かったため、遮蔽マップのベイクをスキップします");
                ++result.SkippedMeshCount;
                continue;
            }

            // HLSL側のStructuredBuffer<uint>に合わせて4バイトへ展開する
            std::vector<uint32_t> validU32(valid.begin(), valid.end());

            ComPtr<ID3D11Buffer> texelBuffer;
            ComPtr<ID3D11Buffer> validBuffer;
            const ComPtr<ID3D11ShaderResourceView> texelSrv = CreateStructuredSrv(
                device.Device(), texels.data(), sizeof(BakeTexel), texelCount, texelBuffer);
            const ComPtr<ID3D11ShaderResourceView> validSrv = CreateStructuredSrv(
                device.Device(), validU32.data(), sizeof(uint32_t), texelCount, validBuffer);

            D3D11_BUFFER_DESC resultDesc{};
            resultDesc.ByteWidth = texelCount * sizeof(float);
            resultDesc.Usage = D3D11_USAGE_DEFAULT;
            resultDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            resultDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            resultDesc.StructureByteStride = sizeof(float);
            ComPtr<ID3D11Buffer> resultBuffer;
            ThrowIfFailed(device.Device()->CreateBuffer(&resultDesc, nullptr, &resultBuffer), "結果バッファの作成に失敗しました");

            D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
            uavDesc.Format = DXGI_FORMAT_UNKNOWN;
            uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            uavDesc.Buffer.NumElements = texelCount;
            ComPtr<ID3D11UnorderedAccessView> resultUav;
            ThrowIfFailed(device.Device()->CreateUnorderedAccessView(resultBuffer.Get(), &uavDesc, &resultUav), "UAVの作成に失敗しました");

            BakeConstants constants{};
            constants.TexelCount = texelCount;
            constants.RayCount = options.RayCount;
            constants.RayLength = rayLength;
            // 自己交差を避ける浮かせ量。レイ長に比例させ、スケールの異なるモデルでも同じ挙動にする
            constants.NormalOffset = rayLength * 1e-3f;

            D3D11_MAPPED_SUBRESOURCE mapped{};
            ThrowIfFailed(device.Context()->Map(device.Constants(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "定数バッファのMapに失敗しました");
            std::memcpy(mapped.pData, &constants, sizeof(constants));
            device.Context()->Unmap(device.Constants(), 0);

            ID3D11ShaderResourceView* srvs[] = { triangleSrv.Get(), nodeSrv.Get(), texelSrv.Get(), validSrv.Get() };
            ID3D11UnorderedAccessView* uavs[] = { resultUav.Get() };
            ID3D11Buffer* cbs[] = { device.Constants() };

            device.Context()->CSSetShader(device.Shader(), nullptr, 0);
            device.Context()->CSSetShaderResources(0, 4, srvs);
            device.Context()->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
            device.Context()->CSSetConstantBuffers(0, 1, cbs);
            device.Context()->Dispatch((texelCount + 63) / 64, 1, 1);

            // バインドを外してからリードバックする(次のメッシュで同じスロットを使い回すため)
            ID3D11ShaderResourceView* nullSrvs[4] = {};
            ID3D11UnorderedAccessView* nullUavs[1] = {};
            device.Context()->CSSetShaderResources(0, 4, nullSrvs);
            device.Context()->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);

            D3D11_BUFFER_DESC stagingDesc = resultDesc;
            stagingDesc.Usage = D3D11_USAGE_STAGING;
            stagingDesc.BindFlags = 0;
            stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Buffer> staging;
            ThrowIfFailed(device.Device()->CreateBuffer(&stagingDesc, nullptr, &staging), "リードバック用バッファの作成に失敗しました");
            device.Context()->CopyResource(staging.Get(), resultBuffer.Get());

            D3D11_MAPPED_SUBRESOURCE readback{};
            ThrowIfFailed(device.Context()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &readback), "リードバックのMapに失敗しました");
            const float* ao = static_cast<const float*>(readback.pData);

            std::vector<uint8_t> texture(texelCount);
            for (uint32_t i = 0; i < texelCount; ++i)
            {
                const float clamped = std::clamp(ao[i], 0.0f, 1.0f);
                texture[i] = static_cast<uint8_t>(clamped * 255.0f + 0.5f);
            }
            device.Context()->Unmap(staging.Get(), 0);

            Dilate(texture, valid, options.Resolution, options.DilationPixels);

            result.MeshTextures[meshIndex] = std::move(texture);
            ++result.BakedMeshCount;
        }

        return result;
    }
}
