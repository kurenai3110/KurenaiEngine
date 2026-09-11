#pragma once

#include <cstdint>

namespace Kurenai::RHI
{
    class IRHITexture;
}

// グラフの登録が進むにつれて確定していく出力を置く場所(段階6)。
//
// 【FrameContext との違い】FrameContext は群にとっての入力で、群は書き換えない。
// こちらは「前の群が書いた先を、後ろの群が読む」ための受け渡しで、
// **登録の途中で書き換わる**。したがって渡し方も const& ではなく、
// 書く側は非 const 参照、読む側は const& で受ける。
//
// 【値捕捉の義務】ここに置いたポインタを Execute ラムダへ渡すときは、
// **必ず値で捕捉する**こと。Blackboard を参照捕捉すると、あとで別の群が
// 書き換えた値をラムダが読むことになり、登録した時点の依存宣言(Reads/Writes)と
// 実際にバインドするテクスチャが食い違う。
// 実例: hdrSceneColor は TAA パスを登録した**後**で確定させる決まりで、
// 先に差し替えると TAA が自分の出力を読む形になりグラフが循環する。
namespace Kurenai::Rendering
{
    struct RenderBlackboard
    {
        // Tonemap が読む HDR のシーンカラー。TAA 有効時はその蓄積結果を指す。
        // **TAA パスを登録した後に確定させること**(上の値捕捉の義務を参照)
        RHI::IRHITexture* HdrSceneColor = nullptr;

        // ソフトウェアラスタライザのパスを今フレーム走らせたか。
        // Present のデバッグ表示が「走っていないなら中身は残骸」の判断に使う
        bool SoftwareRasterPassRuns = false;

        // 超解像(EASU/RCAS)を今フレーム走らせたか。
        // デバッグ表示中は内部解像度のまま等倍で見たいので走らせない
        bool UpscaleActive = false;

        // MegaLightsのデノイズを今フレーム走らせたか。
        // 後段の直接光パスが「生出力とデノイズ後のどちらを t7 へ張るか」をこれで決める
        bool MegaLightsDenoiseRuns = false;

        // モデル単位GPUカリングの候補が今フレーム揃っていたか。
        // graph.Execute() の後で走るカウンタの読み戻しが読む
        bool ModelCullReady = false;

        // MegaLightsの蓄積バッファへこれまでに足したフレーム数(今フレームぶんを含む)。
        // Present のデバッグ表示(Mode 22)がこれで割る。
        // 【FrameContext ではなくここに置く理由】MegaLights の登録中に増えるため、
        // 登録が始まる前に確定している値ではない。フレーム先頭の写しを配ると
        // 1フレーム古い数で割ることになる
        uint32_t MegaLightsAccumFrames = 0;
    };
}
