#include "DX11Device.h"

#include <DirectXTex.h>

#include <cwchar>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "Core/StringUtil.h"
#include "DX11Buffer.h"
#include "DX11CommandList.h"
#include "DX11ComputePipelineState.h"
#include "DX11GPUProfiler.h"
#include "DX11ImGuiBackend.h"
#include "DX11PipelineState.h"
#include "DX11SamplerSet.h"
#include "DX11Shader.h"
#include "DX11SwapChain.h"
#include "DX11Texture.h"
#include "DX11Util.h"
#include "RHI/RHIShaderPackage.h"
#include "RHI/TextureImage.h"

namespace Kurenai::RHI
{
    namespace
    {
        DXGI_FORMAT ToDXGIFormat(Format format)
        {
            switch (format)
            {
            case Format::R32G32_Float:
                return DXGI_FORMAT_R32G32_FLOAT;
            case Format::R32G32B32_Float:
                return DXGI_FORMAT_R32G32B32_FLOAT;
            case Format::R8G8B8A8_UNorm:
                return DXGI_FORMAT_R8G8B8A8_UNORM;
            case Format::R32_Float:
                return DXGI_FORMAT_R32_FLOAT;
            case Format::R16G16_Float:
                return DXGI_FORMAT_R16G16_FLOAT;
            case Format::R16G16B16A16_Float:
                return DXGI_FORMAT_R16G16B16A16_FLOAT;
            case Format::R11G11B10_Float:
                return DXGI_FORMAT_R11G11B10_FLOAT;
            case Format::R32G32B32A32_Float:
            default:
                return DXGI_FORMAT_R32G32B32A32_FLOAT;
            }
        }

        UINT ToBindFlags(BufferUsage usage)
        {
            switch (usage)
            {
            case BufferUsage::Vertex:
                return D3D11_BIND_VERTEX_BUFFER;
            case BufferUsage::Index:
                return D3D11_BIND_INDEX_BUFFER;
            case BufferUsage::Constant:
            default:
                return D3D11_BIND_CONSTANT_BUFFER;
            }
        }
    }

    DX11Device::DX11Device() = default;

    DX11Device::~DX11Device() = default;

    void DX11Device::Initialize()
    {
        UINT createDeviceFlags = 0;
#if defined(_DEBUG)
        createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

        const D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        D3D_FEATURE_LEVEL selectedFeatureLevel{};

        HRESULT hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            createDeviceFlags,
            featureLevels,
            ARRAYSIZE(featureLevels),
            D3D11_SDK_VERSION,
            &m_Device,
            &selectedFeatureLevel,
            &m_Context);
        ThrowIfFailed(hr, "D3D11デバイスの作成に失敗しました");

        Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
        ThrowIfFailed(m_Device.As(&dxgiDevice), "IDXGIDeviceの取得に失敗しました");

        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        ThrowIfFailed(dxgiDevice->GetAdapter(&adapter), "DXGIアダプタの取得に失敗しました");

        ThrowIfFailed(adapter->GetParent(IID_PPV_ARGS(&m_Factory)), "DXGIファクトリの取得に失敗しました");

        // VRAM使用量の問い合わせ(QueryVideoMemoryInfo)はIDXGIAdapter3にしかない。
        // 取れなくても描画には影響しないため、警告だけ出して続行する
        if (FAILED(adapter.As(&m_Adapter)))
        {
            Core::Logger::Warning("DX11", "IDXGIAdapter3を取得できませんでした(VRAM使用量を表示できません)");
        }

        // 実行中のGPUが何かをログに残す。どのGPUで測った値なのかが分からないと性能の記録が
        // 後から比較できなくなる。診断目的の情報なので、取得に失敗しても描画は続行する
        DXGI_ADAPTER_DESC adapterDesc{};
        if (SUCCEEDED(adapter->GetDesc(&adapterDesc)))
        {
            constexpr uint64_t kBytesPerMiB = 1024ull * 1024ull;
            Core::Logger::Info(
                "DX11",
                "GPU: " + Core::WideToUtf8(adapterDesc.Description) + " (専用VRAM " +
                    std::to_string(adapterDesc.DedicatedVideoMemory / kBytesPerMiB) + "MB / 専用システムメモリ " +
                    std::to_string(adapterDesc.DedicatedSystemMemory / kBytesPerMiB) + "MB / 共有システムメモリ " +
                    std::to_string(adapterDesc.SharedSystemMemory / kBytesPerMiB) + "MB, 機能レベル " +
                    (selectedFeatureLevel == D3D_FEATURE_LEVEL_11_1 ? "11_1" : "11_0") + ")");
        }
        else
        {
            Core::Logger::Warning("DX11", "DXGIアダプタの情報を取得できませんでした(GPU名をログに残せません)");
        }

        m_ImmediateCommandList = std::make_unique<DX11CommandList>(m_Context);

