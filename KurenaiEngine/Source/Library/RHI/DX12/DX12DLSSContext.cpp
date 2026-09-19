#include "DX12DLSSContext.h"

#include <cstdio>
#include <string>

#include "DX12CommandList.h"
#include "DX12Device.h"
#include "DX12Texture.h"
#include "Core/Logger.h"
#include "Core/StringUtil.h"

namespace Kurenai::RHI
{
    namespace
    {
        // 数値を16進8桁の文字列にする(NGXの結果コードとフィーチャフラグのログ用)
        std::string ToHex8(unsigned int value)
        {
            char buffer[16]{};
            snprintf(buffer, sizeof(buffer), "%08X", value);
            return std::string(buffer);
        }

        // NGXの結果コードを人間が読める形にする。GetNGXResultAsStringはwchar_tを返すため変換する
        std::string NGXResultToString(NVSDK_NGX_Result result)
        {
            const wchar_t* text = GetNGXResultAsString(result);
            const std::string message = text != nullptr ? Core::WideToUtf8(text) : std::string("(不明)");
            return message + "(0x" + ToHex8(static_cast<unsigned int>(result)) + ")";
        }

        // DLSSQualityとNVSDK_NGX_PerfQuality_Valueの対応表。
        // 【エンジン側の列挙を直接キャストしてはいけない】NGX側の並びは
        // MaxPerf → Balanced → MaxQuality → UltraPerformance → UltraQuality → DLAA で、
        // エンジン側(品質の高い順)とは一致しない
        NVSDK_NGX_PerfQuality_Value ToNGXPerfQuality(DLSSQuality quality)
        {
            switch (quality)
            {
            case DLSSQuality::DLAA:             return NVSDK_NGX_PerfQuality_Value_DLAA;
            case DLSSQuality::UltraQuality:     return NVSDK_NGX_PerfQuality_Value_UltraQuality;
            case DLSSQuality::Quality:          return NVSDK_NGX_PerfQuality_Value_MaxQuality;
            case DLSSQuality::Balanced:         return NVSDK_NGX_PerfQuality_Value_Balanced;
            case DLSSQuality::Performance:      return NVSDK_NGX_PerfQuality_Value_MaxPerf;
            case DLSSQuality::UltraPerformance: return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
            default:
                Core::Logger::Error(
                    "DLSS",
                    "ToNGXPerfQuality: 未知の品質モード(" + std::to_string(static_cast<int>(quality)) +
                        ")です。Quality(MaxQuality)として扱います");
                return NVSDK_NGX_PerfQuality_Value_MaxQuality;
            }
        }

        // フィーチャの生成条件が同じか。1つでも違えば作り直しが必要
        bool SameFeatureDesc(const DLSSFeatureDesc& a, const DLSSFeatureDesc& b)
        {
            return a.RenderWidth == b.RenderWidth && a.RenderHeight == b.RenderHeight &&
                   a.OutputWidth == b.OutputWidth && a.OutputHeight == b.OutputHeight && a.Quality == b.Quality &&
                   a.DepthInverted == b.DepthInverted && a.AutoExposure == b.AutoExposure &&
                   a.MotionVectorsAtRenderResolution == b.MotionVectorsAtRenderResolution;
        }
    }

    DX12DLSSContext::DX12DLSSContext(DX12Device* device) : m_Device(device) {}

    DX12DLSSContext::~DX12DLSSContext()
    {
        // フィーチャが抱えている内部バッファはGPUがまだ読んでいる可能性がある。
        // このコンテキストを壊すのは終了時・API切り替え時だけなので、素直に待ってから解放する
        if (m_Device != nullptr)
        {
            m_Device->WaitForGPUIdle();
        }
        ReleaseFeature();
    }

    void DX12DLSSContext::ReleaseFeature()
    {
        if (m_Handle != nullptr)
        {
            const NVSDK_NGX_Result result = NVSDK_NGX_D3D12_ReleaseFeature(m_Handle);
            if (NVSDK_NGX_FAILED(result))
            {
                Core::Logger::Warning(
                    "DLSS", "NVSDK_NGX_D3D12_ReleaseFeatureに失敗しました: " + NGXResultToString(result));
            }
            m_Handle = nullptr;
        }

        if (m_Parameters != nullptr)
        {
            const NVSDK_NGX_Result result = NVSDK_NGX_D3D12_DestroyParameters(m_Parameters);
            if (NVSDK_NGX_FAILED(result))
            {
                Core::Logger::Warning(
                    "DLSS", "NVSDK_NGX_D3D12_DestroyParametersに失敗しました: " + NGXResultToString(result));
            }
            m_Parameters = nullptr;
        }

        m_HasFeature = false;
        m_CurrentDesc = DLSSFeatureDesc{};
    }

