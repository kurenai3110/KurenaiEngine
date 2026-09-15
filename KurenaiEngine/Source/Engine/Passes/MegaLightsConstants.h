#pragma once

#include <cstddef>
#include <cstdint>

#include <DirectXMath.h>

// タイルライトカリングと MegaLights のシェーダーが読む cbuffer の C++ 側の写し(段階6)。
//
// 【なぜ独立したヘッダーなのか】パスの登録側(Passes/MegaLightsPasses.cpp)と、定数バッファを
// 作る側(KurenaiEngine3D.cpp の CreateSceneResources)の**両方**が sizeof で使う。
//
// 【確率的サンプリングの定数はここに無い】あちらは HLSL の .hlsli と1対1で対応させる
// 決まりなので ShaderInterop/MegaLightsStochasticConstants.h にある(段階4)。
//
// 【static_assert が守るのはC++側だけ】**通すために期待値を書き換えないこと。**
namespace Kurenai::Passes
{
        // 【実行時に振れる。ここは確保の上限】1タイルの抽出数Kは設定が持ち、シェーダへは
        // 定数バッファで渡している。確保は1タイルあたり(6 + 2K) uintで、Kはレイの本数を増やさず
        // 出力バッファの容量だけを増やす
        inline constexpr uint32_t kMegaLightsTilePoolCapacity = 512;
        // Kの下限。これを下回るとタイルに届く灯を代表できない
        inline constexpr int32_t kMegaLightsTilePoolMinCapacity = 8;
        // 1画素あたりの標本数の上限。リザーバ1本は16バイト x 画素数 x 標本数で、2560x1440では
        // 1標本あたり約59MB、上限16標本では約944MBになる。同サイズのリザーバは初期・空間再利用の
        // ping-pong・時間履歴の計5本を確保するため、実際の確保量はさらに大きい。
        // クアッド層化は4層なので、4を超えると層の割り当てが一巡して効きが鈍る
        inline constexpr int32_t kMegaLightsMaxSamplesPerPixel = 16;

        // タイルライトカリングのタイルサイズ(1辺のピクセル数)。
        // LightCulling.hlsl の kTileSize および numthreads と必ず一致させること
        inline constexpr uint32_t kLightTileSize = 16;
        // 1タイルが保持できるライト数の上限。LightCulling.hlsl の kMaxLightsPerTile および
        // DirectLighting.hlsl の同名の定数と必ず一致させること(バッファのストライドがこの値で決まる)。
        // HLSL側はgroupshared配列のサイズに使うためコンパイル時定数である必要があり、
        // C++からの受け渡しでは代用できないので、3箇所で同じ値を書く形になっている。
        // .cppの無名名前空間ではなくここに置いてあるのは、DebugViewPanelがヒートマップの
        // 上限としてこの値を使うため
        inline constexpr uint32_t kLightTileCapacity = 64;
        // 空間再利用の反復ごとの定数(中身は共有分と同じで、反復番号だけが違う)。
        // 【1本を使い回してはいけない】UpdateBuffer は同じフレームで2回書くと
        // 後の値が両方のパスに見えるため、反復の数だけバッファを分ける
        inline constexpr uint32_t kMegaLightsMaxSpatialIterations = 2u;
        // 何フレーム待ってから足し始めるか。小さなシーンの読み込みとリサイズが片付く目安
        inline constexpr uint32_t kMegaLightsAccumWarmup = 180;

