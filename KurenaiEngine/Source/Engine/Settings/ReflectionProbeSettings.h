#pragma once

#include <cstdint>

#include "../EngineDefaults.h"

namespace Kurenai
{
    // プローブの更新モード。焼き直しのコストと「シーンの変化への追従」のどちらを取るかの選択で、
    // ImGuiで切り替えて負荷と品質を比較できるようにしてある(19.10節)
    enum class ProbeUpdateMode
    {
        // シーン読み込み時とImGuiのBakeボタンのときだけ焼く。実行時コストはゼロだが、
        // ライトや時刻を動かしても反射は焼いた時点のまま止まる
        Baked,
        // 上に加えて、焼き上がりに影響する状態(時刻・太陽・ライト)の変化を検出して自動で焼き直す。
        // 変化していないフレームのコストはゼロだが、変化したフレームは全プローブぶんの
        // フルベイクが1フレームに集中する
        OnDemand,
        // 上に加えて、1プローブを12フレームかけて焼き直し、次のプローブへ回る(ラウンドロビン)。
        // 内訳は「6フレームで1面ずつキャプチャ」→「6フレームで1面ぶんのミップチェーンずつ畳み込み」
        // (畳み込みの割り当ての根拠はKurenaiEngine3D.cppのプリフィルタフェーズ参照)。
        // 全プローブを毎フレーム焼くとドローコールがプローブ数×6倍になり非現実的なため、
        // 時間分割を既定の実装方式にしている
        Realtime,
    };

    struct ReflectionProbeSettings
    {
        bool Enabled = Defaults::ReflectionProbeEnabled;
        // 視差補正(box projection)を行うか。Box形状のプローブにのみ効く。無効にすると
        // 反射ベクトルをそのまま引くPhase 1相当の挙動になり、壁際で反射位置がずれるのを確認できる
        bool ParallaxCorrectionEnabled = Defaults::ProbeParallaxCorrectionEnabled;
        // プローブ間・プローブとグローバルIBLの重み付きブレンドを行うか。無効にすると
        // 「影響範囲に入る最も近い1つだけを使う」Phase 1相当の挙動になり、境界の継ぎ目を確認できる
        bool BlendingEnabled = Defaults::ProbeBlendingEnabled;
        // 視差補正に距離キューブを使うか(19.12節)。無効にすると箱との交差だけで補正する
        // 従来の挙動になる。有効時も、箱との交点を探索範囲の上限として使う点は変わらない。
        //
        // 既定でfalseなのは、二重像が軽減される代わりにレイマーチの結果へ距離キューブの
        // テクセルの階段状のエッジが乗るためで、実機で見比べると「全体としては良くなった
        // とは言えない」ため。式としては正しく動いており(19.12節の検証参照)、
        // 距離キューブの解像度を上げるか2次モーメントを持って確率的に扱えば伸ばせる余地が
        // あるので、比較用のトグルとして残してある
        bool DepthParallaxEnabled = Defaults::ProbeDepthParallaxEnabled;
        // 距離キューブによる遮蔽判定で、プローブから見えない位置のピクセルの重みを落とすか
        // (光漏れの抑制)。無効にすると影響範囲に入っているだけで重みが立つ従来の挙動になる。
        //
        // 既定でfalseなのは、プローブが疎な現状では副作用のほうが大きいため(19.12節)。
        // 重みを落とした分はグローバルIBL(=空)が埋めるので、「プローブから見えない」だけの
        // 場所——例えば球の真下の床——が空の色で明るくなり、影のはずの位置に白いハローが出る。
        // 落ちた重みを別のプローブが引き取れる密度になって初めて素直に使える機能なので、
        // 効果と副作用を見比べられるトグルとして残し、既定は従来の挙動にしてある
        bool OcclusionEnabled = Defaults::ProbeOcclusionEnabled;
        // デバッグ表示(Render Targets)で確認するプローブ番号とプリフィルタのミップレベル
        int32_t DebugIndex = 0;
        int32_t PrefilterDebugMipLevel = 0;
        // 距離キューブのデバッグ表示で白になる距離(メートル相当)。距離は色ではないので
        // 表示輝度の倍率(1〜64倍)ではなくこちらで正規化する(Present.hlsl Mode 13へは
        // 逆数をGainとして渡す)
        float DistanceDebugRange = Defaults::ProbeDistanceDebugRange;

        ProbeUpdateMode UpdateMode = ProbeUpdateMode::Baked;
    };
}
