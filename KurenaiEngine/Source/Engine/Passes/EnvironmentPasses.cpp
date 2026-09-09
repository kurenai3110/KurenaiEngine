#include <algorithm>
#include <vector>

#include "Core/Logger.h"
#include "Core/RenderGraph.h"
#include "EnvironmentPasses.h"
#include "EnvironmentConstants.h"
#include "../Rendering/CubeFaceMath.h"
#include "../Rendering/RenderBlackboard.h"
#include "../Rendering/RenderFrameContext.h"
#include "../Rendering/SunLighting.h"
#include "../ShaderInterop/FrameConstants.h"

namespace Kurenai::Passes
{
    namespace
    {
        // 濁り(タービディティ)からMie(エアロゾル)密度の倍率を求める。
        //
        // 【Preethamの定義をそのまま持ち込んではいけない】Preethamのタービディティは
        // 「エアロゾルを含む全光学的厚さ / 分子だけの光学的厚さ」と定義されており、
        // その定義で現在の既定値2.5を換算すると τ_Mie = 1.5 × τ_Rayleigh となる。
        // Hillaireの標準大気の垂直Mie光学的厚さは 0.003996 × 1.2 = 0.0048、
        // Rayleigh(550nm)は 0.013558 × 8 = 0.1085 なので、比は0.044(タービディティ換算で1.04)。
        // つまり定義どおり換算するとエアロゾルが約34倍になり、空が白く霞んでHillaireを
        // 使う意味そのものが消える。
        //
        // そこでここでのタービディティは**Preethamの定義とは別物**として扱い、
        // 「既定値2.5をHillaireの標準大気とする相対的な濁り」と定義し直す。
        // スライダーを上げれば霞み、下げれば澄むという操作の意味は保たれる
        float ComputeAtmosphereMieDensityScale(float turbidity)
        {
            // この値でMie密度の倍率がちょうど1.0(=Hillaireの標準大気)になる
            constexpr float kReferenceTurbidity = 2.5f;
            return std::max(turbidity, 0.0f) / kReferenceTurbidity;
        }
    }

    void EnvironmentPasses::CreateBRDFLUTScratch(RHI::IRHIDevice& device, uint32_t size)
    {
        m_BRDFLUTScratchTexture = device.CreateUAVTexture(size, size, RHI::Format::R16G16_Float);
    }

    void EnvironmentPasses::CreateBRDFLUTPipelineStates(RHI::IRHIDevice& device, const std::wstring& shaderDirectory)
    {
        RHI::ShaderDesc brdfLutCsDesc;
        brdfLutCsDesc.Stage = RHI::ShaderStage::Compute;
        brdfLutCsDesc.FilePath = shaderDirectory + L"BRDFLUT.kshader";
        brdfLutCsDesc.EntryPoint = "CSMain";
        m_BRDFLUTComputeShader = device.CreateShader(brdfLutCsDesc);
        m_BRDFLUTPipelineState = device.CreateComputePipelineState({ m_BRDFLUTComputeShader.get() });

        RHI::ShaderDesc brdfLutCombineCsDesc;
        brdfLutCombineCsDesc.Stage = RHI::ShaderStage::Compute;
        brdfLutCombineCsDesc.FilePath = shaderDirectory + L"BRDFLUT.kshader";
        brdfLutCombineCsDesc.EntryPoint = "CSCombineEavg";
        m_BRDFLUTCombineComputeShader = device.CreateShader(brdfLutCombineCsDesc);
        if (!m_BRDFLUTCombineComputeShader)
        {
            Core::Logger::Error("KurenaiEngine3D",
                "BRDFLUT.hlsl CSCombineEavg のコンパイルに失敗しました"
                "(Kulla-Conty方式が必要とするEavgが焼かれず、同方式が正しく動作しません)");
        }
        m_BRDFLUTCombinePipelineState =
            device.CreateComputePipelineState({ m_BRDFLUTCombineComputeShader.get() });
    }

    void EnvironmentPasses::CreateCloudNoisePipelineStates(RHI::IRHIDevice& device, const std::wstring& shaderDirectory)
    {
        RHI::ShaderDesc cloudShapeNoiseCsDesc;
        cloudShapeNoiseCsDesc.Stage = RHI::ShaderStage::Compute;
        cloudShapeNoiseCsDesc.FilePath = shaderDirectory + L"CloudNoiseGenerate.kshader";
        cloudShapeNoiseCsDesc.EntryPoint = "CSGenerateShape";
        m_CloudShapeNoiseComputeShader = device.CreateShader(cloudShapeNoiseCsDesc);
        if (!m_CloudShapeNoiseComputeShader)
        {
            Core::Logger::Error("KurenaiEngine3D",
                "CloudNoiseGenerate.hlsl CSGenerateShape のコンパイルに失敗しました"
                "(雲の形状ノイズが焼かれません)");
        }
        m_CloudShapeNoisePipelineState =
            device.CreateComputePipelineState({ m_CloudShapeNoiseComputeShader.get() });

        RHI::ShaderDesc cloudDetailNoiseCsDesc;
        cloudDetailNoiseCsDesc.Stage = RHI::ShaderStage::Compute;
        cloudDetailNoiseCsDesc.FilePath = shaderDirectory + L"CloudNoiseGenerate.kshader";
        cloudDetailNoiseCsDesc.EntryPoint = "CSGenerateDetail";
        m_CloudDetailNoiseComputeShader = device.CreateShader(cloudDetailNoiseCsDesc);
        if (!m_CloudDetailNoiseComputeShader)
        {
            Core::Logger::Error("KurenaiEngine3D",
                "CloudNoiseGenerate.hlsl CSGenerateDetail のコンパイルに失敗しました"
                "(雲のディテールノイズが焼かれません)");
        }
        m_CloudDetailNoisePipelineState =
            device.CreateComputePipelineState({ m_CloudDetailNoiseComputeShader.get() });

        RHI::ShaderDesc cloudWeatherNoiseCsDesc;
        cloudWeatherNoiseCsDesc.Stage = RHI::ShaderStage::Compute;
        cloudWeatherNoiseCsDesc.FilePath = shaderDirectory + L"CloudNoiseGenerate.kshader";
        cloudWeatherNoiseCsDesc.EntryPoint = "CSGenerateWeather";
        m_CloudWeatherNoiseComputeShader = device.CreateShader(cloudWeatherNoiseCsDesc);
        if (!m_CloudWeatherNoiseComputeShader)
        {
            Core::Logger::Error("KurenaiEngine3D",
                "CloudNoiseGenerate.hlsl CSGenerateWeather のコンパイルに失敗しました"
                "(ウェザーマップが焼かれず、雲がまったく立ちません)");
        }
        m_CloudWeatherNoisePipelineState =
            device.CreateComputePipelineState({ m_CloudWeatherNoiseComputeShader.get() });
    }

