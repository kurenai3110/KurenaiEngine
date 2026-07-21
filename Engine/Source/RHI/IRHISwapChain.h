#pragma once

#include <cstdint>

namespace Kurenai::RHI
{
    class IRHISwapChain
    {
    public:
        virtual ~IRHISwapChain() = default;

        virtual void Present(bool vsync) = 0;
        virtual void Resize(uint32_t width, uint32_t height) = 0;
    };
}
