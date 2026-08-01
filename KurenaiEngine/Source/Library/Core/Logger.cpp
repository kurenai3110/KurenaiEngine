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

        // 実行ファイルと同じディレクトリのKurenaiEngine<接尾辞>.logへのフルパスを取得する。
        // 実行ファイルのパスを取得できなかった場合はファイル名だけを返す(作業ディレクトリ基準)
        std::string LogFilePath(const std::string& suffix)
        {
            const std::string fileName = "KurenaiEngine" + suffix + ".log";

            char exePath[MAX_PATH] = {};
            const DWORD length = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
            if (length == 0 || length == MAX_PATH)
            {
                return fileName;
            }

            std::string path(exePath, length);
            const size_t lastSlash = path.find_last_of("\\/");
            if (lastSlash == std::string::npos)
            {
                return fileName;
            }

            return path.substr(0, lastSlash + 1) + fileName;
        }

        // ログファイルへの出力ストリーム。プロセス起動後の初回書き込み時に
        // 新規作成(trunc)し、以降はプロセス終了までopenしたまま追記し続ける。
        // 関数ローカルstaticにしているのは静的初期化・破棄の順序に依存しないため
        std::ofstream& LogFileStream()
        {
            static std::ofstream stream;
            return stream;
        }

        // ログファイルを開こうとしたかどうか。openに失敗した場合もtrueにして再試行しない
        // (書き込みのたびに失敗するパスへopenを繰り返さないため)
        bool& LogFileOpened()
        {
            static bool opened = false;
            return opened;
        }

        // ログファイル名に付ける接尾辞(Logger::SetFileSuffixで設定する)
        std::string& LogFileSuffix()
        {
            static std::string suffix;
            return suffix;
        }

        std::mutex& LogMutex()
        {
            static std::mutex mutex;
            return mutex;
        }

        // 初回書き込み時にログファイルを開く。呼び出し元がLogMutex()を保持していること
        void EnsureLogFileOpenLocked()
        {
            if (LogFileOpened())
            {
                return;
            }

            LogFileOpened() = true;

            const std::string path = LogFilePath(LogFileSuffix());
            LogFileStream().open(path, std::ios::out | std::ios::trunc);
            if (!LogFileStream().is_open())
            {
                // ここでLogger::Errorを呼ぶとLogMutex()の再帰ロックでデッドロックするため、
                // またログファイル自体が開けていないため、OutputDebugStringへ直接出力する
                OutputDebugStringA(
                    ("[KurenaiEngine][Logger][Error] ログファイルを開けませんでした: " + path + "\n").c_str());
            }
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

    void Logger::SetFileSuffix(const std::string& suffix)
    {
        std::lock_guard<std::mutex> lock(LogMutex());

        if (LogFileSuffix() == suffix)
        {
            return;
        }

        if (!LogFileOpened())
        {
            // 正常系。まだログファイルを開いていないので、次の出力時にこの接尾辞で開かれる
            LogFileSuffix() = suffix;
            return;
        }

        // ここへ来るのは「既にログを出力し始めた後で接尾辞が指定された」場合。
        // 従来のファイル名のまま書き続けるとDX11とDX12が同じファイルを奪い合って
        // 双方のログが壊れるため、両方のファイルに追跡用の警告を残したうえで開き直す
        const std::string previousPath = LogFilePath(LogFileSuffix());
        const std::string newPath = LogFilePath(suffix);

        WriteLocked(LogLevel::Warning, "Logger",
            "ログの出力開始後に接尾辞が指定されました。以降は " + newPath + " へ出力します");

        LogFileStream().close();
        LogFileOpened() = false;
        LogFileSuffix() = suffix;

        WriteLocked(LogLevel::Warning, "Logger",
            "ログの出力開始後に接尾辞が指定されたため、この行より前のログは " + previousPath + " にあります");
    }

    void Logger::Write(LogLevel level, const std::string& tag, const std::string& message)
    {
        std::lock_guard<std::mutex> lock(LogMutex());
        WriteLocked(level, tag, message);
    }

    void Logger::WriteLocked(LogLevel level, const std::string& tag, const std::string& message)
    {
        std::ostringstream line;
        line << "[" << CurrentTimestamp() << "][KurenaiEngine][" << tag << "][" << LevelToText(level) << "] " << message;

        // DebugView等でリアルタイムに確認できるよう出力する
        OutputDebugStringA((line.str() + "\n").c_str());

        // デバッガ非接続時やクラッシュ後の事後調査のためファイルにも書き出す
        EnsureLogFileOpenLocked();

        std::ofstream& file = LogFileStream();
        if (file.is_open())
        {
            file << line.str() << std::endl;
            file.flush();
        }
    }
}
