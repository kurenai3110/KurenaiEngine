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

    void RenderTargets::CreateLightingChain(
        RHI::IRHIDevice& device, uint32_t width, uint32_t height, RHI::Format aoFormat)
    {
        DirectLightTexture = device.CreateRenderTexture(width, height, RHI::Format::R32G32B32A32_Float);
        SSAORawTexture = device.CreateRenderTexture(width, height, aoFormat);
        SSAOTexture = device.CreateRenderTexture(width, height, aoFormat);
        SSILRawTexture = device.CreateRenderTexture(width, height, aoFormat);
        SSILTexture = device.CreateRenderTexture(width, height, aoFormat);
        SceneColor = device.CreateRenderTexture(width, height, RHI::Format::R16G16B16A16_Float);
        SSRTexture = device.CreateRenderTexture(width, height, RHI::Format::R16G16B16A16_Float);
    }

    void RenderTargets::CreateTonemap(RHI::IRHIDevice& device, uint32_t width, uint32_t height)
    {
        TonemapTexture = device.CreateRenderTexture(width, height, RHI::Format::R8G8B8A8_UNorm);
    }

    void RenderTargets::CreateRTShadow(RHI::IRHIDevice& device, uint32_t width, uint32_t height)
    {
        RTShadowTexture = device.CreateUAVTexture(width, height, RHI::Format::R32_Float);
    }

    void RenderTargets::CreateTAAHistory(RHI::IRHIDevice& device, uint32_t width, uint32_t height)
    {
        // 読みながら同じテクスチャへ書けないため、毎フレーム役割を入れ替える履歴バッファ2枚。
        // Legacy8bitに落としてもSceneColorと同じくfp16を保つ。何十フレームぶんもの蓄積で
        // 量子化誤差が積み上がり、8bitではバンディングになるため。
        TAAHistory[0] = device.CreateRenderTexture(width, height, RHI::Format::R16G16B16A16_Float);
        TAAHistory[1] = device.CreateRenderTexture(width, height, RHI::Format::R16G16B16A16_Float);
    }

    void RenderTargets::CreateHiZ(RHI::IRHIDevice& device, uint32_t width, uint32_t height, uint32_t mipLevels)
    {
        HiZTexture = device.CreateHiZTexture(width, height, mipLevels);
    }

    void RenderTargets::CreateShadowCascadeArray(RHI::IRHIDevice& device, uint32_t size, uint32_t cascadeCount)
    {
        ShadowCascadeArray = device.CreateDepthTextureArray(size, size, cascadeCount);
    }
}
