#pragma once

#include <cstdint>

#include "../EngineDefaults.h"

namespace Kurenai
{
    // --- 超解像(FSR1相当のEASU+RCAS。41.23節) ---
    //
    // 【m_RenderWidth/m_RenderHeightの意味は変えていない】ここで足したのは
    // 「出力解像度」という一段外側の概念だけで、上のm_RenderWidth/m_RenderHeightは
    // 従来どおり「G-Buffer以降すべての中間バッファの解像度」のままである。
    // 超解像が有効なとき、出力解像度を品質モードの倍率で割った値を
    // RequestRenderResolution()へ流し込む、という関係になっている。
    // こうしてあるのは、Render()の各所に散らばるm_RenderWidth/m_RenderHeightの参照を
    // 「レンダー解像度」と「出力解像度」へ仕分ける必要をなくすため。
    // 追加のパスはTonemapの後ろに2本足すだけで済んでいる
    enum class UpscaleQualityMode
    {
        UltraQuality, // 1.3倍
        Quality,      // 1.5倍
        Balanced,     // 1.7倍
        Performance,  // 2.0倍
    };

    // 近傍クリップの方式。TAA.hlsl側のclipModeと値を一致させること
    // (TonemapCurveと同じく、列挙の既定値はEngineDefaults.hではなくここへ直接書く)
    enum class TAAClipMode : int32_t
    {
        None = 0,     // クリップしない(切り分け測定用。ゴーストが激しく出るので常用しない)
        Variance = 1, // 近傍の平均±(標準偏差×ClipGamma)のみ
        Clamped = 2,  // 上記と近傍の実在min/maxとの積集合(最も狭く、最もゴーストに強い)
    };

    // トーンマッピングカーブ。Tonemap.hlsl側のCurveと値を一致させること
    enum class TonemapCurve
    {
        Reinhard, // c/(c+1)。比較用のリファレンスカーブ
        ACES,     // Narkowicz 2015のフィット近似
        AgX,      // Troy Sobotka の AgX(Filament/three.jsの実装形)
    };

    struct PostProcessSettings
    {
        // 品質モードの既定値。EngineDefaults.hは列挙を知らない(<cstdint>しか取り込まない)ため
        // ここに置くが、「メンバの初期化子とUIの『既定値に戻す』が同じ出所を見る」という
        // EngineDefaults.hの原則自体は守る
        static constexpr UpscaleQualityMode kDefaultUpscaleQualityMode = UpscaleQualityMode::Quality;

        bool UpscaleEnabled = Defaults::UpscaleEnabled;
        UpscaleQualityMode UpscaleQuality = kDefaultUpscaleQualityMode;
        // RCASのシャープネス(0〜1)。UIの見た目の値で、シェーダーへ渡す前に
        // ComputeRcasSharpnessScale()で参照実装のスケールへ変換する
        float UpscaleSharpness = Defaults::UpscaleSharpness;
        // 超解像が有効なときの出力解像度。無効なときは内部レンダー解像度そのものになる。
        // ウィンドウサイズには追従しない(追従させるとドラッグ中に何度も
        // レンダーターゲットを作り直すことになる。SystemPanelの「ウィンドウサイズに合わせる」参照)
        uint32_t UpscaleOutputWidth = Defaults::RenderWidth;
        uint32_t UpscaleOutputHeight = Defaults::RenderHeight;

