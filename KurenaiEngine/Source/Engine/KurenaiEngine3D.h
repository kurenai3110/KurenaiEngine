#pragma once

#include <Windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "EngineDefaults.h"
#include "KurenaiEngineBase.h"
#include "KurenaiTypes.h"

#include "Assets/Scene.h"
#include "Core/Camera.h"
#include "Core/CPUProfiler.h"

#pragma warning(push)
#pragma warning(disable: 4251)

namespace Kurenai::UI
{
    class UIManager;
    class ScenePanel;
    class RenderingPanel;
    class PostProcessPanel;
    class DebugViewPanel;
    class LightingPanel;
    class SystemPanel;
    class ProfilerPanel;
    class ReflectionProbePanel;
}

namespace Kurenai
{
    // 3Dサンプルプログラム向けの公開API。Deferred Shading(G-Buffer)によるPBRレンダリング、
    // シャドウマッピング、SSAO/SSIL(間接光)、SSR(反射)、ImGuiによる各種設定パネル、
    // 複数シーンの切り替えまでを内包した完結型のレンダラー。
    // 構築してRun()を呼ぶだけでウィンドウが開き、終了するまでブロックする
    class KURENAI_3D_API KurenaiEngine3D : public KurenaiEngineBase
    {
    public:
        explicit KurenaiEngine3D(GraphicsAPI api = GraphicsAPI::DX11, uint32_t renderWidth = 1280, uint32_t renderHeight = 720);
        ~KurenaiEngine3D();

        void Run();

        // カスケードシャドウマップの分割数。カメラ視錐台をこの数だけの深度範囲に分割し、
        // それぞれ専用のシャドウマップ・ライト正射影を持たせる。
        // FrameConstants::CascadeSplitsがXMFLOAT4(4要素)にfar距離を詰めているため、
        // この値を変える場合はKurenaiEngine3D.cppのCascadeSplits周りも合わせて変更が必要。
        // KurenaiEngine3D.cpp側の匿名名前空間(FrameConstants宣言)からも参照するためpublicにしている
        static constexpr uint32_t kCascadeCount = 4;
        static_assert(kCascadeCount == 4, "CascadeSplitsはXMFLOAT4前提のため4カスケード固定");

    private:
        // UIパネル群(Source/Engine/UI/)は、m_SSAORadius等のパラメータメンバをImGuiウィジェットへ
        // アドレスで直接渡すためprivateへアクセスする必要がある。
        // パラメータを専用の構造体へ切り出して物理的に移動させる案も検討したが、Render()内の
        // 参照が約200箇所あり、書き換えの過程で1箇所間違えてもコンパイルが通ってしまい静かに
        // 壊れるため見送った(元々UI関数がメンバ関数としてprivateに触れていた構造を、
        // 「friendだから触れる」へ平行移動しただけで、振る舞いの差分は無い)。
        // friendにしても「UIから触ってよいのはパラメータ用メンバだけで、RHIリソース
        // (m_GBufferAlbedo等)には触らない」という規約は各パネルの実装側で守ること
        friend class UI::UIManager;
        friend class UI::ScenePanel;
        friend class UI::RenderingPanel;
        friend class UI::PostProcessPanel;
        friend class UI::DebugViewPanel;
        friend class UI::LightingPanel;
        friend class UI::SystemPanel;
        friend class UI::ProfilerPanel;
        friend class UI::ReflectionProbePanel;

        // UpdateスレッドからRenderスレッドへ、1フレーム分のカメラ・ImGui表示状態を引き渡すための
        // スナップショット。m_TimeOfDay等それ以外の状態はRenderスレッド側のみが読み書きするため
        // ここには含めない(RenderThreadMain参照)
        struct FrameState
        {
            Core::Camera Camera;
            bool ImGuiVisible = true;
        };

        void CreateSceneResources();
        // 中間バッファの精度構成(m_BufferPrecision)によって変わるフォーマット。
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
        void LoadScene(size_t sceneIndex);
        // SSAO/SSILの半径・厚みとSSRの距離・厚みを、現在のシーンの対角長から決め直す。
        // これらは固定の既定値を持たないため、UIの「既定値に戻す」ではなくこれを呼ぶ
        // (シーン読み込み時はFrameCameraToModelの先頭から呼ばれる)
        void ResetSceneDependentParams();
        void FrameCameraToModel();
        // imguiWantsMouseはImGuiがマウス入力を掴んでいるか(Renderスレッドから
        // m_ImGuiWantCaptureMouse経由で受け取る)。パネルの上で右ドラッグを始めても
        // 視点回転が始まらないようにするために使う
        void UpdateMouseLook(bool imguiWantsMouse);
        void UpdateMovement(float deltaTime);
        void UpdateImGuiToggle();
        // ScenePanel(Renderスレッド)がm_PendingSceneIndexへ書き込んだシーン切り替え要求を
        // 見て、あればこのUpdateスレッドからLoadSceneを呼ぶ(LoadSceneをUpdateスレッド上で実行するための
        // ハンドオフ。詳細はm_PendingSceneIndexのコメント参照)
        void UpdateSceneSwitch();
        void Update(float deltaTime);
        // 1フレーム分のUpdateと、Renderスレッドへのフレーム状態の受け渡しを行う。
        // 通常はRun()のループから、ウィンドウのドラッグ中(Windowsのモーダルループ中で
        // PumpMessagesが戻ってこない間)はWindowのタイマーから呼ばれる
        void TickFrame();
        void RenderThreadMain();
        void Render(const FrameState& frameState);
        // ProfilerPanel用。m_DeviceはKurenaiEngineBaseのprotectedメンバであり、派生クラスの
        // friendから触れるかどうかはC++の規則の解釈が分かれるため、ここで明示的に橋渡しする
        float GetLastFrameGPUWaitTimeMs() const;
        // SystemPanelの表示用。m_Windowも同様の理由で橋渡しする。
        // Windowsのディスプレイ設定で指定されている拡大率(UIの拡大率もこれに追従する)
        float GetMonitorDpiScale() const;
        // カメラ視錐台をkCascadeCount個の深度範囲に分割する(near/far境界、View空間での距離)。
        // 対数分割と均等分割を混合した実用的な分割(Practical Split Scheme)を使う
        void ComputeCascadeSplits(const Core::Camera& camera, float (&outSplits)[kCascadeCount]) const;
        // カメラ視錐台のうち[splitNear, splitFar]の範囲(View空間距離)だけを覆う、平行光のライト視点
        // 正射影ビュー・プロジェクション行列を求める。カスケードごとに1回呼ぶ
        DirectX::XMMATRIX ComputeCascadeLightViewProj(
            const DirectX::XMFLOAT3& lightDirection, const Core::Camera& camera, float splitNear, float splitFar) const;

        // 起動時に選択されたグラフィックスAPI(タイトルバー・ImGui表示用に保持)
        GraphicsAPI m_GraphicsAPI;

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

        // ImGuiが入力を掴んでいるかを、RenderスレッドからUpdateスレッドへ返す逆方向のハンドオフ。
        // FrameState(Update→Render)の逆向きだが、渡す値がboolを2つだけなのでロックを増やす
        // 価値がなく、atomicで足りる。Updateスレッドはこれを見てWASD移動と視点回転の開始を抑止する
        std::atomic<bool> m_ImGuiWantCaptureKeyboard{ false };
        std::atomic<bool> m_ImGuiWantCaptureMouse{ false };

        // GPUタイムスタンプクエリによる各パスの計測(Shadow/GBufferなど)。数フレーム遅れの結果が返る
        std::unique_ptr<RHI::IRHIGPUProfiler> m_GPUProfiler;
        // 各パスのコマンド記録にかかるCPU時間の計測(RHIに依存しないためDX11/DX12を直接比較できる)
        Core::CPUProfiler m_CPUProfiler;

        // G-Bufferの内部解像度。ウィンドウサイズとは独立しており、表示時はアスペクト比を保って拡大縮小する
        uint32_t m_RenderWidth;
        uint32_t m_RenderHeight;

