#pragma once

#include <cstdint>

namespace Kurenai
{
    // フレームごとの統計値(ImGuiのプロファイラパネル・性能ログ表示用)。
    //
    // 【「今フレーム積み上げ中の値」と「完成した前フレームの値」を混同しないこと】
    // 一部の統計はフレーム先頭で0にリセットしてから積み上げる生カウンタと、
    // Renderの最後にそれを写し取った...LastFrame版の2種類がある。
    // UIパネルはRenderの呼び出しの外(ImGui描画)で読まれるため、生カウンタをそのまま読むと
    // リセット直後の0を掴む。ここには**UIが読むための完成値だけ**を置く
    // (積み上げ中の生カウンタ・集計期間の合計はKurenaiEngine3D側に残したまま)
    struct RenderStats
    {
        // 統計表示用: 1フレームあたりのCPU時間(Renderの呼び出し時間)と、指数移動平均によるFPS。
        // どちらもRenderスレッドのみが書き込み、ImGui描画(同じくRenderスレッド)のみが読むため
        // 追加の排他制御は不要
        float CPUFrameTimeMs = 0.0f;
        float FPS = 0.0f;

        // 直前に描き終えたフレームの値。**UIパネルはこちらを読むこと** ――
        // 上のカウンタはフレーム先頭で0に戻るため、Renderの外で描かれるUIからは常に0に見える
        uint32_t DrawCallsGBufferLastFrame = 0;
        uint32_t DrawCallsShadowLastFrame = 0;
        uint32_t DrawCallsDepthPrepassLastFrame = 0;

        // 完成した最後のフレームの値。UIパネルはRenderの外で描かれるため、上のカウンタを
        // そのまま読むとリセット直後の0になる(ドローコール数のDrawCalls*LastFrameと同じ)
        uint32_t FrustumCullTestedLastFrame = 0;
        uint32_t FrustumCullCulledLastFrame = 0;
        uint32_t MeshCullTestedLastFrame = 0;
        uint32_t MeshCullCulledLastFrame = 0;

        // 直近に読み戻せた値(Perfログの集計に足し込む前の生値)。デバッグ表示にも使う
        uint32_t MeshletCullTested = 0;
        uint32_t MeshletCullFrustumCulled = 0;
        uint32_t MeshletCullOcclusionCulled = 0;

        // 統計。1フレームあたりの段の切り替え回数と、そのフレームでフェード中のインスタンス数。
        // 【0なら一度も切り替わっていない】LODが効いているかはここでしか分からない
        // (切り替え回数そのものはKurenaiEngine3D::m_LODSwitchCountが持つ。ここにはフェード中の
        // インスタンス数だけを置く)
        uint32_t LODFadingCount = 0;

        // 直近のフレームで実際にGPUへ送ったプロキシの数(ImGuiとログの表示用)
        uint32_t EmissiveLightsUsedCount = 0;

        // bindless区画の容量と使用数(IRHIDevice::GetBindlessCapacity/GetBindlessUsedCountの写し)。
        // 容量は初期化時に、使用数はフレーム先頭に控える。**満杯でも例外は飛ばず
        // 白1x1で描かれてしまう**ため、UIとフレーム統計ログの両方へ出す
        uint32_t BindlessCapacity = 0;
        uint32_t BindlessUsedCount = 0;
    };
}
