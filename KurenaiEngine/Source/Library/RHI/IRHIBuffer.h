#pragma once

#include "KurenaiTypes.h"

#include "RHIBindless.h"

namespace Kurenai::RHI
{
    class KURENAI_LIB_API IRHIBuffer
    {
    public:
        virtual ~IRHIBuffer() = default;

        // このバッファのSRVがbindlessヒープの何番に登録されているか(意味と登録の仕方は
        // IRHITexture::GetBindlessIndexと同じ)。SRVを持たないUsage(Vertex/Index/Constant)や
        // 未登録の場合はkInvalidBindlessIndex。
        //
        // メッシュシェーダーがメッシュレットのジオメトリ(頂点・MeshletVertex・MeshletTriangle)を
        // 引くのに使う。メッシュシェーダーには入力アセンブラが無いため、頂点バッファを
        // 「頂点バッファとして」バインドする手段が無く、構造化バッファとして自分で読むしかない
        virtual uint32_t GetBindlessIndex() const { return kInvalidBindlessIndex; }
    };
}
