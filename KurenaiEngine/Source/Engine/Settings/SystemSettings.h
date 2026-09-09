#pragma once

#include "../EngineDefaults.h"

namespace Kurenai
{
    // 中間バッファの精度構成。HDRが本来採用したい構成で、Legacy8bitは
    // 「中間バッファはすべてR8G8B8A8_UNorm」にする比較用の経路。
    //
    // 精度改善の効果を主観ではなく実測で比較できるようにするために残している。
    // UNorm8は刻みが絶対値1/255=0.392%で固定なのに対し、half floatは仮数10bitで
    // 相対2^-11=0.049%が一定のため、両者の相対精度比は格納値vに対して8/vになる
    // (v=0.1で80倍、v=0.02で401倍)。暗い間接光ほど差が開く
    // (詳細と各バッファの根拠はdocs/Architecture.html)
    enum class BufferPrecision
    {
        HDR,
        Legacy8bit,
    };

    struct SystemSettings
    {
        // 垂直同期。既定で無効。有効にするとPresentがvblankまでブロックするため、GPU負荷が軽い
        // シーンではvsync待ちの間GPUがアイドル→省電力クロックに落ち、次フレームの立ち上がりが
        // 遅くなる・待ち時間自体もジッタで1vblank/2vblank分を行き来するなど計測値が不安定になる。
        // 既定はGPU/CPU双方の実処理時間を素直に見られるOFFとし、ティアリングを許容する
        // (ON時はPresentが即座に返らず、モニタのリフレッシュレートにFPSが制限される)
        bool VSyncEnabled = Defaults::VSyncEnabled;

        // 固定FPSモード。有効時、Renderスレッドが目標FPSより速く回った分だけ待機してフレーム間隔を
        // 一定に保つ。VSyncはモニタのリフレッシュレート依存かつティアリング防止が目的だが、こちらは
        // 任意のFPS値に固定できる(物理更新の再現性確保や環境間でのフレーム時間比較などが目的)。
        // 既定で60fps固定を有効にする
        bool FixedFPSEnabled = Defaults::FixedFPSEnabled;
        float TargetFPS = Defaults::TargetFPS;

        // WASD/E/Qの移動速度[m/s]。Shiftを押している間はDefaults::CameraSpeedShiftMultiplier倍。
        //
        // 【スレッド】書き手はRenderスレッド(ScenePanelのスライダとResetSceneDependentParams)、
        // 読み手はUpdateスレッド(UpdateMovement)。TargetFPSと同じく、単一のfloatを跨いで
        // 読み書きするだけなので同期は置かない ―― 途中の値が1フレーム見えても
        // 「その1フレームだけ移動量が古い速度で計算される」以上のことは起きない。
        // m_Camera本体はUpdateスレッド専有のまま(この値はそこへ入力されるだけ)。
        //
        // 値はシーン対角から決まるためResetSceneDependentParams()が上書きする。
        // ここの初期化子は最初のシーンを読むまでの値でしかない
        float CameraSpeed = Defaults::CameraSpeed;

        // 中間バッファの精度構成(m_Settings.System.Precision)によって変わるフォーマット。
        BufferPrecision Precision = BufferPrecision::HDR;

        // 性能ログ(LogFrameStatsIfDue)。プロファイラパネルの表示はその場で消えてしまい後から
        // 比較できないため、FPS・CPU/GPUフレーム時間を一定間隔でログファイルへ残す。
        // すべてRenderスレッドのみが読み書きするため追加の排他制御は不要
        bool FrameStatsLoggingEnabled = Defaults::FrameStatsLoggingEnabled;

        // 自動監視の有効/無効。**既定はオフ**。A/B比較の最中に勝手に再読み込みが走ると
        // 「同一条件で2回撮る」対照が壊れるため、明示的に入れてもらう
        bool SceneAutoReloadEnabled = false;
        // リロード時に現在のカメラを保持するか。オフ(既定)ならファイルの[Camera]を適用する。
        // [Camera]を詰めるときと、飛び回りながら空・水面・露出を詰めるときで要求が逆になる
        bool SceneReloadKeepsCamera = false;
    };
}
