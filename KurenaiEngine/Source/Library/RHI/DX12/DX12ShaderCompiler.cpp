#include "DX12ShaderCompiler.h"

#include <dxcapi.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <vector>

#include "Core/Logger.h"
#include "Core/StringUtil.h"

namespace Kurenai::RHI
{
    namespace
    {
        // dxcのCOMオブジェクトを生成するエクスポート関数の名前
        constexpr const char* kDxcCreateInstanceExport = "DxcCreateInstance";
        // 実行ファイルと同じフォルダへ配布するdxcのDLL名。dxil.dllはdxcompiler.dllが
        // 自分と同じフォルダから読み込む署名用のライブラリで、これが無いと生成したDXILに
        // 署名が付かず、開発者モードでない環境ではパイプラインステートの作成が失敗する
        constexpr const wchar_t* kDxcCompilerDllName = L"dxcompiler.dll";
        constexpr const wchar_t* kDxilDllName = L"dxil.dll";

        // ファイルをバイト列として読み込む。読めない場合はfalseを返す(ログは呼び出し側で出す)
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
        // (DX11のd3dcompiler経路にも影響が及ぶ)ことなく解決する
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
                    // 存在しない候補パスに対しても呼ばれるため、ここではログを出さない
                    // (本当に見つからない場合はdxcが "file not found" を出し、
                    //  それをDX12ShaderCompiler::Compileがエラーログへ載せる)
                    return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
                }

                Microsoft::WRL::ComPtr<IDxcBlobEncoding> blob;
                const HRESULT hr = m_Library->CreateBlobWithEncodingOnHeapCopy(
                    bytes.empty() ? "" : bytes.data(), static_cast<UINT32>(bytes.size()), CP_UTF8, &blob);
                if (FAILED(hr))
                {
                    Core::Logger::Error(
                        "DX12", "インクルードファイルのブロブ生成に失敗しました: " + Core::WideToUtf8(path.wstring()));
                    return hr;
                }

