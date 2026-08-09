#pragma once

#include <d3d11.h>

#include <cstdio>
#include <stdexcept>
#include <string>

#include "Core/Logger.h"

namespace Kurenai::RHI
{
    // HRESULTが失敗の場合、メッセージ付きの例外を送出する。
    //
    // DX12Util.hにも同じ名前・同じシグネチャの関数がログのタグだけ変えて定義されている。
    // inlineのままだと同一名前空間の同一シグネチャで実体が2つある状態(ODR違反)になり、
    // リンカがどちらか一方に統一してしまい、DX12のエラーが[DX11]タグでログに出る。
    // staticにして翻訳単位ごとに別の実体を持たせることで、includeしたヘッダ側のタグが必ず使われる
    static inline void ThrowIfFailed(HRESULT hr, const std::string& message)
    {
        if (FAILED(hr))
        {
            char hrText[16];
            std::snprintf(hrText, sizeof(hrText), "0x%08X", static_cast<unsigned int>(hr));
            const std::string fullMessage = message + " (HRESULT: " + hrText + ")";

            // 例外を送出する前に、ログを出力する
            Core::Logger::Error("DX11", fullMessage);

            throw std::runtime_error(fullMessage);
        }
    }
}
