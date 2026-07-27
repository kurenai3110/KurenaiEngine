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

#include "KurenaiEngineBase.h"
#include "KurenaiTypes.h"

#include "Assets/Scene.h"
#include "Core/Camera.h"
#include "Core/CPUProfiler.h"

#pragma warning(push)
#pragma warning(disable: 4251)

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
        // UpdateスレッドからRenderスレッドへ、1フレーム分のカメラ・ImGui表示状態を引き渡すための
        // スナップショット。m_TimeOfDay等それ以外の状態はRenderスレッド側のみが読み書きするため
        // ここには含めない(RenderThreadMain参照)
        struct FrameState
        {
            Core::Camera Camera;
            bool ImGuiVisible = true;
        };

        void CreateSceneResources();
        // パス用途ごとのサンプラーセット(m_MaterialSamplers / m_ScreenSpaceSamplers)を作る。
        // セットの中身は作成後に書き換えないことが前提のAPIなので、描画を始める前に一度だけ呼ぶ
        // (理由はRHI/IRHISamplerSet.h)
        void CreateSamplerSets();
        void CreateRenderTargets(uint32_t width, uint32_t height);
        // <DLLフォルダ>/Assets/Scenes/*.ksceneを列挙し、m_SceneFilePaths/m_SceneDisplayNamesを構築する。
        // 個々のファイルの[Scene]Name読み取りに失敗した場合はそのファイルを警告ログとともに
        // スキップする(1ファイルの不備でアプリ全体が起動できなくなるのを避けるため)
        void DiscoverScenes();
        void LoadScene(size_t sceneIndex);
        void FrameCameraToModel();
        void UpdateMouseLook();
        void UpdateMovement(float deltaTime);
        void UpdateImGuiToggle();
        // RenderSceneSwitchUI(Renderスレッド)がm_PendingSceneIndexへ書き込んだシーン切り替え要求を
        // 見て、あればこのUpdateスレッドからLoadSceneを呼ぶ(LoadSceneをUpdateスレッド上で実行するための
        // ハンドオフ。詳細はm_PendingSceneIndexのコメント参照)
        void UpdateSceneSwitch();
        void Update(float deltaTime);
        void RenderThreadMain();
        void Render(const FrameState& frameState);
        void RenderSceneSwitchUI();
        void RenderPostProcessUI();
        void RenderDebugViewUI();
        void RenderLightingUI(const FrameState& frameState);
        void RenderProfilerUI();
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

        // GPUタイムスタンプクエリによる各パスの計測(Shadow/GBufferなど)。数フレーム遅れの結果が返る
        std::unique_ptr<RHI::IRHIGPUProfiler> m_GPUProfiler;
        // 各パスのコマンド記録にかかるCPU時間の計測(RHIに依存しないためDX11/DX12を直接比較できる)
        Core::CPUProfiler m_CPUProfiler;

        // G-Bufferの内部解像度。ウィンドウサイズとは独立しており、表示時はアスペクト比を保って拡大縮小する
        uint32_t m_RenderWidth;
        uint32_t m_RenderHeight;

        // ジオメトリパス(G-Buffer書き込み)
        std::unique_ptr<RHI::IRHIShader> m_GBufferVertexShader;
        std::unique_ptr<RHI::IRHIShader> m_GBufferPixelShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_GBufferPipelineState;
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
        bool m_AOEnabled = true;
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
        float m_SSAORadius = 0.5f;
        float m_SSAOPower = 1.5f;

        // SSILパス(Visibility Bitmask): G-BufferのAlbedo/Normal/Depthから遮蔽率と間接拡散光を計算する
        std::unique_ptr<RHI::IRHIShader> m_SSILPixelShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_SSILPipelineState;
        std::unique_ptr<RHI::IRHITexture> m_SSILRawTexture;
        std::unique_ptr<RHI::IRHITexture> m_SSILTexture;
        std::unique_ptr<RHI::IRHIBuffer> m_SSILConstantBuffer;
        float m_SSILRadius = 0.5f;
        float m_SSILThickness = 0.01f;
        float m_SSILIntensity = 2.0f;
        float m_SSILPower = 1.5f;
        uint32_t m_SSILSliceCount = 4;
        uint32_t m_SSILStepCount = 6;

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
        bool m_SSREnabled = true;
        float m_SSRMaxDistance = 5.0f;
        float m_SSRThickness = 0.1f;
        float m_SSRRoughnessCutoff = 0.6f;

        // Tonemapパス: SceneColor(SSR有効時はm_SSRTexture)のHDR値をReinhardトーンマッピング+
        // ガンマ補正でLDRへ変換し、Presentパスへ渡す。SSR等のHDR演算より後、Present直前の
        // 独立したステージとして置くことで、反射や将来のブルーム/露出制御(M7)がトーンマップの
        // 影響を受けないHDR値の上に成立できるようにする
        std::unique_ptr<RHI::IRHIShader> m_TonemapVertexShader;
        std::unique_ptr<RHI::IRHIShader> m_TonemapPixelShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_TonemapPipelineState;
        std::unique_ptr<RHI::IRHITexture> m_TonemapTexture;

        // 垂直同期。既定で無効。有効にするとPresentがvblankまでブロックするため、GPU負荷が軽い
        // シーンではvsync待ちの間GPUがアイドル→省電力クロックに落ち、次フレームの立ち上がりが
        // 遅くなる・待ち時間自体もジッタで1vblank/2vblank分を行き来するなど計測値が不安定になる。
        // 既定はGPU/CPU双方の実処理時間を素直に見られるOFFとし、ティアリングを許容する
        // (ON時はPresentが即座に返らず、モニタのリフレッシュレートにFPSが制限される)
        bool m_VSyncEnabled = false;

        // 固定FPSモード。有効時、Renderスレッドが目標FPSより速く回った分だけ待機してフレーム間隔を
        // 一定に保つ。VSyncはモニタのリフレッシュレート依存かつティアリング防止が目的だが、こちらは
        // 任意のFPS値に固定できる(物理更新の再現性確保や環境間でのフレーム時間比較などが目的)。
        // 既定で60fps固定を有効にする
        bool m_FixedFPSEnabled = true;
        float m_TargetFPS = 60.0f;

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
        };
        DebugView m_DebugView = DebugView::Final;

        // シャドウパス(平行光のライト視点から深度のみを描画する)。カメラ視錐台をkCascadeCount個の
        // 深度範囲に分割し(Practical Split Scheme)、それぞれ専用の正射影・シャドウマップを持たせる
        // カスケードシャドウマップ(CSM)。近いカスケードほどテクセル密度が高く、遠いカスケードほど
        // 広い範囲を粗くカバーする
        static constexpr uint32_t kShadowMapSize = 2048;
        std::unique_ptr<RHI::IRHIShader> m_ShadowVertexShader;
        std::unique_ptr<RHI::IRHIShader> m_ShadowPixelShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_ShadowPipelineState;
        std::array<std::unique_ptr<RHI::IRHITexture>, kCascadeCount> m_ShadowCascades;
        // シャドウパスの各カスケード描画で使う専用の定数バッファ(カスケードごとに値を更新して使い回す)
        std::unique_ptr<RHI::IRHIBuffer> m_ShadowCascadeConstantBuffer;
        bool m_ShadowEnabled = true;
        // PCSS(Percentage Closer Soft Shadows)のライトサイズ。シャドウマップUV空間での
        // ブロッカーサーチ・半影の広さを決める係数(値が大きいほど半影が広く柔らかくなる)
        float m_ShadowLightSize = 0.02f;
        // デバッグ表示(Render Targets - Shadow Map)で確認するカスケード番号(0=カメラに近い方)
        int32_t m_ShadowDebugCascade = 0;

        // 太陽(平行光)そのものの有効/無効。.ksceneの[Sun]Enabledで設定される。
        // TimeOfDayを夜にすると昼度(AmbientColor.a)も一緒に落ちて環境光まで消えてしまうため、
        // 「昼のまま太陽だけ消す」にはこちらを使う(White Furnace Testが必要とする)。
        // 無効時はFrameConstants.LightColorをゼロにするだけでよく、シェーダー側の変更は不要
        bool m_SunEnabled = true;

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
        bool m_IBLBaked = false;
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
        bool m_IBLEnabled = true;
        float m_IBLIntensity = 0.5f;
        // スペキュラBRDFのmultiple-scattering energy compensation(Kulla & Conty 2017)のON/OFF。
        // IBL鏡面・直接光鏡面の両方に効くため、Enable IBLとは独立したトグルにしている。
        // FrameConstants.ShadowParams.wへ1.0f/0.0fとして渡し、3つのシェーダー(DirectLighting/
        // DeferredLighting/Transparent)が共有するSpecularEnergy.hlsliの
        // SpecularEnergyCompensationがこれを見て倍率1.0へ落とす。
        // 既定でONにしているのは、補正しない状態がエネルギー的に不正(粗い面ほど暗い)であり、
        // OFFは実装検証・A/B比較のための選択肢という位置付けのため(14.9節)
        bool m_SpecularEnergyCompensationEnabled = true;
        // Enable IBL無効時に使う定数色アンビエントフォールバックの強度倍率。シェーダ側ではなく
        // Render()がFrameConstants.AmbientColorへ書き込む時点でrgb(alphaのdayFactorは除く)に
        // 乗算する(HLSL側は素のAmbientColor.rgbを読むだけでよい)
        float m_AmbientScale = 0.2f;

        // 昼夜サイクル: ImGuiで操作する時刻(0〜24時)。太陽の向き・色・環境光・空の明るさに反映される
        float m_TimeOfDay = 12.0f;
        bool m_TimeAutoAdvance = false;
        float m_TimeAdvanceSpeed = 1.0f; // 自動進行時、1秒あたりに進む時間(時)

        // 太陽が昇ってくる方位角(度)。X軸を0度、Z軸(+方向)を90度とした水平面上の角度で、
        // ImGuiで調整する(ComputeSunLightingが太陽の日の出側水平方向として使用する)
        float m_SunAzimuthDegrees = 126.87f;

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

        // LoadScene(Updateスレッド。UpdateSceneSwitch経由で呼ばれる)が書き込み、Render()(Renderスレッド。
        // 描画そのものに加えRenderPostProcessUI等のImGuiスライダーがm_SSAORadius等を直接書き換える)が
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
        float m_SceneExposureEV100 = 15.0f;

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
