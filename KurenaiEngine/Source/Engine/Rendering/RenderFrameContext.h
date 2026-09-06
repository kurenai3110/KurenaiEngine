#pragma once

#include <cstddef>
#include <vector>
#include <cstdint>

#include <DirectXMath.h>

#include "RHI/IRHICommandList.h"

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
namespace Kurenai
{
    struct SunLighting;
}

namespace Kurenai::RHI
{
    class IRHITexture;
}

namespace Kurenai::ShaderInterop
{
    struct FrameConstants;
}

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

        // 手動露出時にTonemap/Bloomが割り戻す倍率
        float ManualExposureScale = 1.0f;

        // 自動露出の測光値を上側で止めるための、構図に依存しない基準EV
        float KeyReferenceEV100 = 0.0f;

        // 主カメラの行列。JitteredProjはTAAのジッタを含む
        DirectX::XMMATRIX ViewMatrix{};
        DirectX::XMMATRIX JitteredProj{};
        DirectX::XMMATRIX InvViewProj{};

        // このフレームのTAAジッタ量[UV]
        DirectX::XMFLOAT2 JitterUv{ 0.0f, 0.0f };

        // 内部レンダー解像度のビューポート。
        // **ラムダへは値で渡すこと** ―― 登録関数を抜けたあとにExecuteが走る
        RHI::Viewport GBufferViewport{};

        // シャドウマップ解像度のビューポート
        RHI::Viewport ShadowViewport{};

        // プローブのキューブ面キャプチャが使う射影。反射プローブとDDGIで同じ値を使う
        DirectX::XMMATRIX ProbeFaceProjection{};

        // プローブのキャプチャが読むテクスチャ一式。**Render()のローカルを指す**
        const std::vector<RHI::IRHITexture*>* ProbeCaptureReads = nullptr;

        // 焼き込みに入れてよい灯の数。ライト配列の並べ替え後の総数とは別物なので
        // 添字として使い回さないこと(KurenaiEngine3D.cpp の該当コメント参照)
        size_t BakedLightCount = 0;

        // カスケードごとのライト視点ビュー射影。**Render()のローカル配列を指す。**
        // 要素数は KurenaiEngine3D::kCascadeCount。graph.Execute()が終わるまで生きている
        const DirectX::XMMATRIX* CascadeViewProj = nullptr;

        // このフレームの空が手続き空か(.ksceneのDDSではないか)
        bool UsingProceduralSky = false;

        // 大気遠近パスを今フレーム走らせるか
        bool FogPassRuns = false;

        // 太陽・月・空の状態と、GPUへ送った FrameConstants。
        // **どちらもRender()のローカルを指す。** graph.Execute()が終わるまで生きているので
        // 登録中に読んでよいが、Executeラムダへ渡すときは必要な値だけを値で写すこと
        const SunLighting* Sun = nullptr;
        const ShaderInterop::FrameConstants* Constants = nullptr;

        // 手続き空を今フレーム焼き直すか / 空の照度を積分し直すか
        bool BakeSkyThisFrame = false;
        bool SkyIntegrateThisFrame = false;
    };
}
