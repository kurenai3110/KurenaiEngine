#pragma once

#include <cstddef>
#include <cstdint>

#include <DirectXMath.h>

namespace Kurenai::Core
{
    class RenderGraph;
}

namespace Kurenai::RHI
{
    class IRHICommandList;
}

namespace Kurenai::Rendering
{
    struct RenderFrameContext;
    struct RenderBlackboard;
}

// Present パス群(段階6)。
//
// 【この群が持つ責務】選択中の中間バッファを、アスペクト比を保ってバックバッファへ出す。
// デバッグ表示(Render Targets UI)の切り替えもここに含まれる ―― 表示するのは
// 各パス群が書いた出力そのものなので、切り替えの表はどうしても横断的になる。
//
// 【エンジンへの参照を持つ理由】段階6は「登録順を1つも変えない」ことを唯一の
// 決め手として進めており、その担保はパスマニフェストの完全一致である。
// 状態の引っ越しと登録位置の移動を同時にやると、マニフェストが食い違ったときに
// どちらが原因か分けられない。**まず登録コードだけを機械的に移し**、
// リソースの所有権は後から群へ移す。それまでの間、群はエンジンの private を
// m_Engine 越しに触る(KurenaiEngine3D が friend 宣言している)。
namespace Kurenai
{
    class KurenaiEngine3D;

    namespace Passes
    {
        // Present.hlsl側のModeと一致させる必要がある
        struct alignas(16) PresentConstants
        {
            int32_t Mode;
            float MipLevel; // Mode==6(Hi-Z)でSampleLevelに渡すミップレベル
            // Mode==10(シャドウマップ配列)ではカスケード番号、
            // Mode==12(反射プローブのキューブマップ配列)では表示するプローブ番号として使う
            float ArraySlice;
            // デバッグ表示の輝度倍率(m_DebugViewSettings.Gain)。色として表示するMode 0/3/4にだけ効く
            float Gain;
            // Mode==11(タイルライトカリングのヒートマップ)専用。
            // x=タイル数X, y=タイルの1辺のピクセル数, z=1タイルあたりの容量, w=ヒートマップの上限ライト数
            DirectX::XMFLOAT4 TileParams;
            // xy=レンダー解像度。zw=Mode 21の候補プール格子オフセット。
            // デバッグ表示も書き手と同じ格子を読まないとA/Bの比較結果が嘘になる
            DirectX::XMFLOAT4 TileRenderSize;
            // Mode==22(MegaLightsの蓄積平均)専用。x=これまでに足したフレーム数, yzw=未使用。
            // **末尾に足すこと** ―― cbufferは宣言順レイアウトなので、途中へ挿すと
            // Present.hlsl側の以降のフィールドがすべてずれる
            DirectX::XMFLOAT4 AccumParams;
        };
        // 【HLSL側の宣言とレイアウトを揃えたまま保つための固定】cbuffer(と構造化バッファ)は
        // 宣言順でオフセットが決まるので、ここで並べ替え・挿入・型変更が起きると、
        // HLSL側を直さないかぎり黙って別の値を読むことになる。
        // **通すために期待値を書き換えないこと**(FrameConstants.h と同じ規約)。
        //
        // 【これが守るのはC++側だけ】HLSLの宣言と突き合わせているわけではない。
        // ここが落ちたら「HLSL側も同じだけ動かせ」という合図として使う
        static_assert(offsetof(PresentConstants, Mode) == 0, "Mode のレイアウトが変わっている");
        static_assert(offsetof(PresentConstants, MipLevel) == 4, "MipLevel のレイアウトが変わっている");
        static_assert(offsetof(PresentConstants, ArraySlice) == 8, "ArraySlice のレイアウトが変わっている");
        static_assert(offsetof(PresentConstants, Gain) == 12, "Gain のレイアウトが変わっている");
        static_assert(offsetof(PresentConstants, TileParams) == 16, "TileParams のレイアウトが変わっている");
        static_assert(offsetof(PresentConstants, TileRenderSize) == 32, "TileRenderSize のレイアウトが変わっている");
        static_assert(offsetof(PresentConstants, AccumParams) == 48, "AccumParams のレイアウトが変わっている");
        static_assert(sizeof(PresentConstants) == 64, "PresentConstants の総サイズが変わっている");

        class PresentPass
        {
        public:
            explicit PresentPass(KurenaiEngine3D& engine) : m_Engine(engine) {}

            // 【引数の渡し方】frame はフレーム先頭で確定したスナップショット、
            // bb は登録の途中で確定していく出力。どちらも const& で受け、
            // ラムダへは**必要な値だけを値捕捉**する(構造体そのものを参照捕捉しない)
            void Register(
                Core::RenderGraph& graph,
                RHI::IRHICommandList* commandList,
                const Rendering::RenderFrameContext& frame,
                const Rendering::RenderBlackboard& bb);

        private:
            KurenaiEngine3D& m_Engine;
        };
    }
}
