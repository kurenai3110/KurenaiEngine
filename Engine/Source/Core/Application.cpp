#include "Application.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <random>

#include "Assets/ModelLoader.h"

namespace Kurenai::Core
{
    namespace
    {
        std::wstring GetExecutableDirectory()
        {
            wchar_t path[MAX_PATH];
            GetModuleFileNameW(nullptr, path, MAX_PATH);
            std::wstring pathStr(path);
            size_t pos = pathStr.find_last_of(L"\\/");
            return pos == std::wstring::npos ? L"" : pathStr.substr(0, pos + 1);
        }

        // シーン名はASCIIのみを想定しているが、将来的な非ASCII文字にも対応できるよう
        // WideCharToMultiByteで正しくUTF-8へ変換する(ImGuiのテキストAPIはUTF-8を期待する)
        std::string WideToUtf8(const wchar_t* wide)
        {
            if (!wide || !*wide)
            {
                return {};
            }
            int length = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
            std::string narrow(length, '\0');
            WideCharToMultiByte(CP_UTF8, 0, wide, -1, narrow.data(), length, nullptr, nullptr);
            narrow.resize(length - 1);
            return narrow;
        }

        struct alignas(16) FrameConstants
        {
            DirectX::XMFLOAT4X4 ViewProj;
            DirectX::XMFLOAT4X4 InvViewProj;
            DirectX::XMFLOAT4X4 LightViewProj;
            DirectX::XMFLOAT4 CameraPosition;
            DirectX::XMFLOAT4 LightDirection;
            DirectX::XMFLOAT4 LightColor;
            // SSAOパスがView空間でのサンプリングに使う(末尾に追加し、既存シェーダのオフセットは変えない)
            DirectX::XMFLOAT4X4 View;
            DirectX::XMFLOAT4X4 Proj;
            // 昼夜サイクル用(末尾に追加し、既存シェーダのオフセットは変えない)。rgb=環境光の色、a=昼度(0=夜,1=昼)
            DirectX::XMFLOAT4 AmbientColor;
        };

        // 太陽光の向き・色・環境光を時刻(0〜24時)から計算する
        struct SunLighting
        {
            DirectX::XMFLOAT3 Direction; // 光が進む向き(サーフェスに当たる方向)
            DirectX::XMFLOAT4 Color;
            DirectX::XMFLOAT4 Ambient; // rgb=環境光の色, a=昼度(0=夜,1=昼)
        };

        // edge0とedge1の間をなめらかに0→1で補間する(edge0以下は0、edge1以上は1)
        float Smoothstep(float edge0, float edge1, float x)
        {
            const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        }

        SunLighting ComputeSunLighting(float timeOfDayHours)
        {
            using namespace DirectX;

            // 日の出(東)側の水平方向。太陽はこの方向と天頂(真上)を通る鉛直面内で、
            // 東→天頂(正午)→西→天底(真夜中)と一日一周する半円軌道を描く
            constexpr XMFLOAT3 kSunriseHorizontal{ -0.6f, 0.0f, 0.8f };

            // 6時=0度(日の出/東)、12時=90度(天頂)、18時=180度(日の入り/西)、24時=270度(天底/真夜中)
            const float hourAngle = (timeOfDayHours / 24.0f) * XM_2PI - XM_PIDIV2;
            const float sinHour = std::sin(hourAngle);
            const float cosHour = std::cos(hourAngle);

            // 太陽の方向(地面から見て太陽がある向き)。kSunriseHorizontalとY軸(天頂)を結ぶ円軌道上の点
            const XMFLOAT3 sunDirection{ kSunriseHorizontal.x * cosHour, sinHour, kSunriseHorizontal.z * cosHour };

            SunLighting result{};
            result.Direction = { -sunDirection.x, -sunDirection.y, -sunDirection.z };

            // 6時〜7時でなめらかに夜→昼、17時〜18時でなめらかに昼→夜へ切り替える
            const float dayFactor = Smoothstep(6.0f, 7.0f, timeOfDayHours) * (1.0f - Smoothstep(17.0f, 18.0f, timeOfDayHours));

            const XMFLOAT3 kDayColor{ 3.0f, 2.9f, 2.7f };
            result.Color = { kDayColor.x * dayFactor, kDayColor.y * dayFactor, kDayColor.z * dayFactor, 0.0f };

            const XMFLOAT3 kDayAmbient{ 0.03f, 0.03f, 0.03f };
            const XMFLOAT3 kNightAmbient{ 0.006f, 0.008f, 0.015f };
            result.Ambient =
            {
                kNightAmbient.x + (kDayAmbient.x - kNightAmbient.x) * dayFactor,
                kNightAmbient.y + (kDayAmbient.y - kNightAmbient.y) * dayFactor,
                kNightAmbient.z + (kDayAmbient.z - kNightAmbient.z) * dayFactor,
                dayFactor,
            };

            return result;
        }

        struct alignas(16) MaterialConstants
        {
            float MetallicFactor;
            float RoughnessFactor;
            float Padding[2];
        };

        // Present.hlsl側のModeと一致させる必要がある
        struct alignas(16) PresentConstants
        {
            int32_t Mode;
            float Padding[3];
        };

        // SSAO.hlsl側のkSSAOKernelSizeと一致させる必要がある
        constexpr uint32_t kSSAOKernelSize = 16;

        struct alignas(16) SSAOConstants
        {
            DirectX::XMFLOAT4 Samples[kSSAOKernelSize]; // タンジェント空間の半球カーネル
            DirectX::XMFLOAT4 Params;                   // x: 半径, y: バイアス, z: 強さ(べき乗), w: 未使用
        };

        // SSIL_VisibilityBitmask.hlsl側のcbuffer SSILConstantsと一致させる必要がある
        struct alignas(16) SSILConstants
        {
            DirectX::XMFLOAT4 Params0; // x: 半径, y: 厚み(Thickness Heuristic), z: 間接光の強さ, w: AOのべき乗
            DirectX::XMUINT4 Params1;  // x: スライス数, y: スライスあたりのステップ数, z/w: 未使用
        };

        // タンジェント空間(Z軸=法線方向)の半球状にランダムなカーネルサンプルを生成する。
        // John Chapmanのチュートリアルにならい、原点付近にサンプルが偏るようスケーリングして
        // 近距離のディテールを優先的に拾う
        std::vector<DirectX::XMFLOAT4> GenerateSSAOKernel(uint32_t kernelSize)
        {
            std::mt19937 rng(12345);
            std::uniform_real_distribution<float> dist(0.0f, 1.0f);

            std::vector<DirectX::XMFLOAT4> kernel;
            kernel.reserve(kernelSize);
            for (uint32_t i = 0; i < kernelSize; ++i)
            {
                DirectX::XMVECTOR sample = DirectX::XMVectorSet(
                    dist(rng) * 2.0f - 1.0f,
                    dist(rng) * 2.0f - 1.0f,
                    dist(rng),
                    0.0f);
                sample = DirectX::XMVector3Normalize(sample);
                sample = DirectX::XMVectorScale(sample, dist(rng));

                float scale = static_cast<float>(i) / static_cast<float>(kernelSize);
                scale = 0.1f + 0.9f * scale * scale;
                sample = DirectX::XMVectorScale(sample, scale);

                DirectX::XMFLOAT4 sampleF;
                DirectX::XMStoreFloat4(&sampleF, sample);
                sampleF.w = 0.0f;
                kernel.push_back(sampleF);
            }
            return kernel;
        }

