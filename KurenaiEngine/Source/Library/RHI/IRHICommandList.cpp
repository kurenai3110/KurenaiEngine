#include "IRHICommandList.h"

#include <string>

#include "Core/Logger.h"

// IRHICommandList の非仮想の検証層。**この4つの規則が両バックエンドで同じであることを、
// 型の側で保証するためのファイル**(理由は IRHICommandList.h の冒頭)。
//
//   1. どんな引数を断るか
//   2. 断ったときに何をログへ出すか
//   3. 断ったあと何もしないこと(例外は投げない)
//   4. ビューポートの現在値をいつ更新するか
namespace Kurenai::RHI
{
    void IRHICommandList::SetViewport(const Viewport& viewport)
    {
        ApplyViewport(viewport);

        // 【シザーもビューポート全体へ戻す】D3D11はラスタライザが ScissorEnable=TRUE
        // (DX11Device::CreatePipelineState)、D3D12はシザーが常時有効で、どちらも
        // 「矩形0本」が既定 —— つまり一度も張らないと全ピクセルがクリップされて何も映らない。
        // ここで必ず張ることで、SetScissorRectを使わない呼び出し側から見た挙動が
        // 「シザーなど無い」のと同じになる
        m_CurrentViewport = viewport;
        m_HasViewport = true;
        ApplyScissorRect(MakeFullViewportScissorRect(viewport));
    }

    void IRHICommandList::SetScissorRect(const ScissorRect& rect)
    {
        if (!m_HasViewport)
        {
            Core::Logger::Error(
                GetBackendTag(),
                "SetScissorRect: SetViewportより先に呼ばれました。クランプ先のビューポートが"
                "決まらないため、この呼び出しを無視します");
            return;
        }
        ApplyScissorRect(ClampScissorRectToViewport(rect, m_CurrentViewport));
    }

    void IRHICommandList::ResetScissorRect()
    {
        if (!m_HasViewport)
        {
            Core::Logger::Error(
                GetBackendTag(), "ResetScissorRect: SetViewportより先に呼ばれました。この呼び出しを無視します");
            return;
        }
        ApplyScissorRect(MakeFullViewportScissorRect(m_CurrentViewport));
    }

    void IRHICommandList::DispatchIndirect(IRHIBuffer* argsBuffer, uint32_t offsetInBytes)
    {
        if (!argsBuffer)
        {
            Core::Logger::Error(
                GetBackendTag(), "DispatchIndirect: 引数バッファがnullptrです。ディスパッチをスキップします");
            return;
        }
        if (!IsIndirectArgsBuffer(argsBuffer))
        {
            Core::Logger::Error(
                GetBackendTag(),
                "DispatchIndirect: BufferUsage::IndirectArgs以外のバッファが渡されました。ディスパッチをスキップします");
            return;
        }
        if ((offsetInBytes % 4) != 0)
        {
            Core::Logger::Error(
                GetBackendTag(),
                "DispatchIndirect: offsetInBytes(" + std::to_string(offsetInBytes) +
                    ")が4の倍数ではありません。ディスパッチをスキップします");
            return;
        }

        DispatchIndirectImpl(argsBuffer, offsetInBytes);
    }

    void IRHICommandList::ClearUnorderedAccessBufferUint(IRHIBuffer* buffer, uint32_t value)
    {
        if (!buffer)
        {
            Core::Logger::Error(
                GetBackendTag(), "ClearUnorderedAccessBufferUint: バッファがnullptrです。クリアをスキップします");
            return;
        }
        if (!HasUnorderedAccessView(buffer))
        {
            Core::Logger::Error(
                GetBackendTag(),
                "ClearUnorderedAccessBufferUint: UAVを持たないバッファが渡されました。クリアをスキップします");
            return;
        }

        ClearUnorderedAccessBufferUintImpl(buffer, value);
    }

    void IRHICommandList::CopyBufferToReadback(IRHIBuffer* dst, IRHIBuffer* src, uint32_t sizeInBytes)
    {
        if (dst == nullptr || src == nullptr || sizeInBytes == 0)
        {
            Core::Logger::Error(GetBackendTag(), "CopyBufferToReadback: 引数が不正です。コピーをスキップします");
            return;
        }
        if (!IsReadbackBuffer(dst))
        {
            Core::Logger::Error(
                GetBackendTag(),
                "CopyBufferToReadback: コピー先がBufferUsage::Readbackではありません。コピーをスキップします");
            return;
        }

        CopyBufferToReadbackImpl(dst, src, sizeInBytes);
    }

    void IRHICommandList::CopyTextureToReadback(
        IRHITexture* dst, IRHITexture* src, uint32_t mipLevel, uint32_t arraySlice)
    {
        if (dst == nullptr || src == nullptr)
        {
            Core::Logger::Error(GetBackendTag(), "CopyTextureToReadback: 引数がnullptrです。コピーをスキップします");
            return;
        }
        if (!IsReadbackTexture(dst))
        {
            Core::Logger::Error(
                GetBackendTag(),
                "CopyTextureToReadback: コピー先がCreateReadbackTextureで作ったテクスチャではありません。"
                "コピーをスキップします");
            return;
        }

        // 【サブリソースと寸法の検証はここではない】コピー元のミップ数・配列数は
        // バックエンドのリソース記述子を引かないと分からないため、*Impl の中で
        // ReadbackUtil.h の ValidateTextureReadbackCopy を通す(判定そのものは共有している)
        CopyTextureToReadbackImpl(dst, src, mipLevel, arraySlice);
    }
}
