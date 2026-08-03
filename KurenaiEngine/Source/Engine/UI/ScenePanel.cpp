#include "UI/ScenePanel.h"

#include <imgui.h>

#include <cfloat>
#include <cstdio>
#include <string>

#include <DirectXMath.h>

#include "Core/Logger.h"
#include "Core/StringUtil.h"
#include "KurenaiEngine3D.h"
#include "UI/UIWidgets.h"

namespace Kurenai::UI
{
    namespace
    {
        // 現在のカメラを.ksceneの[Camera]セクションにそのまま貼れる文字列にする(P16)。
        //
        // 【単位に注意】Core::CameraのYaw/Pitchはラジアン、.ksceneの[Camera]Yaw/Pitchは度。
        // 変換はSceneLoader.cppが読み込み時に行っている(XMConvertToRadians)ので、
        // 書き出すこちら側は逆向きに変換する。ここを間違えると貼り戻したときに別の向きになる
        std::string FormatCameraSection(const Core::Camera& camera)
        {
            const DirectX::XMFLOAT3& position = camera.GetPosition();
            char buffer[256];
            std::snprintf(
                buffer, sizeof(buffer),
                "[Camera]\nPosition = %.3f, %.3f, %.3f\nYaw = %.2f\nPitch = %.2f\n",
                position.x, position.y, position.z,
                DirectX::XMConvertToDegrees(camera.GetYaw()),
                DirectX::XMConvertToDegrees(camera.GetPitch()));
            return std::string(buffer);
        }
    }

    void ScenePanel::Draw(const PanelDrawContext& context)
    {
        if (!ImGui::Begin(GetWindowName(), GetVisiblePtr()))
        {
            // 折りたたまれている・ドックの非アクティブタブなどで中身が不要な場合も
            // End()は必ず呼ぶ必要がある(ImGuiのBegin/Endは戻り値に関わらず対で呼ぶ規約)
            ImGui::End();
            return;
        }

        // --- .ksceneのホットリロード(P16) ---
        //
        // 【なぜ一覧より前に置くか】シーンの切り替えはたまにしか使わないが、再読み込みと
        // カメラの書き出しは.ksceneを詰めている間ずっと使う。パネルは他のパネルとドックを
        // 分け合っていて既定の高さに全部は入らないため、常用するものを上に置いて
        // スクロールしてよいもの(一覧)を下へ回す。
        //
        // 一覧のボタンは「今と違うシーンへ切り替える」ものなので現在のシーンでは無効のままにし、
        // 「同じシーンをもう一度読む」はここに独立したボタンとして置く。
        // 読み込み経路は一覧のボタンとまったく同じ(RequestSceneLoad)
        ImGui::SeparatorText(".ksceneの再読み込み");

        if (ImGui::Button("再読み込み###ReloadScene", ImVec2(-FLT_MIN, 0.0f)))
        {
            m_Engine.RequestSceneLoad(m_Engine.m_CurrentSceneIndex);
        }
        // 説明は常時表示ではなくツールチップにする(上記の高さの事情)
        ItemHelp(
            "現在のシーンを読み直す。エンジンが読むのは実行ファイルの隣の Assets\\Scenes\\*.kscene で、"
            "リポジトリの Scenes\\ からはKurenaiPackerとビルド時のコピーを経て届くため、"
            "そちらを編集しても走っているエンジンには反映されない");

        BeginParamGroup();
        CheckboxEx(
            "変更を自動で反映する###SceneAutoReload", &m_Engine.m_SceneAutoReloadEnabled, false,
            "ファイルの更新時刻を250msごとに見て、変わっていたら自動で読み直す。"
            "書式が不正なときは警告を出して見送るのでシーンが空になることはない。"
            "既定はオフ — A/B比較の最中に勝手に読み直されると、同一条件で2回撮る対照が壊れるため");
        CheckboxEx(
            "カメラを保持する###SceneReloadKeepsCamera", &m_Engine.m_SceneReloadKeepsCamera, false,
            "オフ(既定)ならファイルの[Camera]を適用する。オンにすると今の視点のまま読み直すので、"
            "飛び回りながら空・水面・露出を詰めるときに使う。"
            "効くのは同じシーンの読み直しのときだけで、下の一覧で別のシーンへ切り替えたときは"
            "オンでも新しいシーンのカメラが適用される");
        EndParamGroup();

        // --- 現在のカメラを[Camera]の書式で書き出す(P16) ---
        //
        // UIにはカメラの位置・向きを表示する場所が他に無いため、「飛び回って構図を見つける →
        // その視点を.ksceneへ書き戻す」がこれまでできなかった。P10(構図の追い込み)で必要になる
        if (context.Camera != nullptr)
        {
            const std::string section = FormatCameraSection(*context.Camera);

            ImGui::SeparatorText("現在のカメラ");
            // 位置と向きを1行で出す。貼り付ける4行そのものはボタンのツールチップで確かめられる
            const DirectX::XMFLOAT3& position = context.Camera->GetPosition();
            ImGui::Text(
                "(%.1f, %.1f, %.1f)  Yaw %.1f  Pitch %.1f",
                position.x, position.y, position.z,
                DirectX::XMConvertToDegrees(context.Camera->GetYaw()),
                DirectX::XMConvertToDegrees(context.Camera->GetPitch()));
            if (ImGui::Button("[Camera]の書式でコピー###CopyCameraSection", ImVec2(-FLT_MIN, 0.0f)))
            {
                ImGui::SetClipboardText(section.c_str());
                // クリップボードが使えない環境でも値を拾えるようログにも残す
                Core::Logger::Info("ScenePanel", "現在のカメラをコピーしました:\n" + section);
            }
            ItemHelp((".ksceneへそのまま貼れる4行をクリップボードへ入れる:\n\n" + section).c_str());
        }

        // --- シーンの切り替え ---
        // 使用中のグラフィックスAPIはメニューバーに常時出しているため、ここでは扱わない
        ImGui::SeparatorText("シーンの切り替え");
        ImGui::TextDisabled("ボタンを押すとそのシーンを読み込む");

        for (size_t i = 0; i < m_Engine.m_SceneDisplayNames.size(); ++i)
        {
            const bool isCurrent = (i == m_Engine.m_CurrentSceneIndex);
            if (isCurrent)
            {
                ImGui::BeginDisabled();
            }

            const std::string label = Core::WideToUtf8(m_Engine.m_SceneDisplayNames[i]);
            if (ImGui::Button(label.c_str(), ImVec2(-FLT_MIN, 0.0f)))
            {
                // 実際の読み込みはLoaderスレッドが行うため、ここは要求を出すだけで即座に戻る
                // (KurenaiEngine3D::RequestSceneLoad参照)
                m_Engine.RequestSceneLoad(i);
            }

            if (isCurrent)
            {
                ImGui::EndDisabled();
            }
        }

        ImGui::End();
    }
}
