#include "DX12Device.h"

#include <d3dcompiler.h>
#include <d3dx12.h>

#include <DirectXTex.h>

#include <chrono>
#include <cstring>
#include <cwchar>
#include <vector>

#include "DX12Buffer.h"
#include "DX12CommandList.h"
#include "DX12GPUProfiler.h"
#include "DX12ImGuiBackend.h"
#include "DX12PipelineState.h"
#include "DX12Sampler.h"
#include "DX12Shader.h"
#include "DX12SwapChain.h"
#include "DX12Texture.h"
#include "DX12Util.h"

namespace Kurenai::RHI
{
    namespace
    {
        // シェーダのレジスタ実測値(Sandbox/Shaders/*.hlsl)に基づく固定のルートシグネチャレイアウト
        constexpr uint32_t kTextureSlotCount = 7; // t0〜t6 (DeferredLighting.hlslが最大)
        constexpr uint32_t kSamplerSlotCount = 1; // s0のみ
        // 1フレームあたりに払い出せるSRVテーブルブロック(t0〜t6のkTextureSlotCount個ひと組)の最大数。
        // 1フレーム中の(メッシュ数×パス数)を十分上回る値にしておく。実際に確保するヒープ容量は
        // これのkFrameCount倍(CPUがGPU完了を待たずに次フレームを記録し始めるため、直近kFrameCount
        // フレームぶんのブロックがまだGPUに読まれている可能性がある)
        constexpr uint32_t kMaxSrvTableBlocksPerFrame = 4096;
        // 定数バッファ(Usage==Constant)がリングとして持つスロット数。CPUがGPU完了を待たずに次フレームを
        // 記録し始めるため、直近kFrameCountフレームぶんのUpdateBuffer回数(メッシュ数など)を
        // 十分上回る値にしておく
        constexpr uint32_t kConstantBufferRingCapacity = 8192;

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

    }

    DX12Device::DX12Device() = default;

    DX12Device::~DX12Device()
    {
        if (m_Device)
        {
            WaitForGPUIdle();
        }

        if (m_FenceEvent)
        {
            CloseHandle(m_FenceEvent);
        }
    }