    bool DX12DLSSContext::QueryOptimalSettings(
        uint32_t outputWidth, uint32_t outputHeight, DLSSQuality quality, DLSSOptimalSettings& outSettings)
    {
        outSettings = DLSSOptimalSettings{};

        if (outputWidth == 0 || outputHeight == 0)
        {
            Core::Logger::Error(
                "DLSS",
                "QueryOptimalSettings: 出力解像度が0です(" + std::to_string(outputWidth) + "x" +
                    std::to_string(outputHeight) + ")");
            return false;
        }

        // 能力問い合わせ用のパラメータブロックはNGXが所有する。こちらでDestroyしてはいけない
        NVSDK_NGX_Parameter* capabilityParameters = nullptr;
        NVSDK_NGX_Result result = NVSDK_NGX_D3D12_GetCapabilityParameters(&capabilityParameters);
        if (NVSDK_NGX_FAILED(result) || capabilityParameters == nullptr)
        {
            Core::Logger::Error(
                "DLSS", "NVSDK_NGX_D3D12_GetCapabilityParametersに失敗しました: " + NGXResultToString(result));
            return false;
        }

        // sharpnessはDLSS 2.5以降で非推奨(NVSDK_NGX_DLSS_Feature_Flags_DoSharpeningがSR_DEPRECATED)。
        // 受け取るだけで使わない
        float ignoredSharpness = 0.0f;
        result = NGX_DLSS_GET_OPTIMAL_SETTINGS(
            capabilityParameters, outputWidth, outputHeight, ToNGXPerfQuality(quality), &outSettings.RenderWidth,
            &outSettings.RenderHeight, &outSettings.MaxRenderWidth, &outSettings.MaxRenderHeight,
            &outSettings.MinRenderWidth, &outSettings.MinRenderHeight, &ignoredSharpness);
        if (NVSDK_NGX_FAILED(result))
        {
            Core::Logger::Error(
                "DLSS", "NGX_DLSS_GET_OPTIMAL_SETTINGSに失敗しました: " + NGXResultToString(result));
            return false;
        }

        // 【0が返るのは「その品質モードが非対応」の合図】NGXは失敗コードではなく寸法0で返す。
        // 呼び出し側が別の品質モードへ落とせるよう、エラーにせず理由を残してfalseを返す
        if (outSettings.RenderWidth == 0 || outSettings.RenderHeight == 0)
        {
            Core::Logger::Warning(
                "DLSS",
                "品質モード" + std::to_string(static_cast<int>(quality)) +
                    "はこの環境のDLSSでは非対応です(推奨レンダー解像度が0で返りました)");
            return false;
        }

        return true;
    }

