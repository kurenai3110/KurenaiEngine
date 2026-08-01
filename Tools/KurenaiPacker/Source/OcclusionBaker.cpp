#include "OcclusionBaker.h"

#include <Windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <xatlas.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
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

        // === 計測 ==============================================================
        //
        // どのフェーズで時間が溶けているかを推測しないための計装。
        // ベイクは分単位で掛かるため、計測自体のオーバーヘッドは無視できる。

        using Clock = std::chrono::steady_clock;

        double SecondsSince(const Clock::time_point& start)
        {
            return std::chrono::duration<double>(Clock::now() - start).count();
        }

        // 数値を固定小数2桁の文字列にする(std::to_stringは常に6桁出て読みにくい)。
        // 単位はここに含めず呼び出し側で連結する ―― sprintf_sの書式文字列へ日本語を入れると
        // C4819(現在のコードページで表現できない文字)が出るため、既存コードと同じく
        // 書式文字列はASCIIに限り、日本語はstd::stringの連結側へ置く
        std::string Format2(double value)
        {
            char buffer[64];
            sprintf_s(buffer, "%.2f", value);
            return buffer;
        }

        // 検証用。誤差やベクトル長は2桁だと丸めで潰れるため4桁で出す
        std::string Format4(double value)
        {
            char buffer[64];
            sprintf_s(buffer, "%.4f", value);
            return buffer;
        }

        std::string FormatSeconds(double seconds)
        {
            return Format2(seconds) + "秒";
        }

        // フェーズごとの累計時間。メッシュをまたいで積算する
        struct BakeTimings
        {
            double UnwrapSeconds = 0.0;
            double BvhSeconds = 0.0;
            double RasterizeSeconds = 0.0;
            double DispatchSeconds = 0.0;   // Dispatch発行〜リードバック完了(GPU待ちを含む)
            double DilateSeconds = 0.0;
            uint64_t TotalRays = 0;         // 実際に飛ばしたレイの総数(有効テクセル × レイ本数)
        };

        // === BVH ===============================================================
        //
        // 中央値分割の単純なBVH。SAHは使わない ―― ベイクは1アセットにつき数回しか走らない
        // オフライン処理で、構築時間よりも実装の単純さ(=検証しやすさ)を優先したため。
        //
        // 【SAHへ変える必要は無い(実測)】Chinese Dragon(871306三角形、2048² / 64レイ)で
        // レイキャストは0.30秒・毎秒3〜4億レイ、BVH構築は0.31秒。ベイク全体394秒のうち
        // 合わせて0.16%しかない。残り99%はxatlasのUV展開(22.6.6節)。
        // ここを速くしても総時間はまったく変わらないので、単純なまま維持してよい。

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

        // xatlasの進捗・ログはこのツールの出力形式に合わせて捨てる(既定ではstdoutへ出る)。
        //
        // なお、この関数を素通し(vprintf)にしたうえでxatlas.cppの XA_PROFILE を1にし、
        // SetPrintの第2引数(verbose)をtrueにすると、xatlas内蔵のプロファイラが
        // ComputeChartsの内訳(Place seeds / Grow / Merge / Parameterize ...)を出力する。
        // UV展開が遅い原因を追うときはこれが一番早い(22.6.6節)
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

            const Clock::time_point addMeshStart = Clock::now();
            if (xatlas::AddMesh(atlas, decl) != xatlas::AddMeshError::Success)
            {
                return false;
            }
            // AddMeshは非同期。ここで待たないと、続くComputeChartsの計測へ
            // AddMeshぶんの時間が混ざって切り分けにならない
            xatlas::AddMeshJoin(atlas);
            const double addMeshSeconds = SecondsSince(addMeshStart);

            // ベイク時間のほぼ全部(実測で99%)がこの1行に入る。既定のChartOptionsのまま
            // 使っているのは、maxCostを4/8/16と振っても時間がまったく変わらなかったため
            // (378秒/379秒/399秒。詳細と原因は22.6.6節)
            const Clock::time_point computeStart = Clock::now();
            xatlas::ComputeCharts(atlas);
            const double computeSeconds = SecondsSince(computeStart);

            const Clock::time_point packStart = Clock::now();
            xatlas::PackOptions packOptions;
            packOptions.resolution = resolution;
            packOptions.padding = 2;        // バイリニア補間で隣のチャートを拾わないための余白
            packOptions.bilinear = true;
            packOptions.blockAlign = true;  // BC4の4x4ブロックとチャート境界を揃える
            xatlas::PackCharts(atlas, packOptions);

            // 1枚に収まらず複数アトラスへ分かれた場合、UVだけではどのアトラスかを表現できない。
            // texelsPerUnitを下げて(=チャートを小さくして)1枚に収まるまで詰め直す
            int packAttempts = 1;
            for (int attempt = 0; attempt < 4 && atlas->atlasCount > 1; ++attempt)
            {
                packOptions.texelsPerUnit = atlas->texelsPerUnit * 0.7f;
                xatlas::PackCharts(atlas, packOptions);
                ++packAttempts;
            }
            const double packSeconds = SecondsSince(packStart);

            Info("  UV展開の内訳: AddMesh " + FormatSeconds(addMeshSeconds)
                + " / ComputeCharts " + FormatSeconds(computeSeconds)
                + " / PackCharts " + FormatSeconds(packSeconds)
                + " (" + std::to_string(packAttempts) + "回)"
                + " / チャート数 " + std::to_string(atlas->chartCount));
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

        // bent normal用のダイレーション。上のR8版と同じ走査だが、平均する対象がベクトルになる。
        //
        // 【必ずベクトルのまま平均すること】length()は凸関数なので、Jensenの不等式より
        //   E[|b|] >= |E[b]|
        // が常に成り立つ。テクセルごとに長さを出してから平均すると必ず真値より大きくなり、
        // 遮蔽コーンが実際より細く見積もられてスペキュラが明るくなりすぎる。
        // length()を取るのは消費側(シェーダー)だけにする。
        // ミップ生成のボックスフィルタとサンプリング時のバイリニア補間も同じ理由で
        // ベクトルを補間しており、順序は自動的に正しくなる
        void DilateBentNormal(std::vector<float>& values, std::vector<uint8_t> valid, uint32_t resolution, uint32_t pixels)
        {
            const int res = static_cast<int>(resolution);
            for (uint32_t pass = 0; pass < pixels; ++pass)
            {
                std::vector<uint8_t> nextValid = valid;
                std::vector<float> nextValues = values;
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
                        float sum[3] = { 0.0f, 0.0f, 0.0f };
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
                                    for (int k = 0; k < 3; ++k)
                                    {
                                        sum[k] += values[n * 4 + k];
                                    }
                                    ++count;
                                }
                            }
                        }
                        if (count > 0)
                        {
                            for (int k = 0; k < 3; ++k)
                            {
                                nextValues[index * 4 + k] = sum[k] / static_cast<float>(count);
                            }
                            nextValues[index * 4 + 3] = 1.0f;   // 埋めたテクセルは有効にする
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

            // 最後まで埋まらなかったテクセルは有効フラグ0のまま残す。
            // R8版は「遮蔽なし(255)」で埋めるが、bent normalは遮蔽なしを定数で表せないため
            // (方向がテクセルごとに違う)、消費側でNへ落とさせる
        }

        // === bent normalの検証(仕様書§7のチェックリスト) ======================
        //
        // 量子化前のfloat32で集計する。ここを通しておけば、後段で見つけた異常が
        // ベイクのバグなのかfp16量子化なのかを切り分けられる
        struct BentStats
        {
            double MaxLength = 0.0;          // max|bRaw|。1を超えてはいけない
            double MaxAoNMinusAoB = -1.0e9;  // max(aoN - aoB)。0以下でなければいけない
            double SumSqError = 0.0;         // aoNと既存AOの二乗誤差の総和
            double MaxError = 0.0;           // 同、最大誤差
            uint64_t SampleCount = 0;
            uint64_t FullyOccludedCount = 0; // |bRaw| < 1e-3(完全遮蔽)
            uint64_t NanCount = 0;
            uint64_t OverOneCount = 0;       // |bRaw| > 1.05(明らかな異常)
        };

        void AccumulateBentStats(
            const float* bent,
            const float* ao,
            const std::vector<BakeTexel>& texels,
            const std::vector<uint8_t>& valid,
            uint32_t texelCount,
            size_t meshIndex,
            BentStats& stats)
        {
            uint64_t meshNan = 0;
            uint64_t meshOverOne = 0;

            for (uint32_t i = 0; i < texelCount; ++i)
            {
                if (!valid[i])
                {
                    continue;   // チャート外はダイレーションで埋まるので対象外
                }

                const float bx = bent[i * 4 + 0];
                const float by = bent[i * 4 + 1];
                const float bz = bent[i * 4 + 2];

                if (std::isnan(bx) || std::isnan(by) || std::isnan(bz) ||
                    std::isinf(bx) || std::isinf(by) || std::isinf(bz))
                {
                    ++meshNan;
                    continue;
                }

                const double aoB = std::sqrt(static_cast<double>(bx) * bx + static_cast<double>(by) * by + static_cast<double>(bz) * bz);
                const BakeTexel& texel = texels[i];
                const double aoN =
                    static_cast<double>(texel.Normal[0]) * bx +
                    static_cast<double>(texel.Normal[1]) * by +
                    static_cast<double>(texel.Normal[2]) * bz;

                stats.MaxLength = std::max(stats.MaxLength, aoB);
                stats.MaxAoNMinusAoB = std::max(stats.MaxAoNMinusAoB, aoN - aoB);

                // 既存AOとの一致。同じ積分の別推定量なので、モンテカルロ誤差ぶんしかズレないはず
                const double diff = aoN - static_cast<double>(ao[i]);
                stats.SumSqError += diff * diff;
                stats.MaxError = std::max(stats.MaxError, std::abs(diff));
                ++stats.SampleCount;

                if (aoB < 1e-3) { ++stats.FullyOccludedCount; }
                if (aoB > 1.05) { ++meshOverOne; }
            }

            stats.NanCount += meshNan;
            stats.OverOneCount += meshOverOne;

            if (meshNan > 0)
            {
                Warn("メッシュ[" + std::to_string(meshIndex) + "]のbent normalにNaN/Infが "
                    + std::to_string(meshNan) + "テクセルありました");
            }
            if (meshOverOne > 0)
            {
                Warn("メッシュ[" + std::to_string(meshIndex) + "]のbent normalで長さが1.05を超えたテクセルが "
                    + std::to_string(meshOverOne) + "個ありました(サンプリングか正規化係数の誤り)");
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
// bent normal(正規化しない)。.xyz = bRaw、.w = 有効フラグ。
// 正規化して長さを捨てないのは、消費側が長さ(=aoB)を錐体の広がりとして使うため
RWStructuredBuffer<float4> BentResult : register(u1);

cbuffer BakeConstants : register(b0)
{
    uint TexelCount;
    uint RayCount;
    float RayLength;
    float NormalOffset;
    uint BentRayCount;
    uint3 Padding;
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
        // bent normalは「遮蔽なし」を定数で表現できない(遮蔽なしのbRawはNそのもので
        // テクセルごとに違う)。有効フラグを0にして消費側でNへ落とさせる
        BentResult[index] = float4(0.0f, 0.0f, 0.0f, 0.0f);
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

    // --- bent normal(正規化しない可視方向の平均) ---
    //
    //   bRaw = (1/π) ∫ V(ω) ω dω
    //
    // 消費側はこれを軸 normalize(bRaw)、aoB = length(bRaw)、aoN = dot(N, bRaw) の3つへ分解する。
    // aoNは上のResultとまったく同じ量の別推定量なので、両者の一致がそのまま検証になる。
    //
    // 【コサイン重点サンプリングは使えない】上のAOと違い被積分関数にコサイン項が無いため、
    // 重みが打ち消し合わずπ/cosθになる。グレージング(cosθ→0)で重みが発散し分散が爆発する。
    // そのため一様半球サンプリング(pdf = 1/2π)を使う
    float3 bRaw = float3(0.0f, 0.0f, 0.0f);
    for (uint j = 0; j < BentRayCount; ++j)
    {
        const float2 xj = Hammersley(j, BentRayCount);
        // cosθに一様 = 立体角に一様
        const float cosT = xj.x;
        const float sinT = sqrt(max(0.0f, 1.0f - cosT * cosT));
        const float phiB = 6.2831853f * xj.y;
        const float3 dirB = normalize(T * (sinT * cos(phiB)) + B * (sinT * sin(phiB)) + N * cosT);

        if (!Occluded(origin, dirB, RayLength))
        {
            bRaw += dirB;
        }
    }
    // (1/π) ÷ pdf(=1/2π) ÷ 本数 = 2/本数。
    // 遮蔽が無ければ ∫ω dω = πN なので bRaw = N となり、長さがちょうど1になる
    bRaw *= 2.0f / float(BentRayCount);

    BentResult[index] = float4(bRaw, 1.0f);
}
)";

        struct BakeConstants
        {
            uint32_t TexelCount;
            uint32_t RayCount;
            float RayLength;
            float NormalOffset;
            uint32_t BentRayCount;
            uint32_t Padding[3];   // 定数バッファは16バイト単位。HLSL側のuint3 Paddingと対応する
        };
        static_assert(sizeof(BakeConstants) == 32, "BakeConstantsはHLSL側のcbufferと32バイトで一致させること");

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

                LogAdapter();

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
            // 実際に使われているアダプタをログへ出す。
            //
            // D3D11CreateDeviceの第1引数がnullptrのときDXGIの既定アダプタが選ばれるが、
            // それがどれなのかは記録されていなかった。ハイブリッドグラフィックス機で
            // 統合GPUを掴んでいないかを、憶測ではなくログで確認できるようにしておく。
            //
            // (実測では正しくディスクリートGPUが選ばれており、レイキャストは
            //  毎秒3〜4億レイ出ていた。ベイクが遅いのはGPU側ではなくUV展開が原因 ―― 22.6.6節)
            void LogAdapter()
            {
                ComPtr<IDXGIDevice> dxgiDevice;
                if (FAILED(m_Device.As(&dxgiDevice)))
                {
                    Warn("ベイク用デバイスからIDXGIDeviceを取得できなかったため、アダプタ名を記録できません");
                    return;
                }

                ComPtr<IDXGIAdapter> adapter;
                if (FAILED(dxgiDevice->GetAdapter(&adapter)))
                {
                    Warn("ベイク用デバイスからIDXGIAdapterを取得できなかったため、アダプタ名を記録できません");
                    return;
                }

                DXGI_ADAPTER_DESC desc{};
                if (FAILED(adapter->GetDesc(&desc)))
                {
                    Warn("アダプタの情報を取得できなかったため、アダプタ名を記録できません");
                    return;
                }

                // 専用VRAMが極端に小さいものは統合GPU(またはWARP)とみなせる。
                // 判別そのものはしないが、値を出しておけば人間が見て分かる
                const uint64_t dedicatedMB = static_cast<uint64_t>(desc.DedicatedVideoMemory) / (1024ull * 1024ull);
                Info("ベイクに使うGPU: " + WideToUtf8(desc.Description)
                    + " (専用VRAM " + std::to_string(dedicatedMB) + "MB)");
            }

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
        result.MeshBentNormals.resize(sourceModel.Meshes.size());

        if (sourceModel.Meshes.empty())
        {
            return result;
        }

        xatlas::SetPrint(SilentPrint, false);

        BakeTimings timings;
        BentStats stats;
        const Clock::time_point bakeStart = Clock::now();

        // === 1. 全メッシュのライトマップUVを生成する ===
        // BVHは展開後の頂点で組む(展開は頂点を複製するだけで形状は変えないが、
        // 同じ配列を使うほうが対応関係を追いやすい)
        const Clock::time_point unwrapStart = Clock::now();
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
        timings.UnwrapSeconds = SecondsSince(unwrapStart);

        // === 2. モデル全体のBVHを構築する ===
        const Clock::time_point bvhStart = Clock::now();
        const Bvh bvh = BuildBvh(sourceModel);
        timings.BvhSeconds = SecondsSince(bvhStart);
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
            + " / レイ " + std::to_string(options.RayCount) + "本"
            + " / bent normal " + (options.BentNormalRayCount > 0
                ? std::to_string(options.BentNormalRayCount) + "本"
                : std::string("なし"))
            + ")");

        const uint32_t texelCount = options.Resolution * options.Resolution;

        // === 3. メッシュごとにラスタライズ → GPUでレイキャスト ===
        for (size_t meshIndex = 0; meshIndex < sourceModel.Meshes.size(); ++meshIndex)
        {
            if (!unwrapped[meshIndex])
            {
                continue;
            }

            const Clock::time_point rasterizeStart = Clock::now();
            std::vector<BakeTexel> texels;
            std::vector<uint8_t> valid;
            RasterizeLightmapSpace(sourceModel.Meshes[meshIndex], options.Resolution, texels, valid);
            timings.RasterizeSeconds += SecondsSince(rasterizeStart);

            const size_t validCount = std::count(valid.begin(), valid.end(), static_cast<uint8_t>(1));
            if (validCount == 0)
            {
                Warn("メッシュ[" + std::to_string(meshIndex) + "]はライトマップUV空間に有効なテクセルが無かったため、遮蔽マップのベイクをスキップします");
                ++result.SkippedMeshCount;
                continue;
            }

            // 有効テクセルだけがレイを飛ばす(無効テクセルはシェーダー冒頭で即returnする)。
            // スループットの分母はこちらでなければならない
            timings.TotalRays +=
                static_cast<uint64_t>(validCount) * (options.RayCount + options.BentNormalRayCount);

            // UV展開の品質指標(22.6.4)。
            //   被覆率      = 有効テクセル / 全テクセル。アトラスをどれだけ使えているか
            //   テクセル/三角形 = 三角形あたり何テクセル割り当てられたか。1を切ると斑点が出る
            // チャートが小さく数が多いほど、パディングとバイリニアの余白が食ってこの2つが下がる
            const size_t meshTriangleCount = sourceModel.Meshes[meshIndex].Indices.size() / 3;
            const double coverage = static_cast<double>(validCount) / static_cast<double>(texelCount);
            const double texelsPerTriangle = meshTriangleCount > 0
                ? static_cast<double>(validCount) / static_cast<double>(meshTriangleCount)
                : 0.0;
            Info("  メッシュ[" + std::to_string(meshIndex) + "] 被覆率 " + Format2(coverage * 100.0)
                + "% / テクセル毎三角形 " + Format2(texelsPerTriangle));

            const Clock::time_point dispatchStart = Clock::now();

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

            // bent normal用の結果バッファ(float4)。シェーダーは常にu1へ書くため、
            // ベイクしない設定でもバッファ自体は作ってバインドする
            D3D11_BUFFER_DESC bentDesc = resultDesc;
            bentDesc.ByteWidth = texelCount * sizeof(float) * 4;
            bentDesc.StructureByteStride = sizeof(float) * 4;
            ComPtr<ID3D11Buffer> bentBuffer;
            ThrowIfFailed(device.Device()->CreateBuffer(&bentDesc, nullptr, &bentBuffer), "bent normalの結果バッファの作成に失敗しました");
            ComPtr<ID3D11UnorderedAccessView> bentUav;
            ThrowIfFailed(device.Device()->CreateUnorderedAccessView(bentBuffer.Get(), &uavDesc, &bentUav), "bent normalのUAVの作成に失敗しました");

            BakeConstants constants{};
            constants.TexelCount = texelCount;
            constants.RayCount = options.RayCount;
            constants.BentRayCount = options.BentNormalRayCount;
            constants.RayLength = rayLength;
            // 自己交差を避ける浮かせ量。レイ長に比例させ、スケールの異なるモデルでも同じ挙動にする
            constants.NormalOffset = rayLength * 1e-3f;

            D3D11_MAPPED_SUBRESOURCE mapped{};
            ThrowIfFailed(device.Context()->Map(device.Constants(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "定数バッファのMapに失敗しました");
            std::memcpy(mapped.pData, &constants, sizeof(constants));
            device.Context()->Unmap(device.Constants(), 0);

            ID3D11ShaderResourceView* srvs[] = { triangleSrv.Get(), nodeSrv.Get(), texelSrv.Get(), validSrv.Get() };
            ID3D11UnorderedAccessView* uavs[] = { resultUav.Get(), bentUav.Get() };
            ID3D11Buffer* cbs[] = { device.Constants() };

            device.Context()->CSSetShader(device.Shader(), nullptr, 0);
            device.Context()->CSSetShaderResources(0, 4, srvs);
            device.Context()->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
            device.Context()->CSSetConstantBuffers(0, 1, cbs);
            device.Context()->Dispatch((texelCount + 63) / 64, 1, 1);

            // バインドを外してからリードバックする(次のメッシュで同じスロットを使い回すため)
            ID3D11ShaderResourceView* nullSrvs[4] = {};
            ID3D11UnorderedAccessView* nullUavs[2] = {};
            device.Context()->CSSetShaderResources(0, 4, nullSrvs);
            device.Context()->CSSetUnorderedAccessViews(0, 2, nullUavs, nullptr);

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

            // bent normalのリードバック。AO(float)とは別バッファなのでステージングも別に要る
            std::vector<float> bentTexture;
            if (options.BentNormalRayCount > 0)
            {
                D3D11_BUFFER_DESC bentStagingDesc = bentDesc;
                bentStagingDesc.Usage = D3D11_USAGE_STAGING;
                bentStagingDesc.BindFlags = 0;
                bentStagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                ComPtr<ID3D11Buffer> bentStaging;
                ThrowIfFailed(device.Device()->CreateBuffer(&bentStagingDesc, nullptr, &bentStaging),
                    "bent normalのリードバック用バッファの作成に失敗しました");
                device.Context()->CopyResource(bentStaging.Get(), bentBuffer.Get());

                D3D11_MAPPED_SUBRESOURCE bentReadback{};
                ThrowIfFailed(device.Context()->Map(bentStaging.Get(), 0, D3D11_MAP_READ, 0, &bentReadback),
                    "bent normalのリードバックのMapに失敗しました");
                const float* bent = static_cast<const float*>(bentReadback.pData);

                bentTexture.assign(bent, bent + static_cast<size_t>(texelCount) * 4);

                // 仕様書§7のチェックリストを量子化前のfloat32で集計する。
                // ここで既存AOとの一致を見ておかないと、後段で見つけた誤差が
                // ベイクのバグなのかfp16量子化なのか切り分けられない
                AccumulateBentStats(bent, ao, texels, valid, texelCount, meshIndex, stats);

                device.Context()->Unmap(bentStaging.Get(), 0);
            }

            device.Context()->Unmap(staging.Get(), 0);
            timings.DispatchSeconds += SecondsSince(dispatchStart);

            const Clock::time_point dilateStart = Clock::now();
            Dilate(texture, valid, options.Resolution, options.DilationPixels);
            if (!bentTexture.empty())
            {
                DilateBentNormal(bentTexture, valid, options.Resolution, options.DilationPixels);
                result.MeshBentNormals[meshIndex] = std::move(bentTexture);
            }
            timings.DilateSeconds += SecondsSince(dilateStart);

            result.MeshTextures[meshIndex] = std::move(texture);
            ++result.BakedMeshCount;
        }

        // === 4. 計測結果 ===
        //
        // フェーズ別に出すのは「どこを速くすべきか」を推測せずに決めるため。
        // レイ/秒はGPUトラバーサルの改善を評価する唯一の指標なので、必ず残すこと
        const double totalSeconds = SecondsSince(bakeStart);
        Info("ベイク完了 (合計 " + FormatSeconds(totalSeconds) + ")");
        Info("  UV展開       " + FormatSeconds(timings.UnwrapSeconds));
        Info("  BVH構築      " + FormatSeconds(timings.BvhSeconds));
        Info("  ラスタライズ " + FormatSeconds(timings.RasterizeSeconds));
        Info("  レイキャスト " + FormatSeconds(timings.DispatchSeconds) + " (GPU待ちを含む)");
        Info("  ダイレーション " + FormatSeconds(timings.DilateSeconds));
        if (timings.DispatchSeconds > 0.0 && timings.TotalRays > 0)
        {
            const double raysPerSecondM =
                static_cast<double>(timings.TotalRays) / timings.DispatchSeconds / 1.0e6;
            Info("  レイ " + std::to_string(timings.TotalRays) + "本 / "
                + Format2(raysPerSecondM) + " 百万レイ毎秒");
        }

        // === 5. bent normalの検証(仕様書§7) ===
        //
        // 上から2項目が通れば地平線の二重計上が無いこと、
        // 「既存AOとの一致」が通ればサンプリングと正規化係数が正しいことが確認できる
        if (stats.SampleCount > 0)
        {
            const double rms = std::sqrt(stats.SumSqError / static_cast<double>(stats.SampleCount));
            const double occludedRatio =
                static_cast<double>(stats.FullyOccludedCount) / static_cast<double>(stats.SampleCount) * 100.0;

            Info("bent normalの検証 (量子化前のfloat32、有効テクセル " + std::to_string(stats.SampleCount) + "個):");
            Info("  max|bRaw|         " + Format4(stats.MaxLength) + " (1.0以下であること)");
            Info("  max(aoN - aoB)    " + Format4(stats.MaxAoNMinusAoB) + " (0.0以下であること)");
            Info("  既存AOとの誤差    RMS " + Format4(rms) + " / 最大 " + Format4(stats.MaxError));
            Info("  完全遮蔽の割合    " + Format2(occludedRatio) + "%");

            // 一致していないということは、同じ積分の別推定量になっていないということ。
            // 0.05はモンテカルロ誤差(256本で数%)に安全側の余裕を見た値
            if (rms > 0.05)
            {
                Warn("bent normalのaoN(= dot(N,bRaw))が既存AOと一致していません (RMS " + Format4(rms)
                    + ")。一様半球サンプリングの正規化係数(2/本数)かレイの基底を確認してください");
            }
            if (stats.MaxLength > 1.05)
            {
                Warn("bent normalの長さが1を大きく超えています (max " + Format4(stats.MaxLength) + ")");
            }
            if (stats.MaxAoNMinusAoB > 0.05)
            {
                Warn("aoNがaoBを上回っています (max " + Format4(stats.MaxAoNMinusAoB)
                    + ")。コーシー・シュワルツの不等式に反するため、どちらかの計算が誤っています");
            }
            if (stats.NanCount > 0)
            {
                Warn("bent normalにNaN/Infが合計 " + std::to_string(stats.NanCount) + "テクセルありました");
            }
        }

        return result;
    }
}
