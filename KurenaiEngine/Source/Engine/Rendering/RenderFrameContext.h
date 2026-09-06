#pragma once

#include <cstdint>

#include <DirectXMath.h>

// フレームの先頭で確定し、グラフ登録の間ずっと変わらない値をまとめたスナップショット(段階6)。
//
// 【なぜスナップショットにするか】Render() は 6,800 行あり、パス群を別クラスへ出すと
// これらの値は「引数で渡すもの」になる。個別の引数で渡すと、パス群が増えるたびに
// 引数の数が増え、どの値がどのパスで要るのかも読めなくなる。1つの構造体へ集め、
// **const& で渡す**ことで、受け取る側が書き換えられないことも型で示せる。
//
// 【生存期間】Render() のローカルとして作り、graph.Execute() が終わるまで生かすこと。
// パスの Execute ラムダはこの構造体そのものではなく、**必要な値を値捕捉する**。
// 構造体を参照捕捉すると、登録関数を抜けた時点で参照が浮く。
//
// 【Blackboard との違い】こちらは登録が始まる前に確定している値。
// 登録の途中で確定していく出力は Rendering::RenderBlackboard が持つ。
//
// 【段階6の途中である】いまはパス群を1つずつ切り出している最中なので、
// フィールドは切り出した群が必要とするぶんだけ載っている。
// 群を切り出すたびにここへ足していく。
namespace Kurenai::Rendering
{
    struct RenderFrameContext
    {
        // このフレームの実効プリ露出を線形倍率にしたもの(ComputeExposure の結果)。
        // Tonemap / Bloom / AutoExposure が割り戻すため、絵には出ない
        float EffectiveExposure = 1.0f;

        // 平面反射パスを今フレーム走らせるか。
        // Present のデバッグ表示が「走っていないなら中身は残骸なので切り替えない」判断に使う
        bool PlanarReflectionPassRuns = false;

        // MegaLights のタイルジッタを含めた実効タイル数(X)。
        // 候補プールのデバッグ表示が、プールの添字を組み立てるのに使う
        uint32_t MegaLightsEffectiveTilesX = 0;

        // MegaLights のタイルジッタで格子をずらした量[画素]。
        // **書き手と読み手が同じ格子を読むこと** ―― デバッグ表示が別の格子を読むと
        // A/B の比較結果そのものが嘘になる
        DirectX::XMUINT2 MegaLightsTileOffset{ 0u, 0u };
    };
}