        // 中間バッファの精度構成。HDRが本来採用したい構成で、Legacy8bitはM7以前の
        // 「中間バッファはすべてR8G8B8A8_UNorm」だった構成を再現する比較用の経路。
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
        BufferPrecision m_BufferPrecision = BufferPrecision::HDR;
        // ImGuiでBufferPrecisionが変更されたことをRender()へ伝えるフラグ。レンダーターゲットの
        // 作り直しはGPUがそれらを参照していない状態で行う必要があるため、UI関数の中では実行せず
        // Render()の先頭(RenderGraphの構築より前)でm_Device->WaitForGPUIdle()を挟んで処理する
        bool m_BufferPrecisionDirty = false;

        // ジオメトリパス(G-Buffer書き込み)
        std::unique_ptr<RHI::IRHIShader> m_GBufferVertexShader;
        std::unique_ptr<RHI::IRHIShader> m_GBufferPixelShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_GBufferPipelineState;
        // ミラーリング(Worldの行列式が負)されたインスタンス用。表裏判定を入れ替えただけで
        // 他は上と同一(ModelInstance::IsMirrored、docs/Architecture.html 10.2節)
        std::unique_ptr<RHI::IRHIPipelineState> m_GBufferPipelineStateMirrored;
        std::unique_ptr<RHI::IRHITexture> m_GBufferAlbedo;
        std::unique_ptr<RHI::IRHITexture> m_GBufferNormal;
        std::unique_ptr<RHI::IRHITexture> m_GBufferMaterial;
        // 自発光(エミッシブ)。AO/シャドウの影響を受けずライティングパスで常に加算される
        std::unique_ptr<RHI::IRHITexture> m_GBufferEmissive;
        std::unique_ptr<RHI::IRHITexture> m_GBufferDepth;

        // 直接光パス(G-Buffer+シャドウマップからPBRの直接光(拡散+鏡面反射、シャドウ適用済み)を
        // 計算しHDRで書き出す。DeferredLightingパスとSSIL_VisibilityBitmask.hlslの両方から
        // サンプルされるため、G-Bufferと同じレンダー解像度・R32G32B32A32_Float(HDR)で保持する)
        std::unique_ptr<RHI::IRHIShader> m_DirectLightVertexShader;
        std::unique_ptr<RHI::IRHIShader> m_DirectLightPixelShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_DirectLightPipelineState;
        std::unique_ptr<RHI::IRHITexture> m_DirectLightTexture;

        // AO/GI手法の選択。SSAOは遮蔽率のみ、SSIL(Visibility Bitmask)は遮蔽率に加えて
        // 近傍サーフェスからの間接拡散光(バウンス光)も計算する。どちらも出力フォーマットは共通
        // (rgb=間接拡散光, a=遮蔽率)で、ライティングパスは選択中のテクスチャを1枚読むだけでよい
        enum class AOTechnique
        {
            SSAO,
            SSILVisibilityBitmask,
        };
        bool m_AOEnabled = Defaults::AOEnabled;
        AOTechnique m_AOTechnique = AOTechnique::SSAO;
        std::unique_ptr<RHI::IRHITexture> m_AODisabledTexture; // AO無効時に使う、遮蔽なし・間接光なしのテクスチャ

        // AO/GI共通のブラーパス(4x4ボックスブラーでrgba全チャンネルを均す。SSAO/SSIL両方から使い回す)
        std::unique_ptr<RHI::IRHIShader> m_AOVertexShader;
        std::unique_ptr<RHI::IRHIShader> m_AOBlurPixelShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_AOBlurPipelineState;

        // SSAOパス(G-BufferのNormal/Depthから遮蔽率を計算する。G-Bufferと同じレンダー解像度)
        std::unique_ptr<RHI::IRHIShader> m_SSAOPixelShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_SSAOPipelineState;
        std::unique_ptr<RHI::IRHITexture> m_SSAORawTexture;
        std::unique_ptr<RHI::IRHITexture> m_SSAOTexture;
        std::unique_ptr<RHI::IRHIBuffer> m_SSAOConstantBuffer;
        std::vector<DirectX::XMFLOAT4> m_SSAOKernel;
        float m_SSAORadius = Defaults::SSAORadius;
        float m_SSAOPower = Defaults::SSAOPower;

        // SSILパス(Visibility Bitmask): G-BufferのAlbedo/Normal/Depthから遮蔽率と間接拡散光を計算する
        std::unique_ptr<RHI::IRHIShader> m_SSILPixelShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_SSILPipelineState;
        std::unique_ptr<RHI::IRHITexture> m_SSILRawTexture;
        std::unique_ptr<RHI::IRHITexture> m_SSILTexture;
        std::unique_ptr<RHI::IRHIBuffer> m_SSILConstantBuffer;
        float m_SSILRadius = Defaults::SSILRadius;
        float m_SSILThickness = Defaults::SSILThickness;
        float m_SSILIntensity = Defaults::SSILIntensity;
        float m_SSILPower = Defaults::SSILPower;
        uint32_t m_SSILSliceCount = Defaults::SSILSliceCount;
        uint32_t m_SSILStepCount = Defaults::SSILStepCount;

        // ライティングパス(G-Bufferを読みSceneColorへ出力。G-Bufferと同じレンダー解像度)。
        // SceneColorはHDR(R16G16B16A16_Float)で、トーンマッピングは行わない(Tonemapパス参照)
        std::unique_ptr<RHI::IRHIShader> m_LightingVertexShader;
        std::unique_ptr<RHI::IRHIShader> m_LightingPixelShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_LightingPipelineState;
        std::unique_ptr<RHI::IRHITexture> m_SceneColor;

        // 半透明フォワードパス: Deferred(G-Buffer)には書き込まれなかったBLENDマテリアルのメッシュを、
        // Lightingパスの後にSceneColorへ直接フォワードシェーディングしてアルファブレンド合成する
        // (深度テストはGBuffer深度に対して行うが書き込みは行わない)。頂点レイアウトはGBufferパスと
        // 共通(POSITION/NORMAL/TEXCOORD/TANGENT)
        std::unique_ptr<RHI::IRHIShader> m_TransparentVertexShader;
        std::unique_ptr<RHI::IRHIShader> m_TransparentPixelShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_TransparentPipelineState;
        std::unique_ptr<RHI::IRHIPipelineState> m_TransparentPipelineStateMirrored;

        // Hi-Zミップチェーン: G-Buffer深度から、コンピュートシェーダーで1x1まで縮小するミップチェーンを
        // 構築するパス。各ミップは2x2ブロックの最小値(Reverse-Zのため「最も遠い」深度)を保持する。
        // オクルージョンカリングやSSRのレイマーチング高速化に使えるデータ構造だが、現時点では
        // それらの利用箇所は未実装で、デバッグ表示(Render Targets - Hi-Z)でのみ確認できる
        std::unique_ptr<RHI::IRHIShader> m_HiZCopyComputeShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_HiZCopyPipelineState;
        std::unique_ptr<RHI::IRHIShader> m_HiZDownsampleComputeShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_HiZDownsamplePipelineState;
        std::unique_ptr<RHI::IRHITexture> m_HiZTexture;
        std::unique_ptr<RHI::IRHIBuffer> m_HiZConstantBuffer;
        uint32_t m_HiZMipLevels = 1;
        // デバッグ表示(Render Targets - Hi-Z)で確認するミップレベル
        int32_t m_HiZDebugMipLevel = 0;

        // SSR(Screen Space Reflections)パス: LightingパスのSceneColorを反射先の環境色として
        // 再利用し、G-Buffer(Normal/Material/Depth)からワールド空間でレイマーチングして
        // 鏡面反射を加算する。無効時はこのパスをスキップし、Presentが直接m_SceneColorを参照する
        std::unique_ptr<RHI::IRHIShader> m_SSRVertexShader;
        std::unique_ptr<RHI::IRHIShader> m_SSRPixelShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_SSRPipelineState;
        std::unique_ptr<RHI::IRHITexture> m_SSRTexture;
        std::unique_ptr<RHI::IRHIBuffer> m_SSRConstantBuffer;
        bool m_SSREnabled = Defaults::SSREnabled;
        float m_SSRMaxDistance = Defaults::SSRMaxDistance;
        float m_SSRThickness = Defaults::SSRThickness;
        float m_SSRRoughnessCutoff = Defaults::SSRRoughnessCutoff;

