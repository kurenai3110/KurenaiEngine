#pragma once

#include <cstdint>

#include <dxgiformat.h>

#include "IRHITexture.h"

// リードバック時のフォーマット解釈を、DX11とDX12で**1つの表から引く**ためのヘッダ。
//
// 【なぜ共有するのか】ここを両バックエンドに別々に書くと、片方だけ直したときに
// 「DX11では読めるがDX12では別の型として読む」という、絵にもエラーにも出ない食い違いが生まれる。
// リポジトリの規約(RHIのインターフェースを変えたらDX11/DX12の両方を直す)を、
// 直し忘れようがない形にしたもの。
//
// 【RHIの抽象を壊していないか】このヘッダはIRHI*.hのどれからもインクルードされず、
// DX11*/DX12*の実装ファイルからだけ使う。公開インターフェースにDXGI_FORMATは漏れない。
// 依存もdxgiformat.h(列挙だけの小さなヘッダ)に閉じており、d3d11.h/d3d12.hを引き込まない
namespace Kurenai::RHI
{
    // リードバック用の受け皿を作るときに使う、型付きのフォーマット。
    //
    // 深度テクスチャはDSV(D32_FLOAT)とSRV(R32_FLOAT)の両方を張るためR32_TYPELESSで
    // 作られているが、typelessのままではコピー先の記述子に使えない。
    // D3D11/D3D12のどちらもテクスチャ間コピーに「同一またはtypeグループが同じフォーマット」を
    // 要求するため、同じtypeグループの型付きフォーマットへ置き換えるのは合法。
    // 置き換えが要るのは実際には深度の1件だけだが、増えたときに1箇所で足せるよう表にしてある
    inline DXGI_FORMAT ToReadbackTypedFormat(DXGI_FORMAT format)
    {
        switch (format)
        {
        case DXGI_FORMAT_R32_TYPELESS:
            return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_D32_FLOAT:
            // DSVのフォーマットがそのまま渡ってきた場合も、読み出しはR32_FLOATとして扱う
            return DXGI_FORMAT_R32_FLOAT;
        default:
            return format;
        }
    }

    // フォーマットから「何チャンネルの、どの数値型で、1テクセル何バイトか」を引く。
    // 表に無いフォーマット(BC圧縮のアセットテクスチャなど)はElementType::Unknownを返し、
    // 呼び出し側がリードバックを断る。**黙って0で埋めた結果を返さないこと**が肝心で、
    // 「読めなかった」と「全部ゼロだった」を取り違えると原因の特定を丸ごと外す
    inline TextureReadbackDesc DescribeReadbackFormat(DXGI_FORMAT format, uint32_t width, uint32_t height)
    {
        TextureReadbackDesc desc;
        desc.Width = width;
        desc.Height = height;

        switch (ToReadbackTypedFormat(format))
        {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            desc.ChannelCount = 4;
            desc.BytesPerTexel = 4;
            desc.ElementType = TextureElementType::UNorm8;
            break;
        case DXGI_FORMAT_R16G16_FLOAT:
            desc.ChannelCount = 2;
            desc.BytesPerTexel = 4;
            desc.ElementType = TextureElementType::Float16;
            break;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            desc.ChannelCount = 4;
            desc.BytesPerTexel = 8;
            desc.ElementType = TextureElementType::Float16;
            break;
        case DXGI_FORMAT_R32_FLOAT:
            desc.ChannelCount = 1;
            desc.BytesPerTexel = 4;
            desc.ElementType = TextureElementType::Float32;
            break;
        case DXGI_FORMAT_R32G32_FLOAT:
            desc.ChannelCount = 2;
            desc.BytesPerTexel = 8;
            desc.ElementType = TextureElementType::Float32;
            break;
        case DXGI_FORMAT_R32G32B32_FLOAT:
            desc.ChannelCount = 3;
            desc.BytesPerTexel = 12;
            desc.ElementType = TextureElementType::Float32;
            break;
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
            desc.ChannelCount = 4;
            desc.BytesPerTexel = 16;
            desc.ElementType = TextureElementType::Float32;
            break;
        case DXGI_FORMAT_R11G11B10_FLOAT:
            // 1テクセル4バイトに3成分。呼び出し側がfloat3へ展開してから使う
            // (RHIEnums.hのTextureElementType::Packed11_11_10_Floatのコメント参照)
            desc.ChannelCount = 3;
            desc.BytesPerTexel = 4;
            desc.ElementType = TextureElementType::Packed11_11_10_Float;
            break;
        default:
            // ElementTypeはUnknownのまま。呼び出し側がログを出して断る
            break;
        }

        return desc;
    }
}
