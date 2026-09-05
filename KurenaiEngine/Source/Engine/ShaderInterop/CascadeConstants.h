#pragma once

#include <cstddef>
#include <cstdint>

#include <DirectXMath.h>

// シャドウパスの各カスケード描画専用の定数バッファ(FrameConstants とは別バッファ)。
//
// 【HLSL側の宣言はここが唯一の出所】同じレイアウトを
// KurenaiEngine/Shaders/3D/ShaderInterop/CascadeConstants.hlsli が宣言する。
// 以前は Shadow.hlsl と ShadowMeshlet.hlsl が別々に宣言していた

namespace Kurenai::ShaderInterop
{
    struct alignas(16) CascadeConstants
    {
        DirectX::XMFLOAT4X4 ViewProj;
    };

    // 【レイアウトを固定する本体】HLSL側は宣言順でオフセットが決まる。
    // 並べ替え・挿入・型変更が起きればここで落ちるので、CascadeConstants.hlsli を
    // 直し忘れたまま黙って別の値を読むことはない。
    // **通すために期待値を書き換えないこと**(FrameConstants.h と同じ規約)
    static_assert(offsetof(CascadeConstants, ViewProj) == 0, "CascadeConstants.hlsli の ViewProj と位置が食い違っている");
    static_assert(sizeof(CascadeConstants) == 64, "CascadeConstants の総サイズが変わっている");
}