        bool TAAEnabled = Defaults::TAAEnabled;
        // 今フレームの色を履歴へ混ぜる割合。小さいほど収束後は滑らかだが、
        // 遮蔽が変わったときの追従が遅くなる
        float TAABlendWeight = Defaults::TAABlendWeight;
        // ジッターの振れ幅の倍率(1.0でピクセル内いっぱい)。0にするとジッターが無くなり、
        // 時間方向のスーパーサンプリング効果だけが消える(再投影と蓄積は残る)
        float TAAJitterScale = Defaults::TAAJitterScale;
        // 蓄積によるボケを補うシャープネス。TAAの中ではなくTonemapパスで最終出力にのみ掛ける。
        // TAAの入力へ掛けるとアンシャープマスクが「ジッターで変動する高域」を増幅し、
        // ちらつきが実測で約53%増える(Architecture.html 23.7節)
        float TAASharpness = Defaults::TAASharpness;
        // 近傍クリップのボックス幅(近傍の標準偏差の何倍まで履歴を許容するか)。
        // 小さいほどゴーストに強いがちらつきが増え、大きいほどその逆になる。
        // これは「動いている画素」に適用される値で、静止した画素ではm_TAAAntiFlickerに応じて広がる
        float TAAClipGamma = Defaults::TAAClipGamma;
        // 静止している画素に限ってブレンド率を下げ、近傍クリップのボックスを実質無効まで広げる量。
        // 速度が0の画素では再投影誤差が原理的に起きないためクリップは害にしかならず、
        // 一方でちらつきはブレンド率とクリップの両方から出る。動いている画素の挙動は
        // 一切変えないため、ゴーストの出方はこの機能を切ったときと同じままになる。
        // 0で無効(この機能を入れる前の挙動に戻る)
        float TAAAntiFlicker = Defaults::TAAAntiFlicker;
        TAAClipMode TAAClip = TAAClipMode::Clamped;

        // 既定をAgXにしている理由: ACESは飽和した明るい色の色相がシフトする(赤がオレンジへ寄る)
        // ことが知られており、Bistro内観のように赤い壁が支配的なシーンでその欠点が最も出やすい。
        // AgXはハイライトが色相を保ったまま白へ脱色するため、この用途では素直な絵になる
        TonemapCurve Curve = TonemapCurve::AgX;
        // 黒の締め(ブラックポイント)。0で恒等。詳細はShaders/3D/Tonemap.hlslのコメント参照
        float TonemapBlackPoint = Defaults::TonemapBlackPoint;

        // 薄明視(mesopic vision)の適用量。0で無効、1で完全適用。
        //
        // 暗所では錐体が働かなくなり桿体だけの視覚に移る。桿体は1種類しか無いので色を
        // 判別できず、実際の月明かりの下では「形は見えるのに色がほとんど無い」見え方になる。
        // 露出を下げるだけでは「暗いが色鮮やかな夜」にしかならず、肉眼で見た夜と一致しない。
        // 桿体の分光感度が短波長寄り(507nm)であることから来るプルキンエ現象も同時に入る
        // (詳細はTonemap.hlsl の ApplyMesopicVision)。
        // 既定は無効。効果が強く画作りの好みが分かれるため、使うときに明示的に上げる
        float MesopicStrength = Defaults::MesopicStrength;

        // 出力8bit量子化の直前に加えるディザリング。実測(Bistro Interior)では走査線上に
        // 同一色が24px連続しており、これは中間バッファをHDR化しても変わらなかった。
        // つまり暗部のバンディングの主因は最終8bit量子化であり、ここでしか直せない。
        // 効果をA/B比較できるようトグルにしてある
        bool DitherEnabled = Defaults::DitherEnabled;

