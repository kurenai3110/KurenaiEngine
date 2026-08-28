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
    // LoadFromFileの内部フェーズ別の累計時間(秒)。**呼び出したスレッド全部の合計**なので、
    // 和は実時間を超えうる。
    //
    // 【なぜロック待ちと圧縮を分けて持つのか】GPU BC7圧縮はプロセス内で1つの
    // D3D11デバイスをミューテックスで直列化して呼ぶ(CompressBC7参照)。ワーカーを
    // 8本に増やしても、全員がそのミューテックスで待っているだけなら本数を増やす意味は無く、
    // 逆に「圧縮そのもの」が支配的ならプロセスを分けるかGPU側を見直すことになる。
    // どちらなのかは**待ち時間と圧縮時間を別々に測らない限り区別がつかない**
    // (「LoadFromFileに何秒いたか」だけでは両者が混ざる)。
    // 計測はKurenaiPacker.exeのようなオフラインツールが使う想定で、ランタイムは
    // .ktexしか読まないためこの経路自体を通らない
    struct TextureLoadStats
    {
        double DecodeSeconds = 0.0;        // WIC/DDS/TGAのデコード
        double MipSeconds = 0.0;           // GenerateMipMaps
        double BC7WaitSeconds = 0.0;       // 共有デバイスのミューテックス取得待ち
        double BC7CompressSeconds = 0.0;   // DirectX::Compress本体(ロック取得後)
        double DeviceCreateSeconds = 0.0;  // GPU圧縮デバイスの遅延生成(プロセスで1回)
        uint64_t Count = 0;                // LoadFromFileの呼び出し回数
    };

    // 累計のスナップショットを返す / 0に戻す。スレッドセーフ
    KURENAI_LIB_API TextureLoadStats GetTextureLoadStats();
    KURENAI_LIB_API void ResetTextureLoadStats();

    // テクスチャファイルのデコード結果(DirectXTexのTexMetadata/ScratchImage)を保持するクラス。
    // GPUデバイスを一切必要としないため、ワーカースレッドから並列に呼び出せる
    // (GPUリソース作成はIRHIDevice::CreateTextureFromImageへ別途渡す側で行う)。
    // PNG/JPG等の非圧縮形式のBC7圧縮+ミップ生成はKurenaiPacker.exe(オフラインのアセット
    // ビルドツール)が事前に行い、.ktexへ書き出す。ランタイムはLoadFromPackedTextureで
    // その.ktexを読むだけであり、WICデコード・ミップ生成・BC7圧縮のいずれも発生しない
    // (LoadFromFileはKurenaiPacker自身と、スカイボックス等の.dds直接読み込みでのみ使う)
    class KURENAI_LIB_API TextureImage
    {
    public:
        TextureImage(TextureImage&& other) noexcept;
        TextureImage& operator=(TextureImage&& other) noexcept;
        TextureImage(const TextureImage&) = delete;
        TextureImage& operator=(const TextureImage&) = delete;
        ~TextureImage();

        // 失敗した場合はstd::runtime_errorを投げる(呼び出し側でフォールバック処理を行うこと)。
        // PNG/JPG等はWICデコード+ミップ生成+GPU BC7圧縮を行う(KurenaiPacker.exeが使う)。
        // DDS/TGAは既に圧縮・ミップ済みの配布形式として扱いそのまま読み込む(スカイボックス等)
        static TextureImage LoadFromFile(const std::wstring& filePath, bool sRGB);

        // KurenaiPacker.exeが生成した.ktex(ModelPackage.h参照)を読み込む。
        // ヘッダ検証後、DDSペイロードをDirectX::LoadFromDDSMemoryでデコードするだけなので、
        // WICデコード・ミップ生成・BC7圧縮はいずれも発生しない。失敗時はstd::runtime_errorを投げる
        static TextureImage LoadFromPackedTexture(const std::wstring& filePath);

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
