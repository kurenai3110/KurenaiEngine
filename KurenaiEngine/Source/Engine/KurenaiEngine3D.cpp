#include "KurenaiEngine3D.h"

#include <imgui.h>

#include <objbase.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <random>

#include "Assets/ModelLoader.h"
#include "Core/RenderGraph.h"

namespace Kurenai
{
    namespace
    {
        // 呼び出し元(exe)ではなくKurenaiEngine.dll自身のフォルダを返す。
        // Shaders/AssetsはDLLと同じフォルダに配置される運用のため、DLLがどこにコピー
        // されて使われても(各サンプルのBuildフォルダ配下など)データを正しく解決できる
        std::wstring GetModuleDirectory()
        {
            HMODULE module = nullptr;
            GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&GetModuleDirectory),
                &module);

            wchar_t path[MAX_PATH];
            GetModuleFileNameW(module, path, MAX_PATH);
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

        SunLighting ComputeSunLighting(float timeOfDayHours, float sunAzimuthDegrees)
        {
            using namespace DirectX;

            // 日の出(東)側の水平方向。太陽はこの方向と天頂(真上)を通る鉛直面内で、
            // 東→天頂(正午)→西→天底(真夜中)と一日一周する半円軌道を描く。
            // 方位角(sunAzimuthDegrees)はX軸を0度、Z軸(+方向)を90度としてImGuiで調整する
            const float azimuthRadians = XMConvertToRadians(sunAzimuthDegrees);
            const XMFLOAT3 kSunriseHorizontal{ std::cos(azimuthRadians), 0.0f, std::sin(azimuthRadians) };

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

        // GBuffer.hlsl側のcbuffer MaterialConstantsと一致させる必要がある。float3(EmissiveFactor)は
        // HLSLのcbufferパッキング規則により16バイト境界をまたげないため、直前にPaddingを1つ挟んで
        // オフセット16から始まるようにしている(挟まなくてもHLSLコンパイラが自動的に同じ位置へ
        // パディングするが、C++側のレイアウトを明示的に一致させるためここでも挟む)
        struct alignas(16) MaterialConstants
        {
            float MetallicFactor;
            float RoughnessFactor;
            // 0以下ならアルファカットアウト無効
            float AlphaCutoff;
            float Padding0;
            float EmissiveFactor[3];
            float Padding1;
        };

        // Present.hlsl側のModeと一致させる必要がある
        struct alignas(16) PresentConstants
        {
            int32_t Mode;
            float MipLevel; // Mode==6(Hi-Z)でSampleLevelに渡すミップレベル
            float Padding[2];
        };

        // HiZ.hlsl側のcbuffer HiZConstantsと一致させる必要がある
        struct alignas(16) HiZConstants
        {
            DirectX::XMUINT2 SrcSize;
            DirectX::XMUINT2 DstSize;
        };

        // widthとheightのうち大きい方が1になるまでのミップ数(width/heightそのものを含む)を返す。
        // 例: 1280x720 -> max=1280 -> 1280,640,320,160,80,40,20,10,5,2,1 の11ミップ
        uint32_t ComputeMipLevelCount(uint32_t width, uint32_t height)
        {
            uint32_t levels = 1;
            uint32_t size = std::max(width, height);
            while (size > 1)
            {
                size /= 2;
                ++levels;
            }
            return levels;
        }

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

        // SSR.hlsl側のcbuffer SSRConstantsと一致させる必要がある
        struct alignas(16) SSRConstants
        {
            DirectX::XMFLOAT4 Params0; // x: 最大レイ距離, y: ヒット判定の厚み, z: ラフネスカットオフ, w: 未使用
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
            // 密集した屋外の街区全体を包む巨大なバウンディングボックスを持つアセットでは、
            // ホール用ヒューリスティック(バウンズを20%内側に入った位置)では建物の壁の中に埋まってしまうため、
            // そのような場合に実際の描画結果を確認して選んだ初期位置を明示的に指定できるようにしている
            bool HasCameraOverride = false;
            float CameraPosition[3] = { 0.0f, 0.0f, 0.0f };
            float CameraYaw = 0.0f;
        };

        const SceneEntry kScenes[] =
        {
            { L"Sponza", L"Assets\\Sponza\\Sponza.gltf" },
            { L"Bistro (McGuire) - Exterior", L"Assets\\BistroMcGuire\\Exterior_gltf\\exterior.gltf", true, { 21.5f, 16.0f, -53.5f }, 0.0f },
            { L"Bistro (McGuire) - Interior", L"Assets\\BistroMcGuire\\Interior_gltf\\interior.gltf" },
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

    KurenaiEngine3D::KurenaiEngine3D(GraphicsAPI api, uint32_t renderWidth, uint32_t renderHeight)
        : KurenaiEngineBase(L"Kurenai Engine", 1280, 720, api)
        , m_GraphicsAPI(api)
        , m_RenderWidth(renderWidth)
        , m_RenderHeight(renderHeight)
    {
        m_ImGuiBackend = m_Device->CreateImGuiBackend(m_Window->GetHandle());
        m_GPUProfiler = m_Device->CreateGPUProfiler();

        // imgui.iniの保存先を起動時の作業ディレクトリに依存させず、KurenaiEngine.dllと同じフォルダに固定する。
        // ImGuiはIniFilenameのポインタを保持するだけでコピーしないため、m_ImGuiIniPathで寿命を維持する
        m_ImGuiIniPath = WideToUtf8((GetModuleDirectory() + L"imgui.ini").c_str());
        ImGui::GetIO().IniFilename = m_ImGuiIniPath.c_str();

        m_Camera.SetAspectRatio(static_cast<float>(m_RenderWidth) / static_cast<float>(m_RenderHeight));

        CreateSceneResources();

        m_LastFrameTime = std::chrono::steady_clock::now();
    }

    KurenaiEngine3D::~KurenaiEngine3D() = default;

    void KurenaiEngine3D::CreateSceneResources()
    {
        // Shaders/AssetsはビルドでKurenaiEngine.dllと同じフォルダにコピーされる
        const std::wstring dataRoot = GetModuleDirectory();
        const std::wstring shaderDirectory = dataRoot + L"Shaders\\";

        const std::vector<RHI::InputElementDesc> modelInputLayout =
        {
            { "POSITION", 0, RHI::Format::R32G32B32_Float, 0 },
            { "NORMAL", 0, RHI::Format::R32G32B32_Float, 12 },
            { "TEXCOORD", 0, RHI::Format::R32G32_Float, 24 },
            { "TANGENT", 0, RHI::Format::R32G32B32A32_Float, 32 },
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
        gbufferPipelineDesc.RenderTargetFormats =
        {
            RHI::Format::R8G8B8A8_UNorm, // Albedo
            RHI::Format::R16G16_Float,   // Normal(オクタヘドラルエンコード)
            RHI::Format::R8G8B8A8_UNorm, // Material(R=Metallic, G=Roughness)
            RHI::Format::R8G8B8A8_UNorm, // Emissive
        };
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
        lightingPipelineDesc.RenderTargetFormats = { RHI::Format::R16G16B16A16_Float };
        m_LightingPipelineState = m_Device->CreatePipelineState(lightingPipelineDesc);

        // Hi-Zミップチェーン構築パス(コンピュートシェーダー)。CSCopyでG-Buffer深度をミップ0へコピーし、
        // CSDownsampleをミップ数-1回ディスパッチして1x1まで縮小する
        RHI::ShaderDesc hizCopyCsDesc;
        hizCopyCsDesc.Stage = RHI::ShaderStage::Compute;
        hizCopyCsDesc.FilePath = shaderDirectory + L"HiZ.hlsl";
        hizCopyCsDesc.EntryPoint = "CSCopy";
        m_HiZCopyComputeShader = m_Device->CreateShader(hizCopyCsDesc);
        m_HiZCopyPipelineState = m_Device->CreateComputePipelineState({ m_HiZCopyComputeShader.get() });

        RHI::ShaderDesc hizDownsampleCsDesc;
        hizDownsampleCsDesc.Stage = RHI::ShaderStage::Compute;
        hizDownsampleCsDesc.FilePath = shaderDirectory + L"HiZ.hlsl";
        hizDownsampleCsDesc.EntryPoint = "CSDownsample";
        m_HiZDownsampleComputeShader = m_Device->CreateShader(hizDownsampleCsDesc);
        m_HiZDownsamplePipelineState = m_Device->CreateComputePipelineState({ m_HiZDownsampleComputeShader.get() });

        RHI::BufferDesc hizConstantBufferDesc;
        hizConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        hizConstantBufferDesc.SizeInBytes = sizeof(HiZConstants);
        m_HiZConstantBuffer = m_Device->CreateBuffer(hizConstantBufferDesc);

        // SSRパス(頂点バッファなしのフルスクリーン三角形。SceneColorとG-Bufferから鏡面反射を計算し加算する)
        RHI::ShaderDesc ssrVsDesc;
        ssrVsDesc.Stage = RHI::ShaderStage::Vertex;
        ssrVsDesc.FilePath = shaderDirectory + L"SSR.hlsl";
        ssrVsDesc.EntryPoint = "VSMain";
        m_SSRVertexShader = m_Device->CreateShader(ssrVsDesc);

        RHI::ShaderDesc ssrPsDesc;
        ssrPsDesc.Stage = RHI::ShaderStage::Pixel;
        ssrPsDesc.FilePath = shaderDirectory + L"SSR.hlsl";
        ssrPsDesc.EntryPoint = "PSMain";
        m_SSRPixelShader = m_Device->CreateShader(ssrPsDesc);

        RHI::PipelineStateDesc ssrPipelineDesc;
        ssrPipelineDesc.VertexShader = m_SSRVertexShader.get();
        ssrPipelineDesc.PixelShader = m_SSRPixelShader.get();
        ssrPipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
        ssrPipelineDesc.RenderTargetFormats = { RHI::Format::R16G16B16A16_Float };
        m_SSRPipelineState = m_Device->CreatePipelineState(ssrPipelineDesc);

        RHI::BufferDesc ssrConstantBufferDesc;
        ssrConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        ssrConstantBufferDesc.SizeInBytes = sizeof(SSRConstants);
        m_SSRConstantBuffer = m_Device->CreateBuffer(ssrConstantBufferDesc);

        // Tonemapパス(頂点バッファなしのフルスクリーン三角形。HDRのSceneColorをLDRへ変換する)
        RHI::ShaderDesc tonemapVsDesc;
        tonemapVsDesc.Stage = RHI::ShaderStage::Vertex;
        tonemapVsDesc.FilePath = shaderDirectory + L"Tonemap.hlsl";
        tonemapVsDesc.EntryPoint = "VSMain";
        m_TonemapVertexShader = m_Device->CreateShader(tonemapVsDesc);

        RHI::ShaderDesc tonemapPsDesc;
        tonemapPsDesc.Stage = RHI::ShaderStage::Pixel;
        tonemapPsDesc.FilePath = shaderDirectory + L"Tonemap.hlsl";
        tonemapPsDesc.EntryPoint = "PSMain";
        m_TonemapPixelShader = m_Device->CreateShader(tonemapPsDesc);

        RHI::PipelineStateDesc tonemapPipelineDesc;
        tonemapPipelineDesc.VertexShader = m_TonemapVertexShader.get();
        tonemapPipelineDesc.PixelShader = m_TonemapPixelShader.get();
        tonemapPipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
        tonemapPipelineDesc.RenderTargetFormats = { RHI::Format::R8G8B8A8_UNorm };
        m_TonemapPipelineState = m_Device->CreatePipelineState(tonemapPipelineDesc);

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
        m_SkyboxTexture = m_Device->CreateTextureFromFile(dataRoot + L"Assets\\Skybox\\Sky.dds", false);

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

    void KurenaiEngine3D::CreateRenderTargets(uint32_t width, uint32_t height)
    {
        if (width == 0 || height == 0)
        {
            return;
        }

        m_GBufferAlbedo = m_Device->CreateRenderTexture(width, height, RHI::Format::R8G8B8A8_UNorm);
        m_GBufferNormal = m_Device->CreateRenderTexture(width, height, RHI::Format::R16G16_Float);
        m_GBufferMaterial = m_Device->CreateRenderTexture(width, height, RHI::Format::R8G8B8A8_UNorm);
        m_GBufferEmissive = m_Device->CreateRenderTexture(width, height, RHI::Format::R8G8B8A8_UNorm);
        // Reverse-Zのため近平面側(NDC z=1.0)ではなく遠平面側(NDC z=0.0)にクリアする
        m_GBufferDepth = m_Device->CreateDepthTexture(width, height, 0.0f);
        m_DirectLightTexture = m_Device->CreateRenderTexture(width, height, RHI::Format::R32G32B32A32_Float);
        m_SSAORawTexture = m_Device->CreateRenderTexture(width, height, RHI::Format::R8G8B8A8_UNorm);
        m_SSAOTexture = m_Device->CreateRenderTexture(width, height, RHI::Format::R8G8B8A8_UNorm);
        m_SSILRawTexture = m_Device->CreateRenderTexture(width, height, RHI::Format::R8G8B8A8_UNorm);
        m_SSILTexture = m_Device->CreateRenderTexture(width, height, RHI::Format::R8G8B8A8_UNorm);
        m_SceneColor = m_Device->CreateRenderTexture(width, height, RHI::Format::R16G16B16A16_Float);
        m_SSRTexture = m_Device->CreateRenderTexture(width, height, RHI::Format::R16G16B16A16_Float);
        m_TonemapTexture = m_Device->CreateRenderTexture(width, height, RHI::Format::R8G8B8A8_UNorm);

        m_HiZMipLevels = ComputeMipLevelCount(width, height);
        m_HiZTexture = m_Device->CreateHiZTexture(width, height, m_HiZMipLevels);
        m_HiZDebugMipLevel = 0;
    }

    void KurenaiEngine3D::LoadScene(size_t sceneIndex)
    {
        if (sceneIndex >= kSceneCount)
        {
            return;
        }

        // m_Model/m_Camera/Post ProcessingパラメータはRender()(Renderスレッド)も読み書きするため、
        // この関数全体をm_SceneMutexで保護する(詳細はm_SceneMutexのコメント参照)。UpdateSceneSwitch
        // (Updateスレッド)からのみ呼ばれる前提のため、Renderスレッドとの競合はこれで排他できる
        std::lock_guard<std::mutex> sceneLock(m_SceneMutex);

        const std::wstring modelPath = GetModuleDirectory() + kScenes[sceneIndex].RelativePath;

        // 旧シーン(m_Model)のバッファ/テクスチャを破棄する前に、GPUが旧シーンを参照する
        // コマンド(直前まで提出されていた描画コマンド)の実行を終えるまで待つ。特にDX12は
        // CPUがGPU完了を待たずに次フレームの記録を始める多重バッファリング設計のため、
        // これを省くとGPUがまだ読んでいるバッファ/テクスチャを解放してしまい、
        // ヒープ破損によるクラッシュを引き起こす(詳細はIRHIDevice::WaitForGPUIdleのコメント参照)
        m_Device->WaitForGPUIdle();

        // Assets::LoadModelの戻り値(新シーンの全テクスチャ/バッファ)を作り終えてから代入すると、
        // 代入演算子が旧m_Modelを破棄するまでの間、新旧シーンのGPUリソース(特にDX12の
        // 非シェーダー可視SRVディスクリプタ)が同時に確保された状態になり、大規模シーンでは
        // ディスクリプタヒープを圧迫する。先に空のModelで置き換えて旧シーンを解放しておく
        // (直前のWaitForGPUIdleによりGPUはもう旧シーンを参照していないため安全)
        m_Model = Assets::Model{};
        m_Model = Assets::LoadModel(*m_Device, modelPath);
        m_CurrentSceneIndex = sceneIndex;

        FrameCameraToModel();

        const wchar_t* apiName = (m_GraphicsAPI == GraphicsAPI::DX12) ? L"DX12" : L"DX11";
        m_Window->SetTitle(std::wstring(L"Kurenai Engine [") + apiName + L"] - " + kScenes[sceneIndex].DisplayName);
    }

    void KurenaiEngine3D::FrameCameraToModel()
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

        // SSRの最大レイ距離もシーンの規模に応じて変わるべきなので、対角線に比例させる。
        // ヒット判定の厚みはSSAO/SSILと同様、遮蔽・接触判定として妥当な小さい値にする
        m_SSRMaxDistance = std::clamp(diagonal * 0.5f, 1.0f, 100.0f);
        m_SSRThickness = m_SSAORadius * 0.2f;

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
    DirectX::XMMATRIX KurenaiEngine3D::ComputeLightViewProj(const DirectX::XMFLOAT3& lightDirection) const
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

    void KurenaiEngine3D::RenderSceneSwitchUI()
    {
        ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(260.0f, 0.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("Scenes");

        ImGui::TextUnformatted(m_GraphicsAPI == GraphicsAPI::DX12 ? "Graphics API: DX12" : "Graphics API: DX11");
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
                // LoadScene自体はUpdateスレッドから呼ぶ必要があるため、ここでは要求を書き込むだけにする
                // (UpdateSceneSwitch参照)
                m_PendingSceneIndex.store(static_cast<int>(i));
            }

            if (isCurrent)
            {
                ImGui::EndDisabled();
            }
        }

        ImGui::End();
    }

    void KurenaiEngine3D::RenderPostProcessUI()
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

        ImGui::Checkbox("Enable VSync", &m_VSyncEnabled);

        ImGui::Checkbox("Fixed FPS", &m_FixedFPSEnabled);
        if (m_FixedFPSEnabled)
        {
            static const char* kTargetFPSNames[] = { "30", "60", "120" };
            static const float kTargetFPSValues[] = { 30.0f, 60.0f, 120.0f };
            int targetFPSIndex = 1; // 見つからない場合は60fps相当の位置にしておく
            for (int i = 0; i < IM_ARRAYSIZE(kTargetFPSValues); ++i)
            {
                if (kTargetFPSValues[i] == m_TargetFPS)
                {
                    targetFPSIndex = i;
                    break;
                }
            }
            if (ImGui::Combo("Target FPS", &targetFPSIndex, kTargetFPSNames, IM_ARRAYSIZE(kTargetFPSNames)))
            {
                m_TargetFPS = kTargetFPSValues[targetFPSIndex];
            }
        }

        ImGui::Checkbox("Enable SSR", &m_SSREnabled);
        if (m_SSREnabled)
        {
            ImGui::SliderFloat("SSR Max Distance", &m_SSRMaxDistance, 0.1f, 100.0f);
            ImGui::SliderFloat("SSR Thickness", &m_SSRThickness, 0.01f, 2.0f);
            ImGui::SliderFloat("SSR Roughness Cutoff", &m_SSRRoughnessCutoff, 0.05f, 1.0f);
        }

        ImGui::End();
    }

    void KurenaiEngine3D::RenderDebugViewUI()
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
            "Emissive",
            "Depth",
            "Depth (Raw)",
            "Direct Light",
            "AO/GI - Indirect Light (RGB)",
            "AO/GI - Indirect Light (RGB, Before Blur)",
            "AO/GI - Occlusion (Alpha)",
            "AO/GI - Occlusion (Alpha, Before Blur)",
            "Shadow Map",
            "SSR (Final + Reflections)",
            "Hi-Z (Depth Mip Chain)",
        };

        int currentIndex = static_cast<int>(m_DebugView);
        if (ImGui::Combo("View", &currentIndex, kDebugViewNames, IM_ARRAYSIZE(kDebugViewNames)))
        {
            m_DebugView = static_cast<DebugView>(currentIndex);
        }

        if (m_DebugView == DebugView::HiZ)
        {
            ImGui::SliderInt("Hi-Z Mip Level", &m_HiZDebugMipLevel, 0, static_cast<int>(m_HiZMipLevels) - 1);
        }

        ImGui::End();
    }