        struct SceneEntry
        {
            const wchar_t* DisplayName;
            const wchar_t* RelativePath;

            // trueの場合、FrameCameraToModelの自動配置ヒューリスティックの代わりにこの初期カメラ位置を使う。
            // Bistro Exteriorは密集した屋外の街区全体を包む巨大なバウンディングボックスを持ち、
            // ホール用ヒューリスティック(バウンズを20%内側に入った位置)では建物の壁の中に埋まってしまうため、
            // 実際に描画結果を確認して選んだ、街区中心部の広場(街灯・店舗・大きな木がある場所)を
            // 見渡せる位置を明示的に指定する
            bool HasCameraOverride = false;
            float CameraPosition[3] = { 0.0f, 0.0f, 0.0f };
            float CameraYaw = 0.0f;
        };

        const SceneEntry kScenes[] =
        {
            { L"Sponza", L"Assets\\Sponza\\Sponza.gltf" },
            { L"Bistro - Exterior", L"Assets\\Bistro\\BistroExterior.fbx", true, { 64.4f, 2.0f, -58.8f }, 0.0f },
            { L"Bistro - Interior", L"Assets\\Bistro\\BistroInterior.fbx" },
            { L"Bistro - Interior (Wine Cellar)", L"Assets\\Bistro\\BistroInterior_Wine.fbx" },
            { L"White Surface Test", L"Assets\\MaterialTest\\MaterialTest.gltf" },
        };
        constexpr size_t kSceneCount = sizeof(kScenes) / sizeof(kScenes[0]);

        // レンダー解像度(renderWidth x renderHeight)のアスペクト比を保ったまま、
        // windowWidth x windowHeight の中央に収まるビューポート(レターボックス/ピラーボックス)を求める
        RHI::Viewport ComputeLetterboxViewport(uint32_t windowWidth, uint32_t windowHeight, uint32_t renderWidth, uint32_t renderHeight)
        {
            const float windowAspect = static_cast<float>(windowWidth) / static_cast<float>(windowHeight);
            const float renderAspect = static_cast<float>(renderWidth) / static_cast<float>(renderHeight);

            float viewportWidth;
            float viewportHeight;
            if (windowAspect > renderAspect)
            {
                // ウィンドウの方が横長 -> 高さいっぱいに合わせ、左右に余白(ピラーボックス)
                viewportHeight = static_cast<float>(windowHeight);
                viewportWidth = viewportHeight * renderAspect;
            }
            else
            {
                // ウィンドウの方が縦長 -> 幅いっぱいに合わせ、上下に余白(レターボックス)
                viewportWidth = static_cast<float>(windowWidth);
                viewportHeight = viewportWidth / renderAspect;
            }

            RHI::Viewport viewport;
            viewport.TopLeftX = (static_cast<float>(windowWidth) - viewportWidth) * 0.5f;
            viewport.TopLeftY = (static_cast<float>(windowHeight) - viewportHeight) * 0.5f;
            viewport.Width = viewportWidth;
            viewport.Height = viewportHeight;
            return viewport;
        }
    }

    Application::Application(RHI::GraphicsAPI api, uint32_t renderWidth, uint32_t renderHeight)
        : m_GraphicsAPI(api)
        , m_RenderWidth(renderWidth)
        , m_RenderHeight(renderHeight)
    {
        m_Window = std::make_unique<Window>(L"Kurenai Engine", 1280, 720);
        m_Window->SetResizeCallback([this](uint32_t width, uint32_t height)
        {
            // G-Bufferは指定した内部解像度のまま固定し、表示側でアスペクト比を保って拡大縮小するため
            // ウィンドウリサイズではスワップチェインのみ更新する
            if (m_SwapChain)
            {
                m_SwapChain->Resize(width, height);
            }
        });

        m_Device = api == RHI::GraphicsAPI::DX12 ? RHI::CreateDX12Device() : RHI::CreateDX11Device();
        m_SwapChain = m_Device->CreateSwapChain(m_Window->GetHandle(), m_Window->GetWidth(), m_Window->GetHeight());
        m_ImGuiBackend = m_Device->CreateImGuiBackend(m_Window->GetHandle());

        // imgui.iniの保存先を起動時の作業ディレクトリに依存させず、実行ファイルと同じフォルダに固定する。
        // ImGuiはIniFilenameのポインタを保持するだけでコピーしないため、m_ImGuiIniPathで寿命を維持する
        m_ImGuiIniPath = WideToUtf8((GetExecutableDirectory() + L"imgui.ini").c_str());
        ImGui::GetIO().IniFilename = m_ImGuiIniPath.c_str();

        m_Camera.SetAspectRatio(static_cast<float>(m_RenderWidth) / static_cast<float>(m_RenderHeight));

        CreateSceneResources();

        m_LastFrameTime = std::chrono::steady_clock::now();
    }

    Application::~Application() = default;