    bool DX12DLSSContext::EnsureFeature(IRHICommandList* commandList, const DLSSFeatureDesc& desc)
    {
        if (commandList == nullptr)
        {
            Core::Logger::Error("DLSS", "EnsureFeature: コマンドリストがnullptrです");
            return false;
        }

        if (desc.RenderWidth == 0 || desc.RenderHeight == 0 || desc.OutputWidth == 0 || desc.OutputHeight == 0)
        {
            Core::Logger::Error(
                "DLSS",
                "EnsureFeature: 解像度が0です(レンダー " + std::to_string(desc.RenderWidth) + "x" +
                    std::to_string(desc.RenderHeight) + " / 出力 " + std::to_string(desc.OutputWidth) + "x" +
                    std::to_string(desc.OutputHeight) + ")");
            return false;
        }

        if (m_HasFeature && SameFeatureDesc(m_CurrentDesc, desc))
        {
            return true;
        }

        // 作り直す。既存のフィーチャはGPUが読んでいる可能性があるため待ってから捨てる
        // (解像度・品質モードの変更は低頻度なので、素直に待ってよい)
        if (m_HasFeature)
        {
            m_Device->WaitForGPUIdle();
        }
        ReleaseFeature();

        NVSDK_NGX_Result result = NVSDK_NGX_D3D12_AllocateParameters(&m_Parameters);
        if (NVSDK_NGX_FAILED(result) || m_Parameters == nullptr)
        {
            Core::Logger::Error(
                "DLSS", "NVSDK_NGX_D3D12_AllocateParametersに失敗しました: " + NGXResultToString(result));
            m_Parameters = nullptr;
            return false;
        }

        int featureFlags = NVSDK_NGX_DLSS_Feature_Flags_None;
        // HDR(トーンマップ前の線形値)を渡す。このエンジンのDLSSはTonemapより前に入る
        featureFlags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
        if (desc.DepthInverted)
        {
            // Reverse-Z。近平面がz=1.0で遠平面がz=0.0
            featureFlags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
        }
        if (desc.MotionVectorsAtRenderResolution)
        {
            featureFlags |= NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
        }
        if (desc.AutoExposure)
        {
            featureFlags |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
        }
        // 【MVJitteredは立てない】GBufferVelocityはジッターを差し引いた値を書いている
        // (Shaders/3D/GBuffer.hlslのTAAParamsによるデジッター)。ジッター量は
        // InJitterOffsetX/Yで別に渡す

        NVSDK_NGX_DLSS_Create_Params createParams{};
        createParams.Feature.InWidth = desc.RenderWidth;
        createParams.Feature.InHeight = desc.RenderHeight;
        createParams.Feature.InTargetWidth = desc.OutputWidth;
        createParams.Feature.InTargetHeight = desc.OutputHeight;
        createParams.Feature.InPerfQualityValue = ToNGXPerfQuality(desc.Quality);
        createParams.InFeatureCreateFlags = featureFlags;

        // 【記録中のコマンドリストへ積む】NGXのフィーチャ生成はGPUコマンドを伴う。
        // このエンジンのDX12は1フレーム1本のコマンドリストへ全部記録する設計なので、それをそのまま使う
        auto* dx12CommandList = static_cast<DX12CommandList*>(commandList);
        result = NGX_D3D12_CREATE_DLSS_EXT(m_Device->GetCommandList(), 1, 1, &m_Handle, m_Parameters, &createParams);

        // NGXは生成中にもディスクリプタヒープとルートシグネチャを差し替える。
        // 失敗していても差し替えられている可能性があるため、結果を見る前に戻す
        dx12CommandList->RestoreBindingsAfterExternalCommands();

        if (NVSDK_NGX_FAILED(result) || m_Handle == nullptr)
        {
            Core::Logger::Error("DLSS", "NGX_D3D12_CREATE_DLSS_EXTに失敗しました: " + NGXResultToString(result));
            m_Handle = nullptr;
            NVSDK_NGX_D3D12_DestroyParameters(m_Parameters);
            m_Parameters = nullptr;
            return false;
        }

        m_CurrentDesc = desc;
        m_HasFeature = true;

        Core::Logger::Info(
            "DLSS",
            "フィーチャを作成しました: レンダー " + std::to_string(desc.RenderWidth) + "x" +
                std::to_string(desc.RenderHeight) + " → 出力 " + std::to_string(desc.OutputWidth) + "x" +
                std::to_string(desc.OutputHeight) + " / 品質モード " + std::to_string(static_cast<int>(desc.Quality)) +
                " / フィーチャフラグ 0x" + ToHex8(static_cast<unsigned int>(featureFlags)));
        return true;
    }

