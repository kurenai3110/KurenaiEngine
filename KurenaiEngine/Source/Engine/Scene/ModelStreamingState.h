#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "Assets/Model.h"
#include "Assets/ModelLoader.h"

// モデルのストリーミング(常駐の増減)に使う受け渡しの一式。
//
// 【宣言の位置を動かさないこと】メンバは宣言の逆順で壊れる。ここに並ぶものは
// Loaderスレッドが確保しRenderスレッドが返す関係にあり、シーンやテクスチャの
// 持ち主との前後関係が変わると、まだ生きているつもりのものを先に壊すことになる。
// エンジン側では**この塊があった元の位置**にメンバを置いてある。
namespace Kurenai::Scene
{
    struct ModelStreamingState
    {
        // シーン切り替えと同じ条件変数で起こす(専用スレッドを増やさない)
        struct StreamingRequest
        {
            std::wstring Path;
            uint64_t Generation = 0;
        };
        std::vector<StreamingRequest> Requests;

        // Loader → Render の完成品
        std::mutex LoadedMutex;
        struct StreamingLoaded
        {
            std::wstring Path;
            std::shared_ptr<Assets::Model> Model;
            uint64_t Generation = 0;
        };
        std::vector<StreamingLoaded> Loaded;

        // 発注済みで、まだ受け取っていないパス(同じものを何度も発注しないため)
        std::unordered_set<std::wstring> InFlight;

        // 【シーンに紐づく世代番号】シーンを切り替えると進める。古い世代の完成品は捨てる。
        // これが無いと、切り替え前のシーンのモデルが新しいシーンのインスタンスへ差し込まれる
        uint64_t Generation = 0;

        // ストリーミングで読むモデルが使う1x1フォールバックの共有プール。
        //
        // 【Assets::Scene::SharedTexturesを使ってはいけない】あちらはシーンが所有しており、
        // シーン切り替えのときRenderスレッドがstd::moveでRetiredAssetsへ移す。
        // Loaderスレッドが読み込み中にそれが起きるとプールのアドレスが変わり、解放済みを指す。
        // こちらはLoaderスレッドだけが作り・使い・捨てるので、その競合が起きない
        std::unique_ptr<Assets::SharedTexturePool> TexturePool;

        // 破棄を寝かせるフレーム数。
        //
        // 【なぜ即座に捨ててはいけないか】CPUはGPUの完了を待たずに次フレームの記録を始めるため
        // (DX12は kFrameCount = 2 フレーム先行する)、いま画面から外れたモデルの頂点バッファを
        // その場で解放すると、GPUがまだ読んでいる最中のリソースを消すことになる。
        // シーン切り替えの経路は WaitForGPUIdle でこれを避けているが(RetiredAssetsのコメント)、
        // ストリーミングの破棄は毎フレーム起こりうるので待つわけにいかない。
        // 代わりにこの数だけ寝かせてから解放する。DX12の先行分2に1フレームの余裕を足してある
        static constexpr uint32_t kReleaseDelayFrames = 3;

        // 破棄待ち。ここに積まれている間はshared_ptrが実体を生かし続ける。
        // 0になったらLoaderスレッドへ渡す(解放も確保と同じスレッドで行うため)
        struct PendingModelRelease
        {
            std::shared_ptr<Assets::Model> Model;
            uint32_t FramesRemaining = 0;
        };
        std::vector<PendingModelRelease> PendingRelease;

        // Render → Loader の破棄依頼。受け取った側はvectorを空にするだけでよい
        // (shared_ptrの最後の参照が消えてデストラクタが走る)
        std::mutex ReleaseMutex;
        std::vector<std::shared_ptr<Assets::Model>> Release;

        // 統計。【いずれも累計】瞬間値だと短い出来事を取りこぼす(47.9の失敗と同じ)
        uint64_t LoadedTotal = 0;
        uint64_t EvictedTotal = 0;
        uint32_t ResidentCount = 0;
        uint32_t TargetCount = 0;
    };

    // 距離を見て集めた読み込み候補。近い順に並べ替えてから発注する。
    //
    // 【Pathは借り物】m_Scene.Instances が持つ文字列を指しているだけなので、
    // 1フレームの UpdateModelStreaming の中でしか使えない。持ち越さないこと
    struct StreamingCandidate
    {
        float DistanceSq = 0.0f;
        const std::wstring* Path = nullptr;
    };
}
