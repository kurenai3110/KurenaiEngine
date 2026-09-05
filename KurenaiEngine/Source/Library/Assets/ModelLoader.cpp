#include "ModelLoader.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Core/Logger.h"
#include "Core/StringUtil.h"
#include "ModelPackage.h"
#include "RHI/TextureImage.h"
#include "Vertex.h"

// KurenaiPacker.exe(オフラインのアセットビルドツール)が生成した.kmodel/.kgeom/.ktexを
// 読み込む。**assimpによるモデル解析・WICデコード・ミップ生成・GPU BC7圧縮をここで行っては
// いけない**(前処理はすべてKurenaiPackerの担当)。このファイルはassimp/zlibに依存せず、
// 「パース済み・圧縮済みのデータをファイルから読み、GPUバッファ/テクスチャへ流し込むだけ」
// である。詳細な設計判断はdocs/Architecture.htmlの「モデルパッケージ形式」の章を参照

namespace Kurenai::Assets
{
    namespace
    {
        using Core::Utf8ToWide;
        using Core::WideToUtf8;

        std::wstring GetDirectory(const std::wstring& filePath)
        {
            const size_t pos = filePath.find_last_of(L"/\\");
            return pos == std::wstring::npos ? L"" : filePath.substr(0, pos + 1);
        }

        // 2時点間の経過時間をミリ秒の整数文字列にする(ログ表示用)
        std::string FormatMs(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point end)
        {
            return std::to_string(static_cast<long long>(std::chrono::duration<double, std::milli>(end - start).count()));
        }

        // StringPool(offset,length)からUTF-8部分文字列を安全に取り出す。壊れた.kmodelが
        // 範囲外を指していてもプロセスを異常終了させないよう、必ず範囲チェックを行う
        std::string ReadPoolString(const std::string& pool, uint32_t offset, uint32_t length, const char* fieldNameForError)
        {
            if (static_cast<uint64_t>(offset) + length > pool.size())
            {
                throw std::runtime_error(std::string("パッケージのStringPool参照が範囲外です: ") + fieldNameForError);
            }
            return pool.substr(offset, length);
        }

        // メッシュのUV密度(モデルローカル1メートルあたりのUV単位)を、三角形を抜き取って見積もる。
        // 求められない場合(UVが無い・全部縮退している)は0を返す。
        //
        // 【10パーセンタイル(低い側)を採る】1つのテクスチャに常駐段は1つしか持てないので、
        // 縛るのは「最も引き伸ばされている=テクセル密度が最も低い」領域である。そこが
        // 最も細かいミップを要求する。密度を高く見積もると、引き伸ばされた面が耐えられない
        // 段まで削ってぼける。
        //
        // 最小値ではなく1割の位置を採るのは、UVが潰れかけた細長い三角形が最小値を支配し、
        // どのテクスチャも常に全ミップ常駐になってしまうため。
        //
        // 【実測】中央値(従来)とp90で比べると、p90はbias -2で0.80を下回るタイルが3枚→14枚、
        // 最悪タイルが0.572→0.175へ悪化した。密度は高く見積もるほどぼける。
        //
        // 【全三角形を見ない】読み込み時間に効く(LOD2の1タイルで11万三角形×1715メッシュ)。
        // 先頭64個ではなく等間隔に抜くのは、メッシュの一部だけに偏った密度を代表値にしないため
        float EstimateUVPerLocalMeter(const Vertex* vertices, uint32_t vertexCount, const uint32_t* indices, uint32_t indexCount)
        {
            constexpr uint32_t kMaxSamples = 64;

            if (vertices == nullptr || indices == nullptr || vertexCount == 0 || indexCount < 3)
            {
                return 0.0f;
            }

            const uint32_t triangleCount = indexCount / 3;
            const uint32_t stride = std::max(1u, triangleCount / kMaxSamples);

            std::vector<float> densities;
            densities.reserve(kMaxSamples);
            for (uint32_t tri = 0; tri < triangleCount; tri += stride)
            {
                const uint32_t i0 = indices[tri * 3 + 0];
                const uint32_t i1 = indices[tri * 3 + 1];
                const uint32_t i2 = indices[tri * 3 + 2];
                if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount)
                {
                    continue;
                }

                const Vertex& v0 = vertices[i0];
                const Vertex& v1 = vertices[i1];
                const Vertex& v2 = vertices[i2];

                const float e1[3] = { v1.Position[0] - v0.Position[0], v1.Position[1] - v0.Position[1], v1.Position[2] - v0.Position[2] };
                const float e2[3] = { v2.Position[0] - v0.Position[0], v2.Position[1] - v0.Position[1], v2.Position[2] - v0.Position[2] };
                const float cross[3] = {
                    e1[1] * e2[2] - e1[2] * e2[1],
                    e1[2] * e2[0] - e1[0] * e2[2],
                    e1[0] * e2[1] - e1[1] * e2[0],
                };
                const float localArea =
                    0.5f * std::sqrt(cross[0] * cross[0] + cross[1] * cross[1] + cross[2] * cross[2]);

                const float du1 = v1.UV[0] - v0.UV[0];
                const float dv1 = v1.UV[1] - v0.UV[1];
                const float du2 = v2.UV[0] - v0.UV[0];
                const float dv2 = v2.UV[1] - v0.UV[1];
                const float uvArea = 0.5f * std::fabs(du1 * dv2 - dv1 * du2);

                // 縮退した三角形(面積0、UVが潰れている)は密度が発散するので除く
                if (localArea <= 1e-9f || uvArea <= 0.0f)
                {
                    continue;
                }

                densities.push_back(std::sqrt(uvArea / localArea));
            }

            if (densities.empty())
            {
                return 0.0f;
            }

            // 10パーセンタイル。要素が少ないときも範囲外にならないよう添字で丸める
            const size_t index = densities.size() / 10;
            std::nth_element(densities.begin(), densities.begin() + index, densities.end());
            return densities[index];
        }

        // メッシュが実際に使っているUVの範囲(AABB)。求められなければ false。
        //
        // 【自発光テクスチャの平均色に要る】アトラスの一角しか使わないメッシュで
        // テクスチャ全体の平均を取ると、無関係な部分の色を拾う
        bool ComputeUVBounds(
            const Vertex* vertices, uint32_t vertexCount, const uint32_t* indices, uint32_t indexCount,
            float outMin[2], float outMax[2])
        {
            if (vertices == nullptr || indices == nullptr || vertexCount == 0 || indexCount == 0)
            {
                return false;
            }
            bool any = false;
            for (uint32_t i = 0; i < indexCount; ++i)
            {
                const uint32_t v = indices[i];
                if (v >= vertexCount)
                {
                    continue;
                }
                for (int axis = 0; axis < 2; ++axis)
                {
                    const float uv = vertices[v].UV[axis];
                    if (!any)
                    {
                        outMin[axis] = uv;
                        outMax[axis] = uv;
                    }
                    else
                    {
                        outMin[axis] = std::min(outMin[axis], uv);
                        outMax[axis] = std::max(outMax[axis], uv);
                    }
                }
                any = true;
            }
            return any;
        }

        // 線形サムネイルの、UV矩形に対応する部分だけの平均を取る。
        //
        // 【UVが1周を超えていたら全体平均へ落とす】タイリングしているメッシュでは、
        // AABBがテクスチャ全体より広くなり矩形の意味が無くなる
        void AverageThumbnailRect(
            const std::vector<float>& thumbnail, uint32_t size, const float uvMin[2], const float uvMax[2],
            float outRGB[3])
        {
            const bool tiled = (uvMax[0] - uvMin[0] > 1.0f) || (uvMax[1] - uvMin[1] > 1.0f);
            int x0 = 0, x1 = static_cast<int>(size) - 1, y0 = 0, y1 = static_cast<int>(size) - 1;
            if (!tiled)
            {
                // 【[0,1] の外へ出ている範囲は折り返す】幅が1未満でも UV が 3.0〜3.4 のような
                // 位置にあることがある。クランプで済ませると端の1テクセルだけを読み、
                // 黙って無関係な色になる。繰り返し指定なので floor(min) ぶん平行移動すればよい
                const auto wrapped = [](float lo, float hi, float& outLo, float& outHi)
                {
                    const float shift = std::floor(lo);
                    outLo = lo - shift;
                    outHi = hi - shift;
                };
                float u0 = uvMin[0], u1 = uvMax[0], v0 = uvMin[1], v1 = uvMax[1];
                wrapped(uvMin[0], uvMax[0], u0, u1);
                wrapped(uvMin[1], uvMax[1], v0, v1);

                const auto toTexel = [size](float uv)
                {
                    const int t = static_cast<int>(std::floor(uv * static_cast<float>(size)));
                    return std::clamp(t, 0, static_cast<int>(size) - 1);
                };
                x0 = toTexel(u0); x1 = toTexel(u1);
                y0 = toTexel(v0); y1 = toTexel(v1);
                if (x1 < x0) { std::swap(x0, x1); }
                if (y1 < y0) { std::swap(y0, y1); }
            }
            double sum[3] = { 0.0, 0.0, 0.0 };
            uint32_t count = 0;
            for (int y = y0; y <= y1; ++y)
            {
                for (int x = x0; x <= x1; ++x)
                {
                    const size_t bin = static_cast<size_t>(y) * size + x;
                    for (int channel = 0; channel < 3; ++channel)
                    {
                        sum[channel] += thumbnail[bin * 3 + channel];
                    }
                    count += 1u;
                }
            }
            if (count == 0)
            {
                outRGB[0] = outRGB[1] = outRGB[2] = 1.0f;
                return;
            }
            for (int channel = 0; channel < 3; ++channel)
            {
                outRGB[channel] = static_cast<float>(sum[channel] / count);
            }
        }

        // 自発光テクスチャから取る線形サムネイルの一辺。
        // 【UVの矩形を切り出すために面積が要る】1x1の平均だけだと、アトラスの一角しか使わない
        // メッシュで無関係な部分の色を拾う。32x32 なら 1枚 12KB で、切り出しにも足りる
        constexpr uint32_t kEmissiveThumbnailSize = 32;

        // 光源のかたまりの長さ尺度[m]。**分割と併合の両方をこの1つが決める。** 0で両方とも行わない。
        //   ・連結成分がこの寸法より大きければ、等間隔グリッドで割る
        //   ・別々の連結成分でも重心がこの距離より近ければ、1つへ併合する
        //
        // 【根拠の軸は測光の「5倍則」】発光体の最大寸法の5倍以上離れれば、点光源近似の誤差は
        // 1%以下になる。1m を選ぶと 5m 以遠で1%に収まり、屋内で照明の効き方を判断する距離
        // スケール(およそ5m)と噛み合う。
        //
        // 【併合が要る理由は実測で決まった】段Bまで通すと EmeraldSquare が 3370個になり、
        // kMaxLights(1024)を大きく超える。**上限で切り捨てて逃げてはいけない** ――
        // その3370個を面積の大きい順に並べても、上位256個で総面積の46.7%、
        // 上位1024個でも84.9%にしかならず、発光の相当量を黙って捨てることになる。
        // 面積の分布は最小 3.6e-4 / 中央値 8.2e-2 / 最大 3.66 m^2 で、
        // 上位が支配していない ―― 切り捨てが安全になる形の裾ではない。
        // 併合ならエネルギーは厳密に保存される(総面積 864.231 m^2 は3段すべてで同値)。
        //
        // 実測(メッシュ内で完結。段ごとの個数):
        //           段A(連結成分)  段A+B(分割後)  段C(併合後)  最近接クラスタ間
        //   Bistro         7             7            7          2.96 m
        //   ProbeTest     14            28           28          1.86 m
        //   EmeraldSquare 1427        3370          651          0.011 m
        //
        // 【段Bは個数を増やす側にも働く】ProbeTest は連結成分14個が分割で28個になっている。
        // 「分割を切れば併合の効果が分かる」わけではない ―― 両方を同時に切ると
        // ProbeTest では符号すら逆に見える
        //
        // 【平面パネルには効かない】分割しても面積等価半径とRangeの比は変わらない
        // (A ∝ s^2 かつ Range ∝ sqrt(E*A) ∝ s)。改善するのは「位置の局所性」だけで、
        // 大きくて明るい壁の近似の質そのものは三角形メッシュライトでしか直らない
        constexpr float kEmissiveClusterScale = 1.0f;

