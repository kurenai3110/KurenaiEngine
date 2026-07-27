#include "DX12Device.h"

#include <d3dcompiler.h>
#include <d3dx12.h>

#include <DirectXTex.h>

#include <chrono>
#include <cstring>
#include <cwchar>
#include <stdexcept>
#include <string>
#include <vector>

#include "DX12Buffer.h"
#include "DX12CommandList.h"
#include "DX12ComputePipelineState.h"
#include "DX12GPUProfiler.h"
#include "DX12ImGuiBackend.h"
#include "DX12PipelineState.h"
#include "DX12SamplerSet.h"
#include "DX12Shader.h"
#include "DX12SwapChain.h"
#include "DX12Texture.h"
#include "DX12Util.h"
#include "RHI/TextureImage.h"

namespace Kurenai::RHI
{
    namespace
    {
        // シェーダのレジスタ実測値(Sandbox/Shaders/*.hlsl)に基づく固定のルートシグネチャレイアウト
        // t0〜t13。最大はDeferredLighting.hlsl(G-Buffer4枚+スカイボックス+AO+エミッシブ+法線+
        // グローバルIBL3枚+反射プローブのキューブ配列2枚+プローブ一覧のStructuredBuffer)
        constexpr uint32_t kTextureSlotCount = 14;
        // 1つのサンプラーセット(=1つのディスクリプタテーブル)が持つスロット数。
        // s0 = MaterialSampler、s1 = ColorSampler、s2 = DataSampler(役割の定義はShaders/Samplers.hlsli)。
        // どの実体が入るかはパスごとにエンジン側が選んだセットで決まる。
        // 3必要なのはTransparent.hlslが「マテリアル・シャドウマップ・BRDF積分LUT」の3種類を
        // 1回のピクセルシェーダ実行で同時に使うため。
        // 一部のスロットしか宣言しないシェーダーでもテーブルはkSamplerSlotCount個ぶんまとめて
        // バインドされるため、セット生成時に余ったスロットは既定のサンプラーで埋める(CreateSamplerSet参照)
        constexpr uint32_t kSamplerSlotCount = 3;
        // 作成できるサンプラーセットの最大数。セットは初期化時にだけ作られ解放されないため、
        // 用途の種類数(現状はマテリアル用・スクリーン空間用の2つ)に余裕を持たせた値でよい。
        // シェーダ可視Samplerヒープの上限はD3D12の仕様で2048ディスクリプタ
        // (D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE)なので、この程度なら十分収まる
        constexpr uint32_t kMaxSamplerSets = 8;
        // 1フレームあたりに払い出せるSRVテーブルブロック(t0〜t10のkTextureSlotCount個ひと組)の最大数。
        // 1フレーム中の(メッシュ数×パス数)を十分上回る値にしておく。実際に確保するヒープ容量は
        // これのkFrameCount倍(CPUがGPU完了を待たずに次フレームを記録し始めるため、直近kFrameCount
        // フレームぶんのブロックがまだGPUに読まれている可能性がある)
        constexpr uint32_t kMaxSrvTableBlocksPerFrame = 4096;
        // 定数バッファ(Usage==Constant)がリングとして持つスロット数。CPUがGPU完了を待たずに次フレームを
        // 記録し始めるため、直近kFrameCountフレームぶんのUpdateBuffer回数(メッシュ数など)を
        // 十分上回る値にしておく
        constexpr uint32_t kConstantBufferRingCapacity = 8192;

        // コンピュートシェーダー用ルートシグネチャのSRV/UAVディスクリプタテーブルレイアウト(t0〜t3, u0〜u3)
        constexpr uint32_t kComputeSrvSlotCount = 4;
        constexpr uint32_t kComputeUavSlotCount = 4;
        constexpr uint32_t kComputeTableSlotCount = kComputeSrvSlotCount + kComputeUavSlotCount;
        // 1フレームあたりに払い出せるコンピュートSRV+UAVテーブルブロックの最大数(Dispatch呼び出し回数の上限)。
        // 反射プローブのベイクは1プローブあたり6(面コピー)+6(イラディアンス)+36(プリフィルタ6ミップ×6面)=48回
        // ディスパッチし、複数プローブを同一フレームでまとめて焼くため、プローブ数ぶんの余裕が要る
        constexpr uint32_t kMaxComputeDispatchesPerFrame = 1024;
        // グラフィックス用SRVテーブル領域の1フレームあたりのディスクリプタ数。m_ShaderVisibleSrvHeap内では
        // 先頭からこの数×kFrameCountぶんをグラフィックス用が占有し、コンピュートシェーダー用のSRV+UAVテーブルは
        // それより後ろの区画に別リングとして確保する(kFrameCountはDX12Deviceのprivateメンバのため、
        // 実際の掛け合わせはこれを参照できるメンバ関数側で行う)
        constexpr uint32_t kGraphicsSrvHeapCapacityPerFrame = kTextureSlotCount * kMaxSrvTableBlocksPerFrame;
        constexpr uint32_t kComputeSrvHeapCapacityPerFrame = kComputeTableSlotCount * kMaxComputeDispatchesPerFrame;

