#include "DroneShow.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "Core/Logger.h"

namespace Kurenai
{
    namespace
    {
        using DirectX::XMFLOAT3;

        constexpr float kPi = 3.14159265358979323846f;

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
    }

    void DroneShow::SetData(const Assets::ShowData& data)
    {
        m_HasData = false;
        m_Data = Assets::ShowData{};

        // 空のデータは「ショーなし」を意味する正当な状態(ドローンショーを持たないシーンへ
        // 切り替えたとき、前のショーを消すためにこれが渡ってくる)。エラーにはしない
        if (data.DroneCount == 0u || data.Formations.empty())
        {
            return;
        }

        // 【点数が揃っていることをここでも確かめる】モーフは形Aのi番目と形Bのi番目を結ぶだけで、
        // 揃っていないと添字が範囲外になる。ShowLoaderが書き出し時に検査しているが、
        // ApplyDroneShowData(エディタのプレビュー)はファイルを経由せずここへ入ってくるため、
        // 受け取る側でも独立に検査する
        for (size_t f = 0; f < data.Formations.size(); ++f)
        {
            const Assets::ShowFormation& formation = data.Formations[f];
            if (formation.Positions.size() != data.DroneCount || formation.Colors.size() != data.DroneCount)
            {
                Core::Logger::Error(
                    "DroneShow",
                    "編隊" + std::to_string(f) + "の点数が機体数と一致しないため再生できません(位置: " +
                        std::to_string(formation.Positions.size()) + ", 色: " +
                        std::to_string(formation.Colors.size()) + ", 機体数: " +
                        std::to_string(data.DroneCount) + ")");
                return;
            }
        }

        m_Data = data;
        m_HasData = true;
        Core::Logger::Info(
            "DroneShow",
            "ショーを設定しました (機体数: " + std::to_string(m_Data.DroneCount) + ", 編隊: " +
                std::to_string(m_Data.Formations.size()) + ", 1巡: " + std::to_string(LoopDuration()) + "秒)");
    }

    float DroneShow::LoopDuration() const
    {
        return m_HasData ? Assets::ShowLoopDuration(m_Data) : 0.0f;
    }

    void DroneShow::Evaluate(
        float showTime, const DirectX::XMFLOAT3& center, float scale, std::vector<GPUDrone>& outDrones) const
    {
        outDrones.clear();
        if (!m_HasData)
        {
            return;
        }

        const uint32_t count = m_Data.DroneCount;
        const uint32_t formationCount = static_cast<uint32_t>(m_Data.Formations.size());
        const float segment = std::max(0.01f, m_Data.HoldSeconds + m_Data.MorphSeconds);
        const float loop = segment * static_cast<float>(formationCount);

        float t = std::fmod(showTime, loop);
        if (t < 0.0f)
        {
            t += loop;
        }
        const uint32_t fromIndex = std::min(formationCount - 1u, static_cast<uint32_t>(t / segment));
        const uint32_t toIndex = (fromIndex + 1u) % formationCount;
        const float local = t - static_cast<float>(fromIndex) * segment;

        const Assets::ShowFormation& from = m_Data.Formations[fromIndex];
        const Assets::ShowFormation& to = m_Data.Formations[toIndex];

        outDrones.resize(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            // 【機体ごとに遷移の開始をずらす】全機が同時に動き出すと編隊全体が1つの
            // 剛体のように見える。0〜25%ぶんだけ出発を散らすと、実際のショーのように
            // 端から順に崩れて次の形へ組み上がっていく
            const float stagger = Hash01(m_Data.Seed, i, 0x51u) * 0.25f;
            const float morphSpan = std::max(0.01f, m_Data.MorphSeconds * (1.0f - stagger));
            const float morphStart = m_Data.HoldSeconds + m_Data.MorphSeconds * stagger;
            const float blend = SmoothStep01((local - morphStart) / morphSpan);

            // 正規化空間で補間してからワールドへ移す。ここで初めてScaleとCenterが効く
            const XMFLOAT3 local3 = Lerp3(from.Positions[i], to.Positions[i], blend);
            XMFLOAT3 position(
                local3.x * scale + center.x, local3.y * scale + center.y, local3.z * scale + center.z);
            const XMFLOAT3 color = Lerp3(from.Colors[i], to.Colors[i], blend);

            // ホバリング。機体ごとに位相と周期を散らした正弦波で、静止中も微かに揺れる。
            // 振幅はワールドの実寸[m]なのでScaleを掛けた後に足す。
            // showTimeから決まるので、同じ時刻なら同じ絵が再現できる
            if (m_Data.HoverAmplitude > 0.0f)
            {
                const float phaseX = Hash01(m_Data.Seed, i, 0x11u) * 2.0f * kPi;
                const float phaseY = Hash01(m_Data.Seed, i, 0x22u) * 2.0f * kPi;
                const float phaseZ = Hash01(m_Data.Seed, i, 0x33u) * 2.0f * kPi;
                const float speed = 0.5f + Hash01(m_Data.Seed, i, 0x44u) * 0.5f;
                const float a = m_Data.HoverAmplitude;
                position.x += a * std::sin(showTime * speed + phaseX);
                position.y += a * std::sin(showTime * speed * 1.3f + phaseY);
                position.z += a * std::sin(showTime * speed * 0.7f + phaseZ);
            }

            GPUDrone& drone = outDrones[i];
            drone.Position = position;
            drone.Radius = m_Data.Radius;
            drone.Color = color;
            // 機体ごとの明るさのばらつき。全機が完全に同じ輝度だと人工的に見える。
            // 明るさの絶対値(と実効プリ露出)は描画側が定数バッファで一括して掛ける
            drone.Intensity = 0.85f + Hash01(m_Data.Seed, i, 0x77u) * 0.30f;
        }
    }
}
