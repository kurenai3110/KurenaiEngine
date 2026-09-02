#pragma once

#include <cstdint>

#include "KurenaiTypes.h"

#include "RHIBindless.h"
#include "RHIEnums.h"

namespace Kurenai::RHI
{
    class IRHITexture;

    // リードバック用テクスチャ(IRHIDevice::CreateReadbackTextureが作るもの)の中身の形。
    // ミップ段ごとに寸法が変わるため、GetReadbackDescにはミップ番号を渡す。
    //
    // 【DXGI_FORMATを公開しない】RHIの抽象がD3D固有の型を漏らさないという既存の方針
    // (RHIEnums.hのFormatと同じ扱い)。上位層が知りたいのは「何チャンネルの、どの数値型か」
    // だけなので、ここまで噛み砕いた形で渡す
    struct TextureReadbackDesc
    {
        // 指定したミップ段の寸法。**ミップ0の寸法ではない**
        uint32_t Width = 0;
        uint32_t Height = 0;
        uint32_t ChannelCount = 0;  // 1/2/3/4
        uint32_t BytesPerTexel = 0; // 1テクセルの合計バイト数(パディングを除いた値)
        TextureElementType ElementType = TextureElementType::Unknown;
    };

    // IRHIDevice::PrepareTextureContentsが返す、差し替え待ちのGPUリソース。
    // 中身はバックエンドごとに違うため、上位層からは不透明なハンドルとしてだけ扱う。
    // 破棄すれば差し替えは行われず、作りかけのリソースごと捨てられる
    class KURENAI_LIB_API IRHIPendingTextureContents
    {
    public:
        virtual ~IRHIPendingTextureContents() = default;

        // 差し替え先のテクスチャ。呼び出し側が「どれが更新されたか」を照合するために使う
        virtual IRHITexture* GetTarget() const = 0;
        // 差し替え後に常駐するミップ段数(統計の積算用)
        virtual uint32_t GetMipLevels() const = 0;
    };

    class KURENAI_LIB_API IRHITexture
    {
    public:
        virtual ~IRHITexture() = default;

        // このテクスチャのSRVがbindlessヒープの何番に登録されているか。
        // 登録されていない(あるいはバックエンドがbindless非対応の)場合はkInvalidBindlessIndex。
        //
        // 登録はIRHIDevice::RegisterBindlessが行う。全テクスチャを自動登録しないのは、
        // bindless区画の容量が有限(DX12Device::kBindlessDescriptorCapacity)で、
        // レンダーターゲットや中間バッファのように固定スロット経由でしか読まないテクスチャまで
        // 登録すると無駄に食い潰すため。「シェーダーから動的な番号で選びたいもの」だけを登録する
        virtual uint32_t GetBindlessIndex() const { return kInvalidBindlessIndex; }

        // このテクスチャのミップ0のピクセルサイズ。
        //
        // テクスチャアトラスの区画をピクセルで管理する用途(KurenaiEngine2D::GetTextureSize)で要る。
        // CreateTextureFromFileはファイルから読んだ寸法を呼び出し側へ返さないため、
        // RHIの側で持っていないとアプリからは知りようがない。
        // 各バックエンドはリソース記述子から取った値をコンストラクタで控えている
        // (生成経路が多いため、経路ごとに記録するのではなくリソース側から引いて記録漏れを防ぐ)
        virtual uint32_t GetWidth() const = 0;
        virtual uint32_t GetHeight() const = 0;

        // グラフィックスデバッガ(RenderDoc / PIX)へ見せる名前を付ける。
        //
        // 【何のためにあるのか】名前が無いと、キャプチャ上では `ResourceId::562` のような
        // 通し番号としてしか出ない。どれがG-Bufferのアルベドで、どれがSceneColorなのかが
        // 分からず、寸法とフォーマットで候補を絞って総当たりで突き合わせることになる
        // (実際にそうなった。docs/ImplementationDetail.md 62.10)。
        //
        // 名前は`-dumptex`が使うものと同じ表から与える(KurenaiEngine3D::BuildDumpableTextureTable)。
        // **2つの経路で呼び名を揃えることが肝心**で、`-dumptex GBufferAlbedo`で吸い出した値と
        // キャプチャ上の`GBufferAlbedo`が同じものだと、名前だけで分かるようにするため。
        //
        // 実装しないバックエンドがあってよいので純粋仮想にしない(既定は何もしない)
        virtual void SetDebugName(const char* name) { (void)name; }

        // 【この2つはIRHIDevice::CreateReadbackTextureで作ったテクスチャ専用】
        // 通常のテクスチャは既定実装(Unknownを返す / falseを返す)のまま。
        // IRHIBuffer::ReadbackDataと同じく、実装しないバックエンド/用途があるので純粋仮想にしない。

        // 指定ミップ段の寸法と、テクセルの数値としての型。
        // ElementTypeがUnknownなら、このテクスチャはリードバックできない
        // (リードバック用に作られていない、または対応表に無いフォーマット)
        virtual TextureReadbackDesc GetReadbackDesc(uint32_t mipLevel = 0) const
        {
            (void)mipLevel;
            return TextureReadbackDesc{};
        }

        // リードバック用テクスチャの内容をCPU側のメモリへ写す。
        // 読めたらtrue、まだ読めない/リードバック用でない場合はfalse。
        //
        // 【行のパディングはここで剥がす】DX12はGetCopyableFootprintsが返す256バイト整列、
        // DX11はMapが返すRowPitchで、どちらも行の末尾に詰め物が入る。この実装が行ごとに
        // memcpyしてタイトに詰め直すため、**呼び出し側はパディングの存在を一切見ない**。
        // 剥がす側をここに置いたのは、DX11のRowPitchがMapするまで分からず、上位層に持たせると
        // 「確保の時点では知りようのない値」を要求することになるため。
        // 必要なバイト数は GetReadbackDesc(mip) の Width * Height * BytesPerTexel。
        //
        // 【GPUの完了を待たない】約束はIRHIBuffer::ReadbackDataとまったく同じ。
        // 待つとフレームが直列化し、計測のために計測対象を壊すことになる。
        // 呼び出し側がCopyTextureToReadbackを積んでから十分なフレーム数を空けて読むこと
        virtual bool ReadbackData(void* outData, uint32_t sizeInBytes)
        {
            (void)outData;
            (void)sizeInBytes;
            return false;
        }

        // このテクスチャが実際に持っているミップ段数。
        //
        // テクスチャストリーミングの常駐ミップ制御が「今どこまで常駐しているか」を知るために要る。
        // GetWidth/GetHeightと同じく、生成経路が多いためリソース記述子から引いて記録漏れを防ぐ
        virtual uint32_t GetMipLevels() const = 0;
    };
}
