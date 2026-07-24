#pragma once

#include <Windows.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "KurenaiEngineBase.h"
#include "KurenaiTypes.h"

#include "Assets/Model.h"
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
    class KURENAI_API KurenaiEngine3D : public KurenaiEngineBase
    {
    public:
        explicit KurenaiEngine3D(GraphicsAPI api = GraphicsAPI::DX11, uint32_t renderWidth = 1280, uint32_t renderHeight = 720);
        ~KurenaiEngine3D();

        void Run();

    private:
        void CreateSceneResources();
        void CreateRenderTargets(uint32_t width, uint32_t height);
        void LoadScene(size_t sceneIndex);
        void FrameCameraToModel();
        void UpdateMouseLook();
        void UpdateMovement(float deltaTime);
        void UpdateImGuiToggle();
        void Update(float deltaTime);
        void Render();
        void RenderSceneSwitchUI();
        void RenderPostProcessUI();
        void RenderDebugViewUI();
        void RenderLightingUI();
        void RenderProfilerUI();
        DirectX::XMMATRIX ComputeLightViewProj(const DirectX::XMFLOAT3& lightDirection) const;

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

        // ライティングパス(G-Bufferを読みSceneColorへ出力。G-Bufferと同じレンダー解像度)
        std::unique_ptr<RHI::IRHIShader> m_LightingVertexShader;
        std::unique_ptr<RHI::IRHIShader> m_LightingPixelShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_LightingPipelineState;
        std::unique_ptr<RHI::IRHITexture> m_SceneColor;

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

        // 垂直同期。既定で無効。有効にするとPresentがvblankまでブロックするため、GPU負荷が軽い
        // シーンではvsync待ちの間GPUがアイドル→省電力クロックに落ち、次フレームの立ち上がりが
        // 遅くなる・待ち時間自体もジッタで1vblank/2vblank分を行き来するなど計測値が不安定になる。
        // 既定はGPU/CPU双方の実処理時間を素直に見られるOFFとし、ティアリングを許容する
        // (ON時はPresentが即座に返らず、モニタのリフレッシュレートにFPSが制限される)
        bool m_VSyncEnabled = false;

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
            Depth,
            DepthRaw,           // 深度テクスチャの生値(0〜1)を加工せずそのままグレースケール表示
            DirectLight,        // DirectLightingパスの結果(HDR、シャドウ適用済みの直接光)をトーンマッピングして表示
            AOIndirectLight,    // AO/GIバッファのrgb(間接拡散光、ブラー後)をそのまま表示
            AOIndirectLightRaw, // AO/GIバッファのrgb(間接拡散光、ブラー前の生値)
            AOOcclusion,        // AO/GIバッファのa(遮蔽率、ブラー後)をグレースケール表示
            AOOcclusionRaw,     // AO/GIバッファのa(遮蔽率、ブラー前の生値)
            ShadowMap,
            SSR,                // SSRパスの出力(SceneColor+反射)。SSR無効時はSceneColorと同一
        };
        DebugView m_DebugView = DebugView::Final;

        // シャドウパス(平行光のライト視点から深度のみを描画する)
        static constexpr uint32_t kShadowMapSize = 2048;
        std::unique_ptr<RHI::IRHIShader> m_ShadowVertexShader;
        std::unique_ptr<RHI::IRHIShader> m_ShadowPixelShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_ShadowPipelineState;
        std::unique_ptr<RHI::IRHITexture> m_ShadowMap;
        bool m_ShadowEnabled = true;

        // 背景(深度が書き込まれなかったピクセル)に表示する空のキューブマップ
        std::unique_ptr<RHI::IRHITexture> m_SkyboxTexture;

        // 昼夜サイクル: ImGuiで操作する時刻(0〜24時)。太陽の向き・色・環境光・空の明るさに反映される
        float m_TimeOfDay = 12.0f;
        bool m_TimeAutoAdvance = false;
        float m_TimeAdvanceSpeed = 1.0f; // 自動進行時、1秒あたりに進む時間(時)

        std::unique_ptr<RHI::IRHISampler> m_Sampler;
        std::unique_ptr<RHI::IRHIBuffer> m_FrameConstantBuffer;
        std::unique_ptr<RHI::IRHIBuffer> m_MaterialConstantBuffer;
        Assets::Model m_Model;
        size_t m_CurrentSceneIndex = 0;

        Core::Camera m_Camera;
        std::chrono::steady_clock::time_point m_LastFrameTime;

        // 統計表示用: 1フレームあたりのCPU時間(Update+Renderの呼び出し時間)と、指数移動平均によるFPS
        float m_CPUFrameTimeMs = 0.0f;
        float m_FPS = 0.0f;

        bool m_MouseCaptured = false;
        POINT m_MouseCaptureCenter{};

        // F1キーでImGuiの表示/非表示を切り替える(GetAsyncKeyStateはキーの押下状態を毎フレーム返すため、
        // 押した瞬間だけ切り替えるには前フレームの状態と比較するエッジ検出が必要)
        bool m_ImGuiVisible = true;
        bool m_ImGuiToggleKeyWasDown = false;
    };
}

#pragma warning(pop)