        // m_SrvCpuHeap(非シェーダー可視。テクスチャ/構造化バッファ作成時にCreateShaderResourceView等の
        // 恒久的なビューを1つずつ確保する)の総容量。LoadSceneはm_Model = Assets::LoadModel(...)のように
        // 新シーンのテクスチャを先にすべて作成してから旧シーンのテクスチャを解放する(右辺の評価が先に
        // 終わってから代入される)ため、切り替え中は旧シーン+新シーンのテクスチャが一時的に同時に
        // 確保された状態になる。Bistro Exteriorのような大規模シーンではこの一時的な二重確保だけで
        // 数百ディスクリプタに達するため、余裕を持った値にしておく(非シェーダー可視ヒープでCPUメモリの
        // みを消費するため、大きめにしても実害はない)
        constexpr uint32_t kSrvCpuHeapCapacity = 4096;

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

        if (m_UploadFenceEvent)
        {
            CloseHandle(m_UploadFenceEvent);
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

        // リソースアップロード専用のコマンドリスト/アロケータ/フェンス(m_UploadCommandListのコメント参照)
        ThrowIfFailed(
            m_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_UploadCommandAllocator)),
            "アップロード用コマンドアロケータの作成に失敗しました");
        ThrowIfFailed(
            m_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_UploadCommandAllocator.Get(), nullptr, IID_PPV_ARGS(&m_UploadCommandList)),
            "アップロード用コマンドリストの作成に失敗しました");
        ThrowIfFailed(m_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_UploadFence)), "アップロード用フェンスの作成に失敗しました");
        m_UploadFenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!m_UploadFenceEvent)
        {
            throw std::runtime_error("アップロード用フェンスイベントの作成に失敗しました");
        }

        m_RtvHeap = std::make_unique<DX12DescriptorHeap>(m_Device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 16, false);
        m_DsvHeap = std::make_unique<DX12DescriptorHeap>(m_Device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 8, false);
        m_SrvCpuHeap = std::make_unique<DX12DescriptorHeap>(m_Device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSrvCpuHeapCapacity, false);
        // 1フレーム分のコマンドをまとめて記録してから1回だけ実行する設計のため、描画のたびに
        // 新しいkTextureSlotCount個のブロックを払い出せるよう、1フレームに必要な最大数を見込んで確保する。
        // さらにCPUがGPU完了を待たずに次フレームを記録し始めるため、kFrameCountフレームぶんの容量を持たせる
        // コンピュートシェーダー用のSRV+UAVテーブル(kComputeSrvHeapCapacityPerFrame×kFrameCount)ぶんも
        // 同じシェーダ可視ヒープの後ろの区画に確保する(DX12は同時にバインドできるCBV_SRV_UAVヒープが
        // 1つだけのため、グラフィックス用と共存させる必要がある。詳細はAllocateComputeTableBlock参照)
        m_ShaderVisibleSrvHeap = std::make_unique<DX12DescriptorHeap>(
            m_Device.Get(),
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
            (kGraphicsSrvHeapCapacityPerFrame + kComputeSrvHeapCapacityPerFrame) * kFrameCount,
            true);
        // サンプラーセットはCreateSamplerSetで連続ブロックとして払い出す(kMaxSamplerSets個ぶん)。
        // 加えて先頭に1ブロックぶんのフォールバックを確保しておく(下記参照)
        m_ShaderVisibleSamplerHeap = std::make_unique<DX12DescriptorHeap>(
            m_Device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplerSlotCount * (kMaxSamplerSets + 1), true);

        // 上位層が一度もSetSamplerSetを呼ばないままDrawした場合に備えたフォールバックのブロック。
        // ルートディスクリプタテーブルは常にkSamplerSlotCount個ぶんを指すため、未初期化の
        // ディスクリプタを指してしまうと動作が未定義になる。ヒープ先頭の1ブロックを既定の
        // サンプラーで埋めておき、DX12CommandListはセットが未設定のあいだここを指す
        {
            m_FallbackSamplerSetBase = m_ShaderVisibleSamplerHeap->AllocateBlock(kSamplerSlotCount);

            D3D12_SAMPLER_DESC defaultSamplerDesc{};
            defaultSamplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            defaultSamplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            defaultSamplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            defaultSamplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            defaultSamplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
            defaultSamplerDesc.MaxLOD = D3D12_FLOAT32_MAX;
            for (uint32_t slot = 0; slot < kSamplerSlotCount; ++slot)
            {
                m_Device->CreateSampler(&defaultSamplerDesc, m_ShaderVisibleSamplerHeap->GetCpuHandle(m_FallbackSamplerSetBase + slot));
            }
        }

        CreateRootSignature();
        CreateComputeRootSignature();

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
        rootSigDesc.Init(4, rootParams, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);

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

    void DX12Device::UploadSubmitAndWait()
    {
        ThrowIfFailed(m_UploadCommandList->Close(), "アップロード用コマンドリストのクローズに失敗しました");
        ID3D12CommandList* commandLists[] = { m_UploadCommandList.Get() };
        m_CommandQueue->ExecuteCommandLists(1, commandLists);

        // アップロードバッファ(呼び出し元のCreateUploadBufferで確保した一時リソース)はこの完了待ちを
        // 抜けるまで生存させる必要があるため、ExecuteCommandLists後にフェンスで同期的に待つ
        const uint64_t fenceValueToWaitFor = ++m_UploadFenceValue;
        ThrowIfFailed(m_CommandQueue->Signal(m_UploadFence.Get(), fenceValueToWaitFor), "アップロード用フェンスのシグナルに失敗しました");
        if (m_UploadFence->GetCompletedValue() < fenceValueToWaitFor)
        {
            ThrowIfFailed(m_UploadFence->SetEventOnCompletion(fenceValueToWaitFor, m_UploadFenceEvent), "アップロード用フェンスイベントの設定に失敗しました");
            WaitForSingleObject(m_UploadFenceEvent, INFINITE);
        }

        ThrowIfFailed(m_UploadCommandAllocator->Reset(), "アップロード用コマンドアロケータのリセットに失敗しました");
        ThrowIfFailed(m_UploadCommandList->Reset(m_UploadCommandAllocator.Get(), nullptr), "アップロード用コマンドリストのリセットに失敗しました");
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
        m_ComputeTableBlocksUsedThisFrame = 0;
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
        const uint32_t totalCapacity = kGraphicsSrvHeapCapacityPerFrame * kFrameCount;
        const uint32_t base = m_NextSrvTableIndex;
        m_NextSrvTableIndex = (m_NextSrvTableIndex + count) % totalCapacity;
        return base;
    }

    uint32_t DX12Device::AllocateComputeTableBlock(uint32_t count)
    {
        m_ComputeTableBlocksUsedThisFrame += count;
        if (m_ComputeTableBlocksUsedThisFrame > kComputeSrvHeapCapacityPerFrame)
        {
            throw std::runtime_error("コンピュートSRV/UAVテーブルブロックの上限を超えました(1フレーム内のDispatch回数が多すぎます)");
        }

        // グラフィックス用の区画(先頭からkGraphicsSrvHeapCapacityPerFrame×kFrameCount個)より後ろを、
        // コンピュート専用のリングとして扱う。考え方はAllocateSrvTableBlockと同じ
        const uint32_t regionBase = kGraphicsSrvHeapCapacityPerFrame * kFrameCount;
        const uint32_t totalCapacity = kComputeSrvHeapCapacityPerFrame * kFrameCount;
        const uint32_t base = m_NextComputeTableIndex;
        m_NextComputeTableIndex = (m_NextComputeTableIndex + count) % totalCapacity;
        return regionBase + base;
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
        // 初期データのアップロードはm_CommandList(Renderスレッドが毎フレーム使うコマンドリスト)ではなく
        // m_UploadCommandList専用のコマンドリストで行う(詳細はm_UploadCommandListのコメント参照)。
        // この関数はLoadScene等どのスレッドからも呼ばれ得るため、m_UploadCommandListへの記録から
        // UploadSubmitAndWait()完了までをミューテックスで直列化する
        std::lock_guard<std::mutex> uploadLock(m_UploadMutex);

        // 構造化バッファ(RWStructuredBuffer)はコンピュートシェーダーからのUAV書き込みが前提のため、
        // GPUからの読み書きが高速なDEFAULTヒープにD3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS付きで作成する
        if (desc.Usage == BufferUsage::Structured)
        {
            const CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
            const CD3DX12_RESOURCE_DESC resourceDesc =
                CD3DX12_RESOURCE_DESC::Buffer(desc.SizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

            Microsoft::WRL::ComPtr<ID3D12Resource> resource;
            if (desc.InitialData)
            {
                ThrowIfFailed(
                    m_Device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource)),
                    "構造化バッファの作成に失敗しました");

                Microsoft::WRL::ComPtr<ID3D12Resource> uploadBuffer = CreateUploadBuffer(desc.SizeInBytes);

                void* mappedPtr = nullptr;
                const D3D12_RANGE readRange{ 0, 0 };
                ThrowIfFailed(uploadBuffer->Map(0, &readRange, &mappedPtr), "アップロードバッファのマップに失敗しました");
                memcpy(mappedPtr, desc.InitialData, desc.SizeInBytes);
                uploadBuffer->Unmap(0, nullptr);

                const D3D12_RESOURCE_BARRIER toCopyDestBarrier =
                    CD3DX12_RESOURCE_BARRIER::Transition(resource.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
                m_UploadCommandList->ResourceBarrier(1, &toCopyDestBarrier);
                m_UploadCommandList->CopyBufferRegion(resource.Get(), 0, uploadBuffer.Get(), 0, desc.SizeInBytes);
                const D3D12_RESOURCE_BARRIER toUavBarrier =
                    CD3DX12_RESOURCE_BARRIER::Transition(resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                m_UploadCommandList->ResourceBarrier(1, &toUavBarrier);

                // アップロードバッファはこの関数を抜けるまで生存させる必要があるため、ここで同期的に実行完了を待つ
                UploadSubmitAndWait();
            }
            else
            {
                // 初期データがない場合はコマンドリストでの状態遷移を経由せず、作成時点で直接
                // UNORDERED_ACCESS状態にしておく。m_UploadCommandListは初期データがある呼び出しでのみ
                // Submitされるため、ここでバリアだけ積んで済ませると、他の初期データ付きバッファ/
                // テクスチャの作成が一度も起きないまま先にこのリソースがコンピュートシェーダーから
                // 使われた場合に、遷移が未実行のまま参照されてしまう
                ThrowIfFailed(
                    m_Device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&resource)),
                    "構造化バッファの作成に失敗しました");
            }

            const uint32_t uavIndex = m_SrvCpuHeap->Allocate();
            D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
            uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            uavDesc.Format = DXGI_FORMAT_UNKNOWN;
            uavDesc.Buffer.NumElements = desc.SizeInBytes / desc.StrideInBytes;
            uavDesc.Buffer.StructureByteStride = desc.StrideInBytes;
            m_Device->CreateUnorderedAccessView(resource.Get(), nullptr, &uavDesc, m_SrvCpuHeap->GetCpuHandle(uavIndex));

            return std::make_unique<DX12Buffer>(this, resource, uavIndex, desc.SizeInBytes, desc.StrideInBytes);
        }

        // 読み取り専用の構造化バッファ(StructuredBuffer<T>)。ピクセルシェーダが毎フレーム読むため
        // 本体はDEFAULTヒープに置く(UPLOADヒープはCPUから見える代わりにGPU読み取りが低速なため、
        // ピクセルごとに読まれる用途には向かない。頂点/インデックスバッファをDEFAULTヒープに
        // 置いている理由と同じ)。CPUからの書き込みはUPLOADヒープのステージングリング経由で行い、
        // 実際のDEFAULTヒープへのコピーはDX12CommandList::UpdateBufferがCopyBufferRegionで行う
        if (desc.Usage == BufferUsage::StructuredReadOnly)
        {
            const CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
            const CD3DX12_RESOURCE_DESC resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(desc.SizeInBytes);

            // 初期データを持たないため、構造化バッファ(UAVなし初期化)と同様に作成時点で直接
            // PIXEL_SHADER_RESOURCE状態にしておく。これによりUpdateBufferが一度も呼ばれなくても
            // (例: ライトが1つも無いフレーム)SetShaderResourceBufferで安全にバインドできる
            Microsoft::WRL::ComPtr<ID3D12Resource> resource;
            ThrowIfFailed(
                m_Device->CreateCommittedResource(
                    &heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&resource)),
                "読み取り専用構造化バッファの作成に失敗しました");

            const uint32_t srvIndex = m_SrvCpuHeap->Allocate();
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = DXGI_FORMAT_UNKNOWN;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Buffer.NumElements = desc.SizeInBytes / desc.StrideInBytes;
            srvDesc.Buffer.StructureByteStride = desc.StrideInBytes;
            m_Device->CreateShaderResourceView(resource.Get(), &srvDesc, m_SrvCpuHeap->GetCpuHandle(srvIndex));

            // CPUはGPU完了を待たずに次フレームの記録を始める(kFrameCount)ため、直近フレームぶんの
            // 書き込みが同時に生存できるようkFrameCount+1スロットのステージングリングを持たせる
            constexpr uint32_t kStructuredReadOnlyUploadRingCapacity = kFrameCount + 1;
            Microsoft::WRL::ComPtr<ID3D12Resource> uploadResource =
                CreateUploadBuffer(static_cast<uint64_t>(desc.SizeInBytes) * kStructuredReadOnlyUploadRingCapacity);

            void* uploadMappedPtr = nullptr;
            const D3D12_RANGE readRange{ 0, 0 };
            ThrowIfFailed(uploadResource->Map(0, &readRange, &uploadMappedPtr), "ステージングバッファのマップに失敗しました");

            return std::make_unique<DX12Buffer>(
                this,
                resource,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                srvIndex,
                uploadResource,
                uploadMappedPtr,
                desc.SizeInBytes,
                desc.StrideInBytes,
                kStructuredReadOnlyUploadRingCapacity);
        }

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

        if (desc.InitialData)
        {
            ThrowIfFailed(
                m_Device->CreateCommittedResource(
                    &defaultHeapProps, D3D12_HEAP_FLAG_NONE, &defaultResourceDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&resource)),
                "バッファの作成に失敗しました");

            // アップロードヒープの一時バッファ経由でDEFAULTヒープへコピーする
            Microsoft::WRL::ComPtr<ID3D12Resource> uploadBuffer = CreateUploadBuffer(desc.SizeInBytes);

            void* mappedPtr = nullptr;
            const D3D12_RANGE readRange{ 0, 0 };
            ThrowIfFailed(uploadBuffer->Map(0, &readRange, &mappedPtr), "アップロードバッファのマップに失敗しました");
            memcpy(mappedPtr, desc.InitialData, desc.SizeInBytes);
            uploadBuffer->Unmap(0, nullptr);

            m_UploadCommandList->CopyBufferRegion(resource.Get(), 0, uploadBuffer.Get(), 0, desc.SizeInBytes);

            const D3D12_RESOURCE_BARRIER toReadBarrier =
                CD3DX12_RESOURCE_BARRIER::Transition(resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_GENERIC_READ);
            m_UploadCommandList->ResourceBarrier(1, &toReadBarrier);

            // アップロードバッファはこの関数を抜けるまで生存させる必要があるため、ここで同期的に実行完了を待つ
            UploadSubmitAndWait();
        }
        else
        {
            // 初期データがない場合はコマンドリストでの状態遷移を経由せず、作成時点で直接
            // GENERIC_READ状態にしておく(構造化バッファ側のコメント参照)
            ThrowIfFailed(
                m_Device->CreateCommittedResource(
                    &defaultHeapProps, D3D12_HEAP_FLAG_NONE, &defaultResourceDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&resource)),
                "バッファの作成に失敗しました");
        }

        // DEFAULTヒープはCPUからマップできないためnullptrを渡す。頂点/インデックスバッファは
        // 初回アップロード後書き換えない(ringCapacity=1でAdvanceRingAndGetWritePtrは呼ばれない)
        return std::make_unique<DX12Buffer>(resource, nullptr, desc.SizeInBytes, desc.StrideInBytes, desc.Usage, 1);
    }

    std::unique_ptr<IRHIShader> DX12Device::CreateShader(const ShaderDesc& desc)
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
            Core::Logger::Error("DX12", message);
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
        psoDesc.DepthStencilState.DepthWriteMask = desc.DepthWriteEnabled ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
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

    std::unique_ptr<IRHITexture> DX12Device::CreateTextureResourceFromImage(const DirectX::TexMetadata& metadata, const DirectX::ScratchImage& image)
    {
        // 初期データのアップロードはm_CommandList(Renderスレッドが毎フレーム使うコマンドリスト)ではなく
        // m_UploadCommandList専用のコマンドリストで行う(詳細はm_UploadCommandListのコメント参照)。
        // この関数はLoadScene等どのスレッドからも呼ばれ得るため、m_UploadCommandListへの記録から
        // UploadSubmitAndWait()完了までをミューテックスで直列化する
        std::lock_guard<std::mutex> uploadLock(m_UploadMutex);

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
        m_UploadCommandList->ResourceBarrier(1, &toCopyDestBarrier);

        Microsoft::WRL::ComPtr<ID3D12Resource> uploadBuffer = CreateUploadBuffer(requiredSize);
        UpdateSubresources(m_UploadCommandList.Get(), resource.Get(), uploadBuffer.Get(), 0, 0, subresourceCount, subresources.data());

        const D3D12_RESOURCE_BARRIER toSrvBarrier =
            CD3DX12_RESOURCE_BARRIER::Transition(resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_UploadCommandList->ResourceBarrier(1, &toSrvBarrier);

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
        UploadSubmitAndWait();

        return texture;
    }

    std::unique_ptr<IRHITexture> DX12Device::CreateTextureFromFile(const std::wstring& filePath, bool sRGB)
    {
        return CreateTextureFromImage(TextureImage::LoadFromFile(filePath, sRGB));
    }

    std::unique_ptr<IRHITexture> DX12Device::CreateTextureFromImage(const TextureImage& image)
    {
        return CreateTextureResourceFromImage(image.GetMetadata(), image.GetImage());
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

        return CreateTextureResourceFromImage(metadata, image);
    }

    std::unique_ptr<IRHITexture> DX12Device::CreateTextureFromMemory(uint32_t width, uint32_t height, const void* pixelsRGBA8)
    {
        DirectX::TexMetadata metadata{};
        metadata.width = width;
        metadata.height = height;
        metadata.depth = 1;
        metadata.arraySize = 1;
        metadata.mipLevels = 1;
        metadata.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        metadata.dimension = DirectX::TEX_DIMENSION_TEXTURE2D;

        DirectX::ScratchImage image;
        ThrowIfFailed(image.Initialize2D(metadata.format, width, height, 1, 1), "テクスチャの作成に失敗しました");

        // 入力(pixelsRGBA8)はタイトパッキング(1行=width*4バイト)だが、ScratchImageの行ピッチは
        // アライメントの都合で異なる場合があるため、行ごとにコピーする
        const DirectX::Image* image0 = image.GetImage(0, 0, 0);
        const uint8_t* src = static_cast<const uint8_t*>(pixelsRGBA8);
        for (uint32_t y = 0; y < height; ++y)
        {
            memcpy(image0->pixels + y * image0->rowPitch, src + static_cast<size_t>(y) * width * 4, static_cast<size_t>(width) * 4);
        }

        return CreateTextureResourceFromImage(metadata, image);
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

    std::unique_ptr<IRHITexture> DX12Device::CreateUAVTexture(uint32_t width, uint32_t height, Format format)
    {
        const DXGI_FORMAT dxgiFormat = ToDXGIFormat(format);

        const CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
        const CD3DX12_RESOURCE_DESC resourceDesc =
            CD3DX12_RESOURCE_DESC::Tex2D(dxgiFormat, width, height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        ThrowIfFailed(
            m_Device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&resource)),
            "UAVテクスチャの作成に失敗しました");

        const uint32_t srvIndex = m_SrvCpuHeap->Allocate();
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = dxgiFormat;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        m_Device->CreateShaderResourceView(resource.Get(), &srvDesc, m_SrvCpuHeap->GetCpuHandle(srvIndex));

        const uint32_t uavIndex = m_SrvCpuHeap->Allocate();
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = dxgiFormat;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        m_Device->CreateUnorderedAccessView(resource.Get(), nullptr, &uavDesc, m_SrvCpuHeap->GetCpuHandle(uavIndex));

        return std::make_unique<DX12Texture>(
            this, resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srvIndex, DX12Texture::kInvalid, DX12Texture::kInvalid, uavIndex);
    }

    std::unique_ptr<IRHITexture> DX12Device::CreateHiZTexture(uint32_t width, uint32_t height, uint32_t mipLevels)
    {
        return CreateMippedUAVTexture(width, height, Format::R32_Float, mipLevels);
    }

    std::unique_ptr<IRHITexture> DX12Device::CreateMippedUAVTexture(uint32_t width, uint32_t height, Format format, uint32_t mipLevels)
    {
        const DXGI_FORMAT dxgiFormat = ToDXGIFormat(format);

        const CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
        const CD3DX12_RESOURCE_DESC resourceDesc = CD3DX12_RESOURCE_DESC::Tex2D(
            dxgiFormat, width, height, 1, static_cast<UINT16>(mipLevels), 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        ThrowIfFailed(
            m_Device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&resource)),
            "ミップ付きUAVテクスチャの作成に失敗しました");

        // 全ミップを見るSRV(MipLevels=全指定)。デバッグ表示などでSampleLevelにより任意のミップを読む用
        const uint32_t srvIndex = m_SrvCpuHeap->Allocate();
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = dxgiFormat;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = mipLevels;
        m_Device->CreateShaderResourceView(resource.Get(), &srvDesc, m_SrvCpuHeap->GetCpuHandle(srvIndex));

        // ミップごとに単一ミップのUAVを張り、コンピュートシェーダーがミップ単位で書き込めるようにする
        // (Hi-Zの「前段ミップを読んで次段へ書く」ダウンサンプルだけでなく、IBLプリフィルタ済み鏡面マップの
        // 「ミップごとに異なるラフネスで独立に畳み込む」用途でも同じ仕組みを再利用する)
        std::vector<uint32_t> mipUavIndices;
        mipUavIndices.reserve(mipLevels);
        for (uint32_t mip = 0; mip < mipLevels; ++mip)
        {
            const uint32_t uavIndex = m_SrvCpuHeap->Allocate();
            D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
            uavDesc.Format = dxgiFormat;
            uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            uavDesc.Texture2D.MipSlice = mip;
            m_Device->CreateUnorderedAccessView(resource.Get(), nullptr, &uavDesc, m_SrvCpuHeap->GetCpuHandle(uavIndex));
            mipUavIndices.push_back(uavIndex);
        }

        return std::make_unique<DX12Texture>(
            this, resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srvIndex, DX12Texture::kInvalid, DX12Texture::kInvalid,
            DX12Texture::kInvalid, std::move(mipUavIndices));
    }

    std::unique_ptr<IRHITexture> DX12Device::CreateUAVTextureCube(uint32_t size, Format format)
    {
        return CreateMippedUAVTextureCube(size, format, 1);
    }

    std::unique_ptr<IRHITexture> DX12Device::CreateMippedUAVTextureCube(uint32_t size, Format format, uint32_t mipLevels)
    {
        // cubeCount=1のときはSRVをTextureCubeArrayではなくTextureCubeとして張る(HLSL側の
        // TextureCube宣言と一致させるため。IBLConvolve.hlsl等)
        return CreateCubeTextureInternal(size, format, mipLevels, 1, false);
    }

    std::unique_ptr<IRHITexture> DX12Device::CreateMippedUAVTextureCubeArray(
        uint32_t size, Format format, uint32_t mipLevels, uint32_t cubeCount)
    {
        return CreateCubeTextureInternal(size, format, mipLevels, cubeCount, true);
    }

    std::unique_ptr<IRHITexture> DX12Device::CreateCubeTextureInternal(
        uint32_t size, Format format, uint32_t mipLevels, uint32_t cubeCount, bool asArray)
    {
        if (size == 0 || mipLevels == 0 || cubeCount == 0)
        {
            const std::string message =
                "キューブマップUAVテクスチャの作成に失敗しました: サイズ・ミップ数・キューブ数はいずれも1以上である必要があります (size=" +
                std::to_string(size) + ", mipLevels=" + std::to_string(mipLevels) + ", cubeCount=" + std::to_string(cubeCount) + ")";
            Core::Logger::Error("DX12", message);
            throw std::runtime_error(message);
        }

        // D3D12のTexture2D配列は最大2048スライス。キューブマップは1枚あたり6スライス消費する
        const uint32_t arraySize = cubeCount * DX12Texture::kCubeFaceCount;
        if (arraySize > D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION)
        {
            const std::string message =
                "キューブマップUAVテクスチャの作成に失敗しました: 配列スライス数が上限を超えています (cubeCount=" +
                std::to_string(cubeCount) + ", 必要スライス数=" + std::to_string(arraySize) +
                ", 上限=" + std::to_string(D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION) + ")";
            Core::Logger::Error("DX12", message);
            throw std::runtime_error(message);
        }

        const DXGI_FORMAT dxgiFormat = ToDXGIFormat(format);

        const CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
        const CD3DX12_RESOURCE_DESC resourceDesc = CD3DX12_RESOURCE_DESC::Tex2D(
            dxgiFormat, size, size, static_cast<UINT16>(arraySize), static_cast<UINT16>(mipLevels),
            1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        ThrowIfFailed(
            m_Device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&resource)),
            "キューブマップUAVテクスチャの作成に失敗しました");

        // 全6面・全ミップを1枚のTextureCube(配列版はTextureCubeArray)として読むSRV
        // (サンプリング側、DeferredLighting.hlsl等)
        const uint32_t srvIndex = m_SrvCpuHeap->Allocate();
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = dxgiFormat;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (asArray)
        {
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
            srvDesc.TextureCubeArray.MostDetailedMip = 0;
            srvDesc.TextureCubeArray.MipLevels = mipLevels;
            srvDesc.TextureCubeArray.First2DArrayFace = 0;
            srvDesc.TextureCubeArray.NumCubes = cubeCount;
        }
        else
        {
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
            srvDesc.TextureCube.MostDetailedMip = 0;
            srvDesc.TextureCube.MipLevels = mipLevels;
        }
        m_Device->CreateShaderResourceView(resource.Get(), &srvDesc, m_SrvCpuHeap->GetCpuHandle(srvIndex));

        // キューブ×面×ミップの組み合わせごとに単一配列スライス・単一ミップのUAV(Texture2DArray、要素数1)を
        // 張り、コンピュートシェーダーが面ごとに1回ずつディスパッチして書き込めるようにする(HLSL側は
        // RWTexture2DArrayとして宣言する必要がある。IBLConvolve.hlsl参照)。
        // (mip*cubeCount + cubeIndex)*kCubeFaceCount + face の順でフラットに格納する
        // (DX12Texture::GetCubeUavCpuHandle参照)
        std::vector<uint32_t> mipUavIndices;
        mipUavIndices.reserve(static_cast<size_t>(mipLevels) * arraySize);
        for (uint32_t mip = 0; mip < mipLevels; ++mip)
        {
            for (uint32_t cube = 0; cube < cubeCount; ++cube)
            {
                for (uint32_t face = 0; face < DX12Texture::kCubeFaceCount; ++face)
                {
                    const uint32_t uavIndex = m_SrvCpuHeap->Allocate();
                    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
                    uavDesc.Format = dxgiFormat;
                    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                    uavDesc.Texture2DArray.MipSlice = mip;
                    uavDesc.Texture2DArray.FirstArraySlice = cube * DX12Texture::kCubeFaceCount + face;
                    uavDesc.Texture2DArray.ArraySize = 1;
                    m_Device->CreateUnorderedAccessView(resource.Get(), nullptr, &uavDesc, m_SrvCpuHeap->GetCpuHandle(uavIndex));
                    mipUavIndices.push_back(uavIndex);
                }
            }
        }

        return std::make_unique<DX12Texture>(
            this, resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srvIndex, DX12Texture::kInvalid, DX12Texture::kInvalid,
            DX12Texture::kInvalid, std::move(mipUavIndices), cubeCount);
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

    std::unique_ptr<IRHISamplerSet> DX12Device::CreateSamplerSet(const SamplerDesc* descs, uint32_t count)
    {
        if (!descs || count == 0)
        {
            Core::Logger::Error("DX12", "CreateSamplerSet: サンプラー記述子が指定されていません");
            throw std::runtime_error("CreateSamplerSetにサンプラー記述子が指定されていません");
        }

        if (count > kSamplerSlotCount)
        {
            Core::Logger::Warning(
                "DX12",
                "CreateSamplerSet: 指定されたサンプラー数(" + std::to_string(count) + ")がスロット数(" +
                    std::to_string(kSamplerSlotCount) + ")を超えているため、超過分は無視されます");
            count = kSamplerSlotCount;
        }

        // シェーダ可視ヒープ上に連続したkSamplerSlotCount個のブロックを確保し、そこへ直接書き込む。
        // シェーダ可視Samplerヒープに対するCreateSamplerはCPUからの書き込みとして許可されており、
        // このAPIは描画開始前にのみ呼ばれる約束(IRHIDevice::CreateSamplerSet参照)なので、
        // GPUが読んでいる最中のディスクリプタを壊すことはない
        const uint32_t baseIndex = m_ShaderVisibleSamplerHeap->AllocateBlock(kSamplerSlotCount);

        for (uint32_t slot = 0; slot < kSamplerSlotCount; ++slot)
        {
            D3D12_SAMPLER_DESC samplerDesc{};

            if (slot < count)
            {
                const SamplerDesc& desc = descs[slot];

                switch (desc.Filter)
                {
                case SamplerFilter::Anisotropic:
                    samplerDesc.Filter = D3D12_FILTER_ANISOTROPIC;
                    break;
                case SamplerFilter::Point:
                    samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
                    break;
                case SamplerFilter::Linear:
                    samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
                    break;
                default:
                    Core::Logger::Warning(
                        "DX12",
                        "CreateSamplerSet: 未知のSamplerFilter(" + std::to_string(static_cast<int>(desc.Filter)) +
                            ")が指定されたためLinearで代用します");
                    samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
                    break;
                }

                D3D12_TEXTURE_ADDRESS_MODE addressMode = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
                switch (desc.AddressMode)
                {
                case SamplerAddressMode::Clamp:
                    addressMode = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                    break;
                case SamplerAddressMode::Wrap:
                    addressMode = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
                    break;
                default:
                    Core::Logger::Warning(
                        "DX12",
                        "CreateSamplerSet: 未知のSamplerAddressMode(" + std::to_string(static_cast<int>(desc.AddressMode)) +
                            ")が指定されたためWrapで代用します");
                    addressMode = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
                    break;
                }
                samplerDesc.AddressU = addressMode;
                samplerDesc.AddressV = addressMode;
                samplerDesc.AddressW = addressMode;
                // MaxAnisotropyはFilterがANISOTROPICでない場合ハードウェア側で無視されるため、常に設定してよい
                samplerDesc.MaxAnisotropy = desc.MaxAnisotropy;
            }
            else
            {
                // 呼び出し側が指定しなかったスロット。テーブルはkSamplerSlotCount個ぶんまとめて
                // バインドされ、未初期化のディスクリプタが含まれると動作が未定義になるため埋めておく
                samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
                samplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                samplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            }

            samplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
            samplerDesc.MaxLOD = D3D12_FLOAT32_MAX;

            m_Device->CreateSampler(&samplerDesc, m_ShaderVisibleSamplerHeap->GetCpuHandle(baseIndex + slot));
        }

        return std::make_unique<DX12SamplerSet>(this, baseIndex);
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