    void Application::CreateSceneResources()
    {
        // Build/Bin/<Platform>/<Configuration>/ からリポジトリルートまでの相対パス
        const std::wstring repoRoot = GetExecutableDirectory() + L"..\\..\\..\\..\\";
        const std::wstring shaderDirectory = repoRoot + L"Sandbox\\Shaders\\";

        const std::vector<RHI::InputElementDesc> modelInputLayout =
        {
            { "POSITION", 0, RHI::Format::R32G32B32_Float, 0 },
            { "NORMAL", 0, RHI::Format::R32G32B32_Float, 12 },
            { "TEXCOORD", 0, RHI::Format::R32G32_Float, 24 },
        };

        // ジオメトリパス(G-Buffer書き込み)
        RHI::ShaderDesc gbufferVsDesc;
        gbufferVsDesc.Stage = RHI::ShaderStage::Vertex;
        gbufferVsDesc.FilePath = shaderDirectory + L"GBuffer.hlsl";
        gbufferVsDesc.EntryPoint = "VSMain";
        m_GBufferVertexShader = m_Device->CreateShader(gbufferVsDesc);

        RHI::ShaderDesc gbufferPsDesc;
        gbufferPsDesc.Stage = RHI::ShaderStage::Pixel;
        gbufferPsDesc.FilePath = shaderDirectory + L"GBuffer.hlsl";
        gbufferPsDesc.EntryPoint = "PSMain";
        m_GBufferPixelShader = m_Device->CreateShader(gbufferPsDesc);

        RHI::PipelineStateDesc gbufferPipelineDesc;
        gbufferPipelineDesc.InputLayout = modelInputLayout;
        gbufferPipelineDesc.VertexShader = m_GBufferVertexShader.get();
        gbufferPipelineDesc.PixelShader = m_GBufferPixelShader.get();
        gbufferPipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
        gbufferPipelineDesc.RenderTargetFormats = { RHI::Format::R8G8B8A8_UNorm, RHI::Format::R8G8B8A8_UNorm, RHI::Format::R8G8B8A8_UNorm };
        gbufferPipelineDesc.HasDepthStencil = true;
        gbufferPipelineDesc.ReverseZ = true;
        m_GBufferPipelineState = m_Device->CreatePipelineState(gbufferPipelineDesc);

        // 直接光パス(頂点バッファなしのフルスクリーン三角形。G-Buffer+シャドウマップからPBRの
        // 直接光を計算しHDRで書き出す)
        RHI::ShaderDesc directLightVsDesc;
        directLightVsDesc.Stage = RHI::ShaderStage::Vertex;
        directLightVsDesc.FilePath = shaderDirectory + L"DirectLighting.hlsl";
        directLightVsDesc.EntryPoint = "VSMain";
        m_DirectLightVertexShader = m_Device->CreateShader(directLightVsDesc);

        RHI::ShaderDesc directLightPsDesc;
        directLightPsDesc.Stage = RHI::ShaderStage::Pixel;
        directLightPsDesc.FilePath = shaderDirectory + L"DirectLighting.hlsl";
        directLightPsDesc.EntryPoint = "PSMain";
        m_DirectLightPixelShader = m_Device->CreateShader(directLightPsDesc);

        RHI::PipelineStateDesc directLightPipelineDesc;
        directLightPipelineDesc.VertexShader = m_DirectLightVertexShader.get();
        directLightPipelineDesc.PixelShader = m_DirectLightPixelShader.get();
        directLightPipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
        directLightPipelineDesc.RenderTargetFormats = { RHI::Format::R32G32B32A32_Float };
        m_DirectLightPipelineState = m_Device->CreatePipelineState(directLightPipelineDesc);

        // AO/GI共通の頂点シェーダ(頂点バッファなしのフルスクリーン三角形)。SSAO/SSIL/共通ブラーの
        // 3つのピクセルシェーダで使い回す
        RHI::ShaderDesc aoVsDesc;
        aoVsDesc.Stage = RHI::ShaderStage::Vertex;
        aoVsDesc.FilePath = shaderDirectory + L"SSAO.hlsl";
        aoVsDesc.EntryPoint = "VSMain";
        m_AOVertexShader = m_Device->CreateShader(aoVsDesc);

        // SSAOパス
        RHI::ShaderDesc ssaoPsDesc;
        ssaoPsDesc.Stage = RHI::ShaderStage::Pixel;
        ssaoPsDesc.FilePath = shaderDirectory + L"SSAO.hlsl";
        ssaoPsDesc.EntryPoint = "PSMain";
        m_SSAOPixelShader = m_Device->CreateShader(ssaoPsDesc);

        RHI::PipelineStateDesc ssaoPipelineDesc;
        ssaoPipelineDesc.VertexShader = m_AOVertexShader.get();
        ssaoPipelineDesc.PixelShader = m_SSAOPixelShader.get();
        ssaoPipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
        ssaoPipelineDesc.RenderTargetFormats = { RHI::Format::R8G8B8A8_UNorm };
        m_SSAOPipelineState = m_Device->CreatePipelineState(ssaoPipelineDesc);

        m_SSAOKernel = GenerateSSAOKernel(kSSAOKernelSize);

        RHI::BufferDesc ssaoConstantBufferDesc;
        ssaoConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        ssaoConstantBufferDesc.SizeInBytes = sizeof(SSAOConstants);
        m_SSAOConstantBuffer = m_Device->CreateBuffer(ssaoConstantBufferDesc);

        // SSILパス(Visibility Bitmask)
        RHI::ShaderDesc ssilPsDesc;
        ssilPsDesc.Stage = RHI::ShaderStage::Pixel;
        ssilPsDesc.FilePath = shaderDirectory + L"SSIL_VisibilityBitmask.hlsl";
        ssilPsDesc.EntryPoint = "PSMain";
        m_SSILPixelShader = m_Device->CreateShader(ssilPsDesc);

        RHI::PipelineStateDesc ssilPipelineDesc;
        ssilPipelineDesc.VertexShader = m_AOVertexShader.get();
        ssilPipelineDesc.PixelShader = m_SSILPixelShader.get();
        ssilPipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
        ssilPipelineDesc.RenderTargetFormats = { RHI::Format::R8G8B8A8_UNorm };
        m_SSILPipelineState = m_Device->CreatePipelineState(ssilPipelineDesc);

        RHI::BufferDesc ssilConstantBufferDesc;
        ssilConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        ssilConstantBufferDesc.SizeInBytes = sizeof(SSILConstants);
        m_SSILConstantBuffer = m_Device->CreateBuffer(ssilConstantBufferDesc);

        // AO/GI共通のブラーパス(SSAO.hlslのPSMainBlurを、rgbaフォーマットが同じSSAO/SSIL両方で使い回す)
        RHI::ShaderDesc aoBlurPsDesc;
        aoBlurPsDesc.Stage = RHI::ShaderStage::Pixel;
        aoBlurPsDesc.FilePath = shaderDirectory + L"SSAO.hlsl";
        aoBlurPsDesc.EntryPoint = "PSMainBlur";
        m_AOBlurPixelShader = m_Device->CreateShader(aoBlurPsDesc);

        RHI::PipelineStateDesc aoBlurPipelineDesc;
        aoBlurPipelineDesc.VertexShader = m_AOVertexShader.get();
        aoBlurPipelineDesc.PixelShader = m_AOBlurPixelShader.get();
        aoBlurPipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
        aoBlurPipelineDesc.RenderTargetFormats = { RHI::Format::R8G8B8A8_UNorm };
        m_AOBlurPipelineState = m_Device->CreatePipelineState(aoBlurPipelineDesc);

        // AO/GI無効時はこの常に黒・不透明(遮蔽なし=a:1、間接光なし=rgb:0)のテクスチャをライティングパスに渡す
        m_AODisabledTexture = m_Device->CreateSolidColorTexture(0, 0, 0, 255);

        // ライティングパス(頂点バッファなしのフルスクリーン三角形)
        RHI::ShaderDesc lightingVsDesc;
        lightingVsDesc.Stage = RHI::ShaderStage::Vertex;
        lightingVsDesc.FilePath = shaderDirectory + L"DeferredLighting.hlsl";
        lightingVsDesc.EntryPoint = "VSMain";
        m_LightingVertexShader = m_Device->CreateShader(lightingVsDesc);

        RHI::ShaderDesc lightingPsDesc;
        lightingPsDesc.Stage = RHI::ShaderStage::Pixel;
        lightingPsDesc.FilePath = shaderDirectory + L"DeferredLighting.hlsl";
        lightingPsDesc.EntryPoint = "PSMain";
        m_LightingPixelShader = m_Device->CreateShader(lightingPsDesc);

        RHI::PipelineStateDesc lightingPipelineDesc;
        lightingPipelineDesc.VertexShader = m_LightingVertexShader.get();
        lightingPipelineDesc.PixelShader = m_LightingPixelShader.get();
        lightingPipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
        lightingPipelineDesc.RenderTargetFormats = { RHI::Format::R8G8B8A8_UNorm };
        m_LightingPipelineState = m_Device->CreatePipelineState(lightingPipelineDesc);

        // Presentパス(頂点バッファなしのフルスクリーン三角形。SceneColorをバックバッファへ拡大縮小表示)
        RHI::ShaderDesc presentVsDesc;
        presentVsDesc.Stage = RHI::ShaderStage::Vertex;
        presentVsDesc.FilePath = shaderDirectory + L"Present.hlsl";
        presentVsDesc.EntryPoint = "VSMain";
        m_PresentVertexShader = m_Device->CreateShader(presentVsDesc);

        RHI::ShaderDesc presentPsDesc;
        presentPsDesc.Stage = RHI::ShaderStage::Pixel;
        presentPsDesc.FilePath = shaderDirectory + L"Present.hlsl";
        presentPsDesc.EntryPoint = "PSMain";
        m_PresentPixelShader = m_Device->CreateShader(presentPsDesc);

        RHI::PipelineStateDesc presentPipelineDesc;
        presentPipelineDesc.VertexShader = m_PresentVertexShader.get();
        presentPipelineDesc.PixelShader = m_PresentPixelShader.get();
        presentPipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
        presentPipelineDesc.RenderTargetFormats = { RHI::Format::R8G8B8A8_UNorm };
        m_PresentPipelineState = m_Device->CreatePipelineState(presentPipelineDesc);

        // シャドウパス(ライト視点への深度のみの描画。頂点入力はPOSITIONのみ使用)
        RHI::ShaderDesc shadowVsDesc;
        shadowVsDesc.Stage = RHI::ShaderStage::Vertex;
        shadowVsDesc.FilePath = shaderDirectory + L"Shadow.hlsl";
        shadowVsDesc.EntryPoint = "VSMain";
        m_ShadowVertexShader = m_Device->CreateShader(shadowVsDesc);

        RHI::ShaderDesc shadowPsDesc;
        shadowPsDesc.Stage = RHI::ShaderStage::Pixel;
        shadowPsDesc.FilePath = shaderDirectory + L"Shadow.hlsl";
        shadowPsDesc.EntryPoint = "PSMain";
        m_ShadowPixelShader = m_Device->CreateShader(shadowPsDesc);

        const std::vector<RHI::InputElementDesc> shadowInputLayout =
        {
            { "POSITION", 0, RHI::Format::R32G32B32_Float, 0 },
        };

        RHI::PipelineStateDesc shadowPipelineDesc;
        shadowPipelineDesc.InputLayout = shadowInputLayout;
        shadowPipelineDesc.VertexShader = m_ShadowVertexShader.get();
        shadowPipelineDesc.PixelShader = m_ShadowPixelShader.get();
        shadowPipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
        shadowPipelineDesc.HasDepthStencil = true;
        m_ShadowPipelineState = m_Device->CreatePipelineState(shadowPipelineDesc);

        // シャドウマップはG-Bufferと異なりウィンドウ/レンダー解像度に依存しないため固定サイズで一度だけ作成する
        m_ShadowMap = m_Device->CreateDepthTexture(kShadowMapSize, kShadowMapSize);

        // 空のキューブマップはシーンに依存しないため一度だけ読み込む
        m_SkyboxTexture = m_Device->CreateTextureFromFile(repoRoot + L"Assets\\Skybox\\Sky.dds", false);

        m_Sampler = m_Device->CreateDefaultSampler();

        RHI::BufferDesc constantBufferDesc;
        constantBufferDesc.Usage = RHI::BufferUsage::Constant;
        constantBufferDesc.SizeInBytes = sizeof(FrameConstants);
        m_FrameConstantBuffer = m_Device->CreateBuffer(constantBufferDesc);

        RHI::BufferDesc materialConstantBufferDesc;
        materialConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        materialConstantBufferDesc.SizeInBytes = sizeof(MaterialConstants);
        m_MaterialConstantBuffer = m_Device->CreateBuffer(materialConstantBufferDesc);

        RHI::BufferDesc presentConstantBufferDesc;
        presentConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        presentConstantBufferDesc.SizeInBytes = sizeof(PresentConstants);
        m_PresentConstantBuffer = m_Device->CreateBuffer(presentConstantBufferDesc);

        CreateRenderTargets(m_RenderWidth, m_RenderHeight);

        LoadScene(0);
    }

