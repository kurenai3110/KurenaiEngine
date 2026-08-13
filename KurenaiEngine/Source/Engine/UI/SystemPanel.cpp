#include "UI/SystemPanel.h"

#include <imgui.h>

#include <cfloat>

#include "EngineDefaults.h"
#include "KurenaiEngine3D.h"
#include "UI/UIManager.h"
#include "UI/UIWidgets.h"

namespace Kurenai::UI
{
    void SystemPanel::Draw(const PanelDrawContext& context)
    {
        (void)context;

        if (!ImGui::Begin(GetWindowName(), GetVisiblePtr()))
        {
            ImGui::End();
            return;
        }

        DrawUsageHint();

        if (ImGui::CollapsingHeader("表示###Display", ImGuiTreeNodeFlags_DefaultOpen))
        {
            DrawDisplaySection();
        }
        if (ImGui::CollapsingHeader("解像度###Resolution", ImGuiTreeNodeFlags_DefaultOpen))
        {
            DrawResolutionSection();
        }
        if (ImGui::CollapsingHeader("グラフィックスAPI###GraphicsAPI"))
        {
            DrawGraphicsAPISection();
        }
        if (ImGui::CollapsingHeader("UI###UI", ImGuiTreeNodeFlags_DefaultOpen))
        {
            DrawUISection();
        }

        ImGui::End();
    }

    void SystemPanel::DrawDisplaySection()
    {
        BeginParamGroup();

        CheckboxEx(
            "垂直同期###EnableVSync", &m_Engine.m_VSyncEnabled, Defaults::VSyncEnabled,
            "Presentをディスプレイのリフレッシュに同期させる。ティアリングは消えるが遅延は増える");

        CheckboxEx(
            "フレームレート制限###FixedFPS", &m_Engine.m_FixedFPSEnabled, Defaults::FixedFPSEnabled,
            "指定したフレームレートを超えないように待機を入れる");

        if (m_Engine.m_FixedFPSEnabled)
        {
            static const char* kTargetFPSNames[] = { "30", "60", "120" };
            static const float kTargetFPSValues[] = { 30.0f, 60.0f, 120.0f };
            static_assert(
                IM_ARRAYSIZE(kTargetFPSNames) == IM_ARRAYSIZE(kTargetFPSValues),
                "表示名と値の並びを一致させること");

            // 見つからない場合は60fps相当の位置にしておく
            int targetFPSIndex = 1;
            int defaultIndex = 1;
            for (int i = 0; i < IM_ARRAYSIZE(kTargetFPSValues); ++i)
            {
                if (kTargetFPSValues[i] == m_Engine.m_TargetFPS)
                {
                    targetFPSIndex = i;
                }
                if (kTargetFPSValues[i] == Defaults::TargetFPS)
                {
                    defaultIndex = i;
                }
            }

            if (ComboEx(
                    "目標フレームレート###TargetFPS", &targetFPSIndex, kTargetFPSNames, IM_ARRAYSIZE(kTargetFPSNames),
                    defaultIndex, "上限とするフレームレート"))
            {
                m_Engine.m_TargetFPS = kTargetFPSValues[targetFPSIndex];
            }
        }

        CheckboxEx(
            "性能をログに記録###FrameStatsLogging", &m_Engine.m_FrameStatsLoggingEnabled,
            Defaults::FrameStatsLoggingEnabled,
            "FPS・CPU/GPUフレーム時間を1秒ごとにログファイルへ書き出す。"
            "このパネルの表示は実行中しか見えないため、後から実行同士を比較するにはこちらを使う");

        EndParamGroup();
    }

