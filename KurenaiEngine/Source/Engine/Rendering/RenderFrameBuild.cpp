#include "../KurenaiEngine3D.h"

#include <algorithm>
#include <cmath>

#include "RenderFrameContext.h"
#include "SampleSequence.h"

// BuildFrameContext から切り出した、フレームの値を組み立てる各段(段階7.5)。
// KurenaiEngine3D のメンバ関数のまま、翻訳単位だけをここへ分けている
// (宣言は KurenaiEngine3D.h のまま。RenderFrame.cpp と同じ作法)。
//
// **定義の並びは BuildFrameContext から呼ぶ順に合わせてある。** 呼ぶ順が実行順の一部で、
// 後段が前段の書いた frameContext のフィールドを読むため、追うときはこの順で読む。
//
// 【ラムダを1つも書かないこと】段階6の寿命事故はすべてラムダの参照捕捉だった
// (Render() のローカルを捕捉したまま graph.Execute() まで生き延びる)。
// このファイルに `[&]` / `[=]` / `[this]` が1つも無いことを機械で確かめられるよう、
// 切り出し先は必ず名前付きメンバ関数にする。
//
// 【出力は必ず参照で受ける】とくに std::vector<GPULight> を値で受ける signature を
// 作らないこと。コンパイラは黙って通すが、組み立てた配列が捨てられる。
//
// 【frameContext のポインタ型フィールドへアドレスを書くのは BuildFrameContext の末尾だけ】
// 実体は Render() のローカルで、graph.Execute() まで生きている必要がある。
// その関係を1つの関数の中で読み切れるようにしておく
namespace Kurenai
{
    // フレームのジッターと、カメラ由来の行列を確定させる。
    //
    // 【最初に呼ぶこと】m_TAAFrameIndex の前進がここの最初の実行文で、
    // MegaLights のタイル格子ジッターと TAA のサブピクセルジッターの両方が
    // この番号から導かれる。呼ぶ位置が下がると、両者が別のフレーム番号を見る
    void KurenaiEngine3D::DecideFrameJitterAndCamera(
        const KurenaiEngine3D::FrameState& frameState, Rendering::RenderFrameContext& frameContext)
    {
        // --- TAAのサブピクセルジッター ---
        // 投影行列を1ピクセル未満だけずらして、同じ画素が毎フレームわずかに違う位置をサンプルする
        // ようにする。TAAが複数フレームぶんを蓄積することで実質的なスーパーサンプリングになる。
        // TAA無効時はジッターも必ず0にすること(ジッターだけ残ると画面が振動するだけになる)
        ++m_TAAFrameIndex;

        // --- MegaLights候補プールのタイル格子ジッター ---
        // 書き手・Initial/Spatial・Presentへ配る値をここで一度だけ決める。
        // 各パスが個別にフレーム番号から導くと、式の片側だけを直した際に別タイルを静かに読むため
        const bool megaLightsTileJitterEnabled = m_MegaLightsSettings.TileJitterMode != 0;
        DirectX::XMUINT2 megaLightsTileOffset{ 0u, 0u };
        if (m_MegaLightsSettings.TileJitterMode == 1)
        {
            // Halton(2,3)を16段階へ量子化する。RadicalInverseは[0,1)だが、丸め誤差でも
            // 16にならないようタイル幅-1で明示的に押さえる
            megaLightsTileOffset.x = std::min<uint32_t>(
                static_cast<uint32_t>(Rendering::RadicalInverse(m_TAAFrameIndex, 2u) * kLightTileSize),
                kLightTileSize - 1u);
            megaLightsTileOffset.y = std::min<uint32_t>(
                static_cast<uint32_t>(Rendering::RadicalInverse(m_TAAFrameIndex, 3u) * kLightTileSize),
                kLightTileSize - 1u);
        }
        frameContext.MegaLightsTileOffset = megaLightsTileOffset;
        // 無効時だけ従来のタイル数をそのまま使い、添字・乱数の種・ディスパッチ数を保存する。
        // モード2は対照実験なので、オフセット0でも有効側と同じ+1タイルを通す
        frameContext.MegaLightsEffectiveTilesX =
            megaLightsTileJitterEnabled ? (m_RenderTargets.LightTileCountX + 1u) : m_RenderTargets.LightTileCountX;
        frameContext.MegaLightsEffectiveTilesY =
            megaLightsTileJitterEnabled ? (m_RenderTargets.LightTileCountY + 1u) : m_RenderTargets.LightTileCountY;

        DirectX::XMFLOAT2 jitterOffsetPixels{ 0.0f, 0.0f };
        if (m_PostProcessSettings.TAAEnabled)
        {
            // Halton列の添字は1から始める。添字0はradical inverseの定義上どの基数でも0となり、
            // オフセットがピクセルの角(-0.5, -0.5)へ偏ってしまう
            const uint32_t haltonIndex = (m_TAAFrameIndex % Rendering::kTAAJitterSampleCount) + 1;
            jitterOffsetPixels.x =
                (Rendering::RadicalInverse(haltonIndex, 2) - 0.5f) * m_PostProcessSettings.TAAJitterScale;
            jitterOffsetPixels.y =
                (Rendering::RadicalInverse(haltonIndex, 3) - 0.5f) * m_PostProcessSettings.TAAJitterScale;
        }
        // ピクセル単位のオフセットをNDCとUVの2つの単位へ直す。
        // ピクセル座標は右が+x・下が+yなのに対しNDCは上が+yなので、yだけ符号が反転する
        // (この符号を落とすと縦方向のジッターと速度が逆向きになる)
        const DirectX::XMFLOAT2 jitterNdc{
            2.0f * jitterOffsetPixels.x / static_cast<float>(m_RenderWidth),
            -2.0f * jitterOffsetPixels.y / static_cast<float>(m_RenderHeight),
        };
        // NDC→UVは xy * (0.5, -0.5) + 0.5 なので、ジッターのUV換算はピクセル数/解像度そのものになる
        frameContext.JitterUv = {
            jitterOffsetPixels.x / static_cast<float>(m_RenderWidth),
            jitterOffsetPixels.y / static_cast<float>(m_RenderHeight),
        };

        // ビュー行列と「ジッター済み」射影行列をここで一度だけ確定させ、以降のカメラ由来の行列は
        // すべてこれらから作る。
        //
        // 【なぜ行列の掛け算でジッターを入れられるのか】Camera::GetProjectionMatrixは行ベクトル規約
        // (clip = view * P)で、第3列が(0,0,1,0)すなわち clip.w = viewZ である。
        // XMMatrixTranslationは行ベクトル規約では第3行が(jx, jy, 0, 1)になるので、P * T を展開すると
        // 変化するのは要素[2][0]と[2][1]、つまり clip.xy += jitterNdc * clip.w だけになる。
        // w除算後には ndc.xy += jitterNdc という定数オフセットになり、狙いどおり平行移動として効く。
        //
        // 【なぜ全パスで統一するのか】深度バッファはこのジッター済み行列でラスタライズされる。
        // 深度から位置を復元する側(SSAO/SSIL/SSR/スクリーンスペースシャドウ)がジッター前の行列を
        // 使うと、再構成した位置がサブピクセルぶんずれて自己遮蔽やハローの原因になる。
        // なお射影行列の_33/_43(深度のリニアライズ係数)はジッターでは変化しない
        frameContext.ViewMatrix = frameState.Camera.GetViewMatrix();
        frameContext.JitteredProj =
            frameState.Camera.GetProjectionMatrix() * DirectX::XMMatrixTranslation(jitterNdc.x, jitterNdc.y, 0.0f);
    }
}
