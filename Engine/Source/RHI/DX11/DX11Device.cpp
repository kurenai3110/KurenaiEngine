#include "DX11Device.h"

#include <d3dcompiler.h>

#include <DirectXTex.h>

#include <cwchar>
#include <vector>

#include "DX11Buffer.h"
#include "DX11CommandList.h"
#include "DX11ImGuiBackend.h"
#include "DX11PipelineState.h"
#include "DX11Sampler.h"
#include "DX11Shader.h"
#include "DX11SwapChain.h"
#include "DX11Texture.h"
#include "DX11Util.h"

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
            case Format::R32G32B32A32_Float:
            default:
                return DXGI_FORMAT_R32G32B32A32_FLOAT;
            }
        }

        bool HasExtension(const std::wstring& path, const wchar_t* extension)
        {
            const size_t extLen = wcslen(extension);
            if (path.size() < extLen)
            {
                return false;
            }
            return _wcsicmp(path.c_str() + (path.size() - extLen), extension) == 0;
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

        return std::make_unique<DX11SwapChain>(swapChain, m_Device, m_Context, width, height);
    }

    std::unique_ptr<IRHIBuffer> DX11Device::CreateBuffer(const BufferDesc& desc)
    {
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
        const char* target = desc.Stage == ShaderStage::Vertex ? "vs_5_0" : "ps_5_0";

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
            throw std::runtime_error(message);
        }

        Microsoft::WRL::ComPtr<ID3D11DeviceChild> shader;
        if (desc.Stage == ShaderStage::Vertex)
        {
            Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader;
            hr = m_Device->CreateVertexShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr, &vertexShader);
            shader = vertexShader;
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

        return std::make_unique<DX11PipelineState>(inputLayout, vertexShader, pixelShader, desc.Topology);
    }

    std::unique_ptr<IRHITexture> DX11Device::CreateTextureFromFile(const std::wstring& filePath, bool sRGB)
    {
        DirectX::TexMetadata metadata{};
        DirectX::ScratchImage image;

        HRESULT hr;
        if (HasExtension(filePath, L".dds"))
        {
            hr = DirectX::LoadFromDDSFile(filePath.c_str(), DirectX::DDS_FLAGS_NONE, &metadata, image);
        }
        else if (HasExtension(filePath, L".tga"))
        {
            hr = DirectX::LoadFromTGAFile(filePath.c_str(), DirectX::TGA_FLAGS_NONE, &metadata, image);
        }
        else
        {
            hr = DirectX::LoadFromWICFile(filePath.c_str(), DirectX::WIC_FLAGS_FORCE_RGB, &metadata, image);
        }
        ThrowIfFailed(hr, "テクスチャの読み込みに失敗しました");

        if (sRGB)
        {
            image.OverrideFormat(DirectX::MakeSRGB(metadata.format));
        }

        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        ThrowIfFailed(
            DirectX::CreateShaderResourceView(m_Device.Get(), image.GetImages(), image.GetImageCount(), image.GetMetadata(), &srv),
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

    std::unique_ptr<IRHITexture> DX11Device::CreateDepthTexture(uint32_t width, uint32_t height)
    {
        // 深度テクスチャは後段のライティングパスでサンプリングするためSHADER_RESOURCEも付与し、
        // Typelessフォーマットで作成してDSV/SRVそれぞれに適したビューを個別に張る
        D3D11_TEXTURE2D_DESC textureDesc{};
        textureDesc.Width = width;
        textureDesc.Height = height;
        textureDesc.MipLevels = 1;
        textureDesc.ArraySize = 1;
        textureDesc.Format = DXGI_FORMAT_R24G8_TYPELESS;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.Usage = D3D11_USAGE_DEFAULT;
        textureDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        ThrowIfFailed(m_Device->CreateTexture2D(&textureDesc, nullptr, &texture), "深度テクスチャの作成に失敗しました");

        D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv;
        ThrowIfFailed(m_Device->CreateDepthStencilView(texture.Get(), &dsvDesc, &dsv), "深度ステンシルビューの作成に失敗しました");

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        ThrowIfFailed(m_Device->CreateShaderResourceView(texture.Get(), &srvDesc, &srv), "深度シェーダリソースビューの作成に失敗しました");

        return std::make_unique<DX11Texture>(srv, nullptr, dsv);
    }

    std::unique_ptr<IRHISampler> DX11Device::CreateDefaultSampler()
    {
        D3D11_SAMPLER_DESC samplerDesc{};
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
        samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
        samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
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

    std::unique_ptr<IRHIImGuiBackend> DX11Device::CreateImGuiBackend(void* windowHandle)
    {
        return std::make_unique<DX11ImGuiBackend>(m_Device.Get(), m_Context.Get(), windowHandle);
    }

    std::unique_ptr<IRHIDevice> CreateDX11Device()
    {
        auto device = std::make_unique<DX11Device>();
        device->Initialize();
        return device;
    }
}