    void SystemPanel::DrawResolutionSection()
    {
        ImGui::TextWrapped(
            "G-Buffer以降すべての中間バッファの解像度。ウィンドウサイズとは独立していて、"
            "表示時はアスペクト比を保ったままウィンドウへ拡大縮小する"
            "(余る側にレターボックス/ピラーボックスが出る)");

        BeginParamGroup();

        // 表示名と値の並びは必ず一致させること(目標フレームレートのComboと同じ作法)
        static const char* kResolutionNames[] = { "1280x720", "1600x900", "1920x1080", "2560x1440", "3840x2160" };
        static const uint32_t kResolutionValues[][2] =
        {
            { 1280u, 720u }, { 1600u, 900u }, { 1920u, 1080u }, { 2560u, 1440u }, { 3840u, 2160u }
        };
        static_assert(
            IM_ARRAYSIZE(kResolutionNames) == IM_ARRAYSIZE(kResolutionValues), "表示名と値の並びを一致させること");

        // 一覧に無い解像度(「ウィンドウサイズに合わせる」で設定した場合など)のときは-1のままにする。
        // ImGuiのComboは範囲外のインデックスを空表示として扱うため、そのままでも壊れない
        int resolutionIndex = -1;
        int defaultIndex = 0;
        for (int i = 0; i < IM_ARRAYSIZE(kResolutionValues); ++i)
        {
            if (kResolutionValues[i][0] == m_Engine.m_RenderWidth && kResolutionValues[i][1] == m_Engine.m_RenderHeight)
            {
                resolutionIndex = i;
            }
            if (kResolutionValues[i][0] == Defaults::RenderWidth && kResolutionValues[i][1] == Defaults::RenderHeight)
            {
                defaultIndex = i;
            }
        }

        if (ComboEx(
                "内部レンダー解像度###RenderResolution", &resolutionIndex, kResolutionNames,
                IM_ARRAYSIZE(kResolutionNames), defaultIndex,
                "上げるほど精細になるがVRAM使用量とフレーム時間が増える。"
                "変更するとレンダーターゲットを作り直すため、TAAの履歴は一度破棄される"))
        {
            m_Engine.RequestRenderResolution(
                kResolutionValues[resolutionIndex][0], kResolutionValues[resolutionIndex][1]);
        }

        EndParamGroup();

        if (ImGui::Button("現在のウィンドウサイズに合わせる", ImVec2(-FLT_MIN, 0.0f)))
        {
            // 押した時点の1回きり。以後ウィンドウをリサイズしても内部解像度は追従しない
            // (毎フレーム追従させると、ドラッグ中に何度もレンダーターゲットを作り直すことになる)
            m_Engine.RequestRenderResolution(m_Engine.GetWidth(), m_Engine.GetHeight());
        }
        ItemHelp(
            "内部レンダー解像度をウィンドウのクライアント領域と同じにして、拡大縮小の無い等倍表示にする。"
            "押した時点で1回だけ適用され、その後のウィンドウリサイズには追従しない");

        ImGui::Text("内部レンダー解像度: %u x %u", m_Engine.m_RenderWidth, m_Engine.m_RenderHeight);
        ImGui::Text("ウィンドウ(クライアント領域): %u x %u", m_Engine.GetWidth(), m_Engine.GetHeight());
    }

    void SystemPanel::DrawGraphicsAPISection()
    {
        ImGui::TextWrapped(
            "描画に使うグラフィックスAPI。切り替えるとエンジンを作り直すため、"
            "ウィンドウが一度閉じてシーンが読み直される(数秒かかる)。"
            "シーンと内部レンダー解像度は引き継がれるが、それ以外の設定は既定値へ戻る");

        BeginParamGroup();

        static const char* kAPINames[] = { "DX11", "DX12" };
        static const GraphicsAPI kAPIValues[] = { GraphicsAPI::DX11, GraphicsAPI::DX12 };
        static_assert(IM_ARRAYSIZE(kAPINames) == IM_ARRAYSIZE(kAPIValues), "表示名と値の並びを一致させること");

        int apiIndex = static_cast<int>(m_Engine.m_GraphicsAPI);
        if (ComboEx(
                "グラフィックスAPI###GraphicsAPISelect", &apiIndex, kAPINames, IM_ARRAYSIZE(kAPINames),
                static_cast<int>(GraphicsAPI::DX11),
                "レイトレーシング(RT反射 / RTシャドウ / RTAO)はDX12かつDXR Tier 1.1対応のGPUでしか"
                "選べないため、DX11へ切り替えるとそれらの選択肢は消える。"
                "ログは KurenaiEngine_DX11.log / KurenaiEngine_DX12.log にバックエンドごとへ分かれて出る"))
        {
            m_Engine.RequestGraphicsAPIChange(kAPIValues[apiIndex]);
        }

        EndParamGroup();

        ImGui::Text(
            "レイトレーシング: %s", m_Engine.m_RaytracingAvailable ? "利用可能 (DXR Tier 1.1)" : "利用できません");
    }

    void SystemPanel::DrawUISection()
    {
        ImGui::TextUnformatted("パネルの表示");
        m_UIManager.DrawPanelVisibilityControls();

        ImGui::Separator();

        if (ImGui::Button("レイアウトを初期化", ImVec2(-FLT_MIN, 0.0f)))
        {
            m_UIManager.RequestResetLayout();
        }
        ItemHelp(
            "パネルの配置とサイズを既定へ戻し、閉じたパネルも表示し直す。"
            "配置はexeと同じフォルダのimgui.iniへ自動保存される");

        // UIの拡大率はWindowsのディスプレイ設定の拡大率に追従する
        ImGui::Text("UIスケール: %.2fx", ImGui::GetStyle().FontScaleMain);
        ItemHelp(
            "UI全体の拡大率。Windowsのディスプレイ設定の拡大率に追従する。"
            "全体をもう少し大きく/小さくしたい場合はUITheme::kUIScaleMultiplierを変える");
        ImGui::Text("Windowsの拡大率: %.0f%%", m_Engine.GetMonitorDpiScale() * 100.0f);
        ItemHelp("このウィンドウが乗っているモニタに対して、Windowsのディスプレイ設定で指定されている拡大率");
    }
}