        bool AutoExposureEnabled = Defaults::AutoExposureEnabled;
        // 露出のクランプ範囲(EV100)。ヒストグラムのビン割りもこの範囲で行うため、
        // 実シーンの輝度がこの外に出ると端に張り付く
        // 下限-6は月夜の地表(反射率0.2の面で約0.016 cd/m^2 = EV100約-3)を余裕をもって含む値。
        // 星明かりだけの夜まで追うならさらに下げる必要があるが、実写の夜景もEV -3〜-5程度で
        // 撮るのが普通なので実用上はここで足りる。
        // 上限18は、正規化後の昼の空(約6400 cd/m^2 = EV100約15.6)に余裕を持たせた値
        float AutoExposureMinEV100 = Defaults::AutoExposureMinEV100;
        float AutoExposureMaxEV100 = Defaults::AutoExposureMaxEV100;
        // 明順応(暗→明)と暗順応(明→暗)の速度。人間の目は暗順応のほうが遅いため既定値も分けている
        float AutoExposureSpeedUp = Defaults::AutoExposureSpeedUp;
        float AutoExposureSpeedDown = Defaults::AutoExposureSpeedDown;
        // 加重平均から除外する下側/上側の累積割合。暗すぎる画素・明るすぎる画素に露出が
        // 引きずられるのを防ぐ
        float AutoExposureLowPercentile = Defaults::AutoExposureLowPercentile;
        float AutoExposureHighPercentile = Defaults::AutoExposureHighPercentile;
        // 測定結果に対してユーザーが意図的に足すオフセット(EV)
        float AutoExposureCompensation = Defaults::AutoExposureCompensation;
        // 暗いシーンをわざと暗いまま写すための補正量[EV]。0にすると「常に中庸なグレーへ
        // 合わせる」挙動になり、夜が昼と同じ明るさで出る。
        // **m_MesopicStrengthとセットで意味を持つ**点に注意。露出を下げるだけでは
        // 「暗いが色鮮やかな夜」にしかならず、肉眼で見た夜と一致しない。
        // 既定値の掃引は docs/ImplementationDetail.md 21.9.2。
        //
        // **m_AutoExposureKeyCeilingEVとセットで意味を持つ値**である点に注意。
        // 上のクランプが無いと測光値が構図で2〜3.5段振れるので、この値をいくつにしても
        // カメラの向きで夜の明るさが変わってしまう
        float AutoExposureNightRolloffEV = Defaults::AutoExposureNightRolloffEV;
        // 補正カーブの折れ点[EV100]。測定値がDark以下で補正量が最大、Bright以上で0、間は線形。
        // Darkの-2は満月の夜の地表(反射率0.2の面で約0.016 cd/m^2 = EV100約-3)のすぐ上、
        // Brightの10は曇天の屋外あたりで、日中は補正が掛からない値にしてある
        float AutoExposureNightRolloffDarkEV100 = Defaults::AutoExposureNightRolloffDarkEV100;
        float AutoExposureNightRolloffBrightEV100 = Defaults::AutoExposureNightRolloffBrightEV100;
        // 測光値がキー照度の基準EV(ComputeReferenceEV100。構図に依存しない)から
        // 何段上まで行くのを許すか[EV]。十分大きな値(16など)で無効になる。
        //
        // 【位置づけ】構図で露出が振れる問題そのものは、AutoExposure.hlslで
        // **空を測光から外した**ことで根本的に解決している(21.9.8節)。
        // こちらは残った病的なケースへの保険で、通常は発動しない:
        // 夜の街で明るい看板が画面の大半を占めるようなとき、明るい側に寄った測光範囲
        // (50〜95パーセンタイル)がその看板に支配され、街並みが黒く沈むのを防ぐ。
        //
        // **上側だけを止める**のは、屋内のように実際の輝度が屋外のキー照度よりずっと低い
        // シーンでは測光値が下へ振れるのが正しいため(両側を締めると屋内が真っ暗になる)。
        // 下側はm_AutoExposureMinEV100が絶対的な下限として効く。
        //
        // 既定の+2は「通常のシーンでは発動しないが、極端なケースは止まる」余裕を見た値。
        // 測光から空を外してある(AutoExposure.hlsl)ため、-1のような強い値にすると締めすぎになる
        float AutoExposureKeyCeilingEV = Defaults::AutoExposureKeyCeilingEV;

        bool BloomEnabled = Defaults::BloomEnabled;
        // 最終合成の混合比。エネルギー保存のため加算ではなくlerpで混ぜるので、
        // 物理的にレンズ散乱が持ち去る割合(数%)に相当する小さい値が既定になる
        float BloomStrength = Defaults::BloomStrength;
        // しきい値は既定で十分低くしてある(物理的にはブルームは全輝度に掛かるのが正しい)。
        // アート制御として上げられるようにだけしてある
        float BloomThreshold = Defaults::BloomThreshold;
        float BloomSoftKnee = Defaults::BloomSoftKnee;

        // 実在の写真露出値(EV100)。太陽・環境光・ポイント/スポットライトすべてに同じ値がかかる、
        // シーン全体で単一の露出設定(詳細はdocs/Architecture.html参照)
        float SceneExposureEV100 = Defaults::SceneExposureEV100;
        // 実効プリ露出の時間平滑化の速さ[1/秒]。段付きを防ぐために指数追従させる
        float EffectiveExposureAdaptSpeed = 2.0f;
    };
}
