#pragma once

#include <d3d12.h>
#include <d3dcommon.h>
#include <string>
#include <wrl/client.h>

#include "RHI/RHIEnums.h"

struct IDxcLibrary;
struct IDxcCompiler;

namespace Kurenai::RHI
{
    // dxc(DirectX Shader Compiler)を動的ロードし、HLSLをDXIL(シェーダーモデル6.x)へ
    // コンパイルするラッパー。DX12バックエンド専用で、DX11はd3dcompiler/SM 5.0のまま。
    //
    // dxcを使う理由はインラインレイトレーシング(HLSLのRayQuery)がSM 6.5以上でしか
    // 使えないため。d3dcompilerはSM 5.1までしか出力できず、SM 6.x以降はdxcが唯一の経路になる。
    //
    // dxcompiler.dllを静的リンク(dxcompiler.lib)ではなくLoadLibraryWで動的にロードするのは、
    // このDLLがOSに標準で入っておらず実行ファイルと同じフォルダへ配布する必要があるため。
    // 静的リンクだとDLLが無いだけでプロセスが起動できなくなるが、動的ロードなら
    // 「ログを残してd3dcompiler/SM 5.0へフォールバックする」という縮退運転ができる
    // (その場合レイトレーシングは無効になる。DX12Device::DetectRaytracingSupport参照)
    class DX12ShaderCompiler
    {
    public:
        DX12ShaderCompiler();
        ~DX12ShaderCompiler();

        DX12ShaderCompiler(const DX12ShaderCompiler&) = delete;
        DX12ShaderCompiler& operator=(const DX12ShaderCompiler&) = delete;

        // dxcompiler.dllのロードとCOMオブジェクトの生成を行う。highestShaderModelには
        // D3D12_FEATURE_SHADER_MODELで実測したデバイスの上限を渡す(これがSM 6.0未満なら
        // dxcが使えても意味がないため失敗扱いにする)。
        // 戻り値がfalseでも例外は投げない。理由は必ずログへ残す
        bool Initialize(D3D_SHADER_MODEL highestShaderModel);

        bool IsAvailable() const { return m_Compiler != nullptr; }

        // 実際にコンパイルへ使うシェーダーモデル(例: D3D_SHADER_MODEL_6_5)。
        // Initialize前およびInitializeに失敗した場合は0
        D3D_SHADER_MODEL GetShaderModel() const { return m_ShaderModel; }

        // bindless(HLSLのResourceDescriptorHeap)を含むシェーダーをコンパイルできるか。
        // SM 6.6が必要で、これはデバイスの対応状況とdxcompiler.dllのバージョンの
        // 両方で決まる(Initializeのコメント参照)。
        // trueのときだけCompileが -D KURENAI_BINDLESS=1 を渡す
        bool SupportsBindless() const { return m_ShaderModel >= D3D_SHADER_MODEL_6_6; }

        // メッシュシェーダー(as/msプロファイル)をコンパイルできるか。SM 6.5が下限。
        // 【デバイスがメッシュシェーダーに対応しているかは別問題】こちらはあくまで
        // 「コンパイラが投げられるか」で、実行できるかはD3D12_FEATURE_D3D12_OPTIONS7が決める
        // (両方を見て判断するのはDX12Device::SupportsMeshShader)
        bool SupportsMeshShaderProfile() const { return m_ShaderModel >= D3D_SHADER_MODEL_6_5; }

        // HLSLファイルの1エントリポイントをDXILへコンパイルする。
        // 成功時はバイトコード、失敗時はnullptrを返す(エラー内容はログへ出す)。
        // 戻り値の型がID3DBlobなのは、IDxcBlobがID3D10Blob/ID3DBlobと同じIIDの別名として
        // 定義されており(dxcapi.hのコメント)、D3D12_SHADER_BYTECODEへそのまま渡せるため
        Microsoft::WRL::ComPtr<ID3DBlob> Compile(const std::wstring& filePath, const std::string& entryPoint, ShaderStage stage);

    private:
        // LoadLibraryWで開いたdxcompiler.dllのハンドル。デストラクタでFreeLibraryする
        HMODULE m_Module = nullptr;
        Microsoft::WRL::ComPtr<IDxcLibrary> m_Library;
        Microsoft::WRL::ComPtr<IDxcCompiler> m_Compiler;
        D3D_SHADER_MODEL m_ShaderModel = static_cast<D3D_SHADER_MODEL>(0);
    };
}