        // Tonemapパス: SceneColor(SSR有効時はm_SSRTexture)のHDR値をReinhardトーンマッピング+
        // ガンマ補正でLDRへ変換し、Presentパスへ渡す。SSR等のHDR演算より後、Present直前の
        // 独立したステージとして置くことで、反射や将来のブルーム/露出制御(M7)がトーンマップの
        // 影響を受けないHDR値の上に成立できるようにする
        std::unique_ptr<RHI::IRHIShader> m_TonemapVertexShader;
        std::unique_ptr<RHI::IRHIShader> m_TonemapPixelShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_TonemapPipelineState;
        std::unique_ptr<RHI::IRHITexture> m_TonemapTexture;
        std::unique_ptr<RHI::IRHIBuffer> m_TonemapConstantBuffer;

        // トーンマッピングカーブ。Tonemap.hlsl側のCurveと値を一致させること
        enum class TonemapCurve
        {
            Reinhard, // c/(c+1)。M7以前の唯一のカーブで、比較用リファレンスとして残す
            ACES,     // Narkowicz 2015のフィット近似
            AgX,      // Troy Sobotka の AgX(Filament/three.jsの実装形)
        };
        // 既定をAgXにしている理由: ACESは飽和した明るい色の色相がシフトする(赤がオレンジへ寄る)
        // ことが知られており、Bistro内観のように赤い壁が支配的なシーンでその欠点が最も出やすい。
        // AgXはハイライトが色相を保ったまま白へ脱色するため、この用途では素直な絵になる
        TonemapCurve m_TonemapCurve = TonemapCurve::AgX;

        // 薄明視(mesopic vision)の適用量。0で無効、1で完全適用。
        //
        // 暗所では錐体が働かなくなり桿体だけの視覚に移る。桿体は1種類しか無いので色を
        // 判別できず、実際の月明かりの下では「形は見えるのに色がほとんど無い」見え方になる。
        // 露出を下げるだけでは「暗いが色鮮やかな夜」にしかならず、肉眼で見た夜と一致しない。
        // 桿体の分光感度が短波長寄り(507nm)であることから来るプルキンエ現象も同時に入る
        // (詳細はTonemap.hlsl の ApplyMesopicVision)。
        // 既定は無効。効果が強く画作りの好みが分かれるため、使うときに明示的に上げる
        float m_MesopicStrength = Defaults::MesopicStrength;

        // 出力8bit量子化の直前に加えるディザリング。実測(Bistro Interior)では走査線上に
        // 同一色が24px連続しており、これは中間バッファをHDR化しても変わらなかった。
        // つまり暗部のバンディングの主因は最終8bit量子化であり、ここでしか直せない。
        // 効果をA/B比較できるようトグルにしてある
        bool m_DitherEnabled = Defaults::DitherEnabled;

        // 自動露出(eye adaptation)パス: SceneColorの輝度ヒストグラムをGPUで作り、
        // 低/高パーセンタイルを除外した加重平均から目標EV100を求めて時間方向に追従させる。
        // 結果はm_ExposureTextureへ書かれ、Tonemapパスが読んで露出倍率に変換する。
        //
        // 露出そのものはCPU側でライト強度へ事前乗算されている(プリ露出方式、
        // m_SceneExposureEV100)。自動露出の結果をライト強度へ戻すとフィードバックループになり、
        // かつGPU→CPUのリードバック(同期待ち)が要るため、プリ露出は固定のままにして
        // 「プリ露出EVと自動露出EVの差」だけをTonemapで掛ける構成にしている
        // (詳細はAutoExposure.hlsl冒頭)
        std::unique_ptr<RHI::IRHIShader> m_AutoExposureClearComputeShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_AutoExposureClearPipelineState;
        std::unique_ptr<RHI::IRHIShader> m_AutoExposureHistogramComputeShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_AutoExposureHistogramPipelineState;
        std::unique_ptr<RHI::IRHIShader> m_AutoExposureResolveComputeShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_AutoExposureResolvePipelineState;
        std::unique_ptr<RHI::IRHIBuffer> m_ExposureHistogramBuffer;
        std::unique_ptr<RHI::IRHIBuffer> m_AutoExposureConstantBuffer;
        // 2x1のR32_Float。texel(0,0)=平滑化後のEV100、texel(1,0)=初期化済みフラグ。
        // フレームをまたいで保持する必要があるためCreateRenderTargetsではなく一度だけ作る
        // (ウィンドウリサイズで作り直すと順応がリセットされてしまうため)
        std::unique_ptr<RHI::IRHITexture> m_ExposureTexture;
        // 輝度ヒストグラムのビン数。AutoExposure.hlslのHISTOGRAM_BINSと一致させること
        static constexpr uint32_t kExposureHistogramBins = 256;

        bool m_AutoExposureEnabled = Defaults::AutoExposureEnabled;
        // 露出のクランプ範囲(EV100)。ヒストグラムのビン割りもこの範囲で行うため、
        // 実シーンの輝度がこの外に出ると端に張り付く
        // 下限-6は月夜の地表(反射率0.2の面で約0.016 cd/m^2 = EV100約-3)を余裕をもって含む値。
        // 星明かりだけの夜まで追うならさらに下げる必要があるが、実写の夜景もEV -3〜-5程度で
        // 撮るのが普通なので実用上はここで足りる。
        // 上限18は、正規化後の昼の空(約6400 cd/m^2 = EV100約15.6)に余裕を持たせた値
        float m_AutoExposureMinEV100 = Defaults::AutoExposureMinEV100;
        float m_AutoExposureMaxEV100 = Defaults::AutoExposureMaxEV100;
        // 明順応(暗→明)と暗順応(明→暗)の速度。人間の目は暗順応のほうが遅いため既定値も分けている
        float m_AutoExposureSpeedUp = Defaults::AutoExposureSpeedUp;
        float m_AutoExposureSpeedDown = Defaults::AutoExposureSpeedDown;
        // 加重平均から除外する下側/上側の累積割合。暗すぎる画素・明るすぎる画素に露出が
        // 引きずられるのを防ぐ
        float m_AutoExposureLowPercentile = Defaults::AutoExposureLowPercentile;
        float m_AutoExposureHighPercentile = Defaults::AutoExposureHighPercentile;
        // 測定結果に対してユーザーが意図的に足すオフセット(EV)
        float m_AutoExposureCompensation = Defaults::AutoExposureCompensation;
        // 暗いシーンをわざと暗いまま写すための補正量[EV]。
        // 自動露出は測ったものを中庸なグレーへ持ち上げるので、これが0だと夜が昼と同じ明るさで
        // 出てしまう(実測: 補正なしでは22時と12時の空の明度がほぼ一致していた)。
        // 実写でも夜景はわざと露出を切り詰めて撮るため、既定で4.5段暗くする。
        // 既定値は「肉眼で見た月明かりの夜」に合わせて実測で決めた
        // (m_MesopicStrength=1のときの、月光を受ける壁 / 夜空の8bitコード):
        //   3.5段 … 壁19 / 空70  形も質感もはっきり読め、夜というより夕暮れ寄り
        //   4.5段 … 壁 6 / 空44  空が一番明るく、建物は輪郭と影がかろうじて読める ← 既定
        //   5.5段 … 壁 2 / 空23  建物がほぼ完全に沈み、空しか見えない
        //
        // **m_MesopicStrengthとセットで意味を持つ**点に注意。露出を下げるだけでは
        // 「暗いが色鮮やかな夜」にしかならず、肉眼で見た夜と一致しない。
        //
        // 参考: 旧既定(薄明視を入れる前)は次の値だった。
        // 満月に照らされた石壁を、空が画面の約40%を占める屋上視点と、空が入らない構図の
        // 両方で測った8bitコード(m_AutoExposureKeyCeilingEV=-1のとき):
        //   3.0段 … 壁22   / 3.5段 … 壁15/13 / 4.0段 … 壁9/8
        // 0にすると従来どおり「常に中庸なグレーへ合わせる」挙動に戻る。
        //
        // **m_AutoExposureKeyCeilingEVとセットで意味を持つ値**である点に注意。
        // 上のクランプが無いと測光値が構図で2〜3.5段振れるので、この値をいくつにしても
        // カメラの向きで夜の明るさが変わってしまう
        float m_AutoExposureNightRolloffEV = Defaults::AutoExposureNightRolloffEV;
        // 補正カーブの折れ点[EV100]。測定値がDark以下で補正量が最大、Bright以上で0、間は線形。
        // Darkの-2は満月の夜の地表(反射率0.2の面で約0.016 cd/m^2 = EV100約-3)のすぐ上、
        // Brightの10は曇天の屋外あたりで、日中は補正が掛からない値にしてある
        float m_AutoExposureNightRolloffDarkEV100 = Defaults::AutoExposureNightRolloffDarkEV100;
        float m_AutoExposureNightRolloffBrightEV100 = Defaults::AutoExposureNightRolloffBrightEV100;
        // 測光値がキー照度の基準EV(ComputeReferenceEV100。構図に依存しない)から
        // 何段上まで行くのを許すか[EV]。十分大きな値(16など)で無効になる。
        //
        // 【位置づけ】構図で露出が振れる問題そのものは、AutoExposure.hlslで
        // **空を測光から外した**ことで根本的に解決している(21.9.8節)。
        // こちらは残った病的なケースへの保険で、通常は発動しない:
        // 夜の街で明るい看板が画面の大半を占めるようなとき、明るい側に寄った測光範囲
        // (50〜95パーセンタイル)がその看板に支配され、街並みが黒く沈むのを防ぐ。
        //
        // **上側だけを止める**のは、屋内のように実際の輝度が屋外のキー照度よりずっと低い
        // シーンでは測光値が下へ振れるのが正しいため(両側を締めると屋内が真っ暗になる)。
        // 下側はm_AutoExposureMinEV100が絶対的な下限として効く。
        //
        // 既定の+2は「通常のシーンでは発動しないが、極端なケースは止まる」余裕を見た値。
        // 空除外の前は-1にしていたが、空を外した今それでは締めすぎになる
        // (夜の壁が3.8から4.7へ持ち上がってしまう)
        float m_AutoExposureKeyCeilingEV = Defaults::AutoExposureKeyCeilingEV;

