#include "ShaderCompiler.h"

#include <d3dcompiler.h>
#include <dxcapi.h>

#include <atomic>
#include <filesystem>
#include <fstream>

#include "Core/StringUtil.h"
#include "ShaderEntryScanner.h"

using Microsoft::WRL::ComPtr;

namespace Kurenai::ShaderPacker
{
    namespace
    {
        using Assets::ShaderPackageStage;
        using Core::WideToUtf8;

        constexpr const char* kDxcCreateInstanceExport = "DxcCreateInstance";
        constexpr const wchar_t* kDxcCompilerDllName = L"dxcompiler.dll";
        constexpr const wchar_t* kDxilDllName = L"dxil.dll";

        // ファイルをバイト列として読み込む。読めない場合はfalseを返す
        bool ReadFileBytes(const std::wstring& path, std::vector<char>& outBytes)
        {
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file)
            {
                return false;
            }

            const std::streamoff size = file.tellg();
            if (size < 0)
            {
                return false;
            }

            file.seekg(0, std::ios::beg);
            outBytes.resize(static_cast<size_t>(size));
            if (size > 0 && !file.read(outBytes.data(), size))
            {
                return false;
            }
            return true;
        }

        // #includeされたファイルをUTF-8として読み込むインクルードハンドラ。
        //
        // dxcの既定のインクルードハンドラ(IDxcLibrary::CreateIncludeHandler)は、
        // BOMの無いファイルの文字コードをシステムのANSIコードページとして解釈する。
        // このエンジンのシェーダーはBOM無しUTF-8で日本語コメントを含むため、
        // 日本語環境(CP932)ではUTF-8のバイト列が不正なCP932列として扱われ、
        // コンパイル以前にERROR_NO_UNICODE_TRANSLATION(0x80070459)で失敗する。
        // 自前で読み込んでCP_UTF8を明示することで、シェーダーファイル側にBOMを付ける
        // (d3dcompiler経路にも影響が及ぶ)ことなく解決する。
        //
        // 【元はランタイムにあった】DX12ShaderCompiler.cpp から移設したもの。
        // 事前コンパイルへ移しても、BOM無しUTF-8を読むという問題は消えない
        class Utf8IncludeHandler : public IDxcIncludeHandler
        {
        public:
            Utf8IncludeHandler(IDxcLibrary* library, std::filesystem::path baseDirectory)
                : m_Library(library), m_BaseDirectory(std::move(baseDirectory))
            {
            }

            HRESULT STDMETHODCALLTYPE LoadSource(LPCWSTR filename, IDxcBlob** includeSource) override
            {
                if (!filename || !includeSource)
                {
                    return E_POINTER;
                }
                *includeSource = nullptr;

                // dxcは「現在のファイルのあるフォルダ + インクルード名」のような候補パスを
                // 順に渡してくる。相対パスで来た場合はシェーダーフォルダを基準に解決する
                std::filesystem::path path(filename);
                if (path.is_relative())
                {
                    path = m_BaseDirectory / path;
                }

                std::error_code ec;
                const std::filesystem::path normalized = std::filesystem::weakly_canonical(path, ec);
                if (!ec)
                {
                    path = normalized;
                }

                std::vector<char> bytes;
                if (!ReadFileBytes(path.wstring(), bytes))
                {
                    // 存在しない候補パスに対しても呼ばれるため、ここではエラーを出さない
                    // (本当に見つからない場合はdxcが "file not found" を出す)
                    return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
                }

                ComPtr<IDxcBlobEncoding> blob;
                const HRESULT hr = m_Library->CreateBlobWithEncodingOnHeapCopy(
                    bytes.empty() ? "" : bytes.data(), static_cast<UINT32>(bytes.size()), CP_UTF8, &blob);
                if (FAILED(hr))
                {
                    return hr;
                }

                *includeSource = blob.Detach();
                return S_OK;
            }

