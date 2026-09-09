#pragma once

#include <cstddef>
#include <cstdint>

#include <DirectXMath.h>

// 反射のシェーダーが読む cbuffer の C++ 側の写し(段階6)。
//
// 【なぜ独立したヘッダーなのか】パスの登録側(Passes/ReflectionPasses.cpp)と、定数バッファを
// 作る側(KurenaiEngine3D.cpp の CreateSceneResources)の**両方**が sizeof で使う。
//
// 【static_assert が守るのはC++側だけ】HLSL の宣言と突き合わせているわけではない。
// ここが落ちたら「HLSL側も同じだけ動かせ」という合図として使う。
// **通すために期待値を書き換えないこと。**
namespace Kurenai::Passes
{
        // SSR.hlsl側のcbuffer SSRConstantsと一致させる必要がある
        struct alignas(16) SSRConstants
        {
            // w: 水面の解析空フォールバックを使うか(1=使う)。Render()側で
            // m_Settings.Water.AnalyticSkyReflection && usingProceduralSky の両方が立っているときだけ1にする
            // (手続き空が無効なシーンではDDSは任意の絵でPerezモデルとは無関係なため、
            // このトグルの値に関わらず必ず0にする)
            DirectX::XMFLOAT4 Params0; // x: 最大レイ距離, y: ヒット判定の厚み, z: ラフネスカットオフ, w: 水面の解析空フォールバック
            // 平面反射(末尾に追加)。x: 平面反射が有効か(1=使う。m_Settings.Reflection.PlanarEnabled &&
            // 水面インスタンスが存在するときのみ1)、y: 波の法線による画面UVのずらし量
            // (m_Settings.Reflection.PlanarDistortion)、zw: 未使用
            DirectX::XMFLOAT4 Params1;
        };
        // 【HLSL側の宣言とレイアウトを揃えたまま保つための固定】cbuffer(と構造化バッファ)は
        // 宣言順でオフセットが決まるので、ここで並べ替え・挿入・型変更が起きると、
        // HLSL側を直さないかぎり黙って別の値を読むことになる。
        // **通すために期待値を書き換えないこと**(FrameConstants.h と同じ規約)。
        //
        // 【これが守るのはC++側だけ】HLSLの宣言と突き合わせているわけではない。
        // ここが落ちたら「HLSL側も同じだけ動かせ」という合図として使う
        static_assert(offsetof(SSRConstants, Params0) == 0, "Params0 のレイアウトが変わっている");
        static_assert(offsetof(SSRConstants, Params1) == 16, "Params1 のレイアウトが変わっている");
        static_assert(sizeof(SSRConstants) == 32, "SSRConstants の総サイズが変わっている");

        // RTReflection.hlsl側のcbuffer RTReflectionConstantsと一致させる必要がある
        struct alignas(16) RTReflectionConstants
        {
            DirectX::XMFLOAT4 Params0; // xy: 出力サイズ(ピクセル), z: 最大レイ距離, w: ラフネスカットオフ
            // x: 影レイを撃つか(1で撃つ)
            // y: メッシュレットのデバッグ表示(1で、反射に映る面をメッシュレット色で塗る)
            // zw: 未使用
            DirectX::XMFLOAT4 Params1;
        };
        // 【HLSL側の宣言とレイアウトを揃えたまま保つための固定】cbuffer(と構造化バッファ)は
        // 宣言順でオフセットが決まるので、ここで並べ替え・挿入・型変更が起きると、
        // HLSL側を直さないかぎり黙って別の値を読むことになる。
        // **通すために期待値を書き換えないこと**(FrameConstants.h と同じ規約)。
        //
        // 【これが守るのはC++側だけ】HLSLの宣言と突き合わせているわけではない。
        // ここが落ちたら「HLSL側も同じだけ動かせ」という合図として使う
        static_assert(offsetof(RTReflectionConstants, Params0) == 0, "Params0 のレイアウトが変わっている");
        static_assert(offsetof(RTReflectionConstants, Params1) == 16, "Params1 のレイアウトが変わっている");
        static_assert(sizeof(RTReflectionConstants) == 32, "RTReflectionConstants の総サイズが変わっている");
}
