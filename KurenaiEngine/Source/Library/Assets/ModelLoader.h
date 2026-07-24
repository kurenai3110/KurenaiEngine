#pragma once

#include <string>

#include "KurenaiTypes.h"
#include "Model.h"
#include "RHI/IRHIDevice.h"

namespace Kurenai::Assets
{
    KURENAI_API Model LoadModel(RHI::IRHIDevice& device, const std::wstring& filePath);
}
