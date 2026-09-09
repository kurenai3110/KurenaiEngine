#include "DDGIGrid.h"

#include <algorithm>
#include <cmath>

// DDGIのクリップマップ格子の幾何。宣言と設計の意図は DDGIGrid.h にある
namespace Kurenai::GI
{
    DirectX::XMFLOAT3 DDGIGrid::ComputeLODSpacing(uint32_t lod) const
    {
        // LODが1つ上がるごとに間隔が2倍(=覆う範囲が2倍)
        const float scale = static_cast<float>(1u << lod);
        return DirectX::XMFLOAT3{
            m_Volume.ProbeSpacing[0] * scale,
            m_Volume.ProbeSpacing[1] * scale,
            m_Volume.ProbeSpacing[2] * scale,
        };
    }

    DirectX::XMINT3 DDGIGrid::ComputeLODBaseIndex(uint32_t lod) const
    {
        const DirectX::XMFLOAT3 spacing = ComputeLODSpacing(lod);
        const int32_t countX = static_cast<int32_t>(m_Volume.ProbeCounts[0]);
        const int32_t countY = static_cast<int32_t>(m_Volume.ProbeCounts[1]);
        const int32_t countZ = static_cast<int32_t>(m_Volume.ProbeCounts[2]);

        if (!m_Volume.FollowCamera)
        {
            // 追従しないので格子は動かない。基準は0でよい
            // (トロイダルの写像は基準が何であっても自己整合するが、動かないなら0が素直)
            return DirectX::XMINT3{ 0, 0, 0 };
        }

        // 【そのLOD自身の格子へスナップする】スナップすれば、カメラが動いてもプローブの
        // ワールド座標は動かない。動くのは「どのプローブが範囲に入っているか」だけになり、
        // 範囲に残ったプローブの焼き上がりをそのまま使える。
        // スナップしないと毎フレーム全プローブが焼き直しになる
        const DirectX::XMFLOAT3 camera = m_FollowCenter;
        const auto snap = [](float centerValue, float spacingValue, int32_t count) -> int32_t
        {
            if (spacingValue <= 0.0f)
            {
                return 0;
            }
            const int32_t centerIndex = static_cast<int32_t>(std::floor(centerValue / spacingValue));
            // カメラを格子の中央へ置く
            return centerIndex - (count - 1) / 2;
        };

        return DirectX::XMINT3{
            snap(camera.x, spacing.x, countX),
            snap(camera.y, spacing.y, countY),
            snap(camera.z, spacing.z, countZ),
        };
    }

    DirectX::XMFLOAT3 DDGIGrid::ComputeLODOrigin(uint32_t lod) const
    {
        const DirectX::XMFLOAT3 spacing = ComputeLODSpacing(lod);

        if (m_Volume.FollowCamera)
        {
            const DirectX::XMINT3 base = ComputeLODBaseIndex(lod);
            return DirectX::XMFLOAT3{
                static_cast<float>(base.x) * spacing.x,
                static_cast<float>(base.y) * spacing.y,
                static_cast<float>(base.z) * spacing.z,
            };
        }

        // 追従しない場合、LOD0は.ksceneのOriginをそのまま使う(既存シーンの挙動を1ビットも
        // 変えないため)。上のLODは同じ中心を保ったまま広がるように置く
        if (lod == 0)
        {
            return DirectX::XMFLOAT3{ m_Volume.Origin[0], m_Volume.Origin[1], m_Volume.Origin[2] };
        }

        const auto centered = [this, &spacing](int axis) -> float
        {
            const float count = static_cast<float>(m_Volume.ProbeCounts[axis]);
            const float extent0 = (count - 1.0f) * m_Volume.ProbeSpacing[axis];
            const float extentK = (count - 1.0f) * (&spacing.x)[axis];
            return m_Volume.Origin[axis] + (extent0 - extentK) * 0.5f;
        };
        return DirectX::XMFLOAT3{ centered(0), centered(1), centered(2) };
    }

    DirectX::XMINT3 DDGIGrid::ComputeProbeWorldCoord(uint32_t probeIndex) const
    {
        const uint32_t countX = m_Volume.ProbeCounts[0];
        const uint32_t countY = m_Volume.ProbeCounts[1];

        const uint32_t perLOD = std::max(1u, m_ProbesPerLOD);
        const uint32_t lod = std::min(probeIndex / perLOD, m_LODCount - 1u);
        const uint32_t local = probeIndex - lod * perLOD;

        const int32_t atlasX = static_cast<int32_t>(local % countX);
        const int32_t atlasY = static_cast<int32_t>((local / countX) % countY);
        const int32_t atlasZ = static_cast<int32_t>(local / (countX * countY));

        const DirectX::XMINT3 base = ComputeLODBaseIndex(lod);
        const auto unwrap = [](int32_t atlasCoord, int32_t baseCoord, int32_t count) -> int32_t
        {
            return baseCoord + ((atlasCoord - baseCoord) % count + count) % count;
        };

        return DirectX::XMINT3{
            unwrap(atlasX, base.x, static_cast<int32_t>(countX)),
            unwrap(atlasY, base.y, static_cast<int32_t>(m_Volume.ProbeCounts[1])),
            unwrap(atlasZ, base.z, static_cast<int32_t>(m_Volume.ProbeCounts[2])),
        };
    }

    DirectX::XMFLOAT3 DDGIGrid::ComputeProbePosition(uint32_t probeIndex) const
    {
        const uint32_t countX = m_Volume.ProbeCounts[0];
        const uint32_t countY = m_Volume.ProbeCounts[1];
        const uint32_t countZ = m_Volume.ProbeCounts[2];

        // 通し番号 → LOD段 → その段の中の位置
        const uint32_t perLOD = std::max(1u, m_ProbesPerLOD);
        const uint32_t lod = std::min(probeIndex / perLOD, m_LODCount - 1u);
        const uint32_t local = probeIndex - lod * perLOD;

        // アトラス上の格子座標(セルの並びそのもの)
        const int32_t atlasX = static_cast<int32_t>(local % countX);
        const int32_t atlasY = static_cast<int32_t>((local / countX) % countY);
        const int32_t atlasZ = static_cast<int32_t>(local / (countX * countY));

        const DirectX::XMFLOAT3 spacing = ComputeLODSpacing(lod);
        const DirectX::XMFLOAT3 origin = ComputeLODOrigin(lod);
        const DirectX::XMINT3 base = ComputeLODBaseIndex(lod);

        // 【トロイダル(剰余)addressingの逆変換】アトラスのセル番号は
        // 「ワールド格子座標 mod プローブ数」なので、いま範囲に入っている区間
        // [base, base + count) の中で、その剰余に一致する格子座標を1つ選び直す。
        // これがそのセルがいま担当しているプローブのワールド位置になる
        const auto unwrap = [](int32_t atlasCoord, int32_t baseCoord, int32_t count) -> int32_t
        {
            const int32_t offset = ((atlasCoord - baseCoord) % count + count) % count;
            return offset;
        };

        const int32_t localX = unwrap(atlasX, base.x, static_cast<int32_t>(countX));
        const int32_t localY = unwrap(atlasY, base.y, static_cast<int32_t>(countY));
        const int32_t localZ = unwrap(atlasZ, base.z, static_cast<int32_t>(countZ));

        return DirectX::XMFLOAT3{
            origin.x + static_cast<float>(localX) * spacing.x,
            origin.y + static_cast<float>(localY) * spacing.y,
            origin.z + static_cast<float>(localZ) * spacing.z,
        };
    }
}
