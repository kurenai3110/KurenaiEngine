#pragma once

#include <cstdint>

#include "../EngineDefaults.h"

namespace Kurenai
{
    struct CloudSettings
    {
        // DebugView::CloudNoiseSlice で表示する3Dノイズのスライス位置(0〜1、W方向)と、
        // 形状(128^3)とディテール(32^3)のどちらを見るか。タイル境界に継ぎ目が出ていないかを
        // 目と数値の両方で確認するために用意してある
        float NoiseDebugSlice = 0.0f;
        bool NoiseDebugShowDetail = false;

        // --- 雲 ---
        // 積雲(1層目)の有効/無効。巻雲は m_CirrusEnabled が別に持つ。
        // 無効時はFrameConstants.CloudParams0.xへ被覆率0を渡し、Sky.hlsli側の早期脱出
        // (SkyColor)を通す。CloudCoverageスライダー自体は動かせるが効果が出ない状態になる
        bool Enabled = Defaults::CloudEnabled;
        // 被覆率。0.40は写真の見た目に寄せて選んだ値であり、物理的な導出ではない
        // (実測で調整可能。EngineDefaults.h参照)
        float Coverage = Defaults::CloudCoverage;
        // 雲底の高度[m](**ワールドYの絶対高度**。Sky.hlsli EvaluateCloudLayerがレイと
        // 雲層スラブの交差を解くのに使う)。
        // 【P17で意味が変わった】以前は「カメラのワールドY基準」の相対高度で、雲層がカメラの
        // Yに追従していた(上空へ飛んでも雲の上に出られなかった)。渡す値そのものは変えていない
        // ため、カメラが地表付近にいる従来の構図では見た目は実質変わらない
        float Altitude = Defaults::CloudAltitude;
        // ワールド1mあたりのノイズ空間の距離(= 1/セルの広さ[m])。
        // 【C7で厚みに比例させたが撤去した】厚みを上げるとセルも広がる形にしていたが、
        // 「厚みを上げても横幅が広がったように見えない」という判断で外した。
        // 実際には測ると実効セル幅は厚みに正確に比例していた(厚み600/1200/2400で
        // 493/997/1988m)ものの、**同時に雲の背が高くなって空が埋まる**ため、
        // 幅の変化が埋まり具合の変化に飲み込まれて見えなかった。
        // 根本の問題は別にあり、密度がウェザーマップ(2次元)の掛け算で決まるので
        // **雲の輪郭が高さによって変わらない**(同じ形が積み上がるだけ)ことである
        float UvScale = Defaults::CloudUvScale;
        // 雲の種類の偏り(C4)。FrameConstants.CloudParams3.wへ載る。
        // Sky.hlsliのCloudTypeAtが場所ごとの種類(層雲/積雲/雄大積雲)を決めるとき、
        // 空全体をどちらへ寄せるかのバイアスになる。0.5が中立
        float TypeBias = Defaults::CloudTypeBias;
        float Density = Defaults::CloudDensity;
        // 風速[m/s]。実世界の速度としてUIで直感的に扱えるようにしてあり、ノイズ空間の移動量への
        // 換算(CloudUvScaleを掛ける)はRenderThreadMainのm_CloudScrollOffset更新側で行う
        float WindSpeed = Defaults::CloudWindSpeed;
        // 風向き(度)。太陽方位角(m_SunAzimuthDegrees)と同じ規約(X軸0度、Z軸(+方向)90度)
        float WindDirectionDegrees = Defaults::CloudWindDirectionDegrees;
        float ForwardG = Defaults::CloudForwardG;
        // 積雲をボリューム(スラブのレイマーチ)として描くか。falseで従来の平面へ戻る。
        // シェーダー側へはCloudParams1.wの厚みを0にすることで伝える(専用のフラグは持たない)
        bool Volumetric = Defaults::CloudVolumetric;
        // 雲底から雲頂までの厚み[m]。EngineDefaults::CloudThicknessのコメント参照
        float Thickness = Defaults::CloudThickness;
        // trueにすると雲のスクロールが止まる(m_WaterSettings.TimeFrozenの雲版。A/B比較などスクロールが
        // 揺れると困る場面で使う)
        bool TimeFrozen = Defaults::CloudTimeFrozen;
        // 積雲のボリュームレイマーチの段数。**このパスのコストの主なつまみ**。
        // FrameConstants::CloudQualityParams.xとして渡り、SkyCloud.hlslだけが読む
        // (ボリューム経路を持つのがこのシェーダーだけのため。詳細はそちらのコメント)。
        // 減らすと雲の内部の階調が粗くなる=絵が変わるので、41.17までの「見た目を変えない削減」
        // とは性質が違う。品質プリセットの低/中から振るための値である
        uint32_t RaymarchSteps = Defaults::CloudRaymarchSteps;

        // --- 巻雲(高層のレイヤーを2層目として追加し雲を多層化する) ---
        // m_CloudEnabled=falseのときと同じく、無効時はFrameConstants.CloudParams2.xへ
        // 被覆率0を渡し、Sky.hlsli側の早期脱出(SkyColor、判断C)を通す
        bool CirrusEnabled = Defaults::CirrusEnabled;
        float CirrusCoverage = Defaults::CirrusCoverage;
        // 雲底の高度[m](**ワールドYの絶対高度**。積雲と同じ規約。m_CloudAltitude参照)
        float CirrusAltitude = Defaults::CirrusAltitude;
        float CirrusUvScale = Defaults::CirrusUvScale;
        float CirrusDensity = Defaults::CirrusDensity;
        // 風速[m/s]。風向はm_CloudWindDirectionDegreesを積雲と共有する(同じ風系という前提)
        float CirrusWindSpeed = Defaults::CirrusWindSpeed;
        // fBmのUV(U方向)を伸ばして筋状にする倍率
        float CirrusAnisotropy = Defaults::CirrusAnisotropy;
    };
}
