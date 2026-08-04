#include "DroneShow.h"

#include <algorithm>
#include <cmath>

#include "Core/Logger.h"

namespace Kurenai
{
    namespace
    {
        using DirectX::XMFLOAT3;

        constexpr float kPi = 3.14159265358979323846f;
        // 黄金角。フィボナッチ球で点を極に偏らせずに撒くのに使う
        const float kGoldenAngle = kPi * (3.0f - std::sqrt(5.0f));

        // 決定的な整数ハッシュ(Wang hash)。機体ごとの揺れの位相など、
        // 「毎回同じでなければならないばらつき」に使う。実行時の時刻からは決して取らない
        uint32_t HashUInt(uint32_t x)
        {
            x = (x ^ 61u) ^ (x >> 16);
            x *= 9u;
            x = x ^ (x >> 4);
            x *= 0x27d4eb2du;
            x = x ^ (x >> 15);
            return x;
        }

        // [0,1)の決定的な擬似乱数
        float Hash01(uint32_t seed, uint32_t index, uint32_t salt)
        {
            const uint32_t h = HashUInt(HashUInt(seed + salt) ^ (index * 2654435761u));
            return static_cast<float>(h & 0x00FFFFFFu) / static_cast<float>(0x01000000u);
        }

        float Lerp(float a, float b, float t) { return a + (b - a) * t; }

        XMFLOAT3 Lerp3(const XMFLOAT3& a, const XMFLOAT3& b, float t)
        {
            return XMFLOAT3(Lerp(a.x, b.x, t), Lerp(a.y, b.y, t), Lerp(a.z, b.z, t));
        }

        float SmoothStep01(float t)
        {
            t = std::clamp(t, 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        }

        // --- 形状の生成 -------------------------------------------------------------
        // どれも「原点中心・代表半径1」の正規化された空間へ点を撒く。
        // ワールドへの配置(Scale倍してCenterへ移動)は呼び出し側でまとめて行う。
        // 引数countは1以上であることを呼び出し側が保証する

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

        // 形ごとの色。xyz=線形RGB。編隊内で下端の色から上端の色へ補間する
        struct FormationPalette
        {
            XMFLOAT3 Low;
            XMFLOAT3 High;
        };

        FormationPalette PaletteFor(FormationShape shape)
        {
            switch (shape)
            {
            // 【彩度を保つこと】上端に白に近い色(例: 0.85,0.95,1.00)を置くと、加算合成のうえ
            // ACESを通る過程で編隊がほぼ真っ白に見えてしまい、色が変わるという見どころが消える
            case FormationShape::Sphere: return { XMFLOAT3(0.05f, 0.25f, 1.00f), XMFLOAT3(0.20f, 0.90f, 1.00f) };
            case FormationShape::Ring:   return { XMFLOAT3(1.00f, 0.35f, 0.05f), XMFLOAT3(1.00f, 0.85f, 0.20f) };
            case FormationShape::Helix:  return { XMFLOAT3(0.10f, 1.00f, 0.55f), XMFLOAT3(0.20f, 0.60f, 1.00f) };
            case FormationShape::Grid:   return { XMFLOAT3(0.85f, 0.10f, 0.55f), XMFLOAT3(0.20f, 0.30f, 1.00f) };
            case FormationShape::Heart:  return { XMFLOAT3(1.00f, 0.05f, 0.18f), XMFLOAT3(1.00f, 0.45f, 0.60f) };
            case FormationShape::Spiral: return { XMFLOAT3(0.65f, 0.20f, 1.00f), XMFLOAT3(1.00f, 0.90f, 0.55f) };
            default:                     return { XMFLOAT3(1.0f, 1.0f, 1.0f), XMFLOAT3(1.0f, 1.0f, 1.0f) };
            }
        }
    }

    const char* FormationShapeName(FormationShape shape)
    {
        switch (shape)
        {
        case FormationShape::Sphere: return "球";
        case FormationShape::Ring:   return "円環";
        case FormationShape::Helix:  return "二重らせん";
        case FormationShape::Grid:   return "格子";
        case FormationShape::Heart:  return "ハート";
        case FormationShape::Spiral: return "らせん";
        default:                     return "(不明)";
        }
    }

