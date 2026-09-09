#pragma once

#include <cstdint>

#include <DirectXMath.h>

#include "Assets/Scene.h"

// DDGI(動的拡散GI)のクリップマップ格子。
//
// 【この型が持つのは幾何だけ】プローブの通し番号とワールド座標の相互変換、および
// LOD段ごとの間隔・原点・基準添字を出す。アトラスや定数バッファといった資源は
// Rendering::GIResources が、焼き込みの進行状態は Passes::DDGIPasses が持つ。
//
// 【ボリュームは参照で持つ】Assets::GIVolume の実体は GIResources 側にあり、
// シーンを読み込み直すと中身が入れ替わる。写しを持つと入れ替えを取りこぼす。
namespace Kurenai::GI
{
    class DDGIGrid
    {
    public:
        explicit DDGIGrid(const Assets::GIVolume& volume) : m_Volume(volume) {}

        // アトラスを確保し直したときに、実際に使うLOD段数とLOD1段ぶんのプローブ数を入れ直す
        void Configure(uint32_t lodCount, uint32_t probesPerLOD)
        {
            m_LODCount = lodCount;
            m_ProbesPerLOD = probesPerLOD;
        }

        // 格子を追従させる中心(カメラのワールド座標)。
        // 【フレームの先頭で1回だけ入れる】格子の原点・プローブ位置・dirty判定・
        // シェーダーへ渡す値がすべてこれを基準に決まるので、1フレームの途中で
        // 動かすと食い違う
        void SetFollowCenter(const DirectX::XMFLOAT3& center) { m_FollowCenter = center; }

        uint32_t GetLODCount() const { return m_LODCount; }
        uint32_t GetProbesPerLOD() const { return m_ProbesPerLOD; }

        DirectX::XMFLOAT3 ComputeLODSpacing(uint32_t lod) const;
        DirectX::XMINT3 ComputeLODBaseIndex(uint32_t lod) const;
        DirectX::XMFLOAT3 ComputeLODOrigin(uint32_t lod) const;
        DirectX::XMINT3 ComputeProbeWorldCoord(uint32_t probeIndex) const;
        DirectX::XMFLOAT3 ComputeProbePosition(uint32_t probeIndex) const;

    private:
        const Assets::GIVolume& m_Volume;

        // 実際に使うLOD段数(GIVolume.LODCountをkDDGIMaxLODCountでクランプしたもの)
        uint32_t m_LODCount = 1;
        // LOD 1段ぶんのプローブ数(ProbeCountsの3軸の積)。通し番号からLODを割り出すのに使う
        uint32_t m_ProbesPerLOD = 1;

        DirectX::XMFLOAT3 m_FollowCenter{ 0.0f, 0.0f, 0.0f };
    };
}
