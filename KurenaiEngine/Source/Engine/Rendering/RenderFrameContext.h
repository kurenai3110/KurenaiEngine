#pragma once

#include <cstddef>
#include <vector>
#include <cstdint>

#include <DirectXMath.h>

#include "RHI/IRHICommandList.h"
#include "RenderSettingsSnapshot.h"

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
    struct GPULight;
}

namespace Kurenai::Passes
{
    struct LightingConstants;
}

namespace Kurenai::RHI
{
    class IRHIBuffer;
    class IRHISamplerSet;
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
        // 内部レンダー解像度。フレーム先頭の作り直しブロックで確定し、登録中は変わらない
        uint32_t RenderWidth = 0;
        uint32_t RenderHeight = 0;

        // フレーム全体で共有する定数バッファとサンプラー。所有権は KurenaiEngine3D にあり、
        // 所有権を動かさない下ごしらえとして、ここでは借用する生ポインタだけを保持する
        // フレーム先頭で確定し、登録中は作り直されない
        RHI::IRHIBuffer* FrameConstantBuffer = nullptr;
        RHI::IRHIBuffer* ObjectConstantBuffer = nullptr;
        RHI::IRHISamplerSet* MaterialSamplers = nullptr;
        RHI::IRHISamplerSet* ScreenSpaceSamplers = nullptr;

        // このフレームの実効プリ露出を線形倍率にしたもの(ComputeExposure の結果)。
        // Tonemap / Bloom / AutoExposure が割り戻すため、絵には出ない
        float EffectiveExposure = 1.0f;

        // 平面反射パスを今フレーム走らせるか。
        // Present のデバッグ表示が「走っていないなら中身は残骸なので切り替えない」判断に使う
        bool PlanarReflectionPassRuns = false;

        // MegaLights のタイルジッタを含めた実効タイル数(X)。
        // 候補プールのデバッグ表示が、プールの添字を組み立てるのに使う
        uint32_t MegaLightsEffectiveTilesX = 0;
        uint32_t MegaLightsEffectiveTilesY = 0;

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

        // 水面での鏡映変換と、それを掛けたビュー射影。平面反射パスが使う
        DirectX::XMMATRIX ReflectMatrix{};
        DirectX::XMMATRIX ReflectedViewProj{};

        // 水面の高さ[m]。鏡映の基準になる平面
        float WaterPlaneY = 0.0f;

        // 主カメラの位置とジッタ無しのビュー射影
        DirectX::XMFLOAT3 CameraPosition{ 0.0f, 0.0f, 0.0f };
        DirectX::XMMATRIX ViewProj{};

        // GPUへ送るライト配列と、直接光の定数。**どちらもRender()のローカルを指す**
        const std::vector<GPULight>* Lights = nullptr;
        const Passes::LightingConstants* Lighting = nullptr;

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

        // このフレームにどの経路が走るかの判定。**エンジンの Should* を Render() が
        // 1回だけ呼んだ結果**で、bool をここで作り直してはいけない。作り直すと
        // 「MegaLights は走るか」の定義が2つに割れ、静かに食い違う。
        // 判定に使う値(設定・レイトレシーンの有無・PSOやテクスチャの生成可否)は
        // どれも登録が始まる前に確定している
        bool MegaLightsRuns = false;
        bool LightCullingRuns = false;
        bool RaytracedShadowRuns = false;
        bool RaytracedAORuns = false;
        bool RaytracedReflectionRuns = false;
        bool RaytracedDDGITraceRuns = false;
        bool SuppressEmissiveForGI = false;
        // MegaLightsが1画素あたり何標本取るか(手法3以外は必ず1)
        int32_t MegaLightsSamplesPerPixel = 1;

        // ジオメトリの経路で、このフレームに何が有効かを配る一式
        bool DepthPrepassRuns = false;
        bool HiZFromDepthPrepass = false;
        bool OcclusionCullEnabledThisFrame = false;
        bool OcclusionCullingActive = false;
        bool MeshletPathActive = false;
        bool MeshletCullStatsActive = false;

        // 前フレームからカメラが動いた距離[m]。Hi-Zオクルージョンの判定が使う
        float CameraMoveDistance = 0.0f;

        // 太陽・月・空の状態と、GPUへ送った FrameConstants。
        // **どちらもRender()のローカルを指す。** graph.Execute()が終わるまで生きているので
        // 登録中に読んでよいが、Executeラムダへ渡すときは必要な値だけを値で写すこと
        const SunLighting* Sun = nullptr;
        const ShaderInterop::FrameConstants* Constants = nullptr;

        // 手続き空を今フレーム焼き直すか / 空の照度を積分し直すか
        bool BakeSkyThisFrame = false;
        bool SkyIntegrateThisFrame = false;

        // パス群が読む設定の写し。**エンジンの設定そのものではない**ので、
        // ここを書き換えてもUIには戻らない(戻す経路はエンジンのセッターだけ)
        RenderSettingsSnapshot Settings;
    };
}
