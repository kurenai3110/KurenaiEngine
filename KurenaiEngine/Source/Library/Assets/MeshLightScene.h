#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "KurenaiTypes.h"

#include "RHI/IRHIBuffer.h"
#include "RHI/IRHIDevice.h"

#include "MeshLight.h"
#include "Scene.h"

#pragma warning(push)
#pragma warning(disable: 4251)

namespace Kurenai::Assets
{
    // シーン1つ分のメッシュライト資源(段階2)。
    // ワールド空間の三角形テーブルを1本持ち、MegaLights の各パスがそれを読む。
    //
    // 【段階1のプロキシと同じ集合から作る】Build は Scene::EmissiveProxies を走査し、
    // 各プロキシが指すかたまりの三角形だけをワールドへ移す。
    // **判定を2箇所に置かない**ための構造で、こうしておけば「プロキシは出たのに
    // 三角形が出ない」「三角形は出たのにプロキシが無い」がそもそも起こらない
    // (どちらも絵は出るので、起きてからでは気付けない)。
    //
    // シーンは読み込み後に変形しないので、構築し直さない(RaytracingScene と同じ前提)。
    class KURENAI_LIB_API MeshLightScene
    {
    public:
        MeshLightScene() = default;
        ~MeshLightScene() = default;

        MeshLightScene(const MeshLightScene&) = delete;
        MeshLightScene& operator=(const MeshLightScene&) = delete;
        MeshLightScene(MeshLightScene&&) = default;
        MeshLightScene& operator=(MeshLightScene&&) = default;

        // シーンから三角形テーブルを構築する。
        // 三角形が1枚も無い(エミッシブなメッシュが無い)場合は false を返し、
        // このオブジェクトは何も保持しない状態(IsValid()==false)になる。
        //
        // cutoffIrradiance は影響半径 R を解くための打ち切り照度。
        // **フレーム不変の定数を渡すこと** ―― 露出から導くと参照実装が非決定的になり、
        // 「同じ入力で同じ真値が出る」という参照実装の存在理由が消える
        bool Build(RHI::IRHIDevice& device, const Scene& scene, float cutoffIrradiance);

        // 保持しているGPUリソースを解放する。
        // 【重要】呼ぶ前に IRHIDevice::WaitForGPUIdle() でGPUの実行完了を待つこと
        void Reset();

        bool IsValid() const { return m_TriangleBuffer != nullptr; }

        RHI::IRHIBuffer* GetTriangleBuffer() const { return m_TriangleBuffer.get(); }

        // 統計(ImGuiの表示・ログ用)
        uint32_t GetTriangleCount() const { return m_TriangleCount; }
        uint32_t GetClusterCount() const { return m_ClusterCount; }
        // 全三角形の総面積[m^2](ワールド空間)。段階1のプロキシの総面積と一致するはず
        double GetTotalArea() const { return m_TotalArea; }
        // 全三角形の総光束(輝度換算)。露出前
        double GetTotalFlux() const { return m_TotalFlux; }
        uint64_t GetTriangleBufferBytes() const { return m_TriangleBufferBytes; }
        // 影響半径が上限で切られた三角形の数。**切った量はエネルギーを落としている**ので、
        // 0でないなら上限かτを疑う
        uint32_t GetRadiusClampedCount() const { return m_RadiusClampedCount; }

    private:
        std::unique_ptr<RHI::IRHIBuffer> m_TriangleBuffer;

        uint32_t m_TriangleCount = 0;
        uint32_t m_ClusterCount = 0;
        double m_TotalArea = 0.0;
        double m_TotalFlux = 0.0;
        uint64_t m_TriangleBufferBytes = 0;
        uint32_t m_RadiusClampedCount = 0;
    };
}

#pragma warning(pop)