    void DroneShow::Configure(const DroneShowSettings& settings)
    {
        m_Configured = false;
        m_Settings = settings;

        if (m_Settings.Count == 0u)
        {
            Core::Logger::Error("DroneShow", "機体数が0のため編隊を生成できません");
            return;
        }

        const uint32_t count = m_Settings.Count;

        for (uint32_t shapeIndex = 0; shapeIndex < static_cast<uint32_t>(FormationShape::Count); ++shapeIndex)
        {
            const FormationShape shape = static_cast<FormationShape>(shapeIndex);
            Formation& formation = m_Formations[shapeIndex];
            formation.Positions.clear();
            formation.Colors.clear();
            formation.Positions.reserve(count);
            formation.Colors.reserve(count);

            switch (shape)
            {
            case FormationShape::Sphere: GenerateSphere(count, formation.Positions); break;
            case FormationShape::Ring:   GenerateRing(count, formation.Positions); break;
            case FormationShape::Helix:  GenerateHelix(count, formation.Positions); break;
            case FormationShape::Grid:   GenerateGrid(count, formation.Positions); break;
            case FormationShape::Heart:  GenerateHeart(count, formation.Positions); break;
            case FormationShape::Spiral: GenerateSpiral(count, formation.Positions); break;
            default: break;
            }

            // 生成関数が層構造の都合でcountちょうどを返さないことがある(RingやHeartは
            // 「1層あたりの数×層数」で刻むため)。多い分は捨て、足りない分は最後の点で埋めて
            // 必ずcount個に揃える。ここが揃っていないと形どうしのインデックス対応が崩れる
            if (formation.Positions.size() > count)
            {
                formation.Positions.resize(count);
            }
            while (formation.Positions.size() < count)
            {
                formation.Positions.push_back(formation.Positions.empty() ? XMFLOAT3(0.0f, 0.0f, 0.0f) : formation.Positions.back());
            }

            // 【モーフで軌跡を交差させないための並べ替え】
            // 形Aのi番目と形Bのi番目を直線で結ぶのがモーフなので、両者のインデックスが
            // まったく無関係だと全機の軌跡が編隊の中央で交差し、変形の途中がただの
            // 塊になってしまう。そこでどの形も「中心から見た方位角、次に高さ」の順に
            // 並べ替えておくと、i番目どうしが常に概ね同じ方角・同じ高さに来るため、
            // 各機は自分の近くへ短く動くだけで済む。
            // 生成順そのものには意味を持たせない(この並べ替えが唯一の対応規則)
            std::stable_sort(
                formation.Positions.begin(), formation.Positions.end(),
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

            // 正規化空間での高さから色を決める(並べ替えの後に行うこと。
            // 色は位置に付随するので、順序を変えたあとで割り当てないと形と色がずれる)
            float minY = formation.Positions[0].y;
            float maxY = formation.Positions[0].y;
            for (const XMFLOAT3& p : formation.Positions)
            {
                minY = std::min(minY, p.y);
                maxY = std::max(maxY, p.y);
            }
            const float span = std::max(1e-4f, maxY - minY);
            const FormationPalette palette = PaletteFor(shape);
            for (const XMFLOAT3& p : formation.Positions)
            {
                formation.Colors.push_back(Lerp3(palette.Low, palette.High, (p.y - minY) / span));
            }

            // 正規化空間からワールドへ。ここで初めてScaleとCenterが効く
            for (XMFLOAT3& p : formation.Positions)
            {
                p.x = p.x * m_Settings.Scale + m_Settings.Center.x;
                p.y = p.y * m_Settings.Scale + m_Settings.Center.y;
                p.z = p.z * m_Settings.Scale + m_Settings.Center.z;
            }
        }

        m_Configured = true;
        Core::Logger::Info(
            "DroneShow",
            "編隊を生成しました (機体数: " + std::to_string(count) + ", 形状: " +
                std::to_string(static_cast<uint32_t>(FormationShape::Count)) + ", 1巡: " +
                std::to_string(LoopDuration()) + "秒)");
    }

    void DroneShow::UpdateTimingSettings(float radius, float holdSeconds, float morphSeconds, float hoverAmplitude)
    {
        if (!m_Configured)
        {
            return;
        }
        m_Settings.Radius = radius;
        m_Settings.HoldSeconds = holdSeconds;
        m_Settings.MorphSeconds = morphSeconds;
        m_Settings.HoverAmplitude = hoverAmplitude;
    }

    float DroneShow::LoopDuration() const
    {
        if (!m_Configured)
        {
            return 0.0f;
        }
        const float segment = std::max(0.01f, m_Settings.HoldSeconds + m_Settings.MorphSeconds);
        return segment * static_cast<float>(FormationShape::Count);
    }

    FormationShape DroneShow::CurrentShape(float showTime) const
    {
        if (!m_Configured)
        {
            return FormationShape::Sphere;
        }
        const float segment = std::max(0.01f, m_Settings.HoldSeconds + m_Settings.MorphSeconds);
        const float loop = LoopDuration();
        float t = std::fmod(showTime, loop);
        if (t < 0.0f)
        {
            t += loop;
        }
        const uint32_t index = std::min(
            static_cast<uint32_t>(FormationShape::Count) - 1u, static_cast<uint32_t>(t / segment));
        return static_cast<FormationShape>(index);
    }

    void DroneShow::Evaluate(float showTime, std::vector<GPUDrone>& outDrones) const
    {
        outDrones.clear();
        if (!m_Configured)
        {
            return;
        }

        const uint32_t count = m_Settings.Count;
        const uint32_t shapeCount = static_cast<uint32_t>(FormationShape::Count);
        const float segment = std::max(0.01f, m_Settings.HoldSeconds + m_Settings.MorphSeconds);
        const float loop = segment * static_cast<float>(shapeCount);

        float t = std::fmod(showTime, loop);
        if (t < 0.0f)
        {
            t += loop;
        }
        const uint32_t fromIndex = std::min(shapeCount - 1u, static_cast<uint32_t>(t / segment));
        const uint32_t toIndex = (fromIndex + 1u) % shapeCount;
        const float local = t - static_cast<float>(fromIndex) * segment;

        const Formation& from = m_Formations[fromIndex];
        const Formation& to = m_Formations[toIndex];

        outDrones.resize(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            // 【機体ごとに遷移の開始をずらす】全機が同時に動き出すと編隊全体が1つの
            // 剛体のように見える。0〜25%ぶんだけ出発を散らすと、実際のショーのように
            // 端から順に崩れて次の形へ組み上がっていく
            const float stagger = Hash01(m_Settings.Seed, i, 0x51u) * 0.25f;
            const float morphSpan = std::max(0.01f, m_Settings.MorphSeconds * (1.0f - stagger));
            const float morphStart = m_Settings.HoldSeconds + m_Settings.MorphSeconds * stagger;
            const float blend = SmoothStep01((local - morphStart) / morphSpan);

            XMFLOAT3 position = Lerp3(from.Positions[i], to.Positions[i], blend);
            const XMFLOAT3 color = Lerp3(from.Colors[i], to.Colors[i], blend);

            // ホバリング。機体ごとに位相と周期を散らした正弦波で、静止中も微かに揺れる。
            // showTimeから決まるので、時間を凍結すれば同じ絵が再現できる
            if (m_Settings.HoverAmplitude > 0.0f)
            {
                const float phaseX = Hash01(m_Settings.Seed, i, 0x11u) * 2.0f * kPi;
                const float phaseY = Hash01(m_Settings.Seed, i, 0x22u) * 2.0f * kPi;
                const float phaseZ = Hash01(m_Settings.Seed, i, 0x33u) * 2.0f * kPi;
                const float speed = 0.5f + Hash01(m_Settings.Seed, i, 0x44u) * 0.5f;
                const float a = m_Settings.HoverAmplitude;
                position.x += a * std::sin(showTime * speed + phaseX);
                position.y += a * std::sin(showTime * speed * 1.3f + phaseY);
                position.z += a * std::sin(showTime * speed * 0.7f + phaseZ);
            }

            GPUDrone& drone = outDrones[i];
            drone.Position = position;
            drone.Radius = m_Settings.Radius;
            drone.Color = color;
            // 機体ごとの明るさのばらつき。全機が完全に同じ輝度だと人工的に見える。
            // 明るさの絶対値(と実効プリ露出)は描画側が定数バッファで一括して掛ける
            drone.Intensity = 0.85f + Hash01(m_Settings.Seed, i, 0x77u) * 0.30f;
        }
    }
}
