#pragma once

#include <d3d12.h>

#include <cstdio>
#include <stdexcept>
#include <string>

#include "Core/Logger.h"

namespace Kurenai::RHI
{
    // HRESULTが失敗の場合、メッセージ付きの例外を送出する
    inline void ThrowIfFailed(HRESULT hr, const std::string& message)
    {
        if (FAILED(hr))
        {
            char hrText[16];
            std::snprintf(hrText, sizeof(hrText), "0x%08X", static_cast<unsigned int>(hr));
            const std::string fullMessage = message + " (HRESULT: " + hrText + ")";

            // 例外を送出する前に、ログを出力する
            Core::Logger::Error("DX12", fullMessage);

            throw std::runtime_error(fullMessage);
        }
    }
}
