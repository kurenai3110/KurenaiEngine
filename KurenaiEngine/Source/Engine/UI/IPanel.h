#pragma once

namespace Kurenai::Core
{
    class Camera;
}

namespace Kurenai::UI
{
    // パネル描画のたびにUIManagerから渡す、そのフレーム限りのデータ。
    // KurenaiEngine3D本体への参照は各パネルがコンストラクタで受け取って保持するため
    // ここには含めない(毎フレーム渡す必要がないものと寿命が違うものを混ぜないため)
    struct PanelDrawContext
    {
        // FrameState::Cameraのスナップショット。Lightingパネルの「Add」が
        // 新規ライトの初期位置にカメラ位置を使う
        const Core::Camera* Camera = nullptr;
    };

    // 1つのImGuiウィンドウ(=ドックの1タブ)に対応するUIパネル。
    // 生成と所有はUIManagerが行い、表示/非表示もUIManagerがメニューバーから切り替える
    class IPanel
    {
    public:
        virtual ~IPanel() = default;

        IPanel(const IPanel&) = delete;
        IPanel& operator=(const IPanel&) = delete;

        // ImGui::Beginへ渡すウィンドウ名。imgui.iniとドックレイアウトのキーになるため、
        // 一度決めたら変更しないこと(変えると既存のimgui.iniのレイアウトが失われる)。
        // 表示名を日本語にする場合は "日本語表示###ASCII名" の ### 記法を使い、
        // ### より後ろ(=ID)を変えないようにする
        virtual const char* GetWindowName() const = 0;

        // メニューバーの「Window」に出す表示名
        virtual const char* GetMenuLabel() const = 0;

        // ImGui::Begin / Endも含めてこの中で完結させる。
        // 呼び出し元(Renderスレッド)はm_SceneMutexを保持済み
        virtual void Draw(const PanelDrawContext& context) = 0;

        bool IsVisible() const { return m_Visible; }
        bool* GetVisiblePtr() { return &m_Visible; }
        void SetVisible(bool visible) { m_Visible = visible; }

    protected:
        IPanel() = default;

        bool m_Visible = true;
    };
}
