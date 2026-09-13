#pragma once

#include <Windows.h>

#include <map>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <string>
#include <thread>
#include <vector>

#include "Diagnostics/CullStatsReadback.h"
#include "Diagnostics/FrameStatsLogger.h"
#include "Diagnostics/RenderDumpServiceState.h"
#include "Diagnostics/ScheduledRecreationQueue.h"
#include "GI/DDGIGrid.h"
#include "Settings/EngineSettings.h"
#include "UI/EngineUIHost.h"
#include "DroneShow.h"
#include "EngineDefaults.h"
#include "KurenaiEngineBase.h"
#include "KurenaiTypes.h"

#include "Assets/MeshLightScene.h"
#include "Assets/RaytracingScene.h"
#include "Assets/Scene.h"
#include "Assets/TextureStreaming.h"
#include "Core/Camera.h"
#include "Core/CPUProfiler.h"
#include "Diagnostics/RenderCapabilities.h"
#include "Diagnostics/RenderStats.h"
#include "Diagnostics/ScheduledRecreation.h"
#include "Rendering/DroneShowResources.h"
#include "Rendering/GIResources.h"
#include "Rendering/IBLResources.h"
#include "Rendering/SkyResources.h"
#include "Rendering/SceneGPUResources.h"
#include "Rendering/RenderTargets.h"
#include "Rendering/ShadowConstants.h"
#include "Rendering/MeshletLODFrameConstants.h"
#include "Passes/PassHost.h"
#include "Rendering/GeometryDrawHost.h"
#include "Rendering/GeometryDrawTypes.h"
#include "Rendering/DroneShowSystem.h"
#include "Rendering/FrameHistoryState.h"
#include "Rendering/SceneDrawList.h"
#include "Scene/EmissiveLightSet.h"
#include "Scene/InstanceLODState.h"
#include "Scene/ModelStreamingState.h"
#include "Scene/RaytracingRebuildState.h"
#include "Scene/SceneLoadHandoff.h"
#include "Rendering/CubeFaceMath.h"
#include "Passes/DDGIConstants.h"
#include "Passes/EnvironmentConstants.h"
#include "Passes/GeometryConstants.h"
#include "Passes/MegaLightsConstants.h"
#include "Passes/ReflectionProbeConstants.h"

#pragma warning(push)
#pragma warning(disable: 4251)

namespace Kurenai::Core
{
    // IssueTextureDumps / RegisterPasses / WritePassManifestIfDue の引数にだけ使う。
    // RenderGraph.hはRHIのヘッダ群を芋づるで引き込むため、このヘッダをインクルードする側
    // (UIパネル・Sample3D)へ広げないよう前方宣言で済ませる
    class RenderGraph;
}

namespace Kurenai::UI
{
    class UIManager;
}

namespace Kurenai::Passes
{
    // Render()から切り出したパス群(段階6)。実体はPasses/*.hにあり、
    // そちらはこのヘッダをインクルードするため、ここでは前方宣言で止める
    class DDGIPasses;
    class EnvironmentPasses;
    class GeometryPasses;
    class LightingPasses;
    class MegaLightsPasses;
    class PostProcessPasses;
    class ReflectionPasses;
    class ReflectionProbePasses;
    class ShadowPasses;
    class PresentPass;
}

namespace Kurenai::ShaderInterop
{
    // AdvanceFramePrevViewState()の引数にだけ使う。実体はShaderInterop/FrameConstants.hにあり、
    // このヘッダを公開APIから重くしないため前方宣言で止める
    struct FrameConstants;
}

namespace Kurenai::Rendering
{
    // Render()から切り出した関数(ResolveFrameCullStats / RegisterPasses)の引数にだけ使う。
    // 実体はRendering/RenderBlackboard.h・RenderFrameContext.hにあり、
    // そちらはRHIのヘッダ群を引き込むため前方宣言で止める
    struct RenderBlackboard;
    struct RenderFrameContext;
}

namespace Kurenai::Passes
{
    // BuildFrameContext の出力引数にだけ使う。実体は Passes/LightingConstants.h
    struct LightingConstants;
}

namespace Kurenai
{
    // BuildFrameContext の引数にだけ使う。どちらも参照で受けるだけなので、
    // 公開ヘッダの重さを増やさないよう前方宣言で止める
    // (実体は Rendering/GPULight.h と Rendering/SunLighting.h)
    struct GPULight;
    struct SunLighting;
}

namespace Kurenai
{
    // 実体は Rendering/SceneDrawList.h(インスタンシングのレコードと同じ持ち主)
    using GPUModelInstance = Rendering::GPUModelInstance;

