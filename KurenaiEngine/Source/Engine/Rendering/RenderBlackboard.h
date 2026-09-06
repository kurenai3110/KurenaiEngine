#pragma once

namespace Kurenai::RHI
{
    class IRHITexture;
}

// グラフの登録が進むにつれて確定していく出力を置く場所(段階6)。
//
// 【FrameContext との違い】FrameContext は登録が始まる前に確定している値で、
// 登録中は変わらない。こちらは「前の群が書いた先を、後ろの群が読む」ための受け渡しで、
// **登録の途中で書き換わる**。したがって渡し方も const& ではなく、
// 書く側は非 const 参照、読む側は const& で受ける。
//
// 【値捕捉の義務】ここに置いたポインタを Execute ラムダへ渡すときは、
// **必ず値で捕捉する**こと。Blackboard を参照捕捉すると、あとで別の群が
// 書き換えた値をラムダが読むことになり、登録した時点の依存宣言(Reads/Writes)と
// 実際にバインドするテクスチャが食い違う。
// 実例: hdrSceneColor は TAA パスを登録した**後**で確定させる決まりで、
// 先に差し替えると TAA が自分の出力を読む形になりグラフが循環する。
//
// 【段階6の途中である】切り出した群が必要とするぶんだけ載っている。
// 群を切り出すたびにここへ足していく。
namespace Kurenai::Rendering
{
    struct RenderBlackboard
    {
        // このフレームで空として使うキューブマップ(手続き空か .kscene の DDS か)。
        // **Reads 宣言と実際のバインドの両方でこれを使うこと。**
        // ActiveSkyTexture() を都度呼ぶと両者が食い違って依存解決が壊れる
        RHI::IRHITexture* SkyTexture = nullptr;

        // AO/GI パスが書いた先。ブラー後(表示・合成用)とブラー前(デバッグ表示用)。
        // どちらも同じアクセサから取ることで、書いた先と読む先が必ず一致する
        RHI::IRHITexture* ActiveAOTexture = nullptr;
        RHI::IRHITexture* ActiveAORawTexture = nullptr;

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
    };
}