    void DX12Device::Initialize()
    {
#if defined(_DEBUG)
        Microsoft::WRL::ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
        {
            debugController->EnableDebugLayer();
        }
#endif

        UINT dxgiFactoryFlags = 0;
#if defined(_DEBUG)
        dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
#endif
        ThrowIfFailed(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&m_Factory)), "DXGIファクトリの作成に失敗しました");

        ThrowIfFailed(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_Device)), "D3D12デバイスの作成に失敗しました");

        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ThrowIfFailed(m_Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_CommandQueue)), "コマンドキューの作成に失敗しました");

        // CPUがGPUの完了を待たずに次フレームの記録を始められるよう、フレームスロットごとに
        // 独立したコマンドアロケータを持つ(コマンドリスト自体は1つを使い回し、Reset時に
        // そのフレームのアロケータへ切り替える)
        for (uint32_t i = 0; i < kFrameCount; ++i)
        {
            ThrowIfFailed(
                m_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_CommandAllocators[i])),
                "コマンドアロケータの作成に失敗しました");
        }
        ThrowIfFailed(
            m_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_CommandAllocators[0].Get(), nullptr, IID_PPV_ARGS(&m_CommandList)),
            "コマンドリストの作成に失敗しました");

        ThrowIfFailed(m_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_Fence)), "フェンスの作成に失敗しました");
        m_FenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!m_FenceEvent)
        {
            throw std::runtime_error("フェンスイベントの作成に失敗しました");
        }

        m_RtvHeap = std::make_unique<DX12DescriptorHeap>(m_Device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 16, false);
        m_DsvHeap = std::make_unique<DX12DescriptorHeap>(m_Device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 8, false);
        m_SrvCpuHeap = std::make_unique<DX12DescriptorHeap>(m_Device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 256, false);
        m_SamplerCpuHeap = std::make_unique<DX12DescriptorHeap>(m_Device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 4, false);
        // 1フレーム分のコマンドをまとめて記録してから1回だけ実行する設計のため、描画のたびに
        // 新しいkTextureSlotCount個のブロックを払い出せるよう、1フレームに必要な最大数を見込んで確保する。
        // さらにCPUがGPU完了を待たずに次フレームを記録し始めるため、kFrameCountフレームぶんの容量を持たせる
        m_ShaderVisibleSrvHeap = std::make_unique<DX12DescriptorHeap>(
            m_Device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kTextureSlotCount * kMaxSrvTableBlocksPerFrame * kFrameCount, true);
        m_ShaderVisibleSamplerHeap = std::make_unique<DX12DescriptorHeap>(m_Device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplerSlotCount, true);

        CreateRootSignature();

        ID3D12DescriptorHeap* heaps[] = { m_ShaderVisibleSrvHeap->GetHeap(), m_ShaderVisibleSamplerHeap->GetHeap() };
        m_CommandList->SetDescriptorHeaps(2, heaps);

        m_ImmediateCommandList = std::make_unique<DX12CommandList>(this);
    }

    void DX12Device::CreateRootSignature()
    {
        CD3DX12_DESCRIPTOR_RANGE srvRange;
        srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kTextureSlotCount, 0);

        CD3DX12_DESCRIPTOR_RANGE samplerRange;
        samplerRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, kSamplerSlotCount, 0);

        CD3DX12_ROOT_PARAMETER rootParams[4];
        rootParams[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_ALL);
        rootParams[1].InitAsConstantBufferView(1, 0, D3D12_SHADER_VISIBILITY_ALL);
        rootParams[2].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);
        rootParams[3].InitAsDescriptorTable(1, &samplerRange, D3D12_SHADER_VISIBILITY_PIXEL);

        CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc;
        rootSigDesc.Init(4, rootParams, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

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
            throw std::runtime_error(message);
        }

        ThrowIfFailed(
            m_Device->CreateRootSignature(0, signatureBlob->GetBufferPointer(), signatureBlob->GetBufferSize(), IID_PPV_ARGS(&m_RootSignature)),
            "ルートシグネチャの作成に失敗しました");
    }

    void DX12Device::ExecuteCommandList()
    {
        ThrowIfFailed(m_CommandList->Close(), "コマンドリストのクローズに失敗しました");
        ID3D12CommandList* commandLists[] = { m_CommandList.Get() };
        m_CommandQueue->ExecuteCommandLists(1, commandLists);
    }

    void DX12Device::WaitForGPUIdle()
    {
        const uint64_t fenceValueToWaitFor = ++m_FenceValue;
        ThrowIfFailed(m_CommandQueue->Signal(m_Fence.Get(), fenceValueToWaitFor), "フェンスのシグナルに失敗しました");

        if (m_Fence->GetCompletedValue() < fenceValueToWaitFor)
        {
            ThrowIfFailed(m_Fence->SetEventOnCompletion(fenceValueToWaitFor, m_FenceEvent), "フェンスイベントの設定に失敗しました");
            WaitForSingleObject(m_FenceEvent, INFINITE);
        }
    }

    void DX12Device::SignalFrame()
    {
        const uint64_t fenceValueToSignal = ++m_FenceValue;
        ThrowIfFailed(m_CommandQueue->Signal(m_Fence.Get(), fenceValueToSignal), "フェンスのシグナルに失敗しました");
        m_FrameFenceValues[m_FrameIndex] = fenceValueToSignal;
    }

    void DX12Device::AdvanceToNextFrame()
    {
        m_FrameIndex = (m_FrameIndex + 1) % kFrameCount;

        // このスロットを最後に使ったフレーム(kFrameCountフレーム前)のGPU実行完了を待つ。
        // 通常はすでに完了しているため待たずに素通りする。この待ち時間は実際のCPU負荷ではなく
        // GPU側の処理時間を反映したものなので、GetLastFrameGPUWaitTimeMs()で別途取得できるようにし、
        // 呼び出し側(Application)がCPU時間の表示から差し引けるようにしておく
        m_LastFrameGPUWaitTimeMs = 0.0f;
        const uint64_t fenceValueToWaitFor = m_FrameFenceValues[m_FrameIndex];
        if (fenceValueToWaitFor != 0 && m_Fence->GetCompletedValue() < fenceValueToWaitFor)
        {
            const auto waitStart = std::chrono::steady_clock::now();
            ThrowIfFailed(m_Fence->SetEventOnCompletion(fenceValueToWaitFor, m_FenceEvent), "フェンスイベントの設定に失敗しました");
            WaitForSingleObject(m_FenceEvent, INFINITE);
            m_LastFrameGPUWaitTimeMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - waitStart).count();
        }

        ResetCommandList();
    }

    void DX12Device::SubmitAndWaitIdle()
    {
        ExecuteCommandList();
        WaitForGPUIdle();
        ResetCommandList();
    }

    void DX12Device::ResetCommandList()
    {
        auto& allocator = m_CommandAllocators[m_FrameIndex];
        ThrowIfFailed(allocator->Reset(), "コマンドアロケータのリセットに失敗しました");
        ThrowIfFailed(m_CommandList->Reset(allocator.Get(), nullptr), "コマンドリストのリセットに失敗しました");

        ID3D12DescriptorHeap* heaps[] = { m_ShaderVisibleSrvHeap->GetHeap(), m_ShaderVisibleSamplerHeap->GetHeap() };
        m_CommandList->SetDescriptorHeaps(2, heaps);

        // m_NextSrvTableIndexはフレームをまたいで巻き戻さない(kFrameCountフレーム分の容量を
        // 持つリングとして扱う)ため、ここではリセットしない。1フレームあたりの払い出し数の
        // 検証用カウンタのみリセットする
        m_SrvTableBlocksUsedThisFrame = 0;
    }

    uint32_t DX12Device::AllocateSrvTableBlock(uint32_t count)
    {
        m_SrvTableBlocksUsedThisFrame += count;
        if (m_SrvTableBlocksUsedThisFrame > kTextureSlotCount * kMaxSrvTableBlocksPerFrame)
        {
            throw std::runtime_error("SRVテーブルブロックの上限を超えました(1フレーム内の描画回数が多すぎます)");
        }

        // ヒープ全体をkFrameCountフレームぶんの容量を持つリングとして扱う(フレームごとに0へは
        // 巻き戻さない)。CPUがGPU完了を待たずに次フレームを記録し始めるため、直近フレームの
        // ブロックへ書き込み中にGPUがまだそれを読んでいる可能性があるが、1フレームあたりの
        // 消費量が上のチェックでkMaxSrvTableBlocksPerFrameを超えない限り、ここで巻き戻る位置は
        // 少なくともkFrameCount-1フレーム前のブロックであり、AdvanceToNextFrame()のフェンス待ちで
        // そのフレームの実行完了は既に保証されている
        const uint32_t totalCapacity = kTextureSlotCount * kMaxSrvTableBlocksPerFrame * kFrameCount;
        const uint32_t base = m_NextSrvTableIndex;
        m_NextSrvTableIndex = (m_NextSrvTableIndex + count) % totalCapacity;
        return base;
    }

    Microsoft::WRL::ComPtr<ID3D12Resource> DX12Device::CreateUploadBuffer(uint64_t sizeInBytes)
    {
        const CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);
        const CD3DX12_RESOURCE_DESC resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeInBytes);

        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        ThrowIfFailed(
            m_Device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&resource)),
            "アップロードバッファの作成に失敗しました");
        return resource;
    }

    std::unique_ptr<IRHISwapChain> DX12Device::CreateSwapChain(void* windowHandle, uint32_t width, uint32_t height)
    {
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = width;
        desc.Height = height;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

        Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain1;
        ThrowIfFailed(
            m_Factory->CreateSwapChainForHwnd(m_CommandQueue.Get(), static_cast<HWND>(windowHandle), &desc, nullptr, nullptr, &swapChain1),
            "スワップチェインの作成に失敗しました");

        Microsoft::WRL::ComPtr<IDXGISwapChain3> swapChain3;
        ThrowIfFailed(swapChain1.As(&swapChain3), "IDXGISwapChain3の取得に失敗しました");

        return std::make_unique<DX12SwapChain>(this, swapChain3, width, height);
    }

    std::unique_ptr<IRHIBuffer> DX12Device::CreateBuffer(const BufferDesc& desc)
    {
        // 定数バッファはCPUから毎フレームUpdateBufferで書き込むため、UPLOADヒープに常駐させ
        // マップしたままにする(従来通り)
        if (desc.Usage == BufferUsage::Constant)
        {
            // ルート定数バッファビューは256バイトアライメントを要求するため切り上げる
            const uint32_t slotSizeInBytes = (desc.SizeInBytes + 255) & ~255u;

            // 1フレームぶんのコマンドをすべて記録してから1回だけ実行する設計のため、同じ定数バッファへ
            // メッシュごとに複数回UpdateBufferすると、GPU実行時にはそのフレーム最後の書き込みへ
            // 全描画が上書きされてしまう。これを避けるため、リング状に複数コピーを確保しておく
            const uint32_t ringCapacity = kConstantBufferRingCapacity;

            Microsoft::WRL::ComPtr<ID3D12Resource> resource = CreateUploadBuffer(static_cast<uint64_t>(slotSizeInBytes) * ringCapacity);

            void* mappedPtr = nullptr;
            const D3D12_RANGE readRange{ 0, 0 };
            ThrowIfFailed(resource->Map(0, &readRange, &mappedPtr), "バッファのマップに失敗しました");

            if (desc.InitialData)
            {
                memcpy(mappedPtr, desc.InitialData, desc.SizeInBytes);
            }

            return std::make_unique<DX12Buffer>(resource, mappedPtr, slotSizeInBytes, desc.StrideInBytes, desc.Usage, ringCapacity);
        }

        // 頂点/インデックスバッファは初回アップロード後書き換えないため、CPUから見える(低速な)
        // UPLOADヒープに置きっぱなしにせず、GPUからの読み取りが高速なDEFAULTヒープに作成する。
        // ピクセルシェーダの負荷がほぼ無くGPU側が頂点フェッチ律速になるシャドウパスなどで、
        // UPLOADヒープ配置は実測で数倍のGPU時間差として現れることを確認済み
        const CD3DX12_HEAP_PROPERTIES defaultHeapProps(D3D12_HEAP_TYPE_DEFAULT);
        const CD3DX12_RESOURCE_DESC defaultResourceDesc = CD3DX12_RESOURCE_DESC::Buffer(desc.SizeInBytes);
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        ThrowIfFailed(
            m_Device->CreateCommittedResource(
                &defaultHeapProps, D3D12_HEAP_FLAG_NONE, &defaultResourceDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&resource)),
            "バッファの作成に失敗しました");

        if (desc.InitialData)
        {
            // アップロードヒープの一時バッファ経由でDEFAULTヒープへコピーする
            Microsoft::WRL::ComPtr<ID3D12Resource> uploadBuffer = CreateUploadBuffer(desc.SizeInBytes);

            void* mappedPtr = nullptr;
            const D3D12_RANGE readRange{ 0, 0 };
            ThrowIfFailed(uploadBuffer->Map(0, &readRange, &mappedPtr), "アップロードバッファのマップに失敗しました");
            memcpy(mappedPtr, desc.InitialData, desc.SizeInBytes);
            uploadBuffer->Unmap(0, nullptr);

            m_CommandList->CopyBufferRegion(resource.Get(), 0, uploadBuffer.Get(), 0, desc.SizeInBytes);

            const D3D12_RESOURCE_BARRIER toReadBarrier =
                CD3DX12_RESOURCE_BARRIER::Transition(resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_GENERIC_READ);
            m_CommandList->ResourceBarrier(1, &toReadBarrier);

            // アップロードバッファはこの関数を抜けるまで生存させる必要があるため、ここで同期的に実行完了を待つ
            SubmitAndWaitIdle();
        }
        else
        {
            const D3D12_RESOURCE_BARRIER toReadBarrier =
                CD3DX12_RESOURCE_BARRIER::Transition(resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_GENERIC_READ);
            m_CommandList->ResourceBarrier(1, &toReadBarrier);
        }

        // DEFAULTヒープはCPUからマップできないためnullptrを渡す。頂点/インデックスバッファは
        // 初回アップロード後書き換えない(ringCapacity=1でAdvanceRingAndGetWritePtrは呼ばれない)
        return std::make_unique<DX12Buffer>(resource, nullptr, desc.SizeInBytes, desc.StrideInBytes, desc.Usage, 1);
    }

    std::unique_ptr<IRHIShader> DX12Device::CreateShader(const ShaderDesc& desc)
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

        return std::make_unique<DX12Shader>(desc.Stage, bytecode);
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
        psoDesc.PS = pixelShader->GetBytecode();
        psoDesc.InputLayout = { elements.empty() ? nullptr : elements.data(), static_cast<UINT>(elements.size()) };
        psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        {
            D3D12_RENDER_TARGET_BLEND_DESC& rt = psoDesc.BlendState.RenderTarget[0];
            switch (desc.BlendMode)
            {
            case BlendMode::AlphaBlend:
                rt.BlendEnable = TRUE;
                rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
                rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
                rt.BlendOp = D3D12_BLEND_OP_ADD;
                rt.SrcBlendAlpha = D3D12_BLEND_ONE;
                rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
                rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
                break;
            case BlendMode::Additive:
                rt.BlendEnable = TRUE;
                rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
                rt.DestBlend = D3D12_BLEND_ONE;
                rt.BlendOp = D3D12_BLEND_OP_ADD;
                rt.SrcBlendAlpha = D3D12_BLEND_ONE;
                rt.DestBlendAlpha = D3D12_BLEND_ONE;
                rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
                break;
            case BlendMode::Multiply:
                rt.BlendEnable = TRUE;
                rt.SrcBlend = D3D12_BLEND_DEST_COLOR;
                rt.DestBlend = D3D12_BLEND_ZERO;
                rt.BlendOp = D3D12_BLEND_OP_ADD;
                rt.SrcBlendAlpha = D3D12_BLEND_DEST_ALPHA;
                rt.DestBlendAlpha = D3D12_BLEND_ZERO;
                rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
                break;
            case BlendMode::PremultipliedAlpha:
                rt.BlendEnable = TRUE;
                rt.SrcBlend = D3D12_BLEND_ONE;
                rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
                rt.BlendOp = D3D12_BLEND_OP_ADD;
                rt.SrcBlendAlpha = D3D12_BLEND_ONE;
                rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
                rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
                break;
            case BlendMode::Opaque:
            default:
                rt.BlendEnable = FALSE;
                break;
            }
        }
        psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        psoDesc.DepthStencilState.DepthEnable = desc.HasDepthStencil ? TRUE : FALSE;
        // Reverse-Z: 近平面=1.0/遠平面=0.0にマッピングするため、深度テストの向きもGREATERに反転する
        psoDesc.DepthStencilState.DepthFunc = desc.ReverseZ ? D3D12_COMPARISON_FUNC_GREATER : D3D12_COMPARISON_FUNC_LESS;
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = static_cast<UINT>(desc.RenderTargetFormats.size());
        for (size_t i = 0; i < desc.RenderTargetFormats.size(); ++i)
        {
            psoDesc.RTVFormats[i] = ToDXGIFormat(desc.RenderTargetFormats[i]);
        }
        psoDesc.DSVFormat = desc.HasDepthStencil ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_UNKNOWN;
        psoDesc.SampleDesc.Count = 1;

        Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
        ThrowIfFailed(m_Device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso)), "パイプラインステートの作成に失敗しました");

        return std::make_unique<DX12PipelineState>(pso, desc.Topology);
    }

    std::unique_ptr<IRHITexture> DX12Device::CreateTextureFromImage(const DirectX::TexMetadata& metadata, const DirectX::ScratchImage& image)
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        ThrowIfFailed(
            DirectX::CreateTextureEx(m_Device.Get(), metadata, D3D12_RESOURCE_FLAG_NONE, DirectX::CREATETEX_DEFAULT, &resource),
            "テクスチャの作成に失敗しました");

        std::vector<D3D12_SUBRESOURCE_DATA> subresources;
        ThrowIfFailed(
            DirectX::PrepareUpload(m_Device.Get(), image.GetImages(), image.GetImageCount(), metadata, subresources),
            "アップロードデータの準備に失敗しました");

        const UINT subresourceCount = static_cast<UINT>(subresources.size());
        const D3D12_RESOURCE_DESC destDesc = resource->GetDesc();
        UINT64 requiredSize = 0;
        m_Device->GetCopyableFootprints(&destDesc, 0, subresourceCount, 0, nullptr, nullptr, nullptr, &requiredSize);

        // DirectX::CreateTextureEx はデスクトップ環境ではリソースをD3D12_RESOURCE_STATE_COMMONで作成するため、
        // コピー先として使う前にCOPY_DESTへ明示的に遷移させる必要がある
        const D3D12_RESOURCE_BARRIER toCopyDestBarrier =
            CD3DX12_RESOURCE_BARRIER::Transition(resource.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        m_CommandList->ResourceBarrier(1, &toCopyDestBarrier);

        Microsoft::WRL::ComPtr<ID3D12Resource> uploadBuffer = CreateUploadBuffer(requiredSize);
        UpdateSubresources(m_CommandList.Get(), resource.Get(), uploadBuffer.Get(), 0, 0, subresourceCount, subresources.data());

        const D3D12_RESOURCE_BARRIER toSrvBarrier =
            CD3DX12_RESOURCE_BARRIER::Transition(resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_CommandList->ResourceBarrier(1, &toSrvBarrier);

        const uint32_t srvIndex = m_SrvCpuHeap->Allocate();
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = metadata.format;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (metadata.IsCubemap())
        {
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
            srvDesc.TextureCube.MipLevels = static_cast<UINT>(metadata.mipLevels);
        }
        else
        {
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = static_cast<UINT>(metadata.mipLevels);
        }
        m_Device->CreateShaderResourceView(resource.Get(), &srvDesc, m_SrvCpuHeap->GetCpuHandle(srvIndex));

        auto texture = std::make_unique<DX12Texture>(
            this, resource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, srvIndex, DX12Texture::kInvalid, DX12Texture::kInvalid);

        // アップロードバッファはこの関数を抜けるまで生存させる必要があるため、ここで同期的に実行完了を待つ
        SubmitAndWaitIdle();

        return texture;
    }

    std::unique_ptr<IRHITexture> DX12Device::CreateTextureFromFile(const std::wstring& filePath, bool sRGB)
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

        return CreateTextureFromImage(image.GetMetadata(), image);
    }

    std::unique_ptr<IRHITexture> DX12Device::CreateSolidColorTexture(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
    {
        DirectX::TexMetadata metadata{};
        metadata.width = 1;
        metadata.height = 1;
        metadata.depth = 1;
        metadata.arraySize = 1;
        metadata.mipLevels = 1;
        metadata.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        metadata.dimension = DirectX::TEX_DIMENSION_TEXTURE2D;

        DirectX::ScratchImage image;
        ThrowIfFailed(image.Initialize2D(metadata.format, 1, 1, 1, 1), "1x1テクスチャの作成に失敗しました");

        const uint8_t pixel[4] = { r, g, b, a };
        memcpy(image.GetImage(0, 0, 0)->pixels, pixel, sizeof(pixel));

        return CreateTextureFromImage(metadata, image);
    }

    std::unique_ptr<IRHITexture> DX12Device::CreateRenderTexture(uint32_t width, uint32_t height, Format format)
    {
        const DXGI_FORMAT dxgiFormat = ToDXGIFormat(format);

        D3D12_CLEAR_VALUE clearValue{};
        clearValue.Format = dxgiFormat;
        clearValue.Color[0] = clearValue.Color[1] = clearValue.Color[2] = 0.0f;
        clearValue.Color[3] = 1.0f;

        const CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
        const CD3DX12_RESOURCE_DESC resourceDesc =
            CD3DX12_RESOURCE_DESC::Tex2D(dxgiFormat, width, height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);

        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        ThrowIfFailed(
            m_Device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc, D3D12_RESOURCE_STATE_RENDER_TARGET, &clearValue, IID_PPV_ARGS(&resource)),
            "レンダーテクスチャの作成に失敗しました");

        const uint32_t srvIndex = m_SrvCpuHeap->Allocate();
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = dxgiFormat;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        m_Device->CreateShaderResourceView(resource.Get(), &srvDesc, m_SrvCpuHeap->GetCpuHandle(srvIndex));

        const uint32_t rtvIndex = m_RtvHeap->Allocate();
        m_Device->CreateRenderTargetView(resource.Get(), nullptr, m_RtvHeap->GetCpuHandle(rtvIndex));

        return std::make_unique<DX12Texture>(this, resource, D3D12_RESOURCE_STATE_RENDER_TARGET, srvIndex, rtvIndex, DX12Texture::kInvalid);
    }

    std::unique_ptr<IRHITexture> DX12Device::CreateDepthTexture(uint32_t width, uint32_t height, float clearDepth)
    {
        // 深度テクスチャは後段のライティングパスでサンプリングするためSHADER_RESOURCEも付与し、
        // Typelessフォーマットで作成してDSV/SRVそれぞれに適したビューを個別に張る(DX11実装と同じ方針)。
        // ステンシルは使わないためD32_FLOATにしている(Reverse-Zの精度改善はUNORMでは効果がなく、
        // 浮動小数点フォーマットと組み合わせて初めて意味を持つ)
        D3D12_CLEAR_VALUE clearValue{};
        clearValue.Format = DXGI_FORMAT_D32_FLOAT;
        clearValue.DepthStencil.Depth = clearDepth;
        clearValue.DepthStencil.Stencil = 0;

        const CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
        const CD3DX12_RESOURCE_DESC resourceDesc =
            CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_TYPELESS, width, height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);

        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        ThrowIfFailed(
            m_Device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue, IID_PPV_ARGS(&resource)),
            "深度テクスチャの作成に失敗しました");

        const uint32_t dsvIndex = m_DsvHeap->Allocate();
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        m_Device->CreateDepthStencilView(resource.Get(), &dsvDesc, m_DsvHeap->GetCpuHandle(dsvIndex));

        const uint32_t srvIndex = m_SrvCpuHeap->Allocate();
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        m_Device->CreateShaderResourceView(resource.Get(), &srvDesc, m_SrvCpuHeap->GetCpuHandle(srvIndex));

        return std::make_unique<DX12Texture>(this, resource, D3D12_RESOURCE_STATE_DEPTH_WRITE, srvIndex, DX12Texture::kInvalid, dsvIndex);
    }

    std::unique_ptr<IRHISampler> DX12Device::CreateDefaultSampler()
    {
        D3D12_SAMPLER_DESC samplerDesc{};
        samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        samplerDesc.MaxLOD = D3D12_FLOAT32_MAX;

        const uint32_t index = m_SamplerCpuHeap->Allocate();
        m_Device->CreateSampler(&samplerDesc, m_SamplerCpuHeap->GetCpuHandle(index));

        return std::make_unique<DX12Sampler>(this, index);
    }

    IRHICommandList* DX12Device::GetImmediateCommandList()
    {
        return m_ImmediateCommandList.get();
    }

    std::unique_ptr<IRHIImGuiBackend> DX12Device::CreateImGuiBackend(void* windowHandle)
    {
        return std::make_unique<DX12ImGuiBackend>(this, windowHandle);
    }

    std::unique_ptr<IRHIGPUProfiler> DX12Device::CreateGPUProfiler()
    {
        return std::make_unique<DX12GPUProfiler>(this);
    }

    std::unique_ptr<IRHIDevice> CreateDX12Device()
    {
        auto device = std::make_unique<DX12Device>();
        device->Initialize();
        return device;
    }
}
