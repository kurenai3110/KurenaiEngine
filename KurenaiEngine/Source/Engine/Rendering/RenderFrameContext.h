#pragma once

#include <cstddef>
#include <vector>
#include <cstdint>

#include <DirectXMath.h>

#include "RHI/IRHICommandList.h"
#include "../Diagnostics/RenderCapabilities.h"
#include "MeshletLODFrameConstants.h"
#include "GIResources.h"
#include "IBLResources.h"
#include "RenderTargets.h"
#include "SceneGPUResources.h"
#include "DroneShowResources.h"
#include "SkyResources.h"
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
    class IRHIDevice;
    class IRHISamplerSet;
    class IRHISwapChain;
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

        // ウィンドウのクライアント領域。**内部レンダー解像度とは別物**で、
        // Presentがレターボックスの余白を計算するのにだけ使う
        uint32_t WindowWidth = 0;
        uint32_t WindowHeight = 0;

        // デバイスの能力の写し。起動時とリソースの作り直しでしか変わらない
        RenderCapabilities Capabilities;

        // AOの「このフレームの有効な出力」。ブラー後(表示・合成用)と
        // ブラー前(デバッグ表示用)。設定と実行可否から決まる派生値で、
        // **どのパスも書かない**のでBlackboardではなくここに置く。
        // 【1フレームに1回だけ評価する】書いた先と読む先が食い違うと依存解決が壊れる
        RHI::IRHITexture* ActiveAOTexture = nullptr;
        RHI::IRHITexture* ActiveAORawTexture = nullptr;

        // 反射の「このフレームの有効な出力」。SSRならSSRTexture、RT反射ならその出力、
        // どちらも走らないならLightingの結果(SceneColor)がそのまま後段へ渡る。
        // AOと同じく設定と実行可否から決まる派生値で、どのパスも書かない
        RHI::IRHITexture* ActiveReflectionOutput = nullptr;

        // 超解像を走らせられる状態か(設定が有効で、出力テクスチャも確保できている)。
        // **実際に走らせたかはこれではない** ―― デバッグ表示中は等倍で見たいので
        // 走らせず、その判断は Blackboard の UpscaleActive が持つ
        bool UpscaleAvailable = false;

        // このフレームで空として使うキューブマップ(手続き空か .kscene の DDS か)。
        // **Reads 宣言と実際のバインドの両方でこれを使うこと。**
        // ActiveSkyTexture() を都度呼ぶと両者が食い違って依存解決が壊れる
        RHI::IRHITexture* SkyTexture = nullptr;

        // IBLの畳み込み結果一式。**エンジンが持ったままで、ここにはポインタだけ載せる**。
        // 中身はフレーム先頭で確定し、登録中は変わらない
        const IBLResources* IBL = nullptr;

        // 空・大気・雲のリソース一式。IBLと同じくエンジンが持ったまま、ポインタだけ載せる
        const SkyResources* Sky = nullptr;

        // GPU側のシーンデータ一式。シーンの読み込みで作り直されるが、
        // 作り直しは登録が始まるより前(UpdateSceneStreaming)に済んでいる
        const SceneGPUResources* Scene = nullptr;

        // 共有レンダーターゲット一式。**複数の群が読み書きするのでここが唯一の持ち主**
        // (Rendering/RenderTargets.h)。作り直しはフレーム先頭で済んでいる
        const RenderTargets* Targets = nullptr;

        // 間接光(DDGI・反射プローブ)のリソース一式。IBLと同じくポインタだけ載せる
        const GIResources* GI = nullptr;

        // 焼き上がりに影響する状態(時刻・太陽・シャドウ・IBL強度・全ライト)から作った署名。
        // 【フレームで1回だけ求める】反射プローブとDDGIの両方が「前に焼いたときから
        // 変わったか」の判定に使う。登録の途中では入力が動かないので、ここへ載せて配る
        uint64_t ProbeBakeSignature = 0;

        // ドローンショーのGPUリソース一式。IBLと同じくエンジンが持ったまま、ポインタだけ載せる
        const DroneShowResources* DroneShow = nullptr;

        // 今フレーム、ドローンの編隊を描くか。**判定はここが唯一の実装**で、
        // 本描画(PostProcess)と平面反射(Reflection)の2群が同じ述語を見る必要がある
        // (片方だけ描くと、空には編隊が出ているのに水面には映らない、の逆が起きる)
        bool DroneShowRuns = false;
        // 今フレーム描く機体数。1機につき2三角形なので、ドローは DroneCount * 6 頂点
        uint32_t DroneCount = 0;
        // 機体の明るさ(.kshowが持つ値)。実効プリ露出を掛けるのはパス側
        float DroneShowBrightness = 0.0f;
        // 遠方の機体が1画素を割ってTAAのジッターでちらつくのを防ぐ、画面上の最小半径(NDC単位)
        float DroneShowMinScreenRadius = 0.0f;

        // PresentパスがRenderGraphPassDesc::SwapChainTargetへ渡す
        RHI::IRHISwapChain* SwapChain = nullptr;

        // 【登録の途中で作るものにだけ使う】MegaLightsのダンプ構成が、読み戻しバッファを
        // 初めて要ったフレームでここから作る。**生成をフレーム先頭へ引き上げてはいけない**
        // ―― ダンプ構成でしか作られないバッファなので、位置を動かすとDX12の
        // ディスクリプタ枠の割り当て順が変わる
        RHI::IRHIDevice* Device = nullptr;

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

        // メッシュレットLODの段を選ぶ値。**主カメラのものを全パスへ同じだけ配る**
        MeshletLODFrameConstants MeshletLOD;

        // 自動露出が追従した結果のEV100。露出を焼き込む側(DDGI・反射プローブ)が読む。
        // 上の EffectiveExposure は線形の倍率で、これとは別物
        float EffectiveExposureEV100 = 0.0f;

        // 前フレームのViewProjとジッタ。TAAと、Hi-Zオクルージョンの視差の見積もりが読む。
        // **有効でないフレームは零行列に解決済み**にしてある。以前は群の側が
        // 「有効か」を見て同じ三項演算子を別々に書いていた
        DirectX::XMFLOAT4X4 TAAPrevViewProj{};
        DirectX::XMFLOAT2 TAAPrevJitterUv{};
        // 前フレームの露出。TAAが履歴の明るさを今フレームへ合わせ直すのに使う
        float TAAPrevEffectiveExposureEV100 = 0.0f;

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