    bool DX12DLSSContext::Evaluate(IRHICommandList* commandList, const DLSSEvaluateDesc& desc)
    {
        if (commandList == nullptr)
        {
            Core::Logger::Error("DLSS", "Evaluate: コマンドリストがnullptrです");
            return false;
        }

        if (!m_HasFeature || m_Handle == nullptr || m_Parameters == nullptr)
        {
            Core::Logger::Error("DLSS", "Evaluate: フィーチャが作られていません(EnsureFeatureが成功していない)");
            return false;
        }

        if (desc.Color == nullptr || desc.Depth == nullptr || desc.MotionVectors == nullptr || desc.Output == nullptr)
        {
            Core::Logger::Error(
                "DLSS",
                "Evaluate: 入力テクスチャが揃っていません(Color/Depth/MotionVectors/Outputのいずれかがnullptr)");
            return false;
        }

        auto* dx12CommandList = static_cast<DX12CommandList*>(commandList);
        auto* cmdList = m_Device->GetCommandList();

        auto* color = static_cast<DX12Texture*>(desc.Color);
        auto* depth = static_cast<DX12Texture*>(desc.Depth);
        auto* motionVectors = static_cast<DX12Texture*>(desc.MotionVectors);
        auto* output = static_cast<DX12Texture*>(desc.Output);

        // 【リソース状態はNGXが面倒を見ない】このRHIには明示的なバリアAPIが無く、
        // DX12Texture::TransitionToによる暗黙遷移だけがある。NGXは渡されたリソースが
        // 適切な状態にあることを前提にするため、ここで揃える。
        // TransitionToは現在状態を更新するので、後続パスの暗黙遷移と食い違わない
        color->TransitionTo(cmdList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        depth->TransitionTo(cmdList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        motionVectors->TransitionTo(cmdList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        output->TransitionTo(cmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        NVSDK_NGX_D3D12_DLSS_Eval_Params evalParams{};
        evalParams.Feature.pInColor = color->GetResource();
        evalParams.Feature.pInOutput = output->GetResource();
        // シャープネスはDLSS 2.5以降で非推奨。0を渡す(このエンジンのシャープ化はTonemapが持つ)
        evalParams.Feature.InSharpness = 0.0f;
        evalParams.pInDepth = depth->GetResource();
        evalParams.pInMotionVectors = motionVectors->GetResource();
        evalParams.InJitterOffsetX = desc.JitterOffsetX;
        evalParams.InJitterOffsetY = desc.JitterOffsetY;
        evalParams.InMVScaleX = desc.MotionVectorScaleX;
        evalParams.InMVScaleY = desc.MotionVectorScaleY;
        evalParams.InRenderSubrectDimensions.Width = desc.RenderWidth;
        evalParams.InRenderSubrectDimensions.Height = desc.RenderHeight;
        evalParams.InReset = desc.ResetHistory ? 1 : 0;
        evalParams.InPreExposure = desc.PreExposure;

        const NVSDK_NGX_Result result = NGX_D3D12_EVALUATE_DLSS_EXT(cmdList, m_Handle, m_Parameters, &evalParams);

        // 【必ず呼ぶ】NGXが自前のディスクリプタヒープを束ねたまま戻るため、
        // ここで戻さないと後続パスがクラッシュせずに壊れた絵を出す
        dx12CommandList->RestoreBindingsAfterExternalCommands();

        if (NVSDK_NGX_FAILED(result))
        {
            Core::Logger::Error("DLSS", "NGX_D3D12_EVALUATE_DLSS_EXTに失敗しました: " + NGXResultToString(result));
            return false;
        }

        return true;
    }
}

// --- DX12Device 側の実装 ------------------------------------------------------------------
// NGXのヘッダをDX12Device.cpp / DX12DeviceCapabilities.cppへ持ち込まないため、
// DLSSに関わるDX12Deviceのメンバ関数だけをこの翻訳単位へ置く
// (DXR関連をDX12DeviceRaytracing.cppへ分けているのと同じ作法)
namespace Kurenai::RHI
{
    namespace
    {
        // NGXが吐くログをエンジンのログ(KurenaiEngine_DX12.log)へ流す。
        // 【任意のスレッドから呼ばれる】NGXの規約。Core::Loggerは内部で排他している
        void NVSDK_CONV NGXLogCallback(
            const char* message, NVSDK_NGX_Logging_Level loggingLevel, NVSDK_NGX_Feature sourceComponent)
        {
            (void)loggingLevel;
            (void)sourceComponent;
            if (message == nullptr)
            {
                return;
            }

            // NGXのメッセージは末尾に改行を持つことがある。ログの1行1件を崩さないよう落とす
            std::string text(message);
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
            {
                text.pop_back();
            }
            if (text.empty())
            {
                return;
            }

            Core::Logger::Info("NGX", text);
        }

        // NGXへ渡すプロジェクト識別子。NVIDIA側の集計に使われるもので、
        // 「アプリごとに固定のGUID文字列」であればよい(このエンジンのために1つ決めた値)
        constexpr const char* kNGXProjectId = "c0b7a1e4-6f2d-4c8a-9b55-3d1e7f04a2c9";
        constexpr const char* kNGXEngineVersion = "KurenaiEngine";
    }

    void DX12Device::DetectDLSSSupport()
    {
        m_SupportsDLSS = false;
        m_NGXInitialized = false;

        // NGXのログと内部データの置き場所。実行ファイルと同じフォルダにしておくと、
        // エンジンのログ(KurenaiEngine_DX12.log)と同じ場所に揃う
        const std::wstring applicationDataPath = Core::GetModuleDirectory();

        NVSDK_NGX_FeatureCommonInfo featureCommonInfo{};
        featureCommonInfo.LoggingInfo.LoggingCallback = &NGXLogCallback;
        featureCommonInfo.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
        // NGX自身のログファイルは作らせず、コールバック経由でエンジンのログへ一本化する
        featureCommonInfo.LoggingInfo.DisableOtherLoggingSinks = true;

        NVSDK_NGX_Result result = NVSDK_NGX_D3D12_Init_with_ProjectID(
            kNGXProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, kNGXEngineVersion,
            applicationDataPath.empty() ? L"." : applicationDataPath.c_str(), m_Device.Get(), &featureCommonInfo);
        if (NVSDK_NGX_FAILED(result))
        {
            // 非NVIDIA GPU・古いドライバ・nvngx_dlss.dllが見つからない場合はここで落ちる。
            // 上位層はSupportsDLSS()を見てFSR1相当へフォールバックするので、続行して構わない
            Core::Logger::Info(
                "DLSS", "DLSS非対応: NVSDK_NGX_D3D12_Initに失敗しました: " + NGXResultToString(result));
            return;
        }
        m_NGXInitialized = true;

        NVSDK_NGX_Parameter* capabilityParameters = nullptr;
        result = NVSDK_NGX_D3D12_GetCapabilityParameters(&capabilityParameters);
        if (NVSDK_NGX_FAILED(result) || capabilityParameters == nullptr)
        {
            Core::Logger::Warning(
                "DLSS",
                "DLSS非対応: NVSDK_NGX_D3D12_GetCapabilityParametersに失敗しました: " + NGXResultToString(result));
            return;
        }

        int available = 0;
        result = capabilityParameters->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available);
        if (NVSDK_NGX_FAILED(result))
        {
            Core::Logger::Warning(
                "DLSS", "DLSS非対応: SuperSampling.Availableを読めませんでした: " + NGXResultToString(result));
            return;
        }

        if (available == 0)
        {
            // 「なぜ使えないのか」はFeatureInitResultに入る。ドライバ更新が必要なだけのことも多いので必ず出す
            int initResult = 0;
            std::string reason = "(理由を取得できませんでした)";
            if (NVSDK_NGX_SUCCEED(
                    capabilityParameters->Get(NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &initResult)))
            {
                reason = NGXResultToString(static_cast<NVSDK_NGX_Result>(initResult));
            }

            int needsUpdatedDriver = 0;
            capabilityParameters->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsUpdatedDriver);

            Core::Logger::Info(
                "DLSS",
                std::string("DLSS非対応: このアダプタではSuperSamplingが利用できません。理由: ") + reason +
                    (needsUpdatedDriver != 0 ? " / ドライバの更新が必要です" : ""));
            return;
        }

        m_SupportsDLSS = true;
        Core::Logger::Info("DLSS", "DLSS対応: Super Resolution / DLAA が利用できます");
    }

    std::unique_ptr<IRHIDLSSContext> DX12Device::CreateDLSSContext()
    {
        if (!m_SupportsDLSS)
        {
            Core::Logger::Error(
                "DLSS", "CreateDLSSContext: この環境ではDLSSが利用できません。SupportsDLSS()で分岐してください");
            return nullptr;
        }

        return std::make_unique<DX12DLSSContext>(this);
    }

    void DX12Device::ShutdownNGX()
    {
        if (!m_NGXInitialized)
        {
            return;
        }

        const NVSDK_NGX_Result result = NVSDK_NGX_D3D12_Shutdown1(m_Device.Get());
        if (NVSDK_NGX_FAILED(result))
        {
            Core::Logger::Warning("DLSS", "NVSDK_NGX_D3D12_Shutdown1に失敗しました: " + NGXResultToString(result));
        }
        m_NGXInitialized = false;
        m_SupportsDLSS = false;
    }
}
