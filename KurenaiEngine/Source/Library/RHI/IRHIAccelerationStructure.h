#pragma once

#include "KurenaiTypes.h"

namespace Kurenai::RHI
{
    // レイトレーシングの高速化構造(Acceleration Structure)の不透明ハンドル。
    // IRHIBuffer/IRHITextureと同じく、中身はバックエンド実装(DX12AccelerationStructure)側が持ち、
    // 上位層はこのポインタをIRHICommandList::SetComputeAccelerationStructureへ渡すだけでよい。
    //
    // BLAS(Bottom Level)とTLAS(Top Level)を同じ型で表す。両者はD3D12上でも同一のリソース種別
    // (D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTUREのバッファ)で、違うのは
    // 構築時の入力(BLAS=三角形ジオメトリ / TLAS=BLASへの参照を持つインスタンス配列)だけのため。
    // ただしシェーダーへバインドできるのはTLASのみで(SRVを持つのはTLASだけ)、BLASを
    // SetComputeAccelerationStructureへ渡すとログを出して無視される
    class KURENAI_LIB_API IRHIAccelerationStructure
    {
    public:
        virtual ~IRHIAccelerationStructure() = default;
    };
}
