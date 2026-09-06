#pragma once

#include <cstddef>
#include <cstdint>

#include <DirectXMath.h>

// DDGI のシェーダーが読む cbuffer の C++ 側の写し(段階6)。
//
// 【なぜ独立したヘッダーなのか】パスの登録側(Passes/DDGIPasses.cpp)と、定数バッファを
// 作る側(KurenaiEngine3D.cpp の CreateSceneResources)の**両方**が sizeof で使う。
//
// 【static_assert が守るのはC++側だけ】HLSL の宣言と突き合わせているわけではない。
// ここが落ちたら「HLSL側も同じだけ動かせ」という合図として使う。
// **通すために期待値を書き換えないこと。**
namespace Kurenai::Passes
{
        // DDGIのプローブ更新CS(DDGIProbeUpdate.hlsl)専用の定数バッファ。
        // 焼く側にしか要らない値(どのプローブを焼いているか・ヒステリシス・距離のクランプ上限)を持つ
        struct alignas(16) DDGIUpdateConstants
        {
            // x=いま焼いているプローブの通し番号、y=ヒステリシス、z=距離モーメントのクランプ上限、
            // w=キャプチャキューブの1面の解像度(レイの立体角の重み付けに使う)
            DirectX::XMFLOAT4 Params0;
            // x=イラディアンスの1辺のテクセル数(境界を含まない)、y=距離モーメントの1辺のテクセル数、
            // z=境界の幅、w=履歴を無視して上書きするフラグ(初回ベイク時に1。
            // ヒステリシスは「前の値」があって初めて意味を持つため、未初期化のアトラスと混ぜてはいけない)
            DirectX::XMFLOAT4 Params1;
            // xyz=アトラス上でのプローブ格子の並び(x=各軸のプローブ数)。アトラスの列数は
            // ProbeCounts.x * ProbeCounts.y、行数はProbeCounts.zになる。
            // w=このフレームの実効プリ露出。積分した放射輝度をこれで割ってから格納する
            // (理由はFrameConstants::DDGIParams4のコメント参照)
            DirectX::XMFLOAT4 Params2;
        };
        // 【HLSL側の宣言とレイアウトを揃えたまま保つための固定】cbuffer(と構造化バッファ)は
        // 宣言順でオフセットが決まるので、ここで並べ替え・挿入・型変更が起きると、
        // HLSL側を直さないかぎり黙って別の値を読むことになる。
        // **通すために期待値を書き換えないこと**(FrameConstants.h と同じ規約)。
        //
        // 【これが守るのはC++側だけ】HLSLの宣言と突き合わせているわけではない。
        // ここが落ちたら「HLSL側も同じだけ動かせ」という合図として使う
        static_assert(offsetof(DDGIUpdateConstants, Params0) == 0, "Params0 のレイアウトが変わっている");
        static_assert(offsetof(DDGIUpdateConstants, Params1) == 16, "Params1 のレイアウトが変わっている");
        static_assert(offsetof(DDGIUpdateConstants, Params2) == 32, "Params2 のレイアウトが変わっている");
        static_assert(sizeof(DDGIUpdateConstants) == 48, "DDGIUpdateConstants の総サイズが変わっている");

        // DDGIProbeTrace.hlsl側のcbuffer DDGITraceConstants(register b1)と一致させる必要がある
        struct alignas(16) DDGITraceConstants
        {
            // xyz=いま焼いているプローブのワールド座標、w=処理対象の面(D3Dのキューブ標準順)
            DirectX::XMFLOAT4 Params0;
            // x=キャプチャキューブの1面の解像度、y=エミッシブ強度(ラスタ経路のObjectConstantsへ
            // 掛かっているのと同じ倍率)、z=太陽の影レイを撃つか(0/1。対照実験用)、
            // w=プロキシとして起こされたマテリアルの自発光倍率(0で抑止)
            DirectX::XMFLOAT4 Params1;
            // x=このパスが舐めるライトの数。b0のFrameConstants.ActiveLightCountはメイン描画と
            // 共有していて差し替えられないため、ここで別に渡す(ドローンの灯を外すため)。yzw=未使用
            DirectX::XMFLOAT4 Params2;
        };
        // 【HLSL側の宣言とレイアウトを揃えたまま保つための固定】cbuffer(と構造化バッファ)は
        // 宣言順でオフセットが決まるので、ここで並べ替え・挿入・型変更が起きると、
        // HLSL側を直さないかぎり黙って別の値を読むことになる。
        // **通すために期待値を書き換えないこと**(FrameConstants.h と同じ規約)。
        //
        // 【これが守るのはC++側だけ】HLSLの宣言と突き合わせているわけではない。
        // ここが落ちたら「HLSL側も同じだけ動かせ」という合図として使う
        static_assert(offsetof(DDGITraceConstants, Params0) == 0, "Params0 のレイアウトが変わっている");
        static_assert(offsetof(DDGITraceConstants, Params1) == 16, "Params1 のレイアウトが変わっている");
        static_assert(offsetof(DDGITraceConstants, Params2) == 32, "Params2 のレイアウトが変わっている");
        static_assert(sizeof(DDGITraceConstants) == 48, "DDGITraceConstants の総サイズが変わっている");
}
