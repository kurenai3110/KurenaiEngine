#pragma once

#include <cstddef>
#include <cstdint>

#include <DirectXMath.h>

// MegaLights の確率的サンプリング経路が register(b1) で共有する定数バッファ。
//
// 【HLSL側の宣言はここが唯一の出所】同じレイアウトを
// KurenaiEngine/Shaders/3D/ShaderInterop/MegaLightsStochasticConstants.hlsli が宣言する。
// 以前は 5 本の .hlsl が「先頭からの前方一致」として手で再宣言しており、宣言の深さは
// 3〜7 フィールドとばらついていた。FrameConstants とまったく同じ壊れ方
// (途中へ挿すと後ろを宣言している側が黙ってオフセットずれを起こす)をするので、
// 同じやり方で 1 本にした。経緯は docs/ImplementationHistory.md 83節

namespace Kurenai::ShaderInterop
{
    struct alignas(16) MegaLightsStochasticConstants
    {
        // x=出力幅, y=出力高, z=1ピクセルあたりの初期候補数M, w=影レイを撃つか(0で撃たない)
        DirectX::XMUINT4 Params0;
        // x=候補プールのタイル数X、
        // y=タイルの1辺のピクセル数, z=1タイルあたりの候補数K, w=フレーム番号
        DirectX::XMUINT4 Params1;
        // x=借りる近傍の数, y=探す半径(ピクセル),
        // z=空間再利用の結合方式(0=confidence重み, 1=不偏化のZ),
        // w=初期可視レイでリザーバを殺すか(Initialが読む)。
        DirectX::XMUINT4 Params2;
        // x=射影行列の(0,0)成分, y=同(1,1)成分(空間再利用のMIS用。
        // 「その灯が隣のタイルへ届くか」を判定するために隣のタイルの錐台を組み立て直す。
        // **候補プールが使ったのと同じ行列から取ること**。ずれると定義域がずれる)、
        // z=プリ露出の補正倍率(時間再利用用。今の露出 / 前フレームの露出)、
        // w=履歴のMの上限(同)
        DirectX::XMFLOAT4 Params3;
        // x=履歴が使えるか(時間再利用用。0なら履歴を読まない。Initialは
        //   遮蔽が確定した灯のキャッシュを信用してよいかの判定にも使う)、
        // y=空間再利用の反復番号(0起点。近傍の型板の種に混ぜて反復ごとに別の近傍を選ばせる)、
        // z=クアッド共有を行うか(手法3。Resolveが読む。0なら自分の標本だけを使う)、
        // w=クアッドで候補スロットを分けて引くか(手法3の層化。Initialが読む)
        DirectX::XMUINT4 Params4;
        // x=1画素あたりの標本数(リザーバの本数。Initialが書きResolveが読む)。
        // 手法3だけが1より大きくなる ―― 手法2の時間・空間再利用は
        // 「1画素1リザーバ」を前提に添字を組み立てているため。
        // yzw=未使用
        DirectX::XMUINT4 Params5;
        // xy=未使用。
        // z=候補プールのタイル数Y(Params1.x のY版。
        //   確率的バイリニア参照が隣タイルの添字を画面内へクランプするのに使う)、
        // w=候補プールの確率的バイリニア参照(0=自分のタイル固定、1=クアッドごと、
        //   2=画素ごと。Initialが読む)
        DirectX::XMUINT4 Params6;
        // x=asuint(可視灯リストを提案分布へ混ぜた割合 c。0で従来どおり。
        //   Initialが割り戻しに使う。**候補プール側 MegaLightsTilePoolConstants の
        //   VisibleListParams.z と必ず同じ値にすること** ―― 抽出した確率と
        //   割り戻す確率が食い違うと、絵は出たまま静かに偏る)、
        // y=Temporalの履歴深度のカメラ移動補正(0=従来 / 1=前フレームの期待ViewZ)、zw=未使用
        //
        // 【枠を1つ増やす代償を承知で足している】このcbufferは MegaLights の5本が
        // 共有しており、宣言を1つ増やすだけで5本すべてのDXILが変わる。機能を切っていても
        // 浮動小数の丸めが動いて出力がビット同一でなくなる(実測値は 61.7u)。
        // **それでも足したのは、既存の意味へ相乗りさせるほうが後から読む人には危険なため。**
        DirectX::XMUINT4 Params7;
    };

    // 【レイアウトを固定する本体】HLSL側は宣言順でオフセットが決まる。
    // 並べ替え・挿入・型変更が起きればここで落ちるので、MegaLightsStochasticConstants.hlsli を
    // 直し忘れたまま黙って別の値を読むことはない。
    // **通すために期待値を書き換えないこと**(FrameConstants.h と同じ規約)
    static_assert(offsetof(MegaLightsStochasticConstants, Params0) == 0, "MegaLightsStochasticConstants.hlsli の Params0 と位置が食い違っている");
    static_assert(offsetof(MegaLightsStochasticConstants, Params1) == 16, "MegaLightsStochasticConstants.hlsli の Params1 と位置が食い違っている");
    static_assert(offsetof(MegaLightsStochasticConstants, Params2) == 32, "MegaLightsStochasticConstants.hlsli の Params2 と位置が食い違っている");
    static_assert(offsetof(MegaLightsStochasticConstants, Params3) == 48, "MegaLightsStochasticConstants.hlsli の Params3 と位置が食い違っている");
    static_assert(offsetof(MegaLightsStochasticConstants, Params4) == 64, "MegaLightsStochasticConstants.hlsli の Params4 と位置が食い違っている");
    static_assert(offsetof(MegaLightsStochasticConstants, Params5) == 80, "MegaLightsStochasticConstants.hlsli の Params5 と位置が食い違っている");
    static_assert(offsetof(MegaLightsStochasticConstants, Params6) == 96, "MegaLightsStochasticConstants.hlsli の Params6 と位置が食い違っている");
    // Params7 は可視灯リストの混合率と履歴深度のカメラ移動補正を載せるために**意図して足した**。
    // 通すために期待値を書き換えたのではなく、追加したことの記録としてここを更新している
    static_assert(offsetof(MegaLightsStochasticConstants, Params7) == 112, "MegaLightsStochasticConstants.hlsli の Params7 と位置が食い違っている");
    static_assert(sizeof(MegaLightsStochasticConstants) == 128, "MegaLightsStochasticConstants の総サイズが変わっている");
}
