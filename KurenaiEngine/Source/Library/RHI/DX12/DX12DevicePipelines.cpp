#include "DX12Device.h"
#include "DX12DeviceInternal.h"

#include <d3dx12.h>

#include <stdexcept>
#include <string>
#include <vector>

#include "DX12ComputePipelineState.h"
#include "DX12PipelineState.h"
#include "DX12Shader.h"
#include "DX12Util.h"
#include "Core/StringUtil.h"
#include "RHI/DXGIFormatUtil.h"
#include "RHI/PipelineStateNormalize.h"
#include "RHI/RHIShaderPackage.h"

// ルートシグネチャ・コマンドシグネチャ・パイプラインステートの生成。
// DX12Device のメンバ関数のまま、翻訳単位だけをここへ分けている
// (宣言は DX12Device.h のまま。IRHIDevice のインターフェースは1行も変えていない)
namespace Kurenai::RHI
{
    using namespace DX12Internal;

    D3D12_ROOT_SIGNATURE_FLAGS DX12Device::GetBindlessRootSignatureFlags() const
    {
        // シェーダーがResourceDescriptorHeapで直接ヒープを添字するには、ルートシグネチャが
        // 明示的にそれを許可している必要がある。
        //
        // 【対応環境でのみ立てる】このフラグはSM 6.6と同時に追加されたもので、
        // 古いD3D12ランタイムは未知のフラグとしてシリアライズを失敗させる。
        // bindlessが使えない環境でも起動できるよう、判定結果を見てから立てる。
        // SAMPLER_HEAP_DIRECTLY_INDEXEDは使っていない(サンプラーは従来どおり
        // s0〜s3の固定スロットで足りており、動的に選びたい場面が無いため)
        return m_SupportsBindless ? kRootSignatureFlagCbvSrvUavHeapDirectlyIndexed
                                  : D3D12_ROOT_SIGNATURE_FLAG_NONE;
    }

    void DX12Device::CreateMeshRootSignature()
    {
        if (!m_SupportsMeshShader)
        {
            return;
        }

        // レイアウトはグラフィックス用と同じ(b0/b1 + SRVテーブル + サンプラーテーブル)だが、
        // 2点だけ異なる:
        //
        // 1. SRV/サンプラーテーブルの可視性がPIXELではなくALL。増幅シェーダー・
        //    メッシュシェーダーもピクセルシェーダーと同じサンプラーを使い、
        //    ピクセルシェーダー側のマテリアルテクスチャのバインドはそのまま流用したいため。
        // 2. ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUTを立てない。メッシュシェーダーパイプラインには
        //    入力アセンブラが存在せず、このフラグを立てるとPSOの作成が失敗する。
        //
        // ジオメトリ(頂点・メッシュレット各バッファ)はSRVテーブルではなくbindlessで引くため、
        // テーブルのスロット数を増やす必要は無い
        CD3DX12_DESCRIPTOR_RANGE srvRange;
        srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kTextureSlotCount, 0);