        // ブルームパス(Bloom.hlsl): 半解像度から始まるピラミッドを段階的にダウンサンプルし、
        // 3x3テントで戻しながら加算することで広く滑らかな光の裾を作る。
        //
        // ピラミッドをミップチェーン1枚ではなくレベルごとの独立テクスチャで持っているのは、
        // 同一リソースのSRV/UAV同時バインドを避けるため(理由の詳細はBloom.hlsl冒頭)。
        // m_BloomDownTexturesがダウンサンプル結果、m_BloomUpTexturesがアップサンプルの累積で、
        // 最終的にm_BloomUpTextures[0](半解像度)をTonemapパスが読む
        std::unique_ptr<RHI::IRHIShader> m_BloomDownsampleComputeShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_BloomDownsamplePipelineState;
        std::unique_ptr<RHI::IRHIShader> m_BloomUpsampleComputeShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_BloomUpsamplePipelineState;
        std::unique_ptr<RHI::IRHIBuffer> m_BloomConstantBuffer;
        std::vector<std::unique_ptr<RHI::IRHITexture>> m_BloomDownTextures;
        std::vector<std::unique_ptr<RHI::IRHITexture>> m_BloomUpTextures;
        // ピラミッドの段数。半解像度を第0段として、これ以上小さくしても見た目が変わらない範囲で選ぶ
        static constexpr uint32_t kBloomLevelCount = 6;
        // 各段の解像度(CreateRenderTargetsで内部解像度から決まる)
        std::vector<DirectX::XMUINT2> m_BloomLevelSizes;

        bool m_BloomEnabled = Defaults::BloomEnabled;
        // 最終合成の混合比。エネルギー保存のため加算ではなくlerpで混ぜるので、
        // 物理的にレンズ散乱が持ち去る割合(数%)に相当する小さい値が既定になる
        float m_BloomStrength = Defaults::BloomStrength;
        // しきい値は既定で十分低くしてある(物理的にはブルームは全輝度に掛かるのが正しい)。
        // アート制御として上げられるようにだけしてある
        float m_BloomThreshold = Defaults::BloomThreshold;
        float m_BloomSoftKnee = Defaults::BloomSoftKnee;

        // 垂直同期。既定で無効。有効にするとPresentがvblankまでブロックするため、GPU負荷が軽い
        // シーンではvsync待ちの間GPUがアイドル→省電力クロックに落ち、次フレームの立ち上がりが
        // 遅くなる・待ち時間自体もジッタで1vblank/2vblank分を行き来するなど計測値が不安定になる。
        // 既定はGPU/CPU双方の実処理時間を素直に見られるOFFとし、ティアリングを許容する
        // (ON時はPresentが即座に返らず、モニタのリフレッシュレートにFPSが制限される)
        bool m_VSyncEnabled = Defaults::VSyncEnabled;

        // 固定FPSモード。有効時、Renderスレッドが目標FPSより速く回った分だけ待機してフレーム間隔を
        // 一定に保つ。VSyncはモニタのリフレッシュレート依存かつティアリング防止が目的だが、こちらは
        // 任意のFPS値に固定できる(物理更新の再現性確保や環境間でのフレーム時間比較などが目的)。
        // 既定で60fps固定を有効にする
        bool m_FixedFPSEnabled = Defaults::FixedFPSEnabled;
        float m_TargetFPS = Defaults::TargetFPS;

        // Presentパス(選択中のレンダーターゲットをアスペクト比を保ってバックバッファへ拡大縮小表示)
        std::unique_ptr<RHI::IRHIShader> m_PresentVertexShader;
        std::unique_ptr<RHI::IRHIShader> m_PresentPixelShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_PresentPipelineState;
        std::unique_ptr<RHI::IRHIBuffer> m_PresentConstantBuffer;

        // デバッグ表示用: Presentパスで最終的に表示するレンダーターゲットの種類
        enum class DebugView
        {
            Final,
            Albedo,
            Normal,
            Material,
            Emissive,
            Depth,
            DepthRaw,           // 深度テクスチャの生値(0〜1)を加工せずそのままグレースケール表示
            DirectLight,        // DirectLightingパスの結果(HDR、シャドウ適用済みの直接光)をトーンマッピングして表示
            AOIndirectLight,    // AO/GIバッファのrgb(間接拡散光、ブラー後)をそのまま表示
            AOIndirectLightRaw, // AO/GIバッファのrgb(間接拡散光、ブラー前の生値)
            AOOcclusion,        // AO/GIバッファのa(遮蔽率、ブラー後)をグレースケール表示
            AOOcclusionRaw,     // AO/GIバッファのa(遮蔽率、ブラー前の生値)
            ShadowMap,          // m_ShadowDebugCascadeで選択したカスケードのシャドウマップを表示
            SSR,                // SSRパスの出力(SceneColor+反射)。SSR無効時はSceneColorと同一
            HiZ,                // Hi-Zミップチェーンの指定ミップ(m_HiZDebugMipLevel)をグレースケール表示
            IBLIrradiance,      // IBL拡散イラディアンスマップ(TextureCube。現在の視線方向で球面を見回す表示)
            IBLPrefilter,       // IBLプリフィルタ済み鏡面マップの指定ミップ(m_IBLPrefilterDebugMipLevel、TextureCube)
            IBLBRDFLUT,         // IBL BRDF積分LUT(x=NdotV, y=ラフネス)
            Bloom,              // ブルームのピラミッド最上段(半解像度、HDR)をトーンマッピングして表示
            LightTiles,         // タイルライトカリングのライトグリッド(タイルあたりのライト数)をヒートマップ表示
            ProbeIrradiance,    // 反射プローブの拡散イラディアンス(m_ProbeDebugIndex番のプローブ)
            ProbePrefilter,     // 反射プローブのプリフィルタ済み鏡面(ミップ0がキャプチャ結果そのもの)
            ProbeInfluence,     // どのプローブが効いているかをプローブ番号ごとの色で塗り分けて表示
        };
        DebugView m_DebugView = DebugView::Final;
        // デバッグ表示の輝度倍率(Present.hlslのGain)。AO/GIバッファの間接拡散光のように
        // 値そのものが小さいバッファ(この暗い室内では0.02〜0.1程度)は、等倍で表示しても
        // ほぼ真っ黒で階調の粗さが判別できない。持ち上げて表示することで、8bit格納時の
        // ポスタリゼーションが何段あるかを目視で確認できるようにする。
        // 色として表示するモード(Present.hlsl Mode 0/3/4)にのみ効く
        float m_DebugViewGain = Defaults::DebugViewGain;

