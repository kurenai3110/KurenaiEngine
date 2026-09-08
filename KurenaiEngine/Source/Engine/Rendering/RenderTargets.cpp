#include "Rendering/RenderTargets.h"

#include <algorithm>

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

    void RenderTargets::CreateRTReflection(RHI::IRHIDevice& device, uint32_t width, uint32_t height)
    {
        RTReflectionTexture = device.CreateUAVTexture(width, height, RHI::Format::R16G16B16A16_Float);
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
        // 単チャンネル(R32_Float)のフルミップチェーン。ミップごとのUAVへ
        // 「ミップNを読んでN+1へ2x2ブロックの最小値(Reverse-Zで最も遠い深度)を書く」
        // ダウンサンプルを1段ずつ繰り返す
        HiZTexture = device.CreateMippedUAVTexture(width, height, RHI::Format::R32_Float, mipLevels);
    }

    void RenderTargets::CreateShadowCascadeArray(RHI::IRHIDevice& device, uint32_t size, uint32_t cascadeCount)
    {
        ShadowCascadeArray = device.CreateDepthTextureArray(size, size, cascadeCount);
    }

    void RenderTargets::CreateSoftwareRasterOutputs(RHI::IRHIDevice& device, uint32_t width, uint32_t height)
    {
        // 【フォーマットはハードウェア側と揃える】色はHDR(Present Mode 4)、
        // 深度は生値(Mode 5)、法線はGBufferNormalと同じR16G16_Floatの
        // オクタヘドラル符号化(Mode 7)。揃えていないと差分が取れない
        SoftwareRasterColor = device.CreateUAVTexture(width, height, RHI::Format::R16G16B16A16_Float);
        SoftwareRasterDepth = device.CreateUAVTexture(width, height, RHI::Format::R32_Float);
        SoftwareRasterNormal = device.CreateUAVTexture(width, height, RHI::Format::R16G16_Float);
    }

    void RenderTargets::CreatePlanarReflection(RHI::IRHIDevice& device, uint32_t width, uint32_t height)
    {
        // SceneColorと同じHDR形式(R16G16B16A16_Float)。水面はラフネスが低く反射がそのまま
        // 見えるため、CreateRenderTargetsのLegacy8bitフォールバックの対象外にして常にHDR固定にする
        PlanarReflectionColor = device.CreateRenderTexture(width, height, RHI::Format::R16G16B16A16_Float);
        // Reverse-Zのため遠平面側(NDC z=0.0)にクリアする(G-Buffer/ProbeCapture深度と同じ)
        PlanarReflectionDepth = device.CreateDepthTexture(width, height, 0.0f);

        // 【2枚とも作れてから記録する】上で送出したら実寸は前の値のまま残る。
        // デバッグ表示のレターボックス計算が、存在しない解像度を使わないようにするため
        PlanarReflectionWidth = width;
        PlanarReflectionHeight = height;
    }

    void RenderTargets::CreateBloomPyramid(
        RHI::IRHIDevice& device, uint32_t width, uint32_t height, uint32_t levelCount)
    {
        BloomLevelSizes.clear();
        BloomDownTextures.clear();
        BloomUpTextures.clear();
        uint32_t bloomWidth = std::max(1u, width / 2);
        uint32_t bloomHeight = std::max(1u, height / 2);
        for (uint32_t level = 0; level < levelCount; ++level)
        {
            BloomLevelSizes.push_back({ bloomWidth, bloomHeight });
            // アルファを使わないHDRバッファなのでR11G11B10_Floatで足りる。
            // Legacy8bit構成でもブルームはHDR値を扱う必要があるためここは常にHDRのままにする
            // (8bitにすると1.0でクリップされ、ブルームの意味が失われる)
            BloomDownTextures.push_back(
                device.CreateUAVTexture(bloomWidth, bloomHeight, RHI::Format::R16G16B16A16_Float));
            BloomUpTextures.push_back(
                device.CreateUAVTexture(bloomWidth, bloomHeight, RHI::Format::R16G16B16A16_Float));

            bloomWidth = std::max(1u, bloomWidth / 2);
            bloomHeight = std::max(1u, bloomHeight / 2);
        }
    }

    void RenderTargets::CreateUpscale(RHI::IRHIDevice& device, uint32_t width, uint32_t height)
    {
        // Tonemapの出力と同じR8G8B8A8_UNorm。EASU/RCASはどちらも表示レンジの値を前提にしており、
        // ここをHDRフォーマットにしても情報は増えない(入力が既にLDRのため)。
        //
        // 【型付きUAVのフォーマット制約には当たらない】このエンジンが各所で注記している
        // 「R32系しか保証されていない」という制約は型付きUAVからの"読み出し"のもので、
        // EASU/RCASはUAVへ書くだけである(RCASがEASUの結果を読むのはSRV経由)。
        // Bloomが同じくR16G16B16A16_FloatのUAVへ書けているのと同じ理屈
        UpscaleTexture = device.CreateUAVTexture(width, height, RHI::Format::R8G8B8A8_UNorm);
        UpscaleSharpTexture = device.CreateUAVTexture(width, height, RHI::Format::R8G8B8A8_UNorm);
        UpscaleTargetWidth = width;
        UpscaleTargetHeight = height;
    }

    void RenderTargets::ResetUpscale()
    {
        UpscaleTexture.reset();
        UpscaleSharpTexture.reset();
        UpscaleTargetWidth = 0;
        UpscaleTargetHeight = 0;
    }

    void RenderTargets::CreateLightTiles(
        RHI::IRHIDevice& device, uint32_t width, uint32_t height, uint32_t tileSize, uint32_t stride)
    {
        // 端のタイルは部分的にしか埋まらないので切り上げる
        LightTileCountX = (width + tileSize - 1) / tileSize;
        LightTileCountY = (height + tileSize - 1) / tileSize;
        RHI::BufferDesc lightTileBufferDesc;
        lightTileBufferDesc.Usage = RHI::BufferUsage::StructuredRW;
        lightTileBufferDesc.SizeInBytes =
            static_cast<uint32_t>(sizeof(uint32_t)) * stride * LightTileCountX * LightTileCountY;
        lightTileBufferDesc.StrideInBytes = static_cast<uint32_t>(sizeof(uint32_t));
        LightTileBuffer = device.CreateBuffer(lightTileBufferDesc);
    }

    void RenderTargets::CreateMegaLightsTilePool(RHI::IRHIDevice& device, uint32_t stride)
    {
        RHI::BufferDesc tilePoolBufferDesc;
        tilePoolBufferDesc.Usage = RHI::BufferUsage::StructuredRW;
        // ジッター有効時は右端・下端のタイル座標が1つ増える。トグル変更でGPUを
        // 待って再確保しなくて済むよう、無効時も常に+1ぶんを確保しておく
        tilePoolBufferDesc.SizeInBytes =
            static_cast<uint32_t>(sizeof(uint32_t)) * stride * (LightTileCountX + 1u) * (LightTileCountY + 1u);
        tilePoolBufferDesc.StrideInBytes = static_cast<uint32_t>(sizeof(uint32_t));
        MegaLightsTilePoolBuffer = device.CreateBuffer(tilePoolBufferDesc);
    }

    void RenderTargets::CreateMegaLightsOutput(RHI::IRHIDevice& device, uint32_t width, uint32_t height)
    {
        MegaLightsTexture = device.CreateUAVTexture(width, height, RHI::Format::R32G32B32A32_Float);
    }

    void RenderTargets::CreateMegaLightsDenoised(RHI::IRHIDevice& device, uint32_t width, uint32_t height)
    {
        MegaLightsDenoisedTexture = device.CreateUAVTexture(width, height, RHI::Format::R32G32B32A32_Float);
    }

    void RenderTargets::CreateMegaLightsAccum(RHI::IRHIDevice& device, uint32_t elementCount)
    {
        RHI::BufferDesc accumBufferDesc;
        accumBufferDesc.Usage = RHI::BufferUsage::StructuredRW;
        accumBufferDesc.SizeInBytes = static_cast<uint32_t>(sizeof(float) * 4) * elementCount;
        accumBufferDesc.StrideInBytes = static_cast<uint32_t>(sizeof(float) * 4);
        MegaLightsAccumBuffer = device.CreateBuffer(accumBufferDesc);
    }

    void RenderTargets::CreateMegaLightsReservoirs(
        RHI::IRHIDevice& device, uint32_t width, uint32_t height, uint32_t samplesPerPixel)
    {
        RHI::BufferDesc reservoirBufferDesc;
        reservoirBufferDesc.Usage = RHI::BufferUsage::StructuredRW;
        reservoirBufferDesc.SizeInBytes =
            static_cast<uint32_t>(sizeof(uint32_t) * 4) * width * height * samplesPerPixel;
        reservoirBufferDesc.StrideInBytes = static_cast<uint32_t>(sizeof(uint32_t) * 4);
        MegaLightsReservoirBuffer = device.CreateBuffer(reservoirBufferDesc);

        // 画素ごとの「遮蔽が確定した灯」のキャッシュ(uint。0xFFFFFFFFで無し)。
        // 殺しの持ち回りより寿命が長く、影の縁の暗いフリンジを消すのに要る
        // (MegaLightsInitialSample.hlsl の BlockedLights のコメント)
        RHI::BufferDesc blockedBufferDesc;
        blockedBufferDesc.Usage = RHI::BufferUsage::StructuredRW;
        blockedBufferDesc.SizeInBytes = static_cast<uint32_t>(sizeof(uint32_t)) * width * height;
        blockedBufferDesc.StrideInBytes = static_cast<uint32_t>(sizeof(uint32_t));
        MegaLightsBlockedLightBuffer = device.CreateBuffer(blockedBufferDesc);
        // 空間再利用の出力先。近傍を読むので入力と同じバッファへは書けない。
        // 2回以上回すときは2本を ping-pong する
        MegaLightsReservoirSpatialBuffer = device.CreateBuffer(reservoirBufferDesc);
        MegaLightsReservoirSpatialBuffer2 = device.CreateBuffer(reservoirBufferDesc);

        // 時間再利用の履歴。**2本のping-pongにするのは、RenderGraphがWARの辺を
        // 張らないため**。1本で済ませると「今フレームのTemporalが読んだ直後に
        // 同じバッファへ書く」形になり、条件分岐でパスが1つ消えた瞬間に静かに壊れる。
        // 2本なら全ての辺がRAWで張れる(前フレームが書いた側を読み、今フレームは
        // もう片方へ書く)
        for (auto& buffer : MegaLightsReservoirHistory)
        {
            buffer = device.CreateBuffer(reservoirBufferDesc);
        }
    }

    void RenderTargets::CreateMegaLightsHistoryGuide(RHI::IRHIDevice& device, uint32_t width, uint32_t height)
    {
        // 1画素12バイト(法線oct 4 + View空間Z 4 + 材質 4)。
        // MegaLightsCommon.hlsli の MegaLightsHistoryGuide とストライドを一致させること
        RHI::BufferDesc guideBufferDesc;
        guideBufferDesc.Usage = RHI::BufferUsage::StructuredRW;
        guideBufferDesc.SizeInBytes = static_cast<uint32_t>(sizeof(uint32_t) * 3) * width * height;
        guideBufferDesc.StrideInBytes = static_cast<uint32_t>(sizeof(uint32_t) * 3);
        for (auto& buffer : MegaLightsHistoryGuide)
        {
            buffer = device.CreateBuffer(guideBufferDesc);
        }
    }

    void RenderTargets::CreateMegaLightsDenoiseWork(RHI::IRHIDevice& device, uint32_t width, uint32_t height)
    {
        for (int denoiseIndex = 0; denoiseIndex < 2; ++denoiseIndex)
        {
            MegaLightsDenoiseHistory[denoiseIndex] =
                device.CreateUAVTexture(width, height, RHI::Format::R32G32B32A32_Float);
            MegaLightsDenoiseMoments[denoiseIndex] =
                device.CreateUAVTexture(width, height, RHI::Format::R32G32B32A32_Float);
            MegaLightsDenoisePing[denoiseIndex] =
                device.CreateUAVTexture(width, height, RHI::Format::R32G32B32A32_Float);
            MegaLightsDenoiseMomentPing[denoiseIndex] =
                device.CreateUAVTexture(width, height, RHI::Format::R32G32B32A32_Float);
        }
    }

    void RenderTargets::ResetSoftwareRasterOutputs()
    {
        SoftwareRasterColor.reset();
        SoftwareRasterDepth.reset();
        SoftwareRasterNormal.reset();
    }
}
