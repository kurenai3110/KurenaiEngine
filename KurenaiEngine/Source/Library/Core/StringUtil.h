#pragma once

#include <Windows.h>

#include <string>

// UTF-8 <-> UTF-16(wstring)の相互変換と、DLL自身の配置フォルダの取得をまとめた
// ヘッダオンリーの共通ユーティリティ。パス・文字列を扱うコードは各所へ同じ実装を
// 複製せず、必ずここを使うこと。
// パスやシーン名には空白・日本語・記号が入りうるため、std::stringのままパス操作を
// せず、必ずこの変換を通してstd::wstringへ寄せてから扱うこと。

namespace Kurenai::Core
{
    inline std::string WideToUtf8(const std::wstring& wide)
    {
        if (wide.empty())
        {
            return {};
        }

        int length = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string narrow(length, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, narrow.data(), length, nullptr, nullptr);
        narrow.resize(length - 1);
        return narrow;
    }

    inline std::wstring Utf8ToWide(const std::string& narrow)
    {
        if (narrow.empty())
        {
            return {};
        }

        int length = MultiByteToWideChar(CP_UTF8, 0, narrow.c_str(), -1, nullptr, 0);
        std::wstring wide(length, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, narrow.c_str(), -1, wide.data(), length);
        wide.resize(length - 1);
        return wide;
    }

    // 呼び出し元(exe)ではなくこの関数が属するモジュール(KurenaiEngine.dll)自身の
    // フォルダを返す。Shaders/AssetsはDLLと同じフォルダに配置される運用のため、
    // DLLがどこにコピーされて使われても(各サンプルのBuildフォルダ配下など)
    // データを正しく解決できる。inline関数のためどのTUから呼んでも、その関数の
    // 実体が属するモジュール(=最終的にKurenaiEngine.dllにリンクされる)が返る
    inline std::wstring GetModuleDirectory()
    {
        HMODULE module = nullptr;
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&GetModuleDirectory),
            &module);

        wchar_t path[MAX_PATH];
        GetModuleFileNameW(module, path, MAX_PATH);
        std::wstring pathStr(path);
        const size_t pos = pathStr.find_last_of(L"\\/");
        return pos == std::wstring::npos ? L"" : pathStr.substr(0, pos + 1);
    }
}
