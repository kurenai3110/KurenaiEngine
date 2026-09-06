#pragma once

#include <cstddef>
#include <cstdint>

#include <algorithm>
#include <cmath>

#include <DirectXMath.h>

// GPUへ送るライト1灯ぶんのレコード(段階6)。
//
// 【共有ヘッダーにする理由】ライトの配列を組み立てるのは Render()、読むのは
// 直接光・半透明・プローブのキャプチャと広く散っている。フレームのスナップショットが
// この配列を指すため、型が KurenaiEngine3D.cpp の無名名前空間にあると参照できない。
//
// 【static_assert が守るのはC++側だけ】HLSL の宣言と突き合わせているわけではない。
// **通すために期待値を書き換えないこと。**
namespace Kurenai
{
        // GPULight.Params.y のビット。**Shaders/3D/LightAttenuation.hlsli と一致させること**
        constexpr uint32_t kLightShadowScreenSpace = 1u;
        constexpr uint32_t kLightShadowRaytraced = 2u;

        // DirectLighting.hlsl側のstruct GPULightと並び・ストライド(64バイト)を一致させる必要がある
        struct alignas(16) GPULight
        {
            DirectX::XMFLOAT4 PositionType;   // xyz=ワールド座標, w=LightType
            DirectX::XMFLOAT4 ColorRange;     // rgb=露出済み放射輝度, w=Range
            DirectX::XMFLOAT4 DirectionAngle; // xyz=向き(正規化済み), w=spotAngleScale
            // x=spotAngleOffset
            // y=影のフラグ(bit0=画面空間シャドウ / bit1=レイトレース影レイ。kLightShadow* を使う)
            // z=SourceRadius(球光源の半径 / エミッシブ光源プロキシでは面積等価の円板半径)
            // w=指向性κ(エミッシブ光源プロキシのみ。それ以外は0)
            DirectX::XMFLOAT4 Params;
        };
        // 【HLSL側の宣言とレイアウトを揃えたまま保つための固定】cbuffer(と構造化バッファ)は
        // 宣言順でオフセットが決まるので、ここで並べ替え・挿入・型変更が起きると、
        // HLSL側を直さないかぎり黙って別の値を読むことになる。
        // **通すために期待値を書き換えないこと**(FrameConstants.h と同じ規約)。
        //
        // 【これが守るのはC++側だけ】HLSLの宣言と突き合わせているわけではない。
        // ここが落ちたら「HLSL側も同じだけ動かせ」という合図として使う
        static_assert(offsetof(GPULight, PositionType) == 0, "PositionType のレイアウトが変わっている");
        static_assert(offsetof(GPULight, ColorRange) == 16, "ColorRange のレイアウトが変わっている");
        static_assert(offsetof(GPULight, DirectionAngle) == 32, "DirectionAngle のレイアウトが変わっている");
        static_assert(offsetof(GPULight, Params) == 48, "Params のレイアウトが変わっている");
        static_assert(sizeof(GPULight) == 64, "GPULightはDirectLighting.hlsl側と64バイトで一致させる必要がある");

        // エミッシブなメッシュから起こした光源プロキシを GPULight へ変換する。
        //
        // 【なぜ MakeGPULight と別関数なのか。そして exposure を引数に取らないのか】
        // ライトとエミッシブは単位系が違う。ライトの Intensity はカンデラで、
        // MakeGPULight が ComputeExposure(EV100) を掛けて表示空間へ持ち込む。
        // 一方エミッシブは GBuffer.hlsl が EmissiveFactor をそのまま G-Buffer へ書き、
        // DeferredLighting が**露出を通さずに**加算する(EV100 のツールチップ自身が
        // 「太陽・環境光・ポイント/スポットライト」にしか掛からないと書いている)。
        //
        // したがって面の測光輝度は L_v = E / exposure で、面積 A の放射強度は I = L_v * A。
        // これを GPULight へ入れるときに exposure を掛け直すと**約束どおり相殺して消える**:
        //
        //     ColorRange.rgb = I * exposure = (E / exposure) * A * exposure = E * A
        //
        // 露出が式から消えるので、自動露出が動いても TAA のプリ露出補正が入っても
        // プロキシと発光面の見た目の対応が崩れない。
        //
        // **MakeGPULight へ「Intensity = E*A/exposure」を渡す形にはしない。** 相殺に依存した
        // 割り算が2箇所へ散り、片方だけ直したときにコンパイルも通り絵も「それらしく」出る。
        // ここが exposure を受け取らないこと自体が、その事故を構造的に防いでいる。
        //
        // 【指向の分配はシェーダ側が持つ】I(θ) = I * [(1-κ)/4 + κ*max(0,cosθ)] の括弧の中は
        // LightAttenuation.hlsli の型3の枝にある。ここは向きによらない強さだけを入れる
        inline GPULight MakeGPULightFromEmissiveProxy(
            const Assets::EmissiveProxy& proxy, float emissiveIntensity, float cutoffIrradiance,
            float maxRange)
        {
            GPULight gpuLight{};

            const float intensity[3] = {
                proxy.RadianceBase[0] * emissiveIntensity * proxy.Area,
                proxy.RadianceBase[1] * emissiveIntensity * proxy.Area,
                proxy.RadianceBase[2] * emissiveIntensity * proxy.Area,
            };

            // Range は「最も強い向きでも打ち切り照度τまで落ちる距離」から解く。
            // 【上界の余弦ローブを使う】タイルカリング(LightAttenuationUpperBound)が同じ
            // 上界で判定するので、定義域を一致させないと届く灯を取りこぼす
            const float peak = std::max({ intensity[0], intensity[1], intensity[2] });
            const float lobeMax = (1.0f - proxy.Directionality) * 0.25f + proxy.Directionality;
            const float radiusSq = proxy.SourceRadius * proxy.SourceRadius;
            const float solved = peak * lobeMax / std::max(cutoffIrradiance, 1e-9f) - radiusSq;
            float range = (solved > 0.0f) ? std::sqrt(solved) : 0.0f;
            // 下限は 2R。プロキシが自分の発光体の広がりすら覆わないと、
            // 器具の筐体が真っ暗なまま光っている見た目になる
            range = std::max(range, 2.0f * proxy.SourceRadius);
            // 上限。自発光の強度を上げたときに Range が数kmまで伸びて、
            // タイルカリングが全タイルにヒットするのを止める安全弁
            if (maxRange > 0.0f)
            {
                range = std::min(range, maxRange);
            }

            gpuLight.PositionType = {
                proxy.Position[0], proxy.Position[1], proxy.Position[2],
                static_cast<float>(Assets::LightType::EmissiveProxy)
            };
            gpuLight.ColorRange = { intensity[0], intensity[1], intensity[2], range };
            // w(spotAngleScale)は使わない。xyz は発光面の平均法線で、余弦ローブの軸になる
            gpuLight.DirectionAngle = { proxy.Direction[0], proxy.Direction[1], proxy.Direction[2], 0.0f };
            // x=spotAngleOffset(未使用) / y=影のフラグ / z=面積等価の円板半径 / w=指向性κ
            //
            // 【レイトレース影レイだけを立てる】スクリーンスペースシャドウは画素あたりの
            // レイ数に上限(既定4灯)があり、プロキシは数百灯になりうる。両方立てると
            // プロキシが予算を食い尽くし、手置きライトの接触影が消える
            gpuLight.Params = {
                0.0f, static_cast<float>(kLightShadowRaytraced), proxy.SourceRadius, proxy.Directionality
            };
            return gpuLight;
        }
}
