#pragma once

#include <cstdint>

#include "../EngineDefaults.h"

namespace Kurenai
{
    // --- MegaLights: ポイント/スポットライトの直接光を専用パスで求める経路 ---
    // 求めた寄与をHDRのテクスチャへ書き、DirectLighting.hlslがt7でそれを読んで加算する
    // (有効なあいだ、あちらのライトループは回らない)。太陽はこの経路の対象外で、
    // 従来どおりb0とCSM/RTシャドウが担当する。
    //
    // Referenceは全灯を総当たりして1灯ごとに影レイを撃つ、遅いが真値を返す経路で、
    // すべての測定の物差しにするためにある(MegaLightsReference.hlsl冒頭を参照)。
    //
    // 【StochasticとQuadSharedは同じ問題への別の解き方】どちらも候補プールから
    // 確率的に灯を選ぶが、1画素の推定量を良くする手段が違う:
    //   Stochastic … リザーバを時間・空間で再利用する(ReSTIR DI)。厳密に不偏だが、
    //                 再利用のたびに可視性を確かめるレイと不偏化の分母のための補正レイが要り、
    //                 **全灯総当たりの参照実装と同じコスト**になっていた
    //                 (実測は docs/ImplementationDetail.md 61.7j)
    //   QuadShared  … 2x2クアッドの4画素が撃った4本の結果を共有して平均する。
    //                 追加のレイは1本も撃たない(UE5 MegaLightsのDownsampleFactor=2に相当)。
    //                 影の縁が最大1画素ぼける偏りを受け入れる代わりにコストを切り下げる
    enum class MegaLightsMode
    {
        Off,        // 従来どおりDirectLighting.hlslのライトループで評価する
        Reference,  // 全灯総当たり+1灯1影レイ。ノイズは無いが遅い(グラウンドトゥルース)
        Stochastic, // 候補プールからRISで1灯選び、時間・空間再利用で磨く。厳密に不偏だがレイが多い
        QuadShared, // 1画素1レイのまま、2x2クアッドで可視性を共有して平均する
    };

    struct MegaLightsSettings
    {
        MegaLightsMode Mode = Defaults::MegaLightsEnabled ? MegaLightsMode::Reference
                                                          : MegaLightsMode::Off;
        // 1灯あたりに撃つ影レイの本数。**0にすると影を撃たず可視率1で評価する**。
        // その状態の出力は、スクリーンスペースシャドウを切った既存のライトループと
        // 数値的に一致するはずで、BRDF・減衰・スポット円錐・プリ露出をまとめて検証できる
        // (MegaLightsReference.hlslの「恒等テスト」)。punctualは方向が1つに決まるため、
        // 1より大きくしても答えは変わらない(光源に半径が入る段階で意味を持つ)
        int32_t ShadowRayCount = Defaults::MegaLightsShadowRayCount;

        bool DenoiseEnabled = Defaults::MegaLightsDenoiseEnabled;
        int32_t DenoiseAtrousPasses = Defaults::MegaLightsDenoiseAtrousPasses;
        int32_t DenoiseMaxFrames = Defaults::MegaLightsDenoiseMaxFrames;
        // クアッド共有(手法3)での時間累積の上限。**手法ごとに別に持つ。**
        // 1つの変数を共有して手法ごとに黙って読み替えると、UIのつまみが示す値と
        // 実際に効いている値が食い違う(「指定したのに効かない」の型)。
        // 分けておけば、UIもCLIも「いま効いている値」をそのまま触れる。
        // 手法3にリザーバの履歴が無いぶんここを長くしている(根拠は EngineDefaults.h)
        int32_t QuadDenoiseMaxFrames = Defaults::MegaLightsQuadDenoiseMaxFrames;
        float DenoiseSigmaLuminance = Defaults::MegaLightsDenoiseSigmaLuminance;
        float DenoiseFireflyClamp = Defaults::MegaLightsDenoiseFireflyClamp;
        bool DenoiseHistoryCatmullRom = Defaults::MegaLightsDenoiseHistoryCatmullRom;
        bool DenoiseHistory4Tap = Defaults::MegaLightsDenoiseHistory4Tap;
        // 残差駆動のアンチラグ。変化した画素だけ時間累積の上限を短く落とす(根拠は EngineDefaults.h)
        bool DenoiseAntiLag = Defaults::MegaLightsDenoiseAntiLag;
        float DenoiseAntiLagT0 = Defaults::MegaLightsDenoiseAntiLagT0;
        float DenoiseAntiLagT1 = Defaults::MegaLightsDenoiseAntiLagT1;
        int32_t DenoiseAntiLagFastFrames = Defaults::MegaLightsDenoiseAntiLagFastFrames;

        bool TemporalEnabled = Defaults::MegaLightsTemporalEnabled;
        // 履歴のM(何個の候補から絞ったか)の上限。大きいほど収束は速いが、
        // 新しいサンプルが採用されにくくなり、灯を消しても明るさが残る(ゴースト)
        int32_t TemporalMClamp = Defaults::MegaLightsTemporalMClamp;
        // 【検証専用】蓄積開始時に加える摂動(0=なし / 1=全ライトを消す / 2=露出を+2段跳ばす)。
        // 静止した絵では測れない「追従」を測るための入口。SetMegaLightsPerturbのコメント参照
        int32_t PerturbMode = 0;

