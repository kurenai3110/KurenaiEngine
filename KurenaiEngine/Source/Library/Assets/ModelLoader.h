#pragma once

#include <string>

#include "KurenaiTypes.h"
#include "Model.h"
#include "RHI/IRHIDevice.h"

namespace Kurenai::Assets
{
    KURENAI_LIB_API Model LoadModel(RHI::IRHIDevice& device, const std::wstring& filePath);
}