        // シャドウパス(平行光のライト視点から深度のみを描画する)。カメラ視錐台をkCascadeCount個の
        // 深度範囲に分割し(Practical Split Scheme)、それぞれ専用の正射影・シャドウマップを持たせる
        // カスケードシャドウマップ(CSM)。近いカスケードほどテクセル密度が高く、遠いカスケードほど
        // 広い範囲を粗くカバーする
        static constexpr uint32_t kShadowMapSize = 2048;
        std::unique_ptr<RHI::IRHIShader> m_ShadowVertexShader;
        std::unique_ptr<RHI::IRHIShader> m_ShadowPixelShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_ShadowPipelineState;
        std::unique_ptr<RHI::IRHIPipelineState> m_ShadowPipelineStateMirrored;
        // 全カスケードの深度を1つのTexture2DArray(スライス番号=カスケード番号)として保持する。
        // 書き込みはスライスごとの個別DSV(RenderGraphPassDesc::DepthTargetArraySlice)で行い、
        // 読み取りは配列全体を指す1本のSRV(t4)を1回バインドするだけでよい。シェーダ側は
        // ShadowMapArray.Sample(DataSampler, float3(uv, cascadeIndex))で動的にカスケードを選べる
        // (ShadowSampling.hlsli参照)
        std::unique_ptr<RHI::IRHITexture> m_ShadowCascadeArray;
        // シャドウパスの各カスケード描画で使う専用の定数バッファ(カスケードごとに値を更新して使い回す)
        std::unique_ptr<RHI::IRHIBuffer> m_ShadowCascadeConstantBuffer;
        bool m_ShadowEnabled = Defaults::ShadowEnabled;
        // PCSS(Percentage Closer Soft Shadows)のライトサイズ。シャドウマップUV空間での
        // ブロッカーサーチ・半影の広さを決める係数(値が大きいほど半影が広く柔らかくなる)
        float m_ShadowLightSize = Defaults::ShadowLightSize;
        // デバッグ表示(Render Targets - Shadow Map)で確認するカスケード番号(0=カメラに近い方)
        int32_t m_ShadowDebugCascade = 0;

        // 太陽(平行光)そのものの有効/無効。.ksceneの[Sun]Enabledで設定される。
        // TimeOfDayを夜にすると昼度(AmbientColor.a)も一緒に落ちて環境光まで消えてしまうため、
        // 「昼のまま太陽だけ消す」にはこちらを使う(White Furnace Testが必要とする)。
        // 無効時はFrameConstants.LightColorをゼロにするだけでよく、シェーダー側の変更は不要
        bool m_SunEnabled = Defaults::SunEnabled;

        // 背景(深度が書き込まれなかったピクセル)に表示する空のキューブマップ。
        // .ksceneの[Scene]Skyboxでシーンごとに差し替えられる(LoadScene参照)
        std::unique_ptr<RHI::IRHITexture> m_SkyboxTexture;
        // 既定のスカイボックス(Assets/Skybox/Sky.dds)の絶対パス。[Scene]Skybox指定が無いシーンへ
        // 切り替えたときはここへ戻す
        std::wstring m_DefaultSkyboxPath;
        // 現在m_SkyboxTextureへ読み込んでいるファイルの絶対パス。シーン切り替えのたびに
        // 読み直さずに済むよう比較に使う
        std::wstring m_CurrentSkyboxPath;

        // IBL(Image Based Lighting): m_SkyboxTextureから拡散イラディアンス・プリフィルタ済み鏡面・
        // BRDF積分LUTの3つをコンピュートシェーダーで畳み込む(split-sum近似、Karis 2013)。
        // スカイボックスは実行時に変化しない静的アセットのため、起動後最初のRender()で一度だけ
        // 焼いてm_IBLBakedを立て、以降は焼き直さない(詳細はdocs/Architecture.html参照)。
        // 拡散イラディアンス・プリフィルタ済み鏡面はいずれも本物のTextureCube
        // (CreateUAVTextureCube/CreateMippedUAVTextureCube、面ごとに個別のUAVを持つ)で、
        // IBLConvolve.hlslが面ごとに1回ずつディスパッチして書き込む
        // キューブマップの面数(D3D標準順: +X,-X,+Y,-Y,+Z,-Z)。IBLの2つのキューブマップは
        // いずれもこの順で面ごとにディスパッチする(IBLConvolve.hlsl CubeFaceDirectionと一致させる)
        static constexpr uint32_t kCubeFaceCount = 6;
        static constexpr uint32_t kIBLIrradianceSize = 32;
        static constexpr uint32_t kIBLPrefilterBaseSize = 128;
        // プリフィルタ済み鏡面マップのミップ数(128,64,32,16,8,4の6段)。ラフネス[0,1]を
        // [0, kIBLPrefilterMipLevels-1]のミップ番号へ線形マッピングする(DeferredLighting.hlsl参照)
        static constexpr uint32_t kIBLPrefilterMipLevels = 6;
        static constexpr uint32_t kIBLBRDFLUTSize = 128;
        // 手続き空(SkyGenerate.hlsl): Perez分布をGPUで評価してキューブマップを生成する。
        // オフラインで焼いたDDS(Sky.dds)と違い、太陽が動くと空の輝度分布の「形」も追従する
        // (circumsolarの明るい領域が太陽と一緒に動く)。詳細はSkyGenerate.hlsl冒頭。
        //
        // .ksceneで[Scene]Skyboxを明示しているシーン(White Furnace TestのUniformWhite.dds)は
        // 従来どおりDDSを使う必要があるため、手続き空は別テクスチャに持ち、
        // ActiveSkyTexture()がフレームごとにどちらを使うか決める
        static constexpr uint32_t kProceduralSkySize = 256;
        std::unique_ptr<RHI::IRHITexture> m_ProceduralSkyTexture;
        std::unique_ptr<RHI::IRHIShader> m_SkyGenerateComputeShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_SkyGeneratePipelineState;
        // SkyGenerate用の専用定数バッファ。m_IBLPrefilterConstantBufferと共用しないこと
        // (UpdateBuffer→SetComputeConstantBufferの順序制約があり、共用すると事故りやすい。
        //  詳細はRHI/IRHICommandList.hのSetConstantBufferのコメント)
        std::unique_ptr<RHI::IRHIBuffer> m_SkyBakeConstantBuffer;
        bool m_ProceduralSkyEnabled = Defaults::ProceduralSkyEnabled;
        // 手続き空を焼き直す必要があるか。太陽が動いたとき等に立てる
        bool m_SkyBakeDirty = true;
        // 最後に焼いたときの太陽の向き。これと現在の向きの角度差が閾値を超えたら焼き直す。
        // 毎フレーム焼くと空生成6回+プリフィルタ36回が常時走って無駄なため
        DirectX::XMFLOAT3 m_LastBakedSunPosition{ 0.0f, 0.0f, 0.0f };
        // 最後に焼いたときの実効プリ露出。空はプリ露出済みの値で焼かれるため、
        // 露出が動いたときも焼き直さないと空だけ古い露出のまま取り残される
        float m_LastBakedExposureEV100 = 0.0f;
        // 焼き直しの角度閾値(度)。Auto Advance既定(1h/s)では太陽は15度/秒動くので、
        // 1.0度なら毎秒15回の焼き直しになる。空の見た目は15Hz更新でも連続に見える
        float m_SkyBakeAngleThresholdDegrees = 1.0f;

