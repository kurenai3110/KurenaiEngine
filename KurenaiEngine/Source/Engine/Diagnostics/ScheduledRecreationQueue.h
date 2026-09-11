#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ScheduledRecreation.h"

// 作り直し経路の予約(検証専用)。
//
// 【なぜ純粋仮想で受けるのか】この予約が起こすのは、解像度の変更・バッファ精度の
// 切り替え・シーンの読み込みという**エンジン全体に及ぶ作り直し**で、予約を持つ側から
// エンジンの実体を呼び返すしかない。参照をそのまま持たせるとエンジンの公開面が
// 予約のために広がるので、**この4本だけ**を口として切り出してある。
namespace Kurenai::Diagnostics
{
    class IRecreationTarget
    {
    public:
        virtual ~IRecreationTarget() = default;

        // 超解像の設定を入れ直す(内部レンダー解像度もこれで決まる)
        virtual void RequestUpscaleSettings(bool enabled, uint32_t outputWidth, uint32_t outputHeight) = 0;
        // 中間バッファの精度を切り替える。精度依存のPSOも作り直しになる
        virtual void RequestBufferPrecision(BufferPrecision precision) = 0;
        // 読み込めるシーンのファイルパス。名前からの引き当てに使う
        virtual const std::vector<std::wstring>& GetSceneFilePaths() const = 0;
        virtual void RequestSceneLoad(size_t sceneIndex) = 0;
    };

    class ScheduledRecreationQueue
    {
    public:
        // 起動オプションからの登録
        void Add(const ScheduledRecreation& request);

        // 【フレームの先頭で呼ぶ】発火済みのものはFiredを立てて二度と撃たない。
        // フレームが飛んでも取りこぼさないよう、「>= Frameの最初のフレーム」で撃つ
        void Apply(IRecreationTarget& target, uint32_t frameIndex);

    private:
        struct Slot
        {
            ScheduledRecreation Request;
            bool Fired = false;
        };
        std::vector<Slot> m_Slots;
    };
}