        // MegaLightsTilePool.hlsl側のcbuffer MegaLightsTilePoolConstantsと並びを一致させること。
        // 先頭4つはLightCullingConstantsと同じ並びだが、TileParams.wの意味が違う
        // (あちらは1タイルの容量、こちらは抽出する候補数K)ので構造体は分けてある
        struct alignas(16) MegaLightsTilePoolConstants
        {
            DirectX::XMFLOAT4X4 View;
            // xy=候補プールのタイル数、z=有効ライト数, w=1タイルあたりの候補数K
            DirectX::XMUINT4 TileParams;
            // x=レンダー解像度の幅, y=同 高さ, zw=未使用
            DirectX::XMUINT4 RenderSize;
            // x=射影行列の(0,0)成分, y=同(1,1)成分、z=深度リニアライズ定数a, w=同b
            DirectX::XMFLOAT4 ProjParams;
            // x=フレーム番号(候補を毎フレーム引き直すための乱数の種)、yzw=未使用
            DirectX::XMUINT4 PoolParams;
            // 可視灯リスト(提案分布の第3成分)。
            // x=リストの容量(0なら機能そのものが無効)、y=前フレームのリストが使えるか、
            // z=asuint(リスト枝へ回す混合率 c)、w=未使用。
            // **z は MegaLightsStochasticConstants.Params7.x と必ず同じ値にすること**
            // (抽出した確率と割り戻す確率が食い違うと、絵は出たまま静かに偏る)
            DirectX::XMUINT4 VisibleListParams;
        };
        // 【HLSL側の宣言とレイアウトを揃えたまま保つための固定】cbuffer(と構造化バッファ)は
        // 宣言順でオフセットが決まるので、ここで並べ替え・挿入・型変更が起きると、
        // HLSL側を直さないかぎり黙って別の値を読むことになる。
        // **通すために期待値を書き換えないこと**(FrameConstants.h と同じ規約)。
        //
        // 【これが守るのはC++側だけ】HLSLの宣言と突き合わせているわけではない。
        // ここが落ちたら「HLSL側も同じだけ動かせ」という合図として使う
        static_assert(offsetof(MegaLightsTilePoolConstants, View) == 0, "View のレイアウトが変わっている");
        static_assert(offsetof(MegaLightsTilePoolConstants, TileParams) == 64, "TileParams のレイアウトが変わっている");
        static_assert(offsetof(MegaLightsTilePoolConstants, RenderSize) == 80, "RenderSize のレイアウトが変わっている");
        static_assert(offsetof(MegaLightsTilePoolConstants, ProjParams) == 96, "ProjParams のレイアウトが変わっている");
        static_assert(offsetof(MegaLightsTilePoolConstants, PoolParams) == 112, "PoolParams のレイアウトが変わっている");
        static_assert(offsetof(MegaLightsTilePoolConstants, VisibleListParams) == 128, "VisibleListParams のレイアウトが変わっている");
        static_assert(sizeof(MegaLightsTilePoolConstants) == 144, "MegaLightsTilePoolConstants の総サイズが変わっている");

        // MegaLightsVisibleLights.hlsl側のcbuffer MegaLightsVisibleListConstantsと並びを一致させること
        struct alignas(16) MegaLightsVisibleListConstants
        {
            // x=タイル数X, y=同Y, z=1タイルあたりのリスト容量, w=1画素あたりの標本数
            DirectX::XMUINT4 ListParams;
            // x=レンダー解像度の幅, y=同 高さ, zw=未使用
            DirectX::XMUINT4 ListSize;
        };
        static_assert(offsetof(MegaLightsVisibleListConstants, ListParams) == 0, "ListParams のレイアウトが変わっている");
        static_assert(offsetof(MegaLightsVisibleListConstants, ListSize) == 16, "ListSize のレイアウトが変わっている");
        static_assert(sizeof(MegaLightsVisibleListConstants) == 32, "MegaLightsVisibleListConstants の総サイズが変わっている");

        // 可視灯リストの1タイルぶんのヘッダ長と容量の上限。
        // **MegaLightsCommon.hlsli の kMegaLightsVisibleListHeader /
        //   kMegaLightsVisibleListCapacityMax と必ず一致させること**
        // (片方だけ直すと無関係な位置を候補として読む。絵は出るので気付けない)
        inline constexpr uint32_t kMegaLightsVisibleListHeader = 2u;
        inline constexpr uint32_t kMegaLightsVisibleListCapacityMax = 16u;

