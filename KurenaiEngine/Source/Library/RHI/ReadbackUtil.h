#pragma once

#include <cstdint>

#include <dxgiformat.h>

#include "IRHITexture.h"

// リードバック(GPUが書いた値をCPUで読む経路)の**引数検証と行のパディング除去**を、
// DX11とDX12で1箇所に寄せるための層。
//
// 【何をここへ置き、何を置かないのか】置くのは「断る条件」と「詰め直し方」であって、
// Map / CopySubresourceRegion / CopyTextureRegion といったAPIの発行ではない。
// 断る条件が片方のバックエンドだけ緩いと、**そちらでだけ、はみ出して書くか途中で切れる**。
// どちらも例外にならず、絵にも出ず、読み出した数値だけが静かに間違う。
//
// フォーマットの解釈は RHIReadbackFormat.h、寸法とテクセル型の受け渡しは
// TextureReadbackDesc(IRHITexture.h)。このヘッダはその上で「合っているか」を見る。
//
// 依存は IRHITexture.h と dxgiformat.h(列挙だけの小さなヘッダ)で、
// d3d11.h/d3d12.h は引き込まない。公開インターフェースにも漏れない
namespace Kurenai::RHI
{
    // ミップ段 mipLevel の寸法。1未満にはならない(D3Dのミップ規則と同じ)
    inline uint32_t ReadbackMipExtent(uint32_t base, uint32_t mipLevel)
    {
        const uint32_t shifted = base >> mipLevel;
        return shifted < 1u ? 1u : shifted;
    }

    // IRHITexture::ReadbackData の入口の検証。通れば true を返し、
    // outTightRowPitch にパディングを剥がしたあとの1行のバイト数を書く。
    //
    // isReadbackTexture が false(リードバック用に作られていないテクスチャ)、出力先が
    // nullptr かサイズ0、受け皿が必要量に足りない、のいずれかで false を返してログを出す。
    // **「読めなかった」を黙って0埋めの成功にしないこと**が肝心で、
    // 取り違えると原因の特定を丸ごと外す(IRHITexture::ReadbackData のコメント参照)
    bool ValidateTextureReadbackRequest(
        const char* backendTag, bool isReadbackTexture, const void* outData, uint32_t sizeInBytes,
        const TextureReadbackDesc& desc, uint32_t& outTightRowPitch);

    // IRHIBuffer::ReadbackData の入口の検証。テクスチャ版と違い、要求サイズが
    // バッファの実サイズを**超えていないか**を見る(テクスチャは受け皿が足りているか)
    bool ValidateBufferReadbackRequest(
        const char* backendTag, bool isReadbackBuffer, const void* outData, uint32_t sizeInBytes,
        uint32_t bufferSizeInBytes);

    // 行ごとのパディングを剥がしながら写す。
    // srcRowPitch はドライバ/D3Dが決めた値(DX11はMapのRowPitch、DX12は
    // GetCopyableFootprints の256バイト整列)で、tightRowPitch 以上であること
    void CopyReadbackRowsTightly(
        void* outData, const void* mappedData, uint32_t tightRowPitch, uint32_t srcRowPitch, uint32_t height);

    // IRHICommandList::CopyTextureToReadback の検証のうち、バックエンドに依らない部分。
    //
    // サブリソースの指定が範囲外でないか、受け皿の寸法がコピー元の mipLevel 段と
    // 一致しているかを見る。**受け皿は CreateReadbackTexture の時点で特定のミップ段の
    // 寸法に合わせて作ってある**ので、別の段を指定されるとサイズが合わず静かに壊れる
    bool ValidateTextureReadbackCopy(
        const char* backendTag, uint32_t mipLevel, uint32_t arraySlice, uint32_t srcWidth, uint32_t srcHeight,
        uint32_t srcMipLevels, uint32_t srcArraySize, uint32_t dstWidth, uint32_t dstHeight);

    // IRHIDevice::CreateReadbackTexture の検証のうち、バックエンドに依らない部分。
    // 通れば true を返し、outDesc へ受け皿の記述子(mipLevel 段の寸法とテクセルの数値型)を書く。
    //
    // 「コピー元が Texture2D か」の判定だけは各バックエンドに残る ―― DX11はビューから
    // ID3D11Texture2D を引けるかで、DX12は記述子の Dimension で見るため、確かめ方が別物。
    //
    // フォーマットが対応表(RHIReadbackFormat.h)に無ければ ElementType::Unknown になるので
    // false を返す。**黙って0で埋めた結果を返さないこと**が肝心
    bool PrepareReadbackTextureDesc(
        const char* backendTag, uint32_t mipLevel, uint32_t srcMipLevels, uint32_t srcWidth, uint32_t srcHeight,
        DXGI_FORMAT srcFormat, TextureReadbackDesc& outDesc);
}
