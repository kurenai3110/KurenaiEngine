#include "Logger.h"

#include <Windows.h>

#include <chrono>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>

namespace Kurenai::Core
{
    namespace
    {
        const char* LevelToText(LogLevel level)
        {
            switch (level)
            {
            case LogLevel::Info:    return "Info";
            case LogLevel::Warning: return "Warning";
            case LogLevel::Error:   return "Error";
            default:                return "Unknown";
            }
        }

        // 現在時刻を"HH:MM:SS.mmm"形式にフォーマットする
        std::string CurrentTimestamp()
        {
            const auto now = std::chrono::system_clock::now();
            const auto nowTimeT = std::chrono::system_clock::to_time_t(now);
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

            std::tm localTime{};
            localtime_s(&localTime, &nowTimeT);

            char buffer[16];
            std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d.%03d",
                localTime.tm_hour, localTime.tm_min, localTime.tm_sec, static_cast<int>(ms.count()));
            return buffer;
        }

        // 実行ファイルと同じディレクトリのKurenaiEngine.logへのフルパスを取得する
        std::string LogFilePath()
        {
            char exePath[MAX_PATH] = {};
            const DWORD length = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
            if (length == 0 || length == MAX_PATH)
            {
                return "KurenaiEngine.log";
            }

            std::string path(exePath, length);
            const size_t lastSlash = path.find_last_of("\\/");
            if (lastSlash == std::string::npos)
            {
                return "KurenaiEngine.log";
            }

            return path.substr(0, lastSlash + 1) + "KurenaiEngine.log";
        }

        // ログファイルへの出力ストリーム。プロセス起動後の初回書き込み時に
        // 新規作成(trunc)し、以降はプロセス終了までopenしたまま追記し続ける
        std::ofstream& LogFileStream()
        {
            static std::ofstream stream(LogFilePath(), std::ios::out | std::ios::trunc);
            return stream;
        }

        std::mutex& LogMutex()
        {
            static std::mutex mutex;
            return mutex;
        }
    }

    void Logger::Info(const std::string& tag, const std::string& message)
    {
        Write(LogLevel::Info, tag, message);
    }

    void Logger::Warning(const std::string& tag, const std::string& message)
    {
        Write(LogLevel::Warning, tag, message);
    }

    void Logger::Error(const std::string& tag, const std::string& message)
    {
        Write(LogLevel::Error, tag, message);
    }

    void Logger::Write(LogLevel level, const std::string& tag, const std::string& message)
    {
        std::ostringstream line;
        line << "[" << CurrentTimestamp() << "][KurenaiEngine][" << tag << "][" << LevelToText(level) << "] " << message;

        std::lock_guard<std::mutex> lock(LogMutex());

        // DebugView等でリアルタイムに確認できるよう出力する
        OutputDebugStringA((line.str() + "\n").c_str());

        // デバッガ非接続時やクラッシュ後の事後調査のためファイルにも書き出す
        std::ofstream& file = LogFileStream();
        if (file.is_open())
        {
            file << line.str() << std::endl;
            file.flush();
        }
    }
}
