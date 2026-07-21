#pragma once

#include <string>

#include "Model.h"
#include "RHI/IRHIDevice.h"

namespace Kurenai::Assets
{
    Model LoadModel(RHI::IRHIDevice& device, const std::wstring& filePath);
}
