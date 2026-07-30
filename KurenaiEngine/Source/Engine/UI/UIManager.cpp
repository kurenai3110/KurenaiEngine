#include "UI/UIManager.h"

#include <imgui.h>

#include "Core/ImGuiDockLayout.h"
#include "Core/Logger.h"
#include "KurenaiEngine3D.h"
#include "UI/DebugViewPanel.h"
#include "UI/LightingPanel.h"
#include "UI/PostProcessPanel.h"
#include "UI/ProfilerPanel.h"
#include "UI/RenderingPanel.h"
#include "UI/ScenePanel.h"
#include "UI/SystemPanel.h"
#include "UI/UITheme.h"

namespace Kurenai::UI
{
    namespace
    {
        // メインのドックスペースのID。imgui.iniのドックノードのキーになる。
        //
        // 末尾の1バイトはレイアウトの世代を表す。パネルを追加・削除・改名してレイアウトの
        // 構成そのものを変えた場合はここを1つ進めること。既存のimgui.iniに古い世代のノードしか
        // 無い状態ではHasNodeがfalseになり、既定レイアウトが1回だけ組み直される。
        // 進めないと、新しく増えたパネルだけが宙に浮いた状態でユーザーに見えてしまう。
        // (逆に、単に表示名を変えただけならウィンドウIDは"###"以降で決まるため進める必要はない)
        constexpr unsigned int kDockSpaceId = 0x4B554E42u; // 'KUNB' (世代B: 7パネル構成)
    }

    UIManager::UIManager(KurenaiEngine3D& engine)
        : m_Engine(engine)
    {
        // 見た目とフォントは最初のNewFrame()より前に設定する必要がある。
        // 実際の拡大率はRenderスレッドが毎フレームOnUIScaleChangedへ渡すので、
        // ここでは等倍で組んでおく(次のフレームで正しい値に置き換わる)
        UITheme::LoadFonts();
        OnUIScaleChanged(1.0f);

        // 並び順はメニューバーの「Window」に出る順序であり、既定のドックレイアウトを
        // 組むときの基準にもなる
        m_Panels.push_back(std::make_unique<ScenePanel>(engine));
        m_Panels.push_back(std::make_unique<RenderingPanel>(engine));
        m_Panels.push_back(std::make_unique<PostProcessPanel>(engine));
        m_Panels.push_back(std::make_unique<LightingPanel>(engine));
        m_Panels.push_back(std::make_unique<DebugViewPanel>(engine));
        m_Panels.push_back(std::make_unique<SystemPanel>(engine, *this));
        m_Panels.push_back(std::make_unique<ProfilerPanel>(engine));

        Core::Logger::Info("UIManager", "UIパネルを構築しました (" + std::to_string(m_Panels.size()) + "枚)");
    }

    UIManager::~UIManager() = default;