        bool m_IBLBaked = false;
        // BRDF積分LUTを焼き終えたか(m_IBLBakedとは別管理)。このLUTは(NdotV, ラフネス)の
        // 2Dテーブルでスカイボックスにも太陽の位置にも一切依存しないため、起動後に一度焼けば
        // 二度と焼き直す必要がない。プリフィルタ済み鏡面が空の変化に追従して再ベイクされるように
        // なった以降も巻き込まれて焼き直されないよう、専用のフラグとパスに分離してある
        // (128x128 x 1024サンプル = 約1,680万イテレーションあり、毎回焼くと丸損になる)
        bool m_BRDFLUTBaked = false;
        // 検証用の拡散イラディアンスマップを焼き終えたか(m_IBLBakedとは別管理)。既定の描画経路は
        // プリフィルタ済み鏡面の最終ミップなので、こちらは検証を有効にしたときにだけ焼く
        bool m_IBLIrradianceBaked = false;
        std::unique_ptr<RHI::IRHITexture> m_IrradianceTexture;
        std::unique_ptr<RHI::IRHITexture> m_PrefilteredEnvTexture;
        std::unique_ptr<RHI::IRHITexture> m_BRDFLUTTexture;
        std::unique_ptr<RHI::IRHIShader> m_BRDFLUTComputeShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_BRDFLUTPipelineState;
        std::unique_ptr<RHI::IRHIShader> m_IrradianceComputeShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_IrradiancePipelineState;
        std::unique_ptr<RHI::IRHIShader> m_PrefilterComputeShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_PrefilterPipelineState;
        // プリフィルタ済み鏡面のミップごとの畳み込みで使うラフネス値を渡す専用の定数バッファ
        std::unique_ptr<RHI::IRHIBuffer> m_IBLPrefilterConstantBuffer;
        // デバッグ表示(Render Targets)で確認するプリフィルタ済み鏡面マップのミップレベル
        int32_t m_IBLPrefilterDebugMipLevel = 0;
        // IBL(拡散イラディアンス+プリフィルタ済み鏡面)のON/OFFと強度。無効時はシェーダ側
        // (DeferredLighting.hlsl)でEvaluateIBLの代わりにIBL導入以前と同じ定数色アンビエント
        // (AmbientColor.rgb)へフォールバックする(真っ暗にはしない)。既定値を1.0でなく0.5に
        // しているのは、スカイボックスの空の輝度分布を明るく補正した(14.6節)結果、既定の
        // Perez分布そのままではIBL全体の寄与が強すぎたため(実機で指摘された見た目の問題)
        bool m_IBLEnabled = Defaults::IBLEnabled;
        float m_IBLIntensity = Defaults::IBLIntensity;
        // 拡散イラディアンスを専用マップ(m_IrradianceTexture)から取るかどうか。既定はfalseで、
        // プリフィルタ済み鏡面の最終ミップ(roughness=1)を使う。CSPrefilterがV=R=Nを仮定して
        // いるためroughness=1ではGGXの実効カーネルがコサイン畳み込みへ厳密に退化し、両者は同じ
        // E(N)/πを格納する(14.10節)。White Furnace Testで画素一致、実スカイボックスでも
        // 最大2〜4/255の差しか出ないことを実機で確認したうえで専用マップを既定経路から外した。
        // これによりリフレクションプローブのような実行時のキューブマップ焼き直しから、最も重い
        // CSIrradiance(約9750万サンプル)を丸ごと省ける。
        // 畳み込み処理自体はいつでも検証できるよう残してあり、このトグルをONにすると
        // その場で焼いて(m_IBLIrradianceBaked)従来経路に切り替わる
        bool m_IBLUseDedicatedIrradiance = Defaults::IBLUseDedicatedIrradiance;
        // スペキュラBRDFのmultiple-scattering energy compensation(Kulla & Conty 2017)のON/OFF。
        // IBL鏡面・直接光鏡面の両方に効くため、Enable IBLとは独立したトグルにしている。
        // FrameConstants.ShadowParams.wへ1.0f/0.0fとして渡し、3つのシェーダー(DirectLighting/
        // DeferredLighting/Transparent)が共有するSpecularEnergy.hlsliの
        // SpecularEnergyCompensationがこれを見て倍率1.0へ落とす。
        // 既定でONにしているのは、補正しない状態がエネルギー的に不正(粗い面ほど暗い)であり、
        // OFFは実装検証・A/B比較のための選択肢という位置付けのため(14.9節)
        bool m_SpecularEnergyCompensationEnabled = Defaults::SpecularEnergyCompensationEnabled;
        // Enable IBL無効時に使う定数色アンビエントフォールバックの強度倍率。シェーダ側ではなく
        // Render()がFrameConstants.AmbientColorへ書き込む時点でrgb(alphaのdayFactorは除く)に
        // 乗算する(HLSL側は素のAmbientColor.rgbを読むだけでよい)
        float m_AmbientScale = Defaults::AmbientScale;

        // シーン全体の自発光(エミッシブ)の強度倍率。MakeObjectConstantsがmesh.EmissiveFactorへ
        // 乗算する。glTFのemissiveFactorは通常1.0以下に収まるため、G-Bufferのエミッシブを
        // HDR化(R11G11B10_Float)しただけでは照明器具の輝度が1.0を超えず、ブルームが効かない。
        // アセットを再オーサリングせずにHDRな自発光を得るための倍率
        // (KHR_materials_emissive_strengthをインポータが読むようになれば本来はそちらが正しい)
        float m_EmissiveIntensity = Defaults::EmissiveIntensity;

        // 反射プローブ(19章): プローブ位置から6方向をProbeCapture.hlslで2Dレンダーターゲットへ描き、
        // IBLConvolve.hlsl CSCopyCaptureToCubeFaceでスクラッチのキューブマップへ組み上げてから、
        // IBLと同じCSIrradiance/CSPrefilterで畳み込んでプローブごとのキューブマップ配列へ書き込む。
        // 環境ソースを差し替えるだけなので、シェーダー側の評価式(EvaluateIBL)はIBLと完全に共通。
        //
        // キューブマップ配列の枚数上限。TextureCubeArrayは実行時に伸縮できないため固定容量で確保し、
        // これを超えるプローブが置かれたシーンは先頭からこの数だけを採用する(警告ログを出す)
        static constexpr uint32_t kMaxReflectionProbes = 8;
        // キャプチャ解像度。プリフィルタ済み鏡面のベース解像度(kIBLPrefilterBaseSize)と揃えることで、
        // ミップ0が「畳み込み無しのキャプチャそのもの」になりデバッグ表示で生の映り込みを確認できる
        static constexpr uint32_t kProbeCaptureSize = kIBLPrefilterBaseSize;
        std::unique_ptr<RHI::IRHIShader> m_ProbeCaptureVertexShader;
        std::unique_ptr<RHI::IRHIShader> m_ProbeCapturePixelShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_ProbeCapturePipelineState;
        // 1面ぶんのキャプチャ先(6面で使い回す)。HDRのままキューブへ写すためG-Bufferと違いFloat
        std::unique_ptr<RHI::IRHITexture> m_ProbeCaptureColor;
        std::unique_ptr<RHI::IRHITexture> m_ProbeCaptureDepth;
        std::unique_ptr<RHI::IRHIShader> m_ProbeCubeCopyComputeShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_ProbeCubeCopyPipelineState;
        // キャプチャした6面を組み上げるスクラッチのキューブマップ(単一キューブ)。畳み込みの入力に
        // なるためTextureCubeArrayではなくTextureCubeである必要がある(IBLConvolve.hlslのSourceSkyboxは
        // TextureCube宣言のまま。これによりIBLの畳み込みシェーダーを一切変更せず再利用できる)。
        // プローブは1つずつ順に焼くため1枚で足りる
        std::unique_ptr<RHI::IRHITexture> m_ProbeRadianceCube;
        // 畳み込み結果(プローブごと)。DeferredLighting.hlslがTextureCubeArrayとして読む
        std::unique_ptr<RHI::IRHITexture> m_ProbeIrradianceArray;
        std::unique_ptr<RHI::IRHITexture> m_ProbePrefilteredArray;
        // プローブの影響範囲(位置・半径)をシェーダーへ渡すStructuredBuffer(t13)
        std::unique_ptr<RHI::IRHIBuffer> m_ProbeBuffer;
        // キャプチャの面ごとに値を更新して使い回すFrameConstants(共有のm_FrameConstantBufferとは別。
        // ViewProj/CameraPositionだけをプローブのものへ差し替える。詳細はProbeCapture.hlsl冒頭)
        std::unique_ptr<RHI::IRHIBuffer> m_ProbeCaptureConstantBuffer;
        // LoadSceneがm_Scene.ReflectionProbesからコピーし、以降ImGuiが編集する(m_Lightsと同じ方針)。
        // m_SceneMutexで保護される
        std::vector<Assets::ReflectionProbe> m_ReflectionProbes;
        int m_SelectedProbeIndex = -1;
        // 次のRender()でプローブを焼き直す要求。シーン読み込み時とImGuiのBakeボタンで立てる。
        // スカイボックス由来のIBLと違いシーンのジオメトリ・ライトに依存するため、
        // 「一度焼いたら二度と焼かない」ではなく明示的な要求ベースにしている
        bool m_ProbeBakeRequested = false;
        // 一度でも焼けたか。焼く前のプローブは中身が未定義なので、それまでは影響を無効にして
        // グローバルIBLのまま描く(未初期化のキューブマップが映り込むのを防ぐ)
        bool m_ProbeBaked = false;
        bool m_ReflectionProbeEnabled = Defaults::ReflectionProbeEnabled;
        // 視差補正(box projection)を行うか。Box形状のプローブにのみ効く。無効にすると
        // 反射ベクトルをそのまま引くPhase 1相当の挙動になり、壁際で反射位置がずれるのを確認できる
        bool m_ProbeParallaxCorrectionEnabled = Defaults::ProbeParallaxCorrectionEnabled;
        // プローブ間・プローブとグローバルIBLの重み付きブレンドを行うか。無効にすると
        // 「影響範囲に入る最も近い1つだけを使う」Phase 1相当の挙動になり、境界の継ぎ目を確認できる
        bool m_ProbeBlendingEnabled = Defaults::ProbeBlendingEnabled;
        // デバッグ表示(Render Targets)で確認するプローブ番号とプリフィルタのミップレベル
        int32_t m_ProbeDebugIndex = 0;
        int32_t m_ProbePrefilterDebugMipLevel = 0;

