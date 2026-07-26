#pragma once

#include <string>

#include "KurenaiTypes.h"

namespace Kurenai::Core
{
    enum class LogLevel
    {
        Info,
        Warning,
        Error,
    };

    // エンジン全体で共通のログ出力ユーティリティ。
    // OutputDebugString(DebugView等で確認可能)と実行ファイルと同じディレクトリの
    // KurenaiEngine.logの両方に出力する。Update/Renderが別スレッドで動作するため、
    // 内部で排他制御しており、どのスレッドから呼んでも安全
    class KURENAI_API Logger
    {
    public:
        static void Info(const std::string& tag, const std::string& message);
        static void Warning(const std::string& tag, const std::string& message);
        static void Error(const std::string& tag, const std::string& message);

    private:
        static void Write(LogLevel level, const std::string& tag, const std::string& message);
    };
}