    void EnvironmentPasses::CreateAtmosphereResources(RHI::IRHIDevice& device, const std::wstring& shaderDirectory)
    {
        // 【ここで焼き直し要求を必ず立てる】呼び出し元がLUT3枚を作り直した直後なので、
        // 「前回焼いたときの条件」を捨てないと、APIをDX11/DX12で切り替えた直後など
        // CreateSceneResourcesが再度呼ばれた場合に一度も焼かれないまま読まれて空が黒くなる
        m_AtmosphereLUTBakedTurbidity = -1.0f;
        m_SkyViewBakedTurbidity = -1.0f;
        m_SkyViewBakedSunPosition = { 0.0f, 0.0f, 0.0f };

        RHI::BufferDesc atmosphereConstantBufferDesc;
        atmosphereConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        atmosphereConstantBufferDesc.SizeInBytes = sizeof(AtmosphereConstants);
        m_AtmosphereConstantBuffer = device.CreateBuffer(atmosphereConstantBufferDesc);
        if (!m_AtmosphereConstantBuffer)
        {
            Core::Logger::Error("KurenaiEngine3D",
                "大気散乱の定数バッファの作成に失敗しました(日中の空が黒くなります)");
        }

        RHI::ShaderDesc transmittanceCsDesc;
        transmittanceCsDesc.Stage = RHI::ShaderStage::Compute;
        transmittanceCsDesc.FilePath = shaderDirectory + L"AtmosphereLUT.kshader";
        transmittanceCsDesc.EntryPoint = "CSTransmittance";
        m_TransmittanceComputeShader = device.CreateShader(transmittanceCsDesc);
        if (!m_TransmittanceComputeShader)
        {
            Core::Logger::Error("KurenaiEngine3D",
                "AtmosphereLUT.hlsl CSTransmittance のコンパイルに失敗しました");
        }
        m_TransmittancePipelineState =
            device.CreateComputePipelineState({ m_TransmittanceComputeShader.get() });

        RHI::ShaderDesc multiScatteringCsDesc;
        multiScatteringCsDesc.Stage = RHI::ShaderStage::Compute;
        multiScatteringCsDesc.FilePath = shaderDirectory + L"AtmosphereLUT.kshader";
        multiScatteringCsDesc.EntryPoint = "CSMultiScattering";
        m_MultiScatteringComputeShader = device.CreateShader(multiScatteringCsDesc);
        if (!m_MultiScatteringComputeShader)
        {
            Core::Logger::Error("KurenaiEngine3D",
                "AtmosphereLUT.hlsl CSMultiScattering のコンパイルに失敗しました");
        }
        m_MultiScatteringPipelineState =
            device.CreateComputePipelineState({ m_MultiScatteringComputeShader.get() });

        RHI::ShaderDesc skyViewCsDesc;
        skyViewCsDesc.Stage = RHI::ShaderStage::Compute;
        skyViewCsDesc.FilePath = shaderDirectory + L"AtmosphereLUT.kshader";
        skyViewCsDesc.EntryPoint = "CSSkyView";
        m_SkyViewComputeShader = device.CreateShader(skyViewCsDesc);
        if (!m_SkyViewComputeShader)
        {
            Core::Logger::Error("KurenaiEngine3D",
                "AtmosphereLUT.hlsl CSSkyView のコンパイルに失敗しました(日中の空が黒くなります)");
        }
        m_SkyViewPipelineState =
            device.CreateComputePipelineState({ m_SkyViewComputeShader.get() });
    }

    void EnvironmentPasses::CreateIrradiancePipelineState(RHI::IRHIDevice& device, const std::wstring& shaderDirectory)
    {
        RHI::ShaderDesc irradianceCsDesc;
        irradianceCsDesc.Stage = RHI::ShaderStage::Compute;
        irradianceCsDesc.FilePath = shaderDirectory + L"IBLConvolve.kshader";
        irradianceCsDesc.EntryPoint = "CSIrradiance";
        m_IrradianceComputeShader = device.CreateShader(irradianceCsDesc);
        m_IrradiancePipelineState = device.CreateComputePipelineState({ m_IrradianceComputeShader.get() });
    }