    void Application::CreateRenderTargets(uint32_t width, uint32_t height)
    {
        if (width == 0 || height == 0)
        {
            return;
        }

        m_GBufferAlbedo = m_Device->CreateRenderTexture(width, height, RHI::Format::R8G8B8A8_UNorm);
        m_GBufferNormal = m_Device->CreateRenderTexture(width, height, RHI::Format::R8G8B8A8_UNorm);
        m_GBufferMaterial = m_Device->CreateRenderTexture(width, height, RHI::Format::R8G8B8A8_UNorm);
        // Reverse-Zのため近平面側(NDC z=1.0)ではなく遠平面側(NDC z=0.0)にクリアする
        m_GBufferDepth = m_Device->CreateDepthTexture(width, height, 0.0f);
        m_DirectLightTexture = m_Device->CreateRenderTexture(width, height, RHI::Format::R32G32B32A32_Float);
        m_SSAORawTexture = m_Device->CreateRenderTexture(width, height, RHI::Format::R8G8B8A8_UNorm);
        m_SSAOTexture = m_Device->CreateRenderTexture(width, height, RHI::Format::R8G8B8A8_UNorm);
        m_SSILRawTexture = m_Device->CreateRenderTexture(width, height, RHI::Format::R8G8B8A8_UNorm);
        m_SSILTexture = m_Device->CreateRenderTexture(width, height, RHI::Format::R8G8B8A8_UNorm);
        m_SceneColor = m_Device->CreateRenderTexture(width, height, RHI::Format::R8G8B8A8_UNorm);
    }

    void Application::LoadScene(size_t sceneIndex)
    {
        if (sceneIndex >= kSceneCount)
        {
            return;
        }

        const std::wstring repoRoot = GetExecutableDirectory() + L"..\\..\\..\\..\\";
        const std::wstring modelPath = repoRoot + kScenes[sceneIndex].RelativePath;

        m_Model = Assets::LoadModel(*m_Device, modelPath);
        m_CurrentSceneIndex = sceneIndex;

        FrameCameraToModel();

        const wchar_t* apiName = (m_GraphicsAPI == RHI::GraphicsAPI::DX12) ? L"DX12" : L"DX11";
        m_Window->SetTitle(std::wstring(L"Kurenai Engine [") + apiName + L"] - " + kScenes[sceneIndex].DisplayName);
    }

