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
    // KurenaiEngine<接尾辞>.logの両方に出力する(接尾辞はSetFileSuffix参照。既定は空で
    // KurenaiEngine.log)。Update/Renderが別スレッドで動作するため、
    // 内部で排他制御しており、どのスレッドから呼んでも安全
    class KURENAI_LIB_API Logger
    {
    public:
        static void Info(const std::string& tag, const std::string& message);
        static void Warning(const std::string& tag, const std::string& message);
        static void Error(const std::string& tag, const std::string& message);

        // ログファイル名に付ける接尾辞を設定する("_DX12"ならKurenaiEngine_DX12.log)。
        // DX11とDX12は同じexe・同じフォルダで動くため、同時に起動すると1つの
        // ログファイルを奪い合って両方の内容が壊れる。それを避けるためのもの。
        //
        // ログファイルは最初の出力時に開かれるため、このメソッドは
        // 「プロセス内で最初にログを出力する前」に呼ぶこと(呼ばなければ従来どおり
        // KurenaiEngine.logへ出力する)。出力開始後に呼んだ場合は、旧ファイルと新ファイルの
        // 両方に警告を残したうえで新しいファイルへ切り替える
        static void SetFileSuffix(const std::string& suffix);

    private:
        static void Write(LogLevel level, const std::string& tag, const std::string& message);

        // Write()の本体。呼び出し元がLogMutex()を保持していることが前提。
        // std::mutexは非再帰なので、既にロックを持っているSetFileSuffix()からは
        // Write()ではなくこちらを呼ぶ
        static void WriteLocked(LogLevel level, const std::string& tag, const std::string& message);
    };
}