    void EnvironmentPasses::CreateSHResources(RHI::IRHIDevice& device, const std::wstring& shaderDirectory)
    {
        // 拡散イラディアンスの球面調和関数(SH L2)経路。CSIrradianceの
        // 高速な代替で、A/B比較用にトグルで切り替える(m_IBLSettings.UseSHIrradiance、既定false)。
        // 詳細はIBLConvolve.hlsl冒頭のコメント参照
        RHI::ShaderDesc projectShCsDesc;
        projectShCsDesc.Stage = RHI::ShaderStage::Compute;
        projectShCsDesc.FilePath = shaderDirectory + L"IBLConvolve.kshader";
        projectShCsDesc.EntryPoint = "CSProjectSH";
        m_ProjectSHComputeShader = device.CreateShader(projectShCsDesc);
        m_ProjectSHPipelineState = device.CreateComputePipelineState({ m_ProjectSHComputeShader.get() });

        RHI::ShaderDesc projectShFinalCsDesc;
        projectShFinalCsDesc.Stage = RHI::ShaderStage::Compute;
        projectShFinalCsDesc.FilePath = shaderDirectory + L"IBLConvolve.kshader";
        projectShFinalCsDesc.EntryPoint = "CSProjectSHFinal";
        m_ProjectSHFinalComputeShader = device.CreateShader(projectShFinalCsDesc);
        m_ProjectSHFinalPipelineState = device.CreateComputePipelineState({ m_ProjectSHFinalComputeShader.get() });

        RHI::ShaderDesc evaluateShCsDesc;
        evaluateShCsDesc.Stage = RHI::ShaderStage::Compute;
        evaluateShCsDesc.FilePath = shaderDirectory + L"IBLConvolve.kshader";
        evaluateShCsDesc.EntryPoint = "CSEvaluateSH";
        m_EvaluateSHComputeShader = device.CreateShader(evaluateShCsDesc);
        m_EvaluateSHPipelineState = device.CreateComputePipelineState({ m_EvaluateSHComputeShader.get() });

        // SHの部分和(CSProjectSHのグループごとの出力)と最終係数(CSProjectSHFinalの出力)。
        // グループ数は (Passes::kSHProjectionSize/8)² × 6面で固定(射影解像度はSourceSkyboxの実解像度と
        // 無関係な固定値。kSHProjectionSizeのコメント参照)
        const uint32_t groupsPerSide = (Passes::kSHProjectionSize + 7) / 8;
        const uint32_t maxSHGroups = groupsPerSide * groupsPerSide * kCubeFaceCount;
        RHI::BufferDesc shPartialSumsDesc;
        shPartialSumsDesc.Usage = RHI::BufferUsage::StructuredRW;
        shPartialSumsDesc.StrideInBytes = static_cast<uint32_t>(sizeof(DirectX::XMFLOAT4));
        shPartialSumsDesc.SizeInBytes = shPartialSumsDesc.StrideInBytes * maxSHGroups * kSHCoeffCount;
        m_SHPartialSumsBuffer = device.CreateBuffer(shPartialSumsDesc);

        RHI::BufferDesc shCoefficientsDesc;
        shCoefficientsDesc.Usage = RHI::BufferUsage::StructuredRW;
        shCoefficientsDesc.StrideInBytes = static_cast<uint32_t>(sizeof(DirectX::XMFLOAT4));
        shCoefficientsDesc.SizeInBytes = shCoefficientsDesc.StrideInBytes * kSHCoeffCount;
        m_SHCoefficientsBuffer = device.CreateBuffer(shCoefficientsDesc);
    }

    void EnvironmentPasses::CreateSkyGenerateResources(RHI::IRHIDevice& device, const std::wstring& shaderDirectory)
    {
        RHI::ShaderDesc skyGenerateCsDesc;
        skyGenerateCsDesc.Stage = RHI::ShaderStage::Compute;
        skyGenerateCsDesc.FilePath = shaderDirectory + L"SkyGenerate.kshader";
        skyGenerateCsDesc.EntryPoint = "CSGenerateSky";
        m_SkyGenerateComputeShader = device.CreateShader(skyGenerateCsDesc);
        m_SkyGeneratePipelineState = device.CreateComputePipelineState({ m_SkyGenerateComputeShader.get() });

        RHI::BufferDesc skyBakeConstantBufferDesc;
        skyBakeConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        skyBakeConstantBufferDesc.SizeInBytes = sizeof(SkyBakeConstants);
        m_SkyBakeConstantBuffer = device.CreateBuffer(skyBakeConstantBufferDesc);
    }

    void EnvironmentPasses::CreateSkyIntegrateResources(RHI::IRHIDevice& device, const std::wstring& shaderDirectory)
    {
        // 空パラメータ(ティント4本+照度正規化済みの天頂輝度)の積分をGPUで行うコンピュートシェーダー
        // 。SkyGenerateより前に実行し、結果をSkyResources::ParametersBufferへ書く
        RHI::ShaderDesc skyIntegrateCsDesc;
        skyIntegrateCsDesc.Stage = RHI::ShaderStage::Compute;
        skyIntegrateCsDesc.FilePath = shaderDirectory + L"SkyIntegrate.kshader";
        skyIntegrateCsDesc.EntryPoint = "CSIntegrateSky";
        m_SkyIntegrateComputeShader = device.CreateShader(skyIntegrateCsDesc);
        m_SkyIntegratePipelineState = device.CreateComputePipelineState({ m_SkyIntegrateComputeShader.get() });

        RHI::BufferDesc skyIntegrateConstantBufferDesc;
        skyIntegrateConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        skyIntegrateConstantBufferDesc.SizeInBytes = sizeof(SkyIntegrateConstants);
        m_SkyIntegrateConstantBuffer = device.CreateBuffer(skyIntegrateConstantBufferDesc);
    }