        // MegaLightsAccum.hlsl側のcbuffer MegaLightsAccumConstantsと一致させる必要がある
        struct alignas(16) MegaLightsAccumConstants
        {
            // x=出力幅, y=出力高, z=足す前に0で始めるか(1でリセット), w=未使用
            DirectX::XMUINT4 Params0;
        };
        // 【HLSL側の宣言とレイアウトを揃えたまま保つための固定】cbuffer(と構造化バッファ)は
        // 宣言順でオフセットが決まるので、ここで並べ替え・挿入・型変更が起きると、
        // HLSL側を直さないかぎり黙って別の値を読むことになる。
        // **通すために期待値を書き換えないこと**(FrameConstants.h と同じ規約)。
        //
        // 【これが守るのはC++側だけ】HLSLの宣言と突き合わせているわけではない。
        // ここが落ちたら「HLSL側も同じだけ動かせ」という合図として使う
        static_assert(offsetof(MegaLightsAccumConstants, Params0) == 0, "Params0 のレイアウトが変わっている");
        static_assert(sizeof(MegaLightsAccumConstants) == 16, "MegaLightsAccumConstants の総サイズが変わっている");

        // MegaLightsDenoise.hlsl側のcbuffer MegaLightsDenoiseConstantsと一致させること
        struct alignas(16) MegaLightsDenoiseConstants
        {
            // x=出力幅, y=出力高, z=履歴が使えるか, w=à-trousの段(0起点)
            DirectX::XMUINT4 Params0;
            // x=à-trousのステップ幅, y=時間累積の上限フレーム数,
            // z=輝度のエッジ停止の強さ, w=法線のエッジ停止の指数
            DirectX::XMFLOAT4 Params1;
            // x=深度のエッジ停止の強さ, y=ファイアフライのクランプ強さ(0で無効),
            // z=前フレームの幾何(履歴ガイド)が使えるか(0なら現フレームのG-Bufferで代用),
            // w=未使用
            //
            // 【この行はかつて「yzw=未使用」と嘘を書いていた】y と z は実際には使われており、
            // HLSL側の宣言だけが正しかった。コメントを契約として使うコードベースなので、
            // ここがずれていると次の改修が空き枠だと思って y や z を潰す
            DirectX::XMFLOAT4 Params2;
            // x=履歴の妥当性を何タップで判定するか(0=最近傍1タップ(従来) / 1=バイリニア2x2の4タップ),
            // yzw=未使用
            //
            // 【なぜ足したか】履歴の**色**はバイリニアで4タップ混ぜるのに、その4タップが
            // 妥当かどうかは最近傍1点でしか見ていなかった。帰結は2つとも実害で、
            // (1)1点だけがシルエットの向こう側だと履歴全体を棄却する(本当は妥当なのに捨てる)
            // (2)1点が通れば残り3タップが別の面でも 3/4 の重みで色が入る
            DirectX::XMFLOAT4 Params3;
        };
        // 【HLSL側の宣言とレイアウトを揃えたまま保つための固定】cbuffer(と構造化バッファ)は
        // 宣言順でオフセットが決まるので、ここで並べ替え・挿入・型変更が起きると、
        // HLSL側を直さないかぎり黙って別の値を読むことになる。
        // **通すために期待値を書き換えないこと**(FrameConstants.h と同じ規約)。
        //
        // 【これが守るのはC++側だけ】HLSLの宣言と突き合わせているわけではない。
        // ここが落ちたら「HLSL側も同じだけ動かせ」という合図として使う
        static_assert(offsetof(MegaLightsDenoiseConstants, Params0) == 0, "Params0 のレイアウトが変わっている");
        static_assert(offsetof(MegaLightsDenoiseConstants, Params1) == 16, "Params1 のレイアウトが変わっている");
        static_assert(offsetof(MegaLightsDenoiseConstants, Params2) == 32, "Params2 のレイアウトが変わっている");
        // Params3 は履歴の妥当性判定のタップ数を載せるために**意図して足した**。
        // 通すために期待値を書き換えたのではなく、動かしたことの記録としてここを更新している
        static_assert(offsetof(MegaLightsDenoiseConstants, Params3) == 48, "Params3 のレイアウトが変わっている");
        static_assert(sizeof(MegaLightsDenoiseConstants) == 64, "MegaLightsDenoiseConstants の総サイズが変わっている");

