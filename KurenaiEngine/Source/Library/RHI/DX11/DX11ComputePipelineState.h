#pragma once

#include "RHI/IRHIPipelineState.h"

namespace Kurenai::RHI
{
    class DX11Shader;

    // グラフィックスのDX11PipelineStateとは異なり入力レイアウト・ラスタライザ関連のステートを持たず、
    // コンピュートシェーダー本体の参照のみを保持する
    class DX11ComputePipelineState : public IRHIPipelineState
    {
    public:
        explicit DX11ComputePipelineState(DX11Shader* computeShader);

        DX11Shader* GetComputeShader() const { return m_ComputeShader; }

    private:
        DX11Shader* m_ComputeShader;
    };
}