    // 3Dサンプルプログラム向けの公開API。Deferred Shading(G-Buffer)によるPBRレンダリング、
    // シャドウマッピング、SSAO/SSIL(間接光)、SSR(反射)、ImGuiによる各種設定パネル、
    // 複数シーンの切り替えまでを内包した完結型のレンダラー。
    // 構築してRun()を呼ぶだけでウィンドウが開き、終了するまでブロックする
    class KURENAI_3D_API KurenaiEngine3D
        : public KurenaiEngineBase, public Diagnostics::IRecreationTarget, public UI::IEngineUIHost,
          public Rendering::ILODSelector, public Passes::IPassHost
    {
    public:
        // renderWidth/renderHeight: G-Buffer以降の内部解像度(ウィンドウサイズとは独立。
        //   実行時に「システム」パネルからも変更できる)。
        // initialSceneIndex: 起動時に読み込むシーンの番号(Assets/Scenes/*.ksceneをファイル名の
        //   昇順に並べたときの位置)。範囲外なら0にフォールバックする。
        //   グラフィックスAPIを切り替える際、呼び出し側が同じシーンで作り直すために使う
        explicit KurenaiEngine3D(
            GraphicsAPI api = GraphicsAPI::DX11, uint32_t renderWidth = Defaults::RenderWidth,
            uint32_t renderHeight = Defaults::RenderHeight, size_t initialSceneIndex = 0);
        ~KurenaiEngine3D();

        void Run();

        // --- グラフィックスAPIの実行時切り替え ---
        // Run()の中でUI(「システム」パネル)からRequestGraphicsAPIChange()が呼ばれると、
        // Run()はウィンドウを閉じずにループを抜けて戻る。呼び出し側は下の2つを見て、
        // このオブジェクトを破棄してから新しいAPIで作り直すこと(Samples/Sample3D/Source/Main.cpp)。
        //
        // デバイスだけを差し替えるのではなくオブジェクトごと作り直すのは、破棄の順序
        // (派生クラスの全リソース → スワップチェーン → デバイス → ウィンドウ)を
        // C++のメンバ破棄順にそのまま任せられるため。手書きの解放関数にすると、
        // メンバを追加したときに解放漏れが静かに発生する
        bool HasPendingGraphicsAPIChange() const;
        GraphicsAPI GetPendingGraphicsAPI() const;
        // 作り直しへ引き継ぐ状態。上記以外の設定(AO・シャドウ・IBL・ポストプロセス等)は
        // 新しいインスタンスで既定値に戻る
        uint32_t GetRenderWidth() const { return m_RenderWidth; }
        uint32_t GetRenderHeight() const { return m_RenderHeight; }
        size_t GetCurrentSceneIndex() const { return m_CurrentSceneIndex; }

        // デバッグ表示を番号で選ぶ(番号の並びはUIの「デバッグ表示」コンボと同じ)。
        // 範囲外の番号は無視してログを残す(呼び出し側で範囲を知らなくてよいようにする)。
        // GUIのつまみではなく起動オプションで持つ理由は docs/ImplementationDetail.md 64章
        void SetDebugViewIndex(int index);

        // DDGIのレイの取得をラスタライズへ強制する(既定はDXRが使えるならDXR)。
        // ラスタ経路とレイトレース経路のA/B用(docs/ImplementationDetail.md 64.4)
        void ForceDDGIRayModeRaster();

        // プローブ分類のしきい値を上書きする(0以下なら分類そのものを無効にする)。
        // しきい値の効き方をA/Bで測るための起動オプション用。根拠はm_Settings.DDGI.BackfaceThresholdを参照
        void SetDDGIBackfaceThreshold(float threshold);

        // DDGIのクリップマップLODの段数と追従の有無を、読み込んだ`.kscene`の指定より優先して上書きする。
        // 段数を振って効果を測るための起動オプション用。アトラスを確保し直すので、
        // **フレームの記録が始まる前(Run()より前)にだけ呼ぶこと**
        void OverrideDDGILOD(uint32_t lodCount, bool followCamera);

        // MegaLightsの手法と、1灯あたりに撃つ影レイの本数を起動時に上書きする。
        // mode は MegaLightsMode の値(0=なし, 1=参照実装)。
        // mode / shadowRayCount / sampleCount のいずれも、負の値を渡すとその項目は既定のままにする
        // (一部だけの指定ができるようにするため)。sampleCount は確率的サンプリングが
        // 1ピクセルあたりに候補プールから引く数(RISのM)。
        //
        // 影レイ0本(恒等テスト)と従来のライトループの一致を数値で確かめるための口。
        // **A/Bは必ず起動直後から同じ手順で行うこと**(docs/ImplementationDetail.md 64章)
        //
        // 範囲外の値は無視してログを残す(呼び出し側が範囲を知らなくてよいようにする)。
        // レイトレーシング非対応の環境では手法を変えてもパスが走らない(ShouldRunMegaLights)
        void OverrideMegaLights(int mode, int shadowRayCount, int sampleCount);

        // エミッシブ光源(自発光メッシュを光源として扱う)の切り替え。
        //
        // 自発光面はG-Bufferへ書いて加算されるだけで周囲を照らしていない。有効にすると、
        // 読み込み時に自発光メッシュから起こした光源のかたまりをGPULight(LightType 3)として
        // 従来のライトループにもMegaLightsにも流す。**有効にすると絵が明るくなる。**
        //
        // enabled は 0=無効 / 正=有効 / **負なら既定のまま**(OverrideMegaLightsと同じ約束)。
        // しきい値だけを差し替えたいときに、状態まで巻き添えで倒さないための三値にしてある。
        // cutoffIrradiance は打ち切り照度τ(0以下で既定のまま)。Rangeをこれから解く。
        // maxCount は採用するプロキシ数の上限(0以下で既定のまま)。
        // **上限に当たったら切り捨てではなく併合を疑うこと** ―― 面積の大きい順に上位を
        // 残す形はEmeraldSquareの実測で発光の半分以上を捨てる。
        // doubleCountGI は 0=DDGIから自発光を抜く(既定) / 正=抜かずに二重に数える /
        // **負なら既定のまま**。抑止されるのはDDGIだけで、反射プローブ・RT反射・
        // G-Bufferの自発光には掛からない(鏡面が光源を直接見ているのは二重計上ではない)
        void SetEmissiveLights(int enabled, float cutoffIrradiance, int maxCount, int doubleCountGI);

        // 段階2: 発光面を三角形のまま面積分するか(0=無効 / 正=有効 / 負=既定のまま)。
        //
        // 【MegaLights 経路でのみ効く】DX11・非DXR・MegaLights無効のときは何も起きず、
        // 段階1のプロキシがそのまま光る。エミッシブ光源そのものが無効なら三角形も出ない。
        // **いまは参照実装(全三角形総当たり)しか無いので実シーンでは回らない。**
        void SetMeshLights(int enabled);

        // シーン全体の自発光の強度倍率(ImGuiの「自発光の強度」と同じ値)。0以下で既定のまま。
        //
        // 単位の正しさを絵で確かめるために振れるようにしてある
        // (実測と根拠は docs/ImplementationDetail.md 64.6)
        void SetEmissiveIntensity(float intensity);


        // MegaLightsの出力を線形空間で何フレーム足し込むかを設定する(0で蓄積しない)。
        // 指定した枚数に達したら足すのを止めるので、表示が静止し
        // 「ちょうどNサンプルの平均」を決定的に撮れる。
        //
        // 線形空間で足す場所をエンジン側に持つ理由は docs/ImplementationDetail.md 61.7b / 64.6
        void SetMegaLightsAccumFrames(int frames);

        // 蓄積し終えた平均を、指定パスへ生データ(float4 × 画素数)で書き出す。
        // 形式: 'K','M','L','A' / uint32 幅 / uint32 高さ / uint32 足したフレーム数 / uint32 予約 /
        //       そのあとに float4 が 幅×高さ 個(index = y * 幅 + x)。
        //
        // 線形のまま倍精度で取り出す理由は docs/ImplementationDetail.md 61.5 / 64.6
        void SetMegaLightsDumpPath(const wchar_t* path);

        // 空間再利用の有無と、借りる近傍の数・半径を起動時に上書きする。
        // いずれも負の値を渡すとその項目は既定のままにする。
        // 効果を同じ手順で撮り比べるための口(docs/ImplementationDetail.md 64章)
        void SetMegaLightsSpatial(int enabled, int neighborCount, int radius, int useMIS);
        // 初期サンプルの可視レイ(遮蔽されたサンプルをリザーバごと殺す)の有無。負の値は既定のまま
        void SetMegaLightsInitialVisibility(int enabled);
        // 【計測専用】自動露出の有効/無効を起動時に決める。
        //
        // UI(PostProcessPanel)は m_Settings.PostProcess.AutoExposureEnabled を直接触るが、起動オプションから
        // 同じ状態を作れないと「画面で見ていた設定」と「計測で走らせた設定」を揃えられない。
        // 揃っていない条件どうしの比較は、差が手法の差なのか設定の差なのか分けられない
        void SetAutoExposureEnabled(bool enabled);

        // 【計測専用】Hi-Zオクルージョンカリングの有効/無効を起動時に決める。
        // 有効/無効で絵が1画素も変わらないことが、このカリングの正しさの定義そのもの
        // (docs/ImplementationDetail.md 64.4)。**A/Bは起動直後から同じ手順で行うこと**
        void SetOcclusionCullingEnabled(bool enabled);

        // 【計測専用】メッシュレット描画の有効/無効を起動時に決める。
        // 無効にすると従来の頂点シェーダー + DrawIndexed の経路へ落ちる。この経路は
        // メッシュレット単位のカリングを行わないので、「メッシュレット経路が何か
        // 落としていないか」を見るときの基準になる(docs/ImplementationDetail.md 64.4)
        // この基準を同じ起動手順で撮れない
        void SetMeshletRenderingEnabled(bool enabled);

        // 蓄積するものは測る前に切る(docs/ImplementationDetail.md 64.3)
        void SetTAAEnabled(bool enabled);

        void SetAOTechnique(int technique);
        void SetSoftwareRasterEnabled(bool enabled);
        void SetDDGIHalfResolutionEnabled(bool enabled);
        void SetProbeUpdateMode(int mode);
        void SetUpscaleEnabled(bool enabled);
        void SetFixedTimeStep(float seconds);

        // 【計測専用】.ksceneの[CameraPath]を名前で1本選んで再生する。
        //
        // 【何のためにあるか】「カメラを動かしたときのノイズと遅れ」を測るには同じ軌跡を
        // 何度でも再現できる必要があるが、通常の操作経路は移動量がΔtに比例し、視点回転は
        // GetAsyncKeyState(VK_RBUTTON)を見るのでPostMessageからは駆動できない
        // (UpdateMouseLookのコメント参照。これは実カーソルを守るための意図した設計)。
        // 再生中は視点の入力操作を一切受け付けず、フレーム番号だけから姿勢が決まる。
        //
        // 名前が見つからない場合はErrorログを出し、**従来の入力操作のまま続行する**
        // (黙って落とさず、黙って再生もしない)。nullptrや空文字列を渡すと再生を止める
        void SelectCameraPath(const wchar_t* name);
        // 経路の再生を開始するフレーム番号。それまでは先頭キーの姿勢で静止し、
        // 履歴・リザーバ・ストリーミング・内部解像度が整定するのを待つ。
        // 負を渡すと既定(Passes::kMegaLightsAccumWarmup = -dumpframe の既定と同じ定数)になる
        void SetCameraPathStartFrame(int frame);
        // シーンが持つ[CameraPath]すべてについて「本当に画面が動くか」の検算ログを出す。
        // 【シーンの適用を待ってから出す】起動オプションの適用時点ではまだシーンが
        // 読み終わっていないことがあるので、ここでは要求を立てるだけにする
        void SetCameraPathValidate(bool enabled);

        // 【計測専用】GPUの区間計測をウォームアップ後に指定枚数ぶん集計し、
        // パス名ごとの平均[ms]をCSVへ書き出して終了する。
        // Perfログでは足りない理由は docs/ImplementationDetail.md 61.7e.1
        void SetPerfDump(const wchar_t* path, int frames);
        void SetPassManifest(const wchar_t* path, int frames);

        // 【検証専用】中間レンダーターゲットの中身を、線形の生値のままファイルへ書き出す。
        // nameは GetDumpableTextureNames() が返す名前(m_を外したメンバ名)。
        // 何のためにあるか・形式・使い方は docs/ImplementationDetail.md 63章。
        //
        // 複数回呼べば1回の起動で複数枚を同じフレームから落とす(GUIの起動は共有資源なので、
        // 1回の起動で必要な数値が全部取れる形にすること)。
        // 未知の名前・存在しないテクスチャ・非対応フォーマットはログを出して無視する
        void AddTextureDump(const wchar_t* name, const wchar_t* path, int mipLevel, int arraySlice, int frames, int stride)
        {
            m_DumpService.AddTextureDump(name, path, mipLevel, arraySlice, frames, stride);
        }

        // 何フレーム目のものを書き出すか。負なら既定(Passes::kMegaLightsAccumWarmup)。
        // **整定を待たずに撮ると、内部解像度が既定値のままの絵を掴む**(実際に起きた)
        void SetTextureDumpFrame(int frame) { m_DumpService.SetTextureDumpFrame(frame); }

        // 書き出しが全部終わったらウィンドウを閉じる。無人での検証用
        void SetExitAfterDump(bool enabled) { m_DumpService.SetExitAfterDump(enabled); }

        // 【検証専用】指定フレームでGPUリソースの作り直し経路を踏ませる予約を積む。
        // 複数回呼べば1回の起動で複数の経路を順に踏む(GUIの起動は共有資源なので、
        // 1回の起動で必要な経路が全部通る形にすること)。
        // 種別と各フィールドの意味は Diagnostics/ScheduledRecreation.h を見ること
        void AddScheduledRecreation(const ScheduledRecreation& request)
        {
            m_Recreations.Add(request);
        }

        // -dumptex が受け付けるテクスチャ名の一覧(表示・ログ用)。
        // ClaudeのようなUIを見られない利用者にとって、これが唯一の発見手段になる
        std::vector<std::string> GetDumpableTextureNames() const;

        // デノイザの有無、a-trousの段数、時間累積の上限。負/0は既定のまま
        void SetMegaLightsDenoise(int enabled, int atrousPasses, int maxFrames);
        // 輝度のエッジ停止の強さ(負なら既定のまま)
        void SetMegaLightsDenoiseSigmaLuminance(float sigma);
        // ファイアフライの近傍クランプの強さ(0で無効。負なら既定のまま)
        void SetMegaLightsDenoiseFireflyClamp(float k);
        // デノイザの時間累積が履歴の色を引くときの再サンプリングを Catmull-Rom にするか。
        // バイリニアだと毎フレーム補間が重なり、移動中の鮮鋭さが累積的に失われる
        // (根拠と実測は EngineDefaults.h の MegaLightsDenoiseHistoryCatmullRom)
        void SetMegaLightsDenoiseHistoryCatmullRom(bool enabled);
        // 履歴の妥当性を2x2の4タップで判定するか(既定は最近傍1タップ)。
        // 根拠は EngineDefaults.h の MegaLightsDenoiseHistory4Tap
        void SetMegaLightsDenoiseHistory4Tap(bool enabled);
        // 空間再利用の反復回数(負なら既定のまま)
        void SetMegaLightsSpatialIterations(int iterations);
        // 時間再利用の有無と、履歴のMの上限。負/0は既定のまま
        void SetMegaLightsTemporal(int enabled, int mClamp);
        // クアッド共有(手法3)の設定。いずれも負の値ならその項目は既定のまま。
        //   share    … 2x2の仲間が撃ったレイの結果を借りるか。**0が陽性対照**で、
        //               手法2から再利用を外した構成と画素単位で一致するはず
        //   stratify … クアッドの4画素へ候補スロットを分けて引かせるか
        //   blockedCache … 遮蔽が確定した灯のキャッシュを使うか(陽性対照では0にする)
        void SetMegaLightsQuadShare(int share, int stratify, int blockedCache);
        // クアッド共有(手法3)の1画素あたりの標本数。1〜kMegaLightsMaxSamplesPerPixel。
        // 影レイの本数がそのままこの数になるので、コストはほぼ比例して増える
        void SetMegaLightsQuadSamples(int samples);
        // 候補プールが1タイルあたりに抽出する灯の数(K)。
        // kMegaLightsTilePoolMinCapacity 〜 kMegaLightsTilePoolCapacity
        void SetMegaLightsTilePoolCapacity(int capacity);
        // 候補プールのタイル格子を画素単位でずらすモード。
        // 0=無効(従来とビット同一)、1=Halton(2,3)、2=有効だが検証用にオフセット0固定。
        // 範囲外はログを出して無視し、負の値では既定値の状態をログへ残す
        void SetMegaLightsTileJitter(int mode);

        // 【検証専用】蓄積が始まった瞬間にシーンへ摂動を加える。時間再利用の「追従」を測る入口。
        //   0 = 何もしない(既定)
        //   1 = 全ライトを消す。ゴースト(灯を消しても明かりが残る)の追従フレーム数を測る
        //   2 = 実効プリ露出EV100を +2 段跳ばす。プリ露出の補正が効いているかを測る
        // 測り方は docs/ImplementationHistory.md 67章
        void SetMegaLightsPerturb(int mode);

        // カスケードシャドウマップの分割数。カメラ視錐台をこの数だけの深度範囲に分割し、
        // それぞれ専用のシャドウマップ・ライト正射影を持たせる。
        // FrameConstants::CascadeSplitsがXMFLOAT4(4要素)にfar距離を詰めているため、
        // この値を変える場合はKurenaiEngine3D.cppのCascadeSplits周りも合わせて変更が必要。
        // KurenaiEngine3D.cpp側の匿名名前空間(FrameConstants宣言)からも参照するためpublicにしている
        // 出所は Rendering/ShadowConstants.h(移行中の別名)
        static constexpr uint32_t kCascadeCount = Rendering::kCascadeCount;
        static_assert(kCascadeCount == 4, "CascadeSplitsはXMFLOAT4前提のため4カスケード固定");

        // --- オーサリングツール向けの口(Tools/KurenaiShowEditor) ------------------------
        //
        // ドローンショーの編集UIと形状生成は、出荷するエンジンのDLLに持ち込みたくない。
        // かといってエディタが自前でレンダラーを持つと、トーンマップ・ブルーム・露出が
        // 本番と違う経路を通り、そこで作った形は本番で見ると別物になる。
        // そこでエディタはこのエンジンをそのまま使い、下の2つだけを追加で呼ぶ。
        // IPanel/UIWidgetsといった内部の型を公開せずに済むよう、口はこの2つに留める。

        // 全パネルを描いた後に一度だけ呼ばれる追加のImGui描画。nullptrで解除。
        // 【Renderスレッドで呼ばれる】ImGuiの状態を触るのはRenderスレッドだけという
        // 不変条件があるため。コールバックの中からエンジンの状態を触ってよいのは、
        // それがRenderスレッドの持ち物である限りにおいて
        void SetExtraImGuiCallback(std::function<void()> callback);

        // 再生中のショーを差し替える(エディタのプレビュー用。ファイルを書かずに絵へ反映する)。
        // 【SetExtraImGuiCallbackで登録したコールバックの中から呼ぶこと】どちらもRenderスレッドで
        // 走るため、この経路なら同期が要らない。別スレッドから呼ぶとm_Drones.Showを
        // 描画中に書き換えることになる
        void ApplyDroneShowData(const Assets::ShowData& data);

        // --- UIパネルから呼ばれる操作 ---

        // シーン切り替えを要求する(ScenePanel = Renderスレッドから呼ばれる)。
        // 実際の読み込みはLoaderスレッドが行うため即座に戻る。
        // 読み込み中に再度要求された場合は新しい要求で上書きされる(最後の要求が勝つ)
        void RequestSceneLoad(size_t sceneIndex) override;
        // 平面反射の反射解像度の倍率変更を要求する(RenderingPanel = Renderスレッドから呼ばれる)。
        // RequestRenderResolutionと同じ方式(要求を記録するだけにしてRender()の先頭でまとめて反映)
        void RequestPlanarReflectionResolutionScale(float scale);
        // SSAO/SSILの半径・厚みとSSRの距離・厚みを、現在のシーンの対角長から決め直す。
        // これらは固定の既定値を持たないため、UIの「既定値に戻す」ではなくこれを呼ぶ
        // (シーン読み込み時はApplyLoadedSceneから呼ばれる)
        void ResetSceneDependentParams();
        // ProfilerPanel用。m_DeviceはKurenaiEngineBaseのprotectedメンバであり、
        // 派生クラスの外からは直接触れないため、ここで明示的に橋渡しする
        float GetLastFrameGPUWaitTimeMs() const;
        // SystemPanelの表示用。m_Windowも同様の理由で橋渡しする。
        // Windowsのディスプレイ設定で指定されている拡大率(UIの拡大率もこれに追従する)
        float GetMonitorDpiScale() const;
        // 超解像の設定をまとめて要求する(SystemPanel = Renderスレッドから呼ばれる)。
        // 内部でRequestRenderResolution()を呼ぶだけで、レンダーターゲットの作り直しはしない
        void RequestUpscaleSettings(
            bool enabled, UpscaleQualityMode mode, uint32_t outputWidth, uint32_t outputHeight);
        // このフレームでMegaLightsパスを実行するか。上のShouldRunRaytraced*と同じ作法で1か所に集約している。
        // これがfalseのときDirectLighting.hlslは従来のライトループへ戻る ―― 「パスを積むか」と
        // 「ライトループを止めるか」がずれると、ライトが二重に加算されるか、逆に全部消える
        bool ShouldRunMegaLights() const;
        // プリセットを適用する(SystemPanel = Renderスレッドから呼ばれる)
        void ApplyQualityPreset(QualityPreset preset);
        // いま選ばれている段を1つだけ返す(フェード中でも切り替え先だけ)。
        // 半透明・平面反射・ソフトウェアラスタライザ用 ―― これらはクロスディザを実装しておらず、
        // 2段を重ねると同じ画素に両方が描かれてしまうため、フェード中も1段に決め打つ
        const Assets::Model* GetCurrentLOD(size_t instanceIndex) const override;
        // 「システム」パネルのグラフィックスAPI切り替えコンボから呼ばれる。
        // 実際の作り直しはRun()から戻った後に呼び出し側が行う(上のHasPendingGraphicsAPIChange参照)
        void RequestGraphicsAPIChange(GraphicsAPI api);

        // --- UIパネル向けのアクセサ ---
        //
        // UIパネル群(Source/Engine/UI/)がImGuiウィジェットへアドレスで渡すもの、および表示に
        // 使うものだけをここで名指しする。**この一覧に無いもの(RHIリソース・PSO・履歴バッファ等)
        // へUIから触らせない**のがこの区画の目的なので、増やすときは本当にUIの表示・操作に
        // 要るものかを確かめること。
        // UIが書き換えるものは非const参照(アドレスを渡す/直接代入するため)、読むだけのものは
        // const(値またはconst参照)で返す。
        // 【必ず参照で返すこと】UIはここへ直接書き込む。値で返すと一時オブジェクトを
        // 掴んで操作が効かなくなるが、**コンパイルは通ってしまう**
        Settings::EngineSettings& GetSettings() override { return m_Settings; }

        // 【転送が要る】GetWidth/GetHeightはKurenaiEngineBaseの非仮想メンバで、
        // 別の基底の純粋仮想を満たさない。ここで明示的に橋渡しする
        uint32_t GetWidth() const override { return KurenaiEngineBase::GetWidth(); }
        uint32_t GetHeight() const override { return KurenaiEngineBase::GetHeight(); }

        std::vector<Assets::Light>& GetLights() { return m_Lights; }
        int& GetSelectedLightIndex() { return m_SelectedLightIndex; }
        bool& GetBufferPrecisionDirty() { return m_BufferPrecisionDirty; }
        bool& GetSkyBakeDirty() { return m_SkyBakeDirty; }
        // 焼き上がりの状態の持ち主は Passes::EnvironmentPasses。ここは委譲するだけ
        bool& GetIBLBaked();
        bool& GetIBLIrradianceBaked();
        bool& GetEmissiveLightsCapLogged() { return m_EmissiveLights.CapLogged; }
        bool& GetEmissiveLightsValuesLogged() { return m_EmissiveLights.ValuesLogged; }
        bool& GetDDGIEmissiveSuppressLoggedRaster();
        bool& GetDDGIEmissiveSuppressLoggedTrace();
        std::vector<Assets::ReflectionProbe>& GetReflectionProbes() { return m_GIResources.ReflectionProbes; }
        int& GetSelectedProbeIndex() { return m_SelectedProbeIndex; }
        // 焼き上がりの状態の持ち主は Passes::ReflectionProbePasses。ここは委譲するだけ
        bool& GetProbeBaked();
        bool& GetProbeBakeRequested();
        bool& GetDDGIUpdateSuspended();
        uint32_t& GetDDGIStableCycles();
        Assets::TextureStreamingManager& GetTextureStreaming() { return m_TextureStreaming; }

        // std::atomicは呼び出し側が使っているメモリオーダーの書き方(.load/.store)を
        // そのまま維持できるよう、値ではなくatomicへの参照を返す
        std::atomic<bool>& GetTAAHistoryValid() { return m_History.HistoryValid; }
        std::atomic<uint32_t>& GetSceneLoadProgressLoaded() { return m_SceneLoad.ProgressLoaded; }
        std::atomic<uint32_t>& GetSceneLoadProgressTotal() { return m_SceneLoad.ProgressTotal; }

        uint32_t GetHiZMipLevels() const;
        const std::vector<Assets::EmissiveProxy>& GetEmissiveProxies() const { return m_EmissiveLights.Proxies; }
        const RenderStats& GetRenderStats() const { return m_RenderStats; }
        RHI::IRHIGPUProfiler* GetGPUProfiler() const { return m_GPUProfiler.get(); }
        const Core::CPUProfiler& GetCPUProfiler() const { return m_CPUProfiler; }
        uint32_t GetProbeRealtimeProbeIndex() const;
        uint32_t GetProbeRealtimeFace() const;
        const Assets::Scene& GetScene() const { return m_Scene; }
        // 【publicにしてある】G-Bufferパスが水面インスタンスのt7へ張る。
        // 差し替えるのはシーン読み込み(Scene/SceneLoadService.cpp)なので持ち主は変えない
        RHI::IRHITexture* GetWaterNormalMapTexture() const { return m_WaterNormalMapTexture.get(); }
        // 【publicにしてある】カウンタをGPUからコピーするのはジオメトリのパス群で、
        // 読み戻して数値にするのはこちらのRender()。受け皿のリングはこちらが持つ
        RHI::IRHIBuffer* GetMeshletCullStatsReadbackSlot() const { return m_CullStats.GetMeshletWriteSlot(); }
        RHI::IRHIBuffer* GetModelCullReadbackSlot() const { return m_CullStats.GetModelWriteSlot(); }
        const RenderCapabilities& GetRenderCapabilities() const { return m_RenderCapabilities; }
        bool GetHasGIVolume() const { return m_GIResources.HasGIVolume; }
        const Assets::GIVolume& GetGIVolume() const { return m_GIResources.GIVolume; }
        uint32_t GetDDGIProbeCount() const { return m_GIResources.DDGIProbeCount; }
        bool GetDDGIWarmingUp() const;
        ReflectionMode GetSceneDefaultReflectionMode() const { return m_SceneDefaultReflectionMode; }
        bool GetSceneLoadInFlight() const { return m_SceneLoad.InFlight; }
        const std::vector<std::wstring>& GetSceneDisplayNames() const { return m_SceneDisplayNames; }
        size_t GetSceneLoadingIndex() const { return m_SceneLoad.LoadingIndex; }
        const QualitySettings& GetQualitySettings() const { return m_Settings.Quality; }
        uint32_t GetLightTileCountX() const { return m_RenderTargets.LightTileCountX; }
        uint32_t GetLightTileCountY() const { return m_RenderTargets.LightTileCountY; }
        GraphicsAPI GetGraphicsAPI() const { return m_GraphicsAPI; }

        // m_DeviceはKurenaiEngineBaseのprotectedメンバであり、GetLastFrameGPUWaitTimeMsと
        // 同じ理由でここから明示的に橋渡しする。
        // 生成に失敗していればnullptrになりうるので、参照ではなくポインタで返す
        RHI::IRHIDevice* GetDevice() { return m_Device.get(); }


        // --- パス群と Present がコールバックの中から呼ぶ公開の口 ---

        // このインスタンスを「1回のDispatchMeshでモデル全体」の経路で描けるか。
        // 描けない場合は従来どおりメッシュ単位のループで描く
        // modelは「このパスが描く段」。モデルLODが入ったのでinstance.Model(最も詳細な段)とは
        // 限らず、シャドウは最も粗い段、G-Buffer/プリパスは選ばれた段を渡す
        //
        // 【publicにしてある】ForEachGeometryDrawと対で使う述語で、Passes/*の各群が
        // コールバックの中から呼ぶ。状態を持たない判定なので公開しても持ち主は変わらない
        bool ShouldUseModelMeshletPath(
            const Assets::ModelInstance& instance, const Assets::Model& model) const override;

        // メッシュ単位カリングの判定を、共通の描画ループとまったく同じカウンタへ数えながら行う。
        //
        // 【publicにしてある】自前ソフトウェアラスタライザは共通ループへ判定を任せられない
        // (三角形が3つ未満のメッシュを先に落とすため、任せると分母がずれる)。
        // それでも統計は共通ループと同じ2つのカウンタへ積む必要がある
        bool IsMeshVisibleCounted(
            const Rendering::FrustumPlanes& frustum, const Assets::ModelInstance& instance,
            const Assets::Model& model, const Assets::Mesh& mesh);

        // onModel: モデル単位で描き切ったなら真を返す(メッシュのループへ入らない)
        // onMesh : 偽を返すと列挙そのものを打ち切る
        //
        // 【publicにしてある】Passes/*の各群が共通ループを回すために取る。
        // 参照だけを束ねたものなので、**フレームより長く持たせないこと**
        Rendering::GeometryDrawHost MakeGeometryDrawHost()
        {
            return Rendering::GeometryDrawHost{
                m_Scene, m_DrawList, *this,
                { m_FrustumCullTested, m_FrustumCullCulled, m_MeshCullTested, m_MeshCullCulled },
                m_Settings.Geometry.MeshCullingEnabled };
        }

        // 旗が立っていれば上を呼んで下ろす。
        //
        // 【publicにしてある】呼ぶのはPasses::PresentPassだけだが、名前を焼く対象は
        // エンジン全体のテクスチャ表(BuildDumpableTextureTable)なので、この機能を
        // Present群へ降ろすことはできない。**旗の判定と下ろしをここへ閉じておく**と、
        // 群がエンジンのメンバ変数を触らずに済む
        void ApplyDebugNamesIfDirty();

        // 【publicにしてある】積む位置がPresentより前と決まっているためPasses::PresentPassが
        // 呼ぶ。書き出す対象はエンジン全体のテクスチャ表なので、群へは降ろせない
        void IssueTextureDumps(Core::RenderGraph& graph);

        // キューブマップの面数(D3D標準順: +X,-X,+Y,-Y,+Z,-Z)。IBLの2つのキューブマップは
        // いずれもこの順で面ごとにディスパッチする(IBLConvolve.hlsl CubeFaceDirectionと一致させる)
        // 出所は Rendering/CubeFaceMath.h(移行中の別名)
        static constexpr uint32_t kCubeFaceCount = ::Kurenai::kCubeFaceCount;

        // 【publicにしてある】シーン読み込みが構築し、Passes::DDGIPasses が
        // ラスタ経路で「このインスタンスは自発光プロキシか」を引くために読むだけ
        const std::vector<bool>& GetEmissiveProxyInstances() const { return m_EmissiveLights.ProxyInstances; }

        // キューブマップ配列の枚数上限。TextureCubeArrayは実行時に伸縮できないため固定容量で確保し、
        // これを超えるプローブが置かれたシーンは先頭からこの数だけを採用する(警告ログを出す)
        static constexpr uint32_t kMaxReflectionProbes = Passes::kMaxReflectionProbes;

        // 【publicにしてある】Passes::DDGIPasses がプローブの位置と担当座標を引くために呼ぶ。
        // どれも設定と格子から導くだけの計算で、状態を持たないので公開しても持ち主は変わらない
        GI::DDGIGrid& GetDDGIGrid() { return m_DDGIGrid; }

        // 【publicにしてある】上と同じ理由。1フレームに焼けるプローブ数を
        // ObjectConstantsのリング段数から決める判定で、群が登録時に呼ぶ。
        // 抑えないとDX12の1フレームあたりの上限を超えて起動直後に落ちる
        // (経緯は docs/ImplementationHistory.md 45.1)
        uint32_t ClampDDGIProbesPerFrameToConstantRing(uint32_t requested);

        // UIのつまみの上限。**シェーダー側の段数そのものではない。**
        // Sky.hlsli は既定 kCloudMaxRaymarchSteps(384)で走り、cbuffer で0より大きい値を
        // 渡されたときだけそれを使う。その値は kCloudRaymarchStepsHardMax(512)で丸められる。
        // したがってここに要る条件は「512を超えないこと」だけで、一致させる相手はいない
        static constexpr uint32_t kCloudRaymarchStepsMax = Kurenai::kCloudRaymarchStepsMax;

        // MegaLightsの候補プールが1タイルあたりに抽出する候補の数(K)。
        // ライトタイルの容量と違い**これは打ち切りではなく抽出数**で、タイルへ何灯届いていても
        // ここで決めた本数だけを重みつきで取り出す。届いた灯が欠落するわけではない
        // (どの灯も w_i / SumW の確率で選ばれる)ため、容量超過のような静かな欠落は起きない。
        // 【実行時に振れる。ここは確保の上限】1タイルの抽出数Kは
        // m_Settings.MegaLights.TilePoolCapacity が持ち、シェーダへは定数バッファで渡している。
        // バッファの確保だけがコンパイル時の上限を要るのでここに残す
        static constexpr uint32_t kMegaLightsTilePoolCapacity = Passes::kMegaLightsTilePoolCapacity;
        // Kの下限。これを下回るとタイルに届く灯を代表できない。
        static constexpr int32_t kMegaLightsTilePoolMinCapacity = Passes::kMegaLightsTilePoolMinCapacity;

        // 1画素あたりの標本数の上限。リザーババッファはこの倍数まで太る
        //(16バイト x 画素数 x 標本数。2560x1440・4本で236MB)ので、際限なく上げさせない。
        // クアッド層化は4層なので、4を超えると層の割り当てが一巡して効きが鈍る
        static constexpr int32_t kMegaLightsMaxSamplesPerPixel = Passes::kMegaLightsMaxSamplesPerPixel;

        // 【publicにしてある】シーン読み込みが構築し、Passes::MegaLightsPasses が
        // 三角形の数とバッファを引くために読むだけ
        const Assets::MeshLightScene& GetMeshLightScene() const { return m_EmissiveLights.MeshLightScene; }
        bool IsMeshLightsEnabled() const { return m_EmissiveLights.MeshLightsEnabled; }

        // RenderingPanel(モデルLOD段ごとの内訳表示)向け
        const std::vector<Scene::InstanceLODState>& GetInstanceLODStates() const override
        {
            return m_InstanceLODStates;
        }
    private:
        // 機能ごとの設定18個をまとめた入れ物。**Rendering::RenderSettingsSnapshot は
        // これの写し**で、実体を共有させてはいけない(理由は EngineSettings.h)
        Settings::EngineSettings m_Settings;

        // UpdateスレッドからRenderスレッドへ、1フレーム分のカメラ・ImGui表示状態を引き渡すための
        // スナップショット。m_Settings.Sky.TimeOfDay等それ以外の状態はRenderスレッド側のみが読み書きするため
        // ここには含めない(RenderThreadMain参照)
        struct FrameState
        {
            Core::Camera Camera;
            bool ImGuiVisible = true;
            // Updateスレッド側のフレーム番号。Renderスレッドの m_History.FrameIndex と
            // 一致するはず ―― という**推測**を、DecideFrameJitterAndCamera で実際に比べて潰す。
            // 一致しないと「経路のフレーム番号」と「乱数の種・ジッターのフレーム番号」が
            // ずれ、測定そのものが成立しない
            uint32_t PathFrameIndex = 0;
        };

        void CreateSceneResources();
        // CreateSceneResourcesの3段。
        // 【呼ぶ順序を入れ替えないこと】DX12はディスクリプタ枠を生成順に割り当てる。
        // 順序が変わるとシェーダーが読む枠と実際のリソースがずれ、絵が出てから原因を探すことになる
        void CreateScenePipelineStates(
            const std::wstring& shaderDirectory, const std::vector<RHI::InputElementDesc>& modelInputLayout);
        void CreateSceneBuffers(const std::wstring& dataRoot, const std::wstring& shaderDirectory);
        void CreateSceneGIResources(
            const std::wstring& shaderDirectory, const std::vector<RHI::InputElementDesc>& modelInputLayout);
        // 中間バッファの精度構成(m_Settings.System.Precision)によって変わるフォーマット。
        // レンダーターゲットの作成(CreateRenderTargets)と、そこへ描くPSOのRenderTargetFormats
        // 宣言の両方がこれを使う。両者がずれるとD3D12では仕様違反(デバッグレイヤーがID 613を出す)
        // になるため、値の出所をこの2関数に一本化している
        RHI::Format GetEmissiveFormat() const;
        RHI::Format GetAOFormat() const;
        // 上記のフォーマットに依存するPSOを作る(G-Buffer・SSAO・SSIL・AOブラー)。
        // 初回はCreateSceneResourcesの末尾から、以降はバッファ精度が切り替わるたびに
        // Render()から呼び直す。GPUがまだ参照しているPSOを壊さないよう、呼び出し側で
        // WaitForGPUIdleを済ませておくこと
        void CreatePrecisionDependentPipelineStates();
        // パス用途ごとのサンプラーセット(m_MaterialSamplers / m_ScreenSpaceSamplers)を作る。
        // セットの中身は作成後に書き換えないことが前提のAPIなので、描画を始める前に一度だけ呼ぶ
        // (理由はRHI/IRHISamplerSet.h)
        void CreateSamplerSets();
        void CreateRenderTargets(uint32_t width, uint32_t height);
        // 平面反射専用のレンダーターゲット2枚(m_RenderTargets.PlanarReflectionColor/Depth)を、
        // 反射解像度(レンダー解像度 × m_Settings.Reflection.PlanarResolutionScale)で作り直す。
        // メインのCreateRenderTargetsとは独立に呼べる(Legacy8bitフォールバックの対象外。
        // このバッファは常にHDR固定フォーマットのため)。呼び出し箇所はCreateRenderTargetsと
        // 同じ2か所(Initialize直後、Render()の解像度変更ハンドリング)
        void CreatePlanarReflectionTargets();
        // このフレームでRT反射パスを実行するか。手法がRaytracedでも、高速化構造が無ければ
        // (非対応環境・シーン読み込み中の空シーン・構築失敗)撃つ相手がいないため実行しない。
        // 「パスを追加する条件」と「後段がその出力を読む条件」がずれると、
        // 実行していないパスの出力(前フレームの残骸)を読むことになるため、判定はこの1か所に置く
        bool ShouldRunRaytracedReflection() const;
        // このフレームでRTシャドウパスを実行するか。ShouldRunRaytracedReflectionと同じ理由で
        // 判定を1か所に集約している(パスを追加する条件とDirectLightingがその出力を読む条件が
        // ずれると、実行していないパスの残骸を影として使ってしまう)
        bool ShouldRunRaytracedShadow() const;
        // このフレームでRTAOパスを実行するか。上2つと同じ理由で判定を1か所に集約している
        bool ShouldRunRaytracedAO() const;
        // DDGIのプローブ取得をレイトレースで行うか。
        // 「パスを走らせるか」と「その出力を読むか」を同じ1つの述語で判定するための関数
        // (ShouldRunRaytraced*と同じ作法)
        bool ShouldRunRaytracedDDGITrace() const;
        // このフレームでタイルライトカリングパスを実行するか。上と同じ作法で1か所に集約している。
        // **ライトグリッドを実際に読む者が居るときだけ積む** ―― 読み手は
        // DirectLighting.hlsl のローカルライトのループと Present.hlsl のライトグリッド表示
        // (Mode 11)の2つしかなく、MegaLightsが走るフレームは前者がLightCount.wで止まっている
        bool ShouldRunLightCulling() const;
        // いま1画素あたり何本の標本(リザーバ)を引くか。**バッファの確保も定数バッファも
        // 必ずこの関数を通すこと** ―― 2か所で別々に計算すると静かに食い違う。
        // 手法3以外は常に1(手法2の再利用が1画素1リザーバを前提にしているため)
        int32_t MegaLightsSamplesPerPixel() const;
        // このメッシュをメッシュシェーダー経路で描くか。上のShouldRun*と同じく、
        // 「どのPSOを束ねるか」と「DispatchMeshとDrawIndexedのどちらを積むか」の判断が
        // ずれると即座に破綻するため、判定を1か所に集約する。
        // isWaterがtrueのメッシュは常にfalse(理由は実装のコメント参照)
        bool ShouldUseMeshletPath(const Assets::Model& model, const Assets::Mesh& mesh, bool isWater) const;
        // このフレームでライティングパス等が読むべきAO/GIバッファ(ブラー後 / ブラー前の生値)。
        // AO無効時はm_AODisabledTexture、Raytracedを選んでいても実行できないフレームはSSAOのもの
        RHI::IRHITexture* GetActiveAOTexture() const;
        RHI::IRHITexture* GetActiveAORawTexture() const;
        // このフレームでHDRのシーン色として後段(自動露出・ブルーム・トーンマップ)が読むべき
        // テクスチャを返す。反射パスを実行したならその出力、していなければRenderTargets::SceneColor
        RHI::IRHITexture* GetActiveReflectionOutput() const;
        // このフレームで空として使うキューブマップを返す。手続き空が有効で、かつ.ksceneが
        // スカイボックスを明示していないときだけ手続き空を使う(明示しているシーンは
        // そのDDSでなければ意味を成さないため。White Furnace Testが該当する)。
        //
        // 【重要】Render()の冒頭で一度だけ呼んでローカル変数へ保持し、RenderGraphの
        // Reads宣言と実際のバインドの両方で同じポインタを使うこと。
        // 呼び出しごとに評価すると両者が食い違い、依存解決が壊れる
        RHI::IRHITexture* ActiveSkyTexture() const;
        // <DLLフォルダ>/Assets/Scenes/*.ksceneを列挙し、m_SceneFilePaths/m_SceneDisplayNamesを構築する。
        // 個々のファイルの[Scene]Name読み取りに失敗した場合はそのファイルを警告ログとともに
        // スキップする(1ファイルの不備でアプリ全体が起動できなくなるのを避けるため)
        void DiscoverScenes();

        // --- シーン読み込みのスレッド分担 -----------------------------------------------------
        //
        // シーンの読み込みは「重いファイルI/O・デコード・GPUリソース作成」と「一瞬で終わる
        // エンジン状態への反映」に分かれる。両方をUpdateスレッドで行いRender()全体と
        // ミューテックスで排他すると、読み込みの間フレームが1枚も進まない(Bistro Exteriorで約1.3秒)。
        //
        // そのため前者を専用のLoaderスレッドへ、後者をRenderスレッドのフレーム境界へ分ける。
        // 読み込み中もフレームが進み続け、排他は受け渡しの一瞬だけで済む
        // (詳細はdocs/Architecture.html 23章)。

        // Renderスレッドが不要になったアセット由来のGPUリソースをまとめてLoaderスレッドへ渡すための箱。
        //
        // 【なぜRenderスレッドで破棄しないのか】アセット由来のリソースのディスクリプタは
        // アセット用のディスクリプタヒープから確保されており、そのヒープはロックを持たない
        // (DX12Device::GetAssetSrvCpuHeap参照)。確保するのがLoaderスレッドなので、
        // 解放も同じスレッドに寄せることでロックなしのまま安全にする
        struct RetiredAssets
        {
            Assets::Scene Scene;
            Assets::RaytracingScene RaytracingScene;
            // メッシュライトの三角形テーブル(段階2)。RaytracingSceneと同じ扱い
            Assets::MeshLightScene MeshLightScene;
            std::unique_ptr<RHI::IRHITexture> SkyboxTexture;
            // 水面法線マップ版。SkyboxTextureとまったく同じ扱い
            std::unique_ptr<RHI::IRHITexture> WaterNormalMapTexture;
        };

        // Loaderスレッドが作り、Renderスレッドが受け取る「差し替えられる状態まで仕上がったシーン」
        struct LoadedScene
        {
            Assets::Scene Scene;
            Assets::RaytracingScene RaytracingScene;
            Assets::MeshLightScene MeshLightScene;
            size_t SceneIndex = 0;
            // シーンの[Scene]Skyboxが読み込み済みのものと異なる場合のみ非nullptr。
            // nullptrなら現在のスカイボックスを維持する
            std::unique_ptr<RHI::IRHITexture> SkyboxTexture;
            std::wstring SkyboxPath;
            // シーンの[Water]NormalMapが読み込み済みのものと異なる場合のみ非nullptr。
            // nullptrなら現在の水面法線マップ(またはフラット法線フォールバック)を維持する
            std::unique_ptr<RHI::IRHITexture> WaterNormalMapTexture;
            std::wstring WaterNormalMapPath;
            // ComputeInitialCameraの結果(Updateスレッドが所有するm_Cameraへ後で反映される)
            Core::Camera Camera;
        };

        // 内部レンダー解像度の変更を要求する(SystemPanel = Renderスレッドから呼ばれる)。
        // レンダーターゲットの作り直しはGPUがそれらを参照していない状態で行う必要があるため、
        // ここでは要求を記録するだけにしてRender()の先頭でまとめて反映する
        void RequestRenderResolution(uint32_t width, uint32_t height);
        // Renderスレッドがフレーム先頭で呼ぶ。保留中の切り替え要求の発注と、
        // 出来上がったシーンの取り込みを行う
        void UpdateSceneStreaming();
        // .ksceneの更新時刻を見て、変わっていれば再読み込みを要求する。
        // UpdateSceneStreamingの先頭から呼ぶ。m_Settings.System.SceneAutoReloadEnabledがfalseなら何もしない
        void UpdateSceneHotReloadWatch();
        // 現在のシーンの.ksceneの最終更新時刻。取得できなければ0を返す
        // (ファイルが一時的に開けない、削除された等。0のときは何もしないのが正しい振る舞い)
        uint64_t GetCurrentSceneFileWriteTime() const;
        // Loaderスレッドの本体。要求を待ち、旧シーンを破棄し、新シーンを読み込んで publish する
        void LoaderThreadMain();
        // Loaderスレッドで実行する読み込み本体。エンジンの状態は一切書き換えない。
        // 失敗した場合はログを出してnullptrを返す
        std::unique_ptr<LoadedScene> LoadSceneOnLoaderThread(size_t sceneIndex);
        // Renderスレッドで実行する反映。出来上がったシーンを現在のシーンと差し替え、
        // シーン由来の設定(太陽・影・AO・SSR・ライト・反射プローブ・ベイクフラグ等)を適用する
        void ApplyLoadedScene(LoadedScene& loaded);
        // 前のシーンに紐づいていた状態を捨てる。ApplyLoadedSceneの最初に呼ぶ
        void ResetSceneBoundState(LoadedScene& loaded, bool& outIsSameSceneReload);
        // .ksceneが持つ設定をエンジンの設定へ反映する。
        // 【「キーを書いたシーンだけ上書きする」ものと、常に反映するものがある】
        void ApplySceneSettingsFromScene();
        // 不要になったアセット由来のリソースをLoaderスレッドへ破棄依頼として積む。
        // 【重要】呼ぶ前にIRHIDevice::WaitForGPUIdle()でGPUの参照が終わっていることを保証すること
        void RetireAssets(RetiredAssets&& retired);
        // シーンのAABBから初期カメラ(位置・向き・near/far)を決める。エンジンの状態を読まない
        // 純粋な計算なのでLoaderスレッドから呼べる([Camera]セクションがあればそれを優先する)
        static Core::Camera ComputeInitialCamera(const Assets::Scene& scene);

        // imguiWantsMouseはImGuiがマウス入力を掴んでいるか(Renderスレッドから
        // m_ImGuiWantCaptureMouse経由で受け取る)。パネルの上で右ドラッグを始めても
        // 視点回転が始まらないようにするために使う
        void UpdateMouseLook(bool imguiWantsMouse);
        void UpdateMovement(float deltaTime);
        void UpdateImGuiToggle();
        // 決定的カメラ経路。**UpdateAppliedSceneHandoffより後に呼ぶこと** ――
        // 先に呼ぶと、シーンが切り替わったフレームだけ経路が.ksceneの[Camera]に上書きされる
        void UpdateCameraPath();
        // -camerapath で指定された名前を、いま適用されているシーンの[CameraPath]から解決する。
        // シーンの適用とオプションの指定はどちらが先でも起きうるので、両方の契機から呼ぶ
        void ResolveCameraPath();
        // 経路の開始フレーム。負が入っていれば既定(kMegaLightsAccumWarmup)へ落とす
        uint32_t GetCameraPathStartFrame() const;
        // 検算結果をログへ1本ぶん書き出す
        static void LogCameraPathMotionStats(
            const Assets::CameraPath& path, const Assets::CameraPathMotionStats& stats);
        // ApplyLoadedScene(Renderスレッド)が公開した初期カメラ・ウィンドウタイトルを、
        // まだ適用していなければ適用する。m_Cameraの書き込み手をUpdateスレッド1つに保ち、
        // ウィンドウタイトルの変更もウィンドウを所有するこのスレッドから行うためのハンドオフ
        void UpdateAppliedSceneHandoff();
        void Update(float deltaTime);
        // 1フレーム分のUpdateと、Renderスレッドへのフレーム状態の受け渡しを行う。
        // 通常はRun()のループから、ウィンドウのドラッグ中(Windowsのモーダルループ中で
        // PumpMessagesが戻ってこない間)はWindowのタイマーから呼ばれる
        void TickFrame();
        void RenderThreadMain();
        void Render(const FrameState& frameState);
        // --- Render()から切り出したフレームの先頭(段階6.6のAブロック) ---
        // 【定義はRendering/RenderFrame.cppにある】下のEブロックと同じ作法。
        // どれも「このフレームのGPUコマンドをまだ1つも積んでいない」ことを前提にしており、
        // **Render()の先頭から呼ぶ位置を動かさないこと**
        //
        // フレーム単位の統計を前フレームぶんへ控えてから0に戻す
        void ResetFrameCounters();
        // ウィンドウのリサイズ・予約された作り直し・シーンの切り替え・常駐ミップの差し替えを
        // 確定させる。どれもGPUコマンドを積む前でなければならない
        void ApplyPendingRecreations();
        // ImGuiのフレームを開始し、パネルを描いて、入力を掴んでいるかをUpdateスレッドへ返す
        void BeginImGuiFrame(const FrameState& frameState);
        // バッファ精度・内部レンダー解像度・平面反射解像度・超解像の出力を、
        // 要求が立っていれば作り直す。確保に失敗したら元の解像度へ戻す縮退を持つ
        void RecreateDirtyRenderTargets();
        // そのフレームのキー照度[lx]から実効プリ露出(m_EffectiveExposureEV100)を決める
        void UpdateEffectiveExposure(float keyIlluminanceLux);
        // 【検証専用】蓄積が始まる瞬間に1回だけ摂動を加える(-perturbmode)
        void ApplyMegaLightsPerturbationIfDue();
        // モデルLOD・モデルのストリーミング・インスタンスのバッチ・レイトレーシングの
        // 作り直し・テクスチャの常駐目標を、レンダーグラフを組む前にこの1回だけ進める
        void UpdateSceneForFrame(
            RHI::IRHICommandList* commandList, const DirectX::XMFLOAT3& cameraPosition, const Core::Camera& camera);

        // --- Render()から切り出したフレームの値の組み立て(段階6.6のB+Cブロック) ---
        // ジッター・ライト配列・空のパラメータ・FrameConstants・LightingConstants を
        // 組み立て、frameContext のフィールドを埋める(ビューポート・BakedLightCount・
        // ProbeFaceProjection・ProbeCaptureReads・CascadeViewProj の5つだけは
        // RegisterPasses が登録の合間に埋める)。
        //
        // 【定義だけKurenaiEngine3D.cppに残してある】A/D/EブロックはRendering/RenderFrame.cppへ
        // 移したが、この本体はあちらの無名名前空間のkMaxLights・MakeGPULight・
        // GPUReflectionProbe等に依存しており、それらを外へ出すのは別の関心事になる。
        //
        // 【後半4つを出力引数で受ける理由】frameContext.Lights / Lighting / Constants が
        // これらを指す。この関数のローカルにすると graph.Execute() の時点で解放済みになるが、
        // 解放直後なら中身が残っていて同じ絵が出るため、採取では絶対に捕まらない。
        // 寿命を Render() のスコープに保つため、実体は呼び出し側に置く
        // --- BuildFrameContext から切り出した組み立ての各段(段階7.5) ---
        // 【定義は Rendering/RenderFrameBuild.cpp にある】呼ぶ順が実行順の一部で、
        // 後段が前段の書いた frameContext のフィールドを読む。並べ替えないこと
        void DecideFrameJitterAndCamera(
            const FrameState& frameState, Rendering::RenderFrameContext& frameContext);
        void UpdateMeshletLODFrame(const FrameState& frameState);
        void EvaluateDroneShowFrame();
        void UploadDroneInstances(RHI::IRHICommandList* commandList);
        void BuildGpuLightList(
            std::vector<GPULight>& gpuLights, const DirectX::XMFLOAT3& cameraPosition,
            size_t& bakedLightCount);
        void AppendEmissiveProxyLights(
            std::vector<GPULight>& gpuLights, const DirectX::XMFLOAT3& cameraPosition,
            size_t manualLightCount, size_t& bakedLightCount);
        void AppendDroneLights(
            std::vector<GPULight>& gpuLights, const DirectX::XMFLOAT3& cameraPosition);
        void ClampLightsToCapacity(
            std::vector<GPULight>& gpuLights, const DirectX::XMFLOAT3& cameraPosition,
            size_t& bakedLightCount);
        void ResolveSkyFrameState(
            const SunLighting& sunLighting, Rendering::RenderFrameContext& frameContext);
        void ResolveFrameDrawDecisions(Rendering::RenderFrameContext& frameContext);
        void FillFrameConstants(
            const FrameState& frameState, RHI::IRHICommandList* commandList,
            const SunLighting& sunLighting, float effectiveExposure, float manualExposureScale,
            float keyReferenceEV100, const DirectX::XMFLOAT3& cameraPosition,
            const float (&cascadeSplits)[kCascadeCount],
            const DirectX::XMMATRIX (&cascadeViewProj)[kCascadeCount],
            Rendering::RenderFrameContext& frameContext, std::vector<GPULight>& gpuLights,
            ShaderInterop::FrameConstants& constants, Passes::LightingConstants& lightingConstants,
            size_t& bakedLightCount);
        // FillFrameConstantsの後半。反射プローブ・DDGI・空と雲まわりを埋める。
        // 行列と露出が確定した後に呼ぶこと(前半が決めた値を読む)
        void FillEnvironmentFrameConstants(
            RHI::IRHICommandList* commandList, const SunLighting& sunLighting, float effectiveExposure,
            Rendering::RenderFrameContext& frameContext, ShaderInterop::FrameConstants& constants);
        void ResolveOcclusionCullingFrameState(
            RHI::IRHICommandList* commandList, const DirectX::XMFLOAT3& cameraPosition,
            Rendering::RenderFrameContext& frameContext, std::vector<GPULight>& gpuLights,
            ShaderInterop::FrameConstants& constants, Passes::LightingConstants& lightingConstants);
        void FillFrameContextSnapshot(
            const SunLighting& sunLighting, float effectiveExposure, float manualExposureScale,
            float keyReferenceEV100, const DirectX::XMFLOAT3& cameraPosition,
            Rendering::RenderFrameContext& frameContext, std::vector<GPULight>& gpuLights,
            ShaderInterop::FrameConstants& constants, Passes::LightingConstants& lightingConstants);

        void BuildFrameContext(
            const FrameState& frameState, RHI::IRHICommandList* commandList,
            const SunLighting& sunLighting, float effectiveExposure, float manualExposureScale,
            float keyReferenceEV100, const DirectX::XMFLOAT3& cameraPosition,
            const float (&cascadeSplits)[kCascadeCount],
            const DirectX::XMMATRIX (&cascadeViewProj)[kCascadeCount],
            Rendering::RenderFrameContext& frameContext, std::vector<GPULight>& gpuLights,
            ShaderInterop::FrameConstants& constants, Passes::LightingConstants& lightingConstants,
            size_t& bakedLightCount);

        // --- Render()から切り出したパスの登録(段階6.6のDブロック) ---
        // 【定義はRendering/RenderFrame.cppにある】13回のRegisterを1つにまとめたもの。
        // **中の呼び出し順は実行順の一部**で、RenderGraphは依存が同点のとき最小登録番号を
        // 選ぶ。1つでも入れ替えると実行順が変わる。
        //
        // 【probeCaptureReadsを引数で受ける理由】frameContext.ProbeCaptureReadsがこれを指し、
        // graph.Execute()の時点でも生きている必要がある。この関数のローカルにすると
        // 解放済みのメモリを指すが、直後なら中身が残っていて同じ絵が出るため採取では捕まらない
        void RegisterPasses(
            Core::RenderGraph& graph, Rendering::RenderFrameContext& frameContext,
            Rendering::RenderBlackboard& blackboard, RHI::IRHICommandList* commandList,
            std::vector<RHI::IRHITexture*>& probeCaptureReads, size_t bakedLightCount,
            RHI::IRHITexture* skyTexture, const DirectX::XMMATRIX (&cascadeViewProj)[kCascadeCount],
            const Core::Camera& camera);

        // --- Render()から切り出したフレームの締め(段階6.6のEブロック) ---
        // 【定義はRendering/RenderFrame.cppにある】KurenaiEngine3Dのメンバ関数のまま、
        // 翻訳単位だけを分けている(Diagnostics/RenderDumpService.cppと同じ作法)。
        // **Render()から呼ぶ順序が実行順の一部**なので、呼ぶ位置を動かさないこと
        //
        // GPUカリングの結果(メッシュレット統計とモデル単位)を読み戻す。
        // graph.Execute()の直後、Presentより前で呼ぶ
        void ResolveFrameCullStats(const Rendering::RenderBlackboard& blackboard, bool meshletCullStatsActive);
        // ImGuiの描画をバックバッファへ重ね、GPU計測を締めてPresentする
        void SubmitAndPresentFrame();
        // 次フレームが「前フレーム」として参照する行列・ジッター・カメラ位置を確定させる
        void AdvanceFramePrevViewState(const ShaderInterop::FrameConstants& constants, const DirectX::XMFLOAT2& jitterUv);
        // 履歴テクスチャのping-pongを反転する(TAA / MegaLightsの時間再利用・デノイザ)。
        // 【ResolveTextureDumps()より後で呼ぶこと】ダンプは今フレームの書き込み先を読む
        void AdvanceFrameHistory();
        // このフレームの計測値を集計し、集計期間(FrameStatsLogIntervalSeconds)ぶん溜まっていれば
        // 1行にまとめてログへ出す。Renderスレッドからフレームごとに呼ぶ
        void LogFrameStatsIfDue(float renderDeltaTime);
        // LogFrameStatsIfDueが出す内訳。いずれも集計期間ぶんをまとめて1回だけ出す。
        // 【m_FrameStats.Reset()より前に呼ぶこと】積算値を読むのはこれらの中
        void LogFrameTimingStats(float elapsedSeconds);
        void LogCpuSideStats();
        void LogGpuSideCullStats();
        // 常駐しているテクスチャとVRAMの使用量。積算値を使わないのでResetの後でよい
        void LogResidencyStats();
        // カメラ視錐台をkCascadeCount個の深度範囲に分割する(near/far境界、View空間での距離)。
        // 対数分割と均等分割を混合した実用的な分割(Practical Split Scheme)を使う
        void ComputeCascadeSplits(const Core::Camera& camera, float (&outSplits)[kCascadeCount]) const;
        // カメラ視錐台のうち[splitNear, splitFar]の範囲(View空間距離)だけを覆う、平行光のライト視点
        // 正射影ビュー・プロジェクション行列を求める。カスケードごとに1回呼ぶ
        DirectX::XMMATRIX ComputeCascadeLightViewProj(
            const DirectX::XMFLOAT3& lightDirection, const Core::Camera& camera, float splitNear, float splitFar) const;

        // 起動時に選択されたグラフィックスAPI(タイトルバー・ImGui表示用に保持)
        GraphicsAPI m_GraphicsAPI;

        // 「システム」パネルから要求された切り替え先のAPI。-1なら要求なし。
        // UI(Renderスレッド)が書き、Run()のループ条件(Updateスレッド)が読むためatomic。
        // 実際の作り直しはRun()から戻った後に呼び出し側が行う(上のHasPendingGraphicsAPIChange参照)
        std::atomic<int> m_RequestedGraphicsAPI{ -1 };

        // 起動時に読み込むシーンの番号。コンストラクタ引数をそのまま保持する
        // (APIを切り替えても同じシーンで再開できるようにするため)
        size_t m_InitialSceneIndex = 0;

        // ImGuiのIniFilenameはポインタを保持するだけでコピーしないため、文字列の寿命をここで維持する。
        // m_ImGuiBackendのデストラクタ(ImGui::DestroyContextで最終保存)より後に破棄されるよう、
        // メンバ破棄順(宣言の逆順)に従いm_ImGuiBackendより前で宣言する
        std::string m_ImGuiIniPath;

        // KurenaiEngineBaseが破棄される(m_Deviceが破棄される)前にImGuiのバックエンドを
        // 終了させる必要があるが、基底クラスのメンバは派生クラスのメンバより後に破棄されるため
        // (C++の破棄順の規則上)、この宣言順のままで安全に成立する
        std::unique_ptr<RHI::IRHIImGuiBackend> m_ImGuiBackend;

        // UIパネル群の所有者。ImGuiコンテキストが生きている間だけ有効であればよいため、
        // m_ImGuiBackendより後に宣言してメンバ破棄順(宣言の逆順)で先に破棄させる。
        // UI::UIManagerは不完全型のままにするため、デストラクタは.cpp側で定義する
        std::unique_ptr<UI::UIManager> m_UIManager;

        // Render()から切り出したパス群(段階6)。
        //
        // 【エンジンへの参照を持たせている】段階6は「登録順を1つも変えない」ことだけを
        // 決め手に進めており、その担保はパスマニフェストの完全一致である。状態の引っ越しと
        // 登録位置の移動を同時にやると、食い違ったときにどちらが原因か分けられない。
        // まず登録コードだけを機械的に移し、リソースの所有権は後から群へ移した。
        // 【所有権の引っ越しは完了している】群はエンジンのprivateを触らず、
        // 必要なものは公開のアクセサ(GetScene / ForEachGeometryDraw など)から取る。
        // 不完全型のままにするため、デストラクタは.cpp側で定義する
        std::unique_ptr<Passes::DDGIPasses> m_DDGIPasses;
        std::unique_ptr<Passes::EnvironmentPasses> m_EnvironmentPasses;
        std::unique_ptr<Passes::GeometryPasses> m_GeometryPasses;
        std::unique_ptr<Passes::LightingPasses> m_LightingPasses;
        std::unique_ptr<Passes::MegaLightsPasses> m_MegaLightsPasses;
        std::unique_ptr<Passes::PostProcessPasses> m_PostProcessPasses;
        std::unique_ptr<Passes::ReflectionPasses> m_ReflectionPasses;
        std::unique_ptr<Passes::ReflectionProbePasses> m_ReflectionProbePasses;
        std::unique_ptr<Passes::ShadowPasses> m_ShadowPasses;
        std::unique_ptr<Passes::PresentPass> m_PresentPass;

        // SetExtraImGuiCallbackで登録された追加のImGui描画(Tools/KurenaiShowEditor)。
        // Renderスレッドだけが読み書きする
        std::function<void()> m_ExtraImGuiCallback;

        // ImGuiが入力を掴んでいるかを、RenderスレッドからUpdateスレッドへ返す逆方向のハンドオフ。
        // FrameState(Update→Render)の逆向きだが、渡す値がboolを2つだけなのでロックを増やす
        // 価値がなく、atomicで足りる。Updateスレッドはこれを見てWASD移動と視点回転の開始を抑止する
        std::atomic<bool> m_ImGuiWantCaptureKeyboard{ false };
        std::atomic<bool> m_ImGuiWantCaptureMouse{ false };

        // GPUタイムスタンプクエリによる各パスの計測(Shadow/GBufferなど)。数フレーム遅れの結果が返る
        std::unique_ptr<RHI::IRHIGPUProfiler> m_GPUProfiler;
        // 各パスのコマンド記録にかかるCPU時間の計測(RHIに依存しないためDX11/DX12を直接比較できる)
        Core::CPUProfiler m_CPUProfiler;

        // G-Bufferの内部解像度。ウィンドウサイズとは独立しており、表示時はアスペクト比を保って拡大縮小する。
        // Render()の各所(Dispatchのスレッド数・定数バッファのScreenParams・TAAジッター・
        // Hi-Zのミップ・レターボックス)から読まれるため、フレームの途中で変えてはならない。
        // 変更はm_RenderResolutionDirty経由でフレーム先頭にまとめて反映する
        uint32_t m_RenderWidth;
        uint32_t m_RenderHeight;
        // 「システム」パネルから要求された新しい内部解像度。Render()の先頭で反映する。
        // m_BufferPrecisionDirtyとまったく同じ扱い(レンダーターゲットの作り直しはGPUがそれらを
        // 参照していない状態で行う必要があるため、UI関数の中では実行しない)
        uint32_t m_PendingRenderWidth = 0;
        uint32_t m_PendingRenderHeight = 0;
        bool m_RenderResolutionDirty = false;
        // 内部解像度から決まるカメラのアスペクト比。
        // m_CameraはUpdateスレッドしか書けない(Render()はFrameStateのスナップショット経由でしか
        // 読まない)ため、解像度を変えるRenderスレッドからはここへ置くだけにし、
        // Updateスレッドが毎フレーム読み取ってm_Camera.SetAspectRatio()を呼ぶ
        std::atomic<float> m_RenderAspect{ 1.0f };

        // 出力解像度用テクスチャの作り直し要求。m_RenderResolutionDirtyとまったく同じ扱いで、
        // Render()の先頭のWaitForGPUIdle()を挟んだ位置で処理する
        bool m_UpscaleTargetsDirty = false;

        // 品質モードの倍率(1.3 / 1.5 / 1.7 / 2.0)
        static float GetUpscaleRatio(UpscaleQualityMode mode);
        // 出力解像度と品質モードから内部レンダー解像度を求める。
        // 8の倍数へ切り捨てるのは、LightCullのタイル・Hi-Zのミップ連鎖・Bloomのピラミッド・
        // SkyCloud/DDGIResolveの1/2解像度がいずれも2の冪で割っていくため。下限は320x180
        static void ComputeUpscaleRenderResolution(
            uint32_t outputWidth, uint32_t outputHeight, UpscaleQualityMode mode,
            uint32_t& outRenderWidth, uint32_t& outRenderHeight);

        // 出力解像度のテクスチャを作り直す。GPUがそれらを参照していない状態で呼ぶこと
        void CreateUpscaleTargets(uint32_t width, uint32_t height);
        // このフレームで超解像パスを走らせるか(有効かつテクスチャが確保済み)
        bool IsUpscaleActive() const;

        // ImGuiでBufferPrecisionが変更されたことをRender()へ伝えるフラグ。レンダーターゲットの
        // 作り直しはGPUがそれらを参照していない状態で行う必要があるため、UI関数の中では実行せず
        // Render()の先頭(RenderGraphの構築より前)でm_Device->WaitForGPUIdle()を挟んで処理する
        bool m_BufferPrecisionDirty = false;

        // --- インスタンシング(Stage 7) ------------------------------------------------------
        //
        // 同じ .kmodel を指すインスタンスを1回の DrawIndexed(..., instanceCount) へまとめる。
        // インスタンスごとに違う World/NormalMatrix/TangentSignFlip は定数バッファでは渡せないので、
        // 頂点シェーダー専用SRV(t0)の StructuredBuffer を SV_InstanceID で引く
        // (Shaders/3D/ObjectConstants.hlsli の FetchModelInstance)。
        //
        // 【効くシーンは限られる】PLATEAU・Sponza・Bistro は全モデルがユニークなので
        // バッチが1つも作られない。効くのは同じモデルを並べたシーン(InstancingTest /
        // MultiModelTest)と、今後の繰り返し配置(植生・街灯)。
        //
        // 【メッシュシェーダー経路には効かない】DispatchMesh にインスタンス数の概念が無いため、
        // ShouldUseModelMeshletPath が真になるモデルはバッチに入れない。
        // つまり DX12 でメッシュレット描画が有効なあいだ、この機能が働くのは
        // 水面・メッシュレットを持たないモデル・メッシュレット描画を切ったときに限られる
        // このフレームの描画リスト(バッチ・アップロード用レコード・作業領域)。
        // **持ち主を1つにする理由は Rendering/SceneDrawList.h**
        Rendering::SceneDrawList m_DrawList;
        using InstanceBatch = Rendering::InstanceBatch;
        // バッチを組み直す(レンダーグラフの構築より前に1フレーム1回。UpdateModelLODの後)
        void BuildInstanceBatches(RHI::IRHICommandList* commandList);
        // 1つの組(段の選び方)ぶんのバッチを作る。
        // modelOf: そのインスタンスがこの組で描く段を返す。nullptrならこの組の対象外。
        // groups: 呼び出しをまたいで使い回す作業領域(確保をやり直さないため引数で受ける)
        void BuildInstanceBatchesFor(
            const std::function<const Assets::Model*(size_t)>& modelOf,
            std::vector<std::pair<Rendering::InstanceGroupKey, std::vector<size_t>>>& groups,
            std::vector<Rendering::InstanceBatch>& outBatches, std::vector<uint8_t>& outBatched);


        // 起動時に決まる能力値(メッシュシェーダー・レイトレーシング等)。詳細は
        // Diagnostics/RenderCapabilities.h
        RenderCapabilities m_RenderCapabilities;
        // フレームごとの統計値(ImGuiのプロファイラパネル・性能ログ表示用)。詳細は
        // Diagnostics/RenderStats.h
        RenderStats m_RenderStats;
        // 毎フレーム主カメラから作り直し、全パスの定数バッファへ同じものを配る
        MeshletLODFrameConstants m_MeshletLODFrame;
        // カウンタをCPUへ持ってくる受け皿(メッシュレット統計とモデルカリングの両方)。
        // リングの段数と添字の扱いは Diagnostics/CullStatsReadback.h にある
        Diagnostics::CullStatsReadback m_CullStats;

        // --- モデル単位のGPUカリング(Stage 5-3) ---
        //
        // コンピュートシェーダー(ModelCull.hlsl)が、描画候補のワールドAABBを
        // 視錐台とHi-Zで判定し、生き残ったものだけの ExecuteIndirect 引数を詰める。
        // 深度プリパスとG-Bufferは、その引数でまとめて描く。
        //
        // 【区画の分け方と、その理由】Passes/GeometryConstants.h が持つ。
        // ここに並ぶ7本は移行中の別名で、あちらの値を引くだけ。
        // 出所は Passes/GeometryConstants.h(移行中の別名)
        static_assert(
            Passes::kModelCullArgsBaseOffset >= sizeof(uint32_t) * Passes::kModelCullRegionCount,
            "区画ごとの発行数が引数配列の領域へはみ出している");

        // G-Bufferは複数のパス群が共有するため、特定のパス群ではなく唯一の所有者へ集める。
        Rendering::RenderTargets m_RenderTargets;
        // 間接光(DDGI・反射プローブ)のリソースの持ち主は Rendering/GIResources.h
        Rendering::GIResources m_GIResources;

        std::unique_ptr<RHI::IRHITexture> m_AODisabledTexture; // AO無効時に使う、遮蔽なし・間接光なしのテクスチャ


        // ドローンショーの一式(資源・機体データ・設定)。
        // 生成位置を動かせない理由は Rendering/DroneShowSystem.h
        Rendering::DroneShowSystem m_Drones;

        // UIの「既定値に戻す」(右クリック)が戻る先。シーン読み込み時に決まった手法を控えておく。
        // 【静的なDefaultReflectionModeを使ってはいけない】.ksceneが指定を持つ場合、
        // 戻る先はエンジンの既定ではなく**そのシーンを読み込んだ直後の状態**である。
        // ここを取り違えると「既定へ戻したらシーンが要求した反射が消える」ことになる
        ReflectionMode m_SceneDefaultReflectionMode = ReflectionSettings::DefaultReflectionMode(false);

        // 前フレームの実効プリ露出EV100。
        // 【補正には使っていない】リザーバのWは露出に対して不変(比なので約分される)と
        // 実測で確かめた ―― TAAのm_History.PrevEffectiveExposureEV100と違い、掛ける係数は1。
        // 詳細はKurenaiEngine3D.cppの「プリ露出の補正は入れない」。
        // 値は、将来この前提を疑うときに差を見られるよう記録だけ続けている
        float m_MegaLightsPrevEffectiveExposureEV100 = 0.0f;
        // 摂動を適用済みか(蓄積開始の1回だけ効かせる)
        bool m_MegaLightsPerturbApplied = false;

        // いまリザーババッファを確保したときの標本数。**定数バッファへ渡す値と必ず一致させる**。
        // 食い違うと Initial が確保外へ書くか Resolve が別画素の標本を読み、
        // 例外もログも出ないまま絵だけが壊れる
        int32_t m_MegaLightsAllocatedSamplesPerPixel = 1;
        // 標本数が変わったのでリザーババッファを作り直す必要がある。
        // 解像度変更と同じくフレームの先頭(GPUアイドル後)でまとめて処理する
        bool m_MegaLightsReservoirDirty = false;

        // --- GPU計測の書き出し(計測専用) ---
        std::wstring m_PerfDumpPath;
        int32_t m_PerfDumpTargetFrames = 0;
        int32_t m_PerfDumpWarmupFrames = 0;
        int32_t m_PerfDumpCollected = 0;
        bool m_PerfDumpDone = false;
        // パス名 -> 合計時間[ms]。同じ名前のパスが1フレームに複数あるぶんも足し込む
        // (a-trousは段の数だけ同名で登録される。**合計が知りたいので足すのが正しい**)
        std::map<std::string, double> m_PerfDumpTotals;
        // このフレームのパス別GPU時間を上の合計へ足し込み、目標枚数に達したらCSVへ書き出す。
        // 【定義はDiagnostics/RenderDumpService.cppにある】LogFrameStatsIfDueと同じ翻訳単位
        void AccumulatePerfDump();

        // --- 中間レンダーターゲットの生値ダンプ(検証専用。AddTextureDump) ---
        // 名前 -> テクスチャ の対応表。CreateRenderTargetsでテクスチャを増やしたら
        // BuildDumpableTextureTableにも足すこと(表の実体はそちらのコメントを参照)
        using DumpableTexture = Diagnostics::DumpableTexture;
        // 【毎回作り直す】レンダーターゲットはリサイズやバッファ精度の切り替えで
        // ポインタごと作り直される。キャッシュすると解放済みのテクスチャを指す
        std::vector<DumpableTexture> BuildDumpableTextureTable() const;

        // ダンプとパスマニフェストの状態と処理。**表は毎回作り直して渡すこと**
        // (理由は Diagnostics/RenderDumpServiceState.h)
        Diagnostics::RenderDumpService m_DumpService;
        // 作り直し経路の予約。下の IRecreationTarget の4本を通してエンジンを呼び返す
        Diagnostics::ScheduledRecreationQueue m_Recreations;

        // --- Diagnostics::IRecreationTarget ---
        void RequestUpscaleSettings(bool enabled, uint32_t outputWidth, uint32_t outputHeight) override
        {
            RequestUpscaleSettings(enabled, m_Settings.PostProcess.UpscaleQuality, outputWidth, outputHeight);
        }
        void RequestBufferPrecision(BufferPrecision precision) override
        {
            m_Settings.System.Precision = precision;
            m_BufferPrecisionDirty = true;
        }
        const std::vector<std::wstring>& GetSceneFilePaths() const override { return m_SceneFilePaths; }

        // 読み戻しとファイル書き出し。graph.Execute()の後にRender()から呼ぶ
        void ResolveTextureDumps();
        // このフレームのパスマニフェスト(RenderGraphの実行順)を、指定のフレームに達していれば
        // ファイルへ書き出す(検証専用の -passmanifest)。graph.Execute()の直前に呼ぶこと
        void WritePassManifestIfDue(Core::RenderGraph& graph);


        // フレームをまたいで持ち越す値(TAAの履歴と前フレームのカメラ由来の値)。
        // 中身と、有効性を別管理にしている理由は Rendering/FrameHistoryState.h
        Rendering::FrameHistoryState m_History;


        // ピラミッドの段数。半解像度を第0段として、これ以上小さくしても見た目が変わらない範囲で選ぶ
        static constexpr uint32_t kBloomLevelCount = 6;


        // 背景(深度が書き込まれなかったピクセル)に表示する空のキューブマップ。
        // .ksceneの[Scene]Skyboxでシーンごとに差し替えられる(LoadScene参照)
        std::unique_ptr<RHI::IRHITexture> m_SkyboxTexture;
        // 既定のスカイボックス(Assets/Skybox/Sky.dds)の絶対パス。[Scene]Skybox指定が無いシーンへ
        // 切り替えたときはここへ戻す
        std::wstring m_DefaultSkyboxPath;
        // 現在m_SkyboxTextureへ読み込んでいるファイルの絶対パス。シーン切り替えのたびに
        // 読み直さずに済むよう比較に使う
        std::wstring m_CurrentSkyboxPath;

        // 水面法線マップ(水面マテリアル基盤)。m_SkyboxTextureとまったく同じ方針で、
        // .ksceneの[Water]NormalMapで差し替えられる。空文字列のシーン(NormalMap未指定)では
        // 1x1のフラット法線(128,128,255,255、CreateSolidColorTexture)へフォールバックする
        std::unique_ptr<RHI::IRHITexture> m_WaterNormalMapTexture;
        // 現在m_WaterNormalMapTextureへ読み込んでいる絶対パス。空文字列ならフラット法線
        // フォールバックを使用中であることを表す(m_CurrentSkyboxPathと同じ比較用途)
        std::wstring m_CurrentWaterNormalMapPath;

        // 手続き空を焼き直す必要があるか。太陽が動いたとき等に立てる
        bool m_SkyBakeDirty = true;
        // 最後に焼いたときの太陽の向き。これと現在の向きの角度差が閾値を超えたら焼き直す。
        // 毎フレーム焼くと空生成6回+プリフィルタ36回が常時走って無駄なため
        DirectX::XMFLOAT3 m_LastBakedSunPosition{ 0.0f, 0.0f, 0.0f };
        // 最後に焼いたときの実効プリ露出。空はプリ露出済みの値で焼かれるため、
        // 露出が動いたときも焼き直さないと空だけ古い露出のまま取り残される
        float m_LastBakedExposureEV100 = 0.0f;
        // 最後に焼いたときのタービディティ。m_Settings.Sky.Turbidityが動いたときも、Preethamの
        // xyYモデルの形自体が変わるため焼き直しが要る(exposureMovedと同じ形の判定。Render()参照)
        float m_LastBakedTurbidity = 0.0f;
        // 最後に焼いたときの空の彩度。タービディティと同じ理由で、動いたら焼き直す
        float m_LastBakedSkySaturation = 0.0f;

        // P18: 雲込みの空の照度(SkyIntegrateのCloudSkyLight)とIBLキューブの平均透過率
        // (m_ActiveCloudTransmittance)は、どちらもベイクのタイミングでしか更新されない。
        // ところが焼き直しの判定に雲のパラメータが入っていなかったため、被覆率を動かしても
        // 古い値が残り続けていた。ここへ「ベイク時点の雲のパラメータ」を覚えておき、
        // 変化したら焼き直す(Render()の焼き直し判定を参照)。
        // **風のスクロールとカメラ位置は入れない**——毎フレーム動くので入れると毎フレーム
        // 焼き直しになる。求めているのは半球平均なので、雲の場の平行移動では値がほとんど動かない
        struct CloudBakeSignature
        {
            float CumulusCoverage = -1.0f;   // 無効(m_Settings.Cloud.Enabled=false)なら0
            float CumulusAltitude = 0.0f;
            float CumulusUvScale = 0.0f;
            float CumulusDensity = 0.0f;
            float CumulusForwardG = 0.0f;
            float CumulusThickness = 0.0f;   // ボリューム無効なら0(FrameConstantsと同じ扱い)
            float CloudTypeBias = 0.0f;
            float CirrusCoverage = 0.0f;     // 無効(m_Settings.Cloud.CirrusEnabled=false)なら0
            float CirrusAltitude = 0.0f;
            float CirrusUvScale = 0.0f;
            float CirrusDensity = 0.0f;
            float CirrusAnisotropy = 0.0f;
            float FogSigma0 = 0.0f;          // 霞は雲の見え方(打ち切り)を変えるので入れる
            float FogScaleHeight = 0.0f;
            float FogRefHeight = 0.0f;
            float FogEnabled = 0.0f;

            bool operator==(const CloudBakeSignature&) const = default;
        };
        // 現在の設定からシグネチャを作る。**FrameConstants/SkyIntegrateConstantsへ詰めるのと
        // 同じ有効/無効の潰し方をすること**(m_Settings.Cloud.Enabled=falseなら被覆率0、など)。
        // 揃っていないと「無効にしたのに焼き直しが走らない」取りこぼしが出る
        CloudBakeSignature MakeCloudBakeSignature() const;
        CloudBakeSignature m_LastBakedCloudSignature{};
        bool m_HasBakedCloudSignature = false;
        // 空パラメータ(ティント4本+照度正規化済みの天頂輝度)をGPU側で計算するコンピュートシェーダー
        // (SkyIntegrate.hlsl)。**CPU側に同じ式のミラーを置いてはいけない**(二重実装になる)。
        // 結果はm_SkyResources.ParametersBuffer(SkyGenerate.hlsl/DeferredLighting.hlsl/SSR.hlslが読む)へ書く
        // 空・大気・雲のリソースの持ち主は Rendering/SkyResources.h
        Rendering::SkyResources m_SkyResources;
        // IBL・BRDF・雲ノイズ・大気の焼き上がりの状態は Passes/EnvironmentPasses へ移した
        // 畳み込み結果とBRDF積分LUTの持ち主は Rendering/IBLResources.h
        Rendering::IBLResources m_IBLResources;

        std::unique_ptr<RHI::IRHIShader> m_PrefilterComputeShader;

        // --- エミッシブ光源(自発光メッシュを光源として扱う) ---
        // 中身と、それぞれが何のためにあるかは Scene/EmissiveLightSet.h
        Scene::EmissiveLightSet m_EmissiveLights;

        // 反射プローブ(19章): プローブ位置から6方向をProbeCapture.hlslで2Dレンダーターゲットへ描き、
        // IBLConvolve.hlsl CSCopyCaptureToCubeFaceでスクラッチのキューブマップへ組み上げてから、
        // IBLと同じCSIrradiance/CSPrefilterで畳み込んでプローブごとのキューブマップ配列へ書き込む。
        // 環境ソースを差し替えるだけなので、シェーダー側の評価式(EvaluateIBL)はIBLと完全に共通。
        //
        // キャプチャ解像度。プリフィルタ済み鏡面のベース解像度(Passes::kIBLPrefilterBaseSize)と揃えることで、
        // ミップ0が「畳み込み無しのキャプチャそのもの」になりデバッグ表示で生の映り込みを確認できる
        // 出所は Passes/ReflectionProbeConstants.h(移行中の別名)
        std::unique_ptr<RHI::IRHIShader> m_ProbeCaptureVertexShader;
        std::unique_ptr<RHI::IRHIShader> m_ProbeCapturePixelShader;
        std::unique_ptr<RHI::IRHIShader> m_ProbeCubeCopyComputeShader;
        int m_SelectedProbeIndex = -1;
        // 焼き上がりに影響する状態(時刻・太陽・シャドウ・IBL強度・全ライト)から署名を作る。
        // 影響範囲(形状・半径・ブレンド距離)はキャプチャ内容を変えないため含めない
        uint64_t ComputeProbeBakeSignature() const;
        // 焼き直しを要求する実効プリ露出の変化量[EV]。1段=明るさ2倍。
        // 手続き空の0.05段よりずっと粗いのは、フルベイクがプローブ数×6面の描画になるため。
        // 1日を通した時刻変化(最大18段)なら十数回のフルベイクに収まる
        static constexpr float kProbeRebakeExposureEV = 1.0f;

        // オクタヘドラル1プローブぶんの1辺のテクセル数(境界を含まない)。
        // 拡散イラディアンスは低周波なのでこの程度で足りる。距離は遮蔽の輪郭を担うので広く取る
        // 出所は Passes/DDGIConstants.h(移行中の別名)
        // 出所は Passes/DDGIConstants.h(移行中の別名)
        // 各辺に足す境界の幅。オクタヘドラルは正方形の縁が球面上で折り返して繋がるため、
        // その繋がる先のテクセルを外周へ複製しておかないと、バイリニア補間が縁で破綻する
        // (隣のプローブのテクセルを拾ってしまうことの防止も兼ねる)
        // 出所は Passes/DDGIConstants.h(移行中の別名)
        // アトラス上の1プローブぶんのセルの1辺(境界込み)
        // 出所は Passes/DDGIConstants.h(移行中の別名)
        // 出所は Passes/DDGIConstants.h(移行中の別名)
        // プローブ数の上限。反射プローブと違いアトラスはシーン読み込み時に確保し直すので
        // 技術的な固定容量ではないが、.ksceneの書き間違いで数GBのアトラスを作らないための歯止め
        // シーン全体で確保してよいプローブ数の上限。**容量の限界ではなく、`.kscene`の
        // 打ち間違いでギガバイト単位を確保しないための番人**である。
        // クリップマップLODでプローブ総数が「格子の積 × LOD段数」になったので引き上げた
        // (8192でもイラディアンス8MB + 距離16MB程度で、実際の律速は更新スループット側)
        // 出所は Passes/DDGIConstants.h(移行中の別名)
        // クリップマップLODの最大段数。FrameConstantsへ段数ぶんの配列を持つので有界にしておく。
        // SceneLoaderのLODCountの検証範囲と一致させること
        static constexpr uint32_t kDDGIMaxLODCount = 4;
        // 【m_GIResourcesより後に宣言すること】ボリュームの実体を参照で掴むので、
        // 宣言順が逆になると未初期化のメンバを束ねることになる
        GI::DDGIGrid m_DDGIGrid{ m_GIResources.GIVolume };

        // m_GIResources.GIVolumeのProbeCountsに合わせてアトラス2枚を確保し直す。ボリュームが無いシーンでは
        // 1プローブぶんのダミーを確保する(SRVは常にバインドできる必要があるため、
        // 「確保しない」という選択肢は取れない。無効化はDDGIParams0.wで行う)
        void RecreateDDGIAtlases();

        // ObjectConstantsのリングに要求する「1フレームあたりの書き込み回数」。
        // 根拠はこのバッファを作っている箇所(KurenaiEngine3D.cpp)のコメントを参照
        static constexpr uint32_t kObjectConstantUpdatesPerFrame = 16384;
        // DDGI以外のパスが1メッシュあたり何回描くかの見積り。DDGIへ回す予算から差し引く。
        // 内訳の目安: 深度プリパス1 + G-Buffer1 + シャドウ4 + 半透明・平面反射・水面で数回、
        // これに反射プローブのキャプチャ(こちらも1プローブ6面ぶんメッシュを描き直す)が乗る。
        // 厳密に数えず多めに取っているのは、パス構成が設定とシーンで変わるため
        static constexpr uint32_t kDDGIFrameBudgetReserveDrawsPerMesh = 16;
        // 上のクランプが効いたことを一度だけログへ出すためのフラグ(毎フレーム出さない)
        bool m_DDGIProbesPerFrameClampReported = false;

        // 水面。m_Settings.Sky.TimeOfDayの自動進行とまったく同じ方針
        // (RenderThreadMainが同じ場所・同じ条件分岐の形で進める)で、水面法線マップの
        // スクロール位相を[0,1)で持つ。FrameConstants.TimeParams.xとしてWater.hlslへ渡る
        float m_WaterScrollOffset = 0.0f;
        // --- 平面反射 ---
        // 水面に不透明ジオメトリの鏡像を映す専用フォワードパス。設計判断の詳細は
        // Shaders/3D/PlanarReflection.hlsl冒頭のコメントを参照。反射解像度はレンダー解像度に
        // m_Settings.Reflection.PlanarResolutionScaleを掛けた値で、実際の作成はCreatePlanarReflectionTargetsが行う。
        // 「システム」パネルのm_PendingRenderWidth/Height・m_RenderResolutionDirtyとまったく同じ方式
        // (要求を記録するだけにしてRender()の先頭でまとめて反映する。理由はCreateRenderTargets/
        // RequestRenderResolutionのコメント参照。GPUがまだ参照しているテクスチャを
        // 即座には破棄できないため)
        float m_PendingPlanarReflectionResolutionScale = Defaults::PlanarReflectionResolutionScale;
        bool m_PlanarReflectionResolutionDirty = false;
        // 複数の水面インスタンスが異なる高さで見つかったことを検出した最初のフレームだけ
        // 警告ログを出すためのフラグ(m_LightTileOverflowLoggedと同じ作法)。平面反射は
        // 「水面は単一の水平な平面である」という前提に立っており、複数ある場合は最初のものだけを使う
        bool m_PlanarReflectionMultipleWaterLogged = false;

        // 風によるノイズ空間の移動量。m_WaterScrollOffsetと同じくUIつまみではなく内部状態で、
        // RenderThreadMainがSky.hlsliのkCloudNoisePeriodと同じ周期でstd::fmodしながら進める
        DirectX::XMFLOAT2 m_CloudScrollOffset{ 0.0f, 0.0f };
        // 判断B(被覆率による平均透過率をIBLキューブのベイク時にだけ掛ける)のキャッシュ。
        // bakeSkyThisFrameブロックで確定させ、ベイクとFrameConstantsが同じタイミングの
        // 値を見るようにする(GPU側のm_SkyResources.ParametersBufferと同じ更新タイミング)。
        // 巻雲(m_Settings.Cloud.CirrusCoverage)も加味した2層の積になる
        // (ComputeCloudAverageTransmittance参照)
        float m_ActiveCloudTransmittance = 1.0f;

        // 風によるノイズ空間の移動量(巻雲側)。m_CloudScrollOffsetとまったく同じ形で
        // RenderThreadMainがkCloudNoisePeriodの周期でstd::fmodしながら進める。
        // 凍結トグルはm_Settings.Cloud.TimeFrozenを共有する(片方にしか効かないとA/B比較の対照が
        // 崩れるため。RenderThreadMainのスクロール更新箇所を参照)
        DirectX::XMFLOAT2 m_CirrusScrollOffset{ 0.0f, 0.0f };


        // パスごとにバインドするサンプラーの組。スロットの役割(s0=MaterialSampler、
        // s1=ColorSampler、s2=DataSampler)はShaders/3D/Samplers.hlsliで定義しており、
        // どちらのセットを使うかでs0の実体だけが変わる。
        //
        // マテリアルをタイリングで読むパス(G-Buffer・半透明フォワード・IBL畳み込み)用。
        // s0は異方性16x + Wrap
        std::unique_ptr<RHI::IRHISamplerSet> m_MaterialSamplers;
        // フルスクリーンのスクリーン空間パス(DirectLighting/DeferredLighting/SSAO/SSIL/SSR/
        // AOブラー/トーンマップ/Present)用。これらは画面内の中間バッファしか読まないため、
        // s0にもWrapではなくLinear + Clampを入れる。こうしておくとシェーダ側で役割を選び違えても
        // 画面端でUVが反対側へ回り込む不具合が起きない
        std::unique_ptr<RHI::IRHISamplerSet> m_ScreenSpaceSamplers;
        std::unique_ptr<RHI::IRHIBuffer> m_FrameConstantBuffer;
        std::unique_ptr<RHI::IRHIBuffer> m_ObjectConstantBuffer;

        // 有効ライト数を渡すb1の持ち主は Passes/LightingPasses。
        // ライトのリスト本体は m_SceneGPUResources.LightBuffer
        // 容量(kMaxLights)超過を検出した最初のフレームだけ警告ログを出すためのフラグ
        bool m_LightOverflowLogged = false;

        // タイルライトカリング(Shaders/3D/LightCulling.hlsl)。画面を16x16ピクセルのタイルに分け、
        // タイルごとに「そのタイルに届くライト」のインデックスリストをコンピュートシェーダーで作る。
        // 直接光パスはそのリストだけをループするため、ピクセルあたりのコストが
        // シーン全体のライト数ではなくタイル内のライト数になる。
        // これは純粋な最適化であり、有効/無効で最終画像が変わってはならない
        // タイルライトカリングのタイルサイズ(1辺のピクセル数)。
        // LightCulling.hlsl の kTileSize および numthreads と必ず一致させること
        // 出所は Passes/MegaLightsConstants.h(移行中の別名)
        // ライトグリッド1タイルぶんの要素数(先頭1個がライト数、残りがライトインデックス)
        static constexpr uint32_t kLightTileStride = 1 + Passes::kLightTileCapacity;

        // 候補プール1タイルぶんの要素数。先頭6個がヘッダ(SumW / 届いた灯数 / 有効候補数 / 予約 /
        // 手前のViewZ / 奥のViewZ)、
        // 以降は候補1つにつき2個(ライト番号と重み)。MegaLightsTilePool.hlsl 冒頭のレイアウトと一致させること
        static constexpr uint32_t kMegaLightsTilePoolStride = 6 + 2 * kMegaLightsTilePoolCapacity;

        // タイル容量の超過"条件"(シーンのライト数が容量を超えている)を検出した最初のフレームだけ
        // 警告ログを出すためのフラグ(m_LightOverflowLoggedと同じ作法)。
        // 実際に超過したかはGPU側にしか無いため、確認はDebugView::LightTilesのマゼンタで行う
        bool m_LightTileOverflowLogged = false;

        // --- 品質プリセット(41章) ---------------------------------------------------------
        //
        // 【QualitySnapshotという名前にしている理由】Settings/QualitySettings.hの
        // struct QualitySettingsと役目がまったく違う(あちらは「今どのプリセットを
        // 選んでいるか」の静的な設定、こちらは「プリセットが一括で振る個々のつまみの値を
        // 退避・復元するためのスナップショット」)。同じ名前にすると呼び出し側で
        // どちらの型か紛らわしくなるため、クラス内のこちらをQualitySnapshotと呼び分ける

        // 品質プリセットが触る設定の一式(退避・復元用のスナップショット)。
        //
        // 【プリセット「高」はエンジンの静的な既定ではなく「シーン読み込み直後の値」へ戻す】
        // .ksceneはSSR・TAAを自分で指定できる(ApplyLoadedScene参照。実例として
        // MontSaintMichel.ksceneはどちらも明示的に有効化している)。静的なDefaults::へ戻すと
        // 「高にしたらシーンが要求した反射が消える」ことになる。m_SceneDefaultReflectionModeが
        // UIの右クリック(既定値へ戻す)に対して同じ問題を解いており、プリセットもそれに倣う
        struct QualitySnapshot
        {
            ReflectionMode Reflection = ReflectionMode::Off;
            bool PlanarReflectionEnabled = Defaults::PlanarReflectionEnabled;
            float PlanarReflectionResolutionScale = Defaults::PlanarReflectionResolutionScale;
            bool CloudVolumetric = Defaults::CloudVolumetric;
            bool CirrusEnabled = Defaults::CirrusEnabled;
            bool StarsEnabled = Defaults::StarsEnabled;
            bool TAAEnabled = Defaults::TAAEnabled;
            bool BloomEnabled = Defaults::BloomEnabled;
            bool ScreenSpaceShadowEnabled = Defaults::ScreenSpaceShadowEnabled;
            int32_t DDGIProbesPerFrame = Defaults::DDGIProbesPerFrame;
            uint32_t SSAOKernelSize = Defaults::SSAOKernelSize;
            uint32_t CloudRaymarchSteps = Defaults::CloudRaymarchSteps;
            DDGIUpdateMode DDGIUpdate = DDGIUpdateMode::Always;
            bool DDGIHalfResolution = Defaults::DDGIHalfResolution;
        };

        // 現在の各メンバから上記の一式を読み出す
        QualitySnapshot CaptureQualitySettings() const;
        // 一式を各メンバへ書き戻す。平面反射の解像度倍率だけはレンダーターゲットの作り直しを
        // 伴うため直接代入せず、RequestPlanarReflectionResolutionScale()経由で要求する
        void ApplyQualitySettings(const QualitySnapshot& settings);

        // シーンを読み込んだ直後の値。ApplyLoadedSceneが控え、プリセット「高」が戻る先になる
        QualitySnapshot m_SceneDefaultQuality;

        // 現在描画しているシーン。ApplyLoadedScene(Renderスレッド)だけが差し替え、
        // Render()とUIパネル(いずれもRenderスレッド)だけが読む。つまりRenderスレッド専有の状態で、
        // ミューテックスによる保護は不要(LoadSceneがUpdateスレッドから直接書き換える構成だと
        // ミューテックスが要る。26章)。
        //
        // 【読み込み中は空になる】シーン切り替えを開始した時点で旧シーンを手放すため
        // (VRAMの二重常駐を避けるため)、読み込みが終わるまでInstancesが空のまま描画される
        Assets::Scene m_Scene;
        // GPU側のシーンデータの持ち主は Rendering/SceneGPUResources.h。
        // **m_Sceneより後に宣言すること**(理由はそのヘッダの冒頭)
        Rendering::SceneGPUResources m_SceneGPUResources;
        // テクスチャの常駐ミップ制御。自前のワーカースレッドを持ち、そこがm_Sceneの
        // IRHITexture*を掴む。
        //
        // 【破棄順】m_Sceneより後に宣言し、メンバ破棄順(宣言の逆順)でm_Sceneより先に
        // 破棄されるようにする。加えて、シーンを差し替えるときはUpdateSceneStreamingが
        // 明示的にReset()を呼んでワーカーを止める
        Assets::TextureStreamingManager m_TextureStreaming;
        // m_Sceneと同じくRenderスレッド専有(ScenePanelが選択中のシーンの表示に読む)
        size_t m_CurrentSceneIndex = 0;
        // Updateスレッド専有。UpdateMouseLook/UpdateMovementが書き換え、TickFrameがFrameStateへ
        // スナップショットしてRenderスレッドへ渡す。シーン読み込み時の初期カメラも
        // (Renderスレッドではなく)UpdateAppliedSceneHandoff経由でこのスレッドが適用することで、
        // 書き込み手を1スレッドに保っている
        Core::Camera m_Camera;

        // --- シーン読み込みのハンドオフ -------------------------------------------------------

        // シーン読み込みのハンドオフ一式。
        // **この位置から動かさないこと**(破棄順の理由は Scene/SceneLoadHandoff.h)
        Scene::SceneLoadHandoff m_SceneLoad;

        // --- .ksceneのホットリロード -----------------------------------------------------
        //
        // 起動し直さずに.ksceneの変更を絵へ出すための仕組み。読み込み自体は上の非同期経路を
        // そのまま使い(「今のシーンをもう一度読む」だけ)、ここが持つのは「いつ発注するか」だけ。
        //
        // 【監視するのは実行ファイルの隣のファイル】エンジンが読むのは<exe>\Assets\Scenes\*.ksceneで、
        // リポジトリのScenes\*.ksceneからはKurenaiPacker --scene → Assets\Packed → xcopy の
        // 2ホップで届く。エンジンは自分が実際に読んだファイル(m_SceneFilePaths)だけを見る

        // 自動監視の有効/無効とリロード時のカメラ保持はm_Settings.Systemへ移した
        // (SceneAutoReloadEnabled / SceneReloadKeepsCamera)
        // 監視中の.ksceneの更新時刻(FILETIMEを64bitへ詰めたもの)。0は「まだ取得していない」
        uint64_t m_WatchedSceneWriteTime = 0;
        // 検証に失敗した更新時刻。同じ内容で警告ログを繰り返さないために覚えておく
        uint64_t m_SceneReloadRejectedWriteTime = 0;
        // 次に更新時刻を見る時刻。毎フレーム見る必要は無いので250msに1回へ間引く。
        // Render()のフレーム時間ではなく自前のsteady_clockで測るのは、この関数が
        // フレーム時間の更新より前に呼ばれる位置にあり、呼び出し順への依存を作らないため
        std::chrono::steady_clock::time_point m_NextSceneWatchTime{};

        // Render → Loader の破棄依頼(RetiredAssetsのコメント参照)
        std::mutex m_RetiredAssetsMutex;
        std::vector<RetiredAssets> m_RetiredAssets;

        // Loader → Render の完成品
        std::mutex m_LoadedSceneMutex;
        std::unique_ptr<LoadedScene> m_LoadedScene;

        // Render → Update の初期カメラ・ウィンドウタイトル。
        // 毎フレームのロックを避けるため、まずatomicで有無を判定してから中身を取りにいく
        std::atomic<bool> m_AppliedScenePending{ false };
        std::mutex m_AppliedSceneMutex;
        // カメラを適用するか。ホットリロードで「現在のカメラを保持する」を選んでいるときだけ
        // falseになる。falseでもウィンドウタイトルの更新は行うため、引き渡し自体は毎回発生する
        bool m_AppliedSceneApplyCamera = true;
        Core::Camera m_AppliedSceneCamera;
        std::wstring m_AppliedSceneTitle;
        // そのシーンが持つ[CameraPath]の一覧。**consumeせずに持ち続ける**のがポイントで、
        // -camerapath がシーンの適用より後に呼ばれても名前を解決できるようにするため
        // (起動オプションの適用順に依存させない)
        std::vector<Assets::CameraPath> m_AppliedSceneCameraPaths;

        // Loaderスレッド専有。「今どのスカイボックスを読み込み済みか」の真実。
        // スカイボックスを読むのがこのスレッドだけなので、ここで持つのが最も素直になる
        std::wstring m_LoaderSkyboxPath;
        // 水面法線マップ版。m_LoaderSkyboxPathと同じ扱いで、空文字列なら
        // フラット法線フォールバックを読み込み済みであることを表す
        std::wstring m_LoaderWaterNormalMapPath;

        // DiscoverScenesが起動時に一度だけ列挙する.ksceneの一覧。要素の並びがImGuiのシーン
        // 一覧・LoadSceneのインデックスに対応する(ファイル名の昇順)
        std::vector<std::wstring> m_SceneFilePaths;
        std::vector<std::wstring> m_SceneDisplayNames;

        // ApplyLoadedSceneがm_Scene.Lights(SceneLoaderが各ModelInstanceのModel::Lightsをワールド空間へ
        // 変換し、.kscene自身の[Light]セクションのライトと合成済みのシーン全体のライト一覧)から
        // コピーし、以降ImGui(Lightingパネル)が編集する。アセット由来のデータとユーザー編集を
        // 分離するため(シーンを再読み込みすればアセット既定値に戻る)。
        // m_Sceneと同じくRenderスレッド専有のためロックは不要
        std::vector<Assets::Light> m_Lights;
        int m_SelectedLightIndex = -1;
        // 実際にライト強度へ事前乗算される「実効プリ露出」。m_Settings.PostProcess.SceneExposureEV100(ユーザー設定)に
        // 時刻由来のバイアスを足したもので、Renderスレッドのみが読み書きする。
        //
        // 【固定にしないこと】EV100=15固定だと夜がfp16の非正規化域へ落ちて情報が失われる。
        // 導出は docs/ImplementationDetail.md 21.5。プリ露出は Tonemap・Bloom・AutoExposure が
        // すべて同じ値で割り戻すため、フレーム単位で変えても最終的な絵は変わらない
        float m_EffectiveExposureEV100 = 15.0f;
        // 実効プリ露出が初期化済みか(初回フレームは平滑化せず即座に合わせる)。
        // **シーン読み込み時にLoadSceneがfalseへ戻す**。シーンをまたぐと時刻が入れ替わって
        // 実効プリ露出が最大18段跳ぶが、そこを平滑化しても得られるものが無い(プリ露出は
        // Tonemap側で割り戻されるため過渡の絵には現れない)一方で、追従の途中の値で焼かれた
        // 資産(反射プローブ)が残ってしまう。切り替えは平滑化せず即座に合わせるのが正しい
        bool m_EffectiveExposureInitialized = false;

        std::chrono::steady_clock::time_point m_LastFrameTime;

        // Update(メインスレッド)とRender(描画専用スレッド)を並列化するためのハンドオフ機構。
        // キュー深度1(バッファ1面)で、Updateが1フレーム分書き込むたびにRenderが取り込んでから
        // 重いGPU発行に入るため、UpdateスレッドはRenderの実際の描画時間とは並行して次フレームを
        // 計算できる(=Update(N+1)とRender(N)が並列に進む)
        std::thread m_RenderThread;
        std::mutex m_FrameStateMutex;
        std::condition_variable m_FrameStateCV;
        FrameState m_FrameState;
        bool m_FrameStateReady = false;
        bool m_FrameStateTaken = true;
        bool m_StopRenderThread = false;
        // Renderスレッド側のフレーム間隔計測用(時刻自動進行・FPS計測に使う。Renderスレッドのみが読み書きする)
        std::chrono::steady_clock::time_point m_LastRenderFrameTime;
        // 直前のRenderフレームの経過時間[秒]。自動露出の時間方向の順応に使う。
        // RenderThreadMainが書き、Render()が読む。どちらもRenderスレッドなので追加の排他は不要
        // (m_Settings.Sky.TimeOfDayと同じ扱い)
        float m_RenderDeltaTime = 0.0f;
        float m_FixedTimeStep = 0.0f;

        // --- 決定的カメラ経路(計測専用) -------------------------------------------------
        //
        // 【スレッドの持ち分】m_CameraPath / m_CameraPathActive / m_UpdateFrameIndex は
        // **Updateスレッド専有**(m_Cameraと同じ)。m_RequestedCameraPathName と
        // m_CameraPathStartFrame は Run() より前に起動オプションから設定される想定で、
        // 再解決の要求だけを atomic で受け渡す
        std::wstring m_RequestedCameraPathName;
        // 次のUpdateで名前を解決し直す。シーンが適用されたときと、名前が指定されたときに立つ
        std::atomic<bool> m_CameraPathNeedsResolve{ false };
        Assets::CameraPath m_CameraPath;
        bool m_CameraPathActive = false;
        // 負なら Passes::kMegaLightsAccumWarmup を使う(-dumpframe の既定と同じ定数を共有する。
        // 値が2つに割れると片方だけ直す事故が起きるので、新しい定数は作らない)
        int m_CameraPathStartFrame = -1;
        // -camerapathvalidate。シーンが適用された時点で全経路の検算ログを出す
        bool m_CameraPathValidateRequested = false;
        // Updateスレッド側のフレーム番号。TickFrameの冒頭で前進させ、Renderの
        // m_History.FrameIndex と一致することを DecideFrameJitterAndCamera で検算する
        uint32_t m_UpdateFrameIndex = 0;
        // フレーム番号の食い違いは1回だけログに出す(毎フレーム出すとログが埋まる)
        bool m_PathFrameMismatchLogged = false;

        // 集計状態はすべてRenderスレッドのみが読み書きするため追加の排他制御は不要
        Diagnostics::FrameStatsLogger m_FrameStats;

        // モデル単位フラスタムカリングの統計(1フレーム分)。フレーム先頭でリセットし、
        // LogFrameStatsIfDueが集計期間の合計として出す。
        // 絵から判定できないものを数値で出している(理由は docs/ImplementationDetail.md 64.5)
        uint32_t m_FrustumCullTested = 0;
        uint32_t m_FrustumCullCulled = 0;

        // --- モデルLOD(.ksceneの[Model]LODPath / LODDistance) --------------------------------
        //
        // インスタンスごとの「いま使っている段」と、切り替え中のクロスディザの進み具合。
        // m_Scene.Instancesと同じ添字で並び、ApplyLoadedSceneで作り直す。
        //
        // 【Assets::Sceneではなくエンジン側に持つ理由】これは読み込んだデータではなく
        // カメラ位置から毎フレーム決まる実行時の状態で、Loaderスレッドが作るSceneに
        // 混ぜると「シーンの内容」と「今の見え方」の境界が曖昧になる
        // 実体は Scene/InstanceLODState.h(UIが型名を書けるよう入れ子にしていない)
        using InstanceLODState = Scene::InstanceLODState;
        std::vector<InstanceLODState> m_InstanceLODStates;
        // 段の切り替えにかける秒数とヒステリシス幅はm_Settings.Geometry.LODFadeDuration /
        // LODHysteresisへ移した
        // 統計。1フレームあたりの段の切り替え回数と、そのフレームでフェード中のインスタンス数。
        // 【0なら一度も切り替わっていない】LODが効いているかはここでしか分からない
        // (フェード中のインスタンス数はm_RenderStats.LODFadingCountへ出す。UIが読む完成値のため)
        uint32_t m_LODSwitchCount = 0;
        // カメラ位置から各インスタンスの段を決め、フェードを進める。
        // レンダーグラフの構築より前に1フレーム1回だけ呼ぶこと ―― パスごとに測り直すと
        // 深度プリパスとG-Bufferが違う段を選び、画面に穴が開く
        void UpdateModelLOD(const DirectX::XMFLOAT3& cameraPosition, float deltaSeconds);
        // instanceIndex番目のインスタンスについて、このフレームで描く段を返す。
        // フェード中は2件(切り替え先と元)、そうでなければ1件。DitherFadeも一緒に返す
        using LODDraw = Scene::LODDraw;
        // 戻り値の件数。fadingなら2、それ以外は1
        uint32_t GetLODDraws(size_t instanceIndex, LODDraw (&outDraws)[2]) const override;
        // シャドウ・反射プローブ・DDGI用。常に最も粗い段を返す(影と間接光はテクスチャを読まない)
        const Assets::Model* GetCoarsestLOD(const Assets::ModelInstance& instance) const override;

        // DDGIから自発光を抜くか。**判定を1か所に置くこと** ―― ラスタ経路(ObjectConstantsの
        // 倍率)とレイトレ経路(DDGITraceConstants.Params1.w)で条件がずれると、
        // 環境によって二重計上の有無が変わる。しかも絵は両方それらしく出る
        bool ShouldSuppressEmissiveForGI() const;

        // --- モデルのストリーミング(.ksceneの[Scene]StreamingDistance) ----------------------
        //
        // カメラ位置から「読むべきなのにまだ無いモデル」を選んでLoaderスレッドへ発注し、
        // 出来上がったものを受け取ってインスタンスへ差し込む。
        // レンダーグラフの構築より前に1フレーム1回だけ呼ぶこと。
        //
        // 【まず読み込みだけ】破棄はまだ行わない。絵が出ることを確かめてから、
        // kFrameCountフレーム遅延させる解放キューを通して足す
        void UpdateModelStreaming(const DirectX::XMFLOAT3& cameraPosition);
        // UpdateModelStreamingの5段。**順序に意味がある**ので入れ替えないこと
        // (破棄待ちを進める → 出来上がりを取り込む → 候補を集める → 破棄する → 発注する)。
        // 破棄待ちを1フレーム進め、0になったものだけLoaderスレッドへ渡す
        void AdvancePendingModelRelease();
        // Loaderスレッドが仕上げたモデルをインスタンスへ差し込む
        void IntegrateLoadedModels();
        // カメラからの距離を見て、読み込みたいものと「まだ要る」パスを集める
        void CollectStreamingCandidates(
            const DirectX::XMFLOAT3& cameraPosition, std::vector<Scene::StreamingCandidate>& candidates,
            std::unordered_set<std::wstring>& neededPaths);
        // neededPathsに無いモデルを破棄キューへ積む
        void EvictDistantModels(const std::unordered_set<std::wstring>& neededPaths);
        // 近い順に、1フレームの上限まで発注する
        void IssueStreamingRequests(std::vector<Scene::StreamingCandidate>& candidates);
        // 常駐が変わったことを記録する。実際の作り直しは静かになってから
        void RequestRaytracingRebuild();
        // 出来上がったRaytracingSceneの差し替えと、静かになった後の発注。
        // UpdateModelStreamingの後にフレーム1回だけ呼ぶ
        void UpdateRaytracingRebuild();

        // Render → Loader の読み込み発注。m_SceneLoad.RequestMutexで保護し、
        // モデルのストリーミングの受け渡し一式。
        // **この位置から動かさないこと**(破棄順の理由は Scene/ModelStreamingState.h)
        Scene::ModelStreamingState m_Streaming;

        // --- レイトレーシングを常駐の増減へ追随させる ---
        // **この位置から動かさないこと**(破棄順の理由は Scene/RaytracingRebuildState.h)
        Scene::RaytracingRebuildState m_RaytracingRebuild;

        // メッシュ単位フラスタムカリングの統計(1フレーム分)。上のモデル単位とまったく同じ扱い。
        //
        // 【絶対に上のカウンタと混ぜない】分母も意味も違う。モデル単位は
        // 「シーンのインスタンス数」が分母で、メッシュ単位は「モデル単位を通過した
        // インスタンスのメッシュ数の合計」が分母になる。合算すると、どちらが効いているのか
        // ―― あるいは片方が一度も実行されていないのか ―― が読めなくなる。
        //
        // 【効くシーンが逆】モデル単位は.kmodelを多数並べるシーン(PLATEAUの671タイル)で効き、
        // 1モデルに数千メッシュを持つアセット(Emerald Square、Bistro)では1つも間引けない。
        // メッシュ単位はその逆で、後者でしか値が動かない
        uint32_t m_MeshCullTested = 0;
        uint32_t m_MeshCullCulled = 0;

        // 完成した最後のフレームの値はm_RenderStats.FrustumCullTestedLastFrame等へ出す。
        // UIパネルはRenderの外で描かれるため、上のカウンタをそのまま読むと
        // リセット直後の0になる(ドローコール数のm_RenderStats.DrawCalls*LastFrameと同じ)

        bool m_MouseCaptured = false;
        POINT m_MouseCaptureCenter{};

        // F1キーでImGuiの表示/非表示を切り替える(WasKeyPressedがエッジ検出を内蔵しているため、
        // 前フレームの押下状態を保持するメンバは不要)
        bool m_ImGuiVisible = true;
    };
}

#pragma warning(pop)
