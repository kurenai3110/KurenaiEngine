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
        // これが有効なフレームだけHi-Zパスも構築される(RenderTargets::HiZTextureのコメント参照)
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

        // --- 深度プリパス(41.22節) ------------------------------------------------------
        // プリパスを走らせるか。オーバードローが小さいシーンでは、増えるジオメトリ1周ぶんが
        // 省けるピクセルシェーダーより高くつくため切れるようにしてある
        bool DepthPrepassEnabled = Defaults::DepthPrepassEnabled;
        // メッシュ単位のフラスタムカリングを行うか(対照実験用。EngineDefaults.h参照)。
        // OFFのあいだは判定を1回も呼ばないので、統計は「判定なし」になる
        bool MeshCullingEnabled = Defaults::MeshCullingEnabled;

        // --- モデルLOD(.ksceneの[Model]LODPath / LODDistance) --------------------------------
        // 段の切り替えにかける秒数。0にするとポップする(1.1km四方のタイルが丸ごと入れ替わるため
        // 目立つ)。根拠は docs/ImplementationDetail.md
        float LODFadeDuration = 0.25f;
        // 切り替え距離のヒステリシス幅。切替点の±5%を不感帯にして、境界での往復を防ぐ
        float LODHysteresis = 0.05f;

        // --- 自前ソフトウェアラスタライザ(46章) -------------------------------------------
        // スクリーンbboxの画素面積がこれを超えた三角形は、1スレッドでラスタライズせず
        // 巨大三角形リストへ回す既定値。4096 = 64x64相当。
        //
        // 【この値が上限を決めている】小三角形パスは1スレッド1三角形なので、
        // このしきい値がそのまま「1スレッドが回す最大ループ回数」になる。
        // 上げすぎると画面を覆う三角形1個でTDRに達する
        static constexpr uint32_t kSWRasterDefaultLargeTriangleArea = 4096;
        // しきい値の可動範囲。UIから振って2つの経路を突き合わせるために使う(下のメンバ参照)
        static constexpr uint32_t kSWRasterMinLargeTriangleArea = 16;
        static constexpr uint32_t kSWRasterMaxLargeTriangleArea = 1u << 24;
        // 巨大三角形とみなすbbox画素面積のしきい値。
        //
        // 【実行時に振れるようにしている理由】小三角形パス(CSRaster)と巨大三角形パス
        // (CSRasterLarge)は同じ三角形を別のコードで塗る。極端に小さくすればほぼ全三角形が
        // 巨大リストへ回り、極端に大きくすればすべてCSRaster単独になるので、
        // **両極端で同じ絵が出ること**を確かめれば2つの経路が一致していると言える。
        // ビルドし直さずにこの対照実験ができるよう定数ではなくメンバにしてある
        // (「片方が実行されていない」という失敗を先に潰すための手順。ab-compareスキル)
        int SoftwareRasterLargeTriangleArea = static_cast<int>(kSWRasterDefaultLargeTriangleArea);
    };
}
