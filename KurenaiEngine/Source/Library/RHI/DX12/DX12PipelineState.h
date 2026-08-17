#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include "RHI/IRHIPipelineState.h"
#include "RHI/RHIEnums.h"

namespace Kurenai::RHI
{
    class DX12PipelineState : public IRHIPipelineState
    {
    public:
        // isMeshPipeline: 増幅/メッシュシェーダーで作ったPSOか。
        // 通常の描画とはルートシグネチャが別(DX12Device::GetMeshRootSignature)で、
        // 入力アセンブラを持たないためプリミティブトポロジの設定も不要になる。
        // DX12CommandList::SetPipelineStateがこのフラグでどちらを束ねるかを決める
        DX12PipelineState(
            Microsoft::WRL::ComPtr<ID3D12PipelineState> pipelineState, PrimitiveTopology topology, bool isMeshPipeline = false);

        ID3D12PipelineState* GetPipelineState() const { return m_PipelineState.Get(); }
        D3D12_PRIMITIVE_TOPOLOGY GetTopology() const;
        bool IsMeshPipeline() const { return m_IsMeshPipeline; }

    private:
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_PipelineState;
        PrimitiveTopology m_Topology;
        bool m_IsMeshPipeline = false;
    };
}
