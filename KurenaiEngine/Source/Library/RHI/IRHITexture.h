#pragma once

#include "KurenaiTypes.h"

#include "RHIBindless.h"

namespace Kurenai::RHI
{
    class KURENAI_LIB_API IRHITexture
    {
    public:
        virtual ~IRHITexture() = default;

        // このテクスチャのSRVがbindlessヒープの何番に登録されているか。
        // 登録されていない(あるいはバックエンドがbindless非対応の)場合はkInvalidBindlessIndex。
        //
        // 登録はIRHIDevice::RegisterBindlessが行う。全テクスチャを自動登録しないのは、
        // bindless区画の容量が有限(DX12Device::kBindlessDescriptorCapacity)で、
        // レンダーターゲットや中間バッファのように固定スロット経由でしか読まないテクスチャまで
        // 登録すると無駄に食い潰すため。「シェーダーから動的な番号で選びたいもの」だけを登録する
        virtual uint32_t GetBindlessIndex() const { return kInvalidBindlessIndex; }
    };
}
