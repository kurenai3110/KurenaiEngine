#include "Rendering/SkyResources.h"

namespace Kurenai::Rendering
{
    void SkyResources::CreateCloudNoise(
        RHI::IRHIDevice& device, uint32_t shapeSize, uint32_t detailSize, uint32_t weatherSize)
    {
        CloudShapeNoiseTexture = device.CreateUAVTexture3D(
            shapeSize, shapeSize, shapeSize, RHI::Format::R8G8B8A8_UNorm);
        CloudDetailNoiseTexture = device.CreateUAVTexture3D(
            detailSize, detailSize, detailSize, RHI::Format::R8G8B8A8_UNorm);
        // ウェザーマップ(H3)。2Dなので CreateUAVTexture。8bitで足りることは実測済み
        // (同じ解像度なら16bitとの誤差の差は0.0002。効くのは空間の刻みだけ)
        CloudWeatherNoiseTexture = device.CreateUAVTexture(
            weatherSize, weatherSize, RHI::Format::R8G8B8A8_UNorm);
    }

    void SkyResources::CreateAtmosphereLUTs(
        RHI::IRHIDevice& device, uint32_t transmittanceWidth, uint32_t transmittanceHeight,
        uint32_t multiScatteringSize, uint32_t skyViewWidth, uint32_t skyViewHeight)
    {
        // HDRの放射輝度を格納するためR16G16B16A16_Float
        TransmittanceLUT = device.CreateUAVTexture(
            transmittanceWidth, transmittanceHeight, RHI::Format::R16G16B16A16_Float);
        MultiScatteringLUT = device.CreateUAVTexture(
            multiScatteringSize, multiScatteringSize, RHI::Format::R16G16B16A16_Float);
        SkyViewLUT = device.CreateUAVTexture(
            skyViewWidth, skyViewHeight, RHI::Format::R16G16B16A16_Float);
    }

    void SkyResources::CreateParametersBuffer(RHI::IRHIDevice& device, uint32_t sizeInBytes)
    {
        RHI::BufferDesc desc;
        desc.Usage = RHI::BufferUsage::StructuredRW;
        desc.SizeInBytes = sizeInBytes;
        desc.StrideInBytes = sizeInBytes;
        ParametersBuffer = device.CreateBuffer(desc);
    }
}
