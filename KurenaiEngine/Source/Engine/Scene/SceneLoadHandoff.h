#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

// シーン読み込みのハンドオフ(Renderスレッド ⇄ Loaderスレッド)。
//
// 【宣言の位置を動かさないこと】メンバは宣言の逆順で壊れる。Threadはjoin済みで
// ここへ来るが(Run()がjoinする)、要求と進捗はシーンやテクスチャの持ち主との
// 前後関係の中に置かれている。エンジン側では**この塊があった元の位置**にメンバを置いてある。
namespace Kurenai::Scene
{
    struct SceneLoadHandoff
    {
        std::thread Thread;

        // Renderスレッド専有。ScenePanelが押されたときに積まれ、UpdateSceneStreamingが消費する。
        // -1は「要求なし」。UIもRenderスレッドで動くため、これはatomicである必要がない
        int PendingSceneRequest = -1;
        // Renderスレッド専有。Loaderスレッドへ発注してから完成品を受け取るまでtrue。
        // 多重発注を防ぐために見る
        bool InFlight = false;
        // Renderスレッド専有。いまLoaderスレッドが読んでいるシーンの番号(m_SceneDisplayNamesの添字)。
        // 進捗表示にシーン名を出すために持つ ―― m_CurrentSceneIndexは読み込みが完了するまで
        // 旧シーンを指したままで、PendingSceneRequestは発注した時点で-1へ戻る
        size_t LoadingIndex = 0;

        // シーン読み込みの進捗(読み終えたモデル数 / [Model]の総数)。
        //
        // 【なぜ要るか】読み込み中は旧シーンを先に手放すため画面にはUIとスカイボックスしか出ない
        // (UpdateSceneStreamingのコメント参照)。767モデルのシーンでは数十秒かかり、
        // InFlightのboolだけでは「進んでいる」と「固まった」を区別できない。
        //
        // 【atomicにする理由】書き手はLoaderスレッド(Assets::LoadSceneのコールバック)、
        // 読み手はRenderスレッド(UIManagerの進捗ウィンドウ)で、フレーム境界の受け渡しに
        // 乗らない唯一の値のため。表示だけに使うのでmemory_order_relaxedで足りる
        std::atomic<uint32_t> ProgressLoaded{ 0 };
        std::atomic<uint32_t> ProgressTotal{ 0 };

        // Render → Loader の要求。-1は「要求なし」
        std::mutex RequestMutex;
        std::condition_variable RequestCV;
        int RequestSceneIndex = -1;
        bool StopThread = false;
    };
}