        // レイトレーシング(DXR)はD3D12の機能でDX11には存在しない。上位層はこの後
        // SupportsRaytracing()を見て従来のスクリーンスペース手法へ静かにフォールバックするため、
        // 「なぜレイトレーシングが効いていないのか」を追える手がかりをここで残しておく
        Core::Logger::Info(
            "DX11", "レイトレーシング非対応: DXRはD3D12の機能です(スクリーンスペース手法で描画します)");
    }

    std::unique_ptr<IRHISwapChain> DX11Device::CreateSwapChain(void* windowHandle, uint32_t width, uint32_t height)
    {
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = width;
        desc.Height = height;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

        Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain;
        HRESULT hr = m_Factory->CreateSwapChainForHwnd(
            m_Device.Get(),
            static_cast<HWND>(windowHandle),
            &desc,
            nullptr,
            nullptr,
            &swapChain);
        ThrowIfFailed(hr, "スワップチェインの作成に失敗しました");

        return std::make_unique<DX11SwapChain>(this, swapChain, m_Device, m_Context, width, height);
    }

    std::unique_ptr<IRHIBuffer> DX11Device::CreateBuffer(const BufferDesc& desc)
    {
        // 構造化バッファ(RWStructuredBuffer)はUAV+SRVの両方を持つDEFAULTヒープに作成するため、
        // 単純なBindFlagsマッピングでは表現できず専用の経路で作成する
        if (desc.Usage == BufferUsage::Structured)
        {
            D3D11_BUFFER_DESC structuredDesc{};
            structuredDesc.ByteWidth = desc.SizeInBytes;
            structuredDesc.Usage = D3D11_USAGE_DEFAULT;
            structuredDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
            structuredDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            structuredDesc.StructureByteStride = desc.StrideInBytes;

            D3D11_SUBRESOURCE_DATA structuredInitData{};
            structuredInitData.pSysMem = desc.InitialData;

            Microsoft::WRL::ComPtr<ID3D11Buffer> structuredBuffer;
            ThrowIfFailed(
                m_Device->CreateBuffer(&structuredDesc, desc.InitialData ? &structuredInitData : nullptr, &structuredBuffer),
                "構造化バッファの作成に失敗しました");

            D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
            uavDesc.Format = DXGI_FORMAT_UNKNOWN;
            uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            uavDesc.Buffer.FirstElement = 0;
            uavDesc.Buffer.NumElements = desc.SizeInBytes / desc.StrideInBytes;

            Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav;
            ThrowIfFailed(m_Device->CreateUnorderedAccessView(structuredBuffer.Get(), &uavDesc, &uav), "アンオーダードアクセスビューの作成に失敗しました");

            return std::make_unique<DX11Buffer>(structuredBuffer, desc.StrideInBytes, uav);
        }

        // GPUが書いた値をCPUで読むための受け皿。ステージングバッファとして作る。
        // シェーダーからは見えないのでBindFlagsは0(ビューも張らない)
        if (desc.Usage == BufferUsage::Readback)
        {
            if (desc.SizeInBytes == 0)
            {
                Core::Logger::Error("DX11", "Readbackバッファのサイズが0です。作成を中止します");
                throw std::runtime_error("Readbackバッファのサイズが不正です");
            }

            D3D11_BUFFER_DESC readbackDesc{};
            readbackDesc.ByteWidth = desc.SizeInBytes;
            readbackDesc.Usage = D3D11_USAGE_STAGING;
            readbackDesc.BindFlags = 0;
            readbackDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

            Microsoft::WRL::ComPtr<ID3D11Buffer> readbackBuffer;
            ThrowIfFailed(
                m_Device->CreateBuffer(&readbackDesc, nullptr, &readbackBuffer),
                "リードバックバッファの作成に失敗しました");

            return std::make_unique<DX11Buffer>(readbackBuffer, desc.SizeInBytes, m_Context);
        }

        // 間接ディスパッチの引数バッファ。D3D11はDRAWINDIRECT_ARGSと構造化バッファを同時に
        // 指定できないため、raw(ByteAddress)バッファとして作りHLSL側もRWByteAddressBufferで受ける
        // (RHIEnums.hのBufferUsage::IndirectArgsのコメント参照)
        if (desc.Usage == BufferUsage::IndirectArgs)
        {
            // raw UAVは4バイト単位でアドレスを刻むため、サイズも4の倍数でなければ末尾が書けない
            if (desc.SizeInBytes == 0 || (desc.SizeInBytes % 4) != 0)
            {
                Core::Logger::Error("DX11", "IndirectArgsバッファのサイズが0か4の倍数ではありません。作成を中止します");
                throw std::runtime_error("IndirectArgsバッファのサイズが不正です");
            }

            D3D11_BUFFER_DESC argsDesc{};
            argsDesc.ByteWidth = desc.SizeInBytes;
            argsDesc.Usage = D3D11_USAGE_DEFAULT;
            argsDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            argsDesc.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS | D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;

            Microsoft::WRL::ComPtr<ID3D11Buffer> argsBuffer;
            ThrowIfFailed(
                m_Device->CreateBuffer(&argsDesc, nullptr, &argsBuffer),
                "間接ディスパッチ引数バッファ(IndirectArgs)の作成に失敗しました");

            D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
            uavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
            uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            uavDesc.Buffer.FirstElement = 0;
            uavDesc.Buffer.NumElements = desc.SizeInBytes / 4;
            uavDesc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;

            Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav;
            ThrowIfFailed(
                m_Device->CreateUnorderedAccessView(argsBuffer.Get(), &uavDesc, &uav),
                "間接ディスパッチ引数バッファのアンオーダードアクセスビューの作成に失敗しました");

            return std::make_unique<DX11Buffer>(argsBuffer, desc.StrideInBytes, uav, true);
        }

        // 読み取り専用の構造化バッファ(StructuredBuffer)。CPUから毎フレームUpdateBufferで書き換える前提
        // なのでD3D11_USAGE_DYNAMIC + CPU_ACCESS_WRITEで作成し、Map(WRITE_DISCARD)経由で更新する
        // (UAVを持たないためUpdateSubresourceが使えるDEFAULTヒープにする必要はない)
        if (desc.Usage == BufferUsage::StructuredReadOnly)
        {
            D3D11_BUFFER_DESC structuredDesc{};
            structuredDesc.ByteWidth = desc.SizeInBytes;
            structuredDesc.Usage = D3D11_USAGE_DYNAMIC;
            structuredDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            structuredDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            structuredDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            structuredDesc.StructureByteStride = desc.StrideInBytes;

            Microsoft::WRL::ComPtr<ID3D11Buffer> structuredBuffer;
            ThrowIfFailed(
                m_Device->CreateBuffer(&structuredDesc, nullptr, &structuredBuffer),
                "読み取り専用構造化バッファの作成に失敗しました");

            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = DXGI_FORMAT_UNKNOWN;
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            srvDesc.Buffer.FirstElement = 0;
            srvDesc.Buffer.NumElements = desc.SizeInBytes / desc.StrideInBytes;

            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
            ThrowIfFailed(
                m_Device->CreateShaderResourceView(structuredBuffer.Get(), &srvDesc, &srv),
                "読み取り専用構造化バッファのシェーダリソースビュー作成に失敗しました");

            return std::make_unique<DX11Buffer>(structuredBuffer, desc.StrideInBytes, srv, /*isDynamic=*/true);
        }

        // 作成時の初期データから変化しない読み取り専用の構造化バッファ。CPUからの書き換えが無いため
        // D3D11_USAGE_IMMUTABLE(CPUAccessFlagsなし)で作る。レイトレーシングのシーンジオメトリ用
        if (desc.Usage == BufferUsage::StructuredImmutable)
        {
            if (desc.StrideInBytes == 0)
            {
                Core::Logger::Error("DX11", "StructuredImmutableバッファのStrideInBytesが0です。作成を中止します");
                throw std::runtime_error("StructuredImmutableバッファのStrideInBytesが0です");
            }
            if (!desc.InitialData)
            {
                // D3D11_USAGE_IMMUTABLEは初期データが必須(後から書き込む手段が無い)
                Core::Logger::Error("DX11", "StructuredImmutableバッファにInitialDataが指定されていません。作成を中止します");
                throw std::runtime_error("StructuredImmutableバッファにInitialDataが指定されていません");
            }

            D3D11_BUFFER_DESC structuredDesc{};
            structuredDesc.ByteWidth = desc.SizeInBytes;
            structuredDesc.Usage = D3D11_USAGE_IMMUTABLE;
            structuredDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            structuredDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            structuredDesc.StructureByteStride = desc.StrideInBytes;

            D3D11_SUBRESOURCE_DATA initData{};
            initData.pSysMem = desc.InitialData;

            Microsoft::WRL::ComPtr<ID3D11Buffer> structuredBuffer;
            ThrowIfFailed(
                m_Device->CreateBuffer(&structuredDesc, &initData, &structuredBuffer),
                "不変構造化バッファ(StructuredImmutable)の作成に失敗しました");

            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = DXGI_FORMAT_UNKNOWN;
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            srvDesc.Buffer.FirstElement = 0;
            srvDesc.Buffer.NumElements = desc.SizeInBytes / desc.StrideInBytes;

            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
            ThrowIfFailed(
                m_Device->CreateShaderResourceView(structuredBuffer.Get(), &srvDesc, &srv),
                "不変構造化バッファのシェーダリソースビュー作成に失敗しました");

            return std::make_unique<DX11Buffer>(structuredBuffer, desc.StrideInBytes, srv, /*isDynamic=*/false, /*isImmutable=*/true);
        }

        // コンピュートがUAVで書き、ピクセルシェーダがSRVで読む構造化バッファ。CPUからは書き込まないので
        // D3D11_USAGE_DEFAULT(CPUAccessFlagsなし)。DX11は同じリソースをUAVとSRVに同時バインドできないが、
        // DX11CommandList::DispatchがDispatch直後にUAVを全解除しているため追加の対処は要らない
        if (desc.Usage == BufferUsage::StructuredRW)
        {
            if (desc.StrideInBytes == 0)
            {
                Core::Logger::Error("DX11", "StructuredRWバッファのStrideInBytesが0です。作成を中止します");
                throw std::runtime_error("StructuredRWバッファのStrideInBytesが0です");
            }

            D3D11_BUFFER_DESC structuredDesc{};
            structuredDesc.ByteWidth = desc.SizeInBytes;
            structuredDesc.Usage = D3D11_USAGE_DEFAULT;
            structuredDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
            structuredDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            structuredDesc.StructureByteStride = desc.StrideInBytes;

            Microsoft::WRL::ComPtr<ID3D11Buffer> structuredBuffer;
            ThrowIfFailed(
                m_Device->CreateBuffer(&structuredDesc, nullptr, &structuredBuffer),
                "読み書き構造化バッファ(StructuredRW)の作成に失敗しました");

            const UINT elementCount = desc.SizeInBytes / desc.StrideInBytes;

            D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
            uavDesc.Format = DXGI_FORMAT_UNKNOWN;
            uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            uavDesc.Buffer.FirstElement = 0;
            uavDesc.Buffer.NumElements = elementCount;

            Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav;
            ThrowIfFailed(
                m_Device->CreateUnorderedAccessView(structuredBuffer.Get(), &uavDesc, &uav),
                "読み書き構造化バッファのアンオーダードアクセスビュー作成に失敗しました");

            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = DXGI_FORMAT_UNKNOWN;
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            srvDesc.Buffer.FirstElement = 0;
            srvDesc.Buffer.NumElements = elementCount;

            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
            ThrowIfFailed(
                m_Device->CreateShaderResourceView(structuredBuffer.Get(), &srvDesc, &srv),
                "読み書き構造化バッファのシェーダリソースビュー作成に失敗しました");

            return std::make_unique<DX11Buffer>(structuredBuffer, desc.StrideInBytes, uav, srv);
        }

        D3D11_BUFFER_DESC bufferDesc{};
        bufferDesc.ByteWidth = desc.SizeInBytes;
        bufferDesc.BindFlags = ToBindFlags(desc.Usage);
        bufferDesc.Usage = desc.InitialData ? D3D11_USAGE_IMMUTABLE : D3D11_USAGE_DEFAULT;

        D3D11_SUBRESOURCE_DATA initData{};
        initData.pSysMem = desc.InitialData;

        Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
        HRESULT hr = m_Device->CreateBuffer(&bufferDesc, desc.InitialData ? &initData : nullptr, &buffer);
        ThrowIfFailed(hr, "バッファの作成に失敗しました");

        return std::make_unique<DX11Buffer>(buffer, desc.StrideInBytes);
    }

    std::unique_ptr<IRHIShader> DX11Device::CreateShader(const ShaderDesc& desc)
    {
        // 増幅/メッシュシェーダーにはDX11のプロファイルが存在しない。下の三項演算子は
        // 「Vertexでもcomputeでもなければpixel」という作りのため、弾かないとメッシュシェーダーを
        // ピクセルシェーダーとしてコンパイルしようとし、原因の分かりにくいエラーになる
        if (desc.Stage == ShaderStage::Amplification || desc.Stage == ShaderStage::Mesh)
        {
            Core::Logger::Error(
                "DX11",
                "CreateShader: DX11はメッシュシェーダーパイプラインに対応していません。"
                "SupportsMeshShader()で分岐してください");
            return nullptr;
        }

        // DX11が使うのは常にSM 5.0のバリアント(DXBC)。
        // このバイトコードは、以前このメソッドがD3DCompileFromFileで実行時に作っていたものと
        // 同じコンパイラ・同じフラグで、KurenaiShaderPackerがビルド時に焼いたもの
        std::vector<uint8_t> bytecode =
            LoadShaderBytecode(m_ShaderPackages, desc, Assets::ShaderVariant::Dxbc50, "DX11");

        HRESULT hr = S_OK;
        Microsoft::WRL::ComPtr<ID3D11DeviceChild> shader;
        if (desc.Stage == ShaderStage::Vertex)
        {
            Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader;
            hr = m_Device->CreateVertexShader(bytecode.data(), bytecode.size(), nullptr, &vertexShader);
            shader = vertexShader;
        }
        else if (desc.Stage == ShaderStage::Compute)
        {
            Microsoft::WRL::ComPtr<ID3D11ComputeShader> computeShader;
            hr = m_Device->CreateComputeShader(bytecode.data(), bytecode.size(), nullptr, &computeShader);
            shader = computeShader;
        }
        else
        {
            Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader;
            hr = m_Device->CreatePixelShader(bytecode.data(), bytecode.size(), nullptr, &pixelShader);
            shader = pixelShader;
        }
        ThrowIfFailed(hr, "シェーダオブジェクトの作成に失敗しました");

        return std::make_unique<DX11Shader>(desc.Stage, shader, std::move(bytecode));
    }

    std::unique_ptr<IRHIPipelineState> DX11Device::CreatePipelineState(const PipelineStateDesc& desc)
    {
        auto* vertexShader = static_cast<DX11Shader*>(desc.VertexShader);
        auto* pixelShader = static_cast<DX11Shader*>(desc.PixelShader);

        std::vector<D3D11_INPUT_ELEMENT_DESC> elements;
        elements.reserve(desc.InputLayout.size());
        for (const auto& element : desc.InputLayout)
        {
            D3D11_INPUT_ELEMENT_DESC elementDesc{};
            elementDesc.SemanticName = element.SemanticName.c_str();
            elementDesc.SemanticIndex = element.SemanticIndex;
            elementDesc.Format = ToDXGIFormat(element.Format);
            elementDesc.InputSlot = 0;
            elementDesc.AlignedByteOffset = element.AlignedByteOffset;
            elementDesc.InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
            elementDesc.InstanceDataStepRate = 0;
            elements.push_back(elementDesc);
        }

        // フルスクリーンパスなど頂点入力を持たないシェーダの場合は入力レイアウトを作成しない(IASetInputLayout(nullptr)で運用)
        Microsoft::WRL::ComPtr<ID3D11InputLayout> inputLayout;
        if (!elements.empty())
        {
            HRESULT hr = m_Device->CreateInputLayout(
                elements.data(),
                static_cast<UINT>(elements.size()),
                vertexShader->GetBytecode().data(),
                vertexShader->GetBytecode().size(),
                &inputLayout);
            ThrowIfFailed(hr, "入力レイアウトの作成に失敗しました");
        }

        // DX12はPSOごとに深度ステンシルステートを持てるが、DX11はコンテキストへの明示バインドが必要。
        // 何も設定しないとデフォルト状態(DepthEnable=TRUE, DepthFunc=LESS)になってしまうため、
        // Reverse-Z(GREATER)を使うパイプラインのぶんも含めてここで明示的に作成する
        D3D11_DEPTH_STENCIL_DESC depthStencilDesc{};
        depthStencilDesc.DepthEnable = desc.HasDepthStencil ? TRUE : FALSE;
        depthStencilDesc.DepthWriteMask = desc.DepthWriteEnabled ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        depthStencilDesc.DepthFunc = desc.ReverseZ
            ? (desc.DepthAllowEqual ? D3D11_COMPARISON_GREATER_EQUAL : D3D11_COMPARISON_GREATER)
            : (desc.DepthAllowEqual ? D3D11_COMPARISON_LESS_EQUAL : D3D11_COMPARISON_LESS);
        depthStencilDesc.StencilEnable = FALSE;

        Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthStencilState;
        ThrowIfFailed(m_Device->CreateDepthStencilState(&depthStencilDesc, &depthStencilState), "深度ステンシルステートの作成に失敗しました");

        // DX12はPSOごとにブレンドステートを持てるが、DX11はコンテキストへの明示バインドが必要なため、
        // 深度ステンシルステートと同様にここでBlendModeに応じたステートを明示的に作成する
        D3D11_BLEND_DESC blendDesc{};
        D3D11_RENDER_TARGET_BLEND_DESC& rt0 = blendDesc.RenderTarget[0];
        rt0.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        switch (desc.BlendMode)
        {
        case BlendMode::AlphaBlend:
            rt0.BlendEnable = TRUE;
            rt0.SrcBlend = D3D11_BLEND_SRC_ALPHA;
            rt0.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
            rt0.BlendOp = D3D11_BLEND_OP_ADD;
            rt0.SrcBlendAlpha = D3D11_BLEND_ONE;
            rt0.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
            rt0.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            break;
        case BlendMode::Additive:
            rt0.BlendEnable = TRUE;
            rt0.SrcBlend = D3D11_BLEND_SRC_ALPHA;
            rt0.DestBlend = D3D11_BLEND_ONE;
            rt0.BlendOp = D3D11_BLEND_OP_ADD;
            rt0.SrcBlendAlpha = D3D11_BLEND_ONE;
            rt0.DestBlendAlpha = D3D11_BLEND_ONE;
            rt0.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            break;
        case BlendMode::Multiply:
            rt0.BlendEnable = TRUE;
            rt0.SrcBlend = D3D11_BLEND_DEST_COLOR;
            rt0.DestBlend = D3D11_BLEND_ZERO;
            rt0.BlendOp = D3D11_BLEND_OP_ADD;
            rt0.SrcBlendAlpha = D3D11_BLEND_DEST_ALPHA;
            rt0.DestBlendAlpha = D3D11_BLEND_ZERO;
            rt0.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            break;
        case BlendMode::PremultipliedAlpha:
            rt0.BlendEnable = TRUE;
            rt0.SrcBlend = D3D11_BLEND_ONE;
            rt0.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
            rt0.BlendOp = D3D11_BLEND_OP_ADD;
            rt0.SrcBlendAlpha = D3D11_BLEND_ONE;
            rt0.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
            rt0.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            break;
        case BlendMode::Opaque:
        default:
            rt0.BlendEnable = FALSE;
            break;
        }

        Microsoft::WRL::ComPtr<ID3D11BlendState> blendState;
        ThrowIfFailed(m_Device->CreateBlendState(&blendDesc, &blendState), "ブレンドステートの作成に失敗しました");

        // ラスタライザステートも同様にPSO単位で持たせる。ミラーリングされたインスタンスを
        // FrontCounterClockwise=TRUEで描き分けられるように明示的に作成する。
        // CD3D11_RASTERIZER_DESC(D3D11_DEFAULT)はD3D11の暗黙の既定状態
        // (ソリッド塗り・裏面カリング・時計回りが表)と全項目一致するため、
        // FrontCounterClockwise以外の挙動は変わらない
        CD3D11_RASTERIZER_DESC rasterizerDesc(D3D11_DEFAULT);
        rasterizerDesc.FrontCounterClockwise = desc.FrontCounterClockwise ? TRUE : FALSE;
        // IRHICommandList::SetScissorRectを使えるよう常時有効にする。D3D12にはこのフラグ自体が
        // 存在せず(シザーは常時有効)、有効にすることでDX11/DX12の挙動が揃う。
        //
        // 【注意】D3D11のシザー矩形の既定は「矩形0本」であり、ScissorEnable=TRUEのまま
        // RSSetScissorRectsを一度も呼ばないと全ピクセルがクリップされて何も映らなくなる。
        // これを防ぐため、DX11CommandList::SetViewportが必ずビューポート全体の矩形を張る
        // (D3D12もコマンドリストのリセット直後は矩形0本という同じ危険を抱えており、
        //  現にDX12で描画が出ている事実が「全描画がSetViewportを経由している」ことの裏付けになる)
        rasterizerDesc.ScissorEnable = TRUE;

        Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizerState;
        ThrowIfFailed(m_Device->CreateRasterizerState(&rasterizerDesc, &rasterizerState), "ラスタライザステートの作成に失敗しました");

        return std::make_unique<DX11PipelineState>(inputLayout, vertexShader, pixelShader, desc.Topology, depthStencilState, blendState, rasterizerState);
    }

    std::unique_ptr<IRHIPipelineState> DX11Device::CreateComputePipelineState(const ComputePipelineStateDesc& desc)
    {
        auto* computeShader = static_cast<DX11Shader*>(desc.ComputeShader);
        return std::make_unique<DX11ComputePipelineState>(computeShader);
    }

    std::unique_ptr<IRHITexture> DX11Device::CreateTextureFromFile(const std::wstring& filePath, bool sRGB)
    {
        return CreateTextureFromImage(TextureImage::LoadFromFile(filePath, sRGB));
    }

    std::unique_ptr<IRHITexture> DX11Device::CreateTextureFromImage(const TextureImage& image)
    {
        const DirectX::ScratchImage& scratchImage = image.GetImage();

        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        ThrowIfFailed(
            DirectX::CreateShaderResourceView(m_Device.Get(), scratchImage.GetImages(), scratchImage.GetImageCount(), image.GetMetadata(), &srv),
            "シェーダリソースビューの作成に失敗しました");

        return std::make_unique<DX11Texture>(srv);
    }

    std::unique_ptr<IRHIPendingTextureContents> DX11Device::PrepareTextureContents(
        IRHITexture* target, const TextureImage& image)
    {
        auto* texture = static_cast<DX11Texture*>(target);
        if (texture == nullptr)
        {
            Core::Logger::Error("DX11", "PrepareTextureContents: テクスチャがnullptrです");
            return nullptr;
        }

        // 差し替えてよいのは、SRVしか持たないアセット由来のテクスチャに限る。
        // レンダーターゲット等は他のビューとの整合が取れなくなる
        if (texture->GetShaderResourceView() == nullptr || texture->HasNonSrvViews())
        {
            Core::Logger::Error("DX11", "PrepareTextureContents: SRV以外のビューを持つテクスチャは差し替えられません");
            return nullptr;
        }

        // ID3D11Deviceはフリースレッドなので、ここはワーカースレッドから呼んでよい
        // (フリースレッドでないのはImmediate Contextの方)
        const DirectX::ScratchImage& scratchImage = image.GetImage();
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        const HRESULT hr = DirectX::CreateShaderResourceView(
            m_Device.Get(), scratchImage.GetImages(), scratchImage.GetImageCount(), image.GetMetadata(), &srv);
        if (FAILED(hr))
        {
            // 失敗しても元の中身はそのまま。常駐ミップが減らないだけで絵は出続ける
            Core::Logger::Error("DX11", "PrepareTextureContents: シェーダリソースビューの作成に失敗しました");
            return nullptr;
        }

        return std::make_unique<DX11PendingTextureContents>(
            texture, std::move(srv), static_cast<uint32_t>(image.GetMetadata().mipLevels));
    }

    std::unique_ptr<IRHIPendingTextureContents> DX11Device::PrepareTiledTextureResidency(
        IRHITexture* target, const TiledTextureDesc& desc, const TextureImage& image, uint32_t firstMip)
    {
        (void)target;
        (void)desc;
        (void)image;
        (void)firstMip;
        // GetTiledResourcesTier()が0を返すため、上位層はここへ来ないのが正常
        Core::Logger::Error("DX11", "PrepareTiledTextureResidency: DX11はタイルリソースに対応していません");
        return nullptr;
    }

    bool DX11Device::GetVideoMemoryUsage(uint64_t& outUsedBytes, uint64_t& outBudgetBytes) const
    {
        if (!m_Adapter)
        {
            return false;
        }

        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
        if (FAILED(m_Adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
        {
            return false;
        }
        outUsedBytes = info.CurrentUsage;
        outBudgetBytes = info.Budget;
        return true;
    }

    bool DX11Device::CommitTextureContents(IRHIPendingTextureContents* pending)
    {
        auto* entry = static_cast<DX11PendingTextureContents*>(pending);
        if (entry == nullptr || entry->Texture == nullptr || !entry->Srv)
        {
            Core::Logger::Error("DX11", "CommitTextureContents: 差し替え待ちの内容が不正です");
            return false;
        }

        // 古いテクスチャの実体は、GPUが参照し終えるまでDX11ランタイムが破棄を遅らせる
        // (DX12のような自前の遅延解放は要らない)
        entry->Texture->SwapShaderResourceView(std::move(entry->Srv));
        return true;
    }

    std::unique_ptr<IRHITexture> DX11Device::CreateSolidColorTexture(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
    {
        const uint8_t pixel[4] = { r, g, b, a };

        D3D11_TEXTURE2D_DESC textureDesc{};
        textureDesc.Width = 1;
        textureDesc.Height = 1;
        textureDesc.MipLevels = 1;
        textureDesc.ArraySize = 1;
        textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.Usage = D3D11_USAGE_IMMUTABLE;
        textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA initData{};
        initData.pSysMem = pixel;
        initData.SysMemPitch = sizeof(pixel);

        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        ThrowIfFailed(m_Device->CreateTexture2D(&textureDesc, &initData, &texture), "テクスチャの作成に失敗しました");

        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        ThrowIfFailed(m_Device->CreateShaderResourceView(texture.Get(), nullptr, &srv), "シェーダリソースビューの作成に失敗しました");

        return std::make_unique<DX11Texture>(srv);
    }

    std::unique_ptr<IRHITexture> DX11Device::CreateTextureFromMemory(uint32_t width, uint32_t height, const void* pixelsRGBA8)
    {
        D3D11_TEXTURE2D_DESC textureDesc{};
        textureDesc.Width = width;
        textureDesc.Height = height;
        textureDesc.MipLevels = 1;
        textureDesc.ArraySize = 1;
        textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.Usage = D3D11_USAGE_IMMUTABLE;
        textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA initData{};
        initData.pSysMem = pixelsRGBA8;
        initData.SysMemPitch = width * 4;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        ThrowIfFailed(m_Device->CreateTexture2D(&textureDesc, &initData, &texture), "テクスチャの作成に失敗しました");

        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        ThrowIfFailed(m_Device->CreateShaderResourceView(texture.Get(), nullptr, &srv), "シェーダリソースビューの作成に失敗しました");

        return std::make_unique<DX11Texture>(srv);
    }

    std::unique_ptr<IRHITexture> DX11Device::CreateRenderTexture(uint32_t width, uint32_t height, Format format)
    {
        D3D11_TEXTURE2D_DESC textureDesc{};
        textureDesc.Width = width;
        textureDesc.Height = height;
        textureDesc.MipLevels = 1;
        textureDesc.ArraySize = 1;
        textureDesc.Format = ToDXGIFormat(format);
        textureDesc.SampleDesc.Count = 1;
        textureDesc.Usage = D3D11_USAGE_DEFAULT;
        textureDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        ThrowIfFailed(m_Device->CreateTexture2D(&textureDesc, nullptr, &texture), "レンダーテクスチャの作成に失敗しました");

        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
        ThrowIfFailed(m_Device->CreateRenderTargetView(texture.Get(), nullptr, &rtv), "レンダーターゲットビューの作成に失敗しました");

        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        ThrowIfFailed(m_Device->CreateShaderResourceView(texture.Get(), nullptr, &srv), "シェーダリソースビューの作成に失敗しました");

        return std::make_unique<DX11Texture>(srv, rtv, nullptr);
    }

    std::unique_ptr<IRHITexture> DX11Device::CreateUAVTexture(uint32_t width, uint32_t height, Format format)
    {
        D3D11_TEXTURE2D_DESC textureDesc{};
        textureDesc.Width = width;
        textureDesc.Height = height;
        textureDesc.MipLevels = 1;
        textureDesc.ArraySize = 1;
        textureDesc.Format = ToDXGIFormat(format);
        textureDesc.SampleDesc.Count = 1;
        textureDesc.Usage = D3D11_USAGE_DEFAULT;
        textureDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        ThrowIfFailed(m_Device->CreateTexture2D(&textureDesc, nullptr, &texture), "UAVテクスチャの作成に失敗しました");

        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        ThrowIfFailed(m_Device->CreateShaderResourceView(texture.Get(), nullptr, &srv), "シェーダリソースビューの作成に失敗しました");

        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav;
        ThrowIfFailed(m_Device->CreateUnorderedAccessView(texture.Get(), nullptr, &uav), "アンオーダードアクセスビューの作成に失敗しました");

        return std::make_unique<DX11Texture>(srv, nullptr, nullptr, uav);
    }

    std::unique_ptr<IRHITexture> DX11Device::CreateUAVTexture3D(
        uint32_t width, uint32_t height, uint32_t depth, Format format)
    {
        // CreateUAVTexture(上)の3D版。違いはD3D11_TEXTURE3D_DESCとCreateTexture3Dを使うことと、
        // 奥行き(Depth)がある代わりにArraySizeが無いことだけで、ビューの作成と
        // DX11Textureへの詰め方は2Dとまったく同じ(記述子nullptrで既定のTexture3Dビューになる)
        D3D11_TEXTURE3D_DESC textureDesc{};
        textureDesc.Width = width;
        textureDesc.Height = height;
        textureDesc.Depth = depth;
        textureDesc.MipLevels = 1;
        textureDesc.Format = ToDXGIFormat(format);
        textureDesc.Usage = D3D11_USAGE_DEFAULT;
        textureDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;

        Microsoft::WRL::ComPtr<ID3D11Texture3D> texture;
        ThrowIfFailed(
            m_Device->CreateTexture3D(&textureDesc, nullptr, &texture), "3D UAVテクスチャの作成に失敗しました");

        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        ThrowIfFailed(
            m_Device->CreateShaderResourceView(texture.Get(), nullptr, &srv),
            "3D UAVテクスチャのシェーダリソースビューの作成に失敗しました");

        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav;
        ThrowIfFailed(
            m_Device->CreateUnorderedAccessView(texture.Get(), nullptr, &uav),
            "3D UAVテクスチャのアンオーダードアクセスビューの作成に失敗しました");

        return std::make_unique<DX11Texture>(srv, nullptr, nullptr, uav);
    }

    std::unique_ptr<IRHITexture> DX11Device::CreateHiZTexture(uint32_t width, uint32_t height, uint32_t mipLevels)
    {
        return CreateMippedUAVTexture(width, height, Format::R32_Float, mipLevels);
    }

    std::unique_ptr<IRHITexture> DX11Device::CreateMippedUAVTexture(uint32_t width, uint32_t height, Format format, uint32_t mipLevels)
    {
        const DXGI_FORMAT dxgiFormat = ToDXGIFormat(format);

        D3D11_TEXTURE2D_DESC textureDesc{};
        textureDesc.Width = width;
        textureDesc.Height = height;
        textureDesc.MipLevels = mipLevels;
        textureDesc.ArraySize = 1;
        textureDesc.Format = dxgiFormat;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.Usage = D3D11_USAGE_DEFAULT;
        textureDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        ThrowIfFailed(m_Device->CreateTexture2D(&textureDesc, nullptr, &texture), "ミップ付きUAVテクスチャの作成に失敗しました");

        // 全ミップを見るSRV(nullptrで既定=全ミップ)。デバッグ表示などでSampleLevelにより任意のミップを読む用
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        ThrowIfFailed(m_Device->CreateShaderResourceView(texture.Get(), nullptr, &srv), "ミップ付きUAVシェーダリソースビューの作成に失敗しました");

        // ミップごとに単一ミップのUAVを張り、コンピュートシェーダーがミップ単位で書き込めるようにする
        // (Hi-Zの「前段ミップを読んで次段へ書く」ダウンサンプルだけでなく、IBLプリフィルタ済み鏡面マップの
        // 「ミップごとに異なるラフネスで独立に畳み込む」用途でも同じ仕組みを再利用する)
        std::vector<Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>> mipUavs;
        mipUavs.reserve(mipLevels);
        for (uint32_t mip = 0; mip < mipLevels; ++mip)
        {
            D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
            uavDesc.Format = dxgiFormat;
            uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
            uavDesc.Texture2D.MipSlice = mip;

            Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav;
            ThrowIfFailed(m_Device->CreateUnorderedAccessView(texture.Get(), &uavDesc, &uav), "ミップ付きUAVアンオーダードアクセスビューの作成に失敗しました");
            mipUavs.push_back(std::move(uav));
        }

        return std::make_unique<DX11Texture>(srv, nullptr, nullptr, nullptr, std::move(mipUavs));
    }

    std::unique_ptr<IRHITexture> DX11Device::CreateUAVTextureCube(uint32_t size, Format format)
    {
        return CreateMippedUAVTextureCube(size, format, 1);
    }

    std::unique_ptr<IRHITexture> DX11Device::CreateMippedUAVTextureCube(uint32_t size, Format format, uint32_t mipLevels)
    {
        // cubeCount=1のときはSRVをTextureCubeArrayではなくTextureCubeとして張る(HLSL側の
        // TextureCube宣言と一致させるため。IBLConvolve.hlsl等)
        return CreateCubeTextureInternal(size, format, mipLevels, 1, false);
    }

    std::unique_ptr<IRHITexture> DX11Device::CreateMippedUAVTextureCubeArray(
        uint32_t size, Format format, uint32_t mipLevels, uint32_t cubeCount)
    {
        return CreateCubeTextureInternal(size, format, mipLevels, cubeCount, true);
    }

    std::unique_ptr<IRHITexture> DX11Device::CreateCubeTextureInternal(
        uint32_t size, Format format, uint32_t mipLevels, uint32_t cubeCount, bool asArray)
    {
        if (size == 0 || mipLevels == 0 || cubeCount == 0)
        {
            const std::string message =
                "キューブマップUAVテクスチャの作成に失敗しました: サイズ・ミップ数・キューブ数はいずれも1以上である必要があります (size=" +
                std::to_string(size) + ", mipLevels=" + std::to_string(mipLevels) + ", cubeCount=" + std::to_string(cubeCount) + ")";
            Core::Logger::Error("DX11", message);
            throw std::runtime_error(message);
        }

        // D3D11のTexture2D配列は最大2048スライス。キューブマップは1枚あたり6スライス消費する
        const uint32_t arraySize = cubeCount * DX11Texture::kCubeFaceCount;
        if (arraySize > D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION)
        {
            const std::string message =
                "キューブマップUAVテクスチャの作成に失敗しました: 配列スライス数が上限を超えています (cubeCount=" +
                std::to_string(cubeCount) + ", 必要スライス数=" + std::to_string(arraySize) +
                ", 上限=" + std::to_string(D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION) + ")";
            Core::Logger::Error("DX11", message);
            throw std::runtime_error(message);
        }

        const DXGI_FORMAT dxgiFormat = ToDXGIFormat(format);

        D3D11_TEXTURE2D_DESC textureDesc{};
        textureDesc.Width = size;
        textureDesc.Height = size;
        textureDesc.MipLevels = mipLevels;
        textureDesc.ArraySize = arraySize;
        textureDesc.Format = dxgiFormat;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.Usage = D3D11_USAGE_DEFAULT;
        textureDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
        textureDesc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        ThrowIfFailed(m_Device->CreateTexture2D(&textureDesc, nullptr, &texture), "キューブマップUAVテクスチャの作成に失敗しました");

        // 全6面・全ミップを1枚のTextureCube(配列版はTextureCubeArray)として読むSRV
        // (サンプリング側、DeferredLighting.hlsl等)
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = dxgiFormat;
        if (asArray)
        {
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
            srvDesc.TextureCubeArray.MostDetailedMip = 0;
            srvDesc.TextureCubeArray.MipLevels = mipLevels;
            srvDesc.TextureCubeArray.First2DArrayFace = 0;
            srvDesc.TextureCubeArray.NumCubes = cubeCount;
        }
        else
        {
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
            srvDesc.TextureCube.MostDetailedMip = 0;
            srvDesc.TextureCube.MipLevels = mipLevels;
        }
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        ThrowIfFailed(m_Device->CreateShaderResourceView(texture.Get(), &srvDesc, &srv), "キューブマップシェーダリソースビューの作成に失敗しました");

        // キューブ×面×ミップの組み合わせごとに単一配列スライス・単一ミップのUAV(Texture2DArray、要素数1)を
        // 張り、コンピュートシェーダーが面ごとに1回ずつディスパッチして書き込めるようにする(HLSL側は
        // RWTexture2DArrayとして宣言する必要がある。IBLConvolve.hlsl参照)。
        // (mip*cubeCount + cubeIndex)*kCubeFaceCount + face の順でフラットに格納する
        // (DX11Texture::GetCubeUnorderedAccessView参照)
        std::vector<Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>> mipUavs;
        mipUavs.reserve(static_cast<size_t>(mipLevels) * arraySize);
        for (uint32_t mip = 0; mip < mipLevels; ++mip)
        {
            for (uint32_t cube = 0; cube < cubeCount; ++cube)
            {
                for (uint32_t face = 0; face < DX11Texture::kCubeFaceCount; ++face)
                {
                    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
                    uavDesc.Format = dxgiFormat;
                    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
                    uavDesc.Texture2DArray.MipSlice = mip;
                    uavDesc.Texture2DArray.FirstArraySlice = cube * DX11Texture::kCubeFaceCount + face;
                    uavDesc.Texture2DArray.ArraySize = 1;

                    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav;
                    ThrowIfFailed(
                        m_Device->CreateUnorderedAccessView(texture.Get(), &uavDesc, &uav),
                        "キューブマップアンオーダードアクセスビューの作成に失敗しました");
                    mipUavs.push_back(std::move(uav));
                }
            }
        }

        return std::make_unique<DX11Texture>(
            srv, nullptr, nullptr, nullptr, std::move(mipUavs),
            std::vector<Microsoft::WRL::ComPtr<ID3D11DepthStencilView>>{}, cubeCount);
    }

    std::unique_ptr<IRHITexture> DX11Device::CreateDepthTexture(uint32_t width, uint32_t height, float clearDepth)
    {
        // 深度テクスチャは後段のライティングパスでサンプリングするためSHADER_RESOURCEも付与し、
        // Typelessフォーマットで作成してDSV/SRVそれぞれに適したビューを個別に張る。
        // ステンシルは使わないためD32_FLOATにしている(Reverse-Zの精度改善はUNORMでは効果がなく、
        // 浮動小数点フォーマットと組み合わせて初めて意味を持つ)。clearDepthはD3D11では
        // リソース生成時に宣言する概念がないため未使用(実際のクリア値はClearDepth呼び出し時に指定する)
        (void)clearDepth;
        D3D11_TEXTURE2D_DESC textureDesc{};
        textureDesc.Width = width;
        textureDesc.Height = height;
        textureDesc.MipLevels = 1;
        textureDesc.ArraySize = 1;
        textureDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.Usage = D3D11_USAGE_DEFAULT;
        textureDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        ThrowIfFailed(m_Device->CreateTexture2D(&textureDesc, nullptr, &texture), "深度テクスチャの作成に失敗しました");

        D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv;
        ThrowIfFailed(m_Device->CreateDepthStencilView(texture.Get(), &dsvDesc, &dsv), "深度ステンシルビューの作成に失敗しました");

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        ThrowIfFailed(m_Device->CreateShaderResourceView(texture.Get(), &srvDesc, &srv), "深度シェーダリソースビューの作成に失敗しました");

        return std::make_unique<DX11Texture>(srv, nullptr, dsv);
    }

    std::unique_ptr<IRHITexture> DX11Device::CreateDepthTextureArray(
        uint32_t width, uint32_t height, uint32_t arraySize, float clearDepth)
    {
        if (width == 0 || height == 0 || arraySize == 0)
        {
            const std::string message = "CreateDepthTextureArray: 不正なサイズが指定されました(width=" +
                                        std::to_string(width) + ", height=" + std::to_string(height) +
                                        ", arraySize=" + std::to_string(arraySize) + ")";
            Core::Logger::Error("DX11", message);
            throw std::runtime_error(message);
        }

        if (arraySize > D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION)
        {
            const std::string message = "CreateDepthTextureArray: 配列サイズがD3D11の上限を超えています(arraySize=" +
                                        std::to_string(arraySize) +
                                        ", 上限=" + std::to_string(D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION) + ")";
            Core::Logger::Error("DX11", message);
            throw std::runtime_error(message);
        }

        // CreateDepthTextureと同じくR32_TYPELESSで作り、書き込み用のD32_FLOAT DSVと
        // 読み取り用のR32_FLOAT SRVを別々に張る。違いはArraySizeが1より大きいことと、
        // DSVをスライスごとに(Texture2DArray、要素数1で)張ること。
        // clearDepthが未使用な理由はCreateDepthTextureと同じ
        (void)clearDepth;
        D3D11_TEXTURE2D_DESC textureDesc{};
        textureDesc.Width = width;
        textureDesc.Height = height;
        textureDesc.MipLevels = 1;
        textureDesc.ArraySize = arraySize;
        textureDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.Usage = D3D11_USAGE_DEFAULT;
        textureDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        ThrowIfFailed(m_Device->CreateTexture2D(&textureDesc, nullptr, &texture), "深度テクスチャ配列の作成に失敗しました");

        // 全スライスを1枚のTexture2DArrayとして読むSRV(サンプリング側。ShadowSampling.hlsli等)
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        srvDesc.Texture2DArray.MostDetailedMip = 0;
        srvDesc.Texture2DArray.MipLevels = 1;
        srvDesc.Texture2DArray.FirstArraySlice = 0;
        srvDesc.Texture2DArray.ArraySize = arraySize;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        ThrowIfFailed(
            m_Device->CreateShaderResourceView(texture.Get(), &srvDesc, &srv),
            "深度テクスチャ配列のシェーダリソースビューの作成に失敗しました");

        // スライスごとに単一配列スライスのDSV(Texture2DArray、要素数1)を張り、
        // パスごとに1スライスずつ描き込めるようにする(CreateMippedUAVTextureCubeのUAVと同じ考え方)
        std::vector<Microsoft::WRL::ComPtr<ID3D11DepthStencilView>> sliceDsvs;
        sliceDsvs.reserve(arraySize);
        for (uint32_t slice = 0; slice < arraySize; ++slice)
        {
            D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
            dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
            dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
            dsvDesc.Texture2DArray.MipSlice = 0;
            dsvDesc.Texture2DArray.FirstArraySlice = slice;
            dsvDesc.Texture2DArray.ArraySize = 1;
            Microsoft::WRL::ComPtr<ID3D11DepthStencilView> sliceDsv;
            ThrowIfFailed(
                m_Device->CreateDepthStencilView(texture.Get(), &dsvDesc, &sliceDsv),
                "深度テクスチャ配列のスライス" + std::to_string(slice) + "の深度ステンシルビューの作成に失敗しました");
            sliceDsvs.push_back(std::move(sliceDsv));
        }

        return std::make_unique<DX11Texture>(
            srv, nullptr, nullptr, nullptr,
            std::vector<Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>>{}, std::move(sliceDsvs));
    }

    std::unique_ptr<IRHISamplerSet> DX11Device::CreateSamplerSet(const SamplerDesc* descs, uint32_t count)
    {
        if (!descs || count == 0)
        {
            Core::Logger::Error("DX11", "CreateSamplerSet: サンプラー記述子が指定されていません");
            throw std::runtime_error("CreateSamplerSetにサンプラー記述子が指定されていません");
        }

        std::vector<Microsoft::WRL::ComPtr<ID3D11SamplerState>> samplers;
        samplers.reserve(count);

        for (uint32_t i = 0; i < count; ++i)
        {
            const SamplerDesc& desc = descs[i];

            D3D11_SAMPLER_DESC samplerDesc{};
            switch (desc.Filter)
            {
            case SamplerFilter::Anisotropic:
                samplerDesc.Filter = D3D11_FILTER_ANISOTROPIC;
                break;
            case SamplerFilter::Point:
                samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
                break;
            case SamplerFilter::Linear:
                samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
                break;
            default:
                Core::Logger::Warning(
                    "DX11",
                    "CreateSamplerSet: 未知のSamplerFilter(" + std::to_string(static_cast<int>(desc.Filter)) +
                        ")が指定されたためLinearで代用します");
                samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
                break;
            }

            D3D11_TEXTURE_ADDRESS_MODE addressMode = D3D11_TEXTURE_ADDRESS_WRAP;
            switch (desc.AddressMode)
            {
            case SamplerAddressMode::Clamp:
                addressMode = D3D11_TEXTURE_ADDRESS_CLAMP;
                break;
            case SamplerAddressMode::Wrap:
                addressMode = D3D11_TEXTURE_ADDRESS_WRAP;
                break;
            default:
                Core::Logger::Warning(
                    "DX11",
                    "CreateSamplerSet: 未知のSamplerAddressMode(" + std::to_string(static_cast<int>(desc.AddressMode)) +
                        ")が指定されたためWrapで代用します");
                addressMode = D3D11_TEXTURE_ADDRESS_WRAP;
                break;
            }
            samplerDesc.AddressU = addressMode;
            samplerDesc.AddressV = addressMode;
            samplerDesc.AddressW = addressMode;
            // MaxAnisotropyはFilterがANISOTROPICでない場合ハードウェア側で無視されるため、常に設定してよい
            samplerDesc.MaxAnisotropy = desc.MaxAnisotropy;
            samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
            samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

            Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler;
            ThrowIfFailed(
                m_Device->CreateSamplerState(&samplerDesc, &sampler),
                "サンプラー(スロット" + std::to_string(i) + ")の作成に失敗しました");
            samplers.push_back(std::move(sampler));
        }

        return std::make_unique<DX11SamplerSet>(std::move(samplers));
    }

    IRHICommandList* DX11Device::GetImmediateCommandList()
    {
        return m_ImmediateCommandList.get();
    }

    void DX11Device::WaitForGPUIdle()
    {
        D3D11_QUERY_DESC queryDesc{};
        queryDesc.Query = D3D11_QUERY_EVENT;
        Microsoft::WRL::ComPtr<ID3D11Query> query;
        if (FAILED(m_Device->CreateQuery(&queryDesc, &query)))
        {
            Core::Logger::Error("DX11", "WaitForGPUIdle: 同期用クエリの作成に失敗したため、GPU待機をスキップします");
            return;
        }

        m_Context->End(query.Get());
        m_Context->Flush();

        // GetDataはGPUが該当区間(End呼び出しまでに発行された全コマンド)の実行を完了するまでS_FALSEを返す
        while (m_Context->GetData(query.Get(), nullptr, 0, 0) == S_FALSE)
        {
            Sleep(0);
        }
    }

    std::unique_ptr<IRHIAccelerationStructure> DX11Device::CreateBottomLevelAS(const BottomLevelASDesc& desc)
    {
        // 引数は使わないが、シグネチャはIRHIDeviceの契約通りに保つ
        (void)desc;
        Core::Logger::Error(
            "DX11", "CreateBottomLevelAS: DX11はレイトレーシングに対応していません。SupportsRaytracing()で分岐してください");
        return nullptr;
    }

    std::unique_ptr<IRHIAccelerationStructure> DX11Device::CreateTopLevelAS(const TopLevelASDesc& desc)
    {
        (void)desc;
        Core::Logger::Error(
            "DX11", "CreateTopLevelAS: DX11はレイトレーシングに対応していません。SupportsRaytracing()で分岐してください");
        return nullptr;
    }

    uint32_t DX11Device::RegisterBindless(IRHITexture* texture)
    {
        (void)texture;
        Core::Logger::Error(
            "DX11", "RegisterBindless: DX11はbindlessに対応していません。SupportsBindless()で分岐してください");
        return kInvalidBindlessIndex;
    }

    uint32_t DX11Device::RegisterBindless(IRHIBuffer* buffer)
    {
        (void)buffer;
        Core::Logger::Error(
            "DX11", "RegisterBindless: DX11はbindlessに対応していません。SupportsBindless()で分岐してください");
        return kInvalidBindlessIndex;
    }

    uint32_t DX11Device::RegisterBindlessUAV(IRHIBuffer* buffer)
    {
        (void)buffer;
        Core::Logger::Error(
            "DX11", "RegisterBindlessUAV: DX11はbindlessに対応していません。SupportsBindless()で分岐してください");
        return kInvalidBindlessIndex;
    }

    std::unique_ptr<IRHIPipelineState> DX11Device::CreateMeshPipelineState(const MeshPipelineStateDesc& desc)
    {
        (void)desc;
        Core::Logger::Error(
            "DX11",
            "CreateMeshPipelineState: DX11はメッシュシェーダーに対応していません。SupportsMeshShader()で分岐してください");
        return nullptr;
    }

    std::unique_ptr<IRHIImGuiBackend> DX11Device::CreateImGuiBackend(void* windowHandle)
    {
        return std::make_unique<DX11ImGuiBackend>(m_Device.Get(), m_Context.Get(), windowHandle);
    }

    std::unique_ptr<IRHIGPUProfiler> DX11Device::CreateGPUProfiler()
    {
        return std::make_unique<DX11GPUProfiler>(m_Device, m_Context);
    }

    std::unique_ptr<IRHIDevice> CreateDX11Device()
    {
        auto device = std::make_unique<DX11Device>();
        device->Initialize();
        return device;
    }
}
