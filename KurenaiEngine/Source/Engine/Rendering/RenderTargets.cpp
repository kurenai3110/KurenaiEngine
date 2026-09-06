#include "Rendering/RenderTargets.h"

namespace Kurenai::Rendering
{
    void RenderTargets::CreateGBufferCore(
        RHI::IRHIDevice& device, uint32_t width, uint32_t height, RHI::Format emissiveFormat)
    {
        GBufferAlbedo = device.CreateRenderTexture(width, height, RHI::Format::R8G8B8A8_UNorm);
        GBufferNormal = device.CreateRenderTexture(width, height, RHI::Format::R16G16_Float);
        GBufferMaterial = device.CreateRenderTexture(width, height, RHI::Format::R8G8B8A8_UNorm);
        GBufferEmissive = device.CreateRenderTexture(width, height, emissiveFormat);
        // Reverse-Zのため近平面側(NDC z=1.0)ではなく遠平面側(NDC z=0.0)にクリアする
        GBufferDepth = device.CreateDepthTexture(width, height, 0.0f);
    }

    void RenderTargets::CreateGBufferVelocity(RHI::IRHIDevice& device, uint32_t width, uint32_t height)
    {
        // モーションベクター(速度バッファ)。G-Bufferの5枚目として、GBuffer.hlslが
        // 「この画素に映っているものが前フレームでは画面のどこにいたか」をUV単位の2Dベクトルで書く。
        // 2成分しか要らないのでR16G16_Float。1画素ぶんの移動量が1/解像度(1920幅なら約0.00052)と
        // 小さいため、絶対精度ではなく相対精度で効く浮動小数点フォーマットが適している
        GBufferVelocity = device.CreateRenderTexture(width, height, RHI::Format::R16G16_Float);
    }

    void RenderTargets::CreateGBufferBentNormal(RHI::IRHIDevice& device, uint32_t width, uint32_t height)
    {
        // bent normal(正規化しない可視方向の平均、ワールド空間)。G-Bufferの6枚目。
        // .rgb = bRaw、.a = 有効フラグ。
        //
        // R11G11B10_Floatにはできない ―― 符号なしのため負の成分が落ち、
        // 半球の半分の方向を表現できなくなる。1080pで約16MB増える(34章)
        GBufferBentNormal = device.CreateRenderTexture(width, height, RHI::Format::R16G16B16A16_Float);
    }
}