        // エミッシブなメッシュの三角形を「光源のかたまり」へ分け、プロキシの元になる量を求める。
        //
        // 【1メッシュ = 1かたまり にしてはいけない】Bistro の内装は「1マテリアル・1メッシュに
        // 複数の電球」という持ち方をしている。メッシュのAABBを1個の光源にすると部屋全体を包む
        // 灯になり、docs/ImplementationHistory.md にある「エミッシブの位置を手で割り出して
        // ポイントライトを置く」運用に逆戻りする。
        //
        // 【段取りは2段】
        //   段A: 位置で溶接してから連結成分を取る。しきい値以外にパラメータが無く、
        //        物理的に離れた器具はこれだけで割れる
        //   段B: 連結成分がまだ大きいとき(蛍光灯の長い列など)だけ、等間隔グリッドで割る
        //   段C: 近すぎるかたまりどうしを併合する。街区の看板のように「小さな面がばらばらに
        //        大量にある」形は段Aで数千個に割れてしまい、ライト数の上限を超える
        //
        // 【決定的であること】乱数を使わないのはもちろん、unordered_map の**反復順に依存しない**
        // ように書く(添字は必ず三角形の走査順で採番し、段Cの種は面積と重心で順序を決める)。
        // 起動ごとに灯の位置や順番が変わると、A/B比較そのものが成立しなくなる。
        // BuildEmissiveClusters が返した三角形→かたまりの対応から、かたまりごとに連続した
        // 三角形の並びを作る。**かたまりの TriangleOffset もここで書き込む**。
        //
        // 【並べ替えの向きに注意】三角形を選ぶ第2段はこの連続範囲を面積比で引くので、
        // 「かたまりごとに連続していること」が提案分布の前提そのものになる。
        // 崩すと、あるかたまりを選んだのに別のかたまりの三角形へ行く ―― 絵は出る
        void BuildEmissiveTriangles(
            const Vertex* vertices, uint32_t vertexCount, const uint32_t* indices,
            const std::vector<uint32_t>& triangleCluster, std::vector<EmissiveCluster>& clusters,
            std::vector<EmissiveTriangle>& outTriangles)
        {
            outTriangles.clear();
            if (vertices == nullptr || indices == nullptr || clusters.empty())
            {
                return;
            }
            // かたまりごとの枚数を数えてから開始位置を確定する(2パス。並べ替えは1回で済む)
            std::vector<uint32_t> counts(clusters.size(), 0u);
            for (const uint32_t cluster : triangleCluster)
            {
                if (cluster < clusters.size()) { ++counts[cluster]; }
            }
            uint32_t offset = 0;
            for (size_t c = 0; c < clusters.size(); ++c)
            {
                clusters[c].TriangleOffset = offset;
                offset += counts[c];
            }
            outTriangles.resize(offset);
            std::vector<uint32_t> cursor(clusters.size(), 0u);
            for (size_t tri = 0; tri < triangleCluster.size(); ++tri)
            {
                const uint32_t cluster = triangleCluster[tri];
                if (cluster >= clusters.size()) { continue; }
                const uint32_t i0 = indices[tri * 3 + 0];
                const uint32_t i1 = indices[tri * 3 + 1];
                const uint32_t i2 = indices[tri * 3 + 2];
                if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount) { continue; }

                EmissiveTriangle& dst = outTriangles[clusters[cluster].TriangleOffset + cursor[cluster]];
                ++cursor[cluster];
                for (int axis = 0; axis < 3; ++axis)
                {
                    dst.P0[axis] = vertices[i0].Position[axis];
                    dst.E1[axis] = vertices[i1].Position[axis] - vertices[i0].Position[axis];
                    dst.E2[axis] = vertices[i2].Position[axis] - vertices[i0].Position[axis];
                }
                dst.ClusterIndex = cluster;
            }
        }

        // outTriangleCluster に nullptr 以外を渡すと、三角形ごとの「どのかたまりに属するか」を
        // 返す(要素数 = indexCount/3、どこにも属さない三角形は kEmissiveTriangleUnassigned)。
        //
        // 【段階2のメッシュライトが要る】三角形を面積比で引くとき、まずかたまりを選んでから
        // その中の三角形を引く2段構えにする。**かたまりの中身が分からないと第2段が書けない**
        std::vector<EmissiveCluster> BuildEmissiveClusters(
            const Vertex* vertices, uint32_t vertexCount, const uint32_t* indices, uint32_t indexCount,
            const float boundsMin[3], const float boundsMax[3], float clusterScale,
            std::vector<uint32_t>* outTriangleCluster = nullptr)
        {
            std::vector<EmissiveCluster> result;
            if (outTriangleCluster != nullptr)
            {
                outTriangleCluster->assign(indexCount / 3, kEmissiveTriangleUnassigned);
            }
            if (vertices == nullptr || indices == nullptr || vertexCount == 0 || indexCount < 3)
            {
                return result;
            }
            const uint32_t triangleCount = indexCount / 3;

            // --- 頂点を位置で溶接する ---
            //
            // 【素の頂点番号で連結成分を取ってはいけない】.kgeom の頂点は meshopt を通した後で、
            // 法線やUVが違えば同じ位置でも別の頂点になっている。溶接を省くと**1個の電球が
            // 数十片に割れる**。しかも「細かい光源がたくさん出た」だけに見えるので気付けない。
            //
            // 【しきい値はメッシュの大きさに比例させる】絶対値で固定すると、1.1km四方の
            // PLATEAU タイルで float32 の分解能(1000m 付近で約 6e-5)を割り込む。
            // 相対 1e-5 は float32 の仮数(約 1.2e-7 相対)に対して十分な余裕があり、
            // DCC の頂点溶接許容(ふつう 1e-4 m 前後)よりは細かい
            float diagonal = 0.0f;
            for (int axis = 0; axis < 3; ++axis)
            {
                const float d = boundsMax[axis] - boundsMin[axis];
                diagonal += d * d;
            }
            diagonal = std::sqrt(diagonal);
            const float weldEpsilon = std::clamp(1e-5f * diagonal, 1e-5f, 1e-3f);
            const float invWeld = 1.0f / weldEpsilon;

            struct QuantizedKey
            {
                int64_t X, Y, Z;
                bool operator==(const QuantizedKey& other) const
                {
                    return X == other.X && Y == other.Y && Z == other.Z;
                }
            };
            struct QuantizedKeyHash
            {
                size_t operator()(const QuantizedKey& k) const
                {
                    size_t h = static_cast<size_t>(k.X) * 0x9E3779B97F4A7C15ull;
                    h ^= static_cast<size_t>(k.Y) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
                    h ^= static_cast<size_t>(k.Z) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
                    return h;
                }
            };

            // union-find の親配列。溶接と三角形の連結の両方に使う
            std::vector<uint32_t> parent(vertexCount);
            for (uint32_t v = 0; v < vertexCount; ++v)
            {
                parent[v] = v;
            }
            // 経路圧縮。再帰にすると頂点数ぶんの深さになりうるのでループで書く
            const auto findRoot = [&parent](uint32_t v) -> uint32_t
            {
                uint32_t root = v;
                while (parent[root] != root) { root = parent[root]; }
                while (parent[v] != root) { const uint32_t next = parent[v]; parent[v] = root; v = next; }
                return root;
            };
            // 【小さい番号を親にする】結合の向きを入力順に依存させないための決定性の要件
            const auto unite = [&parent, &findRoot](uint32_t a, uint32_t b)
            {
                const uint32_t ra = findRoot(a);
                const uint32_t rb = findRoot(b);
                if (ra == rb) { return; }
                if (ra < rb) { parent[rb] = ra; } else { parent[ra] = rb; }
            };

            {
                std::unordered_map<QuantizedKey, uint32_t, QuantizedKeyHash> weldMap;
                weldMap.reserve(vertexCount);
                for (uint32_t v = 0; v < vertexCount; ++v)
                {
                    QuantizedKey key;
                    key.X = std::llround(static_cast<double>(vertices[v].Position[0]) * invWeld);
                    key.Y = std::llround(static_cast<double>(vertices[v].Position[1]) * invWeld);
                    key.Z = std::llround(static_cast<double>(vertices[v].Position[2]) * invWeld);
                    const auto inserted = weldMap.emplace(key, v);
                    if (!inserted.second)
                    {
                        unite(inserted.first->second, v);
                    }
                }
            }

            // --- 三角形の3頂点をつないで連結成分にする ---
            for (uint32_t tri = 0; tri < triangleCount; ++tri)
            {
                const uint32_t i0 = indices[tri * 3 + 0];
                const uint32_t i1 = indices[tri * 3 + 1];
                const uint32_t i2 = indices[tri * 3 + 2];
                if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount)
                {
                    continue;
                }
                unite(i0, i1);
                unite(i1, i2);
            }

            // --- 三角形を「かたまり」へ割り当てて累積する ---
            struct GroupKey
            {
                uint32_t Root;
                int64_t CellX, CellY, CellZ; // 段Bのグリッドセル(分割しないときは常に0)
                bool operator==(const GroupKey& o) const
                {
                    return Root == o.Root && CellX == o.CellX && CellY == o.CellY && CellZ == o.CellZ;
                }
            };
            struct GroupKeyHash
            {
                size_t operator()(const GroupKey& k) const
                {
                    size_t h = static_cast<size_t>(k.Root) * 0x9E3779B97F4A7C15ull;
                    h ^= static_cast<size_t>(k.CellX) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
                    h ^= static_cast<size_t>(k.CellY) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
                    h ^= static_cast<size_t>(k.CellZ) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
                    return h;
                }
            };

            struct Accum
            {
                double Area = 0.0;
                double CentroidSum[3] = { 0.0, 0.0, 0.0 }; // Σ A_i c_i
                double NormalSum[3] = { 0.0, 0.0, 0.0 };   // Σ A_i n_i (= Σ cross_i / 2)
                double OwnMoment = 0.0;                    // Σ (A_i/36)(|ab|^2+|bc|^2+|ca|^2)
                float BoundsMin[3] = { 0.0f, 0.0f, 0.0f };
                float BoundsMax[3] = { 0.0f, 0.0f, 0.0f };
                uint32_t TriangleCount = 0;
            };
            std::vector<Accum> groups;
            std::unordered_map<GroupKey, uint32_t, GroupKeyHash> groupIndexOf;
            std::vector<uint32_t> triangleGroup(triangleCount, 0xFFFFFFFFu);

            const bool splitEnabled = clusterScale > 0.0f;
            const float invSplit = splitEnabled ? (1.0f / clusterScale) : 0.0f;

            for (uint32_t tri = 0; tri < triangleCount; ++tri)
            {
                const uint32_t i0 = indices[tri * 3 + 0];
                const uint32_t i1 = indices[tri * 3 + 1];
                const uint32_t i2 = indices[tri * 3 + 2];
                if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount)
                {
                    continue;
                }
                const float* p0 = vertices[i0].Position;
                const float* p1 = vertices[i1].Position;
                const float* p2 = vertices[i2].Position;

                const double e1[3] = { static_cast<double>(p1[0]) - p0[0], static_cast<double>(p1[1]) - p0[1], static_cast<double>(p1[2]) - p0[2] };
                const double e2[3] = { static_cast<double>(p2[0]) - p0[0], static_cast<double>(p2[1]) - p0[1], static_cast<double>(p2[2]) - p0[2] };
                const double cross[3] = {
                    e1[1] * e2[2] - e1[2] * e2[1],
                    e1[2] * e2[0] - e1[0] * e2[2],
                    e1[0] * e2[1] - e1[1] * e2[0],
                };
                const double area =
                    0.5 * std::sqrt(cross[0] * cross[0] + cross[1] * cross[1] + cross[2] * cross[2]);
                // 縮退した三角形は面積0で法線の向きも決まらない。重心にも寄与しないので飛ばす
                if (area <= 1e-12)
                {
                    continue;
                }

                const double centroid[3] = {
                    (static_cast<double>(p0[0]) + p1[0] + p2[0]) / 3.0,
                    (static_cast<double>(p0[1]) + p1[1] + p2[1]) / 3.0,
                    (static_cast<double>(p0[2]) + p1[2] + p2[2]) / 3.0,
                };

                GroupKey key{};
                key.Root = findRoot(i0);
                key.CellX = key.CellY = key.CellZ = 0;
                if (splitEnabled)
                {
                    // 【重心でセルを決める】頂点で決めると1枚の三角形が複数セルに跨る。
                    // セルの原点はメッシュのAABB最小コーナーで、インスタンス変換に依存しない
                    key.CellX = static_cast<int64_t>(std::floor((centroid[0] - boundsMin[0]) * invSplit));
                    key.CellY = static_cast<int64_t>(std::floor((centroid[1] - boundsMin[1]) * invSplit));
                    key.CellZ = static_cast<int64_t>(std::floor((centroid[2] - boundsMin[2]) * invSplit));
                }

                uint32_t groupIndex;
                const auto found = groupIndexOf.find(key);
                if (found == groupIndexOf.end())
                {
                    groupIndex = static_cast<uint32_t>(groups.size());
                    groupIndexOf.emplace(key, groupIndex);
                    groups.emplace_back();
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        groups[groupIndex].BoundsMin[axis] = p0[axis];
                        groups[groupIndex].BoundsMax[axis] = p0[axis];
                    }
                }
                else
                {
                    groupIndex = found->second;
                }
                triangleGroup[tri] = groupIndex;

                Accum& acc = groups[groupIndex];
                acc.Area += area;
                acc.TriangleCount += 1u;
                for (int axis = 0; axis < 3; ++axis)
                {
                    acc.CentroidSum[axis] += area * centroid[axis];
                    // Σ A_i n_i は Σ cross_i / 2 と厳密に等しい(crossの長さが 2*A_i)。
                    // 正規化してから面積を掛け直すより丸めが1段少ない
                    acc.NormalSum[axis] += 0.5 * cross[axis];
                    acc.BoundsMin[axis] = std::min({ acc.BoundsMin[axis], p0[axis], p1[axis], p2[axis] });
                    acc.BoundsMax[axis] = std::max({ acc.BoundsMax[axis], p0[axis], p1[axis], p2[axis] });
                }

                // 三角形自身の広がり(自分の重心まわりの二次モーメント)。
                // ∫|x-g|^2 dA = (A/36)(|ab|^2 + |bc|^2 + |ca|^2)
                //
                // 【これを落とすと三角形1枚のかたまりで半径が厳密に0になる】重心の散らばりだけを
                // 見ると1枚しかないときに0になる。半径0は点光源を意味し、半影が消え、
                // 近傍のクランプ(d^2 + R^2)も効かなくなる
                const double ab[3] = { static_cast<double>(p1[0]) - p0[0], static_cast<double>(p1[1]) - p0[1], static_cast<double>(p1[2]) - p0[2] };
                const double bc[3] = { static_cast<double>(p2[0]) - p1[0], static_cast<double>(p2[1]) - p1[1], static_cast<double>(p2[2]) - p1[2] };
                const double ca[3] = { static_cast<double>(p0[0]) - p2[0], static_cast<double>(p0[1]) - p2[1], static_cast<double>(p0[2]) - p2[2] };
                const double edgeSq =
                    ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2] +
                    bc[0] * bc[0] + bc[1] * bc[1] + bc[2] * bc[2] +
                    ca[0] * ca[0] + ca[1] * ca[1] + ca[2] * ca[2];
                acc.OwnMoment += area * edgeSq / 36.0;
            }

            if (groups.empty())
            {
                return result;
            }

            // --- 段C: 近すぎるかたまりを併合する ---
            //
            // 【上限で切り捨てて逃げてはいけない】街区の看板のように「小さな発光面がばらばらに
            // 大量にある」形は段Aで数千個に割れる。面積の大きい順に上位を残す形にすると、
            // EmeraldSquare では上位256個で総面積の46.7%にしかならず、発光の半分以上を
            // 黙って捨てることになる。併合ならエネルギーは厳密に保存される
            constexpr uint32_t kUnassigned = 0xFFFFFFFFu;
            std::vector<uint32_t> mergedOf(groups.size(), kUnassigned);
            uint32_t mergedCount = 0;
            if (clusterScale > 0.0f)
            {
                std::vector<double> provisional(groups.size() * 3, 0.0);
                for (size_t g = 0; g < groups.size(); ++g)
                {
                    const double inv = (groups[g].Area > 0.0) ? (1.0 / groups[g].Area) : 0.0;
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        provisional[g * 3 + axis] = groups[g].CentroidSum[axis] * inv;
                    }
                }

                // 【総当たりにしない】三角形の多いメッシュではかたまりが数百〜数千になり、
                // O(n^2) の距離判定が読み込み時間に効く。セル幅を併合距離に取れば、
                // 距離が併合距離以下の相手は必ず隣接27セルの中にいる
                const double invCell = 1.0 / clusterScale;
                std::unordered_map<QuantizedKey, std::vector<uint32_t>, QuantizedKeyHash> cellMap;
                cellMap.reserve(groups.size());
                std::vector<QuantizedKey> cellOf(groups.size());
                for (size_t g = 0; g < groups.size(); ++g)
                {
                    QuantizedKey cell;
                    cell.X = static_cast<int64_t>(std::floor(provisional[g * 3 + 0] * invCell));
                    cell.Y = static_cast<int64_t>(std::floor(provisional[g * 3 + 1] * invCell));
                    cell.Z = static_cast<int64_t>(std::floor(provisional[g * 3 + 2] * invCell));
                    cellOf[g] = cell;
                    cellMap[cell].push_back(static_cast<uint32_t>(g));
                }

                // 【種の順序で結果が決まるので、順序を完全に決めておく】面積の大きい順。
                // 同値は重心の辞書順で割る(浮動小数の同値は起きうるので添字までは落とさない)
                std::vector<uint32_t> order(groups.size());
                for (size_t g = 0; g < groups.size(); ++g)
                {
                    order[g] = static_cast<uint32_t>(g);
                }
                std::sort(
                    order.begin(), order.end(),
                    [&groups, &provisional](uint32_t a, uint32_t b)
                    {
                        if (groups[a].Area != groups[b].Area) { return groups[a].Area > groups[b].Area; }
                        for (int axis = 0; axis < 3; ++axis)
                        {
                            const double pa = provisional[a * 3 + axis];
                            const double pb = provisional[b * 3 + axis];
                            if (pa != pb) { return pa < pb; }
                        }
                        return a < b;
                    });

                const double mergeDistSq = static_cast<double>(clusterScale) * clusterScale;
                for (const uint32_t seed : order)
                {
                    if (mergedOf[seed] != kUnassigned)
                    {
                        continue;
                    }
                    const uint32_t target = mergedCount++;
                    mergedOf[seed] = target;

                    const QuantizedKey base = cellOf[seed];
                    for (int64_t dz = -1; dz <= 1; ++dz)
                    {
                        for (int64_t dy = -1; dy <= 1; ++dy)
                        {
                            for (int64_t dx = -1; dx <= 1; ++dx)
                            {
                                QuantizedKey probe{ base.X + dx, base.Y + dy, base.Z + dz };
                                const auto it = cellMap.find(probe);
                                if (it == cellMap.end())
                                {
                                    continue;
                                }
                                // セル内は添字の昇順で並んでいる(挿入順がそのまま昇順)
                                for (const uint32_t candidate : it->second)
                                {
                                    if (mergedOf[candidate] != kUnassigned)
                                    {
                                        continue;
                                    }
                                    double distSq = 0.0;
                                    for (int axis = 0; axis < 3; ++axis)
                                    {
                                        const double d =
                                            provisional[candidate * 3 + axis] - provisional[seed * 3 + axis];
                                        distSq += d * d;
                                    }
                                    if (distSq <= mergeDistSq)
                                    {
                                        mergedOf[candidate] = target;
                                    }
                                }
                            }
                        }
                    }
                }
            }
            else
            {
                for (size_t g = 0; g < groups.size(); ++g)
                {
                    mergedOf[g] = mergedCount++;
                }
            }

            // 併合後の累積へ畳む(面積・重心・法線・自分の広がりはすべて単純な和になる)
            std::vector<Accum> merged(mergedCount);
            std::vector<bool> mergedInitialized(mergedCount, false);
            for (size_t g = 0; g < groups.size(); ++g)
            {
                Accum& dst = merged[mergedOf[g]];
                const Accum& src = groups[g];
                dst.Area += src.Area;
                dst.OwnMoment += src.OwnMoment;
                dst.TriangleCount += src.TriangleCount;
                for (int axis = 0; axis < 3; ++axis)
                {
                    dst.CentroidSum[axis] += src.CentroidSum[axis];
                    dst.NormalSum[axis] += src.NormalSum[axis];
                    dst.BoundsMin[axis] = mergedInitialized[mergedOf[g]]
                        ? std::min(dst.BoundsMin[axis], src.BoundsMin[axis]) : src.BoundsMin[axis];
                    dst.BoundsMax[axis] = mergedInitialized[mergedOf[g]]
                        ? std::max(dst.BoundsMax[axis], src.BoundsMax[axis]) : src.BoundsMax[axis];
                }
                mergedInitialized[mergedOf[g]] = true;
            }
            groups.swap(merged);
            for (uint32_t& g : triangleGroup)
            {
                if (g != 0xFFFFFFFFu)
                {
                    g = mergedOf[g];
                }
            }

            // --- 二次モーメントの第2段(重心が確定してから、重心の散らばりを足す) ---
            std::vector<double> centroidSpread(groups.size(), 0.0);
            for (uint32_t tri = 0; tri < triangleCount; ++tri)
            {
                const uint32_t groupIndex = triangleGroup[tri];
                if (groupIndex == 0xFFFFFFFFu)
                {
                    continue;
                }
                const float* p0 = vertices[indices[tri * 3 + 0]].Position;
                const float* p1 = vertices[indices[tri * 3 + 1]].Position;
                const float* p2 = vertices[indices[tri * 3 + 2]].Position;
                const double e1[3] = { static_cast<double>(p1[0]) - p0[0], static_cast<double>(p1[1]) - p0[1], static_cast<double>(p1[2]) - p0[2] };
                const double e2[3] = { static_cast<double>(p2[0]) - p0[0], static_cast<double>(p2[1]) - p0[1], static_cast<double>(p2[2]) - p0[2] };
                const double cross[3] = {
                    e1[1] * e2[2] - e1[2] * e2[1],
                    e1[2] * e2[0] - e1[0] * e2[2],
                    e1[0] * e2[1] - e1[1] * e2[0],
                };
                const double area =
                    0.5 * std::sqrt(cross[0] * cross[0] + cross[1] * cross[1] + cross[2] * cross[2]);
                const Accum& acc = groups[groupIndex];
                const double invArea = (acc.Area > 0.0) ? (1.0 / acc.Area) : 0.0;
                double distSq = 0.0;
                for (int axis = 0; axis < 3; ++axis)
                {
                    const double c = (static_cast<double>(p0[axis]) + p1[axis] + p2[axis]) / 3.0;
                    const double d = c - acc.CentroidSum[axis] * invArea;
                    distSq += d * d;
                }
                centroidSpread[groupIndex] += area * distSq;
            }

            // --- かたまりごとの値を確定する ---
            //
            // 【かたまり番号と出力の添字は一致しない】面積0のかたまりはここで落ちるので、
            // 三角形→かたまりの対応もこの写像を通してから返す。
            // 現状は面積0のかたまりが作られない(面積が正の三角形しか登録しない)が、
            // **その前提に依存させない** ―― 依存すると、落ちる条件が増えた瞬間に
            // 三角形が1つずつずれたかたまりを指し、絵は出るのに静かに壊れる
            std::vector<uint32_t> groupToCluster(groups.size(), kEmissiveTriangleUnassigned);
            result.reserve(groups.size());
            for (size_t g = 0; g < groups.size(); ++g)
            {
                const Accum& acc = groups[g];
                if (acc.Area <= 0.0 || acc.TriangleCount == 0)
                {
                    continue;
                }
                groupToCluster[g] = static_cast<uint32_t>(result.size());
                const double invArea = 1.0 / acc.Area;

                EmissiveCluster cluster;
                cluster.Area = static_cast<float>(acc.Area);
                cluster.TriangleCount = acc.TriangleCount;
                for (int axis = 0; axis < 3; ++axis)
                {
                    cluster.Centroid[axis] = static_cast<float>(acc.CentroidSum[axis] * invArea);
                    cluster.BoundsMin[axis] = acc.BoundsMin[axis];
                    cluster.BoundsMax[axis] = acc.BoundsMax[axis];
                }

                // 指向性 κ = |Σ A_i n_i| / Σ A_i。閉じた曲面なら0、平らな片面なら1
                const double normalLength = std::sqrt(
                    acc.NormalSum[0] * acc.NormalSum[0] + acc.NormalSum[1] * acc.NormalSum[1] +
                    acc.NormalSum[2] * acc.NormalSum[2]);
                cluster.Directionality = static_cast<float>(std::clamp(normalLength * invArea, 0.0, 1.0));
                if (normalLength > 1e-12)
                {
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        cluster.AverageNormal[axis] = static_cast<float>(acc.NormalSum[axis] / normalLength);
                    }
                }
                // 打ち消し合って向きが決まらない場合(閉じた形・両面)は既定値のまま。
                // そのとき κ もほぼ0なので寄与はほぼ等方になり、向きは効かない

                // 面積等価の円板半径。平行軸の定理により、かたまり全体の二次モーメントは
                // 「三角形自身の広がり」と「重心の散らばり」の和になる。
                // 半径 a の一様な円板では 2*moment/A = a^2 なので、定義どおり a に戻る
                const double moment = acc.OwnMoment + centroidSpread[g];
                cluster.SourceRadius = static_cast<float>(std::sqrt(std::max(0.0, 2.0 * moment * invArea)));

                result.push_back(cluster);
            }

            if (outTriangleCluster != nullptr)
            {
                for (uint32_t tri = 0; tri < triangleCount; ++tri)
                {
                    const uint32_t groupIndex = triangleGroup[tri];
                    (*outTriangleCluster)[tri] = (groupIndex == 0xFFFFFFFFu)
                                                     ? kEmissiveTriangleUnassigned
                                                     : groupToCluster[groupIndex];
                }
            }

            return result;
        }

        // テクスチャの読み込みとキャッシュ・共有インスタンス(白/フラット法線/マゼンタ)の管理。
        // .kmodelのTextureEntryは既にKurenaiPacker側でユニーク化(同じ画像+同じsRGBは1件に集約)
        // 済みのため、パス文字列ベースの重複排除キャッシュは持たず、
        // 添字(TextureEntryのインデックス)だけで管理する
        class TextureLoader
        {
        public:
            // sharedTextures が非nullなら、1x1のフォールバック(白/フラット法線/黒/マゼンタ)を
            // そちらから借りる。nullならモデル自身が持つ(従来の挙動)
            TextureLoader(RHI::IRHIDevice& device, Model& model, SharedTexturePool* sharedTextures)
                : m_Device(device)
                , m_Model(model)
                , m_SharedTextures(sharedTextures)
            {
            }

            // texturePathsの各要素(.ktexへのフルパス)を並列にデコードし、成功した分だけGPU
            // リソース化してoutTexturesへ格納する。失敗した添字はoutTextures[i]==nullptrのまま
            // 残すので、呼び出し側(LoadModel)がスロットの種類(BaseColor/Normal/MetallicRoughness)
            // ごとに適切なフォールバック(白/フラット法線/マゼンタ)を選んで埋めること
            //
            // thumbnailIndices に入っている添字については、**線形空間の 32x32 サムネイル**を
            // outThumbnails へ残す。自発光テクスチャの平均色を求めるためのもので、
            // デコード済みの TextureImage が生きているのはここだけ(GPUへ送ったら解放される)。
            // 1枚あたり 32*32*3 float = 12KB しか持たない
            void LoadAll(
                const std::vector<std::wstring>& texturePaths, std::vector<RHI::IRHITexture*>& outTextures,
                const std::unordered_set<int32_t>& thumbnailIndices,
                std::unordered_map<int32_t, std::vector<float>>& outThumbnails)
            {
                outTextures.assign(texturePaths.size(), nullptr);
                if (texturePaths.empty())
                {
                    return;
                }

                // デコード(TextureImage::LoadFromPackedTexture、単なるファイル読み込み+DDSデコードで
                // GPUデバイスを必要としない)はワーカースレッドで並列化できるが、GPUリソース作成
                // (device.CreateTextureFromImage)はデバイスに紐づく処理のためこのスレッドで直列に行う
                constexpr unsigned int kMaxWorkers = 8;
                const unsigned int hardwareThreads = std::min(kMaxWorkers, std::max(1u, std::thread::hardware_concurrency()));
                const unsigned int workerCount = std::min(hardwareThreads, static_cast<unsigned int>(texturePaths.size()));

                struct CompletedItem
                {
                    size_t Index = 0;
                    std::optional<RHI::TextureImage> Image;
                    std::string ErrorMessage;
                    uint64_t SizeInBytes = 0;
                    // .ktexのヘッダ情報。常駐ミップ制御が使う(Model::TextureInfosのコメント参照)。
                    // **ここで取るのは、後でRenderスレッドに読ませないため**
                    RHI::PackedTextureInfo Info{};
                };

                std::mutex queueMutex;
                std::condition_variable spaceAvailable;
                std::condition_variable itemAvailable;
                std::deque<CompletedItem> completedQueue;
                uint64_t pendingBytes = 0;
                std::atomic<size_t> nextIndex{ 0 };

                // ワーカーがGPU化(このスレッド)に追いつかれすぎてデコード済みイメージを
                // メモリに溜め込みすぎないよう、件数とバイト数の両方で上限を設ける
                const size_t maxPendingCount = static_cast<size_t>(workerCount) * 2;
                constexpr uint64_t kMaxPendingBytes = 1ull * 1024 * 1024 * 1024;

                auto workerFn = [&]()
                {
                    for (;;)
                    {
                        const size_t index = nextIndex.fetch_add(1);
                        if (index >= texturePaths.size())
                        {
                            break;
                        }

                        CompletedItem item;
                        item.Index = index;
                        try
                        {
                            RHI::TextureImage image = RHI::TextureImage::LoadFromPackedTexture(texturePaths[index]);
                            item.SizeInBytes = image.GetSizeInBytes();
                            item.Image = std::move(image);
                            // 読めなくてもテクスチャ自体は使える(常駐ミップ制御の対象から外れるだけ)
                            RHI::TextureImage::TryReadPackedTextureInfo(texturePaths[index], item.Info);
                        }
                        catch (const std::exception& e)
                        {
                            item.ErrorMessage = e.what();
                        }

                        std::unique_lock<std::mutex> lock(queueMutex);
                        spaceAvailable.wait(lock, [&] { return completedQueue.size() < maxPendingCount && pendingBytes < kMaxPendingBytes; });
                        pendingBytes += item.SizeInBytes;
                        completedQueue.push_back(std::move(item));
                        lock.unlock();
                        itemAvailable.notify_one();
                    }
                };

                std::vector<std::thread> workers;
                workers.reserve(workerCount);
                for (unsigned int w = 0; w < workerCount; ++w)
                {
                    workers.emplace_back(workerFn);
                }

                for (size_t consumed = 0; consumed < texturePaths.size(); ++consumed)
                {
                    std::unique_lock<std::mutex> lock(queueMutex);
                    itemAvailable.wait(lock, [&] { return !completedQueue.empty(); });
                    CompletedItem item = std::move(completedQueue.front());
                    completedQueue.pop_front();
                    pendingBytes -= item.SizeInBytes;
                    lock.unlock();
                    spaceAvailable.notify_one();

                    if (item.Image.has_value())
                    {
                        // 【GPUへ送る前にここで取る】デコード済みの画像が生きているのはこの場だけ
                        if (thumbnailIndices.count(static_cast<int32_t>(item.Index)) != 0)
                        {
                            std::vector<float> thumbnail(
                                static_cast<size_t>(kEmissiveThumbnailSize) * kEmissiveThumbnailSize * 3, 1.0f);
                            if (item.Image->ExtractLinearThumbnail(kEmissiveThumbnailSize, thumbnail.data()))
                            {
                                outThumbnails.emplace(static_cast<int32_t>(item.Index), std::move(thumbnail));
                            }
                            else
                            {
                                Core::Logger::Warning(
                                    "ModelLoader",
                                    "自発光テクスチャの平均色を取り出せませんでした。白として扱います: " +
                                        WideToUtf8(texturePaths[item.Index]));
                            }
                        }

                        try
                        {
                            auto texture = m_Device.CreateTextureFromImage(*item.Image);
                            outTextures[item.Index] = texture.get();
                            m_Model.Textures.push_back(std::move(texture));
                            // テクスチャストリーミングが常駐ミップを変えるときに読み直す元。
                            // Texturesと同じ並びになるようここで一緒に積む
                            m_Model.TexturePaths.push_back(texturePaths[item.Index]);
                            m_Model.TextureInfos.push_back(item.Info);
                        }
                        catch (const std::exception& e)
                        {
                            Core::Logger::Error("ModelLoader", "テクスチャのGPU転送に失敗しました (" + WideToUtf8(texturePaths[item.Index]) + "): " + e.what());
                        }
                    }
                    else
                    {
                        Core::Logger::Error("ModelLoader", "テクスチャの読み込みに失敗しました (" + WideToUtf8(texturePaths[item.Index]) + "): " + item.ErrorMessage);
                    }
                }

                for (auto& worker : workers)
                {
                    worker.join();
                }
            }

            RHI::IRHITexture* GetWhite()
            {
                return Acquire(m_White, m_SharedTextures ? &m_SharedTextures->White : nullptr, 255, 255, 255, 255);
            }

            RHI::IRHITexture* GetFlatNormal()
            {
                // タンジェント空間で(0,0,1)、すなわち「法線マップなし」を表す色
                return Acquire(m_FlatNormal, m_SharedTextures ? &m_SharedTextures->FlatNormal : nullptr, 128, 128, 255, 255);
            }

            // bent normalを持たないマテリアルのフォールバック。
            //
            // 【白ではなく黒】bent normalは「遮蔽なし」を定数テクスチャで表現できない ――
            // 遮蔽なしのbRawは法線Nそのもので、ピクセルごとに違うため。
            // アルファ0を「データ無し」の明示的なフラグとして使い、消費側で
            // axis = N / aoB = 1(遮蔽なし)へ落とさせる。長さ0を遮蔽なしと解釈させると
            // 完全遮蔽(SO=0)と区別がつかなくなる(34章)
            RHI::IRHITexture* GetBlack()
            {
                return Acquire(m_Black, m_SharedTextures ? &m_SharedTextures->Black : nullptr, 0, 0, 0, 0);
            }

            // 読み込みに失敗したBaseColor/MetallicRoughnessテクスチャの代替。目立つ色にすることで
            // モデル全体の読み込みは継続しつつ問題箇所が分かるようにする
            RHI::IRHITexture* GetMagentaPlaceholder()
            {
                return Acquire(m_Magenta, m_SharedTextures ? &m_SharedTextures->Magenta : nullptr, 255, 0, 255, 255);
            }

        private:
            // 1x1の定数テクスチャを1つ返す。共有プールがあればそこへ、無ければモデルへ所有させる。
            //
            // localCache はどちらの経路でも使う。共有プール経由でも、2回目以降にプールの
            // メンバを読みに行くコストを省ける(1モデルあたり最大4スロット×メッシュ数だけ呼ばれる)
            RHI::IRHITexture* Acquire(
                RHI::IRHITexture*& localCache,
                RHI::IRHITexture** sharedSlot,
                uint8_t r, uint8_t g, uint8_t b, uint8_t a)
            {
                if (localCache)
                {
                    return localCache;
                }

                if (sharedSlot)
                {
                    if (!*sharedSlot)
                    {
                        auto texture = m_Device.CreateSolidColorTexture(r, g, b, a);
                        *sharedSlot = texture.get();
                        m_SharedTextures->Owned.push_back(std::move(texture));
                    }
                    localCache = *sharedSlot;
                    return localCache;
                }

                auto texture = m_Device.CreateSolidColorTexture(r, g, b, a);
                localCache = texture.get();
                m_Model.Textures.push_back(std::move(texture));
                return localCache;
            }

            RHI::IRHIDevice& m_Device;
            Model& m_Model;
            // 非nullなら1x1のフォールバックをここから借りる(所有もこちら)
            SharedTexturePool* m_SharedTextures = nullptr;
            RHI::IRHITexture* m_White = nullptr;
            RHI::IRHITexture* m_FlatNormal = nullptr;
            RHI::IRHITexture* m_Black = nullptr;
            RHI::IRHITexture* m_Magenta = nullptr;
        };

        // メッシュをマテリアル(3枚のテクスチャの組み合わせ)単位でまとめておく。
        // .kmodelはKurenaiPackerがシーングラフ巡回順のまま書き出しているため、DX12バックエンドの
        // 「直前の描画と同じテクスチャならSRVテーブルを使い回す」最適化(DX12CommandList::
        // FlushPendingSrvWrites)がヒットしやすいよう、読み込み後にソートしておく
        void SortMeshesByMaterial(Model& model)
        {
            std::sort(
                model.Meshes.begin(), model.Meshes.end(),
                [](const Mesh& a, const Mesh& b)
                {
                    const std::less<RHI::IRHITexture*> less;
                    if (a.BaseColorTexture != b.BaseColorTexture)
                    {
                        return less(a.BaseColorTexture, b.BaseColorTexture);
                    }
                    if (a.NormalTexture != b.NormalTexture)
                    {
                        return less(a.NormalTexture, b.NormalTexture);
                    }
                    if (a.MetallicRoughnessTexture != b.MetallicRoughnessTexture)
                    {
                        return less(a.MetallicRoughnessTexture, b.MetallicRoughnessTexture);
                    }
                    if (a.OcclusionTexture != b.OcclusionTexture)
                    {
                        return less(a.OcclusionTexture, b.OcclusionTexture);
                    }
                    return less(a.BentNormalTexture, b.BentNormalTexture);
                });
        }

        // マテリアルテーブル(Model::MaterialTableBuffer)を組み立てる。
        //
        // 【SortMeshesByMaterialの後で呼ぶこと】あちらはメッシュの並びを入れ替えるため、
        // 先に番号を振ると対応が崩れる。
        //
        // 【テクスチャのbindless登録もここで行う】Mesh側のポインタは、モデルが所有する
        // テクスチャとシーン共有の1x1フォールバックのどちらでもありうるが、
        // RegisterBindlessは同じリソースに対して同じ番号を返す(冪等)ので区別しなくてよい。
        // どのメッシュからも参照されないテクスチャは登録されず、区画を無駄にしない。
        //
        // 【v9はマテリアルとメッシュが1対1】.kmodel v9のMeshEntryは係数とテクスチャ番号を
        // 直接持っており、マテリアルテーブルという概念自体が無い。そのため
        // 「メッシュ1つ = マテリアル1件」として作る。**重複除去は行わない** ――
        // .kmodelがマテリアルテーブルを持つようになればそちらの番号をそのまま使うので、
        // ここで独自の集約規則を作ると二重管理になる。
        // (PLATEAU LOD2はマテリアル1,715件がすべて異なり、集約しても1件も減らない)
        void BuildMaterialTable(RHI::IRHIDevice& device, Model& model)
        {
            if (!device.SupportsBindless() || model.Meshes.empty())
            {
                return;
            }

            const auto registerTexture = [&device](RHI::IRHITexture* texture) {
                return texture ? device.RegisterBindless(texture) : RHI::kInvalidBindlessIndex;
            };

            std::vector<GpuMaterial> materials;
            materials.reserve(model.Meshes.size());
            for (Mesh& mesh : model.Meshes)
            {
                GpuMaterial material;
                material.BaseColorFactor[0] = mesh.BaseColorFactor[0];
                material.BaseColorFactor[1] = mesh.BaseColorFactor[1];
                material.BaseColorFactor[2] = mesh.BaseColorFactor[2];
                material.BaseColorFactor[3] = mesh.BaseColorFactor[3];
                material.EmissiveFactor[0] = mesh.EmissiveFactor[0];
                material.EmissiveFactor[1] = mesh.EmissiveFactor[1];
                material.EmissiveFactor[2] = mesh.EmissiveFactor[2];
                material.MetallicFactor = mesh.MetallicFactor;
                material.RoughnessFactor = mesh.RoughnessFactor;
                material.AlphaCutoff = mesh.AlphaCutoff;
                material.OcclusionStrength = mesh.OcclusionStrength;
                material.Translucency = mesh.Translucency;
                material.BaseColorTextureIndex = registerTexture(mesh.BaseColorTexture);
                material.NormalTextureIndex = registerTexture(mesh.NormalTexture);
                material.MetallicRoughnessTextureIndex = registerTexture(mesh.MetallicRoughnessTexture);
                material.EmissiveTextureIndex = registerTexture(mesh.EmissiveTexture);
                material.OcclusionTextureIndex = registerTexture(mesh.OcclusionTexture);
                material.BentNormalTextureIndex = registerTexture(mesh.BentNormalTexture);
                material.Flags = 0;
                if (mesh.IsTransparent)
                {
                    material.Flags |= kGpuMaterialFlagTransparent;
                }
                if (mesh.AlphaCutoff > 0.0f)
                {
                    material.Flags |= kGpuMaterialFlagCutout;
                }

                mesh.MaterialIndex = static_cast<uint32_t>(materials.size());
                materials.push_back(material);
            }

            RHI::BufferDesc desc;
            desc.Usage = RHI::BufferUsage::StructuredImmutable;
            desc.SizeInBytes = static_cast<uint32_t>(materials.size() * sizeof(GpuMaterial));
            desc.StrideInBytes = sizeof(GpuMaterial);
            desc.InitialData = materials.data();
            model.MaterialTableBuffer = device.CreateBuffer(desc);
            if (!model.MaterialTableBuffer)
            {
                Core::Logger::Error("ModelLoader", "マテリアルテーブルのバッファ作成に失敗しました");
                return;
            }

            model.MaterialCount = static_cast<uint32_t>(materials.size());
            if (device.RegisterBindless(model.MaterialTableBuffer.get()) == RHI::kInvalidBindlessIndex)
            {
                // bindless区画が満杯。ここで諦めておかないと、シェーダーが
                // 無効番号でテーブルを引こうとする(=未定義動作)ことになる。
                // テーブルを捨てれば描画側は従来のメッシュ単位経路へ落ちる
                Core::Logger::Error(
                    "ModelLoader", "マテリアルテーブルをbindlessへ登録できませんでした(区画が満杯?)");
                model.MaterialTableBuffer.reset();
                model.MaterialCount = 0;
            }
        }

        // モデル単位に連結したメッシュレットの3ブロックをGPUバッファにする。
        //
        // 【SortMeshesByMaterialとBuildMaterialTableの後で呼ぶこと】メッシュレット1件ごとに
        // 「どのマテリアルか」を書き込む必要があり、その番号はマテリアルテーブルを
        // 組み立てて初めて決まる。またSortMeshesByMaterialはメッシュを入れ替えるが、
        // Mesh::MeshletOffsetはメッシュ自身が持っているので入れ替わっても対応は保たれる
        void BuildMeshletTables(
            RHI::IRHIDevice& device, Model& model, std::vector<GpuMeshlet>& meshlets,
            const std::vector<uint32_t>& meshletVertices, const std::vector<uint32_t>& meshletTriangles)
        {
            if (meshlets.empty())
            {
                return;
            }

            // マテリアル番号と材質のフラグを、メッシュから各メッシュレットへ配る。
            // 1つのメッシュレットは必ず1つのメッシュ(=1つのマテリアル)に属する ――
            // KurenaiPackerがmeshopt_buildMeshletsをメッシュごとに呼んでいるため、
            // 塊が材質を跨ぐことは構造的に起きない
            for (const Mesh& mesh : model.Meshes)
            {
                uint32_t flags = 0;
                if (mesh.IsTransparent)
                {
                    flags |= kGpuMaterialFlagTransparent;
                }
                if (mesh.AlphaCutoff > 0.0f)
                {
                    flags |= kGpuMaterialFlagCutout;
                }

                // 【全段へ配ること】材質はメッシュの性質なので、そのメッシュの
                // どの段の塊にも同じものが要る。LOD0だけに配ると、粗い段を選んだ瞬間に
                // 半透明・アルファカットアウトのふるい分けが効かなくなる
                for (uint32_t m = 0; m < mesh.MeshletTotalCount; ++m)
                {
                    GpuMeshlet& meshlet = meshlets[mesh.MeshletOffset + m];
                    meshlet.MaterialIndex = mesh.MaterialIndex;
                    // 【代入ではなくOR】Flagsの上位には詰め替えのときに入れた段のビットが
                    // 入っている。代入すると段が全部0になり、どのモデルも常に原寸で描かれる
                    // (絵は正しく出るので、性能が変わらないことでしか気づけない)
                    meshlet.Flags = (meshlet.Flags & ~kGpuMaterialFlagMask) | flags;
                }
            }

            // 3本ともシーン読み込み時に一度書いたら変わらないためStructuredImmutable
            const auto createImmutable = [&device](const void* data, size_t count, uint32_t stride) {
                RHI::BufferDesc desc;
                desc.Usage = RHI::BufferUsage::StructuredImmutable;
                desc.SizeInBytes = static_cast<uint32_t>(count) * stride;
                desc.StrideInBytes = stride;
                desc.InitialData = data;
                return device.CreateBuffer(desc);
            };

            model.MeshletBuffer = createImmutable(meshlets.data(), meshlets.size(), sizeof(GpuMeshlet));
            model.MeshletVertexBuffer =
                createImmutable(meshletVertices.data(), meshletVertices.size(), sizeof(uint32_t));
            model.MeshletTriangleBuffer =
                createImmutable(meshletTriangles.data(), meshletTriangles.size(), sizeof(uint32_t));
            if (!model.MeshletBuffer || !model.MeshletVertexBuffer || !model.MeshletTriangleBuffer)
            {
                Core::Logger::Error("ModelLoader", "メッシュレットのバッファ作成に失敗しました");
                model.MeshletBuffer.reset();
                model.MeshletVertexBuffer.reset();
                model.MeshletTriangleBuffer.reset();
                return;
            }

            // メッシュシェーダーはこの3本をResourceDescriptorHeap経由で読む
            const bool registered =
                device.RegisterBindless(model.MeshletBuffer.get()) != RHI::kInvalidBindlessIndex &&
                device.RegisterBindless(model.MeshletVertexBuffer.get()) != RHI::kInvalidBindlessIndex &&
                device.RegisterBindless(model.MeshletTriangleBuffer.get()) != RHI::kInvalidBindlessIndex;
            if (!registered)
            {
                // 無効番号のままメッシュシェーダーへ渡すと未定義動作になる。
                // 表ごと捨てれば描画側は従来の頂点シェーダー経路へ落ちる
                Core::Logger::Error(
                    "ModelLoader", "メッシュレットの表をbindlessへ登録できませんでした(区画が満杯?)");
                model.MeshletBuffer.reset();
                model.MeshletVertexBuffer.reset();
                model.MeshletTriangleBuffer.reset();
                return;
            }

            model.TotalMeshletCount = static_cast<uint32_t>(meshlets.size());

            // 1モデル1ドローにできるかの前提条件。1つでも塊を持たないメッシュがあると、
            // そのメッシュは表に載っておらず、1回のDispatchMeshでは描かれずに消える
            model.AllMeshesHaveMeshlets = true;
            for (const Mesh& mesh : model.Meshes)
            {
                if (mesh.MeshletCount == 0)
                {
                    model.AllMeshesHaveMeshlets = false;
                    Core::Logger::Warning(
                        "ModelLoader",
                        "メッシュレットを持たないメッシュがあるため、このモデルは1ドロー化できません"
                        "(メッシュ単位の描画へ落とします)");
                    break;
                }
            }
        }
    }

    Model LoadModel(RHI::IRHIDevice& device, const std::wstring& filePath, SharedTexturePool* sharedTextures)
    {
        const auto startTime = std::chrono::steady_clock::now();
        const std::wstring directory = GetDirectory(filePath);

        // 既定のstreambufバッファ(通常数百バイト~数KB)のままだと、Bistro級の.kgeom
        // (100MB超)を細切れのreadで読むことになりオーバーヘッドが無視できないため、
        // openより前に大きめ(1MB)のバッファを設定しておく。ioBufferはinより先に構築し
        // (=inより後に破棄され)、in使用中は常に有効な状態を保つ
        std::vector<char> manifestIoBuffer(1 << 20);
        std::ifstream in;
        in.rdbuf()->pubsetbuf(manifestIoBuffer.data(), static_cast<std::streamsize>(manifestIoBuffer.size()));
        in.open(filePath, std::ios::binary);
        if (!in.is_open())
        {
            throw std::runtime_error("モデルパッケージを開けませんでした: " + WideToUtf8(filePath));
        }

        PackageHeader header{};
        std::vector<TextureEntry> textureEntries;
        std::vector<MaterialEntry> materialEntries;
        std::vector<MeshEntry> meshEntries;
        std::vector<LightEntry> lightEntries;
        std::string stringPool;

        try
        {
            in.exceptions(std::ios::failbit | std::ios::badbit);

            in.read(reinterpret_cast<char*>(&header), sizeof(header));
            if (std::memcmp(header.Magic, kPackageMagic, sizeof(kPackageMagic)) != 0)
            {
                throw std::runtime_error("マジックナンバーが不正です");
            }
            if (header.Version != kPackageVersion)
            {
                throw std::runtime_error(
                    "バージョンが対応していません(ファイル: " + std::to_string(header.Version) +
                    ", ランタイム: " + std::to_string(kPackageVersion) + ")");
            }
            if (header.VertexStride != sizeof(Vertex) || header.IndexStride != sizeof(uint32_t))
            {
                throw std::runtime_error("頂点/インデックスのレイアウトが現在のランタイムと一致しません");
            }

            textureEntries.resize(header.TextureCount);
            if (header.TextureCount > 0)
            {
                in.read(reinterpret_cast<char*>(textureEntries.data()), static_cast<std::streamsize>(textureEntries.size() * sizeof(TextureEntry)));
            }

            // マテリアルはテクスチャ番号を参照するのでテクスチャの後ろ、メッシュの前
            // (v10で追加。ModelPackage.hのファイルレイアウト参照)
            materialEntries.resize(header.MaterialCount);
            if (header.MaterialCount > 0)
            {
                in.read(reinterpret_cast<char*>(materialEntries.data()), static_cast<std::streamsize>(materialEntries.size() * sizeof(MaterialEntry)));
            }

            meshEntries.resize(header.MeshCount);
            if (header.MeshCount > 0)
            {
                in.read(reinterpret_cast<char*>(meshEntries.data()), static_cast<std::streamsize>(meshEntries.size() * sizeof(MeshEntry)));
            }

            lightEntries.resize(header.LightCount);
            if (header.LightCount > 0)
            {
                in.read(reinterpret_cast<char*>(lightEntries.data()), static_cast<std::streamsize>(lightEntries.size() * sizeof(LightEntry)));
            }

            stringPool.resize(header.StringPoolSize);
            if (header.StringPoolSize > 0)
            {
                in.read(stringPool.data(), static_cast<std::streamsize>(stringPool.size()));
            }
        }
        catch (const std::exception& e)
        {
            throw std::runtime_error("モデルパッケージの読み込みに失敗しました(" + WideToUtf8(filePath) + "): " + e.what());
        }

        if (meshEntries.empty())
        {
            throw std::runtime_error("モデルパッケージにメッシュが含まれていません: " + WideToUtf8(filePath));
        }

        // StringPoolからジオメトリ/テクスチャのパスを解決する(.kmodel自身のディレクトリからの相対パス)
        const std::wstring geometryPath = directory + Utf8ToWide(
            ReadPoolString(stringPool, header.GeometryPathOffset, header.GeometryPathLength, "GeometryPath"));

        std::vector<std::wstring> texturePaths(textureEntries.size());
        for (size_t i = 0; i < textureEntries.size(); ++i)
        {
            texturePaths[i] = directory + Utf8ToWide(
                ReadPoolString(stringPool, textureEntries[i].PathOffset, textureEntries[i].PathLength, "TexturePath"));
        }

        const auto manifestReadTime = std::chrono::steady_clock::now();

        // .kgeomを読み込む。Bistro級では100MBを超えるため、こちらにも大きめのI/Oバッファを設定する
        std::vector<char> geometryIoBuffer(1 << 20);
        std::ifstream geomIn;
        geomIn.rdbuf()->pubsetbuf(geometryIoBuffer.data(), static_cast<std::streamsize>(geometryIoBuffer.size()));
        geomIn.open(geometryPath, std::ios::binary);
        if (!geomIn.is_open())
        {
            throw std::runtime_error("ジオメトリファイルを開けませんでした: " + WideToUtf8(geometryPath));
        }

        std::vector<uint8_t> geometryPayload;
        try
        {
            geomIn.exceptions(std::ios::failbit | std::ios::badbit);

            GeometryHeader geomHeader{};
            geomIn.read(reinterpret_cast<char*>(&geomHeader), sizeof(geomHeader));
            if (std::memcmp(geomHeader.Magic, kGeometryMagic, sizeof(kGeometryMagic)) != 0)
            {
                throw std::runtime_error("マジックナンバーが不正です");
            }
            if (geomHeader.Version != kGeometryVersion)
            {
                throw std::runtime_error("バージョンが対応していません");
            }
            if (geomHeader.VertexStride != sizeof(Vertex) || geomHeader.IndexStride != sizeof(uint32_t))
            {
                throw std::runtime_error("頂点/インデックスのレイアウトが現在のランタイムと一致しません");
            }

            geometryPayload.resize(geomHeader.PayloadSize);
            if (geomHeader.PayloadSize > 0)
            {
                geomIn.read(reinterpret_cast<char*>(geometryPayload.data()), static_cast<std::streamsize>(geometryPayload.size()));
            }
        }
        catch (const std::exception& e)
        {
            throw std::runtime_error("ジオメトリファイルの読み込みに失敗しました(" + WideToUtf8(geometryPath) + "): " + e.what());
        }

        // 各MeshEntryのオフセット/カウントがペイロード範囲内かを必ず検証する。不正な.kmodelを
        // 読んだ場合にバッファオーバーラン(境界外の頂点/インデックスデータを読む)を防ぐため
        for (size_t i = 0; i < meshEntries.size(); ++i)
        {
            const MeshEntry& mesh = meshEntries[i];
            const uint64_t vertexEnd = mesh.VertexOffset + static_cast<uint64_t>(mesh.VertexCount) * sizeof(Vertex);
            const uint64_t indexEnd = mesh.IndexOffset + static_cast<uint64_t>(mesh.IndexCount) * sizeof(uint32_t);
            // メッシュレットの3ブロックも同様に検証する。カウントが0の場合はオフセットが
            // ペイロード末尾を指しうるが、末尾ちょうどは範囲内として扱ってよい(0バイト読む)
            const uint64_t meshletEnd =
                mesh.MeshletOffset + static_cast<uint64_t>(mesh.MeshletCount) * sizeof(MeshletEntry);
            const uint64_t meshletVertexEnd =
                mesh.MeshletVertexOffset + static_cast<uint64_t>(mesh.MeshletVertexCount) * sizeof(uint32_t);
            const uint64_t meshletTriangleEnd =
                mesh.MeshletTriangleOffset + static_cast<uint64_t>(mesh.MeshletTriangleCount) * sizeof(uint32_t);
            if (vertexEnd > geometryPayload.size() || indexEnd > geometryPayload.size() ||
                meshletEnd > geometryPayload.size() || meshletVertexEnd > geometryPayload.size() ||
                meshletTriangleEnd > geometryPayload.size())
            {
                throw std::runtime_error(
                    "メッシュ[" + std::to_string(i) + "]がジオメトリペイロードの範囲外を参照しています: " + WideToUtf8(geometryPath));
            }
            // メッシュレットの段は、範囲がメッシュレット配列に収まっていなければならない。
            // 段の範囲が壊れていると、描画時に他のメッシュのメッシュレットを掴む
            if (mesh.MeshletLODCount > kMaxMeshletLODCount)
            {
                throw std::runtime_error(
                    "メッシュ[" + std::to_string(i) + "]のメッシュレットLODの段数が上限を超えています: " + WideToUtf8(filePath));
            }
            for (uint32_t lod = 0; lod < mesh.MeshletLODCount; ++lod)
            {
                const uint64_t lodEnd =
                    static_cast<uint64_t>(mesh.MeshletLODOffsets[lod]) + mesh.MeshletLODCounts[lod];
                if (lodEnd > mesh.MeshletCount)
                {
                    throw std::runtime_error(
                        "メッシュ[" + std::to_string(i) + "]のメッシュレットLOD[" + std::to_string(lod) +
                        "]がメッシュレット配列の範囲外です: " + WideToUtf8(filePath));
                }
            }

            if (mesh.MaterialIndex < 0 || mesh.MaterialIndex >= static_cast<int32_t>(materialEntries.size()))
            {
                throw std::runtime_error("メッシュ[" + std::to_string(i) + "]が範囲外のマテリアルを参照しています: " + WideToUtf8(filePath));
            }
        }

        // テクスチャ番号の検証はマテリアル側で行う(v10で材質がMeshEntryから移ったため)
        for (size_t i = 0; i < materialEntries.size(); ++i)
        {
            const MaterialEntry& material = materialEntries[i];
            if (material.BaseColorTextureIndex >= static_cast<int32_t>(textureEntries.size()) ||
                material.NormalTextureIndex >= static_cast<int32_t>(textureEntries.size()) ||
                material.MetallicRoughnessTextureIndex >= static_cast<int32_t>(textureEntries.size()) ||
                material.EmissiveTextureIndex >= static_cast<int32_t>(textureEntries.size()) ||
                material.OcclusionTextureIndex >= static_cast<int32_t>(textureEntries.size()) ||
                material.BentNormalTextureIndex >= static_cast<int32_t>(textureEntries.size()))
            {
                throw std::runtime_error("マテリアル[" + std::to_string(i) + "]が範囲外のテクスチャを参照しています: " + WideToUtf8(filePath));
            }
        }

        const auto geometryReadTime = std::chrono::steady_clock::now();

        Model model;
        model.BoundsMin[0] = header.BoundsMin[0];
        model.BoundsMin[1] = header.BoundsMin[1];
        model.BoundsMin[2] = header.BoundsMin[2];
        model.BoundsMax[0] = header.BoundsMax[0];
        model.BoundsMax[1] = header.BoundsMax[1];
        model.BoundsMax[2] = header.BoundsMax[2];

        TextureLoader textureLoader(device, model, sharedTextures);
        std::vector<RHI::IRHITexture*> resolvedTextures;
        // 自発光の光源プロキシに要るテクスチャだけサムネイルを残す。
        // **係数が0のマテリアルは対象外** ―― 掛け算の結果が黒になるので光源にもならない
        // (BistroMcGuire の看板は map_Ke を持つのに Ke=0 で、画面でも真っ黒)
        std::unordered_set<int32_t> emissiveTextureIndices;
        for (const MaterialEntry& material : materialEntries)
        {
            const bool hasEmissive = material.EmissiveFactor[0] > 0.0f || material.EmissiveFactor[1] > 0.0f ||
                                     material.EmissiveFactor[2] > 0.0f;
            if (hasEmissive && material.EmissiveTextureIndex >= 0)
            {
                emissiveTextureIndices.insert(material.EmissiveTextureIndex);
            }
        }
        std::unordered_map<int32_t, std::vector<float>> emissiveThumbnails;
        textureLoader.LoadAll(texturePaths, resolvedTextures, emissiveTextureIndices, emissiveThumbnails);

        const auto textureLoadTime = std::chrono::steady_clock::now();

        // -1(指定なし)は白/フラット法線、指定されていたのに読み込みに失敗した場合は
        // マゼンタ/フラット法線を使う(詳細はGetMagentaPlaceholder/GetFlatNormalのコメント参照)
        auto resolveBaseColorOrMetallicRoughness = [&](int32_t index) -> RHI::IRHITexture*
        {
            if (index == kNoTextureIndex)
            {
                return textureLoader.GetWhite();
            }
            RHI::IRHITexture* texture = resolvedTextures[static_cast<size_t>(index)];
            return texture ? texture : textureLoader.GetMagentaPlaceholder();
        };
        auto resolveNormal = [&](int32_t index) -> RHI::IRHITexture*
        {
            if (index == kNoTextureIndex)
            {
                return textureLoader.GetFlatNormal();
            }
            RHI::IRHITexture* texture = resolvedTextures[static_cast<size_t>(index)];
            return texture ? texture : textureLoader.GetFlatNormal();
        };
        // 読み込みに失敗した場合も黒(=有効フラグ0)へ落とす。マゼンタのような目立つ色にすると
        // bRawとして解釈された結果が不定になるため、ここは「データ無し」で縮退させるのが正しい
        auto resolveBentNormal = [&](int32_t index) -> RHI::IRHITexture*
        {
            if (index == kNoTextureIndex)
            {
                return textureLoader.GetBlack();
            }
            RHI::IRHITexture* texture = resolvedTextures[static_cast<size_t>(index)];
            return texture ? texture : textureLoader.GetBlack();
        };

        // レイトレーシング用の頂点属性・インデックスを作るか。デバイスが非対応なら作らない
        // (Bistro級では100MB規模になるため、使わない環境で確保しない)
        const bool buildRaytracingGeometry = device.SupportsRaytracing();
        if (buildRaytracingGeometry)
        {
            size_t totalVertexCount = 0;
            size_t totalIndexCount = 0;
            for (const MeshEntry& mesh : meshEntries)
            {
                totalVertexCount += mesh.VertexCount;
                totalIndexCount += mesh.IndexCount;
            }
            model.RaytracingAttributes.reserve(totalVertexCount);
            model.RaytracingIndices.reserve(totalIndexCount);
        }

        // メッシュレットのGPUバッファを作るか。デバイスがメッシュシェーダーに対応していない、
        // あるいは.kmodelが--no-meshletsで焼かれている場合は作らない
        // (読まれないバッファでVRAMを占有しないため。レイトレーシング用配列と同じ考え方)
        const bool buildMeshletGeometry = device.SupportsMeshShader();

        // 頂点/インデックスバッファへSRVを重ねて張り、bindlessで引けるようにするか。
        // 使うのはメッシュシェーダー経路(頂点のみ)とコンピュートシェーダーによる
        // 自前ラスタライザ経路(頂点+インデックス)。
        //
        // 【メッシュシェーダー対応と連動させない】SM 6.6には対応しているがメッシュシェーダーを
        // 持たないGPU(NVIDIA Pascal世代など)では、buildMeshletGeometryがfalseのまま
        // 自前ラスタライザだけが使える。連動させるとその環境でジオメトリを引けなくなる。
        //
        // 追加コストはメッシュあたりSRV 2本ぶんのディスクリプタだけで、
        // バッファ本体は頂点バッファビュー/インデックスバッファビューと同一リソースを共有する
        const bool shaderReadableGeometry = buildMeshletGeometry || device.SupportsSoftwareRaster();

        // モデル単位に連結したメッシュレットの3ブロック(GPUバッファはメッシュのループを
        // 抜けてから1本ずつ作る。理由はループ内のコメント参照)
        std::vector<GpuMeshlet> modelMeshlets;
        // Model::MeshletLODLevelCapは「全メッシュの最小」なので、最初の1件は
        // 比較ではなく代入で入れる(0で初期化したまま min を取ると常に0になる)
        bool meshletLODCapInitialized = false;
        std::vector<uint32_t> modelMeshletVertices;
        std::vector<uint32_t> modelMeshletTriangles;
        if (buildMeshletGeometry)
        {
            size_t totalMeshletCount = 0;
            size_t totalMeshletVertexCount = 0;
            size_t totalMeshletTriangleCount = 0;
            for (const MeshEntry& mesh : meshEntries)
            {
                totalMeshletCount += mesh.MeshletCount;
                totalMeshletVertexCount += mesh.MeshletVertexCount;
                totalMeshletTriangleCount += mesh.MeshletTriangleCount;
            }
            modelMeshlets.reserve(totalMeshletCount);
            modelMeshletVertices.reserve(totalMeshletVertexCount);
            modelMeshletTriangles.reserve(totalMeshletTriangleCount);
        }

        // エミッシブから起こした光源の集計(読み込み後に1行だけログへ出す)。
        // メッシュごとに出すとEmeraldSquareで12行になり、他のログに埋もれる
        uint32_t emissiveMeshCount = 0;
        uint32_t emissiveClusterCount = 0;
        uint32_t emissiveTriangleCount = 0;
        uint32_t emissiveTexturedMeshCount = 0;
        std::vector<float> emissiveTextureLuminances;

        model.Meshes.reserve(meshEntries.size());
        for (const MeshEntry& mesh : meshEntries)
        {
            Mesh outMesh;

            RHI::BufferDesc vertexBufferDesc;
            vertexBufferDesc.Usage = RHI::BufferUsage::Vertex;
            vertexBufferDesc.SizeInBytes = static_cast<uint32_t>(mesh.VertexCount) * sizeof(Vertex);
            vertexBufferDesc.StrideInBytes = sizeof(Vertex);
            vertexBufferDesc.InitialData = geometryPayload.data() + mesh.VertexOffset;
            // メッシュシェーダーには入力アセンブラが無く、頂点は自分でバッファから読む。
            // 同じリソースへ頂点バッファビューとStructuredBuffer<Vertex>のSRVを重ねて張り、
            // 従来経路とメッシュシェーダー経路で1本の頂点バッファを共有する
            // (別に複製するとVRAMを二重に食う)
            vertexBufferDesc.ShaderReadable = shaderReadableGeometry;
            outMesh.VertexBuffer = device.CreateBuffer(vertexBufferDesc);

            RHI::BufferDesc indexBufferDesc;
            indexBufferDesc.Usage = RHI::BufferUsage::Index;
            indexBufferDesc.SizeInBytes = static_cast<uint32_t>(mesh.IndexCount) * sizeof(uint32_t);
            indexBufferDesc.StrideInBytes = sizeof(uint32_t);
            indexBufferDesc.InitialData = geometryPayload.data() + mesh.IndexOffset;
            // 頂点と同じ理由でインデックスバッファにもSRVを重ねる。自前ラスタライザは
            // 三角形番号からインデックスを3つ引くため、StructuredBuffer<uint>として読む
            // (メッシュシェーダー経路はメッシュレット側の間接テーブルを使うのでこれは読まない)
            indexBufferDesc.ShaderReadable = shaderReadableGeometry;
            outMesh.IndexBuffer = device.CreateBuffer(indexBufferDesc);
            outMesh.IndexCount = mesh.IndexCount;
            outMesh.VertexCount = mesh.VertexCount;

            // メッシュ単位のAABBは.kmodel v10がMeshEntryに持っている(パック時に確定した値)。
            // モデルのローカル空間のまま写し、ワールド空間への変換はSceneLoaderが行う
            // (Modelは複数インスタンスから共有されうるため)
            for (int axis = 0; axis < 3; ++axis)
            {
                outMesh.BoundsMin[axis] = mesh.BoundsMin[axis];
                outMesh.BoundsMax[axis] = mesh.BoundsMax[axis];
            }

            // UV密度はフォーマットに持たせていないため、geometryPayloadが生存している
            // ここで求める(GPUへ送った後は頂点バッファとしてしか触れない)
            outMesh.UVPerLocalMeter = EstimateUVPerLocalMeter(
                reinterpret_cast<const Vertex*>(geometryPayload.data() + mesh.VertexOffset), mesh.VertexCount,
                reinterpret_cast<const uint32_t*>(geometryPayload.data() + mesh.IndexOffset), mesh.IndexCount);

            // アセットが持つメッシュレット数。GPUバッファを作るかどうか(下)とは独立で、
            // メッシュシェーダー非対応の環境でもレイトレーシング側が使うため常に控える。
            //
            // 【全段の合計ではなくLOD0の個数を入れる】v10からメッシュレット配列は
            // 離散LODの全段を連結して持つ。描画もレイトレーシングもLOD0だけを見るので、
            // MeshEntry.MeshletCount(全段の合計)をそのまま渡すと、簡略化した段まで
            // 重ねて描かれる/三角形番号の対応が崩れる。段を選ぶのはメッシュレットLODの実装で行う
            outMesh.MeshletCount = mesh.MeshletLODCount > 0 ? mesh.MeshletLODCounts[0] : 0u;
            // 全段の合計と段数。増幅シェーダーが段を選ぶには全段をGPUへ載せる必要がある
            // (Stage 6。載せるのは下のGpuMeshletの詰め替えループ)
            outMesh.MeshletTotalCount = mesh.MeshletCount;
            outMesh.MeshletLODCount = mesh.MeshletLODCount;

            // 頂点/インデックスのbindless番号。メッシュシェーダー経路は頂点を、
            // 自前ラスタライザ経路は両方を、ResourceDescriptorHeap経由で読む。
            // 番号は描画時に定数バッファへ載せて渡すため、ここで一度だけ登録して
            // IRHIBuffer側に覚えさせる(GetBindlessIndexで取り出せる)。
            //
            // 【メッシュレットの有無と連動させない】メッシュレットを持たない.kmodelでも
            // 自前ラスタライザはジオメトリを引く必要がある
            if (shaderReadableGeometry)
            {
                device.RegisterBindless(outMesh.VertexBuffer.get());
                device.RegisterBindless(outMesh.IndexBuffer.get());
            }

            if (buildMeshletGeometry && outMesh.MeshletCount > 0)
            {
                // メッシュレットの3ブロックは**モデル単位の1本へ連結する**。
                // メッシュごとに別バッファのままだと1回のDispatchMeshで1メッシュしか
                // 描けず、メッシュが1,715個あるモデルは必ず1,715ドローになる
                // (Assets::GpuMeshletのコメント参照)。
                //
                // 連結にあたって、ディスク上はメッシュ内相対だったVertexOffset /
                // TriangleOffsetをモデル基準へ付け替える。この足し込みを忘れると
                // 「別のメッシュの頂点で描かれた三角形」が出るが、
                // 位置がでたらめなだけで絵は出てしまうので気づきにくい
                const uint32_t meshletVertexBase = static_cast<uint32_t>(modelMeshletVertices.size());
                const uint32_t meshletTriangleBase = static_cast<uint32_t>(modelMeshletTriangles.size());

                const auto* srcMeshlets =
                    reinterpret_cast<const MeshletEntry*>(geometryPayload.data() + mesh.MeshletOffset);
                const auto* srcMeshletVertices =
                    reinterpret_cast<const uint32_t*>(geometryPayload.data() + mesh.MeshletVertexOffset);
                const auto* srcMeshletTriangles =
                    reinterpret_cast<const uint32_t*>(geometryPayload.data() + mesh.MeshletTriangleOffset);

                // 【全段を載せる】v10からメッシュレット配列は離散LODの全段を連結して持つ。
                // 増幅シェーダーは1つの段だけを選んで描くので、選ばれうる段が表に無いと
                // 選びようがない。**重ねて描かれないのは、増幅シェーダーが
                // 段の一致しない塊を落とすからであって、表に載っていないからではない**
                // (段の判定を外すと全段が同じ場所へ重なって描かれる)。
                //
                // 【間接参照テーブルも全段ぶんをそのまま連結する】各段のメッシュレットが
                // 指す先はブロック内に揃っているので、オフセットの付け替えは段によらず同じ
                outMesh.MeshletOffset = static_cast<uint32_t>(modelMeshlets.size());
                // ディスク上のメッシュレットが本当に所属メッシュと同じ材質を指しているか。
                //
                // 【なぜ確かめるのか】1モデル1ドローは「メッシュレットが材質を跨がない」
                // という前提の上に成り立っている。破れていても絵は出てしまい、
                // 「一部の面だけ別のテクスチャで描かれる」という形でしか現れない。
                // v10はMeshletEntry自身がMaterialIndexを持つので、突き合わせれば
                // パッカーとローダーの食い違いをその場で機械的に検出できる
                bool meshletMaterialMismatch = false;

                for (uint32_t m = 0; m < outMesh.MeshletTotalCount; ++m)
                {
                    const MeshletEntry& src = srcMeshlets[m];
                    if (src.MaterialIndex != static_cast<uint32_t>(mesh.MaterialIndex))
                    {
                        meshletMaterialMismatch = true;
                    }
                    GpuMeshlet dst;
                    dst.VertexOffset = meshletVertexBase + src.VertexOffset;
                    dst.TriangleOffset = meshletTriangleBase + src.TriangleOffset;
                    dst.VertexCount = src.VertexCount;
                    dst.TriangleCount = src.TriangleCount;
                    dst.BoundsCenter[0] = src.BoundsCenter[0];
                    dst.BoundsCenter[1] = src.BoundsCenter[1];
                    dst.BoundsCenter[2] = src.BoundsCenter[2];
                    dst.BoundsRadius = src.BoundsRadius;
                    dst.ConeAxis[0] = src.ConeAxis[0];
                    dst.ConeAxis[1] = src.ConeAxis[1];
                    dst.ConeAxis[2] = src.ConeAxis[2];
                    dst.ConeCutoff = src.ConeCutoff;
                    // 頂点バッファはメッシュ単位のまま。番号は上で登録済み
                    dst.VertexBufferIndex = outMesh.VertexBuffer->GetBindlessIndex();
                    // MaterialIndexと材質のフラグは、メッシュの並びが確定してから
                    // BuildMeshletTablesがまとめて埋める(SortMeshesByMaterialが
                    // メッシュを入れ替えるため、ここで振ると対応が崩れる)。
                    //
                    // 段のビットだけはここで入れる。段はディスク上のMeshletEntryが持つ値で、
                    // メッシュの並び替えとは無関係に決まるため(向こうは材質ビットをORする)
                    dst.Flags = (src.LODLevel & kGpuMeshletLODLevelMask) << kGpuMeshletLODLevelShift;
                    dst.MeshletIndexInMesh = m;
                    modelMeshlets.push_back(dst);
                }

                if (meshletMaterialMismatch)
                {
                    Core::Logger::Error(
                        "ModelLoader",
                        "メッシュレットが所属メッシュと違う材質を指しています(1モデル1ドローの前提が"
                        "破れています): " + WideToUtf8(filePath));
                }

                // 間接参照テーブルは中身を書き換えずそのまま連結してよい
                // (メッシュレット側のオフセットを付け替えたことで辻褄が合う)
                modelMeshletVertices.insert(
                    modelMeshletVertices.end(), srcMeshletVertices,
                    srcMeshletVertices + mesh.MeshletVertexCount);
                modelMeshletTriangles.insert(
                    modelMeshletTriangles.end(), srcMeshletTriangles,
                    srcMeshletTriangles + mesh.MeshletTriangleCount);
            }

            if (buildRaytracingGeometry)
            {
                // geometryPayloadがまだ生存しているこの場でしか元データを読めないため、
                // ここでレイトレーシング用の圧縮属性を作っておく(位置は持たない。理由は
                // RaytracingGeometry.hのコメント参照)
                outMesh.RaytracingAttributeOffset = static_cast<uint32_t>(model.RaytracingAttributes.size());
                outMesh.RaytracingIndexOffset = static_cast<uint32_t>(model.RaytracingIndices.size());

                const auto* vertices = reinterpret_cast<const Vertex*>(geometryPayload.data() + mesh.VertexOffset);
                for (uint32_t v = 0; v < mesh.VertexCount; ++v)
                {
                    model.RaytracingAttributes.push_back(PackRaytracingVertexAttribute(vertices[v].Normal, vertices[v].UV));
                }

                const auto* indices = reinterpret_cast<const uint32_t*>(geometryPayload.data() + mesh.IndexOffset);
                model.RaytracingIndices.insert(model.RaytracingIndices.end(), indices, indices + mesh.IndexCount);

                // ヒットした三角形番号から所属メッシュレットを引くための表。
                // MeshletEntryのうちTriangleOffsetだけを抜き出して詰める
                // (理由はRaytracingScene::GetMeshletTriangleOffsetBufferのコメント参照)
                //
                // 【LOD0だけ詰める】三角形番号はインデックスバッファ上の番号で、
                // インデックスバッファにはLOD0の三角形しか入っていない(.kgeom v4)。
                // 簡略化した段のメッシュレットを混ぜると、TriangleOffsetが昇順でなくなり
                // 二分探索が破綻する
                outMesh.RaytracingMeshletOffset = static_cast<uint32_t>(model.RaytracingMeshletTriangleOffsets.size());
                const auto* meshlets = reinterpret_cast<const MeshletEntry*>(geometryPayload.data() + mesh.MeshletOffset);
                for (uint32_t m = 0; m < outMesh.MeshletCount; ++m)
                {
                    model.RaytracingMeshletTriangleOffsets.push_back(meshlets[m].TriangleOffset);
                }
            }

            // 材質はv10からMaterialEntry側にある。番号の範囲は上の検証で確認済み。
            //
            // 【Assets::Meshの持ち方は変えない】ランタイムの構造体はメッシュごとに材質を
            // 持ったままで、ここで転記する。描画側(レンダラ・シェーダー)に一切影響を出さず、
            // フォーマットの変更をこの1関数へ閉じ込めるため
            const MaterialEntry& material = materialEntries[static_cast<size_t>(mesh.MaterialIndex)];

            outMesh.BaseColorTexture = resolveBaseColorOrMetallicRoughness(material.BaseColorTextureIndex);
            outMesh.NormalTexture = resolveNormal(material.NormalTextureIndex);
            outMesh.MetallicRoughnessTexture = resolveBaseColorOrMetallicRoughness(material.MetallicRoughnessTextureIndex);
            outMesh.EmissiveTexture = resolveBaseColorOrMetallicRoughness(material.EmissiveTextureIndex);
            // 遮蔽マップも未指定なら白1x1(=遮蔽なし)へフォールバックさせればよいので、
            // BaseColor/MetallicRoughnessと同じ解決を再利用する
            outMesh.OcclusionTexture = resolveBaseColorOrMetallicRoughness(material.OcclusionTextureIndex);
            outMesh.OcclusionStrength = material.OcclusionStrength;
            // bent normalだけは白ではなく黒(=有効フラグ0)へ落とす。理由はGetBlackのコメント参照
            outMesh.BentNormalTexture = resolveBentNormal(material.BentNormalTextureIndex);
            outMesh.MetallicFactor = material.MetallicFactor;
            outMesh.RoughnessFactor = material.RoughnessFactor;
            outMesh.AlphaCutoff = material.AlphaCutoff;
            outMesh.Translucency = material.Translucency;
            outMesh.IsTransparent = (material.Flags & kMeshEntryFlagTransparent) != 0;
            outMesh.BaseColorFactor[0] = material.BaseColorFactor[0];
            outMesh.BaseColorFactor[1] = material.BaseColorFactor[1];
            outMesh.BaseColorFactor[2] = material.BaseColorFactor[2];
            outMesh.BaseColorFactor[3] = material.BaseColorFactor[3];
            outMesh.EmissiveFactor[0] = material.EmissiveFactor[0];
            outMesh.EmissiveFactor[1] = material.EmissiveFactor[1];
            outMesh.EmissiveFactor[2] = material.EmissiveFactor[2];

            // エミッシブなメッシュを光源のかたまりへ分ける。geometryPayloadが生存している
            // ここでしか元データを読めないため、UV密度の見積もりと同じ場所で行う。
            //
            // 【判定は係数そのもので行う。材質名で選んではいけない】BistroMcGuire の
            // exterior.mtl には `Spotlight_Emissive` という名前で Ke=0 の材質があり、
            // 逆に map_Ke を持つ看板(Shopsign_Book_Store)も Ke=0 なので画面では真っ黒になる。
            // GBuffer.hlsl は係数とテクスチャを乗算するので、係数が0の面は光って見えない。
            // 係数で判定すれば「光源化される集合」と「画面で光って見える集合」が厳密に一致する
            if (outMesh.EmissiveFactor[0] > 0.0f || outMesh.EmissiveFactor[1] > 0.0f ||
                outMesh.EmissiveFactor[2] > 0.0f)
            {
                // 自発光テクスチャの平均色。**係数と掛け合わせたものが実際の放射輝度**で、
                // 係数だけを見るとテクスチャの黒い部分まで光る面として数えて過大評価する
                if (material.EmissiveTextureIndex >= 0)
                {
                    const auto found = emissiveThumbnails.find(material.EmissiveTextureIndex);
                    if (found != emissiveThumbnails.end())
                    {
                        float uvMin[2] = { 0.0f, 0.0f };
                        float uvMax[2] = { 1.0f, 1.0f };
                        ComputeUVBounds(
                            reinterpret_cast<const Vertex*>(geometryPayload.data() + mesh.VertexOffset),
                            mesh.VertexCount,
                            reinterpret_cast<const uint32_t*>(geometryPayload.data() + mesh.IndexOffset),
                            mesh.IndexCount, uvMin, uvMax);
                        AverageThumbnailRect(
                            found->second, kEmissiveThumbnailSize, uvMin, uvMax, outMesh.EmissiveTextureAverage);
                        // 【全部1.0なら取れていない、全部0.0なら真っ黒を掛けている】
                        // どちらも絵からは分からないので、分布をログに出せるよう控える
                        emissiveTextureLuminances.push_back(
                            0.2126f * outMesh.EmissiveTextureAverage[0] +
                            0.7152f * outMesh.EmissiveTextureAverage[1] +
                            0.0722f * outMesh.EmissiveTextureAverage[2]);
                    }
                    else
                    {
                        // 取り出せなかった。白のまま(過大評価)になるので数えておく
                        emissiveTexturedMeshCount += 1u;
                    }
                }
                const auto* const meshVertices =
                    reinterpret_cast<const Vertex*>(geometryPayload.data() + mesh.VertexOffset);
                const auto* const meshIndices =
                    reinterpret_cast<const uint32_t*>(geometryPayload.data() + mesh.IndexOffset);
                std::vector<uint32_t> triangleCluster;
                outMesh.EmissiveClusters = BuildEmissiveClusters(
                    meshVertices, mesh.VertexCount, meshIndices, mesh.IndexCount,
                    outMesh.BoundsMin, outMesh.BoundsMax, kEmissiveClusterScale, &triangleCluster);
                BuildEmissiveTriangles(
                    meshVertices, mesh.VertexCount, meshIndices, triangleCluster, outMesh.EmissiveClusters,
                    outMesh.EmissiveTriangles);
                emissiveMeshCount += 1u;
                emissiveClusterCount += static_cast<uint32_t>(outMesh.EmissiveClusters.size());
                emissiveTriangleCount += static_cast<uint32_t>(outMesh.EmissiveTriangles.size());

                // 【三角形の総面積はかたまりの総面積と一致しなければならない】
                // 並べ替えでどれかを取りこぼしても、かたまりの範囲がずれても、
                // 絵はそれらしく出てしまう。ここで数として突き合わせておく
                double triangleArea = 0.0;
                for (const EmissiveTriangle& tri : outMesh.EmissiveTriangles)
                {
                    const double cross[3] = {
                        static_cast<double>(tri.E1[1]) * tri.E2[2] - static_cast<double>(tri.E1[2]) * tri.E2[1],
                        static_cast<double>(tri.E1[2]) * tri.E2[0] - static_cast<double>(tri.E1[0]) * tri.E2[2],
                        static_cast<double>(tri.E1[0]) * tri.E2[1] - static_cast<double>(tri.E1[1]) * tri.E2[0],
                    };
                    triangleArea +=
                        0.5 * std::sqrt(cross[0] * cross[0] + cross[1] * cross[1] + cross[2] * cross[2]);
                }
                double clusterArea = 0.0;
                for (const EmissiveCluster& cluster : outMesh.EmissiveClusters)
                {
                    clusterArea += cluster.Area;
                }
                if (clusterArea > 0.0 && std::fabs(triangleArea - clusterArea) > 1e-4 * clusterArea)
                {
                    Core::Logger::Warning(
                        "ModelLoader",
                        "エミッシブ三角形の総面積がかたまりの総面積と一致しません: 三角形 " +
                            std::to_string(triangleArea) + " m^2 / かたまり " + std::to_string(clusterArea) +
                            " m^2(材質 " + std::to_string(mesh.MaterialIndex) + ")");
                }
            }

            // LOD0の三角形数を積む(メッシュレットLODのしきい値の基準。Model::TotalTriangleCount)
            model.TotalTriangleCount += outMesh.IndexCount / 3;

            // モデルが選べる最も粗い段は、全メッシュが持っている段の共通部分。
            // 【メッシュレットを持たないメッシュは数えない】そのメッシュは
            // メッシュシェーダー経路に載らないので、段の上限を縛る理由が無い
            if (outMesh.MeshletLODCount > 0)
            {
                const uint32_t meshCap = outMesh.MeshletLODCount - 1u;
                model.MeshletLODLevelCap = meshletLODCapInitialized
                    ? std::min(model.MeshletLODLevelCap, meshCap)
                    : meshCap;
                meshletLODCapInitialized = true;
            }
            model.Meshes.push_back(std::move(outMesh));
        }

        // 【0件でも黙らない】エミッシブなメッシュが有るのにかたまりが0個なら、
        // 溶接や縮退の判定が効きすぎている。逆にメッシュ1個から数十個出ていたら
        // 溶接が効いていない(法線違いで頂点が割れたまま連結成分を取った形)。
        // どちらも絵からは分からないので、数を出しておく
        if (emissiveMeshCount > 0)
        {
            Core::Logger::Info(
                "ModelLoader",
                "エミッシブな光源: " + std::to_string(emissiveClusterCount) + "個(" +
                    std::to_string(emissiveMeshCount) + "メッシュ由来 / 三角形 " +
                    std::to_string(emissiveTriangleCount) + "枚)");
            if (!emissiveTextureLuminances.empty())
            {
                std::sort(emissiveTextureLuminances.begin(), emissiveTextureLuminances.end());
                char buffer[192];
                std::snprintf(
                    buffer, sizeof(buffer),
                    "自発光テクスチャの平均色: %zuメッシュ / 輝度 最小 %.4f 中央 %.4f 最大 %.4f",
                    emissiveTextureLuminances.size(), emissiveTextureLuminances.front(),
                    emissiveTextureLuminances[emissiveTextureLuminances.size() / 2],
                    emissiveTextureLuminances.back());
                Core::Logger::Info("ModelLoader", buffer);
            }
            if (emissiveTexturedMeshCount > 0)
            {
                Core::Logger::Warning(
                    "ModelLoader",
                    "自発光テクスチャの平均色を取り出せなかったメッシュが " +
                        std::to_string(emissiveTexturedMeshCount) +
                        "個あります。白として扱うため、これらの光源プロキシは実際より明るくなります");
            }
        }

        model.Lights.reserve(lightEntries.size());
        for (const LightEntry& entry : lightEntries)
        {
            Light light;
            light.Type = static_cast<LightType>(entry.Type);
            light.Position[0] = entry.Position[0];
            light.Position[1] = entry.Position[1];
            light.Position[2] = entry.Position[2];
            light.Direction[0] = entry.Direction[0];
            light.Direction[1] = entry.Direction[1];
            light.Direction[2] = entry.Direction[2];
            light.Color[0] = entry.Color[0];
            light.Color[1] = entry.Color[1];
            light.Color[2] = entry.Color[2];
            light.Intensity = entry.Intensity;
            light.Range = entry.Range;
            light.SpotInnerConeAngle = entry.SpotInnerConeAngle;
            light.SpotOuterConeAngle = entry.SpotOuterConeAngle;
            light.Enabled = entry.Enabled != 0;
            light.Name = ReadPoolString(stringPool, entry.NameOffset, entry.NameLength, "LightName");

            model.Lights.push_back(std::move(light));
        }

        // アルファカットアウトの有無はアセット固有の性質なので、**デバイスの機能に依らず**ここで決める。
        // BuildMaterialTable の中で立てると、bindless非対応(DX11)ではテーブル自体を作らないため
        // 「カットアウトを持つモデルなのに常にfalse」になる。今の使用箇所はメッシュレット経路の
        // 内側だけなので実害は出ないが、バックエンドで 0/1 が変わる値を
        // アセットの性質として持たせるべきではない
        for (const Mesh& mesh : model.Meshes)
        {
            if (mesh.AlphaCutoff > 0.0f)
            {
                model.HasCutoutMaterial = true;
                break;
            }
        }

        SortMeshesByMaterial(model);
        // マテリアルテーブルはメッシュの並びが確定してから作る(番号がずれるため)。
        // メッシュレットの表はさらにその後 ―― 塊1件ごとにマテリアル番号を書き込むため
        BuildMaterialTable(device, model);
        BuildMeshletTables(device, model, modelMeshlets, modelMeshletVertices, modelMeshletTriangles);

        const auto endTime = std::chrono::steady_clock::now();
        Core::Logger::Info(
            "ModelLoader",
            "モデル読み込み完了: " + WideToUtf8(filePath) +
            " (マニフェスト " + FormatMs(startTime, manifestReadTime) + "ms" +
            " / ジオメトリ " + FormatMs(manifestReadTime, geometryReadTime) + "ms" +
            " / テクスチャ " + FormatMs(geometryReadTime, textureLoadTime) + "ms" +
            " / 合計 " + FormatMs(startTime, endTime) + "ms" +
            ", テクスチャ要求 " + std::to_string(texturePaths.size()) + "件)");

        return model;
    }
}