        // MegaLightsReference.hlsl側のcbuffer MegaLightsConstantsと一致させる必要がある
        struct alignas(16) MegaLightsConstants
        {
            // x: 出力幅, y: 出力高, z: 1灯あたりに撃つ影レイの本数(0なら影を撃たず可視率1。恒等テスト用),
            // w: 有効ライト数
            DirectX::XMUINT4 Params0;
            // x: フレーム番号。球光源のサンプル列を毎フレーム回すのに使う。
            // 【混ぜないと蓄積が効かない】固定すると毎フレーム同じ点を引き、
            // 何枚足しても可視率のばらつきが残る(MegaLightsReference.hlsl)
            // y: 発光三角形の枚数(0ならメッシュライト無効。段階1のプロキシがそのまま光る)
            // z/w: 未使用
            DirectX::XMUINT4 Params1;
            // x: シーン全体の自発光の強度倍率。三角形テーブルには焼けない(毎フレーム変わる)。
            // 【露出ではない】自発光は露出を通らない経路で、段階1のプロキシも露出抜き。
            // 露出を掛けるとEV100=15で1/39322倍になる。y/z/w: 未使用
            DirectX::XMFLOAT4 Params2;
        };
        // 【HLSL側の宣言とレイアウトを揃えたまま保つための固定】cbuffer(と構造化バッファ)は
        // 宣言順でオフセットが決まるので、ここで並べ替え・挿入・型変更が起きると、
        // HLSL側を直さないかぎり黙って別の値を読むことになる。
        // **通すために期待値を書き換えないこと**(FrameConstants.h と同じ規約)。
        //
        // 【これが守るのはC++側だけ】HLSLの宣言と突き合わせているわけではない。
        // ここが落ちたら「HLSL側も同じだけ動かせ」という合図として使う
        static_assert(offsetof(MegaLightsConstants, Params0) == 0, "Params0 のレイアウトが変わっている");
        static_assert(offsetof(MegaLightsConstants, Params1) == 16, "Params1 のレイアウトが変わっている");
        static_assert(offsetof(MegaLightsConstants, Params2) == 32, "Params2 のレイアウトが変わっている");
        static_assert(sizeof(MegaLightsConstants) == 48, "MegaLightsConstants の総サイズが変わっている");

        // LightCulling.hlsl側のcbuffer LightCullingConstantsと一致させる必要がある
        struct alignas(16) LightCullingConstants
        {
            DirectX::XMFLOAT4X4 View;
            // x=タイル数X, y=タイル数Y, z=有効ライト数, w=1タイルあたりの容量
            DirectX::XMUINT4 TileParams;
            // x=レンダー解像度の幅, y=同 高さ, zw=未使用
            DirectX::XMUINT4 RenderSize;
            // x=射影行列の(0,0)成分, y=同(1,1)成分, z=深度リニアライズ定数a, w=同b
            DirectX::XMFLOAT4 ProjParams;
        };
        // 【HLSL側の宣言とレイアウトを揃えたまま保つための固定】cbuffer(と構造化バッファ)は
        // 宣言順でオフセットが決まるので、ここで並べ替え・挿入・型変更が起きると、
        // HLSL側を直さないかぎり黙って別の値を読むことになる。
        // **通すために期待値を書き換えないこと**(FrameConstants.h と同じ規約)。
        //
        // 【これが守るのはC++側だけ】HLSLの宣言と突き合わせているわけではない。
        // ここが落ちたら「HLSL側も同じだけ動かせ」という合図として使う
        static_assert(offsetof(LightCullingConstants, View) == 0, "View のレイアウトが変わっている");
        static_assert(offsetof(LightCullingConstants, TileParams) == 64, "TileParams のレイアウトが変わっている");
        static_assert(offsetof(LightCullingConstants, RenderSize) == 80, "RenderSize のレイアウトが変わっている");
        static_assert(offsetof(LightCullingConstants, ProjParams) == 96, "ProjParams のレイアウトが変わっている");
        static_assert(sizeof(LightCullingConstants) == 112, "LightCullingConstants の総サイズが変わっている");
}
