#pragma once

#include <cstdint>

#include "../EngineDefaults.h"

namespace Kurenai
{
    struct GeometrySettings
    {
        bool InstancingEnabled = Defaults::InstancingEnabled;

        // メッシュレット経路を使うか(ImGuiのレンダリングパネルから切り替える)。
        // 対応環境では既定で有効。無効にすると従来の頂点シェーダー描画に戻るため、
        // 見た目の差分を目で比較できる
        bool MeshletRenderingEnabled = true;
        // メッシュレットごとの色分け表示。m_MeshletRenderingEnabledが有効なときだけ効く
        bool MeshletDebugViewEnabled = false;
        // 増幅シェーダーのHi-Zオクルージョンカリング(Stage 5-2)。メッシュレットのバウンディング球を
        // 前フレームのHi-Zへ投影し、「視界内だが手前の何かに完全に隠れている」塊を落とす。
        //
        // 【メッシュレット経路でしか効かない】判定を書いてあるのは増幅シェーダーなので、
        // メッシュシェーダー非対応の環境(基準機のIntel UHD 620を含む)では一切走らない。
        // これが有効なフレームだけHi-Zパスも構築される(m_HiZTextureのコメント参照)
        bool OcclusionCullingEnabled = Defaults::OcclusionCullingEnabled;
        // オクルージョン判定でバウンディング球を膨らませる倍率。
        //
        // 【1.0が基準】判定に使うHi-Zは前フレームのものなので、そのフレームのカメラ移動ぶんは
        // 別項(移動距離をそのまま半径へ足す)で吸収している。この倍率が埋めるのはそれとは別の
        // 誤差 ―― バウンディング球がメッシュレットの実体より緩いこと、およびカメラ回転による
        // 見え方の変化。ポップ(隠れていないものが消える)が出たら上げる
        float OcclusionCullRadiusScale = Defaults::OcclusionCullRadiusScale;

        // --- メッシュレットカリングの統計(Stage 5-2) ---
        //
        // 【「間引き0」だけでは何も分からない】判定式が常に通しているのか、本当に全部
        // 見えているのかを区別できない。CPU側のフラスタムカリング(m_FrustumCullTested /
        // m_FrustumCullCulled)が判定数と対で出しているのと同じ理由で、ここでも対で出す。
        // **オクルージョンは視錐台+コーンとは別のカウンタにする** ―― 合算すると
        // 「俯瞰(遮蔽が少ない)と街路(遮蔽が多い)で差が出るか」という確認ができない。
        bool MeshletCullStatsEnabled = Defaults::MeshletCullStatsEnabled;

        // --- メッシュレットLOD(離散LOD。Stage 6) ---------------------------------------
        //
        // 段を選ぶのは増幅シェーダーで、ここにあるのはその入力。
        // 【1つのモデル内で段を混ぜない】選択の入力はモデルのバウンディング球とカメラだけで、
        // メッシュレットごとの値を使わない。段が混ざると、簡略化で頂点が動いた側と
        // 動いていない側で辺が一致せず、境目に穴が開く
        bool MeshletLODEnabled = Defaults::MeshletLODEnabled;
        float MeshletLODQuality = Defaults::MeshletLODQuality;
        int32_t MeshletLODForcedLevel = Defaults::MeshletLODForcedLevel;
        // 色分け表示を段ごとにする。上の「メッシュレットを色分け」が有効なときだけ効く
        bool MeshletLODDebugColorEnabled = false;

        // Hi-Zを深度プリパスの深度から作るか。切ると従来どおりG-Bufferの後で作り、
        // 判定は前フレームのHi-Zで行う(意味と効果はDefaults::HiZFromDepthPrepass)
        bool HiZFromDepthPrepassEnabled = Defaults::HiZFromDepthPrepass;
        bool ModelCullGpuEnabled = Defaults::ModelCullGpuEnabled;
        // カリング結果で実際に描画発行まで行うか。falseなら判定と計数だけ行い、
        // 描くのは従来のCPUループのまま(コストと効果をA/Bで測るためのトグル)
        bool ModelCullIndirectEnabled = Defaults::ModelCullIndirectEnabled;

        bool LightCullingEnabled = Defaults::LightCullingEnabled;
        bool SoftwareRasterEnabled = Defaults::SoftwareRasterEnabled;
    };
}