    void Application::FrameCameraToModel()
    {
        const float sizeY = m_Model.BoundsMax[1] - m_Model.BoundsMin[1];
        const float dx = m_Model.BoundsMax[0] - m_Model.BoundsMin[0];
        const float dz = m_Model.BoundsMax[2] - m_Model.BoundsMin[2];
        const float diagonal = std::sqrt(dx * dx + sizeY * sizeY + dz * dz);

        // SSAO/SSILのサンプリング半径はシーンの規模に応じて変わるべきなので、対角線に比例させる
        // (小さすぎる/大きすぎるシーンでも遮蔽表現が破綻しないよう妥当な範囲にクランプする)
        m_SSAORadius = std::clamp(diagonal * 0.01f, 0.05f, 2.0f);
        m_SSILRadius = m_SSAORadius;
        m_SSILThickness = m_SSILRadius * 0.2f;

        const SceneEntry& currentScene = kScenes[m_CurrentSceneIndex];
        if (currentScene.HasCameraOverride)
        {
            m_Camera.SetPosition({ currentScene.CameraPosition[0], currentScene.CameraPosition[1], currentScene.CameraPosition[2] });
            m_Camera.SetYawPitch(currentScene.CameraYaw, 0.0f);
            m_Camera.SetLens(DirectX::XM_PIDIV4, std::max(0.01f, diagonal * 0.0005f), std::max(100.0f, diagonal * 4.0f));
            return;
        }

        const float centerX = (m_Model.BoundsMin[0] + m_Model.BoundsMax[0]) * 0.5f;
        const float centerY = (m_Model.BoundsMin[1] + m_Model.BoundsMax[1]) * 0.5f;
        const float centerZ = (m_Model.BoundsMin[2] + m_Model.BoundsMax[2]) * 0.5f;
        const float eyeHeight = m_Model.BoundsMin[1] + sizeY * 0.15f;

        const float longAxis = std::max(dx, dz);
        const float shortAxis = std::min(dx, dz);
        // 短辺が長辺に対して極端に短い場合は、歩いて回れる建物内部ではなく横に並んだ物体と判断し、
        // 内部に入り込む配置ではなく外側から全体を見渡す配置にする
        const bool isThinProp = shortAxis < longAxis * 0.15f;

        float posX;
        float posY;
        float posZ;
        float yaw;
        float nearZ;
        const float farZ = std::max(100.0f, diagonal * 4.0f);

        if (isThinProp)
        {
            // 縦FOVの半角のtanを使い、アスペクト比に依らず長辺全体が収まる距離を保守的に求める
            const float halfFovTan = std::tan(DirectX::XM_PIDIV4 * 0.5f);
            const float requiredDistance = (longAxis * 0.5f) / halfFovTan * 1.25f;

            posX = centerX;
            posY = centerY;
            posZ = centerZ + requiredDistance;
            yaw = DirectX::XM_PI;

            // カメラは物体から離れた位置にあるため、near平面をdiagonal基準の極小値のままにすると
            // 深度バッファの精度が視距離全体で失われてしまう(near:distance比が極端になるため)。
            // 実際の視距離に応じたスケールにして深度精度を確保する
            nearZ = std::max(0.05f, requiredDistance * 0.02f);
        }
        else if (dx >= dz)
        {
            // ホールの長辺方向の端寄りから中心を見る位置を初期視点にする(中央の装飾物や壁に埋まらないように)
            posX = m_Model.BoundsMin[0] + dx * 0.2f;
            posY = eyeHeight;
            posZ = centerZ;
            yaw = DirectX::XM_PIDIV2;
            nearZ = std::max(0.01f, diagonal * 0.0005f);
        }
        else
        {
            posX = centerX;
            posY = eyeHeight;
            posZ = m_Model.BoundsMin[2] + dz * 0.2f;
            yaw = 0.0f;
            nearZ = std::max(0.01f, diagonal * 0.0005f);
        }

        m_Camera.SetPosition({ posX, posY, posZ });
        m_Camera.SetYawPitch(yaw, 0.0f);
        m_Camera.SetLens(DirectX::XM_PIDIV4, nearZ, farZ);
    }

    // 平行光のライト視点からシーン全体を覆う正射影のビュー・プロジェクション行列を求める。
    // シーンのバウンディングスフィア(AABBの外接球)を基準に、ライト方向の逆側から見渡す位置に
    // 仮想的なライトカメラを置き、球全体が収まる正射影範囲・奥行きを設定する
    DirectX::XMMATRIX Application::ComputeLightViewProj(const DirectX::XMFLOAT3& lightDirection) const
    {
        using namespace DirectX;

        const XMFLOAT3 center
        {
            (m_Model.BoundsMin[0] + m_Model.BoundsMax[0]) * 0.5f,
            (m_Model.BoundsMin[1] + m_Model.BoundsMax[1]) * 0.5f,
            (m_Model.BoundsMin[2] + m_Model.BoundsMax[2]) * 0.5f,
        };
        const float dx = m_Model.BoundsMax[0] - m_Model.BoundsMin[0];
        const float dy = m_Model.BoundsMax[1] - m_Model.BoundsMin[1];
        const float dz = m_Model.BoundsMax[2] - m_Model.BoundsMin[2];
        const float sceneRadius = std::max(0.01f, std::sqrt(dx * dx + dy * dy + dz * dz) * 0.5f);

        const XMVECTOR lightDirVec = XMVector3Normalize(XMLoadFloat3(&lightDirection));
        const XMVECTOR centerVec = XMLoadFloat3(&center);

        // シーンを包む球全体を見渡せるよう、ライトが進む方向と逆側に球の半径分だけ余裕を持って離れた位置に置く
        const float margin = 1.5f;
        const XMVECTOR eye = XMVectorSubtract(centerVec, XMVectorScale(lightDirVec, sceneRadius * margin));

        // ライト方向がほぼ真上/真下(upベクトルと平行)だとLookAt行列が縮退するため、そのときだけ別軸を使う
        XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        if (std::abs(XMVectorGetY(lightDirVec)) > 0.99f)
        {
            up = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
        }

        const XMMATRIX lightView = XMMatrixLookAtLH(eye, centerVec, up);

        const float orthoSize = sceneRadius * margin * 2.0f;
        const float nearZ = 0.1f;
        const float farZ = sceneRadius * margin * 2.0f + sceneRadius;
        const XMMATRIX lightProj = XMMatrixOrthographicLH(orthoSize, orthoSize, nearZ, farZ);

        return lightView * lightProj;
    }