        // プローブの更新モード。焼き直しのコストと「シーンの変化への追従」のどちらを取るかの選択で、
        // ImGuiで切り替えて負荷と品質を比較できるようにしてある(19.10節)
        enum class ProbeUpdateMode
        {
            // シーン読み込み時とImGuiのBakeボタンのときだけ焼く。実行時コストはゼロだが、
            // ライトや時刻を動かしても反射は焼いた時点のまま止まる
            Baked,
            // 上に加えて、焼き上がりに影響する状態(時刻・太陽・ライト)の変化を検出して自動で焼き直す。
            // 変化していないフレームのコストはゼロだが、変化したフレームは全プローブぶんの
            // フルベイクが1フレームに集中する
            OnDemand,
            // 上に加えて、毎フレーム1面ずつ焼き直す。6面揃った時点でそのプローブを畳み込み、
            // 次のプローブへ回る(ラウンドロビン)。全プローブを毎フレーム焼くとドローコールが
            // プローブ数×6倍になり非現実的なため、時間分割を既定の実装方式にしている
            Realtime,
        };
        ProbeUpdateMode m_ProbeUpdateMode = ProbeUpdateMode::Baked;
        // Realtimeの進行状態。次に焼くプローブ番号と面番号
        uint32_t m_ProbeRealtimeProbeIndex = 0;
        uint32_t m_ProbeRealtimeFace = 0;
        // OnDemandの変化検出用。最後にフルベイクを発行した時点の状態の署名。
        // 毎フレームの署名と突き合わせ、変わっていれば焼き直しを要求する
        uint64_t m_ProbeBakeSignature = 0;
        // 焼き上がりに影響する状態(時刻・太陽・シャドウ・IBL強度・全ライト)から署名を作る。
        // 影響範囲(形状・半径・ブレンド距離)はキャプチャ内容を変えないため含めない
        uint64_t ComputeProbeBakeSignature() const;

        // 昼夜サイクル: ImGuiで操作する時刻(0〜24時)。太陽の向き・色・環境光・空の明るさに反映される
        float m_TimeOfDay = Defaults::TimeOfDay;
        bool m_TimeAutoAdvance = Defaults::TimeAutoAdvance;
        float m_TimeAdvanceSpeed = Defaults::TimeAdvanceSpeed; // 自動進行時、1秒あたりに進む時間(時)

        // 太陽が昇ってくる方位角(度)。X軸を0度、Z軸(+方向)を90度とした水平面上の角度で、
        // ImGuiで調整する(ComputeSunLightingが太陽の日の出側水平方向として使用する)
        float m_SunAzimuthDegrees = Defaults::SunAzimuthDegrees;

        // 月の位置。**時刻には連動せず、ここで指定した固定位置に居続ける**。
        // 実際の月は太陽とは独立した周期(朔望月)で動くので、反太陽方向に固定するのは
        // 「常に満月かつ常に真夜中に南中する」という二重の簡略化になってしまう。
        // 任意の月齢・任意の時刻の見え方を作れるよう、位置は手動指定にしている。
        // 方位角の規約は太陽と同じ(X軸が0度、Z軸(+方向)が90度)。
        // 仰角が0度以下なら月は地平線下にあり、月光は出ない。
        // シーンを切り替えても引き継がれる(.ksceneのキーは持たない)
        float m_MoonAzimuthDegrees = Defaults::MoonAzimuthDegrees;
        float m_MoonElevationDegrees = Defaults::MoonElevationDegrees;

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

        // ポイント/スポットライトのリスト(t8、StructuredReadOnly)と、有効ライト数を渡すb1。
        // 太陽(平行光)はb0のLightDirection/LightColorのまま(詳細はdocs/Architecture.html参照)
        std::unique_ptr<RHI::IRHIBuffer> m_LightBuffer;
        std::unique_ptr<RHI::IRHIBuffer> m_LightingConstantBuffer;
        // 容量(kMaxLights)超過を検出した最初のフレームだけ警告ログを出すためのフラグ
        bool m_LightOverflowLogged = false;

        // ポイント/スポットライトのスクリーンスペースシャドウ(接触影)の設定。
        // シャドウマップを増やさず、G-Bufferの深度バッファをライト方向へレイマーチして影を出す
        // (Shaders/3D/ScreenSpaceShadow.hlsli、docs/Architecture.html 18章)
        bool m_ScreenSpaceShadowEnabled = Defaults::ScreenSpaceShadowEnabled;
        // レイマーチのステップ数。ScreenSpaceShadow.hlsliのkSSSMaxStepCount(64)が上限
        int m_ScreenSpaceShadowStepCount = Defaults::ScreenSpaceShadowStepCount;
        // 1本のレイが伸びる最大のワールド距離。ライトまでの距離がこれより短ければそちらが優先される。
        // 短いほど「接触影」寄りになり、コストも下がる
        float m_ScreenSpaceShadowMaxRayLength = Defaults::ScreenSpaceShadowMaxRayLength;
        // 遮蔽と判定する深度差の上限。深度バッファがサーフェスの厚みを持たないための近似で、
        // 大きすぎると遠景が無限に厚い遮蔽物として振る舞い、小さすぎると薄い物体を貫通する
        float m_ScreenSpaceShadowThickness = Defaults::ScreenSpaceShadowThickness;
        // レイ始点を法線方向へ押し出す量(View空間深度に比例させる係数)。自己遮蔽(シャドウアクネ)対策
        float m_ScreenSpaceShadowNormalBias = Defaults::ScreenSpaceShadowNormalBias;
        // ヒット位置が画面端に近いときに影を弱める幅(UV単位)。SSRのkSSREdgeFadeDistanceと同じ役割
        float m_ScreenSpaceShadowEdgeFade = Defaults::ScreenSpaceShadowEdgeFade;
        // 1ピクセルが撃てるシャドウレイ数の上限。ライトを増やしてもレイマーチのコストが
        // 線形に伸び続けないようにするための予算
        int m_ScreenSpaceShadowMaxLightsPerPixel = Defaults::ScreenSpaceShadowMaxLightsPerPixel;

