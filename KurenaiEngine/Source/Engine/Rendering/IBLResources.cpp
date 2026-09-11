#include "Rendering/IBLResources.h"

namespace Kurenai::Rendering
{
    void IBLResources::CreateEnvironmentMaps(
        RHI::IRHIDevice& device, uint32_t irradianceSize, uint32_t prefilterBaseSize,
        uint32_t prefilterMipLevels)
    {
        IrradianceTexture = device.CreateUAVTextureCube(irradianceSize, RHI::Format::R16G16B16A16_Float);
        PrefilteredEnvTexture = device.CreateMippedUAVTextureCube(
            prefilterBaseSize, RHI::Format::R16G16B16A16_Float, prefilterMipLevels);
    }

    void IBLResources::CreateBRDFLUT(RHI::IRHIDevice& device, uint32_t size)
    {
        BRDFLUTTexture = device.CreateUAVTexture(size, size, RHI::Format::R16G16B16A16_Float);
    }

    void IBLResources::CreatePrefilterConstantBuffer(RHI::IRHIDevice& device, uint32_t sizeInBytes)
    {
        RHI::BufferDesc desc;
        desc.Usage = RHI::BufferUsage::Constant;
        desc.SizeInBytes = sizeInBytes;
        PrefilterConstantBuffer = device.CreateBuffer(desc);
    }
}
