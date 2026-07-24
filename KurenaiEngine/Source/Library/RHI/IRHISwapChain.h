#pragma once

#include <cstdint>

#include "KurenaiTypes.h"

namespace Kurenai::RHI
{
    class KURENAI_API IRHISwapChain
    {
    public:
        virtual ~IRHISwapChain() = default;

        virtual void Present(bool vsync) = 0;
        virtual void Resize(uint32_t width, uint32_t height) = 0;
    };
}
