#include "DX11Device.h"

#include <d3dcompiler.h>

#include <DirectXTex.h>

#include <cwchar>
#include <vector>

#include "DX11Buffer.h"
#include "DX11CommandList.h"
#include "DX11ComputePipelineState.h"
#include "DX11GPUProfiler.h"
#include "DX11ImGuiBackend.h"
#include "DX11PipelineState.h"
#include "DX11Sampler.h"
#include "DX11Shader.h"
#include "DX11SwapChain.h"
#include "DX11Texture.h"
#include "DX11Util.h"
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

        m_ImmediateCommandList = std::make_unique<DX11CommandList>(m_Context);
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
        const char* target =
            desc.Stage == ShaderStage::Vertex ? "vs_5_0" : desc.Stage == ShaderStage::Compute ? "cs_5_0" : "ps_5_0";

        UINT compileFlags = 0;
#if defined(_DEBUG)
        compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

        Microsoft::WRL::ComPtr<ID3DBlob> bytecode;
        Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;
        HRESULT hr = D3DCompileFromFile(
            desc.FilePath.c_str(),
            nullptr,
            D3D_COMPILE_STANDARD_FILE_INCLUDE,
            desc.EntryPoint.c_str(),
            target,
            compileFlags,
            0,
            &bytecode,
            &errorBlob);

        if (FAILED(hr))
        {
            std::string message = "シェーダのコンパイルに失敗しました";
            if (errorBlob)
            {
                message += ": ";
                message += static_cast<const char*>(errorBlob->GetBufferPointer());
            }
            Core::Logger::Error("DX11", message);
            throw std::runtime_error(message);
        }

        Microsoft::WRL::ComPtr<ID3D11DeviceChild> shader;
        if (desc.Stage == ShaderStage::Vertex)
        {
            Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader;
            hr = m_Device->CreateVertexShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr, &vertexShader);
            shader = vertexShader;
        }
        else if (desc.Stage == ShaderStage::Compute)
        {
            Microsoft::WRL::ComPtr<ID3D11ComputeShader> computeShader;
            hr = m_Device->CreateComputeShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr, &computeShader);
            shader = computeShader;
        }
        else
        {
            Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader;
            hr = m_Device->CreatePixelShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr, &pixelShader);
            shader = pixelShader;
        }
        ThrowIfFailed(hr, "シェーダオブジェクトの作成に失敗しました");

        return std::make_unique<DX11Shader>(desc.Stage, shader, bytecode);
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
                vertexShader->GetBytecode()->GetBufferPointer(),
                vertexShader->GetBytecode()->GetBufferSize(),
                &inputLayout);
            ThrowIfFailed(hr, "入力レイアウトの作成に失敗しました");
        }

        // DX12はPSOごとに深度ステンシルステートを持てるが、DX11はコンテキストへの明示バインドが必要。
        // 何も設定しないとデフォルト状態(DepthEnable=TRUE, DepthFunc=LESS)になってしまうため、
        // Reverse-Z(GREATER)を使うパイプラインのぶんも含めてここで明示的に作成する
        D3D11_DEPTH_STENCIL_DESC depthStencilDesc{};
        depthStencilDesc.DepthEnable = desc.HasDepthStencil ? TRUE : FALSE;
        depthStencilDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        depthStencilDesc.DepthFunc = desc.ReverseZ ? D3D11_COMPARISON_GREATER : D3D11_COMPARISON_LESS;
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

        return std::make_unique<DX11PipelineState>(inputLayout, vertexShader, pixelShader, desc.Topology, depthStencilState, blendState);
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
        const DXGI_FORMAT dxgiFormat = ToDXGIFormat(format);

        D3D11_TEXTURE2D_DESC textureDesc{};
        textureDesc.Width = size;
        textureDesc.Height = size;
        textureDesc.MipLevels = mipLevels;
        textureDesc.ArraySize = DX11Texture::kCubeFaceCount;
        textureDesc.Format = dxgiFormat;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.Usage = D3D11_USAGE_DEFAULT;
        textureDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
        textureDesc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        ThrowIfFailed(m_Device->CreateTexture2D(&textureDesc, nullptr, &texture), "キューブマップUAVテクスチャの作成に失敗しました");

        // 全6面・全ミップを1枚のTextureCubeとして読むSRV(サンプリング側、DeferredLighting.hlsl等)
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = dxgiFormat;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
        srvDesc.TextureCube.MostDetailedMip = 0;
        srvDesc.TextureCube.MipLevels = mipLevels;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        ThrowIfFailed(m_Device->CreateShaderResourceView(texture.Get(), &srvDesc, &srv), "キューブマップシェーダリソースビューの作成に失敗しました");

        // 面×ミップの組み合わせごとに単一配列スライス・単一ミップのUAV(Texture2DArray、要素数1)を張り、
        // コンピュートシェーダーが面ごとに1回ずつディスパッチして書き込めるようにする(HLSL側は
        // RWTexture2DArrayとして宣言する必要がある。IBLConvolve.hlsl参照)。mip*kCubeFaceCount+face の
        // 順でフラットに格納する(DX11Texture::GetCubeUnorderedAccessView参照)
        std::vector<Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>> mipUavs;
        mipUavs.reserve(mipLevels * DX11Texture::kCubeFaceCount);
        for (uint32_t mip = 0; mip < mipLevels; ++mip)
        {
            for (uint32_t face = 0; face < DX11Texture::kCubeFaceCount; ++face)
            {
                D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
                uavDesc.Format = dxgiFormat;
                uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
                uavDesc.Texture2DArray.MipSlice = mip;
                uavDesc.Texture2DArray.FirstArraySlice = face;
                uavDesc.Texture2DArray.ArraySize = 1;

                Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav;
                ThrowIfFailed(
                    m_Device->CreateUnorderedAccessView(texture.Get(), &uavDesc, &uav),
                    "キューブマップアンオーダードアクセスビューの作成に失敗しました");
                mipUavs.push_back(std::move(uav));
            }
        }

        return std::make_unique<DX11Texture>(srv, nullptr, nullptr, nullptr, std::move(mipUavs));
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

    std::unique_ptr<IRHISampler> DX11Device::CreateDefaultSampler(const SamplerDesc& desc)
    {
        D3D11_SAMPLER_DESC samplerDesc{};
        samplerDesc.Filter = desc.Filter == SamplerFilter::Anisotropic ? D3D11_FILTER_ANISOTROPIC : D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
        samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
        samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        // MaxAnisotropyはFilterがANISOTROPICでない場合ハードウェア側で無視されるため、常に設定してよい
        samplerDesc.MaxAnisotropy = desc.MaxAnisotropy;
        samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
        samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

        Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler;
        ThrowIfFailed(m_Device->CreateSamplerState(&samplerDesc, &sampler), "サンプラーの作成に失敗しました");

        return std::make_unique<DX11Sampler>(sampler);
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