    void Application::RenderSceneSwitchUI()
    {
        ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(260.0f, 0.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("Scenes");

        ImGui::TextUnformatted(m_GraphicsAPI == RHI::GraphicsAPI::DX12 ? "Graphics API: DX12" : "Graphics API: DX11");
        ImGui::Separator();

        for (size_t i = 0; i < kSceneCount; ++i)
        {
            const bool isCurrent = (i == m_CurrentSceneIndex);
            if (isCurrent)
            {
                ImGui::BeginDisabled();
            }

            const std::string label = WideToUtf8(kScenes[i].DisplayName);
            if (ImGui::Button(label.c_str(), ImVec2(-FLT_MIN, 0.0f)))
            {
                LoadScene(i);
            }

            if (isCurrent)
            {
                ImGui::EndDisabled();
            }
        }

        ImGui::End();
    }

    void Application::RenderPostProcessUI()
    {
        ImGui::SetNextWindowPos(ImVec2(10.0f, 280.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(260.0f, 0.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("Post Processing");

        ImGui::Checkbox("Enable AO / Indirect Light", &m_AOEnabled);
        if (m_AOEnabled)
        {
            static const char* kAOTechniqueNames[] = { "SSAO", "SSIL (Visibility Bitmask)" };
            int techniqueIndex = static_cast<int>(m_AOTechnique);
            if (ImGui::Combo("Technique", &techniqueIndex, kAOTechniqueNames, IM_ARRAYSIZE(kAOTechniqueNames)))
            {
                m_AOTechnique = static_cast<AOTechnique>(techniqueIndex);
            }

            if (m_AOTechnique == AOTechnique::SSAO)
            {
                ImGui::SliderFloat("SSAO Radius", &m_SSAORadius, 0.01f, 5.0f);
                ImGui::SliderFloat("SSAO Power", &m_SSAOPower, 0.1f, 4.0f);
            }
            else
            {
                ImGui::SliderFloat("SSIL Radius", &m_SSILRadius, 0.01f, 5.0f);
                ImGui::SliderFloat("SSIL Thickness", &m_SSILThickness, 0.01f, 2.0f);
                ImGui::SliderFloat("SSIL Intensity", &m_SSILIntensity, 0.0f, 8.0f);
                ImGui::SliderFloat("SSIL AO Power", &m_SSILPower, 0.1f, 4.0f);

                int sliceCount = static_cast<int>(m_SSILSliceCount);
                if (ImGui::SliderInt("SSIL Slices", &sliceCount, 1, 8))
                {
                    m_SSILSliceCount = static_cast<uint32_t>(sliceCount);
                }

                int stepCount = static_cast<int>(m_SSILStepCount);
                if (ImGui::SliderInt("SSIL Steps", &stepCount, 1, 16))
                {
                    m_SSILStepCount = static_cast<uint32_t>(stepCount);
                }
            }
        }

        ImGui::Checkbox("Enable Shadow", &m_ShadowEnabled);

        ImGui::End();
    }

    void Application::RenderDebugViewUI()
    {
        ImGui::SetNextWindowPos(ImVec2(10.0f, 540.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(260.0f, 0.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("Render Targets");

        static const char* kDebugViewNames[] =
        {
            "Final (Lit)",
            "Albedo",
            "Normal",
            "Material (R=Metallic, G=Roughness)",
            "Depth",
            "Depth (Raw)",
            "Direct Light",
            "AO/GI - Indirect Light (RGB)",
            "AO/GI - Indirect Light (RGB, Before Blur)",
            "AO/GI - Occlusion (Alpha)",
            "AO/GI - Occlusion (Alpha, Before Blur)",
            "Shadow Map",
        };

        int currentIndex = static_cast<int>(m_DebugView);
        if (ImGui::Combo("View", &currentIndex, kDebugViewNames, IM_ARRAYSIZE(kDebugViewNames)))
        {
            m_DebugView = static_cast<DebugView>(currentIndex);
        }

        ImGui::End();
    }

    void Application::RenderLightingUI()
    {
        ImGui::SetNextWindowPos(ImVec2(280.0f, 10.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(260.0f, 0.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("Lighting");

        ImGui::SliderFloat("Time of Day", &m_TimeOfDay, 0.0f, 24.0f, "%.2f h");
        ImGui::Checkbox("Auto Advance", &m_TimeAutoAdvance);
        if (m_TimeAutoAdvance)
        {
            ImGui::SliderFloat("Speed", &m_TimeAdvanceSpeed, 0.1f, 10.0f, "%.1f h/s");
        }

        ImGui::End();
    }

    void Application::Run()
    {
        while (!m_Window->ShouldClose())
        {
            m_Window->PumpMessages();
            if (m_Window->ShouldClose())
            {
                break;
            }

            const auto now = std::chrono::steady_clock::now();
            const float deltaTime = std::chrono::duration<float>(now - m_LastFrameTime).count();
            m_LastFrameTime = now;

            Update(deltaTime);
            Render();
        }
    }

    void Application::UpdateMouseLook()
    {
        // GetAsyncKeyStateはウィンドウフォーカスに関係なくグローバルなキー状態を返すため、
        // フォアグラウンドウィンドウチェックがないとデスクトップ上の右クリックでも
        // カーソルがSandboxウィンドウ中央へ強制移動してしまう
        const bool isForeground = GetForegroundWindow() == m_Window->GetHandle();
        if (isForeground && (GetAsyncKeyState(VK_RBUTTON) & 0x8000))
        {
            if (!m_MouseCaptured)
            {
                m_MouseCaptured = true;
                ShowCursor(FALSE);

                RECT clientRect;
                GetClientRect(m_Window->GetHandle(), &clientRect);
                POINT center{ (clientRect.right - clientRect.left) / 2, (clientRect.bottom - clientRect.top) / 2 };
                ClientToScreen(m_Window->GetHandle(), &center);
                m_MouseCaptureCenter = center;
                SetCursorPos(center.x, center.y);
            }
            else
            {
                POINT currentPos;
                GetCursorPos(&currentPos);
                const float deltaX = static_cast<float>(currentPos.x - m_MouseCaptureCenter.x);
                const float deltaY = static_cast<float>(currentPos.y - m_MouseCaptureCenter.y);

                const float mouseSensitivity = 0.0025f;
                m_Camera.Rotate(deltaX * mouseSensitivity, -deltaY * mouseSensitivity);

                SetCursorPos(m_MouseCaptureCenter.x, m_MouseCaptureCenter.y);
            }
        }
        else if (m_MouseCaptured)
        {
            m_MouseCaptured = false;
            ShowCursor(TRUE);
        }
    }

    void Application::UpdateMovement(float deltaTime)
    {
        const float moveSpeed = (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 20.0f : 5.0f;
        const float moveAmount = moveSpeed * deltaTime;

        const DirectX::XMFLOAT3 forward = m_Camera.GetForward();
        const DirectX::XMFLOAT3 right = m_Camera.GetRight();

        DirectX::XMFLOAT3 move{ 0.0f, 0.0f, 0.0f };
        auto add = [&move](const DirectX::XMFLOAT3& v, float sign)
        {
            move.x += v.x * sign;
            move.y += v.y * sign;
            move.z += v.z * sign;
        };

        if (GetAsyncKeyState('W') & 0x8000) add(forward, 1.0f);
        if (GetAsyncKeyState('S') & 0x8000) add(forward, -1.0f);
        if (GetAsyncKeyState('D') & 0x8000) add(right, 1.0f);
        if (GetAsyncKeyState('A') & 0x8000) add(right, -1.0f);
        if (GetAsyncKeyState('E') & 0x8000) move.y += 1.0f;
        if (GetAsyncKeyState('Q') & 0x8000) move.y -= 1.0f;

        DirectX::XMVECTOR moveVec = DirectX::XMLoadFloat3(&move);
        if (DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(moveVec)) > 0.0001f)
        {
            moveVec = DirectX::XMVectorScale(DirectX::XMVector3Normalize(moveVec), moveAmount);
            DirectX::XMFLOAT3 delta;
            DirectX::XMStoreFloat3(&delta, moveVec);
            m_Camera.Move(delta);
        }
    }

    void Application::Update(float deltaTime)
    {
        UpdateMouseLook();
        UpdateMovement(deltaTime);

        if (m_TimeAutoAdvance)
        {
            m_TimeOfDay = std::fmod(m_TimeOfDay + m_TimeAdvanceSpeed * deltaTime, 24.0f);
            if (m_TimeOfDay < 0.0f)
            {
                m_TimeOfDay += 24.0f;
            }
        }
    }

    void Application::Render()
    {
        if (m_Window->GetWidth() == 0 || m_Window->GetHeight() == 0)
        {
            return;
        }

        m_ImGuiBackend->NewFrame();
        RenderSceneSwitchUI();
        RenderPostProcessUI();
        RenderDebugViewUI();
        RenderLightingUI();

        auto* commandList = m_Device->GetImmediateCommandList();

        const SunLighting sunLighting = ComputeSunLighting(m_TimeOfDay);
        const DirectX::XMMATRIX lightViewProj = ComputeLightViewProj(sunLighting.Direction);

        FrameConstants constants;
        const DirectX::XMMATRIX viewProj = m_Camera.GetViewMatrix() * m_Camera.GetProjectionMatrix();
        DirectX::XMStoreFloat4x4(&constants.ViewProj, DirectX::XMMatrixTranspose(viewProj));
        DirectX::XMVECTOR determinant;
        const DirectX::XMMATRIX invViewProj = DirectX::XMMatrixInverse(&determinant, viewProj);
        DirectX::XMStoreFloat4x4(&constants.InvViewProj, DirectX::XMMatrixTranspose(invViewProj));
        DirectX::XMStoreFloat4x4(&constants.LightViewProj, DirectX::XMMatrixTranspose(lightViewProj));
        const DirectX::XMFLOAT3 cameraPosition = m_Camera.GetPosition();
        constants.CameraPosition = { cameraPosition.x, cameraPosition.y, cameraPosition.z, 0.0f };
        constants.LightDirection = { sunLighting.Direction.x, sunLighting.Direction.y, sunLighting.Direction.z, 0.0f };
        constants.LightColor = sunLighting.Color;
        DirectX::XMStoreFloat4x4(&constants.View, DirectX::XMMatrixTranspose(m_Camera.GetViewMatrix()));
        DirectX::XMStoreFloat4x4(&constants.Proj, DirectX::XMMatrixTranspose(m_Camera.GetProjectionMatrix()));
        constants.AmbientColor = sunLighting.Ambient;
        commandList->UpdateBuffer(m_FrameConstantBuffer.get(), &constants, sizeof(constants));

        // --- シャドウパス: ライト視点から深度のみを描画する(常に固定のシャドウマップ解像度) ---
        RHI::Viewport shadowViewport;
        shadowViewport.Width = static_cast<float>(kShadowMapSize);
        shadowViewport.Height = static_cast<float>(kShadowMapSize);
        commandList->SetViewport(shadowViewport);

        commandList->SetRenderTargets(nullptr, 0, m_ShadowMap.get());
        // 深度1.0(最遠)にクリアしておく。無効時はこの後の描画をスキップするため、
        // シェーダー側は深度比較で常に「影なし」と判定する(ComputeShadowFactor参照)
        commandList->ClearDepth(1.0f);

        if (m_ShadowEnabled)
        {
            commandList->SetPipelineState(m_ShadowPipelineState.get());
            commandList->SetConstantBuffer(0, m_FrameConstantBuffer.get());

            for (const auto& mesh : m_Model.Meshes)
            {
                commandList->SetVertexBuffer(mesh.VertexBuffer.get());
                commandList->SetIndexBuffer(mesh.IndexBuffer.get());
                commandList->DrawIndexed(mesh.IndexCount, 0, 0);
            }
        }

        // --- ジオメトリパス: G-Bufferへ書き込む(常に指定した内部解像度) ---
        RHI::Viewport gbufferViewport;
        gbufferViewport.Width = static_cast<float>(m_RenderWidth);
        gbufferViewport.Height = static_cast<float>(m_RenderHeight);
        commandList->SetViewport(gbufferViewport);

        RHI::IRHITexture* gbufferTargets[] = { m_GBufferAlbedo.get(), m_GBufferNormal.get(), m_GBufferMaterial.get() };
        commandList->SetRenderTargets(gbufferTargets, 3, m_GBufferDepth.get());
        commandList->ClearRenderTarget({ 0.0f, 0.0f, 0.0f, 0.0f });
        // Reverse-Zのため遠平面側(NDC z=0.0)にクリアする(GBuffer.hlsl参照)
        commandList->ClearDepth(0.0f);

        commandList->SetPipelineState(m_GBufferPipelineState.get());
        commandList->SetConstantBuffer(0, m_FrameConstantBuffer.get());
        commandList->SetSampler(0, m_Sampler.get());

        for (const auto& mesh : m_Model.Meshes)
        {
            MaterialConstants materialConstants{};
            materialConstants.MetallicFactor = mesh.MetallicFactor;
            materialConstants.RoughnessFactor = mesh.RoughnessFactor;
            commandList->UpdateBuffer(m_MaterialConstantBuffer.get(), &materialConstants, sizeof(materialConstants));
            commandList->SetConstantBuffer(1, m_MaterialConstantBuffer.get());

            commandList->SetVertexBuffer(mesh.VertexBuffer.get());
            commandList->SetIndexBuffer(mesh.IndexBuffer.get());
            commandList->SetTexture(0, mesh.BaseColorTexture);
            commandList->SetTexture(1, mesh.NormalTexture);
            commandList->SetTexture(2, mesh.MetallicRoughnessTexture);
            commandList->DrawIndexed(mesh.IndexCount, 0, 0);
        }

        // --- 直接光パス: G-Buffer+シャドウマップからPBRの直接光(拡散+鏡面反射、シャドウ適用済み)を
        //     計算しHDRで書き出す(常に指定した内部解像度)。DeferredLighting/SSILの両方から読まれる ---
        RHI::IRHITexture* directLightTarget[] = { m_DirectLightTexture.get() };
        commandList->SetRenderTargets(directLightTarget, 1, nullptr);
        commandList->SetViewport(gbufferViewport);

        commandList->SetPipelineState(m_DirectLightPipelineState.get());
        commandList->SetConstantBuffer(0, m_FrameConstantBuffer.get());
        commandList->SetSampler(0, m_Sampler.get());
        commandList->SetTexture(0, m_GBufferAlbedo.get());
        commandList->SetTexture(1, m_GBufferNormal.get());
        commandList->SetTexture(2, m_GBufferMaterial.get());
        commandList->SetTexture(3, m_GBufferDepth.get());
        commandList->SetTexture(4, m_ShadowMap.get());
        commandList->Draw(3, 0);

        // --- AO/GIパス: 選択中の手法(SSAO or SSIL)でG-Bufferから遮蔽率(・間接拡散光)を計算し、
        //     ブラーで均す(常に指定した内部解像度)。出力フォーマットはどちらもrgb=間接拡散光, a=遮蔽率で共通 ---
        if (m_AOEnabled)
        {
            commandList->SetViewport(gbufferViewport);
            commandList->SetConstantBuffer(0, m_FrameConstantBuffer.get());
            commandList->SetSampler(0, m_Sampler.get());

            RHI::IRHITexture* aoRawTexture = nullptr;
            RHI::IRHITexture* aoBlurredTexture = nullptr;

            if (m_AOTechnique == AOTechnique::SSAO)
            {
                SSAOConstants ssaoConstants{};
                std::copy(m_SSAOKernel.begin(), m_SSAOKernel.end(), ssaoConstants.Samples);
                ssaoConstants.Params = { m_SSAORadius, m_SSAORadius * 0.05f, m_SSAOPower, 0.0f };
                commandList->UpdateBuffer(m_SSAOConstantBuffer.get(), &ssaoConstants, sizeof(ssaoConstants));

                RHI::IRHITexture* ssaoRawTarget[] = { m_SSAORawTexture.get() };
                commandList->SetRenderTargets(ssaoRawTarget, 1, nullptr);

                commandList->SetPipelineState(m_SSAOPipelineState.get());
                commandList->SetConstantBuffer(1, m_SSAOConstantBuffer.get());
                commandList->SetTexture(0, m_GBufferNormal.get());
                commandList->SetTexture(1, m_GBufferDepth.get());
                commandList->Draw(3, 0);

                aoRawTexture = m_SSAORawTexture.get();
                aoBlurredTexture = m_SSAOTexture.get();
            }
            else
            {
                SSILConstants ssilConstants{};
                ssilConstants.Params0 = { m_SSILRadius, m_SSILThickness, m_SSILIntensity, m_SSILPower };
                ssilConstants.Params1 = { m_SSILSliceCount, m_SSILStepCount, 0u, 0u };
                commandList->UpdateBuffer(m_SSILConstantBuffer.get(), &ssilConstants, sizeof(ssilConstants));

                RHI::IRHITexture* ssilRawTarget[] = { m_SSILRawTexture.get() };
                commandList->SetRenderTargets(ssilRawTarget, 1, nullptr);

                commandList->SetPipelineState(m_SSILPipelineState.get());
                commandList->SetConstantBuffer(1, m_SSILConstantBuffer.get());
                commandList->SetTexture(0, m_GBufferNormal.get());
                commandList->SetTexture(1, m_GBufferDepth.get());
                commandList->SetTexture(2, m_DirectLightTexture.get());
                commandList->Draw(3, 0);

                aoRawTexture = m_SSILRawTexture.get();
                aoBlurredTexture = m_SSILTexture.get();
            }

            // ブラーパス: 遮蔽率・間接拡散光のタイル状ノイズをボックスブラーで均す(SSAO/SSIL共通シェーダ)
            RHI::IRHITexture* aoBlurTarget[] = { aoBlurredTexture };
            commandList->SetRenderTargets(aoBlurTarget, 1, nullptr);
            commandList->SetPipelineState(m_AOBlurPipelineState.get());
            commandList->SetTexture(0, aoRawTexture);
            commandList->Draw(3, 0);
        }

        // --- ライティングパス: G-Bufferを読み、SceneColorへ出力(常に指定した内部解像度) ---
        RHI::IRHITexture* sceneColorTarget[] = { m_SceneColor.get() };
        commandList->SetRenderTargets(sceneColorTarget, 1, nullptr);
        commandList->SetViewport(gbufferViewport);
        // 深度テストに失敗した(=何も描かれていない)ピクセル用の背景色。discardされた箇所に前フレームのデータが
        // 残らないよう、フルスクリーン三角形を描く前に明示的にクリアしておく
        commandList->ClearRenderTarget({ 0.05f, 0.05f, 0.08f, 1.0f });

        commandList->SetPipelineState(m_LightingPipelineState.get());
        commandList->SetConstantBuffer(0, m_FrameConstantBuffer.get());
        commandList->SetSampler(0, m_Sampler.get());
        commandList->SetTexture(0, m_GBufferAlbedo.get());
        commandList->SetTexture(1, m_DirectLightTexture.get());
        commandList->SetTexture(2, m_GBufferMaterial.get());
        commandList->SetTexture(3, m_GBufferDepth.get());
        commandList->SetTexture(4, m_SkyboxTexture.get());
        RHI::IRHITexture* activeAOTexture = m_AODisabledTexture.get();
        // デバッグ表示(ブラー前確認用)のため、ブラー前の生バッファへの参照も別途保持しておく
        RHI::IRHITexture* activeAORawTexture = m_AODisabledTexture.get();
        if (m_AOEnabled)
        {
            activeAOTexture = (m_AOTechnique == AOTechnique::SSAO) ? m_SSAOTexture.get() : m_SSILTexture.get();
            activeAORawTexture = (m_AOTechnique == AOTechnique::SSAO) ? m_SSAORawTexture.get() : m_SSILRawTexture.get();
        }
        commandList->SetTexture(5, activeAOTexture);
        commandList->Draw(3, 0);

        // --- Presentパス: 選択中のレンダーターゲットを、アスペクト比を保ってバックバッファへ出力 ---
        // デバッグ表示(Render Targets UI)で選択されたバッファに応じて表示ソースを切り替える。
        // 深度バッファ(GBuffer深度・シャドウマップ)はPresent.hlsl側でグレースケール化するためMode=1を渡す
        RHI::IRHITexture* presentSourceTexture = m_SceneColor.get();
        int32_t presentMode = 0;
        uint32_t presentSourceWidth = m_RenderWidth;
        uint32_t presentSourceHeight = m_RenderHeight;
        switch (m_DebugView)
        {
        case DebugView::Final:
            presentSourceTexture = m_SceneColor.get();
            break;
        case DebugView::Albedo:
            presentSourceTexture = m_GBufferAlbedo.get();
            break;
        case DebugView::Normal:
            presentSourceTexture = m_GBufferNormal.get();
            break;
        case DebugView::Material:
            presentSourceTexture = m_GBufferMaterial.get();
            break;
        case DebugView::Depth:
            presentSourceTexture = m_GBufferDepth.get();
            presentMode = 2;
            break;
        case DebugView::DepthRaw:
            presentSourceTexture = m_GBufferDepth.get();
            presentMode = 5; // 生の深度値(0〜1)を加工せずそのまま表示(reverse-z等の生値確認用)
            break;
        case DebugView::DirectLight:
            presentSourceTexture = m_DirectLightTexture.get();
            presentMode = 4; // HDRのためトーンマッピング(Reinhard)+ガンマ補正して表示
            break;
        case DebugView::AOIndirectLight:
            presentSourceTexture = activeAOTexture;
            presentMode = 0; // rgb(間接拡散光)をそのまま表示。SSAOはrgbが常に0のため常に黒になる
            break;
        case DebugView::AOIndirectLightRaw:
            presentSourceTexture = activeAORawTexture;
            presentMode = 0; // ブラー前の生値(タイル状ノイズが乗った状態)
            break;
        case DebugView::AOOcclusion:
            presentSourceTexture = activeAOTexture;
            presentMode = 3; // a(遮蔽率)をグレースケール表示
            break;
        case DebugView::AOOcclusionRaw:
            presentSourceTexture = activeAORawTexture;
            presentMode = 3; // ブラー前の生値(タイル状ノイズが乗った状態)
            break;
        case DebugView::ShadowMap:
            presentSourceTexture = m_ShadowMap.get();
            presentMode = 1;
            presentSourceWidth = kShadowMapSize;
            presentSourceHeight = kShadowMapSize;
            break;
        }

        PresentConstants presentConstants{};
        presentConstants.Mode = presentMode;
        commandList->UpdateBuffer(m_PresentConstantBuffer.get(), &presentConstants, sizeof(presentConstants));

        commandList->SetRenderTarget(m_SwapChain.get());
        commandList->ClearRenderTarget({ 0.05f, 0.05f, 0.08f, 1.0f });
        commandList->ClearDepth(1.0f);

        // レターボックス/ピラーボックスの余白もクリア色のまま残るよう、絞ったビューポートで描画する
        const RHI::Viewport letterboxViewport = ComputeLetterboxViewport(
            m_Window->GetWidth(), m_Window->GetHeight(), presentSourceWidth, presentSourceHeight);
        commandList->SetViewport(letterboxViewport);

        commandList->SetPipelineState(m_PresentPipelineState.get());
        commandList->SetConstantBuffer(0, m_FrameConstantBuffer.get());
        commandList->SetConstantBuffer(1, m_PresentConstantBuffer.get());
        commandList->SetSampler(0, m_Sampler.get());
        commandList->SetTexture(0, presentSourceTexture);
        commandList->Draw(3, 0);

        // ImGuiはPresentパスでバインドされたバックバッファにそのまま重ねて描画する
        m_ImGuiBackend->Render();

        m_SwapChain->Present(true);
    }
}