                *includeSource = blob.Detach();
                return S_OK;
            }

            // このハンドラはCompile()のスコープ内でスタックに置き、dxcへ生ポインタで渡すだけなので
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

        // シェーダーステージからターゲットプロファイルの接頭辞を求める
        const wchar_t* GetProfilePrefix(ShaderStage stage)
        {
            switch (stage)
            {
            case ShaderStage::Vertex:
                return L"vs";
            case ShaderStage::Compute:
                return L"cs";
            case ShaderStage::Amplification:
                return L"as";
            case ShaderStage::Mesh:
                return L"ms";
            case ShaderStage::Pixel:
            default:
                return L"ps";
            }
        }

        // D3D_SHADER_MODEL_6_5 → "6_5" のような、プロファイル文字列に使う表記へ変換する
        std::wstring ToProfileSuffix(D3D_SHADER_MODEL model)
        {
            const int value = static_cast<int>(model);
            return std::to_wstring((value >> 4) & 0xF) + L"_" + std::to_wstring(value & 0xF);
        }
    }

    DX12ShaderCompiler::DX12ShaderCompiler() = default;

    DX12ShaderCompiler::~DX12ShaderCompiler()
    {
        // COMオブジェクトはDLLの中に実装があるため、FreeLibraryより先に解放する
        m_Compiler.Reset();
        m_Library.Reset();

        if (m_Module)
        {
            FreeLibrary(m_Module);
            m_Module = nullptr;
        }
    }

    bool DX12ShaderCompiler::Initialize(D3D_SHADER_MODEL highestShaderModel)
    {
        // SM 6.0未満しか動かないデバイスではDXILを渡せないため、dxcをロードする意味がない
        if (highestShaderModel < D3D_SHADER_MODEL_6_0)
        {
            Core::Logger::Warning(
                "DX12",
                "シェーダーモデル6.0非対応のデバイスのため、dxcを使用しません(d3dcompiler/SM 5.0で動作します)");
            return false;
        }

        // このエンジンが必要とする最上位はSM 6.6(bindlessのResourceDescriptorHeap)。
        // それ以降のシェーダーモデルを使う機能は無いため、対応していても6.6で頭打ちにする。
        //
        // 【デバイスの上限だけでは決められない】プロファイル文字列を受け付けるかは
        // dxcompiler.dll側のバージョンにも依存する。Windows SDK 10.0.19041同梱の
        // dxcompiler.dll 1.5はSM 6.5までしか知らないため、GPUが6.6対応でも6.6を指定すると
        // 「invalid target」で全シェーダーのコンパイルが落ちる。
        // COMオブジェクト生成後にIDxcVersionInfoで実際のdxcバージョンを見て確定させる
        // (この時点ではデバイス側の上限だけを控えておく)
        m_ShaderModel = highestShaderModel > D3D_SHADER_MODEL_6_6 ? D3D_SHADER_MODEL_6_6 : highestShaderModel;

        m_Module = LoadLibraryW(kDxcCompilerDllName);
        if (!m_Module)
        {
            Core::Logger::Warning(
                "DX12",
                std::string("dxcompiler.dllをロードできませんでした(GetLastError=") + std::to_string(GetLastError()) +
                    ")。d3dcompiler/SM 5.0へフォールバックします(レイトレーシングは無効になります)");
            m_ShaderModel = static_cast<D3D_SHADER_MODEL>(0);
            return false;
        }

        auto createInstance =
            reinterpret_cast<DxcCreateInstanceProc>(reinterpret_cast<void*>(GetProcAddress(m_Module, kDxcCreateInstanceExport)));
        if (!createInstance)
        {
            Core::Logger::Warning(
                "DX12", "dxcompiler.dllにDxcCreateInstanceが見つかりませんでした。d3dcompiler/SM 5.0へフォールバックします");
            FreeLibrary(m_Module);
            m_Module = nullptr;
            m_ShaderModel = static_cast<D3D_SHADER_MODEL>(0);
            return false;
        }

        if (FAILED(createInstance(CLSID_DxcLibrary, IID_PPV_ARGS(&m_Library))) ||
            FAILED(createInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&m_Compiler))))
        {
            Core::Logger::Warning(
                "DX12", "dxcのCOMオブジェクトを生成できませんでした。d3dcompiler/SM 5.0へフォールバックします");
            m_Compiler.Reset();
            m_Library.Reset();
            FreeLibrary(m_Module);
            m_Module = nullptr;
            m_ShaderModel = static_cast<D3D_SHADER_MODEL>(0);
            return false;
        }

        // dxcompiler.dll側が扱えるシェーダーモデルの上限で、デバイス側の上限をさらに抑える。
        // SM 6.6のプロファイル(vs_6_6等)を知っているのはdxc 1.6以降。IDxcVersionInfoを
        // 取得できない古い実装は1.5以前とみなして6.5へ落とす。
        //
        // ここで落とすとResourceDescriptorHeap(bindless)が使えなくなるが、
        // SupportsBindless()がfalseになるだけで、レイトレーシングや通常描画は
        // 従来どおりSM 6.5で動く(RT/SSRの選択と同じ縮退の仕方)
        if (m_ShaderModel > D3D_SHADER_MODEL_6_5)
        {
            UINT32 dxcMajor = 0;
            UINT32 dxcMinor = 0;
            Microsoft::WRL::ComPtr<IDxcVersionInfo> versionInfo;
            const bool hasVersion = SUCCEEDED(m_Compiler.As(&versionInfo)) && versionInfo &&
                                    SUCCEEDED(versionInfo->GetVersion(&dxcMajor, &dxcMinor));
            if (!hasVersion || dxcMajor < 1 || (dxcMajor == 1 && dxcMinor < 6))
            {
                Core::Logger::Warning(
                    "DX12",
                    "dxcompiler.dllがシェーダーモデル6.6に対応していないため6.5で動作します"
                    "(bindlessは無効。Windows SDKのバージョンを上げるとdxcも更新されます)");
                m_ShaderModel = D3D_SHADER_MODEL_6_5;
            }
        }

        // dxil.dll(署名用ライブラリ)はdxcompiler.dllが自分と同じフォルダから読み込む。
        // 無い場合でもコンパイル自体は通るが、生成されたDXILは未署名になり、
        // 開発者モードが有効でない環境ではCreateGraphicsPipelineStateが失敗する。
        // 気付きにくい失敗なので、ロードできたdxcompiler.dllの隣にあるかどうかを確認して警告する
        wchar_t modulePath[MAX_PATH] = {};
        if (GetModuleFileNameW(m_Module, modulePath, MAX_PATH) != 0)
        {
            std::error_code ec;
            const std::filesystem::path dxilPath = std::filesystem::path(modulePath).parent_path() / kDxilDllName;
            if (!std::filesystem::exists(dxilPath, ec))
            {
                Core::Logger::Warning(
                    "DX12",
                    "dxil.dllがdxcompiler.dllと同じフォルダに見つかりません: " + Core::WideToUtf8(dxilPath.wstring()) +
                        " (DXILが未署名になり、開発者モードでない環境ではパイプラインステートの作成に失敗します)");
            }
        }

        Core::Logger::Info(
            "DX12",
            "dxcでシェーダーをコンパイルします(シェーダーモデル " + Core::WideToUtf8(ToProfileSuffix(m_ShaderModel)) + ")");
        return true;
    }

    Microsoft::WRL::ComPtr<ID3DBlob> DX12ShaderCompiler::Compile(
        const std::wstring& filePath, const std::string& entryPoint, ShaderStage stage)
    {
        if (!IsAvailable())
        {
            Core::Logger::Error("DX12", "dxcが利用できない状態でCompileが呼ばれました");
            return nullptr;
        }

        std::vector<char> source;
        if (!ReadFileBytes(filePath, source))
        {
            Core::Logger::Error("DX12", "シェーダファイルを開けませんでした: " + Core::WideToUtf8(filePath));
            return nullptr;
        }

        // BOM無しUTF-8として明示的に読み込む(Utf8IncludeHandlerのコメント参照)
        Microsoft::WRL::ComPtr<IDxcBlobEncoding> sourceBlob;
        if (FAILED(m_Library->CreateBlobWithEncodingOnHeapCopy(
                source.empty() ? "" : source.data(), static_cast<UINT32>(source.size()), CP_UTF8, &sourceBlob)))
        {
            Core::Logger::Error("DX12", "シェーダソースのブロブ生成に失敗しました: " + Core::WideToUtf8(filePath));
            return nullptr;
        }

        const std::filesystem::path shaderPath(filePath);
        const std::wstring includeDirectory = shaderPath.parent_path().wstring();
        Utf8IncludeHandler includeHandler(m_Library.Get(), shaderPath.parent_path());

        const std::wstring target = std::wstring(GetProfilePrefix(stage)) + L"_" + ToProfileSuffix(m_ShaderModel);
        const std::wstring entryPointWide = Core::Utf8ToWide(entryPoint);

        std::vector<const wchar_t*> arguments;
        // HLSLの言語バージョンを2018に固定する。dxcは将来のバージョンで既定を2021以降へ
        // 引き上げるため、明示しないと配布するdxcompiler.dllを差し替えただけで
        // 既存シェーダーの意味が変わりうる
        arguments.push_back(L"-HV");
        arguments.push_back(L"2018");
        arguments.push_back(L"-I");
        arguments.push_back(includeDirectory.c_str());
#if defined(_DEBUG)
        // -Ziのデバッグ情報は既定では別ブロブになるため、-Qembed_debugでバイトコードへ埋め込む
        // (d3dcompiler経路のD3DCOMPILE_DEBUGと同じく、PIX等でシェーダーソースを追えるようにする)
        arguments.push_back(L"-Zi");
        arguments.push_back(L"-Qembed_debug");
        arguments.push_back(L"-Od");
#else
        arguments.push_back(L"-O3");
#endif

        // bindless(ResourceDescriptorHeap)が使えるかをシェーダー側へ伝える。
        // Shaders/3D/Bindless.hlsliがこのマクロで実装を切り替え、定義されていない場合は
        // 「常に無効なディスクリプタ番号を返す」実装になるため、SM 6.5以下でも
        // 同じシェーダーソースがコンパイルできる
        std::vector<DxcDefine> defines;
        if (SupportsBindless())
        {
            defines.push_back(DxcDefine{ L"KURENAI_BINDLESS", L"1" });
        }

        Microsoft::WRL::ComPtr<IDxcOperationResult> operationResult;
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

        const std::string context =
            Core::WideToUtf8(shaderPath.filename().wstring()) + " (" + entryPoint + ", " + Core::WideToUtf8(target) + ")";

        if (FAILED(compileHr) || !operationResult)
        {
            Core::Logger::Error("DX12", "シェーダのコンパイル呼び出しに失敗しました: " + context);
            return nullptr;
        }

        // 成功時でも警告(dxil.dllが無い場合の未署名警告など)が入ることがあるため、
        // 状態に関わらずエラーバッファの内容を確認する
        std::string diagnostics;
        Microsoft::WRL::ComPtr<IDxcBlobEncoding> errorBlob;
        if (SUCCEEDED(operationResult->GetErrorBuffer(&errorBlob)) && errorBlob && errorBlob->GetBufferSize() > 0)
        {
            // GetErrorBufferはコンパイラの文字コード設定に依存するため、UTF-8へ正規化してから読む
            Microsoft::WRL::ComPtr<IDxcBlobEncoding> utf8Errors;
            if (SUCCEEDED(m_Library->GetBlobAsUtf8(errorBlob.Get(), &utf8Errors)) && utf8Errors)
            {
                diagnostics.assign(static_cast<const char*>(utf8Errors->GetBufferPointer()), utf8Errors->GetBufferSize());
            }
            else
            {
                diagnostics.assign(static_cast<const char*>(errorBlob->GetBufferPointer()), errorBlob->GetBufferSize());
            }
            // 末尾の改行と、UTF-8化で付くことがある終端のヌル文字を落とす
            while (!diagnostics.empty() && (diagnostics.back() == '\0' || diagnostics.back() == '\n' || diagnostics.back() == '\r'))
            {
                diagnostics.pop_back();
            }
        }

        HRESULT status = E_FAIL;
        if (FAILED(operationResult->GetStatus(&status)) || FAILED(status))
        {
            std::string message = "シェーダのコンパイルに失敗しました: " + context;
            if (!diagnostics.empty())
            {
                message += "\n" + diagnostics;
            }
            Core::Logger::Error("DX12", message);
            return nullptr;
        }

        if (!diagnostics.empty())
        {
            Core::Logger::Warning("DX12", "シェーダのコンパイル警告: " + context + "\n" + diagnostics);
        }

        Microsoft::WRL::ComPtr<IDxcBlob> dxcBytecode;
        if (FAILED(operationResult->GetResult(&dxcBytecode)) || !dxcBytecode)
        {
            Core::Logger::Error("DX12", "シェーダのバイトコードを取得できませんでした: " + context);
            return nullptr;
        }

        // IDxcBlobはID3DBlob(ID3D10Blob)と同じIIDの別名のため、QueryInterfaceで取り出せる
        Microsoft::WRL::ComPtr<ID3DBlob> bytecode;
        if (FAILED(dxcBytecode.As(&bytecode)) || !bytecode)
        {
            Core::Logger::Error("DX12", "シェーダのバイトコードをID3DBlobへ変換できませんでした: " + context);
            return nullptr;
        }

        return bytecode;
    }
}
