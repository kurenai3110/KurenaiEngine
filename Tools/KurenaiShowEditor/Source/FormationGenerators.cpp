#include "FormationGenerators.h"

#include <algorithm>
#include <cmath>

namespace Kurenai::ShowEditor
{
    namespace
    {
        using DirectX::XMFLOAT3;

        constexpr float kPi = 3.14159265358979323846f;
        // 黄金角。フィボナッチ球で点を極に偏らせずに撒くのに使う
        const float kGoldenAngle = kPi * (3.0f - std::sqrt(5.0f));

        float Lerp(float a, float b, float t) { return a + (b - a) * t; }

        XMFLOAT3 Lerp3(const XMFLOAT3& a, const XMFLOAT3& b, float t)
        {
            return XMFLOAT3(Lerp(a.x, b.x, t), Lerp(a.y, b.y, t), Lerp(a.z, b.z, t));
        }

        void GenerateSphere(uint32_t count, std::vector<XMFLOAT3>& out)
        {
            // フィボナッチ球。yを-1〜1で等間隔に取り、方位角に黄金角を積むことで
            // 緯度線に沿った密集(素朴な球座標の格子で必ず起きる)を避ける
            for (uint32_t i = 0; i < count; ++i)
            {
                const float t = (count > 1u) ? (static_cast<float>(i) / static_cast<float>(count - 1u)) : 0.5f;
                const float y = 1.0f - 2.0f * t;
                const float r = std::sqrt(std::max(0.0f, 1.0f - y * y));
                const float theta = kGoldenAngle * static_cast<float>(i);
                out.emplace_back(r * std::cos(theta), y, r * std::sin(theta));
            }
        }

        void GenerateRing(uint32_t count, std::vector<XMFLOAT3>& out)
        {
            // 水平な円環を高さ方向に数段。段ごとに半径を変えて樽形にする
            constexpr uint32_t kLevels = 5u;
            const uint32_t perLevel = std::max(1u, count / kLevels);
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t level = std::min(kLevels - 1u, i / perLevel);
                const uint32_t indexInLevel = i - level * perLevel;
                const float levelT = (kLevels > 1u) ? (static_cast<float>(level) / static_cast<float>(kLevels - 1u)) : 0.5f;
                // 中央の段ほど太い樽形
                const float radius = 0.65f + 0.35f * std::sin(levelT * kPi);
                const float angle = 2.0f * kPi * static_cast<float>(indexInLevel) / static_cast<float>(perLevel);
                out.emplace_back(radius * std::cos(angle), (levelT - 0.5f) * 1.2f, radius * std::sin(angle));
            }
        }

        void GenerateHelix(uint32_t count, std::vector<XMFLOAT3>& out)
        {
            // 二重らせん。2本を位相180度でずらして絡ませる
            constexpr float kTurns = 3.0f;
            for (uint32_t i = 0; i < count; ++i)
            {
                const float t = (count > 1u) ? (static_cast<float>(i) / static_cast<float>(count - 1u)) : 0.5f;
                const float strand = (i % 2u == 0u) ? 0.0f : kPi;
                const float angle = t * kTurns * 2.0f * kPi + strand;
                out.emplace_back(0.75f * std::cos(angle), (t - 0.5f) * 2.0f, 0.75f * std::sin(angle));
            }
        }

