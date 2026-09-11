#pragma once

#include <cstdint>
#include <memory>

#include "RHI/IRHIBuffer.h"
#include "RHI/IRHIDevice.h"

#include "../Passes/GeometryConstants.h"

// カリング統計をGPUから読み戻す受け皿。
//
// 【なぜリングなのか】GPUを待たずに読むため、リングの中で**最も古いもの**
// (kRingSize-1 = 2フレーム前に書いたもの)を読む。DX12はkFrameCount(=2)フレームぶん
// CPUが先行するので、2フレーム前のGPU実行は完了している。待ちを入れるとフレームが
// 直列化し、計測のために計測対象を壊す。
//
// 【リングの添字をここだけが持つ理由】書き込み先を選ぶのも、読む位置を決めるのも、
// 進めるのも同じ添字である。呼ぶ側と両方で持つと、片方だけ進めたときに
// 「GPUが書いた値」と「同じフレームのCPUの値」がずれる。**この食い違いは絵に出ず、
// 中間バッファにもパスマニフェストにも現れない**ので、採取では捕まらない。
namespace Kurenai::Diagnostics
{
    class CullStatsReadback
    {
    public:
        // 【段数の理由】コピーを積んだ直後に読んでもGPUはまだ実行していない。
        // DX12はkFrameCount(=2)フレームぶんCPUが先行するので、3本持って
        // 「2フレーム前に書いたもの」を読めばGPUの完了を待たずに済む。
        // メッシュレット統計とモデルカリングで段数を揃えてある(どちらも同じ理由)
        static constexpr uint32_t kRingSize = 3;

        // --- メッシュレットカリングの統計 ---

        // 例外を投げうる。呼ぶ側が捕まえて ResetMeshletRing() する
        void CreateMeshletRing(RHI::IRHIDevice& device, uint32_t counterCount);
        void ResetMeshletRing();
        // 今フレームGPUが書き込む先
        RHI::IRHIBuffer* GetMeshletWriteSlot() const { return m_MeshletRing[m_MeshletIndex].get(); }
        // 最も古いスロットを読み、**読めても読めなくても添字を進める**。
        // 読めたときだけ true(DX11のMap(DO_NOT_WAIT)はまだ実行中ならfalseを返す。
        // 0として集計に足すと間引き率が実際より低く出るので、そのフレームは丸ごと飛ばす)
        bool ResolveMeshlet(uint32_t* outCounters, size_t byteCount);

        void SetMeshletBindlessIndex(uint32_t index) { m_MeshletBindlessIndex = index; }
        uint32_t GetMeshletBindlessIndex() const { return m_MeshletBindlessIndex; }

        // --- モデル単位のGPUカリング ---

        void CreateModelRing(RHI::IRHIDevice& device, uint32_t counterCount);
        void ResetModelRing();
        RHI::IRHIBuffer* GetModelWriteSlot() const { return m_ModelRing[m_ModelIndex].get(); }

        // 今フレームのCPU側の結果を積み、最も古いGPUの結果を読み、添字を進める。
        //
        // 【GPUの数値は2フレーム遅れなので、CPU側も同じだけ遅らせて比べる】今フレームの
        // CPU値と2フレーム前のGPU値を比べると、カメラが動いている間は常に食い違って見える。
        // 同じリングに積んで、同じフレームのもの同士を比べる
        void ResolveModel(uint32_t cpuFrustumCulled, uint32_t candidateCount);
        // 統計を切っている間に古い値が残っていると、UIやログが「今もこの数だけ
        // 間引いている」ように見える。切った時点で0へ戻す
        void ClearModelStats();

        uint32_t GetModelTested() const { return m_ModelTested; }
        uint32_t GetModelFrustumCulled() const { return m_ModelFrustumCulled; }
        uint32_t GetModelOcclusionCulled() const { return m_ModelOcclusionCulled; }
        uint32_t GetModelSurvived() const { return m_ModelSurvived; }
        // 区画ごとにGPUが実際に発行したドロー数。ここが0のまま絵が出ているなら、
        // 間接描画ではなく従来のCPUループが描いている
        uint32_t GetModelRegionIssued(uint32_t region) const { return m_ModelRegionIssued[region]; }
        // GPUの数値と同じフレームのCPU側の値(ログの比較に使う)
        uint32_t GetModelComparedCpuFrustumCulled() const { return m_ModelComparedCpuFrustumCulled; }
        uint32_t GetModelComparedCandidateCount() const { return m_ModelComparedCandidateCount; }

    private:
        std::unique_ptr<RHI::IRHIBuffer> m_MeshletRing[kRingSize];
        uint32_t m_MeshletIndex = 0;
        uint32_t m_MeshletBindlessIndex = RHI::kInvalidBindlessIndex;

        std::unique_ptr<RHI::IRHIBuffer> m_ModelRing[kRingSize];
        uint32_t m_ModelIndex = 0;

        // 直近に読み戻せた値(判定 / 視錐台で間引き / オクルージョンで間引き / 生き残り)
        uint32_t m_ModelTested = 0;
        uint32_t m_ModelFrustumCulled = 0;
        uint32_t m_ModelOcclusionCulled = 0;
        uint32_t m_ModelSurvived = 0;
        uint32_t m_ModelRegionIssued[Passes::kModelCullRegionCount]{};

        // GPUの数値と突き合わせるためのCPU側の値を積むリング
        uint32_t m_ModelCpuFrustumHistory[kRingSize]{};
        uint32_t m_ModelCandidateHistory[kRingSize]{};
        uint32_t m_ModelComparedCpuFrustumCulled = 0;
        uint32_t m_ModelComparedCandidateCount = 0;
    };
}