        CD3DX12_DESCRIPTOR_RANGE samplerRange;
        samplerRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, kSamplerSlotCount, 0);

        CD3DX12_ROOT_PARAMETER rootParams[4];
        rootParams[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_ALL);
        rootParams[1].InitAsConstantBufferView(1, 0, D3D12_SHADER_VISIBILITY_ALL);
        rootParams[2].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_ALL);
        rootParams[3].InitAsDescriptorTable(1, &samplerRange, D3D12_SHADER_VISIBILITY_ALL);

        CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc;
        rootSigDesc.Init(4, rootParams, 0, nullptr, GetBindlessRootSignatureFlags());

        Microsoft::WRL::ComPtr<ID3DBlob> signatureBlob;
        Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;
        const HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signatureBlob, &errorBlob);
        if (FAILED(hr))
        {
            std::string message = "メッシュシェーダー用ルートシグネチャのシリアライズに失敗しました";
            if (errorBlob)
            {
                message += ": ";
                message += static_cast<const char*>(errorBlob->GetBufferPointer());
            }
            // ここで例外を投げるとメッシュシェーダー非対応環境と同じ状況で起動できなくなる。
            // 従来の頂点シェーダー描画へ縮退させれば動作は続けられるため、警告に留める
            Core::Logger::Warning("DX12", message + " (メッシュシェーダーを無効にします)");
            m_SupportsMeshShader = false;
            return;
        }

        if (FAILED(m_Device->CreateRootSignature(
                0, signatureBlob->GetBufferPointer(), signatureBlob->GetBufferSize(), IID_PPV_ARGS(&m_MeshRootSignature))))
        {
            Core::Logger::Warning(
                "DX12", "メッシュシェーダー用ルートシグネチャの作成に失敗しました(メッシュシェーダーを無効にします)");
            m_MeshRootSignature.Reset();
            m_SupportsMeshShader = false;
        }
    }

    void DX12Device::CreateRootSignature()
    {
        CD3DX12_DESCRIPTOR_RANGE srvRange;
        srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kTextureSlotCount, 0);

        CD3DX12_DESCRIPTOR_RANGE samplerRange;
        samplerRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, kSamplerSlotCount, 0);

        CD3DX12_ROOT_PARAMETER rootParams[5];
        rootParams[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_ALL);
        rootParams[1].InitAsConstantBufferView(1, 0, D3D12_SHADER_VISIBILITY_ALL);
        rootParams[2].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);
        rootParams[3].InitAsDescriptorTable(1, &samplerRange, D3D12_SHADER_VISIBILITY_PIXEL);
        // 頂点シェーダ専用のStructuredBuffer(IRHICommandList::SetVertexShaderResourceBuffer)。
        //
        // 【なぜディスクリプタテーブルではなくルートSRVなのか】用途が「1回のDrawで参照する
        // 構造化バッファ1本」に限られるため、ディスクリプタヒープへブロックを払い出して
        // CopyDescriptorsする経路(rootParams[2]がやっていること)を通す必要がない。
        // ルートSRVならGPU仮想アドレスを直接書き込むだけで済み、ヒープの割り当ても
        // Draw直前のフラッシュも不要になる。
        //
        // 【t0がrootParams[2]のレンジ(t0〜t20)と重なるが衝突しない理由】rootParams[2]は
        // D3D12_SHADER_VISIBILITY_PIXEL、こちらはD3D12_SHADER_VISIBILITY_VERTEXで、
        // 可視ステージが素で分離している。ルートシグネチャの一意性はシェーダーステージごとに
        // 判定されるため、頂点シェーダから見えるt0はこのルートSRVだけ、ピクセルシェーダから
        // 見えるt0はテーブル側だけになる
        rootParams[4].InitAsShaderResourceView(0, 0, D3D12_SHADER_VISIBILITY_VERTEX);

        CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc;
        rootSigDesc.Init(
            5, rootParams, 0, nullptr,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | GetBindlessRootSignatureFlags());

        Microsoft::WRL::ComPtr<ID3DBlob> signatureBlob;
        Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;
        HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signatureBlob, &errorBlob);
        if (FAILED(hr))
        {
            std::string message = "ルートシグネチャのシリアライズに失敗しました";
            if (errorBlob)
            {
                message += ": ";
                message += static_cast<const char*>(errorBlob->GetBufferPointer());
            }
            Core::Logger::Error("DX12", message);
            throw std::runtime_error(message);
        }

        ThrowIfFailed(
            m_Device->CreateRootSignature(0, signatureBlob->GetBufferPointer(), signatureBlob->GetBufferSize(), IID_PPV_ARGS(&m_RootSignature)),
            "ルートシグネチャの作成に失敗しました");
    }

    void DX12Device::CreateComputeRootSignature()
    {
        // グラフィックス用ルートシグネチャはSRV/サンプラーテーブルがピクセルシェーダのみ可視だが、
        // コンピュートシェーダーはそれとは別のパイプラインステージのため、専用のルートシグネチャを
        // ALL可視(実質コンピュートのみ)で用意する。SRV(t0〜)・UAV(u0〜)は1つのディスクリプタテーブルに
        // まとめ、m_ShaderVisibleSrvHeap上の連続した区画へCopyDescriptorsする(DX12CommandList参照)
        CD3DX12_DESCRIPTOR_RANGE ranges[2];
        ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kComputeSrvSlotCount, 0);
        ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, kComputeUavSlotCount, 0);

        // サンプラーはグラフィックス側と同じs0固定の共有ヒープ(m_ShaderVisibleSamplerHeap)をそのまま使う
        CD3DX12_DESCRIPTOR_RANGE samplerRange;
        samplerRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, kSamplerSlotCount, 0);

        CD3DX12_ROOT_PARAMETER rootParams[4];
        rootParams[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_ALL);
        rootParams[1].InitAsConstantBufferView(1, 0, D3D12_SHADER_VISIBILITY_ALL);
        rootParams[2].InitAsDescriptorTable(2, ranges, D3D12_SHADER_VISIBILITY_ALL);
        rootParams[3].InitAsDescriptorTable(1, &samplerRange, D3D12_SHADER_VISIBILITY_ALL);

        CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc;
        rootSigDesc.Init(4, rootParams, 0, nullptr, GetBindlessRootSignatureFlags());

        Microsoft::WRL::ComPtr<ID3DBlob> signatureBlob;
        Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;
        HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signatureBlob, &errorBlob);
        if (FAILED(hr))
        {
            std::string message = "コンピュート用ルートシグネチャのシリアライズに失敗しました";
            if (errorBlob)
            {
                message += ": ";
                message += static_cast<const char*>(errorBlob->GetBufferPointer());
            }
            Core::Logger::Error("DX12", message);
            throw std::runtime_error(message);
        }

        ThrowIfFailed(
            m_Device->CreateRootSignature(0, signatureBlob->GetBufferPointer(), signatureBlob->GetBufferSize(), IID_PPV_ARGS(&m_ComputeRootSignature)),
            "コンピュート用ルートシグネチャの作成に失敗しました");
    }

    void DX12Device::CreateDispatchCommandSignature()
    {
        // 引数はuint3(スレッドグループ数X/Y/Z)1個だけ。ルートシグネチャの内容を
        // 引数バッファから変更しない(ルート定数もビューも含めない)ため、
        // CreateCommandSignatureへ渡すルートシグネチャはnullptrでよい。
        // ByteStrideはD3D12_DISPATCH_ARGUMENTSと同じ12バイト
        D3D12_INDIRECT_ARGUMENT_DESC argumentDesc{};
        argumentDesc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;

        D3D12_COMMAND_SIGNATURE_DESC signatureDesc{};
        signatureDesc.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
        signatureDesc.NumArgumentDescs = 1;
        signatureDesc.pArgumentDescs = &argumentDesc;

        ThrowIfFailed(
            m_Device->CreateCommandSignature(&signatureDesc, nullptr, IID_PPV_ARGS(&m_DispatchCommandSignature)),
            "間接ディスパッチ用コマンドシグネチャの作成に失敗しました");
    }

    void DX12Device::CreateDispatchMeshCommandSignature()
    {
        // メッシュシェーダーが無ければ間接起動する相手がいない。
        // CreateMeshRootSignatureがルートシグネチャの作成に失敗した場合も
        // そこでm_SupportsMeshShaderが降りているので、ここは呼ばれても素通りする
        if (!m_SupportsMeshShader || !m_MeshRootSignature)
        {
            return;
        }

        // 引数は「ルート定数バッファビュー(b1)」と「DispatchMeshのスレッドグループ数」の2つ。
        //
        // 【ルートシグネチャを渡す必要がある】引数バッファがルートシグネチャの内容
        // (ここではルートパラメータ1のCBV)を書き換えるため、DispatchIndirect側のように
        // nullptrでは作成できない。渡すのは実際に描画で使うメッシュ用ルートシグネチャで、
        // これと違うPSOでExecuteIndirectするとデバッグレイヤが弾く
        D3D12_INDIRECT_ARGUMENT_DESC argumentDescs[2]{};
        argumentDescs[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW;
        argumentDescs[0].ConstantBufferView.RootParameterIndex = kRootParamObjectConstants;
        argumentDescs[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH;

        // 8(CBVのGPU仮想アドレス) + 12(uint3) = 20バイトだが、24へ切り上げる。
        // 【切り上げる理由】次の要素の先頭に来るGPU仮想アドレスを8バイト境界へ載せるため。
        // ByteStrideは引数の合計より大きくてよい(隙間は読まれない)
        D3D12_COMMAND_SIGNATURE_DESC signatureDesc{};
        signatureDesc.ByteStride = IRHICommandList::kDispatchMeshIndirectArgStride;
        signatureDesc.NumArgumentDescs = 2;
        signatureDesc.pArgumentDescs = argumentDescs;

        const HRESULT hr = m_Device->CreateCommandSignature(
            &signatureDesc, m_MeshRootSignature.Get(), IID_PPV_ARGS(&m_DispatchMeshCommandSignature));
        if (FAILED(hr))
        {
            // ここで落とすとメッシュシェーダー描画そのものが止まる。間接起動を諦めれば
            // 従来のCPUループで描けるため、警告に留めてnullptrのままにする
            // (DX12Device::SupportsIndirectDispatchMeshがfalseを返し、呼び出し側が縮退する)
            Core::Logger::Warning(
                "DX12",
                std::string("間接DispatchMesh用コマンドシグネチャの作成に失敗しました") +
                    "(間接描画を無効にします) hr=" + std::to_string(static_cast<long>(hr)));
            m_DispatchMeshCommandSignature.Reset();
        }
    }

    std::unique_ptr<IRHIShader> DX12Device::CreateShader(const ShaderDesc& desc)
    {
        // 使うバリアントはDetectShaderModelAndSelectVariantが起動時に1つ決めている。
        // ここでは選び直さない(シェーダーごとに段が変わると、bindlessの有無が
        // パスによって食い違うことになる)
        std::vector<uint8_t> bytecode = LoadShaderBytecode(m_ShaderPackages, desc, m_ShaderVariant, "DX12");
        return std::make_unique<DX12Shader>(desc.Stage, std::move(bytecode));
    }

    std::unique_ptr<IRHIPipelineState> DX12Device::CreatePipelineState(const PipelineStateDesc& desc)
    {
        auto* vertexShader = static_cast<DX12Shader*>(desc.VertexShader);
        auto* pixelShader = static_cast<DX12Shader*>(desc.PixelShader);

        std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
        elements.reserve(desc.InputLayout.size());
        for (const auto& element : desc.InputLayout)
        {
            D3D12_INPUT_ELEMENT_DESC elementDesc{};
            elementDesc.SemanticName = element.SemanticName.c_str();
            elementDesc.SemanticIndex = element.SemanticIndex;
            elementDesc.Format = ToDXGIFormat(element.Format);
            elementDesc.InputSlot = 0;
            elementDesc.AlignedByteOffset = element.AlignedByteOffset;
            elementDesc.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
            elementDesc.InstanceDataStepRate = 0;
            elements.push_back(elementDesc);
        }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = m_RootSignature.Get();
        psoDesc.VS = vertexShader->GetBytecode();
        // ピクセルシェーダーを持たないパイプライン(深度プリパス)は空のバイトコードを渡す。
        // DX12はこれをピクセルシェーダー段なしとして扱う
        psoDesc.PS = pixelShader ? pixelShader->GetBytecode() : D3D12_SHADER_BYTECODE{ nullptr, 0 };
        psoDesc.InputLayout = { elements.empty() ? nullptr : elements.data(), static_cast<UINT>(elements.size()) };
        psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        // 既定は「時計回りが表・裏面カリング」。ミラーリング(負のスケール)を含むインスタンスは
        // スクリーン上での三角形の向きが反転するため、表裏の判定を入れ替えたPSOで描く
        // (RHIDesc.hのFrontCounterClockwise、docs/Architecture.html 10.2節)
        psoDesc.RasterizerState.FrontCounterClockwise = desc.FrontCounterClockwise ? TRUE : FALSE;
        psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        ApplyBlendMode(psoDesc.BlendState.RenderTarget[0], desc.BlendMode);
        psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        psoDesc.DepthStencilState.DepthEnable = desc.HasDepthStencil ? TRUE : FALSE;
        psoDesc.DepthStencilState.DepthWriteMask = desc.DepthWriteEnabled ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        // Reverse-Z: 近平面=1.0/遠平面=0.0にマッピングするため、深度テストの向きもGREATERに反転する
        psoDesc.DepthStencilState.DepthFunc =
            ToD3D12Comparison(NormalizeDepthCompare(desc.ReverseZ, desc.DepthAllowEqual));
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = static_cast<UINT>(desc.RenderTargetFormats.size());
        for (size_t i = 0; i < desc.RenderTargetFormats.size(); ++i)
        {
            psoDesc.RTVFormats[i] = ToDXGIFormat(desc.RenderTargetFormats[i]);
        }
        // 実際にDSVがバインドされる描画では、深度テストの有無に関わらずフォーマットを申告する必要がある
        // (エンジン内の深度バッファはオフスクリーン・スワップチェインともD32_FLOATで統一している)
        psoDesc.DSVFormat = (desc.HasDepthStencil || desc.DepthTargetAttached) ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_UNKNOWN;
        psoDesc.SampleDesc.Count = 1;

        Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
        ThrowIfFailed(m_Device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso)), "パイプラインステートの作成に失敗しました");

        return std::make_unique<DX12PipelineState>(pso, desc.Topology);
    }

    std::unique_ptr<IRHIPipelineState> DX12Device::CreateMeshPipelineState(const MeshPipelineStateDesc& desc)
    {
        if (!m_SupportsMeshShader || !m_Device2 || !m_MeshRootSignature)
        {
            Core::Logger::Error("DX12", "メッシュシェーダー非対応の環境でCreateMeshPipelineStateが呼ばれました");
            return nullptr;
        }
        if (!desc.MeshShader)
        {
            Core::Logger::Error("DX12", "CreateMeshPipelineStateにメッシュシェーダーが指定されていません");
            return nullptr;
        }

        // メッシュシェーダーPSOはD3D12_GRAPHICS_PIPELINE_STATE_DESCでは表現できない
        // (AS/MSのサブオブジェクトが定義されていない)。ID3D12Device2で追加された
        // 「パイプラインステートストリーム」= サブオブジェクトを型タグ付きで並べた構造体を渡す形式を使う。
        // 並び順に決まりは無く、必要なものだけを列挙すればよい
        struct MeshPipelineStateStream
        {
            CD3DX12_PIPELINE_STATE_STREAM_ROOT_SIGNATURE RootSignature;
            CD3DX12_PIPELINE_STATE_STREAM_AS AS;
            CD3DX12_PIPELINE_STATE_STREAM_MS MS;
            CD3DX12_PIPELINE_STATE_STREAM_PS PS;
            CD3DX12_PIPELINE_STATE_STREAM_RASTERIZER Rasterizer;
            CD3DX12_PIPELINE_STATE_STREAM_BLEND_DESC Blend;
            CD3DX12_PIPELINE_STATE_STREAM_DEPTH_STENCIL DepthStencil;
            CD3DX12_PIPELINE_STATE_STREAM_DEPTH_STENCIL_FORMAT DSVFormat;
            CD3DX12_PIPELINE_STATE_STREAM_RENDER_TARGET_FORMATS RTVFormats;
            CD3DX12_PIPELINE_STATE_STREAM_SAMPLE_DESC SampleDesc;
            CD3DX12_PIPELINE_STATE_STREAM_SAMPLE_MASK SampleMask;
        };

        // 以降のラスタライザ・ブレンド・深度・RTVフォーマットの決め方は
        // CreatePipelineStateとまったく同じ(同じG-Bufferへ書くパスを2通りの経路で
        // 切り替えられるようにするため、ここがずれると見た目が変わってしまう)
        CD3DX12_RASTERIZER_DESC rasterizer(D3D12_DEFAULT);
        rasterizer.FrontCounterClockwise = desc.FrontCounterClockwise ? TRUE : FALSE;

        CD3DX12_BLEND_DESC blend(D3D12_DEFAULT);
        ApplyBlendMode(blend.RenderTarget[0], desc.BlendMode);

        CD3DX12_DEPTH_STENCIL_DESC depthStencil(D3D12_DEFAULT);
        depthStencil.DepthEnable = desc.HasDepthStencil ? TRUE : FALSE;
        depthStencil.DepthWriteMask = desc.DepthWriteEnabled ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        depthStencil.DepthFunc = ToD3D12Comparison(NormalizeDepthCompare(desc.ReverseZ, desc.DepthAllowEqual));

        D3D12_RT_FORMAT_ARRAY rtvFormats{};
        rtvFormats.NumRenderTargets = static_cast<UINT>(desc.RenderTargetFormats.size());
        for (size_t i = 0; i < desc.RenderTargetFormats.size(); ++i)
        {
            rtvFormats.RTFormats[i] = ToDXGIFormat(desc.RenderTargetFormats[i]);
        }

        auto* meshShader = static_cast<DX12Shader*>(desc.MeshShader);
        auto* pixelShader = static_cast<DX12Shader*>(desc.PixelShader);
        auto* amplificationShader = static_cast<DX12Shader*>(desc.AmplificationShader);

        MeshPipelineStateStream stream{};
        stream.RootSignature = m_MeshRootSignature.Get();
        // 増幅シェーダーは任意。指定が無い場合は空のバイトコードを置く
        // (サブオブジェクト自体を省く必要はなく、長さ0なら「無し」として扱われる)
        stream.AS = amplificationShader ? amplificationShader->GetBytecode() : D3D12_SHADER_BYTECODE{ nullptr, 0 };
        stream.MS = meshShader->GetBytecode();
        // ピクセルシェーダーも任意。深度だけを書くパス(深度プリパスの不透明ぶん・シャドウ)は
        // 段ごと省きたいので、長さ0のバイトコードを置いて「無し」にする
        // (CreatePipelineStateがPixelShader=nullptrを同じ扱いにしているのに揃える)
        stream.PS = pixelShader ? pixelShader->GetBytecode() : D3D12_SHADER_BYTECODE{ nullptr, 0 };
        stream.Rasterizer = rasterizer;
        stream.Blend = blend;
        stream.DepthStencil = depthStencil;
        stream.DSVFormat = (desc.HasDepthStencil || desc.DepthTargetAttached) ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_UNKNOWN;
        stream.RTVFormats = rtvFormats;
        stream.SampleDesc = DXGI_SAMPLE_DESC{ 1, 0 };
        stream.SampleMask = UINT_MAX;

        D3D12_PIPELINE_STATE_STREAM_DESC streamDesc{};
        streamDesc.SizeInBytes = sizeof(stream);
        streamDesc.pPipelineStateSubobjectStream = &stream;

        Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
        if (FAILED(m_Device2->CreatePipelineState(&streamDesc, IID_PPV_ARGS(&pso))))
        {
            // 従来の頂点シェーダー描画へ縮退できるため、例外ではなくnullptrを返す
            Core::Logger::Error("DX12", "メッシュシェーダーパイプラインステートの作成に失敗しました");
            return nullptr;
        }

        // トポロジはメッシュシェーダーの[outputtopology]属性が決めるため、ここで渡す値は使われない
        // (DX12CommandList::SetPipelineStateがIASetPrimitiveTopologyを呼ばない)
        return std::make_unique<DX12PipelineState>(pso, PrimitiveTopology::TriangleList, /*isMeshPipeline*/ true);
    }

    std::unique_ptr<IRHIPipelineState> DX12Device::CreateComputePipelineState(const ComputePipelineStateDesc& desc)
    {
        auto* computeShader = static_cast<DX12Shader*>(desc.ComputeShader);

        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = m_ComputeRootSignature.Get();
        psoDesc.CS = computeShader->GetBytecode();

        Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
        ThrowIfFailed(m_Device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pso)), "コンピュートパイプラインステートの作成に失敗しました");

        return std::make_unique<DX12ComputePipelineState>(pso);
    }
}
