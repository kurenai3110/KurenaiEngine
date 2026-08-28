#pragma once

#include "UI/IPanel.h"

namespace Kurenai
{
    class KurenaiEngine3D;
}

namespace Kurenai::UI
{
    // ストリーミングの常駐状態を俯瞰(XZ平面)の地図で見るパネル。
    //
    // 各モデルインスタンスのワールドAABBをXZ平面へ落とした矩形で描き、
    // 常駐 / 読み込み中 / 未読み込み で色分けする。カメラの位置と向きも重ねる。
    //
    // 【なぜ地図なのか】ストリーミングの破綻(破棄が早すぎる・範囲内なのに読み込まれない)は
    // 画面を見ても分からない。「そこに何も無い」のが正しいのか間違いなのかを、絵からは
    // 区別できないため。位置ごとの常駐状態を一望できる形にしておけば、抜けが
    // どのあたりで起きているかが1枚で分かる(東京23区は32km四方に767タイルが並ぶ)。
    //
    // 描画はImGuiのDrawListだけで行い、レンダラには一切触れない
    class StreamingPanel final : public IPanel
    {
    public:
        explicit StreamingPanel(KurenaiEngine3D& engine) : m_Engine(engine) {}

        // ###以降がウィンドウIDになる。imgui.iniとドックレイアウトのキーになるため
        // ###以降は変更しないこと(表示名だけなら変更してよい)
        const char* GetWindowName() const override { return "ストリーミング###Streaming"; }
        const char* GetMenuLabel() const override { return "ストリーミング"; }
        void Draw(const PanelDrawContext& context) override;

    private:
        KurenaiEngine3D& m_Engine;

        // 地図の表示倍率。1.0でシーン全体がちょうど収まる。上げると拡大する
        float m_MapZoom = 1.0f;
        // カメラを地図の中心に置き続けるか。オフならシーン全体の中心に固定する。
        // 32km四方のシーンを拡大して見るときは、追従していないとすぐ画面外へ出る
        bool m_FollowCamera = false;
    };
}