    void EnvironmentPasses::Register(
        Core::RenderGraph& graph,
        const Rendering::RenderFrameContext& frame,
        const Rendering::RenderBlackboard& bb)
    {
        // 【フレームの写しをローカルで受ける】frame自体はラムダへ捕捉しない
        RHI::IRHITexture* const cloudDetailNoiseTexture = frame.Sky->CloudDetailNoiseTexture.get();
        RHI::IRHITexture* const cloudShapeNoiseTexture = frame.Sky->CloudShapeNoiseTexture.get();
        RHI::IRHITexture* const cloudWeatherNoiseTexture = frame.Sky->CloudWeatherNoiseTexture.get();
        RHI::IRHITexture* const multiScatteringLUT = frame.Sky->MultiScatteringLUT.get();
        RHI::IRHIBuffer* const skyParametersBuffer = frame.Sky->ParametersBuffer.get();
        RHI::IRHITexture* const skyViewLUT = frame.Sky->SkyViewLUT.get();
        RHI::IRHITexture* const transmittanceLUT = frame.Sky->TransmittanceLUT.get();
        RHI::IRHITexture* const proceduralSkyTexture = frame.Sky->ProceduralSkyTexture.get();

        // 【フレームの写しをローカルで受ける】ラムダへ値で渡すため
        const float activeCloudTransmittance = frame.ActiveCloudTransmittance;

        // 【フレームの写しをローカルで受ける】frame自体はラムダへ捕捉しない
        RHI::IRHITexture* const brdfLUTTexture = frame.IBL->BRDFLUTTexture.get();
        RHI::IRHIBuffer* const iblPrefilterConstantBuffer = frame.IBL->PrefilterConstantBuffer.get();
        RHI::IRHIPipelineState* const iblPrefilterPipelineState = frame.IBL->PrefilterPipelineState.get();
        RHI::IRHITexture* const irradianceTexture = frame.IBL->IrradianceTexture.get();
        RHI::IRHITexture* const prefilteredEnvTexture = frame.IBL->PrefilteredEnvTexture.get();

        // 【ラムダへ値で渡すためローカルへ受け直す】frame そのものは捕捉しない作法
        // (Rendering/RenderFrameContext.h の冒頭)。設定は POD なので写しは安い
        const IBLSettings iblSettings = frame.Settings.IBL;

        RHI::IRHISamplerSet* const materialSamplers = frame.MaterialSamplers;
        RHI::IRHISamplerSet* const screenSpaceSamplers = frame.ScreenSpaceSamplers;

        // 【フレームの値をここで写し取る】以下はRender()から機械的に移した登録コードなので、
        // 参照している名前を変えずに済むよう同じ名前で受け直す。
        //
        // sunLighting と constants は Render() のローカルを指しており、
        // graph.Execute() が終わるまで生きている。ここでの参照はその実体を指す
        const SunLighting& sunLighting = *frame.Sun;
        const ShaderInterop::FrameConstants& constants = *frame.Constants;
        RHI::IRHITexture* const skyTexture = frame.SkyTexture;
        const float effectiveExposure = frame.EffectiveExposure;
        const bool bakeSkyThisFrame = frame.BakeSkyThisFrame;
        const bool skyIntegrateThisFrame = frame.SkyIntegrateThisFrame;

        // --- 大気散乱のLUTのベイクパス ---
        //
        //     AtmosphereLUTBake: Transmittance → MultiScattering の順。MultiScatteringは
        //     TransmittanceをSRVで読むため順序が意味を持つ(BRDF積分LUTの2パス構成と同じ形)。
        //     どちらも大気パラメータだけの関数なので、濁りが変わったときだけ焼き直す。
        //
        //     SkyViewBake: 空そのもの。太陽が動くと変わるので毎フレーム焼く。
        //
        //     【この2つは必ずSkyIntegrateより前に「登録」すること】RenderGraphの依存解決は
        //     登録順に1回だけ舐める前方走査で、あるパスのReadsは**自分より前に登録された
        //     書き手**しか見つけられない(RenderGraph::ResolveExecutionOrderのlastWriter)。
        //     つまりグラフはパスを後ろへ遅らせることはできても前へ動かすことはできない。
        //     この2つをSkyIntegrateより後ろに置くと、SkyIntegrateが.Reads = { m_SkyResources.SkyViewLUT }を
        //     宣言していても辺が張られず、**未初期化のLUTを積分してしまう**。
        //     太陽が静止したシーンではSkyIntegrateは起動直後の1回しか走らないため、
        //     壊れた天頂輝度がそのまま最後まで残る(実測: 積分値が5.29ではなく1.58になり、
        //     空が3.3倍明るくなって青が白く飛んでいた)。
        //
        //     【定数バッファは3つのエントリポイント共通】濁りはMieの密度としてTransmittanceにも
        //     MultiScatteringにも効くため、AtmosphereConstantsを3者で共有している
        const float atmosphereMieDensityScale = ComputeAtmosphereMieDensityScale(frame.Settings.Sky.Turbidity);
        const auto updateAtmosphereConstants = [this, &sunLighting, atmosphereMieDensityScale]
            (RHI::IRHICommandList* cmd)
        {
            AtmosphereConstants atmosphereConstants{};
            atmosphereConstants.SunDirection = {
                sunLighting.SunPosition.x, sunLighting.SunPosition.y, sunLighting.SunPosition.z, 0.0f
            };
            atmosphereConstants.Params0 = { atmosphereMieDensityScale, 0.0f, 0.0f, 0.0f };
            cmd->UpdateBuffer(m_AtmosphereConstantBuffer.get(), &atmosphereConstants, sizeof(atmosphereConstants));
            cmd->SetComputeConstantBuffer(0, m_AtmosphereConstantBuffer.get());
        };

        if (m_AtmosphereLUTBakedTurbidity != frame.Settings.Sky.Turbidity &&
            m_TransmittancePipelineState && m_MultiScatteringPipelineState)
        {
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "AtmosphereLUTBake",
                .Writes = { transmittanceLUT, multiScatteringLUT },
                .Execute = [this, multiScatteringLUT, transmittanceLUT, updateAtmosphereConstants, screenSpaceSamplers](RHI::IRHICommandList* cmd)
                {
                    cmd->SetComputePipelineState(m_TransmittancePipelineState.get());
                    updateAtmosphereConstants(cmd);
                    cmd->SetComputeUnorderedAccessTexture(0, transmittanceLUT, 0);
                    cmd->Dispatch((Passes::kTransmittanceLUTWidth + 7) / 8, (Passes::kTransmittanceLUTHeight + 7) / 8, 1);

                    // UAVはDispatch直後に自動で解除されるため張り直す。
                    // ここでTransmittanceをSRV(t0)として読むので、上のDispatchより後でなければならない
                    cmd->SetComputePipelineState(m_MultiScatteringPipelineState.get());
                    updateAtmosphereConstants(cmd);
                    cmd->SetComputeTexture(0, transmittanceLUT);
                    cmd->SetComputeSamplerSet(screenSpaceSamplers);
                    cmd->SetComputeUnorderedAccessTexture(0, multiScatteringLUT, 0);
                    const uint32_t groups = (Passes::kMultiScatteringLUTSize + 7) / 8;
                    cmd->Dispatch(groups, groups, 1);
                },
            });
            m_AtmosphereLUTBakedTurbidity = frame.Settings.Sky.Turbidity;
        }

        // SkyView LUTを焼き直すかどうか。CSSkyViewの入力は太陽の向きと濁りだけで、
        // 視点位置はkSkyViewHeightKm固定(カメラ非依存)なので、この2つが動かなければ
        // まったく同じ内容を焼き直すことになる。実測1.15〜1.53ms/フレームがまるごと無駄だった。
        // 濁りは上のAtmosphereLUTBakeとまったく同じ条件で判定するため、濁りが動いたフレームでは
        // Transmittance/MultiScatteringとSkyViewが同じフレームで焼き直され、実行順序は
        // Reads/Writesの依存からレンダーグラフが決める
        bool bakeSkyViewThisFrame = m_SkyViewBakedTurbidity != frame.Settings.Sky.Turbidity;
        if (!bakeSkyViewThisFrame)
        {
            const DirectX::XMVECTOR current = DirectX::XMLoadFloat3(&sunLighting.SunPosition);
            const DirectX::XMVECTOR baked = DirectX::XMLoadFloat3(&m_SkyViewBakedSunPosition);
            const float cosAngle = DirectX::XMVectorGetX(DirectX::XMVector3Dot(current, baked));
            bakeSkyViewThisFrame =
                cosAngle < std::cos(DirectX::XMConvertToRadians(Passes::kSkyViewRebakeAngleDegrees));
        }

        if (m_SkyViewPipelineState && bakeSkyViewThisFrame)
        {
            m_SkyViewBakedSunPosition = sunLighting.SunPosition;
            m_SkyViewBakedTurbidity = frame.Settings.Sky.Turbidity;
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "SkyViewBake",
                .Reads = { transmittanceLUT, multiScatteringLUT },
                .Writes = { skyViewLUT },
                .Execute = [this, multiScatteringLUT, skyViewLUT, transmittanceLUT, updateAtmosphereConstants, screenSpaceSamplers](RHI::IRHICommandList* cmd)
                {
                    cmd->SetComputePipelineState(m_SkyViewPipelineState.get());
                    updateAtmosphereConstants(cmd);
                    cmd->SetComputeTexture(0, transmittanceLUT);
                    cmd->SetComputeTexture(1, multiScatteringLUT);
                    cmd->SetComputeSamplerSet(screenSpaceSamplers);
                    cmd->SetComputeUnorderedAccessTexture(0, skyViewLUT, 0);
                    cmd->Dispatch((Passes::kSkyViewLUTWidth + 7) / 8, (Passes::kSkyViewLUTHeight + 7) / 8, 1);
                },
            });
        }

        // --- 空パラメータの積分パス: 色味の決定とθ64×φ256=16,384サンプルの照度正規化積分を
        //     GPUで行い、結果(ティント4本+正規化済みの天頂輝度)をm_SkyResources.ParametersBufferへ書く。
        //     **CPU側で計算してはいけない**(Sky.hlsli側の式と二重実装になる)。
        //     このバッファをSkyGenerate/DeferredLighting/SSRの3者が読むため、下のSkyGenerateパスより
        //     必ず先に実行する必要がある。実行条件はbakeSkyThisFrameではなくskyIntegrateThisFrame
        //     (手続き空が無効なシーンでも初回の1回だけは走らせ、未初期化状態を解消する。
        //     理由はm_SkyResources.ParametersBuffer作成箇所とskyIntegrateThisFrame宣言のコメント参照) ---
        if (skyIntegrateThisFrame)
        {
            // 第2段(P18: 雲込みの空の照度)へ渡す値。**FrameConstants(constants)から
            // そのまま複製する**——別々に組み立てると、背景に見えている雲と大気遠近が
            // 想定している雲が食い違いうる。ここで値を作り、ラムダへは値渡しで捕まえる
            SkyIntegrateConstants integrateConstants{};
            integrateConstants.SunDirection = {
                sunLighting.SunPosition.x, sunLighting.SunPosition.y, sunLighting.SunPosition.z, 0.0f
            };
            integrateConstants.IntegrateParams = {
                sunLighting.SkyIlluminanceLux, effectiveExposure, frame.Settings.Sky.Turbidity, frame.Settings.Sky.Saturation
            };
            integrateConstants.CloudParams0 = constants.CloudParams0;
            integrateConstants.CloudParams1 = constants.CloudParams1;
            integrateConstants.CloudParams2 = constants.CloudParams2;
            integrateConstants.CloudParams3 = constants.CloudParams3;
            integrateConstants.FogParams0 = constants.FogParams0;
            // レイの起点はカメラ。wはSkyParams.zと同じ太陽照度/空照度比
            // (雲の明るさを太陽照度基準にするためにEvaluateCloudLayerが使う)
            integrateConstants.ViewerAndSunRatio = {
                constants.CameraPosition.x, constants.CameraPosition.y, constants.CameraPosition.z,
                constants.SkyParams.z
            };

            // 雲の3Dノイズとウェザーマップ(P18の第2段)。
            // **Readsへ入れることが順序の保証そのもの**——CloudNoiseBakeパスはこのパスより
            // 後に登録されるが、レンダーグラフのKahn法が依存を見て焼き込みを先へ回す。
            // 宣言を外すと初回フレームで未初期化のノイズを読み、雲込みの照度が意味の無い値になる。
            //
            // 【テクスチャの作成に失敗していた場合】バインドを外して縮退させる。SRVが
            // 張られていなければサンプルは0を返し、ウェザーマップが0なら密度も0、つまり
            // 「雲が無い」と積分される。結果CloudSkyLightは(1,1,1)になり、大気遠近は
            // P18より前とまったく同じ振る舞いへ戻る。作成失敗そのものはInitialize側で
            // 既にエラーを出しているので、ここでは1度だけ「P18が効いていない」ことを残す
            const bool cloudNoiseReady = cloudShapeNoiseTexture && cloudDetailNoiseTexture
                                         && cloudWeatherNoiseTexture;
            if (!cloudNoiseReady && !m_SkyIntegrateCloudMissingLogged)
            {
                Core::Logger::Error("KurenaiEngine3D",
                    "雲のノイズテクスチャが無いためSkyIntegrateへ束ねられません"
                    "(大気遠近が曇り空へ追従せず、被覆率を上げても遠景に晴天の青い散乱光が残ります)");
                m_SkyIntegrateCloudMissingLogged = true;
            }

            std::vector<RHI::IRHITexture*> integrateReads = { skyViewLUT };
            if (cloudNoiseReady)
            {
                integrateReads.push_back(cloudShapeNoiseTexture);
                integrateReads.push_back(cloudDetailNoiseTexture);
                integrateReads.push_back(cloudWeatherNoiseTexture);
            }

            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "SkyIntegrate",
                // 日中の空はSkyView LUTを引くため、このパスもLUTを読む。
                // これによりレンダーグラフがSkyViewBakeより後へ自動で並べてくれる。
                // 雲の3枚(P18)も同じ仕組みでCloudNoiseBakeより後へ並ぶ
                .Reads = integrateReads,
                .BufferWrites = { skyParametersBuffer },
                .Execute = [this, cloudDetailNoiseTexture, cloudShapeNoiseTexture, cloudWeatherNoiseTexture, skyParametersBuffer, skyViewLUT, integrateConstants, cloudNoiseReady, screenSpaceSamplers](RHI::IRHICommandList* cmd)
                {
                    cmd->UpdateBuffer(m_SkyIntegrateConstantBuffer.get(), &integrateConstants, sizeof(integrateConstants));

                    cmd->SetComputePipelineState(m_SkyIntegratePipelineState.get());
                    cmd->SetComputeConstantBuffer(0, m_SkyIntegrateConstantBuffer.get());
                    // SkyView LUT(t0)とサンプラー(s1 ColorSampler)。日中の空はこのLUTから
                    // 引くため、積分側も同じLUTを読む必要がある
                    cmd->SetComputeTexture(0, skyViewLUT);
                    if (cloudNoiseReady)
                    {
                        // 雲(P18)。形状t1・ディテールt2・ウェザーマップt3。SkyIntegrate.hlslの
                        // KURENAI_CLOUD_*_REGISTERと**同じ番号**であること
                        cmd->SetComputeTexture(1, cloudShapeNoiseTexture);
                        cmd->SetComputeTexture(2, cloudDetailNoiseTexture);
                        cmd->SetComputeTexture(3, cloudWeatherNoiseTexture);
                    }
                    // s3 VolumeSamplerがLinear+Wrapで入っている(Samplers.hlsliの役割表参照)。
                    // 雲の3Dノイズとウェザーマップはこれで引く
                    cmd->SetComputeSamplerSet(screenSpaceSamplers);
                    cmd->SetComputeUnorderedAccessBuffer(0, skyParametersBuffer);
                    // 1グループ×256スレッド固定(SkyIntegrate.hlsl参照)
                    cmd->Dispatch(1, 1, 1);
                },
            });
            m_SkyParametersBufferInitialized = true;
        }

        // --- 手続き空の生成パス: Perez分布をGPUで評価してキューブマップを焼く。
        //     太陽が動くと空の輝度分布の形も変わるため、オフラインDDSと違い焼き直しが要る
        //     (詳細はSkyGenerate.hlsl冒頭)。焼き直しの要否・雲の平均透過率のキャッシュ・
        //     m_SkyBakeDirty等のフラグ更新はすべて上のbakeSkyThisFrameブロックで済ませてあるため、
        //     ここではそのキャッシュ(frame.ActiveCloudTransmittance)と、直前のSkyIntegrateパスが
        //     書いたm_SkyResources.ParametersBufferを使ってパスを登録するだけでよい ---
        if (bakeSkyThisFrame)
        {
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "SkyGenerate",
                // IBLキューブへ焼く空もSkyView LUT経由なので読み手に加わる
                .Reads = { skyViewLUT },
                .Writes = { proceduralSkyTexture },
                .BufferReads = { skyParametersBuffer },
                .Execute = [this, proceduralSkyTexture, activeCloudTransmittance, skyParametersBuffer, skyViewLUT, &sunLighting, materialSamplers](RHI::IRHICommandList* cmd)
                {
                    cmd->SetComputePipelineState(m_SkyGeneratePipelineState.get());
                    cmd->SetComputeShaderResourceBuffer(0, skyParametersBuffer);
                    // SkyView LUT(t1)とサンプラー(s1 ColorSampler)。**サンプラーのバインドを
                    // 外してはいけない** ―― LUTを線形補間で引くため、このパスにもサンプラーが要る
                    cmd->SetComputeTexture(1, skyViewLUT);
                    cmd->SetComputeSamplerSet(materialSamplers);
                    for (uint32_t face = 0; face < kCubeFaceCount; ++face)
                    {
                        SkyBakeConstants skyConstants{};
                        skyConstants.Face = face;
                        // 雲(判断B)。SkyParametersBuffer側の天頂輝度は雲を考慮しない晴天の値の
                        // ままで、キューブへ焼く値にだけRenderFrameContext::ActiveCloudTransmittance(被覆率が
                        // 変わらない限り1.0)を掛ける。理由はRenderFrameContext::ActiveCloudTransmittanceの
                        // 代入元(上のbakeSkyThisFrameブロック)のコメント参照
                        skyConstants.CloudTransmittance = activeCloudTransmittance;
                        skyConstants.SunDirection = {
                            sunLighting.SunPosition.x, sunLighting.SunPosition.y, sunLighting.SunPosition.z, 0.0f
                        };
                        cmd->UpdateBuffer(m_SkyBakeConstantBuffer.get(), &skyConstants, sizeof(skyConstants));
                        cmd->SetComputeConstantBuffer(0, m_SkyBakeConstantBuffer.get());
                        cmd->SetComputeUnorderedAccessTextureCubeFace(0, proceduralSkyTexture, face, 0);
                        cmd->Dispatch((Passes::kProceduralSkySize + 7) / 8, (Passes::kProceduralSkySize + 7) / 8, 1);
                    }
                },
            });
        }

        // --- BRDF積分LUTのベイクパス: (NdotV, ラフネス)の2Dテーブルで、スカイボックスにも
        //     太陽の位置にも一切依存しないため起動後に一度だけ焼く。
        //     プリフィルタ済み鏡面(下記)が空の変化へ追従して焼き直されるようになっても、
        //     こちらが巻き込まれないよう別パス・別フラグに分離してある(m_BRDFLUTBaked参照) ---
        if (!m_BRDFLUTBaked)
        {
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "BRDFLUTBake",
                // 2パス構成のためスクラッチも書き込み対象として挙げる(RenderGraphが
                // パス内の依存を追えるように)。中身の説明はBRDFLUT.hlsl参照
                .Writes = { brdfLUTTexture, m_BRDFLUTScratchTexture.get() },
                .Execute = [this, brdfLUTTexture](RHI::IRHICommandList* cmd)
                {
                    // パス1: (A, B)をスクラッチへ焼く
                    cmd->SetComputePipelineState(m_BRDFLUTPipelineState.get());
                    cmd->SetComputeUnorderedAccessTexture(0, m_BRDFLUTScratchTexture.get(), 0);
                    cmd->Dispatch((Passes::kIBLBRDFLUTSize + 7) / 8, (Passes::kIBLBRDFLUTSize + 7) / 8, 1);

                    // パス2: スクラッチをSRVで読み、Eavgを足した float4(A, B, Eavg, 0) を最終LUTへ。
                    // UAVはDispatch直後に自動で解除されるため、ここで張り直す必要がある
                    // (IRHICommandList.hのバインド寿命の説明を参照)
                    cmd->SetComputePipelineState(m_BRDFLUTCombinePipelineState.get());
                    cmd->SetComputeTexture(0, m_BRDFLUTScratchTexture.get());
                    cmd->SetComputeUnorderedAccessTexture(0, brdfLUTTexture, 0);
                    cmd->Dispatch((Passes::kIBLBRDFLUTSize + 7) / 8, (Passes::kIBLBRDFLUTSize + 7) / 8, 1);
                },
            });
            m_BRDFLUTBaked = true;
        }

        // --- 雲の3Dノイズのベイクパス: 形状(128^3)とディテール(32^3)を起動後に一度だけ焼く。
        //     カメラにも太陽にも空の状態にも依存しない純粋な手続き生成なので、BRDF積分LUTと
        //     まったく同じ理由で焼き直さない。2枚は互いに独立なので1パスの中で連続して
        //     ディスパッチしてよい(SRVとして読み合う関係が無く、BRDFLUTの2パス構成のような
        //     中間バッファも要らない) ---
        if (!m_CloudNoiseBaked && m_CloudShapeNoisePipelineState && m_CloudDetailNoisePipelineState
            && m_CloudWeatherNoisePipelineState)
        {
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "CloudNoiseBake",
                .Writes = { cloudShapeNoiseTexture, cloudDetailNoiseTexture,
                            cloudWeatherNoiseTexture },
                .Execute = [this, cloudDetailNoiseTexture, cloudShapeNoiseTexture, cloudWeatherNoiseTexture](RHI::IRHICommandList* cmd)
                {
                    // スレッドグループは4x4x4。3次元なのでグループあたり64スレッドで、
                    // 2次元パスの8x8(=64)と同じ粒度になる
                    constexpr uint32_t kGroupSize = 4;

                    cmd->SetComputePipelineState(m_CloudShapeNoisePipelineState.get());
                    cmd->SetComputeUnorderedAccessTexture(0, cloudShapeNoiseTexture, 0);
                    const uint32_t shapeGroups = (Passes::kCloudShapeNoiseSize + kGroupSize - 1) / kGroupSize;
                    cmd->Dispatch(shapeGroups, shapeGroups, shapeGroups);

                    // UAVはDispatch直後に自動で解除されるため張り直す
                    // (IRHICommandList.hのバインド寿命の説明を参照)
                    cmd->SetComputePipelineState(m_CloudDetailNoisePipelineState.get());
                    cmd->SetComputeUnorderedAccessTexture(0, cloudDetailNoiseTexture, 0);
                    const uint32_t detailGroups = (Passes::kCloudDetailNoiseSize + kGroupSize - 1) / kGroupSize;
                    cmd->Dispatch(detailGroups, detailGroups, detailGroups);

                    // ウェザーマップ(H3)は2Dなのでスレッドグループが8x8(=64。上の4x4x4と同じ粒度)。
                    // 4096^2 = 1,678万テクセルを一度だけ焼く
                    constexpr uint32_t kWeatherGroupSize = 8;
                    cmd->SetComputePipelineState(m_CloudWeatherNoisePipelineState.get());
                    cmd->SetComputeUnorderedAccessTexture(0, cloudWeatherNoiseTexture, 0);
                    const uint32_t weatherGroups =
                        (Passes::kCloudWeatherNoiseSize + kWeatherGroupSize - 1) / kWeatherGroupSize;
                    cmd->Dispatch(weatherGroups, weatherGroups, 1);
                },
            });
            m_CloudNoiseBaked = true;
        }

        // --- プリフィルタ済み鏡面の畳み込みパス: スカイボックスを入力に、ミップごとに異なる
        //     ラフネスで畳み込む(面×ミップの組み合わせごとに1回ずつディスパッチ) ---
        if (!m_IBLBaked)
        {
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "IBLPrefilter",
                .Reads = { skyTexture },
                .Writes = { prefilteredEnvTexture },
                .Execute = [this, iblPrefilterConstantBuffer, iblPrefilterPipelineState, prefilteredEnvTexture, skyTexture, materialSamplers](RHI::IRHICommandList* cmd)
                {
                    cmd->SetComputePipelineState(iblPrefilterPipelineState);
                    cmd->SetComputeTexture(0, skyTexture);
                    cmd->SetComputeSamplerSet(materialSamplers);
                    for (uint32_t mip = 0; mip < Passes::kIBLPrefilterMipLevels; ++mip)
                    {
                        const uint32_t mipSize = std::max(1u, Passes::kIBLPrefilterBaseSize >> mip);
                        const float roughness = static_cast<float>(mip) / static_cast<float>(Passes::kIBLPrefilterMipLevels - 1);
                        for (uint32_t face = 0; face < kCubeFaceCount; ++face)
                        {
                            IBLFaceConstants faceConstants{};
                            faceConstants.Face = face;
                            faceConstants.Roughness = roughness;
                            cmd->UpdateBuffer(iblPrefilterConstantBuffer, &faceConstants, sizeof(faceConstants));
                            cmd->SetComputeConstantBuffer(0, iblPrefilterConstantBuffer);
                            cmd->SetComputeUnorderedAccessTextureCubeFace(0, prefilteredEnvTexture, face, mip);
                            cmd->Dispatch((mipSize + 7) / 8, (mipSize + 7) / 8, 1);
                        }
                    }
                },
            });
            m_IBLBaked = true;
        }

        // --- 専用の拡散イラディアンスマップ(検証用に残している経路) ---
        // 既定の描画経路はプリフィルタ済み鏡面の最終ミップ(roughness=1)である。CSPrefilterは
        // V=R=Nを仮定しているためroughness=1のGGXはコサイン畳み込みへ厳密に退化し、専用マップと
        // 同じE(N)/πを与える(14.10節。White Furnace Testで画素一致を確認済み)。そのため通常の
        // 描画では1テクセルあたり約15,876サンプル(全体で約9,750万サンプル)のCSIrradianceを
        // 一切実行しない。この畳み込み処理自体はいつでも検証できるよう残してあり、ImGuiの
        // 「Use Dedicated Irradiance Map」トグルか、Render Targetsでイラディアンス表示を選んだ
        // ときだけ焼く。RenderGraphがReads/Writesから順序付けるため、トグルを入れたその同じ
        // フレームでLightingパスより先に実行される
        const bool needIrradianceBake =
            frame.Settings.IBL.UseDedicatedIrradiance || frame.Settings.DebugView.View == DebugView::IBLIrradiance;
        if (needIrradianceBake && !m_IBLIrradianceBaked)
        {
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "IBLIrradianceBake",
                .Reads = { skyTexture },
                .Writes = { irradianceTexture },
                .Execute = [this, iblPrefilterConstantBuffer, irradianceTexture, iblSettings, skyTexture, materialSamplers](RHI::IRHICommandList* cmd)
                {
                    // 拡散イラディアンス(本物のTextureCube、32x32x6面)。HLSLはリソースを動的に
                    // スライス選択できないため、面ごとに1回ずつディスパッチする。
                    //
                    // m_IBLSettings.UseSHIrradianceでCSIrradiance(総当たり積分、約9,750万
                    // サンプル)とSH L2経路(CSProjectSH→CSProjectSHFinal→CSEvaluateSH、
                    // 射影は24,576テクセルを1回ずつ読むだけ)を切り替えられる。
                    // 出力(IBLResources::IrradianceTexture)の形・規約はどちらの経路でも完全に同一
                    if (iblSettings.UseSHIrradiance)
                    {
                        IBLFaceConstants shConstants{};
                        shConstants.SHProjectionSize = static_cast<float>(Passes::kSHProjectionSize);
                        shConstants.SHWindowLambda = iblSettings.SHWindowLambda;

                        // --- 1. 射影: ソースキューブ全体を1回だけ読んで9個の係数(RGB)へ集約する ---
                        cmd->UpdateBuffer(iblPrefilterConstantBuffer, &shConstants, sizeof(shConstants));
                        cmd->SetComputeConstantBuffer(0, iblPrefilterConstantBuffer);
                        cmd->SetComputePipelineState(m_ProjectSHPipelineState.get());
                        cmd->SetComputeTexture(0, skyTexture);
                        cmd->SetComputeSamplerSet(materialSamplers);
                        cmd->SetComputeUnorderedAccessBuffer(0, m_SHPartialSumsBuffer.get());
                        const uint32_t groupsPerSide = (Passes::kSHProjectionSize + 7) / 8;
                        cmd->Dispatch(groupsPerSide, groupsPerSide, kCubeFaceCount);

                        // --- 2. 最終合算: 全グループぶんの部分和を1ディスパッチでまとめる ---
                        // (SHProjectionSizeはCSProjectSHと同じ値でなければグループ番号の対応がずれる)
                        cmd->UpdateBuffer(iblPrefilterConstantBuffer, &shConstants, sizeof(shConstants));
                        cmd->SetComputeConstantBuffer(0, iblPrefilterConstantBuffer);
                        cmd->SetComputePipelineState(m_ProjectSHFinalPipelineState.get());
                        cmd->SetComputeShaderResourceBuffer(1, m_SHPartialSumsBuffer.get());
                        cmd->SetComputeUnorderedAccessBuffer(0, m_SHCoefficientsBuffer.get());
                        cmd->Dispatch(1, 1, 1);

                        // --- 3. 評価: 9個の係数から出力テクセルごとのirradianceを求める ---
                        cmd->SetComputePipelineState(m_EvaluateSHPipelineState.get());
                        cmd->SetComputeShaderResourceBuffer(1, m_SHCoefficientsBuffer.get());
                        for (uint32_t face = 0; face < kCubeFaceCount; ++face)
                        {
                            IBLFaceConstants faceConstants{};
                            faceConstants.Face = face;
                            faceConstants.SHWindowLambda = iblSettings.SHWindowLambda;
                            cmd->UpdateBuffer(iblPrefilterConstantBuffer, &faceConstants, sizeof(faceConstants));
                            cmd->SetComputeConstantBuffer(0, iblPrefilterConstantBuffer);
                            cmd->SetComputeUnorderedAccessTextureCubeFace(0, irradianceTexture, face, 0);
                            cmd->Dispatch((Passes::kIBLIrradianceSize + 7) / 8, (Passes::kIBLIrradianceSize + 7) / 8, 1);
                        }
                    }
                    else
                    {
                        cmd->SetComputePipelineState(m_IrradiancePipelineState.get());
                        cmd->SetComputeTexture(0, skyTexture);
                        cmd->SetComputeSamplerSet(materialSamplers);
                        for (uint32_t face = 0; face < kCubeFaceCount; ++face)
                        {
                            IBLFaceConstants faceConstants{};
                            faceConstants.Face = face;
                            cmd->UpdateBuffer(iblPrefilterConstantBuffer, &faceConstants, sizeof(faceConstants));
                            cmd->SetComputeConstantBuffer(0, iblPrefilterConstantBuffer);
                            cmd->SetComputeUnorderedAccessTextureCubeFace(0, irradianceTexture, face, 0);
                            cmd->Dispatch((Passes::kIBLIrradianceSize + 7) / 8, (Passes::kIBLIrradianceSize + 7) / 8, 1);
                        }
                    }
                },
            });
            m_IBLIrradianceBaked = true;
            Core::Logger::Info("KurenaiEngine3D", "検証用の拡散イラディアンスマップを焼きました(通常の描画経路では使用しません)");
        }
    }
}