        bool SpatialEnabled = Defaults::MegaLightsSpatialEnabled;
        int32_t SpatialNeighborCount = Defaults::MegaLightsSpatialNeighborCount;
        int32_t SpatialRadius = Defaults::MegaLightsSpatialRadius;
        int32_t SpatialIterations = Defaults::MegaLightsSpatialIterations;
        // 結合を不偏化(Z)にするか。単純なconfidence重みは、近傍が自分と違う候補集合から
        // 引いている可能性を無視するため不偏にならない(実測で総和の相対差 -8.0%)。
        // **切り替えて長時間平均を比べられるようにしてある** ――
        // 差が出なければどちらかが実装されていない
        bool SpatialMIS = Defaults::MegaLightsSpatialMIS;
        // 初期サンプルの可視レイでリザーバを殺すか。殺すと影の縁に暗い側の系統誤差が残る
        // (Zが可視率まで判定できないため)。詳細は EngineDefaults.h のコメント
        bool InitialVisibility = Defaults::MegaLightsInitialVisibility;
        // 1ピクセルあたりに候補プールから引く数(RISのM)。影レイの本数はこれとは独立で常に1本
        int32_t SampleCount = Defaults::MegaLightsSampleCount;

        // --- クアッド共有(手法3) ---
        // 2x2クアッドの仲間が撃った影レイの結果を借りて平均するか。
        // **切れるようにしてあるのは陽性対照のため** ―― 切ると自分の標本だけを使う形になり、
        // 手法2から時間再利用と空間再利用を外した構成と画素単位で一致するはず。
        // 一致しなければ配線のバグで、共有の効果を測る前にそこを潰す
        bool QuadShareEnabled = Defaults::MegaLightsQuadShareEnabled;
        // クアッドの4画素へ候補スロットを分けて引かせるか(層化)。
        // プールのスロットは混合分布からの i.i.d. 抽出なので、スロットの選び方を変えても
        // **周辺分布は変わらず割り戻しの式はそのまま厳密**。クアッドで重複した灯を
        // 引く確率が下がるぶん、4標本の多様性が上がる
        bool QuadStratify = Defaults::MegaLightsQuadStratify;
        // 遮蔽が確定した灯のキャッシュ(BlockedLights)を手法3でも使うか。
        // 手法3は時間再利用パスを持たないが、キャッシュ自体は Initial が維持している。
        // **陽性対照では切る**(履歴に依存すると手法2との画素単位の一致が崩れる)
        bool BlockedCacheEnabled = Defaults::MegaLightsBlockedCacheEnabled;
        // 1画素あたりに引く標本(リザーバ)の数。**手法3だけが1より大きくできる。**
        // 手法2の時間・空間再利用は「1画素1リザーバ」を前提に添字を組み立てているため。
        // 影レイの本数はそのままこの数になる(標本ごとに1本撃つ)
        int32_t QuadSamplesPerPixel = Defaults::MegaLightsQuadSamplesPerPixel;
        // デノイザの予測棄却画素へ追加する標本数と対象モード。
        // 1=予測棄却、2=予測棄却または短い履歴、3=全画素(検算専用)。
        // 既定は無効。効き代の実測は docs 61.7q.1(棄却画素は最悪画素の 7%)。
        int32_t QuadBoostSamples = Defaults::MegaLightsQuadBoostSamples;
        int32_t QuadBoostMode = Defaults::MegaLightsQuadBoostMode;
        // 候補プールが1タイルあたりに抽出する灯の数(K)。
        // **1画素あたりの標本数では減らないノイズがここで決まる** ―― プールはタイルに1つで、
        // タイル内の全画素が同じK個から引くので、プールの引き方のばらつきはタイル内で
        // 共通のオフセットとして乗る(根拠は EngineDefaults.h)
        int32_t TilePoolCapacity = Defaults::MegaLightsTilePoolCapacity;
        // タイル格子を動かすと共通誤差が時間方向に別の画面位置へ移る。
        // boolではなくモードなのは、+1タイルの経路を保ったままオフセットだけ0にする対照実験を行うため
        int32_t TileJitterMode = Defaults::MegaLightsTileJitterEnabled ? 1 : 0;
        // 候補プールを自分のタイル固定で引くか、最も近い4タイルから確率的バイリニアで引くか。
        // 0=固定(従来)、1=2x2クアッドごとに1タイル、2=画素ごとに1タイル。
        // **モードなのは粒度を実測で決めたため** ―― 画素ごとのほうがばらけるが、
        // クアッド層化(QuadStratify)はクアッドの4画素が同じプールを引く前提なので、
        // 画素ごとに違うタイルを選ぶと層化がクアッドを跨いで壊れる。指標は両者で有意に
        // 違わなかったので、壊れないクアッドごとを既定にしてある。根拠は EngineDefaults.h
        int32_t TilePoolBilinearMode = Defaults::MegaLightsTilePoolBilinearMode;

        // 前フレームの可視灯リストを提案分布の第3成分として混ぜるか。
        // 【OFFのとき出力はビット同一】混合率0で候補プールの抽選は1bitも変わらない
        // (枝ごとに別の定数で種をハッシュしており、乱数の*列*ではないため)
        bool VisibleListEnabled = Defaults::MegaLightsVisibleListEnabled;
        // 1タイルあたりに覚える灯の数。上限は kMegaLightsVisibleListCapacityMax
        int32_t VisibleListCapacity = Defaults::MegaLightsVisibleListCapacity;
        // リスト枝へ回す割合 c。一様枝(0.25)は削らないので不偏性は c に依らない。
        // 上げるほど可視灯へ寄るが、リストが1フレーム古いぶん遅れが増える
        float VisibleListMix = Defaults::MegaLightsVisibleListMix;

        // 何フレーム足したら止めるか。0なら蓄積そのものを行わない。
        // **止めることに意味がある** ―― 止めれば表示が静止し、「ちょうどNサンプルの平均」を
        // 決定的に撮れる(1/√Nで誤差が下がるかを測るのに要る)
        int32_t AccumTargetFrames = 0;
    };
}
