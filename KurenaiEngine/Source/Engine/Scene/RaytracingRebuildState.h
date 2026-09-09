#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "Assets/RaytracingScene.h"

// レイトレーシングの加速構造を、モデルの常駐の増減へ追随させるための一式。
//
// 【宣言の位置を動かさないこと】メンバは宣言の逆順で壊れる。ここに並ぶものは
// Loaderスレッドが確保しRenderスレッドが返す関係にあり、他の持ち主との前後関係が
// 変わると、まだ生きているつもりのものを先に壊すことになる。
// エンジン側では**この塊があった元の位置**にメンバを置いてある。
namespace Kurenai::Scene
{
    struct RaytracingRebuildState
    {
        //
        // 常駐が変わるとBLAS/TLASと統合バッファが実態と食い違う。作り直して追随させる。
        // 最後の増減からこの時間だけ静かなら作り直す(走行中は毎フレーム変わりうるため)
        bool RebuildPending = false;
        std::chrono::steady_clock::time_point RebuildAfter{};
        static constexpr float kRebuildQuietSeconds = 0.5f;
        std::mutex RebuiltMutex;
        std::unique_ptr<Assets::RaytracingScene> Rebuilt;
        uint64_t RebuiltGeneration = 0;
        bool RebuildRequested = false;   // m_LoadRequestMutexで保護
        // 再構築が走っている間はtrue。立っている間はRenderスレッド側の差し込みと破棄を見送る。
        // Loaderスレッドが m_Scene を走査している最中に書き換えると走査中のコンテナが変わるため
        std::atomic<bool> RebuildInFlight{ false };
        // 差し替えた旧RaytracingSceneの破棄待ち。モデルと同じくフレームを寝かせる。
        //
        // 【Renderスレッドで破棄してはいけない】RaytracingSceneが持つBLAS/TLASと統合バッファの
        // ディスクリプタは、ロックを持たないアセット用ヒープ(DX12Device::GetAssetSrvCpuHeap)
        // から取られている。Loaderスレッドがストリーミングで確保している最中にRenderスレッドが
        // 解放するとフリーリストが壊れる。寝かせたあとはLoaderスレッドへ渡すこと
        struct PendingRaytracingRelease
        {
            std::unique_ptr<Assets::RaytracingScene> Scene;
            uint32_t FramesRemaining = 0;
        };
        std::vector<PendingRaytracingRelease> PendingRelease;
        std::mutex ReleaseMutex;
        std::vector<std::unique_ptr<Assets::RaytracingScene>> Release;
        // 統計。0なら一度も作り直していない
        uint64_t RebuildCount = 0;
        double RebuildLastMs = 0.0;
    };
}
