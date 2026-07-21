#pragma once

#include <d3d11.h>

#include <cstdio>
#include <stdexcept>
#include <string>

namespace Kurenai::RHI
{
    // HRESULTが失敗の場合、メッセージ付きの例外を送出する
    inline void ThrowIfFailed(HRESULT hr, const std::string& message)
    {
        if (FAILED(hr))
        {
            char hrText[16];
            std::snprintf(hrText, sizeof(hrText), "0x%08X", static_cast<unsigned int>(hr));
            throw std::runtime_error(message + " (HRESULT: " + hrText + ")");
        }
    }
}