    void UIManager::Draw(const PanelDrawContext& context)
    {
        // マルチビューポートはこのプロジェクトでは使わない(動作確認のPostMessageによる自動クリックが
        // メインウィンドウのHWND前提のため。RHI/ImGuiContextSetup.cpp参照)。
        // 何らかの理由で有効化された場合に備え、毎フレーム落としてログに残す
        ImGuiIO& io = ImGui::GetIO();
        if ((io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0)
        {
            io.ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;
            Core::Logger::Warning("UIManager", "ImGuiConfigFlags_ViewportsEnableが有効だったため無効化しました");
        }

        const float menuBarHeight = DrawMainMenuBar();
        DrawDockSpaceAndLayout(menuBarHeight);

        for (const std::unique_ptr<IPanel>& panel : m_Panels)
        {
            if (panel == nullptr)
            {
                // 構築時に確保できていれば起こらないが、起きた場合に原因を追えるようにしておく
                Core::Logger::Error("UIManager", "パネルがnullptrのため描画をスキップしました");
                continue;
            }

            if (panel->IsVisible())
            {
                panel->Draw(context);
            }
        }
    }

    float UIManager::DrawMainMenuBar()
    {
        // ImGui::BeginMainMenuBarが内部で使う高さと同じ計算(imgui_widgets.cppのBeginMainMenuBar)。
        // 現在のスタイルから求まるので、拡大率を変えたそのフレームでも正しい値になる
        const float menuBarHeight = ImGui::GetFrameHeight();

        if (!ImGui::BeginMainMenuBar())
        {
            return 0.0f;
        }

        if (ImGui::BeginMenu("ウィンドウ"))
        {
            for (const std::unique_ptr<IPanel>& panel : m_Panels)
            {
                if (panel != nullptr)
                {
                    ImGui::MenuItem(panel->GetMenuLabel(), nullptr, panel->GetVisiblePtr());
                }
            }

            ImGui::Separator();
            if (ImGui::MenuItem("レイアウトを初期化"))
            {
                // 閉じられていたパネルも一緒に戻す。レイアウトだけ戻しても
                // 非表示のパネルは出てこないため、ユーザーの意図(初期状態に戻す)と合わない
                for (const std::unique_ptr<IPanel>& panel : m_Panels)
                {
                    if (panel != nullptr)
                    {
                        panel->SetVisible(true);
                    }
                }
                RequestResetLayout();
            }
            ImGui::EndMenu();
        }

        ImGui::Separator();
        ImGui::TextUnformatted(
            m_Engine.m_GraphicsAPI == GraphicsAPI::DX12 ? "Graphics API: DX12" : "Graphics API: DX11");

        ImGui::EndMainMenuBar();
        return menuBarHeight;
    }

    void UIManager::OnUIScaleChanged(float uiScale)
    {
        if (uiScale <= 0.0f || uiScale == m_AppliedUIScale)
        {
            return;
        }

        const float previousScale = m_AppliedUIScale;
        m_AppliedUIScale = uiScale;

        // UITheme::ApplyはImGuiStyleを既定へ戻してから組み直す冪等な実装なので、
        // 何度呼んでも寸法が累積しない。フォントの再ベイクはimgui 1.92の動的アトラスが
        // 自動で行うため、明示的なBuild()は不要
        UITheme::Apply(uiScale);

        // ImGuiがピクセル単位で保持しているレイアウト情報(ドックのパネル幅・高さ、各ウィンドウの
        // サイズと内容サイズ・スクロール量)も同じ比率で拡縮する。これをしないと文字だけが
        // 大きくなってパネルの幅が据え置きになったり、スクロールバーの表示が1フレーム乱れたりする
        // (詳細はImGuiDockLayout::ScaleForUIScaleChangeのコメント)。
        // previousScaleが0のときは起動直後の初回適用で、まだ拡縮すべきものが無いので何もしない
        if (previousScale > 0.0f)
        {
            Core::ImGuiDockLayout::ScaleForUIScaleChange(kDockSpaceId, uiScale / previousScale);
        }

        Core::Logger::Info("UITheme", "UI拡大率を " + std::to_string(uiScale) + " へ更新しました");
    }

    void UIManager::DrawPanelVisibilityControls()
    {
        for (const std::unique_ptr<IPanel>& panel : m_Panels)
        {
            if (panel != nullptr)
            {
                ImGui::Checkbox(panel->GetMenuLabel(), panel->GetVisiblePtr());
            }
        }
    }

    void UIManager::DrawDockSpaceAndLayout(float menuBarHeight)
    {
        // ドックスペースのIDは固定値にする。DockSpaceに0を渡すと内部でGetID("DockSpace")が
        // 使われるが、その値はホストウィンドウのIDスタックに依存するため呼び出し前には求められない。
        // 「初回起動かどうか」の判定はDockSpaceより前に行う必要がある
        // (DockSpaceは呼ばれた時点でノードを作ってしまうので、後からHasNodeを見ても常にtrueになる)
        m_DockSpaceId = kDockSpaceId;

        // 初回起動(imgui.iniにドック情報が無い)の判定は一度だけ行う。2回目以降の起動では
        // NewFrame内でimgui.iniからノードが復元済みなのでHasNodeがtrueになり、何もしない
        if (!m_LayoutChecked)
        {
            m_LayoutChecked = true;
            if (!Core::ImGuiDockLayout::HasNode(m_DockSpaceId))
            {
                m_ResetLayoutRequested = true;
            }
        }

        // PassthruCentralNode: どのパネルもドッキングされていない中央ノードを完全に透過させる。
        // これによりPresentパスが描いた3D映像がそのまま中央に見えるため、「3Dをテクスチャへ描いて
        // ImGui::Imageで出す」構成にしなくてもドッキングが成立する。
        // NoDockingOverCentralNode: 中央ノードを常に空のまま保つ。付けないとユーザーが誤って
        // 中央へパネルをドロップし、3D映像が完全に隠れてしまう
        const ImGuiDockNodeFlags dockFlags =
            ImGuiDockNodeFlags_PassthruCentralNode | ImGuiDockNodeFlags_NoDockingOverCentralNode;

        // ImGui::DockSpaceOverViewportと同等の処理を自前で行う。
        //
        // あちらはドックスペースの位置・サイズにviewport->WorkPos / WorkSize(メニューバーの
        // 高さを差し引いた領域)を使うが、この値は**1フレーム遅れる**。メニューバーの高さを
        // 作業領域へ反映する処理がimgui_widgets.cppのBeginViewportSideBarにあり、
        // 「Report our size into work area (for next frame)」というコメントのとおり
        // 次フレーム用のBuildWorkInsetMinへ足し込む仕様のため。
        // UIの拡大率が変わったフレームでは、メニューバーは新しい高さで描かれるのに
        // WorkPosは古い高さのままとなり、ドックスペース全体が1フレームだけ縦にずれる
        // (実際にディスプレイ間の移動で発生した)。
        // ここではメニューバーの高さを現在のスタイルから直接求めて使うことで、そのずれを防ぐ
        const ImGuiViewport* mainViewport = ImGui::GetMainViewport();
        if (mainViewport == nullptr)
        {
            Core::Logger::Error("UIManager", "メインビューポートを取得できないためドックスペースを出せません");
            return;
        }

        ImGui::SetNextWindowPos(ImVec2(mainViewport->Pos.x, mainViewport->Pos.y + menuBarHeight));
        ImGui::SetNextWindowSize(ImVec2(mainViewport->Size.x, mainViewport->Size.y - menuBarHeight));
        ImGui::SetNextWindowViewport(mainViewport->ID);

        // ドックスペースを載せるだけのホストウィンドウ。フラグはDockSpaceOverViewportと同じ。
        // PassthruCentralNode指定時はホストの背景を描かない(中央を透過させるため)
        ImGuiWindowFlags hostWindowFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                           ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                           ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                           ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::Begin("KurenaiDockSpaceHost", nullptr, hostWindowFlags);
        ImGui::PopStyleVar(3);

        ImGui::DockSpace(m_DockSpaceId, ImVec2(0.0f, 0.0f), dockFlags);

        ImGui::End();

        if (!m_ResetLayoutRequested)
        {
            return;
        }
        m_ResetLayoutRequested = false;

        // 同じスロットへ複数を割り当てたものは1つのドックノードのタブとしてまとまる
        // ウィンドウ名は各パネルのGetWindowName()と完全一致させること。
        // ImHashStrは"###"以降だけをハッシュするため、ここでは表示名を省いたID部分だけを書けばよい
        static const Core::ImGuiDockSlotDesc kDefaultSlots[] =
        {
            { "###Scenes",         Core::ImGuiDockSlot::Left },
            { "###System",         Core::ImGuiDockSlot::LeftBottom },
            { "###Rendering",      Core::ImGuiDockSlot::Right },
            { "###Post Processing", Core::ImGuiDockSlot::Right },       // Renderingと同じノード = タブになる
            { "###Lighting",       Core::ImGuiDockSlot::RightBottom },
            { "###Render Targets", Core::ImGuiDockSlot::RightBottom },  // Lightingと同じノード = タブになる
            { "###Profiler",       Core::ImGuiDockSlot::Bottom },
        };

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        if (viewport == nullptr)
        {
            Core::Logger::Error("UIManager", "メインビューポートを取得できないため既定レイアウトを構築できません");
            return;
        }

        // BuildDefaultは失敗時に自身でLogger::Errorを出す。失敗してもUI自体は
        // 手動配置のまま使えるため、ここでは続行する
        (void)Core::ImGuiDockLayout::BuildDefault(
            m_DockSpaceId, viewport->WorkSize.x, viewport->WorkSize.y, kDefaultSlots, IM_ARRAYSIZE(kDefaultSlots));
    }
}
