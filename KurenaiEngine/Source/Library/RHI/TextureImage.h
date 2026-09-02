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

    // .ktexのDDSペイロードを「デコードせずにヘッダだけ読んで」得た情報。
    //
    // テクスチャストリーミングは「どのミップから常駐させるか」を決めてから読み込むため、
    // 中身を展開する前に寸法とミップ数を知る必要がある。DDSヘッダはDXT10拡張を含めても
    // 148バイトしかないので、先頭256バイトだけ読めば足りる
    // (KurenaiPackerのExistingKtexIsUnsupportedが既に同じ手を使っている)
    struct PackedTextureInfo
    {
        uint32_t Width = 0;
        uint32_t Height = 0;
        uint32_t MipLevels = 0;
        // DXGI_FORMATの値。このヘッダーはd3d12.h/dxgiformat.hに依存させたくないためuint32_tで持つ
        uint32_t Format = 0;
        // .ktexの24Bヘッダが持つDDSペイロードのバイト数(DDSヘッダを含む)
        uint64_t PayloadSize = 0;
        // .ktexのFlags bit0。実体はDXGIフォーマットが持つので検証用
        bool SRGB = false;
        // ミップ単位の部分読み出しが使えるか。2Dの単一テクスチャでミップが2段以上あるものだけtrue
        // (キューブマップ・テクスチャ配列・ボリュームはミップの並びが単純でないため対象外)
        bool SupportsPartialMipLoad = false;
    };

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

        // 上と同じだが、ミップ0からfirstMip-1段を読み飛ばして「firstMipを新しいミップ0とする
        // 小さなテクスチャ」として読む。テクスチャストリーミングの常駐ミップ制御で使う。
        //
        // 【なぜ小さなテクスチャとして返すのか】RHIにミップ範囲を指定してアップロードするAPIが無く、
        // SRVもMipLevels=全段で作りきりになっている。ここで寸法ごと縮めて返せば、
        // IRHIDevice::CreateTextureFromImage/ReplaceTextureContentsは何も知らずに済み、
        // シェーダー側の変更も要らない。DDSはミップ0を先頭に降順で連続しているため、
        // 目的のミップのバイト範囲へseekするだけで読み飛ばせる。
        //
        // firstMip==0、あるいは部分読み出しの前提を満たさないファイル(キューブマップ・
        // テクスチャ配列・ミップ1段)の場合は、警告を出して全ミップを読む上の版へ委譲する
        // ―― 常駐量が減らないだけで絵は正しく出る、という安全側へ倒す
        static TextureImage LoadFromPackedTexture(const std::wstring& filePath, uint32_t firstMip);

        // .ktexを展開せずにヘッダだけ読む。失敗時は例外を投げずfalseを返しログを出す
        // (常駐管理は数万枚を相手にするため、1枚読めないことで走査全体を止めない)
        static bool TryReadPackedTextureInfo(const std::wstring& filePath, PackedTextureInfo& outInfo);

        // firstMip以降を常駐させた場合のGPU上のバイト数(ミップチェーンの合計)。
        // 常駐VRAMの自己申告値を積算するために使う
        static uint64_t ComputeMipChainBytes(const PackedTextureInfo& info, uint32_t firstMip);

        // デコード済みデータの総バイト数。並列プリフェッチ時のメモリ使用量制御に使う
        uint64_t GetSizeInBytes() const;

        // **線形空間**のサムネイル(size×size、画素あたりRGBの3float)を取り出す。
        // outRGB は size*size*3 個ぶんの領域を呼び出し側が用意すること。
        // 取り出せなかった場合は false を返す(呼び出し側は「白」として扱い、警告を出すこと)。
        //
        // 【何のためにあるのか】自発光メッシュから光源プロキシを起こすとき、面が実際に
        // 出している放射輝度は「係数 × テクスチャの平均色」で決まる。係数だけを見ると、
        // テクスチャの黒い部分まで光る面として数えて過大評価する。
        //
        // 【sRGB→線形は必ず平均の前に行う】逆順(平均してから EOTF)にすると、暗い背景に
        // 明るいグリフが乗った看板で真値0.5に対して0.214が出る(2倍以上暗い)。
        // この関数は float へ変換した時点で線形になっているものを畳む。
        // **自前で EOTF を掛けてはいけない** ―― DirectXTex は BC*_UNORM_SRGB から
        // 非sRGBへ変換するとき、指定しなくても線形化する(2回掛かる)。
        //
        // 【ミップ0から求める】.ktex のミップは元の .dds が持っていたものをそのまま
        // 運んでいることがあり、ガンマ空間で畳まれていると線形平均が保存されない
        // (実測で 32^2 のミップは 2048^2 のミップ0より 2〜3倍暗く出た)
        bool ExtractLinearThumbnail(uint32_t size, float* outRGB) const;

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