            // このハンドラはCompileのスコープ内でスタックに置き、dxcへ生ポインタで渡すだけなので
            // 参照カウントは実際には使われない。COMの規約を満たすためだけの最小実装で、
            // deleteは行わない(スタックオブジェクトのため)
            ULONG STDMETHODCALLTYPE AddRef() override { return ++m_RefCount; }
            ULONG STDMETHODCALLTYPE Release() override { return --m_RefCount; }

            HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
            {
                if (!object)
                {
                    return E_POINTER;
                }
                if (riid == __uuidof(IDxcIncludeHandler) || riid == __uuidof(IUnknown))
                {
                    *object = this;
                    AddRef();
                    return S_OK;
                }
                *object = nullptr;
                return E_NOINTERFACE;
            }

        private:
            IDxcLibrary* m_Library = nullptr;
            std::filesystem::path m_BaseDirectory;
            std::atomic<ULONG> m_RefCount{ 1 };
        };

        // 末尾の改行と、UTF-8化で付くことがある終端のヌル文字を落とす
        void TrimTrailing(std::string& text)
        {
            while (!text.empty() && (text.back() == '\0' || text.back() == '\n' || text.back() == '\r'))
            {
                text.pop_back();
            }
        }
    }

    DxcRuntime::~DxcRuntime()
    {
        if (m_Module)
        {
            FreeLibrary(m_Module);
            m_Module = nullptr;
        }
    }

    bool DxcRuntime::Initialize(std::string& outMessage)
    {
        m_Module = LoadLibraryW(kDxcCompilerDllName);
        if (!m_Module)
        {
            outMessage = "dxcompiler.dllをロードできませんでした(GetLastError=" + std::to_string(GetLastError()) + ")";
            return false;
        }

        m_CreateInstance = reinterpret_cast<void*>(GetProcAddress(m_Module, kDxcCreateInstanceExport));
        if (!m_CreateInstance)
        {
            outMessage = "dxcompiler.dllにDxcCreateInstanceが見つかりませんでした";
            FreeLibrary(m_Module);
            m_Module = nullptr;
            return false;
        }

        // SM 6.6 を知っているかを IDxcVersionInfo で判定する。
        // 判定用に一時的なコンパイラを1つ作って、すぐ捨てる
        auto createInstance = reinterpret_cast<DxcCreateInstanceProc>(m_CreateInstance);
        ComPtr<IDxcCompiler> compiler;
        if (FAILED(createInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))) || !compiler)
        {
            outMessage = "dxcのCOMオブジェクトを生成できませんでした";
            m_CreateInstance = nullptr;
            FreeLibrary(m_Module);
            m_Module = nullptr;
            return false;
        }

        UINT32 major = 0;
        UINT32 minor = 0;
        ComPtr<IDxcVersionInfo> versionInfo;
        const bool hasVersion =
            SUCCEEDED(compiler.As(&versionInfo)) && versionInfo && SUCCEEDED(versionInfo->GetVersion(&major, &minor));
        m_SupportsShaderModel66 = hasVersion && (major > 1 || (major == 1 && minor >= 6));

        // dxil.dll(署名用ライブラリ)はdxcompiler.dllが自分と同じフォルダから読み込む。
        // 無い場合でもコンパイル自体は通るが、生成されたDXILは未署名になり、
        // 開発者モードが有効でない環境ではCreateGraphicsPipelineStateが失敗する
        wchar_t modulePath[MAX_PATH] = {};
        if (GetModuleFileNameW(m_Module, modulePath, MAX_PATH) != 0)
        {
            std::error_code ec;
            const std::filesystem::path dxilPath = std::filesystem::path(modulePath).parent_path() / kDxilDllName;
            m_HasDxil = std::filesystem::exists(dxilPath, ec);
        }

        outMessage = "dxc " + std::to_string(major) + "." + std::to_string(minor) +
                     (m_SupportsShaderModel66 ? " (SM 6.6 対応)" : " (SM 6.6 非対応。bindlessバリアントは焼けません)");
        return true;
    }

    ShaderCompiler::~ShaderCompiler()
    {
        // COMオブジェクトはDLLの中に実装があるため、DxcRuntimeのFreeLibraryより先に解放する
        m_Compiler.Reset();
        m_Library.Reset();
    }

    bool ShaderCompiler::Initialize(const DxcRuntime& runtime, std::string& outMessage)
    {
        if (!runtime.IsAvailable())
        {
            outMessage = "dxcが利用できません";
            return false;
        }

        auto createInstance = reinterpret_cast<DxcCreateInstanceProc>(runtime.CreateInstanceProc());
        if (FAILED(createInstance(CLSID_DxcLibrary, IID_PPV_ARGS(&m_Library))) ||
            FAILED(createInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&m_Compiler))))
        {
            m_Compiler.Reset();
            m_Library.Reset();
            outMessage = "dxcのCOMオブジェクトを生成できませんでした";
            return false;
        }
        return true;
    }

    CompileResult ShaderCompiler::CompileDxil(
        const std::wstring& filePath,
        const std::string& entryPoint,
        ShaderPackageStage stage,
        const char* shaderModelSuffix,
        bool bindless,
        bool debugBuild) const
    {
        CompileResult result{};

        if (!m_Compiler || !m_Library)
        {
            result.Diagnostics = "dxcが初期化されていません";
            return result;
        }

        std::vector<char> source;
        if (!ReadFileBytes(filePath, source))
        {
            result.Diagnostics = "シェーダファイルを開けませんでした: " + WideToUtf8(filePath);
            return result;
        }

        // BOM無しUTF-8として明示的に読み込む(Utf8IncludeHandlerのコメント参照)
        ComPtr<IDxcBlobEncoding> sourceBlob;
        if (FAILED(m_Library->CreateBlobWithEncodingOnHeapCopy(
                source.empty() ? "" : source.data(), static_cast<UINT32>(source.size()), CP_UTF8, &sourceBlob)))
        {
            result.Diagnostics = "シェーダソースのブロブ生成に失敗しました";
            return result;
        }

        const std::filesystem::path shaderPath(filePath);
        const std::wstring includeDirectory = shaderPath.parent_path().wstring();
        Utf8IncludeHandler includeHandler(m_Library.Get(), shaderPath.parent_path());

        const std::wstring target =
            Core::Utf8ToWide(std::string(StagePrefix(stage)) + "_" + shaderModelSuffix);
        const std::wstring entryPointWide = Core::Utf8ToWide(entryPoint);

        std::vector<const wchar_t*> arguments;
        // HLSLの言語バージョンを2018に固定する。dxcは将来のバージョンで既定を2021以降へ
        // 引き上げるため、明示しないとdxcompiler.dllを差し替えただけで
        // 既存シェーダーの意味が変わりうる
        arguments.push_back(L"-HV");
        arguments.push_back(L"2018");
        arguments.push_back(L"-I");
        arguments.push_back(includeDirectory.c_str());
        if (debugBuild)
        {
            // -Ziのデバッグ情報は既定では別ブロブになるため、-Qembed_debugでバイトコードへ埋め込む
            arguments.push_back(L"-Zi");
            arguments.push_back(L"-Qembed_debug");
            arguments.push_back(L"-Od");
        }
        else
        {
            arguments.push_back(L"-O3");
        }

        // bindless(ResourceDescriptorHeap)が使えるかをシェーダー側へ伝える。
        // Shaders/3D/Bindless.hlsliがこのマクロで実装を切り替え、定義されていない場合は
        // 「常に無効なディスクリプタ番号を返す」実装になるため、SM 6.5以下でも
        // 同じシェーダーソースがコンパイルできる
        std::vector<DxcDefine> defines;
        if (bindless)
        {
            defines.push_back(DxcDefine{ L"KURENAI_BINDLESS", L"1" });
        }

        ComPtr<IDxcOperationResult> operationResult;
        const HRESULT compileHr = m_Compiler->Compile(
            sourceBlob.Get(),
            filePath.c_str(),
            entryPointWide.c_str(),
            target.c_str(),
            arguments.data(),
            static_cast<UINT32>(arguments.size()),
            defines.empty() ? nullptr : defines.data(),
            static_cast<UINT32>(defines.size()),
            &includeHandler,
            &operationResult);

        if (FAILED(compileHr) || !operationResult)
        {
            result.Diagnostics = "シェーダのコンパイル呼び出しに失敗しました";
            return result;
        }

        // 成功時でも警告(dxil.dllが無い場合の未署名警告など)が入ることがあるため、
        // 状態に関わらずエラーバッファの内容を確認する
        ComPtr<IDxcBlobEncoding> errorBlob;
        if (SUCCEEDED(operationResult->GetErrorBuffer(&errorBlob)) && errorBlob && errorBlob->GetBufferSize() > 0)
        {
            // GetErrorBufferはコンパイラの文字コード設定に依存するため、UTF-8へ正規化してから読む
            ComPtr<IDxcBlobEncoding> utf8Errors;
            if (SUCCEEDED(m_Library->GetBlobAsUtf8(errorBlob.Get(), &utf8Errors)) && utf8Errors)
            {
                result.Diagnostics.assign(
                    static_cast<const char*>(utf8Errors->GetBufferPointer()), utf8Errors->GetBufferSize());
            }
            else
            {
                result.Diagnostics.assign(
                    static_cast<const char*>(errorBlob->GetBufferPointer()), errorBlob->GetBufferSize());
            }
            TrimTrailing(result.Diagnostics);
        }

        HRESULT status = E_FAIL;
        if (FAILED(operationResult->GetStatus(&status)) || FAILED(status))
        {
            return result;
        }

        ComPtr<IDxcBlob> bytecode;
        if (FAILED(operationResult->GetResult(&bytecode)) || !bytecode || bytecode->GetBufferSize() == 0)
        {
            result.Diagnostics = "シェーダのバイトコードを取得できませんでした";
            return result;
        }

        const auto* data = static_cast<const uint8_t*>(bytecode->GetBufferPointer());
        result.Bytecode.assign(data, data + bytecode->GetBufferSize());
        result.Succeeded = true;
        return result;
    }

    CompileResult ShaderCompiler::CompileDxbc(
        const std::wstring& filePath, const std::string& entryPoint, ShaderPackageStage stage, bool debugBuild)
    {
        CompileResult result{};

        // 増幅/メッシュシェーダーにはSM 5.0のプロファイルが存在しない
        if (stage == ShaderPackageStage::Amplification || stage == ShaderPackageStage::Mesh)
        {
            result.Diagnostics = "SM 5.0には増幅/メッシュシェーダーのプロファイルがありません";
            return result;
        }

        const std::string target = std::string(StagePrefix(stage)) + "_5_0";

        // 【実行時(DX11Device::CreateShader)と同じフラグにすること】
        // ここを変えると、事前コンパイルへ移した前後で生成バイトコードが変わってしまい、
        // 「描画が変わっていない」ことをバイト一致で確かめられなくなる
        UINT compileFlags = 0;
        if (debugBuild)
        {
            compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
        }

        ComPtr<ID3DBlob> bytecode;
        ComPtr<ID3DBlob> errorBlob;
        const HRESULT hr = D3DCompileFromFile(
            filePath.c_str(),
            nullptr,
            D3D_COMPILE_STANDARD_FILE_INCLUDE,
            entryPoint.c_str(),
            target.c_str(),
            compileFlags,
            0,
            &bytecode,
            &errorBlob);

        if (errorBlob && errorBlob->GetBufferSize() > 0)
        {
            result.Diagnostics.assign(
                static_cast<const char*>(errorBlob->GetBufferPointer()), errorBlob->GetBufferSize());
            TrimTrailing(result.Diagnostics);
        }

        if (FAILED(hr) || !bytecode || bytecode->GetBufferSize() == 0)
        {
            if (result.Diagnostics.empty())
            {
                result.Diagnostics = "D3DCompileFromFileが失敗しました(HRESULT=0x" + std::to_string(hr) + ")";
            }
            return result;
        }

        const auto* data = static_cast<const uint8_t*>(bytecode->GetBufferPointer());
        result.Bytecode.assign(data, data + bytecode->GetBufferSize());
        result.Succeeded = true;
        return result;
    }
}
