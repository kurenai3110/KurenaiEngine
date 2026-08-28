#pragma once

#include <cstdint>

#include "KurenaiTypes.h"

#include "RHIBindless.h"

namespace Kurenai::RHI
{
    class IRHITexture;

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

        // このテクスチャが実際に持っているミップ段数。
        //
        // テクスチャストリーミングの常駐ミップ制御が「今どこまで常駐しているか」を知るために要る。
        // GetWidth/GetHeightと同じく、生成経路が多いためリソース記述子から引いて記録漏れを防ぐ
        virtual uint32_t GetMipLevels() const = 0;
    };
}
