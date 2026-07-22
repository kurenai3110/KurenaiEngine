#pragma once

#include <d3d11.h>

#include "RHI/IRHIImGuiBackend.h"

namespace Kurenai::RHI
{
    class DX11ImGuiBackend : public IRHIImGuiBackend
    {
    public:
        DX11ImGuiBackend(ID3D11Device* device, ID3D11DeviceContext* context, void* windowHandle);
        ~DX11ImGuiBackend() override;

        void NewFrame() override;
        void Render() override;
    };
}