        // タイルライトカリング(Shaders/3D/LightCulling.hlsl)。画面を16x16ピクセルのタイルに分け、
        // タイルごとに「そのタイルに届くライト」のインデックスリストをコンピュートシェーダーで作る。
        // 直接光パスはそのリストだけをループするため、ピクセルあたりのコストが
        // シーン全体のライト数ではなくタイル内のライト数になる。
        // これは純粋な最適化であり、有効/無効で最終画像が変わってはならない
        // タイルライトカリングのタイルサイズ(1辺のピクセル数)。
        // LightCulling.hlsl の kTileSize および numthreads と必ず一致させること
        static constexpr uint32_t kLightTileSize = 16;
        // 1タイルが保持できるライト数の上限。LightCulling.hlsl の kMaxLightsPerTile および
        // DirectLighting.hlsl の同名の定数と必ず一致させること(バッファのストライドがこの値で決まる)。
        // HLSL側はgroupshared配列のサイズに使うためコンパイル時定数である必要があり、
        // C++からの受け渡しでは代用できないので、3箇所で同じ値を書く形になっている。
        // .cppの無名名前空間ではなくここに置いてあるのは、DebugViewPanelがヒートマップの
        // 上限としてこの値を使うため(UIパネルはfriendなのでprivateのまま参照できる)
        static constexpr uint32_t kLightTileCapacity = 64;
        // ライトグリッド1タイルぶんの要素数(先頭1個がライト数、残りがライトインデックス)
        static constexpr uint32_t kLightTileStride = 1 + kLightTileCapacity;

        std::unique_ptr<RHI::IRHIShader> m_LightCullingComputeShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_LightCullingPipelineState;
        std::unique_ptr<RHI::IRHIBuffer> m_LightCullingConstantBuffer;
        // ライトグリッド本体(BufferUsage::StructuredRW)。コンピュートがUAVで書き、
        // 直接光パスのピクセルシェーダがSRVで読む。解像度に依存するためCreateRenderTargetsで作り直す
        std::unique_ptr<RHI::IRHIBuffer> m_LightTileBuffer;
        uint32_t m_LightTileCountX = 0;
        uint32_t m_LightTileCountY = 0;
        bool m_LightCullingEnabled = Defaults::LightCullingEnabled;
        // タイル容量の超過"条件"(シーンのライト数が容量を超えている)を検出した最初のフレームだけ
        // 警告ログを出すためのフラグ(m_LightOverflowLoggedと同じ作法)。
        // 実際に超過したかはGPU側にしか無いため、確認はDebugView::LightTilesのマゼンタで行う
        bool m_LightTileOverflowLogged = false;
        // DebugView::LightTilesのヒートマップで赤に振り切る基準のライト数。容量(64)を基準にすると
        // 実データ(数灯)ではほぼ真っ青で差が読めないため、別のつまみにしてある
        int m_LightTileHeatmapMax = Defaults::LightTileHeatmapMax;

        // LoadScene(Updateスレッド。UpdateSceneSwitch経由で呼ばれる)が書き込み、Render()(Renderスレッド。
        // 描画そのものに加えUIパネルのスライダーがm_SSAORadius等を直接書き換える)が
        // 読み書きする「シーン状態」一式をこのミューテックスで保護する。LoadScene呼び出し全体と
        // Render()呼び出し全体をそれぞれこのミューテックスで包むため、この2つは同時に走らない
        // (=個々のメンバに追加のロックは不要)。対象はm_Scene/m_CurrentSceneIndex/m_Cameraと、
        // FrameCameraToModelが書き換えるm_SSAORadius等のPost Processingパラメータ、および
        // m_Lights/m_SelectedLightIndex/m_SceneExposureEV100
        // (宣言はそれぞれの節にあるが、書き込み元がLoadScene/ImGuiスライダーの2スレッドにまたがる点は共通)
        std::mutex m_SceneMutex;
        Assets::Scene m_Scene;
        size_t m_CurrentSceneIndex = 0;
        Core::Camera m_Camera;

        // DiscoverScenesが起動時に一度だけ列挙する.ksceneの一覧。要素の並びがImGuiのシーン
        // 一覧・LoadSceneのインデックスに対応する(ファイル名の昇順)
        std::vector<std::wstring> m_SceneFilePaths;
        std::vector<std::wstring> m_SceneDisplayNames;

        // LoadSceneがm_Scene.Lights(SceneLoaderが各ModelInstanceのModel::Lightsをワールド空間へ
        // 変換し、.kscene自身の[Light]セクションのライトと合成済みのシーン全体のライト一覧)から
        // コピーし、以降ImGui(Lightingパネル)が編集する。アセット由来のデータとユーザー編集を
        // 分離するため(シーンを再読み込みすればアセット既定値に戻る)。m_SceneMutexで保護される
        // (m_Sceneと同じ理由)
        std::vector<Assets::Light> m_Lights;
        int m_SelectedLightIndex = -1;
        // 実在の写真露出値(EV100)。太陽・環境光・ポイント/スポットライトすべてに同じ値がかかる、
        // シーン全体で単一の露出設定(詳細はdocs/Architecture.html参照)
        float m_SceneExposureEV100 = Defaults::SceneExposureEV100;

        // 実際にライト強度へ事前乗算される「実効プリ露出」。m_SceneExposureEV100(ユーザー設定)に
        // 時刻由来のバイアスを足したもので、Renderスレッドのみが読み書きする。
        //
        // 【なぜ可変にする必要があるか】
        // プリ露出をEV100=15固定のままだと夜がfp16でつぶれる。満月の照度は0.25lxで、
        // 反射率0.2の面の輝度は 0.25*0.2/π = 0.016 cd/m^2。これに ComputeExposure(15)=2.54e-5 を
        // 掛けると 4.0e-7 となり、SceneColor(R16G16B16A16_Float、最小正規化数6.1e-5)の
        // 非正規化域へ落ちて情報が失われる。AutoExposure.hlsl も輝度1e-6未満の画素は
        // ヒストグラムに数えないため、露出計にも乗らず復元できない。
        //
        // M7で導入したプリ露出方式は Tonemap・Bloom・AutoExposure がすべて同じ値を受け取って
        // 割り戻す構造になっているため、**フレーム単位で変えても最終的な絵は変わらない**。
        // その性質をそのまま利用して、バッファの数値レンジだけを健全に保つ
        float m_EffectiveExposureEV100 = 15.0f;
        // 実効プリ露出が初期化済みか(初回フレームは平滑化せず即座に合わせる)
        bool m_EffectiveExposureInitialized = false;
        // 実効プリ露出の時間平滑化の速さ[1/秒]。段付きを防ぐために指数追従させる
        float m_EffectiveExposureAdaptSpeed = 2.0f;

        // RenderSceneSwitchUI(Renderスレッド)でシーン切り替えボタンが押されたときに書き込まれ、
        // UpdateSceneSwitch(Updateスレッド)が毎フレーム読み取って消費する1要素の受け渡し用。
        // LoadScene自体はUpdateスレッドから(m_SceneMutexで保護して)呼ぶ必要があるため、
        // クリック検出(Renderスレッド)と実際の呼び出し(Updateスレッド)をこれで分離する
        std::atomic<int> m_PendingSceneIndex{ -1 };

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
        // (m_TimeOfDayと同じ扱い)
        float m_RenderDeltaTime = 0.0f;

        // 統計表示用: 1フレームあたりのCPU時間(Renderの呼び出し時間)と、指数移動平均によるFPS。
        // どちらもRenderスレッドのみが書き込み、ImGui描画(同じくRenderスレッド)のみが読むため
        // 追加の排他制御は不要
        float m_CPUFrameTimeMs = 0.0f;
        float m_FPS = 0.0f;

        bool m_MouseCaptured = false;
        POINT m_MouseCaptureCenter{};

        // F1キーでImGuiの表示/非表示を切り替える(WasKeyPressedがエッジ検出を内蔵しているため、
        // 前フレームの押下状態を保持するメンバは不要)
        bool m_ImGuiVisible = true;
    };
}

#pragma warning(pop)
