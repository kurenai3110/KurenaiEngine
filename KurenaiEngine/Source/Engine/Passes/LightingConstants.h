#pragma once

#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include <DirectXMath.h>

// 直接光・AO/GI・RTシャドウ・RTAO のシェーダーが読む cbuffer の C++ 側の写しと、
// SSAOのカーネル生成(段階6)。
//
// 【なぜ独立したヘッダーなのか】パスの登録側(Passes/LightingPasses.cpp)と、定数バッファを
// 作る側(KurenaiEngine3D.cpp の CreateSceneResources)の**両方**が sizeof で使う。
//
// 【static_assert が守るのはC++側だけ】HLSL の宣言と突き合わせているわけではない。
// **通すために期待値を書き換えないこと。**
namespace Kurenai::Passes
{
        // SSAO.hlsl側のkSSAOKernelSizeMaxと一致させる必要がある。
        // 定数バッファに確保する数であって、実際に回す段数(m_Settings.AmbientOcclusion.SSAOKernelSize)ではない
        constexpr uint32_t kSSAOKernelSizeMax = 16;

        struct alignas(16) SSAOConstants
        {
            DirectX::XMFLOAT4 Samples[kSSAOKernelSizeMax]; // タンジェント空間の半球カーネル
            DirectX::XMFLOAT4 Params;                      // x: 半径, y: バイアス, z: 強さ(べき乗), w: 使うサンプル数
        };
        // 【HLSL側の宣言とレイアウトを揃えたまま保つための固定】cbuffer(と構造化バッファ)は
        // 宣言順でオフセットが決まるので、ここで並べ替え・挿入・型変更が起きると、
        // HLSL側を直さないかぎり黙って別の値を読むことになる。
        // **通すために期待値を書き換えないこと**(FrameConstants.h と同じ規約)。
        //
        // 【これが守るのはC++側だけ】HLSLの宣言と突き合わせているわけではない。
        // ここが落ちたら「HLSL側も同じだけ動かせ」という合図として使う
        static_assert(offsetof(SSAOConstants, Samples) == 0, "Samples のレイアウトが変わっている");
        static_assert(offsetof(SSAOConstants, Params) == 256, "Params のレイアウトが変わっている");
        static_assert(sizeof(SSAOConstants) == 272, "SSAOConstants の総サイズが変わっている");

        // SSIL_VisibilityBitmask.hlsl側のcbuffer SSILConstantsと一致させる必要がある
        struct alignas(16) SSILConstants
        {
            DirectX::XMFLOAT4 Params0; // x: 半径, y: 厚み(Thickness Heuristic), z: 間接光の強さ, w: AOのべき乗
            DirectX::XMUINT4 Params1;  // x: スライス数, y: スライスあたりのステップ数, z/w: 未使用
        };
        // 【HLSL側の宣言とレイアウトを揃えたまま保つための固定】cbuffer(と構造化バッファ)は
        // 宣言順でオフセットが決まるので、ここで並べ替え・挿入・型変更が起きると、
        // HLSL側を直さないかぎり黙って別の値を読むことになる。
        // **通すために期待値を書き換えないこと**(FrameConstants.h と同じ規約)。
        //
        // 【これが守るのはC++側だけ】HLSLの宣言と突き合わせているわけではない。
        // ここが落ちたら「HLSL側も同じだけ動かせ」という合図として使う
        static_assert(offsetof(SSILConstants, Params0) == 0, "Params0 のレイアウトが変わっている");
        static_assert(offsetof(SSILConstants, Params1) == 16, "Params1 のレイアウトが変わっている");
        static_assert(sizeof(SSILConstants) == 32, "SSILConstants の総サイズが変わっている");

        // RTShadow.hlsl側のcbuffer RTShadowConstantsと一致させる必要がある
        struct alignas(16) RTShadowConstants
        {
            // xy: 出力サイズ(ピクセル), z: 太陽の見かけの半径(ラジアン), w: 1ピクセルあたりのレイ本数
            DirectX::XMFLOAT4 Params0;
        };
        // 【HLSL側の宣言とレイアウトを揃えたまま保つための固定】cbuffer(と構造化バッファ)は
        // 宣言順でオフセットが決まるので、ここで並べ替え・挿入・型変更が起きると、
        // HLSL側を直さないかぎり黙って別の値を読むことになる。
        // **通すために期待値を書き換えないこと**(FrameConstants.h と同じ規約)。
        //
        // 【これが守るのはC++側だけ】HLSLの宣言と突き合わせているわけではない。
        // ここが落ちたら「HLSL側も同じだけ動かせ」という合図として使う
        static_assert(offsetof(RTShadowConstants, Params0) == 0, "Params0 のレイアウトが変わっている");
        static_assert(sizeof(RTShadowConstants) == 16, "RTShadowConstants の総サイズが変わっている");

