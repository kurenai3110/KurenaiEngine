#pragma once

#include <string>
#include <vector>

#include "Assets/ShowLoader.h"
#include "FormationGenerators.h"

namespace Kurenai
{
    class KurenaiEngine3D;
}

namespace Kurenai::ShowEditor
{
    // 標準の6形状(球・円環・二重らせん・格子・ハート・らせん)を生成したショー。
    // GUIの「標準の6形状を生成」とCLIの--generate-standardの両方がこれを使う
    Assets::ShowData BuildStandardShow(uint32_t droneCount);

    // 編集中のショー1本。ここが編集の実体で、Assets::ShowDataは入出力の形。
    //
    // 【生成器の種類とパレットは.kshowに残らない】ファイルが持つのは点と色だけなので、
    // 読み込んだ編隊は「どの生成器で作られたか」を復元できない。それでも編集は続けられる
    // ようにするため、生成器はKind::Count(=取り込み)として扱い、機体数を変えるときだけ
    // 点の添字を取り直す近似で揃える
    struct EditorFormation
    {
        std::string Name;
        GeneratorKind Kind = GeneratorKind::Count;  // Count = 取り込み(生成器不明)
        FormationPalette Palette = DefaultPalette(GeneratorKind::Count);
        std::vector<DirectX::XMFLOAT3> Positions;   // 正規化空間(原点中心・代表半径1)
        std::vector<DirectX::XMFLOAT3> Colors;      // 線形RGB
    };

    // ドローンショーのオーサリングUI。エンジンのImGuiコンテキストへ1枚ウィンドウを足し、
    // 変更があればその場でエンジンのショーを差し替えて絵に反映する。
    //
    // 【エンジンをそのまま使う理由】エディタが自前でレンダラーを持つと、トーンマップ・
    // ブルーム・露出が本番と別経路になり、ここで作った形は本番で見ると別物になる。
    // 発光点は特にそうで、加算合成とACESの組み合わせで「上げても白く飛ぶだけ」の領域がある
    class Editor
    {
    public:
        // engineはRun()の前に渡すこと。Draw()はRenderスレッドから呼ばれる
        explicit Editor(KurenaiEngine3D& engine, std::wstring initialPath);

        // SetExtraImGuiCallbackへ登録する描画本体
        void Draw();

    private:
        void DrawFileSection();
        void DrawShowSection();
        void DrawFormationListSection();
        void DrawSelectedFormationSection();

        // 現在の編集内容をエンジンへ送る(プレビュー)
        void ApplyToEngine();
        // 全編隊をm_DroneCountへ揃える。生成器の分かっている編隊は撒き直し、
        // 取り込みの編隊は添字を取り直す
        void ResizeAllFormations();

        Assets::ShowData ToShowData() const;
        void FromShowData(const Assets::ShowData& data);

        KurenaiEngine3D& m_Engine;

        // 入出力パス。OSのファイルダイアログは使わない ―― モーダルダイアログを
        // Renderスレッドから開くことになり、メッセージポンプの持ち主(Updateスレッド)と
        // 噛み合わない。テキストで持てば手順を記録・再現できるという利点もある
        std::string m_PathUtf8;
        // 直近の操作結果(ImGuiに1行で出す)
        std::string m_Status;
        bool m_StatusIsError = false;

        std::vector<EditorFormation> m_Formations;
        int m_Selected = 0;
        int m_DroneCount = 1500;
        float m_Speed = 1.0f;
        float m_HoldSeconds = 6.0f;
        float m_MorphSeconds = 4.0f;
        float m_Brightness = 1.0f;
        float m_Radius = 4.0f;
        float m_HoverAmplitude = 0.6f;
        int m_Seed = 20260804;

        // 編集があったフレームの終わりに一度だけエンジンへ送るための印。
        // ウィジェットごとに送ると、スライダーを1回動かすだけで数千点の再構築が何度も走る
        bool m_Dirty = true;
    };
}
