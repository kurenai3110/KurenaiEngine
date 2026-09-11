#pragma once

#include <dxgiformat.h>

#include "RHIEnums.h"

// RHI::Format から DXGI_FORMAT への変換を、DX11とDX12で**1つの表から引く**ためのヘッダ。
//
// 【なぜ共有するのか】この switch は両バックエンドの無名名前空間へ1文字も違わずに
// 書き写されていた。Format を1つ足したときに片方だけ直すと、そのフォーマットは
// 一方のバックエンドでだけ既定値(R32G32B32A32_FLOAT)へ落ち、エラーも出さずに
// 別のフォーマットで描かれる。RHIReadbackFormat.h と同じ考え方で1箇所へ寄せる。
//
// 【RHIの抽象を壊していないか】このヘッダは IRHI*.h のどれからもインクルードされず、
// DX11*/DX12* の実装ファイルからだけ使う。公開インターフェースに DXGI_FORMAT は漏れない。
// 依存も dxgiformat.h(列挙だけの小さなヘッダ)に閉じており、d3d11.h/d3d12.h を引き込まない
namespace Kurenai::RHI
{
    // 【default が R32G32B32A32_Float を返す】表に無い Format を弾かずに最も広い
    // フォーマットへ落とすのは元の実装からの挙動で、変えていない
    inline DXGI_FORMAT ToDXGIFormat(Format format)
    {
        switch (format)
        {
        case Format::R32G32_Float:
            return DXGI_FORMAT_R32G32_FLOAT;
        case Format::R32G32B32_Float:
            return DXGI_FORMAT_R32G32B32_FLOAT;
        case Format::R8G8B8A8_UNorm:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case Format::R32_Float:
            return DXGI_FORMAT_R32_FLOAT;
        case Format::R16G16_Float:
            return DXGI_FORMAT_R16G16_FLOAT;
        case Format::R16G16B16A16_Float:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case Format::R11G11B10_Float:
            return DXGI_FORMAT_R11G11B10_FLOAT;
        case Format::R32G32B32A32_Float:
        default:
            return DXGI_FORMAT_R32G32B32A32_FLOAT;
        }
    }
}
