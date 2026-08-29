#pragma once

#include <Windows.h>

// ComPtr<IDxcCompiler> のデストラクタを実体化するには完全型が要るため、
// 前方宣言では足りない(ビルド時ツールなのでコンパイル時間は問題にならない)
#include <dxcapi.h>

#include <cstdint>
#include <string>
#include <vector>
#include <wrl/client.h>

#include "Assets/ShaderPackage.h"

namespace Kurenai::ShaderPacker
{
    struct CompileResult
    {
        bool Succeeded = false;
        std::vector<uint8_t> Bytecode;
        // 失敗理由、または成功時の警告。空でなければ必ず表示する
        std::string Diagnostics;
    };

    // dxcompiler.dll のロードと、そこから作れるシェーダーモデルの上限判定。
    // プロセスで1つだけ作り、各ワーカースレッドの ShaderCompiler がこれを共有する。
    //
    // 【動的ロードのままにしている理由】dxcompiler.dll は OS 標準では入っておらず、
    // Windows SDK の bin\<SDKバージョン>\x64 から実行ファイルの隣へ配布する必要がある。
    // 静的リンク(dxcompiler.lib)にすると DLL が無いだけでプロセスが起動できなくなり、
    // 「SM 6.x のバリアントだけ諦めて SM 5.0 は焼く」という縮退ができない
    class DxcRuntime
    {
    public:
        DxcRuntime() = default;
        ~DxcRuntime();

        DxcRuntime(const DxcRuntime&) = delete;
        DxcRuntime& operator=(const DxcRuntime&) = delete;

        // dxcompiler.dll をロードし、対応シェーダーモデルの上限を調べる。
        // 見つからない/古い場合も例外は投げず false を返す(理由は outMessage へ)
        bool Initialize(std::string& outMessage);

        bool IsAvailable() const { return m_CreateInstance != nullptr; }

        // SM 6.6 のプロファイル("ps_6_6"等)を受け付けるか。
        // 【dxc のバージョン = Windows SDK のバージョン】SM 6.6 を知っているのは dxc 1.6 以降で、
        // Windows SDK 10.0.19041 同梱の dxc 1.5 では "invalid target" になる。
        // これが false のとき Dxil66 バリアントは焼けず、実行時に bindless が無効になる
        bool SupportsShaderModel66() const { return m_SupportsShaderModel66; }

        // dxil.dll(署名用)が dxcompiler.dll と同じフォルダにあるか。
        // 無いと生成した DXIL が未署名になり、開発者モードでない環境で
        // パイプラインステートの作成に失敗する(気付きにくいので警告する)
        bool HasDxil() const { return m_HasDxil; }

        // DxcCreateInstance のアドレス。ShaderCompiler がスレッドごとの COM を作るのに使う
        void* CreateInstanceProc() const { return m_CreateInstance; }

    private:
        HMODULE m_Module = nullptr;
        void* m_CreateInstance = nullptr;
        bool m_SupportsShaderModel66 = false;
        bool m_HasDxil = false;
    };

    // 1スレッドにつき1つ持つコンパイラ。
    // IDxcCompiler をスレッド間で共有しないために分けている
    class ShaderCompiler
    {
    public:
        ShaderCompiler() = default;
        ~ShaderCompiler();

        ShaderCompiler(const ShaderCompiler&) = delete;
        ShaderCompiler& operator=(const ShaderCompiler&) = delete;

        // dxc の COM オブジェクトを生成する。runtime が使えない場合は false
        bool Initialize(const DxcRuntime& runtime, std::string& outMessage);

        // dxc で DXIL へコンパイルする。
        // 引数は KurenaiEngine の実行時コンパイル(旧 DX12ShaderCompiler::Compile)と
        // 同じものを渡す ―― -HV 2018 の固定、シェーダーフォルダの -I、
        // Debug構成の -Zi -Qembed_debug -Od / Release構成の -O3、bindless の define。
        // ここを変えると、焼いた結果が従来の実行時コンパイルと一致しなくなる
        CompileResult CompileDxil(
            const std::wstring& filePath,
            const std::string& entryPoint,
            Assets::ShaderPackageStage stage,
            const char* shaderModelSuffix,   // "6_6" / "6_5"
            bool bindless,
            bool debugBuild) const;

        // d3dcompiler(D3DCompileFromFile)で DXBC(SM 5.0)へコンパイルする。
        //
        // 【fxc.exe ではなく D3DCompileFromFile を呼ぶ】DX11 の実行時コンパイル
        // (DX11Device::CreateShader)がまさにこの関数を同じ引数で呼んでいた。
        // 同じコンパイラを同じ引数で通すので、焼いたバイトコードは従来と一致する
        // ―― 「事前コンパイルにしても描画が変わっていない」を言うための前提になる
        static CompileResult CompileDxbc(
            const std::wstring& filePath,
            const std::string& entryPoint,
            Assets::ShaderPackageStage stage,
            bool debugBuild);

    private:
        Microsoft::WRL::ComPtr<IDxcLibrary> m_Library;
        Microsoft::WRL::ComPtr<IDxcCompiler> m_Compiler;
    };
}