        // RTAO.hlsl側のcbuffer RTAOConstantsと一致させる必要がある
        struct alignas(16) RTAOConstants
        {
            // xy: 出力サイズ(ピクセル), z: レイの最大距離, w: 遮蔽率のコントラスト(べき乗)
            DirectX::XMFLOAT4 Params0;
            // x: レイ本数, y: 間接光の強さ, z: バウンス面へ影レイを撃つか, w: 未使用
            DirectX::XMFLOAT4 Params1;
        };
        // 【HLSL側の宣言とレイアウトを揃えたまま保つための固定】cbuffer(と構造化バッファ)は
        // 宣言順でオフセットが決まるので、ここで並べ替え・挿入・型変更が起きると、
        // HLSL側を直さないかぎり黙って別の値を読むことになる。
        // **通すために期待値を書き換えないこと**(FrameConstants.h と同じ規約)。
        //
        // 【これが守るのはC++側だけ】HLSLの宣言と突き合わせているわけではない。
        // ここが落ちたら「HLSL側も同じだけ動かせ」という合図として使う
        static_assert(offsetof(RTAOConstants, Params0) == 0, "Params0 のレイアウトが変わっている");
        static_assert(offsetof(RTAOConstants, Params1) == 16, "Params1 のレイアウトが変わっている");
        static_assert(sizeof(RTAOConstants) == 32, "RTAOConstants の総サイズが変わっている");

        // DirectLighting.hlsl側のcbuffer LightingConstantsと一致させる必要がある。
        // b0はFrameConstantsが使っており定数バッファスロットは2本しか無いため、
        // 直接光パス固有のパラメータはすべてここへ足していく
        struct alignas(16) LightingConstants
        {
            // x=有効ライト数, y=ピクセルあたりに撃つスクリーンスペースシャドウのレイ数の上限,
            // z=太陽の影の手法(ShadowMode。2のときだけRTShadowTexture(t6)を読む),
            // w=MegaLightsの寄与を使うか(1なら t7 のテクスチャを読み、ライトループを回さない)
            DirectX::XMUINT4 LightCount;
            // スクリーンスペースシャドウ(ScreenSpaceShadow.hlsli)のパラメータ。
            // x=レイマーチのステップ数, y=最大レイ長(ワールド単位), z=遮蔽とみなす深度差の上限(thickness),
            // w=有効フラグ(0で無効)
            DirectX::XMFLOAT4 SSSParams0;
            // x=深度リニアライズ定数a, y=同b(viewZ = b / (depth - a))、
            // z=レイ始点の法線方向への押し出し量(View空間深度に比例させる係数)、w=画面端フェード幅(UV)
            DirectX::XMFLOAT4 SSSParams1;
            // タイルライトカリング(LightCulling.hlsl)のパラメータ。
            // x=タイル数X, y=タイルの1辺のピクセル数, z=1タイルあたりの容量, w=カリング有効フラグ
            DirectX::XMUINT4 TileParams;
        };
        // 【HLSL側の宣言とレイアウトを揃えたまま保つための固定】cbuffer(と構造化バッファ)は
        // 宣言順でオフセットが決まるので、ここで並べ替え・挿入・型変更が起きると、
        // HLSL側を直さないかぎり黙って別の値を読むことになる。
        // **通すために期待値を書き換えないこと**(FrameConstants.h と同じ規約)。
        //
        // 【これが守るのはC++側だけ】HLSLの宣言と突き合わせているわけではない。
        // ここが落ちたら「HLSL側も同じだけ動かせ」という合図として使う
        static_assert(offsetof(LightingConstants, LightCount) == 0, "LightCount のレイアウトが変わっている");
        static_assert(offsetof(LightingConstants, SSSParams0) == 16, "SSSParams0 のレイアウトが変わっている");
        static_assert(offsetof(LightingConstants, SSSParams1) == 32, "SSSParams1 のレイアウトが変わっている");
        static_assert(offsetof(LightingConstants, TileParams) == 48, "TileParams のレイアウトが変わっている");
        static_assert(sizeof(LightingConstants) == 64, "LightingConstants の総サイズが変わっている");

        // タンジェント空間(Z軸=法線方向)の半球状にランダムなカーネルサンプルを生成する。
        // John Chapmanのチュートリアルにならい、原点付近にサンプルが偏るようスケーリングして
        // 近距離のディテールを優先的に拾う
        inline std::vector<DirectX::XMFLOAT4> GenerateSSAOKernel(uint32_t kernelSize)
        {
            std::mt19937 rng(12345);
            std::uniform_real_distribution<float> dist(0.0f, 1.0f);

            std::vector<DirectX::XMFLOAT4> kernel;
            kernel.reserve(kernelSize);
            for (uint32_t i = 0; i < kernelSize; ++i)
            {
                DirectX::XMVECTOR sample = DirectX::XMVectorSet(
                    dist(rng) * 2.0f - 1.0f,
                    dist(rng) * 2.0f - 1.0f,
                    dist(rng),
                    0.0f);
                sample = DirectX::XMVector3Normalize(sample);
                sample = DirectX::XMVectorScale(sample, dist(rng));

                float scale = static_cast<float>(i) / static_cast<float>(kernelSize);
                scale = 0.1f + 0.9f * scale * scale;
                sample = DirectX::XMVectorScale(sample, scale);

                DirectX::XMFLOAT4 sampleF;
                DirectX::XMStoreFloat4(&sampleF, sample);
                sampleF.w = 0.0f;
                kernel.push_back(sampleF);
            }
            return kernel;
        }
}
