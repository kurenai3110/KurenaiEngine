#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <DirectXMath.h>

#include "RHI/IRHIDevice.h"

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
// 【エンジンへの参照を持つ理由】この群のリソース(PSO・シェーダー・定数バッファ)は
// 下の private が持っており、エンジンの private はもう触らない(friend は外れている)。
// 残る m_Engine は、デバッグ名の焼き付けとテクスチャダンプという**エンジンが持ち主の
// 診断機能**を呼ぶためだけにある。どちらもエンジン全体のテクスチャ表を対象にするので
// この群へは降ろせず、public メソッドとして呼ぶ。
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
            // デバッグ表示の輝度倍率(m_Settings.DebugView.Gain)。色として表示するMode 0/3/4にだけ効く
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

            // 【エンジン側の元の行位置から呼ぶこと】DX12はディスクリプタ枠を生成順に
            // 割り当てるため、生成の呼び出しを寄せ集めると他のリソースとの前後関係が崩れ、
            // パスマニフェストの採取が一斉に不一致になる。所有権だけをこの群へ移し、
            // 呼び出しは CreateSceneResources() の元あった場所に残してある
            void CreatePipelineState(RHI::IRHIDevice& device, const std::wstring& shaderDirectory);
            void CreateConstantBuffer(RHI::IRHIDevice& device);

        private:
            KurenaiEngine3D& m_Engine;

            // Presentパス(頂点バッファなしのフルスクリーン三角形。
            // 選択中のレンダーターゲットをアスペクト比を保ってバックバッファへ拡大縮小表示)
            std::unique_ptr<RHI::IRHIShader> m_PresentVertexShader;
            std::unique_ptr<RHI::IRHIShader> m_PresentPixelShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_PresentPipelineState;
            std::unique_ptr<RHI::IRHIBuffer> m_PresentConstantBuffer;
        };
    }
}