        // 鉛直な平面(XY平面)へ点を撒く形状が3つあるため、共通の向きをここで決めておく。
        // 平面の法線は+Z、すなわち編隊は「Z軸の手前側から見て正しく見える」向きに立つ
        void GenerateGrid(uint32_t count, std::vector<XMFLOAT3>& out)
        {
            // 縦横比がおよそ4:3になる格子
            const uint32_t columns = std::max(1u, static_cast<uint32_t>(std::round(std::sqrt(static_cast<float>(count) * 4.0f / 3.0f))));
            const uint32_t rows = std::max(1u, (count + columns - 1u) / columns);
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t cx = i % columns;
                const uint32_t cy = i / columns;
                const float u = (columns > 1u) ? (static_cast<float>(cx) / static_cast<float>(columns - 1u) - 0.5f) : 0.0f;
                const float v = (rows > 1u) ? (static_cast<float>(cy) / static_cast<float>(rows - 1u) - 0.5f) : 0.0f;
                out.emplace_back(u * 2.0f, v * 1.5f, 0.0f);
            }
        }

        void GenerateHeart(uint32_t count, std::vector<XMFLOAT3>& out)
        {
            // ハート曲線 x=16sin^3(t), y=13cos(t)-5cos(2t)-2cos(3t)-cos(4t) を輪郭とし、
            // 中心へ向かって縮小した相似形を層状に重ねて塗りつぶす。
            // 輪郭だけだと機体数が多いときに線が濃くなりすぎて形が潰れる
            constexpr uint32_t kLayers = 12u;
            const uint32_t perLayer = std::max(1u, count / kLayers);
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t layer = std::min(kLayers - 1u, i / perLayer);
                const uint32_t indexInLayer = i - layer * perLayer;
                // 外周ほど点を多く見せたいので、層の縮小率は線形ではなく平方根で詰める
                const float shrink = std::sqrt(1.0f - static_cast<float>(layer) / static_cast<float>(kLayers));
                const float t = 2.0f * kPi * static_cast<float>(indexInLayer) / static_cast<float>(perLayer);
                const float s = std::sin(t);
                const float x = 16.0f * s * s * s;
                const float y = 13.0f * std::cos(t) - 5.0f * std::cos(2.0f * t) - 2.0f * std::cos(3.0f * t) - std::cos(4.0f * t);
                // 上の式のxyはおよそ±17の範囲なので、代表半径1へ正規化する
                out.emplace_back(x / 17.0f * shrink, y / 17.0f * shrink, 0.0f);
            }
        }

        void GenerateSpiral(uint32_t count, std::vector<XMFLOAT3>& out)
        {
            // 対数らせんの腕を数本。銀河のように中心が密で外へ流れる
            constexpr uint32_t kArms = 3u;
            constexpr float kTurns = 1.5f;
            for (uint32_t i = 0; i < count; ++i)
            {
                const float t = (count > 1u) ? (static_cast<float>(i) / static_cast<float>(count - 1u)) : 0.5f;
                const float arm = static_cast<float>(i % kArms) / static_cast<float>(kArms) * 2.0f * kPi;
                // 半径を平方根で取ると面積あたりの密度が一定になり、中心だけが極端に濃くならない
                const float radius = std::sqrt(t);
                const float angle = t * kTurns * 2.0f * kPi + arm;
                out.emplace_back(radius * std::cos(angle), radius * std::sin(angle), 0.0f);
            }
        }
    }

    const char* GeneratorName(GeneratorKind kind)
    {
        switch (kind)
        {
        case GeneratorKind::Sphere: return "球";
        case GeneratorKind::Ring:   return "円環";
        case GeneratorKind::Helix:  return "二重らせん";
        case GeneratorKind::Grid:   return "格子";
        case GeneratorKind::Heart:  return "ハート";
        case GeneratorKind::Spiral: return "らせん";
        default:                    return "(取り込み)";
        }
    }

    FormationPalette DefaultPalette(GeneratorKind kind)
    {
        switch (kind)
        {
        // 【彩度を保つこと】上端に白に近い色(例: 0.85,0.95,1.00)を置くと、加算合成のうえ
        // ACESを通る過程で編隊がほぼ真っ白に見えてしまい、色が変わるという見どころが消える
        case GeneratorKind::Sphere: return { XMFLOAT3(0.05f, 0.25f, 1.00f), XMFLOAT3(0.20f, 0.90f, 1.00f) };
        case GeneratorKind::Ring:   return { XMFLOAT3(1.00f, 0.35f, 0.05f), XMFLOAT3(1.00f, 0.85f, 0.20f) };
        case GeneratorKind::Helix:  return { XMFLOAT3(0.10f, 1.00f, 0.55f), XMFLOAT3(0.20f, 0.60f, 1.00f) };
        case GeneratorKind::Grid:   return { XMFLOAT3(0.85f, 0.10f, 0.55f), XMFLOAT3(0.20f, 0.30f, 1.00f) };
        case GeneratorKind::Heart:  return { XMFLOAT3(1.00f, 0.05f, 0.18f), XMFLOAT3(1.00f, 0.45f, 0.60f) };
        case GeneratorKind::Spiral: return { XMFLOAT3(0.65f, 0.20f, 1.00f), XMFLOAT3(1.00f, 0.90f, 0.55f) };
        default:                    return { XMFLOAT3(1.0f, 1.0f, 1.0f), XMFLOAT3(1.0f, 1.0f, 1.0f) };
        }
    }

    void GenerateFormation(GeneratorKind kind, uint32_t count, std::vector<XMFLOAT3>& outPositions)
    {
        outPositions.clear();
        if (count == 0u)
        {
            return;
        }
        outPositions.reserve(count);

        switch (kind)
        {
        case GeneratorKind::Sphere: GenerateSphere(count, outPositions); break;
        case GeneratorKind::Ring:   GenerateRing(count, outPositions); break;
        case GeneratorKind::Helix:  GenerateHelix(count, outPositions); break;
        case GeneratorKind::Grid:   GenerateGrid(count, outPositions); break;
        case GeneratorKind::Heart:  GenerateHeart(count, outPositions); break;
        case GeneratorKind::Spiral: GenerateSpiral(count, outPositions); break;
        default: break;
        }

        // 生成関数が層構造の都合でcountちょうどを返さないことがある(RingやHeartは
        // 「1層あたりの数×層数」で刻むため)。多い分は捨て、足りない分は最後の点で埋めて
        // 必ずcount個に揃える。ここが揃っていないと形どうしのインデックス対応が崩れる
        if (outPositions.size() > count)
        {
            outPositions.resize(count);
        }
        while (outPositions.size() < count)
        {
            outPositions.push_back(outPositions.empty() ? XMFLOAT3(0.0f, 0.0f, 0.0f) : outPositions.back());
        }

        // 【モーフで軌跡を交差させないための並べ替え】
        // 形Aのi番目と形Bのi番目を直線で結ぶのがモーフなので、両者のインデックスが
        // まったく無関係だと全機の軌跡が編隊の中央で交差し、変形の途中がただの
        // 塊になってしまう。そこでどの形も「中心から見た方位角、次に高さ」の順に
        // 並べ替えておくと、i番目どうしが常に概ね同じ方角・同じ高さに来るため、
        // 各機は自分の近くへ短く動くだけで済む。
        // 生成順そのものには意味を持たせない(この並べ替えが唯一の対応規則)。
        //
        // 【この並べ替えはここで焼き込むこと】対応づけはデータの性質であって、再生する側が
        // 決め直すことではない。.kshow内の順序がそのまま対応関係になり、
        // エンジンはi番目とi番目を結ぶだけで済む
        std::stable_sort(
            outPositions.begin(), outPositions.end(),
            [](const XMFLOAT3& a, const XMFLOAT3& b)
            {
                const float azimuthA = std::atan2(a.z, a.x);
                const float azimuthB = std::atan2(b.z, b.x);
                if (azimuthA != azimuthB)
                {
                    return azimuthA < azimuthB;
                }
                return a.y < b.y;
            });
    }

    void PaintByHeight(
        const std::vector<XMFLOAT3>& positions, const FormationPalette& palette, std::vector<XMFLOAT3>& outColors)
    {
        outColors.clear();
        if (positions.empty())
        {
            return;
        }
        outColors.reserve(positions.size());

        float minY = positions[0].y;
        float maxY = positions[0].y;
        for (const XMFLOAT3& p : positions)
        {
            minY = std::min(minY, p.y);
            maxY = std::max(maxY, p.y);
        }
        // 平面格子のように高さが1点へ潰れる形でも0除算にならないよう下限を置く
        const float span = std::max(1e-4f, maxY - minY);
        for (const XMFLOAT3& p : positions)
        {
            outColors.push_back(Lerp3(palette.Low, palette.High, (p.y - minY) / span));
        }
    }

    void ResamplePoints(const std::vector<XMFLOAT3>& source, uint32_t newCount, std::vector<XMFLOAT3>& out)
    {
        out.clear();
        if (newCount == 0u)
        {
            return;
        }
        if (source.empty())
        {
            out.assign(newCount, XMFLOAT3(0.0f, 0.0f, 0.0f));
            return;
        }

        out.reserve(newCount);
        const size_t sourceCount = source.size();
        for (uint32_t i = 0; i < newCount; ++i)
        {
            // 元の並びを保ったまま等間隔に拾う。並びは方位角順に焼き込まれているので、
            // 間引いても増やしても「方位角順」という対応規則自体は壊れない
            const size_t index = (newCount > 1u)
                ? static_cast<size_t>(
                      static_cast<double>(i) * static_cast<double>(sourceCount - 1) / static_cast<double>(newCount - 1) + 0.5)
                : 0u;
            out.push_back(source[std::min(index, sourceCount - 1)]);
        }
    }
}