    void KurenaiEngine3D::RenderLightingUI()
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
        ImGui::SliderFloat("Sun Azimuth", &m_SunAzimuthDegrees, 0.0f, 360.0f, "%.1f deg");

        ImGui::End();
    }

    void KurenaiEngine3D::RenderProfilerUI()
    {
        ImGui::SetNextWindowPos(ImVec2(280.0f, 280.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(280.0f, 460.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("Profiler");

        ImGui::Text("FPS: %.1f", m_FPS);
        ImGui::Text("CPU Frame Time: %.3f ms", m_CPUFrameTimeMs);
        ImGui::Text("GPU Frame Time: %.3f ms", m_GPUProfiler->GetTotalFrameTimeMs());
        // GPUの完了待ち(DX12のフレームパイプライン化に伴うフェンス待ち)。CPU Frame Timeや
        // PresentSubmitの計測値からは既に除外済みなので、参考情報として別枠で表示する
        ImGui::Text("GPU Wait: %.3f ms", m_Device->GetLastFrameGPUWaitTimeMs());
        ImGui::Separator();
        ImGui::TextUnformatted("CPU Pass Breakdown:");
        for (const auto& result : m_CPUProfiler.GetResults())
        {
            ImGui::Text("  %s: %.3f ms", result.Name.c_str(), result.TimeMs);
        }
        ImGui::Separator();
        ImGui::TextUnformatted("GPU Pass Breakdown:");
        for (const auto& result : m_GPUProfiler->GetResults())
        {
            ImGui::Text("  %s: %.3f ms", result.Name.c_str(), result.TimeMs);
        }

        ImGui::End();
    }

    void KurenaiEngine3D::Run()
    {
        // 描画専用スレッドを起動する。以後このスレッドがRender()の呼び出しとPresentを担当し、
        // 呼び出し元スレッド(以下Updateスレッド)はPumpMessages/Updateに専念する
        m_RenderThread = std::thread(&KurenaiEngine3D::RenderThreadMain, this);

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

            // m_CameraはUpdateスレッド(UpdateMouseLook/UpdateMovement/LoadScene経由のFrameCameraToModel)
            // のみが書き込み、Render()はframeStateのスナップショット経由でしか読まないため、
            // ここでの読み取りに追加のロックは不要
            FrameState newFrameState;
            newFrameState.Camera = m_Camera;
            newFrameState.ImGuiVisible = m_ImGuiVisible;

            // Renderスレッドが直前フレーム分を取り込み終えるまで待つ(キュー深度1)。
            // 取り込み自体はスナップショットのコピーだけなので即座に完了し、その後の重いGPU発行は
            // このUpdateスレッドの次フレーム処理と並行して進む
            {
                std::unique_lock<std::mutex> lock(m_FrameStateMutex);
                m_FrameStateCV.wait(lock, [this] { return m_FrameStateTaken; });
                m_FrameState = newFrameState;
                m_FrameStateReady = true;
                m_FrameStateTaken = false;
            }
            m_FrameStateCV.notify_one();
        }

        {
            std::lock_guard<std::mutex> lock(m_FrameStateMutex);
            m_StopRenderThread = true;
        }
        m_FrameStateCV.notify_one();
        m_RenderThread.join();
    }

    void KurenaiEngine3D::RenderThreadMain()
    {
        // LoadScene(RenderSceneSwitchUI経由でこのスレッドから呼ばれる)がWICテクスチャ読み込みで
        // COMを使用する。COMはスレッドごとに初期化が必要(wWinMainでのCoInitializeExはUpdate
        // スレッド=呼び出し元スレッドにしか適用されない)なため、この描画スレッドでも初期化しておく。
        // 未初期化のままだとWIC呼び出しがハングする(Main.cppと同じAPARTMENTTHREADEDに揃える)
        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

        m_LastRenderFrameTime = std::chrono::steady_clock::now();

        for (;;)
        {
            FrameState frameState;
            {
                std::unique_lock<std::mutex> lock(m_FrameStateMutex);
                m_FrameStateCV.wait(lock, [this] { return m_FrameStateReady || m_StopRenderThread; });
                if (m_StopRenderThread && !m_FrameStateReady)
                {
                    break;
                }
                frameState = m_FrameState;
                m_FrameStateReady = false;
                m_FrameStateTaken = true;
            }
            m_FrameStateCV.notify_one();

            const auto now = std::chrono::steady_clock::now();
            const float renderDeltaTime = std::chrono::duration<float>(now - m_LastRenderFrameTime).count();
            m_LastRenderFrameTime = now;

            // 昼夜サイクルの自動進行はUpdateスレッドではなくこちら(Renderスレッド)で行う。
            // m_TimeOfDay/m_TimeAutoAdvance/m_TimeAdvanceSpeedはImGuiパネル(RenderLightingUI、
            // Renderスレッドから描画)でも書き換えられるため、両方をRenderスレッド専有にすることで
            // 追加の排他制御なしに済ませられる
            if (m_TimeAutoAdvance)
            {
                m_TimeOfDay = std::fmod(m_TimeOfDay + m_TimeAdvanceSpeed * renderDeltaTime, 24.0f);
                if (m_TimeOfDay < 0.0f)
                {
                    m_TimeOfDay += 24.0f;
                }
            }

            const auto cpuStart = std::chrono::steady_clock::now();
            {
                // WM_SIZEによるスワップチェーンのリサイズ、およびLoadScene(Updateスレッド、
                // UpdateSceneSwitch経由)によるm_Model/m_Camera/Post Processingパラメータの書き換えと
                // 同時に走らないよう、Render()全体をこれらのミューテックスで保護する。この2つの
                // ミューテックスをこの組み合わせ・この順序でロックするのはここだけなので、
                // std::scoped_lockでなくてもデッドロックの心配はないが、明示的にまとめて扱っておく
                std::scoped_lock renderLock(m_SwapChainMutex, m_SceneMutex);
                Render(frameState);
            }
            const auto cpuEnd = std::chrono::steady_clock::now();
            // GPUの完了待ち(DX12のフレームパイプライン化に伴うフェンス待ち)は実際のCPU負荷ではなく
            // GPU側の処理時間の反映なので差し引く(DX11は常に0が返るため影響しない)
            const float rawCPUTimeMs = std::chrono::duration<float, std::milli>(cpuEnd - cpuStart).count();
            m_CPUFrameTimeMs = std::max(0.0f, rawCPUTimeMs - m_Device->GetLastFrameGPUWaitTimeMs());

            // 固定FPSモード: このフレームの処理(Time of Day更新+Render+Present)が目標フレーム時間
            // より短く終わった場合、余った時間だけ待機して間隔を揃える。CPU/GPU計測(上記)の後に
            // 行うことで、この待機時間自体がプロファイラの計測値に混ざらないようにしている
            if (m_FixedFPSEnabled && m_TargetFPS > 0.0f)
            {
                const auto targetFrameDuration = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(1.0 / m_TargetFPS));
                const auto frameDeadline = now + targetFrameDuration;
                if (std::chrono::steady_clock::now() < frameDeadline)
                {
                    std::this_thread::sleep_until(frameDeadline);
                }
            }

            // FPSは指数移動平均で平滑化する(生の1/deltaTimeだとフレームごとの揺れが大きく読み取りにくいため)
            if (renderDeltaTime > 0.0f)
            {
                const float instantFPS = 1.0f / renderDeltaTime;
                m_FPS = (m_FPS == 0.0f) ? instantFPS : (m_FPS * 0.9f + instantFPS * 0.1f);
            }
        }

        if (SUCCEEDED(comResult))
        {
            CoUninitialize();
        }
    }

    void KurenaiEngine3D::UpdateMouseLook()
    {
        // このメソッドだけは意図的にGetAsyncKeyState/GetCursorPos/SetCursorPosを使い続けている。
        // カーソルを画面中央へ強制的に固定し続ける(SetCursorPos)ことで無限ドラッグを実現しており、
        // これは実カーソルを動かす・隠す操作そのものであるため、メッセージベース化(PostMessageで
        // WM_RBUTTONDOWN/WM_MOUSEMOVEを送るだけで発火する形)にしてしまうと、動作確認用の
        // PostMessage送信が実デスクトップのカーソルを意図せず動かし・隠してしまう経路になる。
        // GetAsyncKeyState(VK_RBUTTON)はPostMessageでは変化しない実ハードウェアの状態のため、
        // このままにしておくことでPostMessageによる動作確認が誤ってカーソル操作を引き起こさない
        // (=実カーソル・他ウィンドウに影響を与えない)ことを構造的に保証している
        //
        // GetAsyncKeyStateはウィンドウフォーカスに関係なくグローバルなキー状態を返すため、
        // フォアグラウンドウィンドウチェックがないとデスクトップ上の右クリックでも
        // カーソルがウィンドウ中央へ強制移動してしまう
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

    void KurenaiEngine3D::UpdateMovement(float deltaTime)
    {
        // メッセージベースの入力API(IsKeyDown)を使う。GetAsyncKeyStateと異なりウィンドウが
        // フォーカスを失っている間は反応せず、PostMessageによるテスト自動化とも整合する
        const float moveSpeed = IsKeyDown(VK_SHIFT) ? 20.0f : 5.0f;
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

        if (IsKeyDown('W')) add(forward, 1.0f);
        if (IsKeyDown('S')) add(forward, -1.0f);
        if (IsKeyDown('D')) add(right, 1.0f);
        if (IsKeyDown('A')) add(right, -1.0f);
        if (IsKeyDown('E')) move.y += 1.0f;
        if (IsKeyDown('Q')) move.y -= 1.0f;

        DirectX::XMVECTOR moveVec = DirectX::XMLoadFloat3(&move);
        if (DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(moveVec)) > 0.0001f)
        {
            moveVec = DirectX::XMVectorScale(DirectX::XMVector3Normalize(moveVec), moveAmount);
            DirectX::XMFLOAT3 delta;
            DirectX::XMStoreFloat3(&delta, moveVec);
            m_Camera.Move(delta);
        }
    }

    void KurenaiEngine3D::UpdateImGuiToggle()
    {
        // WasKeyPressedはウィンドウメッセージ由来のエッジ検出を内蔵しているため、
        // 前フレームの押下状態を自前で保持する必要がない
        if (WasKeyPressed(VK_F1))
        {
            m_ImGuiVisible = !m_ImGuiVisible;
        }
    }

    void KurenaiEngine3D::UpdateSceneSwitch()
    {
        // -1は「切り替え要求なし」を表す番兵値。exchangeで読み取りと同時に-1へ戻すことで、
        // 同じ要求を二重に処理しない
        const int pendingIndex = m_PendingSceneIndex.exchange(-1);
        if (pendingIndex >= 0)
        {
            LoadScene(static_cast<size_t>(pendingIndex));
        }
    }

    void KurenaiEngine3D::Update(float deltaTime)
    {
        UpdateMouseLook();
        UpdateMovement(deltaTime);
        UpdateImGuiToggle();
        UpdateSceneSwitch();
        // 昼夜サイクルの自動進行(m_TimeOfDay)はRenderThreadMain側で行う(RenderThreadMain参照)
    }

    void KurenaiEngine3D::Render(const FrameState& frameState)
    {
        if (m_Window->GetWidth() == 0 || m_Window->GetHeight() == 0)
        {
            return;
        }

        // WndProc(Updateスレッド)でキューイングされたメッセージを、ImGuiの状態を実際に読み書きする
        // このRenderスレッド自身からImGui_ImplWin32_WndProcHandlerへ転送する。ImGui::NewFrame()より前に
        // 行うことで、このフレームのNewFrame()が最新のマウス/キーボード状態を反映できる
        m_Window->ForwardQueuedMessagesToImGui();

        m_ImGuiBackend->NewFrame();
        if (frameState.ImGuiVisible)
        {
            RenderSceneSwitchUI();
            RenderPostProcessUI();
            RenderDebugViewUI();
            RenderLightingUI();
            RenderProfilerUI();
        }

        auto* commandList = m_Device->GetImmediateCommandList();
        m_GPUProfiler->BeginFrame();
        m_CPUProfiler.BeginFrame();

        const SunLighting sunLighting = ComputeSunLighting(m_TimeOfDay, m_SunAzimuthDegrees);
        const DirectX::XMMATRIX lightViewProj = ComputeLightViewProj(sunLighting.Direction);

        FrameConstants constants;
        const DirectX::XMMATRIX viewProj = frameState.Camera.GetViewMatrix() * frameState.Camera.GetProjectionMatrix();
        DirectX::XMStoreFloat4x4(&constants.ViewProj, DirectX::XMMatrixTranspose(viewProj));
        DirectX::XMVECTOR determinant;
        const DirectX::XMMATRIX invViewProj = DirectX::XMMatrixInverse(&determinant, viewProj);
        DirectX::XMStoreFloat4x4(&constants.InvViewProj, DirectX::XMMatrixTranspose(invViewProj));
        DirectX::XMStoreFloat4x4(&constants.LightViewProj, DirectX::XMMatrixTranspose(lightViewProj));
        const DirectX::XMFLOAT3 cameraPosition = frameState.Camera.GetPosition();
        constants.CameraPosition = { cameraPosition.x, cameraPosition.y, cameraPosition.z, 0.0f };
        constants.LightDirection = { sunLighting.Direction.x, sunLighting.Direction.y, sunLighting.Direction.z, 0.0f };
        constants.LightColor = sunLighting.Color;
        DirectX::XMStoreFloat4x4(&constants.View, DirectX::XMMatrixTranspose(frameState.Camera.GetViewMatrix()));
        DirectX::XMStoreFloat4x4(&constants.Proj, DirectX::XMMatrixTranspose(frameState.Camera.GetProjectionMatrix()));
        constants.AmbientColor = sunLighting.Ambient;
        commandList->UpdateBuffer(m_FrameConstantBuffer.get(), &constants, sizeof(constants));

        // 各パスをリソースの読み書き依存関係から自動的に順序付けて実行するレンダーグラフ。
        // トランジェントリソースの確保は行わず、既存の永続確保済みテクスチャ(G-Buffer・SceneColor等)を
        // そのまま読み書きする(詳細はRenderGraph.h参照)
        Core::RenderGraph graph(commandList, m_GPUProfiler.get(), &m_CPUProfiler);

        RHI::Viewport shadowViewport;
        shadowViewport.Width = static_cast<float>(kShadowMapSize);
        shadowViewport.Height = static_cast<float>(kShadowMapSize);

        RHI::Viewport gbufferViewport;
        gbufferViewport.Width = static_cast<float>(m_RenderWidth);
        gbufferViewport.Height = static_cast<float>(m_RenderHeight);

        // --- シャドウパス: ライト視点から深度のみを描画する(常に固定のシャドウマップ解像度) ---
        graph.AddPass(Core::RenderGraphPassDesc{
            .Name = "Shadow",
            .DepthTarget = m_ShadowMap.get(),
            .Execute = [this, &shadowViewport](RHI::IRHICommandList* cmd)
            {
                cmd->SetViewport(shadowViewport);
                // 深度1.0(最遠)にクリアしておく。無効時はこの後の描画をスキップするため、
                // シェーダー側は深度比較で常に「影なし」と判定する(ComputeShadowFactor参照)
                cmd->ClearDepth(1.0f);

                if (m_ShadowEnabled)
                {
                    cmd->SetPipelineState(m_ShadowPipelineState.get());
                    cmd->SetConstantBuffer(0, m_FrameConstantBuffer.get());

                    for (const auto& mesh : m_Model.Meshes)
                    {
                        cmd->SetVertexBuffer(mesh.VertexBuffer.get());
                        cmd->SetIndexBuffer(mesh.IndexBuffer.get());
                        cmd->DrawIndexed(mesh.IndexCount, 0, 0);
                    }
                }
            },
        });

        // --- ジオメトリパス: G-Bufferへ書き込む(常に指定した内部解像度) ---
        graph.AddPass(Core::RenderGraphPassDesc{
            .Name = "GBuffer",
            .RenderTargets = { m_GBufferAlbedo.get(), m_GBufferNormal.get(), m_GBufferMaterial.get(), m_GBufferEmissive.get() },
            .DepthTarget = m_GBufferDepth.get(),
            .Execute = [this, &gbufferViewport](RHI::IRHICommandList* cmd)
            {
                cmd->SetViewport(gbufferViewport);
                cmd->ClearRenderTarget({ 0.0f, 0.0f, 0.0f, 0.0f });
                // Reverse-Zのため遠平面側(NDC z=0.0)にクリアする(GBuffer.hlsl参照)
                cmd->ClearDepth(0.0f);

                cmd->SetPipelineState(m_GBufferPipelineState.get());
                cmd->SetConstantBuffer(0, m_FrameConstantBuffer.get());
                cmd->SetSampler(0, m_Sampler.get());

                for (const auto& mesh : m_Model.Meshes)
                {
                    MaterialConstants materialConstants{};
                    materialConstants.MetallicFactor = mesh.MetallicFactor;
                    materialConstants.RoughnessFactor = mesh.RoughnessFactor;
                    materialConstants.AlphaCutoff = mesh.AlphaCutoff;
                    materialConstants.EmissiveFactor[0] = mesh.EmissiveFactor[0];
                    materialConstants.EmissiveFactor[1] = mesh.EmissiveFactor[1];
                    materialConstants.EmissiveFactor[2] = mesh.EmissiveFactor[2];
                    cmd->UpdateBuffer(m_MaterialConstantBuffer.get(), &materialConstants, sizeof(materialConstants));
                    cmd->SetConstantBuffer(1, m_MaterialConstantBuffer.get());

                    cmd->SetVertexBuffer(mesh.VertexBuffer.get());
                    cmd->SetIndexBuffer(mesh.IndexBuffer.get());
                    cmd->SetTexture(0, mesh.BaseColorTexture);
                    cmd->SetTexture(1, mesh.NormalTexture);
                    cmd->SetTexture(2, mesh.MetallicRoughnessTexture);
                    cmd->SetTexture(3, mesh.EmissiveTexture);
                    cmd->DrawIndexed(mesh.IndexCount, 0, 0);
                }
            },
        });

        // --- Hi-Zミップチェーン構築パス: G-Buffer深度から1x1までのミップチェーンをコンピュートシェーダーで
        //     構築する(現時点では利用箇所は無く、デバッグ表示専用) ---
        graph.AddPass(Core::RenderGraphPassDesc{
            .Name = "HiZ",
            .Reads = { m_GBufferDepth.get() },
            .Writes = { m_HiZTexture.get() },
            .Execute = [this](RHI::IRHICommandList* cmd)
            {
                HiZConstants hizConstants{};
                hizConstants.SrcSize = { m_RenderWidth, m_RenderHeight };
                hizConstants.DstSize = { m_RenderWidth, m_RenderHeight };
                cmd->UpdateBuffer(m_HiZConstantBuffer.get(), &hizConstants, sizeof(hizConstants));

                cmd->SetComputePipelineState(m_HiZCopyPipelineState.get());
                cmd->SetComputeConstantBuffer(0, m_HiZConstantBuffer.get());
                cmd->SetComputeTexture(0, m_GBufferDepth.get());
                cmd->SetComputeUnorderedAccessTexture(0, m_HiZTexture.get(), 0);
                cmd->Dispatch((m_RenderWidth + 7) / 8, (m_RenderHeight + 7) / 8, 1);

                cmd->SetComputePipelineState(m_HiZDownsamplePipelineState.get());
                uint32_t hizSrcWidth = m_RenderWidth;
                uint32_t hizSrcHeight = m_RenderHeight;
                for (uint32_t mip = 1; mip < m_HiZMipLevels; ++mip)
                {
                    const uint32_t hizDstWidth = std::max(1u, hizSrcWidth / 2);
                    const uint32_t hizDstHeight = std::max(1u, hizSrcHeight / 2);

                    hizConstants.SrcSize = { hizSrcWidth, hizSrcHeight };
                    hizConstants.DstSize = { hizDstWidth, hizDstHeight };
                    cmd->UpdateBuffer(m_HiZConstantBuffer.get(), &hizConstants, sizeof(hizConstants));
                    cmd->SetComputeConstantBuffer(0, m_HiZConstantBuffer.get());
                    cmd->SetComputeUnorderedAccessTexture(0, m_HiZTexture.get(), mip - 1);
                    cmd->SetComputeUnorderedAccessTexture(1, m_HiZTexture.get(), mip);
                    cmd->Dispatch((hizDstWidth + 7) / 8, (hizDstHeight + 7) / 8, 1);

                    hizSrcWidth = hizDstWidth;
                    hizSrcHeight = hizDstHeight;
                }
            },
        });

        // --- 直接光パス: G-Buffer+シャドウマップからPBRの直接光(拡散+鏡面反射、シャドウ適用済み)を
        //     計算しHDRで書き出す(常に指定した内部解像度)。DeferredLighting/SSILの両方から読まれる ---
        graph.AddPass(Core::RenderGraphPassDesc{
            .Name = "DirectLight",
            .Reads = { m_GBufferAlbedo.get(), m_GBufferNormal.get(), m_GBufferMaterial.get(), m_GBufferDepth.get(), m_ShadowMap.get() },
            .RenderTargets = { m_DirectLightTexture.get() },
            .Execute = [this, &gbufferViewport](RHI::IRHICommandList* cmd)
            {
                cmd->SetViewport(gbufferViewport);

                cmd->SetPipelineState(m_DirectLightPipelineState.get());
                cmd->SetConstantBuffer(0, m_FrameConstantBuffer.get());
                cmd->SetSampler(0, m_Sampler.get());
                cmd->SetTexture(0, m_GBufferAlbedo.get());
                cmd->SetTexture(1, m_GBufferNormal.get());
                cmd->SetTexture(2, m_GBufferMaterial.get());
                cmd->SetTexture(3, m_GBufferDepth.get());
                cmd->SetTexture(4, m_ShadowMap.get());
                cmd->Draw(3, 0);
            },
        });

        // --- AO/GIパス: 選択中の手法(SSAO or SSIL)でG-Bufferから遮蔽率(・間接拡散光)を計算し、
        //     ブラーで均す(常に指定した内部解像度)。出力フォーマットはどちらもrgb=間接拡散光, a=遮蔽率で共通 ---
        if (m_AOEnabled)
        {
            RHI::IRHITexture* aoRawTexture = (m_AOTechnique == AOTechnique::SSAO) ? m_SSAORawTexture.get() : m_SSILRawTexture.get();
            RHI::IRHITexture* aoBlurredTexture = (m_AOTechnique == AOTechnique::SSAO) ? m_SSAOTexture.get() : m_SSILTexture.get();

            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "AO",
                .Reads = (m_AOTechnique == AOTechnique::SSAO)
                    ? std::vector<RHI::IRHITexture*>{ m_GBufferNormal.get(), m_GBufferDepth.get() }
                    : std::vector<RHI::IRHITexture*>{ m_GBufferNormal.get(), m_GBufferDepth.get(), m_DirectLightTexture.get() },
                .RenderTargets = { aoRawTexture },
                .Execute = [this, &gbufferViewport](RHI::IRHICommandList* cmd)
                {
                    cmd->SetViewport(gbufferViewport);
                    cmd->SetConstantBuffer(0, m_FrameConstantBuffer.get());
                    cmd->SetSampler(0, m_Sampler.get());

                    if (m_AOTechnique == AOTechnique::SSAO)
                    {
                        SSAOConstants ssaoConstants{};
                        std::copy(m_SSAOKernel.begin(), m_SSAOKernel.end(), ssaoConstants.Samples);
                        ssaoConstants.Params = { m_SSAORadius, m_SSAORadius * 0.05f, m_SSAOPower, 0.0f };
                        cmd->UpdateBuffer(m_SSAOConstantBuffer.get(), &ssaoConstants, sizeof(ssaoConstants));

                        cmd->SetPipelineState(m_SSAOPipelineState.get());
                        cmd->SetConstantBuffer(1, m_SSAOConstantBuffer.get());
                        cmd->SetTexture(0, m_GBufferNormal.get());
                        cmd->SetTexture(1, m_GBufferDepth.get());
                        cmd->Draw(3, 0);
                    }
                    else
                    {
                        SSILConstants ssilConstants{};
                        ssilConstants.Params0 = { m_SSILRadius, m_SSILThickness, m_SSILIntensity, m_SSILPower };
                        ssilConstants.Params1 = { m_SSILSliceCount, m_SSILStepCount, 0u, 0u };
                        cmd->UpdateBuffer(m_SSILConstantBuffer.get(), &ssilConstants, sizeof(ssilConstants));

                        cmd->SetPipelineState(m_SSILPipelineState.get());
                        cmd->SetConstantBuffer(1, m_SSILConstantBuffer.get());
                        cmd->SetTexture(0, m_GBufferNormal.get());
                        cmd->SetTexture(1, m_GBufferDepth.get());
                        cmd->SetTexture(2, m_DirectLightTexture.get());
                        cmd->Draw(3, 0);
                    }
                },
            });

            // ブラーパス: 遮蔽率・間接拡散光のタイル状ノイズをボックスブラーで均す(SSAO/SSIL共通シェーダ)
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "AOBlur",
                .Reads = { aoRawTexture },
                .RenderTargets = { aoBlurredTexture },
                .Execute = [this, &gbufferViewport, aoRawTexture](RHI::IRHICommandList* cmd)
                {
                    cmd->SetViewport(gbufferViewport);
                    cmd->SetPipelineState(m_AOBlurPipelineState.get());
                    cmd->SetTexture(0, aoRawTexture);
                    cmd->Draw(3, 0);
                },
            });
        }

        // デバッグ表示(ブラー前確認用)のため、ブラー前の生バッファへの参照も別途保持しておく
        RHI::IRHITexture* activeAOTexture = m_AODisabledTexture.get();
        RHI::IRHITexture* activeAORawTexture = m_AODisabledTexture.get();
        if (m_AOEnabled)
        {
            activeAOTexture = (m_AOTechnique == AOTechnique::SSAO) ? m_SSAOTexture.get() : m_SSILTexture.get();
            activeAORawTexture = (m_AOTechnique == AOTechnique::SSAO) ? m_SSAORawTexture.get() : m_SSILRawTexture.get();
        }

        // --- ライティングパス: G-Bufferを読み、SceneColorへ出力(常に指定した内部解像度) ---
        graph.AddPass(Core::RenderGraphPassDesc{
            .Name = "Lighting",
            .Reads = { m_GBufferAlbedo.get(), m_DirectLightTexture.get(), m_GBufferMaterial.get(), m_GBufferDepth.get(), m_SkyboxTexture.get(), activeAOTexture, m_GBufferEmissive.get() },
            .RenderTargets = { m_SceneColor.get() },
            .Execute = [this, &gbufferViewport, activeAOTexture](RHI::IRHICommandList* cmd)
            {
                cmd->SetViewport(gbufferViewport);
                // 深度テストに失敗した(=何も描かれていない)ピクセル用の背景色。discardされた箇所に前フレームのデータが
                // 残らないよう、フルスクリーン三角形を描く前に明示的にクリアしておく
                cmd->ClearRenderTarget({ 0.05f, 0.05f, 0.08f, 1.0f });

                cmd->SetPipelineState(m_LightingPipelineState.get());
                cmd->SetConstantBuffer(0, m_FrameConstantBuffer.get());
                cmd->SetSampler(0, m_Sampler.get());
                cmd->SetTexture(0, m_GBufferAlbedo.get());
                cmd->SetTexture(1, m_DirectLightTexture.get());
                cmd->SetTexture(2, m_GBufferMaterial.get());
                cmd->SetTexture(3, m_GBufferDepth.get());
                cmd->SetTexture(4, m_SkyboxTexture.get());
                cmd->SetTexture(5, activeAOTexture);
                cmd->SetTexture(6, m_GBufferEmissive.get());
                cmd->Draw(3, 0);
            },
        });

        // --- SSRパス: LightingパスのSceneColorとG-Bufferから鏡面反射を計算し加算する。
        //     無効時はスキップし、Presentが直接m_SceneColorを参照する ---
        if (m_SSREnabled)
        {
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "SSR",
                .Reads = { m_SceneColor.get(), m_GBufferNormal.get(), m_GBufferMaterial.get(), m_GBufferDepth.get(), m_SkyboxTexture.get(), m_GBufferAlbedo.get() },
                .RenderTargets = { m_SSRTexture.get() },
                .Execute = [this, &gbufferViewport](RHI::IRHICommandList* cmd)
                {
                    SSRConstants ssrConstants{};
                    ssrConstants.Params0 = { m_SSRMaxDistance, m_SSRThickness, m_SSRRoughnessCutoff, 0.0f };
                    cmd->UpdateBuffer(m_SSRConstantBuffer.get(), &ssrConstants, sizeof(ssrConstants));

                    cmd->SetViewport(gbufferViewport);
                    cmd->SetPipelineState(m_SSRPipelineState.get());
                    cmd->SetConstantBuffer(0, m_FrameConstantBuffer.get());
                    cmd->SetConstantBuffer(1, m_SSRConstantBuffer.get());
                    cmd->SetSampler(0, m_Sampler.get());
                    cmd->SetTexture(0, m_SceneColor.get());
                    cmd->SetTexture(1, m_GBufferNormal.get());
                    cmd->SetTexture(2, m_GBufferMaterial.get());
                    cmd->SetTexture(3, m_GBufferDepth.get());
                    cmd->SetTexture(4, m_SkyboxTexture.get());
                    cmd->SetTexture(5, m_GBufferAlbedo.get());
                    cmd->Draw(3, 0);
                },
            });
        }

        // --- Tonemapパス: HDRのSceneColor(SSR有効時はSSR適用後)をLDRへ変換する。
        //     SSR等のHDR演算がすべて完了した後、Present直前の独立したステージとして常に実行する ---
        RHI::IRHITexture* hdrSceneColor = m_SSREnabled ? m_SSRTexture.get() : m_SceneColor.get();
        graph.AddPass(Core::RenderGraphPassDesc{
            .Name = "Tonemap",
            .Reads = { hdrSceneColor },
            .RenderTargets = { m_TonemapTexture.get() },
            .Execute = [this, &gbufferViewport, hdrSceneColor](RHI::IRHICommandList* cmd)
            {
                cmd->SetViewport(gbufferViewport);
                cmd->SetPipelineState(m_TonemapPipelineState.get());
                cmd->SetSampler(0, m_Sampler.get());
                cmd->SetTexture(0, hdrSceneColor);
                cmd->Draw(3, 0);
            },
        });

        // --- Presentパス: 選択中のレンダーターゲットを、アスペクト比を保ってバックバッファへ出力 ---
        // デバッグ表示(Render Targets UI)で選択されたバッファに応じて表示ソースを切り替える。
        // 深度バッファ(GBuffer深度・シャドウマップ)はPresent.hlsl側でグレースケール化するためMode=1を渡す
        RHI::IRHITexture* presentSourceTexture = m_TonemapTexture.get();
        int32_t presentMode = 0;
        uint32_t presentSourceWidth = m_RenderWidth;
        uint32_t presentSourceHeight = m_RenderHeight;
        switch (m_DebugView)
        {
        case DebugView::Final:
            // Tonemapパスが既にSSR有効/無効を考慮したHDRソースをLDR変換済みのため、そのまま使う
            presentSourceTexture = m_TonemapTexture.get();
            break;
        case DebugView::Albedo:
            presentSourceTexture = m_GBufferAlbedo.get();
            break;
        case DebugView::Normal:
            presentSourceTexture = m_GBufferNormal.get();
            presentMode = 7; // オクタヘドラルエンコードをデコードして[0,1]へ再マップして表示
            break;
        case DebugView::Material:
            presentSourceTexture = m_GBufferMaterial.get();
            break;
        case DebugView::Emissive:
            presentSourceTexture = m_GBufferEmissive.get();
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
        case DebugView::SSR:
            // SSR無効時はSSRパスをスキップしているため、Tonemapパスの入力もSceneColorになり
            // 結果的にFinalと同一表示になる
            presentSourceTexture = m_TonemapTexture.get();
            break;
        case DebugView::HiZ:
            presentSourceTexture = m_HiZTexture.get();
            presentMode = 6; // 指定ミップをSampleLevelで読みグレースケール表示
            presentSourceWidth = std::max(1u, m_RenderWidth >> m_HiZDebugMipLevel);
            presentSourceHeight = std::max(1u, m_RenderHeight >> m_HiZDebugMipLevel);
            break;
        }

        PresentConstants presentConstants{};
        presentConstants.Mode = presentMode;
        presentConstants.MipLevel = static_cast<float>(m_HiZDebugMipLevel);
        commandList->UpdateBuffer(m_PresentConstantBuffer.get(), &presentConstants, sizeof(presentConstants));

        // レターボックス/ピラーボックスの余白もクリア色のまま残るよう、絞ったビューポートで描画する
        const RHI::Viewport letterboxViewport = ComputeLetterboxViewport(
            m_Window->GetWidth(), m_Window->GetHeight(), presentSourceWidth, presentSourceHeight);

        graph.AddPass(Core::RenderGraphPassDesc{
            .Name = "Present",
            .Reads = { presentSourceTexture },
            .SwapChainTarget = m_SwapChain.get(),
            .Execute = [this, &letterboxViewport, presentSourceTexture](RHI::IRHICommandList* cmd)
            {
                cmd->ClearRenderTarget({ 0.05f, 0.05f, 0.08f, 1.0f });
                cmd->ClearDepth(1.0f);
                cmd->SetViewport(letterboxViewport);

                cmd->SetPipelineState(m_PresentPipelineState.get());
                cmd->SetConstantBuffer(0, m_FrameConstantBuffer.get());
                cmd->SetConstantBuffer(1, m_PresentConstantBuffer.get());
                cmd->SetSampler(0, m_Sampler.get());
                cmd->SetTexture(0, presentSourceTexture);
                cmd->Draw(3, 0);
            },
        });

        graph.Execute();

        // ImGuiはPresentパスでバインドされたバックバッファにそのまま重ねて描画する。
        // GPU側は計測していない(このスコープ専用の描画パイプラインを持たないため)が、
        // CPU側のコマンド記録コストはDX11/DX12で差が出やすいのでここも計測しておく
        m_CPUProfiler.BeginScope("ImGui");
        m_ImGuiBackend->Render();
        m_CPUProfiler.EndScope(); // ImGui

        // Present呼び出しでコマンドリストが実行投入される(DX12)ため、それより前にEndFrame()で
        // フレーム終端のタイムスタンプ書き込み・結果リードバックのコマンドを記録しておく必要がある
        m_GPUProfiler->EndFrame();

        // ExecuteCommandLists・実際のPresent・(DX12のみ)フェンス待ちを含む区間。
        // Present呼び出し自体のCPUコストはここで計測しないと、各パスのコマンド記録時間の
        // 合計とCPU Frame Time全体の差分がどこにあるのか分からなくなるため計測しておく
        m_CPUProfiler.BeginScope("PresentSubmit");
        m_SwapChain->Present(m_VSyncEnabled);
        m_CPUProfiler.EndScope(); // PresentSubmit

        // GPUの完了待ち(DX12のフレームパイプライン化に伴うフェンス待ち)は実際のCPU負荷ではなく
        // GPU側の処理時間の反映なので、PresentSubmitの計測値からは除外しておく
        m_CPUProfiler.SubtractFromScope("PresentSubmit", m_Device->GetLastFrameGPUWaitTimeMs());
    }
}
