#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "KurenaiTypes.h"

// DirectXTexの型はここでは前方宣言のみに留め、GPUデバイスを持たないファイル
// (ModelLoader.cpp等)がこのヘッダーだけでテクスチャのデコードを呼び出せるようにする。
// 実体(DirectX::TexMetadata/ScratchImage)へのアクセスはDX11Device/DX12Device.cppなど
// 既にDirectXTex.hをインクルードしている側からGetMetadata()/GetImage()経由でのみ行う
namespace DirectX
{
    struct TexMetadata;
    class ScratchImage;
}

#pragma warning(push)
#pragma warning(disable: 4251)

namespace Kurenai::RHI
{
    // テクスチャファイルのデコード結果(DirectXTexのTexMetadata/ScratchImage)を保持するクラス。
    // GPUデバイスを一切必要としないため、ワーカースレッドから並列に呼び出せる
    // (GPUリソース作成はIRHIDevice::CreateTextureFromImageへ別途渡す側で行う)。
    // PNG/JPG等の非圧縮形式は、初回のみBC7圧縮+ミップ生成を行いディスクキャッシュ
    // (<元ファイル>.srgb.ktexcache / .linear.ktexcache)へ保存する。2回目以降はこのキャッシュを
    // 読むだけになり、WICデコード・ミップ生成・BC7圧縮のいずれも発生しない
    class KURENAI_API TextureImage
    {
    public:
        TextureImage(TextureImage&& other) noexcept;
        TextureImage& operator=(TextureImage&& other) noexcept;
        TextureImage(const TextureImage&) = delete;
        TextureImage& operator=(const TextureImage&) = delete;
        ~TextureImage();

        // 失敗した場合はstd::runtime_errorを投げる(呼び出し側でフォールバック処理を行うこと)
        static TextureImage LoadFromFile(const std::wstring& filePath, bool sRGB);

        // デコード済みデータの総バイト数。並列プリフェッチ時のメモリ使用量制御に使う
        uint64_t GetSizeInBytes() const;

        // DX11Device/DX12DeviceのCreateTextureFromImage実装専用のアクセサ
        const DirectX::TexMetadata& GetMetadata() const;
        const DirectX::ScratchImage& GetImage() const;

    private:
        TextureImage();

        struct Impl;
        std::unique_ptr<Impl> m_Impl;
    };
}

#pragma warning(pop)
