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

        // このバッファへ1フレームのあいだに安全にUpdateBufferできる回数。
        //
        // 【なぜ上位層が知る必要があるのか】DX12はCPUがGPUの完了を待たずに先行して記録するため、
        // 定数バッファをリング状の複数スロットで持ち、UpdateBufferのたびに次のスロットへ進む。
        // 1フレームでリングを一周すると、GPUがまだ読んでいるスロットを上書きして描画が壊れる。
        // 書き込み回数がシーンの内容で決まるパス ―― 例えばDDGIのラスタ経路は
        // 「プローブ数 × 6面 × 不透明メッシュ数」だけ書く ―― は、自分の仕事量が
        // この上限に収まるかを確かめ、収まらないなら仕事量の側を減らさなければならない。
        //
        // DX11はUpdateBufferごとにMap(WRITE_DISCARD)でドライバに領域をリネームさせるため
        // リングを持たず、実質的な上限が無い(既定のUINT32_MAXがそのまま返る)
        virtual uint32_t GetSafeUpdatesPerFrame() const { return UINT32_MAX; }
    };
}
