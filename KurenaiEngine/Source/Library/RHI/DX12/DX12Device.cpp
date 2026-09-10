#include "DX12Device.h"
#include "DX12DeviceInternal.h"

#include <d3dx12.h>

#include <DirectXTex.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "DX12AccelerationStructure.h"
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
#include "Core/StringUtil.h"
#include "RHI/DXGIFormatUtil.h"
#include "RHI/RHIBindingLimits.h"
#include "RHI/PipelineStateNormalize.h"
#include "RHI/ReadbackUtil.h"
#include "RHI/RHIReadbackFormat.h"
#include "RHI/RHIShaderPackage.h"
#include "RHI/TextureImage.h"

namespace Kurenai::RHI
{
    // 定数と変換関数は DX12DeviceInternal.h(この .cpp 群だけが読む内部ヘッダー)にある
    using namespace DX12Internal;

    // 実行中のGPUが何かをログに残す。どのGPUで測った値なのかが分からないと性能の記録が
    // 後から比較できなくなるため、レイトレーシング等の対応状況ログと並べてここで出す。
    // 診断目的の情報であり、取得に失敗しても描画は続行できるので例外は投げない
    bool DX12Device::GetVideoMemoryUsage(uint64_t& outUsedBytes, uint64_t& outBudgetBytes) const
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

    void DX12Device::LogAdapterInfo()
    {
        const LUID deviceLuid = m_Device->GetAdapterLuid();

        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        for (UINT index = 0; m_Factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND; ++index)
        {
            DXGI_ADAPTER_DESC1 desc{};
            if (FAILED(adapter->GetDesc1(&desc)))
            {
                continue;
            }

            // D3D12CreateDeviceへnullptrを渡しているため、実際に使われたアダプタは
            // デバイスのLUIDと一致するものを探して特定する
            if (desc.AdapterLuid.LowPart != deviceLuid.LowPart || desc.AdapterLuid.HighPart != deviceLuid.HighPart)
            {
                continue;
            }

            // VRAM使用量の問い合わせ(QueryVideoMemoryInfo)はIDXGIAdapter3にしかないため、
            // ここで見つけたアダプタを控えておく。取れなくてもGPU名のログは続ける
            if (FAILED(adapter.As(&m_Adapter)))
            {
                Core::Logger::Warning("DX12", "IDXGIAdapter3を取得できませんでした(VRAM使用量を表示できません)");
            }

            constexpr uint64_t kBytesPerMiB = 1024ull * 1024ull;
            Core::Logger::Info(
                "DX12",
                "GPU: " + Core::WideToUtf8(desc.Description) + " (専用VRAM " +
                    std::to_string(desc.DedicatedVideoMemory / kBytesPerMiB) + "MB / 専用システムメモリ " +
                    std::to_string(desc.DedicatedSystemMemory / kBytesPerMiB) + "MB / 共有システムメモリ " +
                    std::to_string(desc.SharedSystemMemory / kBytesPerMiB) + "MB)");
            return;
        }

        Core::Logger::Warning("DX12", "使用中のDXGIアダプタを特定できませんでした(GPU名をログに残せません)");
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

        LogAdapterInfo();

#if defined(_DEBUG)
        // デバッグレイヤーの指摘はそのままではデバッガの出力ウィンドウにしか出ず、
        // デバッガを繋がずに実行した場合に気付けない。ID3D12InfoQueueに溜まったメッセージを
        // 毎フレーム引き取ってエンジンのログ(KurenaiEngine_DX12.log)へ出すことで、
        // 通常の実行でも検出できるようにする
        if (SUCCEEDED(m_Device.As(&m_InfoQueue)))
        {
            // 情報レベルの通知は量が多く実害もないため保存しない(警告以上のみ残す)
            m_InfoQueue->SetMuteDebugOutput(FALSE);
            m_InfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
            D3D12_MESSAGE_SEVERITY deniedSeverities[] = { D3D12_MESSAGE_SEVERITY_INFO, D3D12_MESSAGE_SEVERITY_MESSAGE };

            // 実害が無いと確認済みで、かつ毎フレーム大量に出るためログを埋め尽くしてしまう指摘は除外する。
            // 除外しないと本当に見たいエラーが埋もれる(実測でこの2件だけで1回の起動あたり約2万件)
            D3D12_MESSAGE_ID deniedIds[] = {
                // クリア色がリソース生成時に指定した最適化用クリア値と違う、という性能上の注意。
                // エンジンはレンダーテクスチャを一律{0,0,0,1}で作り、パスごとに別の色でクリアしている
                // (G-Bufferは{0,0,0,0}、Lighting/Presentは{0.05,0.05,0.08,1})。高速クリア経路には
                // 乗らないが結果は正しい。用途ごとのクリア色をテクスチャ生成時に指定できるようにすれば
                // 解消できるが、RHIのAPI変更を伴うため現状は許容している
                D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
                // バッファはD3D12の仕様上つねにCOMMON状態で作られるため、CreateCommittedResourceへ
                // 渡したInitialStateが無視される、という通知(CREATERESOURCE_STATE_IGNORED)。
                // 仕様通りの動作で対処のしようがない。このIDの列挙子はビルドに使っている
                // Windows SDK 10.0.19041のd3d12sdklayers.hにまだ存在しないため数値で指定する
                static_cast<D3D12_MESSAGE_ID>(1328),
            };

            D3D12_INFO_QUEUE_FILTER filter{};
            filter.DenyList.NumSeverities = _countof(deniedSeverities);
            filter.DenyList.pSeverityList = deniedSeverities;
            filter.DenyList.NumIDs = _countof(deniedIds);
            filter.DenyList.pIDList = deniedIds;
            m_InfoQueue->PushStorageFilter(&filter);

            // 以降このログにD3D12DebugLayerの行が出なければ「指摘が無い」と判断してよいことを
            // はっきりさせるため、引き取りが有効になったこと自体を記録しておく
            Core::Logger::Info("DX12", "デバッグレイヤーの指摘をこのログファイルへ出力します(警告以上のみ)");
        }
        else
        {
            Core::Logger::Warning("DX12", "ID3D12InfoQueueを取得できませんでした。デバッグレイヤーの指摘はログに出ません");
        }
#endif

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

        // シェーダーモデルの判定とdxcの初期化はレイトレーシング判定より先に行う。
        // RayQueryを含むシェーダーはSM 6.5でしかコンパイルできないため、
        // DetectRaytracingSupportがこの結果を参照する
        DetectShaderModelAndSelectVariant();
        DetectRaytracingSupport();
        // bindless・メッシュシェーダーの判定もシェーダーモデルに依存するためこの後で行う。
        // ルートシグネチャの作成(CreateRootSignature)がbindlessの可否でフラグを変えるので、
        // それより前である必要もある
        DetectBindlessSupport();
        DetectMeshShaderSupport();
        // 自前ラスタライザは頂点/インデックスをbindlessで引くため、bindlessの判定より後で行う
        DetectSoftwareRasterSupport();
        DetectTiledResourcesSupport();

        // RTVの内訳: スワップチェーンのバックバッファ2 + オフスクリーンのレンダーテクスチャ12 = 常時14。
        // DSVと同じくCreateRenderTargetsのリサイズ処理は「新しいテクスチャを作ってから古いunique_ptrを
        // 解放する」順になるため、リサイズ中はほぼ倍のRTVが同時に生存する。余裕を持たせて32本確保する
        // (RTVヒープはCPU側のみでGPUメモリを消費しないため、多めに取っても実害がない)
        m_RtvHeap = std::make_unique<DX12DescriptorHeap>(m_Device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 32, false);
        // DSVの内訳: スワップチェーンの深度1 + G-Bufferの深度1 + シャドウマップ配列のスライス4 = 常時6本。
        // ただしCreateRenderTargetsのリサイズ処理は「新しいテクスチャを作ってから古いunique_ptrを解放する」
        // 順になるため、リサイズ中は一時的に7本必要になる。余裕を持たせて16本確保する(DSVヒープは
        // CPU側のみでGPUメモリを消費しないため、多めに取っても実害がない)
        m_DsvHeap = std::make_unique<DX12DescriptorHeap>(m_Device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 16, false);
        m_AssetSrvCpuHeap =
            std::make_unique<DX12DescriptorHeap>(m_Device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kAssetSrvCpuHeapCapacity, false);
        m_RenderSrvCpuHeap =
            std::make_unique<DX12DescriptorHeap>(m_Device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kRenderSrvCpuHeapCapacity, false);
        // 1フレーム分のコマンドをまとめて記録してから1回だけ実行する設計のため、描画のたびに
        // 新しいkTextureSlotCount個のブロックを払い出せるよう、1フレームに必要な最大数を見込んで確保する。
        // さらにCPUがGPU完了を待たずに次フレームを記録し始めるため、kFrameCountフレームぶんの容量を持たせる
        // コンピュートシェーダー用のSRV+UAVテーブル(kComputeSrvHeapCapacityPerFrame×kFrameCount)ぶんも
        // 同じシェーダ可視ヒープの後ろの区画に確保する(DX12は同時にバインドできるCBV_SRV_UAVヒープが
        // 1つだけのため、グラフィックス用と共存させる必要がある。詳細はAllocateComputeTableBlock参照)
        // さらにその後ろへbindless区画(恒久ディスクリプタ)を足す。区画の切り分けは
        // 定数計算だけで決まり、DX12DescriptorHeap内部のAllocate/AllocateBlockは使わない
        m_ShaderVisibleSrvHeap = std::make_unique<DX12DescriptorHeap>(
            m_Device.Get(),
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
            (kGraphicsSrvHeapCapacityPerFrame + kComputeSrvHeapCapacityPerFrame) * kFrameCount + kBindlessDescriptorCapacity,
            true);
        // サンプラーセットはCreateSamplerSetで連続ブロックとして払い出す(kMaxSamplerSets個ぶん)。
        // 加えて先頭に1ブロックぶんのフォールバックを確保しておく(下記参照)
        m_ShaderVisibleSamplerHeap = std::make_unique<DX12DescriptorHeap>(
            m_Device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplerSlotCount * (kMaxSamplerSets + 1), true);

        // デバッグレイヤーの指摘に「どのヒープか」が出るよう名前を付けておく
        // (名前が無いと"Unnamed ID3D12DescriptorHeap Object"としか出ず、アドレスから推測するしかない)
        m_RtvHeap->GetHeap()->SetName(L"KurenaiEngine RTV Heap");
        m_DsvHeap->GetHeap()->SetName(L"KurenaiEngine DSV Heap");
        m_AssetSrvCpuHeap->GetHeap()->SetName(L"KurenaiEngine Asset SRV CPU Heap");
        m_RenderSrvCpuHeap->GetHeap()->SetName(L"KurenaiEngine Render SRV CPU Heap");
        m_ShaderVisibleSrvHeap->GetHeap()->SetName(L"KurenaiEngine Shader Visible SRV Heap");
        m_ShaderVisibleSamplerHeap->GetHeap()->SetName(L"KurenaiEngine Shader Visible Sampler Heap");

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

        // 一度もバインドされていないSRV/UAVスロットを埋めるためのnullディスクリプタ。
        // D3D12はリソースにnullptrを渡したビューの作成を認めており、そのディスクリプタを読むと0が返る
        // (=DX11の未バインドスロットと同じ挙動)。これをDX12CommandListのシャドウ配列の初期値にすることで、
        // ディスクリプタテーブルのブロックに未初期化のまま残る領域が構造的に無くなる
        {
            m_NullSrvIndex = m_RenderSrvCpuHeap->Allocate();
            D3D12_SHADER_RESOURCE_VIEW_DESC nullSrvDesc{};
            nullSrvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            nullSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            nullSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            nullSrvDesc.Texture2D.MipLevels = 1;
            m_Device->CreateShaderResourceView(nullptr, &nullSrvDesc, m_RenderSrvCpuHeap->GetCpuHandle(m_NullSrvIndex));

            m_NullUavIndex = m_RenderSrvCpuHeap->Allocate();
            D3D12_UNORDERED_ACCESS_VIEW_DESC nullUavDesc{};
            nullUavDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            nullUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            m_Device->CreateUnorderedAccessView(nullptr, nullptr, &nullUavDesc, m_RenderSrvCpuHeap->GetCpuHandle(m_NullUavIndex));
        }

        // bindless区画。シェーダ可視SRVヒープの、リング2区画より後ろの残り全部
        // (kBindlessDescriptorCapacityのコメント参照)
        m_BindlessTable = std::make_unique<DX12BindlessTable>(
            m_Device.Get(),
            m_ShaderVisibleSrvHeap.get(),
            (kGraphicsSrvHeapCapacityPerFrame + kComputeSrvHeapCapacityPerFrame) * kFrameCount,
            kBindlessDescriptorCapacity);

        CreateRootSignature();
        CreateComputeRootSignature();
        CreateMeshRootSignature();
        CreateDispatchCommandSignature();
        CreateDispatchMeshCommandSignature();

        ID3D12DescriptorHeap* heaps[] = { m_ShaderVisibleSrvHeap->GetHeap(), m_ShaderVisibleSamplerHeap->GetHeap() };
        m_CommandList->SetDescriptorHeaps(2, heaps);

        m_ImmediateCommandList = std::make_unique<DX12CommandList>(this);
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

        // GPUが空になったので、遅延解放待ちのリソースは無条件に解放してよい
        CollectRetiredResources(true);
        CollectRetiredTileMappings(true);
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

        // フレーム境界で、GPUが使い終わったリソースを回収する。
        // m_FenceValueをここ(Renderスレッド)でだけ読むことで、どのスレッドから
        // RetireResourceされても競合しない
        CollectRetiredResources(false);
        CollectRetiredTileMappings(false);

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

        // DX12Bufferがリングへの書き込みを「同一フレーム内で何回目か」として数えるための通し番号を進める
        ++m_FrameStamp;

        DrainDebugMessages();
    }

    void DX12Device::DrainDebugMessages()
    {
        if (!m_InfoQueue)
        {
            return;
        }

        const UINT64 messageCount = m_InfoQueue->GetNumStoredMessages();
        for (UINT64 i = 0; i < messageCount; ++i)
        {
            SIZE_T messageLength = 0;
            if (FAILED(m_InfoQueue->GetMessage(i, nullptr, &messageLength)) || messageLength == 0)
            {
                continue;
            }

            std::vector<uint8_t> storage(messageLength);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if (FAILED(m_InfoQueue->GetMessage(i, message, &messageLength)))
            {
                continue;
            }

            // メッセージIDも併記する。除外したい指摘が出たときに、この番号をそのまま
            // Initialize()のdeniedIdsへ追加できるようにするため
            const std::string text =
                "[ID " + std::to_string(static_cast<int>(message->ID)) + "] " +
                std::string(message->pDescription, message->DescriptionByteLength > 0 ? message->DescriptionByteLength - 1 : 0);
            if (message->Severity == D3D12_MESSAGE_SEVERITY_WARNING)
            {
                Core::Logger::Warning("D3D12DebugLayer", text);
            }
            else
            {
                Core::Logger::Error("D3D12DebugLayer", text);
            }
        }

        m_InfoQueue->ClearStoredMessages();
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

    D3D12_SHADER_RESOURCE_VIEW_DESC DX12Device::MakeSrvDesc(const DirectX::TexMetadata& metadata)
    {
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
        return srvDesc;
    }

    Microsoft::WRL::ComPtr<ID3D12Resource> DX12Device::CreateAndUploadTextureResource(
        const DirectX::TexMetadata& metadata, const DirectX::ScratchImage& image)
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

        // アップロードバッファはこの関数を抜けるまで生存させる必要があるため、ここで同期的に実行完了を待つ
        UploadSubmitAndWait();

        return resource;
    }

    std::unique_ptr<IRHITexture> DX12Device::CreateTextureResourceFromImage(const DirectX::TexMetadata& metadata, const DirectX::ScratchImage& image)
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource = CreateAndUploadTextureResource(metadata, image);

        // ファイル/デコード済み画像から作るテクスチャ(マテリアル・スカイボックス・プレースホルダ)は
        // すべてアセット由来。シーン読み込み専用スレッドが確保・解放するためアセット側のヒープを使う
        const uint32_t srvIndex = m_AssetSrvCpuHeap->Allocate();
        const D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = MakeSrvDesc(metadata);
        m_Device->CreateShaderResourceView(resource.Get(), &srvDesc, m_AssetSrvCpuHeap->GetCpuHandle(srvIndex));

        return std::make_unique<DX12Texture>(
            this, m_AssetSrvCpuHeap.get(), resource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, srvIndex, DX12Texture::kInvalid, DX12Texture::kInvalid);
    }

    std::unique_ptr<IRHIPendingTextureContents> DX12Device::PrepareTextureContents(
        IRHITexture* target, const TextureImage& image)
    {
        auto* texture = static_cast<DX12Texture*>(target);
        if (texture == nullptr)
        {
            Core::Logger::Error("DX12", "PrepareTextureContents: テクスチャがnullptrです");
            return nullptr;
        }

        // 差し替えてよいのは、アセット用ヒープから確保したSRVだけを持つテクスチャに限る。
        // レンダーターゲットやUAVを持つものは他のビューとの整合が取れなくなる
        if (!texture->HasSrv() || texture->HasRtv() || texture->HasDsv() || texture->HasUav())
        {
            Core::Logger::Error("DX12", "PrepareTextureContents: SRV以外のビューを持つテクスチャは差し替えられません");
            return nullptr;
        }
        if (texture->GetSrvUavHeap() != m_AssetSrvCpuHeap.get())
        {
            Core::Logger::Error("DX12", "PrepareTextureContents: アセット用ヒープ以外から確保されたテクスチャは差し替えられません");
            return nullptr;
        }

        const DirectX::TexMetadata& metadata = image.GetMetadata();

        Microsoft::WRL::ComPtr<ID3D12Resource> newResource;
        try
        {
            newResource = CreateAndUploadTextureResource(metadata, image.GetImage());
        }
        catch (const std::exception& e)
        {
            // 失敗しても元の中身はそのまま。常駐ミップが減らないだけで絵は出続ける
            Core::Logger::Error("DX12", std::string("PrepareTextureContents: リソースの作成に失敗しました: ") + e.what());
            return nullptr;
        }

        return std::make_unique<DX12PendingTextureContents>(
            texture, std::move(newResource), MakeSrvDesc(metadata), static_cast<uint32_t>(metadata.mipLevels));
    }

    bool DX12Device::CommitTextureContents(IRHIPendingTextureContents* pending)
    {
        auto* entry = static_cast<DX12PendingTextureContents*>(pending);
        if (entry == nullptr || entry->Texture == nullptr)
        {
            Core::Logger::Error("DX12", "CommitTextureContents: 差し替え待ちの内容が不正です");
            return false;
        }

        if (entry->IsTiledResidency)
        {
            // タイルリソース経路。リソースもSRV番号もbindless番号も変わらず、
            // 変えるのはディスクリプタのResourceMinLODClampだけ
            if (entry->NewTiledState)
            {
                // 初めてタイルリソース化する回だけ、リソースそのものも入れ替わる
                entry->Texture->SetTiledState(std::move(entry->NewTiledState));
                RetireResource(entry->Texture->SwapResource(
                    std::move(entry->Resource), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
            }

            DX12TiledTextureState* state = entry->Texture->GetTiledState();
            if (state == nullptr)
            {
                Core::Logger::Error("DX12", "CommitTextureContents: タイル情報が失われています");
                return false;
            }
            state->ResidentFirstMip = entry->TiledFirstMip;

            const uint32_t srvIndex = entry->Texture->GetSrvIndex();
            m_Device->CreateShaderResourceView(
                entry->Texture->GetResource(), &entry->SrvDesc, m_AssetSrvCpuHeap->GetCpuHandle(srvIndex));

            const uint32_t bindlessIndex = entry->Texture->GetBindlessIndex();
            if (bindlessIndex != kInvalidBindlessIndex && m_BindlessTable)
            {
                m_BindlessTable->Rebind(bindlessIndex, m_AssetSrvCpuHeap->GetCpuHandle(srvIndex));
            }

            // 外す側は、直前まで記録されたコマンドリストがそのミップを読み終わるまで待つ
            if (entry->UnmapMipCount > 0 || !entry->TilesToRelease.empty())
            {
                RetireTileMapping(
                    entry->Texture->GetResource(), entry->UnmapFirstMip, entry->UnmapMipCount,
                    std::move(entry->TilesToRelease));
            }
            return true;
        }

        if (!entry->Resource)
        {
            Core::Logger::Error("DX12", "CommitTextureContents: 差し替え待ちのリソースがありません");
            return false;
        }

        // 【同じ番号のディスクリプタを作り直す】新しい番号を払い出さないので、
        // Assets::Meshが持つIRHITexture*も、bindless番号も変わらない
        const uint32_t srvIndex = entry->Texture->GetSrvIndex();
        m_Device->CreateShaderResourceView(
            entry->Resource.Get(), &entry->SrvDesc, m_AssetSrvCpuHeap->GetCpuHandle(srvIndex));

        const uint32_t bindlessIndex = entry->Texture->GetBindlessIndex();
        if (bindlessIndex != kInvalidBindlessIndex && m_BindlessTable)
        {
            m_BindlessTable->Rebind(bindlessIndex, m_AssetSrvCpuHeap->GetCpuHandle(srvIndex));
        }

        // 古いリソースはGPUがまだ読んでいる可能性がある。ここで手放さず遅延解放キューへ積む
        RetireResource(entry->Texture->SwapResource(
            std::move(entry->Resource), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
        return true;
    }

    void DX12Device::MapStandardMip(
        ID3D12Resource* resource, const DX12TiledTextureState& state, uint32_t mip,
        const std::vector<DX12TilePool::Tile>& tiles)
    {
        const D3D12_SUBRESOURCE_TILING& tiling = state.SubresourceTiling[mip];

        D3D12_TILED_RESOURCE_COORDINATE start{};
        start.X = 0;
        start.Y = 0;
        start.Z = 0;
        start.Subresource = mip;

        D3D12_TILE_REGION_SIZE region{};
        region.NumTiles = tiling.WidthInTiles * tiling.HeightInTiles * tiling.DepthInTiles;
        region.UseBox = TRUE;
        region.Width = tiling.WidthInTiles;
        region.Height = static_cast<UINT16>(tiling.HeightInTiles);
        region.Depth = static_cast<UINT16>(tiling.DepthInTiles);
        if (region.NumTiles == 0)
        {
            return;
        }

        if (tiles.empty())
        {
            // NULLマッピング(外す)。Tier 2以上は未マップの読み出しが0を返すと保証されている
            const D3D12_TILE_RANGE_FLAGS rangeFlag = D3D12_TILE_RANGE_FLAG_NULL;
            const UINT rangeTileCount = region.NumTiles;
            m_CommandQueue->UpdateTileMappings(
                resource, 1, &start, &region, nullptr, 1, &rangeFlag, nullptr, &rangeTileCount,
                D3D12_TILE_MAPPING_FLAG_NONE);
            return;
        }

        // タイルはヒープをまたいで散らばりうる。UpdateTileMappings は1回の呼び出しで
        // 1つのヒープしか指せないため、同じヒープの連続したタイルごとに区切って呼ぶ。
        // 領域側の座標は「x→y→z の順に数えたときの通し番号」で進む
        uint32_t consumed = 0;
        while (consumed < tiles.size())
        {
            const uint32_t heapIndex = tiles[consumed].HeapIndex;
            uint32_t run = 1;
            while (consumed + run < tiles.size() && tiles[consumed + run].HeapIndex == heapIndex &&
                   tiles[consumed + run].TileIndex == tiles[consumed + run - 1].TileIndex + 1)
            {
                ++run;
            }

            D3D12_TILED_RESOURCE_COORDINATE runStart = start;
            // 通し番号 consumed をタイル座標へ戻す
            runStart.X = consumed % tiling.WidthInTiles;
            runStart.Y = (consumed / tiling.WidthInTiles) % tiling.HeightInTiles;
            runStart.Z = consumed / (tiling.WidthInTiles * tiling.HeightInTiles);

            D3D12_TILE_REGION_SIZE runRegion{};
            runRegion.NumTiles = run;
            // UseBox=FALSEなら「その座標からNumTiles個ぶん、x→y→zの順に進む」直線的な指定になる。
            // ヒープの連続範囲ごとに切っているためこちらが素直
            runRegion.UseBox = FALSE;

            const D3D12_TILE_RANGE_FLAGS rangeFlag = D3D12_TILE_RANGE_FLAG_NONE;
            const UINT heapStartOffset = tiles[consumed].TileIndex;
            const UINT rangeTileCount = run;
            m_CommandQueue->UpdateTileMappings(
                resource, 1, &runStart, &runRegion, m_TilePool->GetHeap(heapIndex), 1, &rangeFlag,
                &heapStartOffset, &rangeTileCount, D3D12_TILE_MAPPING_FLAG_NONE);

            consumed += run;
        }
    }

    std::unique_ptr<IRHIPendingTextureContents> DX12Device::PrepareTiledTextureResidency(
        IRHITexture* target, const TiledTextureDesc& desc, const TextureImage& image, uint32_t firstMip)
    {
        if (m_TiledResourcesTier == 0)
        {
            return nullptr;
        }

        auto* texture = static_cast<DX12Texture*>(target);
        if (texture == nullptr || !texture->HasSrv() || texture->HasRtv() || texture->HasDsv() ||
            texture->HasUav() || texture->GetSrvUavHeap() != m_AssetSrvCpuHeap.get())
        {
            return nullptr;
        }
        if (desc.MipLevels == 0 || firstMip >= desc.MipLevels)
        {
            return nullptr;
        }

        // アップロード経路とタイルプールをまとめて直列化する。
        // UpdateTileMappingsはキューの操作で、その後に投入するコマンドリストから見える
        std::lock_guard<std::mutex> uploadLock(m_UploadMutex);

        if (!m_TilePool)
        {
            m_TilePool = std::make_unique<DX12TilePool>(m_Device.Get());
        }

        auto pending = std::make_unique<DX12PendingTextureContents>(
            texture, nullptr, D3D12_SHADER_RESOURCE_VIEW_DESC{}, desc.MipLevels);
        pending->IsTiledResidency = true;
        pending->TiledFirstMip = firstMip;

        DX12TiledTextureState* state = texture->GetTiledState();
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;

        if (state == nullptr)
        {
            // --- 初めてタイルリソース化する ---
            D3D12_RESOURCE_DESC resourceDesc{};
            resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            resourceDesc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
            resourceDesc.Width = desc.Width;
            resourceDesc.Height = desc.Height;
            resourceDesc.DepthOrArraySize = 1;
            resourceDesc.MipLevels = static_cast<UINT16>(desc.MipLevels);
            resourceDesc.Format = static_cast<DXGI_FORMAT>(desc.DxgiFormat);
            resourceDesc.SampleDesc.Count = 1;
            // 予約リソースはこのレイアウトでしか作れない(D3D12_TEXTURE_LAYOUTの規定)
            resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;
            resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

            if (FAILED(m_Device->CreateReservedResource(
                    &resourceDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource))))
            {
                Core::Logger::Warning(
                    "DX12", "予約リソースを作成できませんでした(このテクスチャは従来経路で扱います)");
                return nullptr;
            }

            auto newState = std::make_unique<DX12TiledTextureState>();
            newState->Format = resourceDesc.Format;
            newState->Width = desc.Width;
            newState->Height = desc.Height;
            newState->MipLevels = desc.MipLevels;

            // 【タイルの形もミップテールの構成も実測する】仕様上どちらもアダプタ依存で、
            // 「64KBタイル = BC7で256x256」は見積もりに過ぎない
            UINT numTiles = 0;
            UINT numSubresourceTilings = desc.MipLevels;
            newState->SubresourceTiling.resize(desc.MipLevels);
            m_Device->GetResourceTiling(
                resource.Get(), &numTiles, &newState->PackedMipInfo, &newState->TileShape,
                &numSubresourceTilings, 0, newState->SubresourceTiling.data());

            if (newState->PackedMipInfo.NumStandardMips == 0)
            {
                // 全部がミップテール = 一括でしか出し入れできない。タイルにする意味が無いので
                // 従来経路(リソースごと作り直す)へ委ねる。512x512以下ではこちらが普通
                return nullptr;
            }

            newState->MappedTiles.resize(newState->PackedMipInfo.NumStandardMips);
            resource->SetName(L"TiledStreamingTexture");

            // ミップテールは一括でしかマップ/アンマップできないため、常に貼りっぱなしにする
            if (newState->PackedMipInfo.NumTilesForPackedMips > 0)
            {
                if (!m_TilePool->Allocate(newState->PackedMipInfo.NumTilesForPackedMips, newState->PackedMipTiles))
                {
                    return nullptr;
                }

                D3D12_TILED_RESOURCE_COORDINATE tailStart{};
                tailStart.Subresource = newState->PackedMipInfo.NumStandardMips;
                D3D12_TILE_REGION_SIZE tailRegion{};
                tailRegion.NumTiles = newState->PackedMipInfo.NumTilesForPackedMips;
                tailRegion.UseBox = FALSE;

                // ミップテールのタイルもヒープをまたぎうるので、連続範囲ごとに切って貼る
                uint32_t consumed = 0;
                while (consumed < newState->PackedMipTiles.size())
                {
                    const uint32_t heapIndex = newState->PackedMipTiles[consumed].HeapIndex;
                    uint32_t run = 1;
                    while (consumed + run < newState->PackedMipTiles.size() &&
                           newState->PackedMipTiles[consumed + run].HeapIndex == heapIndex &&
                           newState->PackedMipTiles[consumed + run].TileIndex ==
                               newState->PackedMipTiles[consumed + run - 1].TileIndex + 1)
                    {
                        ++run;
                    }

                    D3D12_TILED_RESOURCE_COORDINATE runStart = tailStart;
                    runStart.X = consumed;
                    D3D12_TILE_REGION_SIZE runRegion{};
                    runRegion.NumTiles = run;
                    runRegion.UseBox = FALSE;

                    const D3D12_TILE_RANGE_FLAGS rangeFlag = D3D12_TILE_RANGE_FLAG_NONE;
                    const UINT heapStartOffset = newState->PackedMipTiles[consumed].TileIndex;
                    const UINT rangeTileCount = run;
                    m_CommandQueue->UpdateTileMappings(
                        resource.Get(), 1, &runStart, &runRegion, m_TilePool->GetHeap(heapIndex), 1, &rangeFlag,
                        &heapStartOffset, &rangeTileCount, D3D12_TILE_MAPPING_FLAG_NONE);
                    consumed += run;
                }
            }

            state = newState.get();
            pending->NewTiledState = std::move(newState);
            pending->Resource = resource;
            // 初回は「常駐なし」から始めて、下の共通処理でfirstMipまで貼る
            state->ResidentFirstMip = state->MipLevels;
        }
        else
        {
            resource = texture->GetResource();
        }

        const uint32_t standardMips = state->PackedMipInfo.NumStandardMips;
        const uint32_t oldFirstMip = state->ResidentFirstMip;

        // --- 細かくする方向: [firstMip, oldFirstMip) の標準ミップを貼る ---
        for (uint32_t mip = firstMip; mip < std::min(oldFirstMip, standardMips); ++mip)
        {
            const D3D12_SUBRESOURCE_TILING& tiling = state->SubresourceTiling[mip];
            const uint32_t tileCount = tiling.WidthInTiles * tiling.HeightInTiles * tiling.DepthInTiles;
            if (tileCount == 0)
            {
                continue;
            }
            if (!m_TilePool->Allocate(tileCount, state->MappedTiles[mip]))
            {
                Core::Logger::Error(
                    "DX12",
                    "タイルプール不足のためテクスチャ(SRV " + std::to_string(texture->GetSrvIndex()) + ", " +
                        std::to_string(desc.Width) + "x" + std::to_string(desc.Height) + ", ミップ" +
                        std::to_string(mip) + ")の常駐化を中止します");
                return nullptr;
            }
            MapStandardMip(resource.Get(), *state, mip, state->MappedTiles[mip]);
        }

        // --- 粗くする方向: [oldFirstMip, firstMip) を外す。**ここではまだ外さない** ---
        if (firstMip > oldFirstMip)
        {
            pending->UnmapFirstMip = oldFirstMip;
            pending->UnmapMipCount = std::min(firstMip, standardMips) - std::min(oldFirstMip, standardMips);
            for (uint32_t mip = oldFirstMip; mip < std::min(firstMip, standardMips); ++mip)
            {
                pending->TilesToRelease.insert(
                    pending->TilesToRelease.end(), state->MappedTiles[mip].begin(), state->MappedTiles[mip].end());
                state->MappedTiles[mip].clear();
            }
        }

        // --- 新しく貼ったミップへデータを流し込む ---
        const DirectX::TexMetadata& imageMeta = image.GetMetadata();
        const uint32_t uploadFirst = firstMip;
        const uint32_t uploadCount = std::min<uint32_t>(
            static_cast<uint32_t>(imageMeta.mipLevels), std::min(oldFirstMip, state->MipLevels) - firstMip);
        if (uploadCount > 0)
        {
            std::vector<D3D12_SUBRESOURCE_DATA> subresources(uploadCount);
            for (uint32_t i = 0; i < uploadCount; ++i)
            {
                const DirectX::Image* src = image.GetImage().GetImage(i, 0, 0);
                if (src == nullptr)
                {
                    Core::Logger::Error(
                        "DX12",
                        "テクスチャ(SRV " + std::to_string(texture->GetSrvIndex()) + ", " +
                            std::to_string(desc.Width) + "x" + std::to_string(desc.Height) + ", ミップ" +
                            std::to_string(uploadFirst + i) + ")の画像データを取得できず常駐化を中止します");
                    return nullptr;
                }
                subresources[i].pData = src->pixels;
                subresources[i].RowPitch = static_cast<LONG_PTR>(src->rowPitch);
                subresources[i].SlicePitch = static_cast<LONG_PTR>(src->slicePitch);
            }

            const D3D12_RESOURCE_DESC destDesc = resource->GetDesc();
            UINT64 requiredSize = 0;
            m_Device->GetCopyableFootprints(&destDesc, uploadFirst, uploadCount, 0, nullptr, nullptr, nullptr, &requiredSize);

            const D3D12_RESOURCE_BARRIER toCopyDest = CD3DX12_RESOURCE_BARRIER::Transition(
                resource.Get(),
                texture->IsTiled() ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_COPY_DEST);
            m_UploadCommandList->ResourceBarrier(1, &toCopyDest);

            Microsoft::WRL::ComPtr<ID3D12Resource> uploadBuffer = CreateUploadBuffer(requiredSize);
            UpdateSubresources(
                m_UploadCommandList.Get(), resource.Get(), uploadBuffer.Get(), 0, uploadFirst, uploadCount,
                subresources.data());

            const D3D12_RESOURCE_BARRIER toSrv = CD3DX12_RESOURCE_BARRIER::Transition(
                resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            m_UploadCommandList->ResourceBarrier(1, &toSrv);

            UploadSubmitAndWait();
        }

        // SRVは「全ミップを持つが、firstMipより細かい段はサンプルさせない」形にする。
        // マップしていないミップを読ませないための下限がResourceMinLODClamp
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = state->Format;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = state->MipLevels;
        srvDesc.Texture2D.ResourceMinLODClamp = static_cast<float>(firstMip);
        pending->SrvDesc = srvDesc;
        return pending;
    }

    void DX12Device::GetTilePoolUsage(uint64_t& outReservedBytes, uint64_t& outUsedBytes) const
    {
        outReservedBytes = 0;
        outUsedBytes = 0;
        if (!m_TilePool)
        {
            return;
        }
        outReservedBytes = m_TilePool->GetReservedBytes();
        outUsedBytes = static_cast<uint64_t>(m_TilePool->GetUsedTileCount()) * DX12TilePool::kTileSizeBytes;
    }

    void DX12Device::RetireTileMapping(
        Microsoft::WRL::ComPtr<ID3D12Resource> resource, uint32_t firstMip, uint32_t mipCount,
        std::vector<DX12TilePool::Tile> tiles)
    {
        if (!resource || (mipCount == 0 && tiles.empty()))
        {
            return;
        }

        std::lock_guard<std::mutex> lock(m_RetiredTileMappingsMutex);
        m_RetiredTileMappings.push_back(
            RetiredTileMapping{ std::move(resource), firstMip, mipCount, std::move(tiles), 0 });
    }

    void DX12Device::CollectRetiredTileMappings(bool releaseAll)
    {
        std::vector<RetiredTileMapping> ready;
        {
            std::lock_guard<std::mutex> lock(m_RetiredTileMappingsMutex);
            if (m_RetiredTileMappings.empty())
            {
                return;
            }

            const uint64_t completed = m_Fence ? m_Fence->GetCompletedValue() : 0;
            for (auto it = m_RetiredTileMappings.begin(); it != m_RetiredTileMappings.end();)
            {
                const bool due = releaseAll || (it->FenceValue != 0 && it->FenceValue <= completed);
                if (due)
                {
                    ready.push_back(std::move(*it));
                    it = m_RetiredTileMappings.erase(it);
                    continue;
                }
                if (it->FenceValue == 0)
                {
                    it->FenceValue = m_FenceValue;
                }
                ++it;
            }
        }

        for (RetiredTileMapping& entry : ready)
        {
            // NULLマッピングで外してからプールへ返す。逆順にすると、返したタイルが
            // 別のテクスチャへ貼られたあとにこちらの古いマッピングが残ることになる
            for (uint32_t i = 0; i < entry.MipCount; ++i)
            {
                const uint32_t mip = entry.FirstMip + i;
                D3D12_TILED_RESOURCE_COORDINATE start{};
                start.Subresource = mip;
                D3D12_TILE_REGION_SIZE region{};
                region.NumTiles = 0;
                region.UseBox = FALSE;

                // 領域のタイル数はリソース側から引き直す(状態を持ち回らずに済ませる)
                UINT numTiles = 0;
                D3D12_PACKED_MIP_INFO packed{};
                D3D12_TILE_SHAPE shape{};
                UINT subresourceCount = 1;
                D3D12_SUBRESOURCE_TILING tiling{};
                m_Device->GetResourceTiling(entry.Resource.Get(), &numTiles, &packed, &shape, &subresourceCount, mip, &tiling);
                region.NumTiles = tiling.WidthInTiles * tiling.HeightInTiles * tiling.DepthInTiles;
                if (region.NumTiles == 0)
                {
                    continue;
                }

                const D3D12_TILE_RANGE_FLAGS rangeFlag = D3D12_TILE_RANGE_FLAG_NULL;
                const UINT rangeTileCount = region.NumTiles;
                m_CommandQueue->UpdateTileMappings(
                    entry.Resource.Get(), 1, &start, &region, nullptr, 1, &rangeFlag, nullptr, &rangeTileCount,
                    D3D12_TILE_MAPPING_FLAG_NONE);
            }

            if (m_TilePool && !entry.Tiles.empty())
            {
                m_TilePool->Free(entry.Tiles);
            }
        }
    }

    void DX12Device::RetireResource(Microsoft::WRL::ComPtr<ID3D12Resource> resource)
    {
        if (!resource)
        {
            return;
        }

        std::lock_guard<std::mutex> lock(m_RetiredResourcesMutex);
        m_RetiredResources.push_back(RetiredResource{ std::move(resource), 0 });
    }

    void DX12Device::CollectRetiredResources(bool releaseAll)
    {
        std::lock_guard<std::mutex> lock(m_RetiredResourcesMutex);
        if (m_RetiredResources.empty())
        {
            return;
        }

        if (releaseAll)
        {
            // WaitForGPUIdle直後専用。GPUは何も実行していないので無条件に解放してよい
            m_RetiredResources.clear();
            return;
        }

        const uint64_t completed = m_Fence ? m_Fence->GetCompletedValue() : 0;
        auto removeFrom = std::remove_if(
            m_RetiredResources.begin(), m_RetiredResources.end(),
            [completed](const RetiredResource& entry) {
                return entry.FenceValue != 0 && entry.FenceValue <= completed;
            });
        m_RetiredResources.erase(removeFrom, m_RetiredResources.end());

        // 未押印のものへ、直前のフレームがシグナルしたフェンス値を押す。
        // この値が完了した時点で、差し替えを行ったフレームまでのGPU実行はすべて終わっている
        for (RetiredResource& entry : m_RetiredResources)
        {
            if (entry.FenceValue == 0)
            {
                entry.FenceValue = m_FenceValue;
            }
        }
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

        const uint32_t srvIndex = m_RenderSrvCpuHeap->Allocate();
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = dxgiFormat;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        m_Device->CreateShaderResourceView(resource.Get(), &srvDesc, m_RenderSrvCpuHeap->GetCpuHandle(srvIndex));

        const uint32_t rtvIndex = m_RtvHeap->Allocate();
        m_Device->CreateRenderTargetView(resource.Get(), nullptr, m_RtvHeap->GetCpuHandle(rtvIndex));

        return std::make_unique<DX12Texture>(this, m_RenderSrvCpuHeap.get(), resource, D3D12_RESOURCE_STATE_RENDER_TARGET, srvIndex, rtvIndex, DX12Texture::kInvalid);
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

        const uint32_t srvIndex = m_RenderSrvCpuHeap->Allocate();
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = dxgiFormat;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        m_Device->CreateShaderResourceView(resource.Get(), &srvDesc, m_RenderSrvCpuHeap->GetCpuHandle(srvIndex));

        const uint32_t uavIndex = m_RenderSrvCpuHeap->Allocate();
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = dxgiFormat;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        m_Device->CreateUnorderedAccessView(resource.Get(), nullptr, &uavDesc, m_RenderSrvCpuHeap->GetCpuHandle(uavIndex));

        return std::make_unique<DX12Texture>(
            this, m_RenderSrvCpuHeap.get(), resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srvIndex, DX12Texture::kInvalid, DX12Texture::kInvalid, uavIndex);
    }

    std::unique_ptr<IRHITexture> DX12Device::CreateUAVTexture3D(
        uint32_t width, uint32_t height, uint32_t depth, Format format)
    {
        // CreateUAVTexture(上)の3D版。ビューの次元指定をTEXTURE3Dにすることと、
        // UAVにWSize(書き込み対象の奥行きスライス数)を明示することだけが2Dとの違い。
        // 【WSizeの指定を忘れないこと】0のままだとUAVが奥行き0枚を指すことになり、
        // ディスパッチしても何も書き込まれない(エラーにはならず、テクスチャが黒いままになる)
        const DXGI_FORMAT dxgiFormat = ToDXGIFormat(format);

        const CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
        const CD3DX12_RESOURCE_DESC resourceDesc = CD3DX12_RESOURCE_DESC::Tex3D(
            dxgiFormat, width, height, static_cast<UINT16>(depth), 1, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        ThrowIfFailed(
            m_Device->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                IID_PPV_ARGS(&resource)),
            "3D UAVテクスチャの作成に失敗しました");

        const uint32_t srvIndex = m_RenderSrvCpuHeap->Allocate();
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = dxgiFormat;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture3D.MipLevels = 1;
        m_Device->CreateShaderResourceView(resource.Get(), &srvDesc, m_RenderSrvCpuHeap->GetCpuHandle(srvIndex));

        const uint32_t uavIndex = m_RenderSrvCpuHeap->Allocate();
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = dxgiFormat;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        uavDesc.Texture3D.WSize = depth;
        m_Device->CreateUnorderedAccessView(
            resource.Get(), nullptr, &uavDesc, m_RenderSrvCpuHeap->GetCpuHandle(uavIndex));

        return std::make_unique<DX12Texture>(
            this, m_RenderSrvCpuHeap.get(), resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srvIndex,
            DX12Texture::kInvalid, DX12Texture::kInvalid, uavIndex);
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
        const uint32_t srvIndex = m_RenderSrvCpuHeap->Allocate();
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = dxgiFormat;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = mipLevels;
        m_Device->CreateShaderResourceView(resource.Get(), &srvDesc, m_RenderSrvCpuHeap->GetCpuHandle(srvIndex));

        // ミップごとに単一ミップのUAVを張り、コンピュートシェーダーがミップ単位で書き込めるようにする
        // (Hi-Zの「前段ミップを読んで次段へ書く」ダウンサンプルだけでなく、IBLプリフィルタ済み鏡面マップの
        // 「ミップごとに異なるラフネスで独立に畳み込む」用途でも同じ仕組みを再利用する)
        std::vector<uint32_t> mipUavIndices;
        mipUavIndices.reserve(mipLevels);
        for (uint32_t mip = 0; mip < mipLevels; ++mip)
        {
            const uint32_t uavIndex = m_RenderSrvCpuHeap->Allocate();
            D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
            uavDesc.Format = dxgiFormat;
            uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            uavDesc.Texture2D.MipSlice = mip;
            m_Device->CreateUnorderedAccessView(resource.Get(), nullptr, &uavDesc, m_RenderSrvCpuHeap->GetCpuHandle(uavIndex));
            mipUavIndices.push_back(uavIndex);
        }

        return std::make_unique<DX12Texture>(
            this, m_RenderSrvCpuHeap.get(), resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srvIndex, DX12Texture::kInvalid, DX12Texture::kInvalid,
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
        const uint32_t srvIndex = m_RenderSrvCpuHeap->Allocate();
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
        m_Device->CreateShaderResourceView(resource.Get(), &srvDesc, m_RenderSrvCpuHeap->GetCpuHandle(srvIndex));

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
                    const uint32_t uavIndex = m_RenderSrvCpuHeap->Allocate();
                    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
                    uavDesc.Format = dxgiFormat;
                    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                    uavDesc.Texture2DArray.MipSlice = mip;
                    uavDesc.Texture2DArray.FirstArraySlice = cube * DX12Texture::kCubeFaceCount + face;
                    uavDesc.Texture2DArray.ArraySize = 1;
                    m_Device->CreateUnorderedAccessView(resource.Get(), nullptr, &uavDesc, m_RenderSrvCpuHeap->GetCpuHandle(uavIndex));
                    mipUavIndices.push_back(uavIndex);
                }
            }
        }

        return std::make_unique<DX12Texture>(
            this, m_RenderSrvCpuHeap.get(), resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srvIndex, DX12Texture::kInvalid, DX12Texture::kInvalid,
            DX12Texture::kInvalid, std::move(mipUavIndices), std::vector<uint32_t>{}, cubeCount);
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

        const uint32_t srvIndex = m_RenderSrvCpuHeap->Allocate();
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        m_Device->CreateShaderResourceView(resource.Get(), &srvDesc, m_RenderSrvCpuHeap->GetCpuHandle(srvIndex));

        return std::make_unique<DX12Texture>(this, m_RenderSrvCpuHeap.get(), resource, D3D12_RESOURCE_STATE_DEPTH_WRITE, srvIndex, DX12Texture::kInvalid, dsvIndex);
    }

    std::unique_ptr<IRHITexture> DX12Device::CreateDepthTextureArray(
        uint32_t width, uint32_t height, uint32_t arraySize, float clearDepth)
    {
        if (width == 0 || height == 0 || arraySize == 0)
        {
            const std::string message = "CreateDepthTextureArray: 不正なサイズが指定されました(width=" +
                                        std::to_string(width) + ", height=" + std::to_string(height) +
                                        ", arraySize=" + std::to_string(arraySize) + ")";
            Core::Logger::Error("DX12", message);
            throw std::runtime_error(message);
        }

        if (arraySize > D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION)
        {
            const std::string message = "CreateDepthTextureArray: 配列サイズがD3D12の上限を超えています(arraySize=" +
                                        std::to_string(arraySize) +
                                        ", 上限=" + std::to_string(D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION) + ")";
            Core::Logger::Error("DX12", message);
            throw std::runtime_error(message);
        }

        // 方針はCreateDepthTextureと同じ(R32_TYPELESSで作りDSV/SRVを個別に張る)。違いは
        // ArraySizeが1より大きいことと、DSVをスライスごとに(Texture2DArray、要素数1で)張ること
        D3D12_CLEAR_VALUE clearValue{};
        clearValue.Format = DXGI_FORMAT_D32_FLOAT;
        clearValue.DepthStencil.Depth = clearDepth;
        clearValue.DepthStencil.Stencil = 0;

        const CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
        const CD3DX12_RESOURCE_DESC resourceDesc = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_R32_TYPELESS, width, height, static_cast<UINT16>(arraySize), 1, 1, 0,
            D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);

        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        ThrowIfFailed(
            m_Device->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue,
                IID_PPV_ARGS(&resource)),
            "深度テクスチャ配列の作成に失敗しました");

        // 全スライスを1枚のTexture2DArrayとして読むSRV(サンプリング側。ShadowSampling.hlsli等)
        const uint32_t srvIndex = m_RenderSrvCpuHeap->Allocate();
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2DArray.MostDetailedMip = 0;
        srvDesc.Texture2DArray.MipLevels = 1;
        srvDesc.Texture2DArray.FirstArraySlice = 0;
        srvDesc.Texture2DArray.ArraySize = arraySize;
        m_Device->CreateShaderResourceView(resource.Get(), &srvDesc, m_RenderSrvCpuHeap->GetCpuHandle(srvIndex));

        // スライスごとに単一配列スライスのDSVを張り、パスごとに1スライスずつ描き込めるようにする
        // (CreateMippedUAVTextureCubeが面ごとのUAVを張るのと同じ考え方)
        std::vector<uint32_t> sliceDsvIndices;
        sliceDsvIndices.reserve(arraySize);
        for (uint32_t slice = 0; slice < arraySize; ++slice)
        {
            const uint32_t sliceDsvIndex = m_DsvHeap->Allocate();
            D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
            dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
            dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            dsvDesc.Texture2DArray.MipSlice = 0;
            dsvDesc.Texture2DArray.FirstArraySlice = slice;
            dsvDesc.Texture2DArray.ArraySize = 1;
            m_Device->CreateDepthStencilView(resource.Get(), &dsvDesc, m_DsvHeap->GetCpuHandle(sliceDsvIndex));
            sliceDsvIndices.push_back(sliceDsvIndex);
        }

        // dsvIndexはkInvalidにする(スライスごとのDSVで代替するため。~DX12Textureでの二重解放も防ぐ)
        return std::make_unique<DX12Texture>(
            this, m_RenderSrvCpuHeap.get(), resource, D3D12_RESOURCE_STATE_DEPTH_WRITE, srvIndex, DX12Texture::kInvalid, DX12Texture::kInvalid,
            DX12Texture::kInvalid, std::vector<uint32_t>{}, std::move(sliceDsvIndices));
    }

    std::unique_ptr<IRHITexture> DX12Device::CreateReadbackTexture(IRHITexture* source, uint32_t mipLevel)
    {
        if (source == nullptr)
        {
            Core::Logger::Error("DX12", "CreateReadbackTexture: コピー元がnullptrです");
            return nullptr;
        }

        auto* dx12Source = static_cast<DX12Texture*>(source);
        ID3D12Resource* sourceResource = dx12Source->GetResource();
        if (sourceResource == nullptr)
        {
            Core::Logger::Error("DX12", "CreateReadbackTexture: コピー元がリソースを持っていません");
            return nullptr;
        }

        D3D12_RESOURCE_DESC sourceDesc = sourceResource->GetDesc();
        if (sourceDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
        {
            // Texture3D(雲の3Dノイズ)とバッファは対象外。ファイル形式もCPU側の読み手も
            // 2次元を前提にしているため、半端に通さずここで断る
            Core::Logger::Error(
                "DX12",
                "CreateReadbackTexture: Texture2D以外はリードバックに対応していません (Dimension=" +
                    std::to_string(static_cast<int>(sourceDesc.Dimension)) + ")");
            return nullptr;
        }
        // 【DX11とまったく同じ判定と表を引く】ここを別々に書くと片方だけ直したときに静かに食い違う
        TextureReadbackDesc readbackDesc{};
        if (!PrepareReadbackTextureDesc(
                "DX12", mipLevel, sourceDesc.MipLevels, static_cast<uint32_t>(sourceDesc.Width), sourceDesc.Height,
                sourceDesc.Format, readbackDesc))
        {
            return nullptr;
        }
        const uint32_t mipWidth = readbackDesc.Width;
        const uint32_t mipHeight = readbackDesc.Height;

        // 【typelessのまま配置情報を求めない】深度はDSVとSRVを両立させるためR32_TYPELESSで
        // 作られている。コピー先の記述子に使う配置情報は型付きフォーマットで求める
        // (置き換えの根拠はRHIReadbackFormat.hのToReadbackTypedFormat)
        D3D12_RESOURCE_DESC typedDesc = sourceDesc;
        typedDesc.Format = ToReadbackTypedFormat(sourceDesc.Format);

        // コピー元のどのサブリソースを写すかで配置情報が変わる(ミップ段ごとに寸法が違う)。
        // 配列スライスはどれでも同じ配置になるのでスライス0のぶんを求めておき、
        // 実際にどのスライスを写すかはCopyTextureToReadbackで選ぶ
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT numRows = 0;
        UINT64 rowSizeInBytes = 0;
        UINT64 totalBytes = 0;
        m_Device->GetCopyableFootprints(
            &typedDesc, mipLevel, 1, 0, &footprint, &numRows, &rowSizeInBytes, &totalBytes);

        if (totalBytes == 0 || numRows == 0)
        {
            Core::Logger::Error("DX12", "CreateReadbackTexture: 配置情報を取得できませんでした(サイズが0)");
            return nullptr;
        }

        // 求めた配置と、こちらで計算したテクセル寸法が食い違っていないかを検算する。
        // 食い違ったまま進むと「読めるが中身がずれている」という最も気づきにくい壊れ方になる
        if (footprint.Footprint.Width != mipWidth || numRows != mipHeight ||
            rowSizeInBytes != static_cast<UINT64>(mipWidth) * readbackDesc.BytesPerTexel)
        {
            Core::Logger::Error(
                "DX12",
                "CreateReadbackTexture: 配置情報と寸法が一致しません (footprint=" +
                    std::to_string(footprint.Footprint.Width) + "x" + std::to_string(numRows) + " rowSize=" +
                    std::to_string(rowSizeInBytes) + " / 期待=" + std::to_string(mipWidth) + "x" +
                    std::to_string(mipHeight) + " rowSize=" +
                    std::to_string(static_cast<UINT64>(mipWidth) * readbackDesc.BytesPerTexel) + ")");
            return nullptr;
        }

        // 【READBACKヒープにテクスチャは置けない】D3D12の仕様上、READBACK/UPLOADヒープに
        // 置けるのはバッファだけ。配置情報つきのバッファとして確保し、
        // CopyTextureRegionでテクスチャ→バッファのコピーを行う
        const CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_READBACK);
        const CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(totalBytes);

        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        const HRESULT hr = m_Device->CreateCommittedResource(
            // READBACKヒープのリソースはCOPY_DEST状態でしか作れない(D3D12の仕様)
            &heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&resource));
        if (FAILED(hr))
        {
            // 【throwしない】これは描画に必須の資源ではなく、デバッグ用の吸い出しの受け皿。
            // 大きなテクスチャで確保に失敗しても起動そのものを落とすべきではない
            Core::Logger::Error(
                "DX12",
                "CreateReadbackTexture: リードバックテクスチャの作成に失敗しました (" +
                    std::to_string(totalBytes) + "バイト)");
            return nullptr;
        }

        // 【永続マップする】リードバックバッファ(DX12Device::CreateBuffer)と同じ扱い。
        // 読むたびにMap/Unmapすると、Unmapへ渡す書き込み範囲の指定を誤ったときに
        // ドライバがキャッシュを吐き出して遅くなる。読み取り専用なのでマップしたままでよい
        void* mappedPtr = nullptr;
        const D3D12_RANGE readRange{ 0, static_cast<SIZE_T>(totalBytes) };
        if (FAILED(resource->Map(0, &readRange, &mappedPtr)))
        {
            Core::Logger::Error("DX12", "CreateReadbackTexture: リードバックテクスチャのマップに失敗しました");
            return nullptr;
        }

        auto state = std::make_unique<DX12ReadbackState>();
        state->Footprint = footprint;
        state->MappedPtr = mappedPtr;
        state->BufferSizeInBytes = totalBytes;
        state->Desc = readbackDesc;

        return std::make_unique<DX12Texture>(std::move(resource), std::move(state));
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

                samplerDesc.Filter = ToD3D12Filter(NormalizeSamplerFilter(desc.Filter, "DX12"));

                const D3D12_TEXTURE_ADDRESS_MODE addressMode =
                    ToD3D12AddressMode(NormalizeSamplerAddressMode(desc.AddressMode, "DX12"));
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

    void DX12Device::ApplyPendingResourceInvalidation()
    {
        ApplyPendingShadowedDescriptorInvalidation();
    }

    void DX12Device::OnGPUResourceDestroyed()
    {
        // どのスレッドから破棄されても安全なよう、ここではatomicな印を立てるだけにする。
        m_ShadowedDescriptorsDirty.store(true, std::memory_order_release);
    }

    void DX12Device::ApplyPendingShadowedDescriptorInvalidation()
    {
        // このメソッドはRenderスレッドからだけ呼び、シャドウ配列を他スレッドから変更しない。
        // Loaderスレッドがフレーム途中で破棄した場合、そのフレームの残りは古いハンドルが残り、
        // 消去は次フレーム先頭になる。修正前は永久に残っていたものを1フレーム以内に縮めるが、
        // このフレーム途中の残存は限界として残る。
        if (m_ShadowedDescriptorsDirty.exchange(false, std::memory_order_acq_rel) && m_ImmediateCommandList)
        {
            m_ImmediateCommandList->InvalidateShadowedDescriptors();
        }
    }

    std::unique_ptr<IRHIImGuiBackend> DX12Device::CreateImGuiBackend(void* windowHandle)
    {
        return std::make_unique<DX12ImGuiBackend>(this, windowHandle);
    }

    std::unique_ptr<IRHIGPUProfiler> DX12Device::CreateGPUProfiler()
    {
        return std::make_unique<DX12GPUProfiler>(this);
    }

    // --- レイトレーシング -------------------------------------------------------------------

    void DX12Device::DetectShaderModelAndSelectVariant()
    {
        // D3D12_FEATURE_SHADER_MODELは「HighestShaderModelへ聞きたい上限を入れて呼ぶと、
        // 対応している値まで引き下げて返す」APIだが、ランタイムが知らない列挙値を渡すと
        // E_INVALIDARGを返す。そのため上から順に下げながら問い合わせる
        // 先頭が6_6なのはbindless(ResourceDescriptorHeap)がSM 6.6で追加されたため。
        // ここを6_5のままにするとデバイスが6.6対応でも6.5としか報告されず、
        // bindlessが常に無効になる
        static constexpr D3D_SHADER_MODEL kCandidates[] = {
            D3D_SHADER_MODEL_6_6, D3D_SHADER_MODEL_6_5, D3D_SHADER_MODEL_6_4, D3D_SHADER_MODEL_6_3,
            D3D_SHADER_MODEL_6_2, D3D_SHADER_MODEL_6_1, D3D_SHADER_MODEL_6_0,
        };

        m_HighestShaderModel = static_cast<D3D_SHADER_MODEL>(0);
        for (const D3D_SHADER_MODEL candidate : kCandidates)
        {
            D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{ candidate };
            if (SUCCEEDED(m_Device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel))))
            {
                m_HighestShaderModel = shaderModel.HighestShaderModel;
                break;
            }
        }

        if (m_HighestShaderModel == static_cast<D3D_SHADER_MODEL>(0))
        {
            Core::Logger::Warning(
                "DX12", "対応シェーダーモデルを判定できませんでした。SM 5.0のバリアントで動作します");
        }

        // 【どのバリアントを使えるかはパッケージ側にも依存する】以前はここでdxcompiler.dllを
        // ロードし、そのバージョン(IDxcVersionInfo)を見てSM 6.6が使えるかを決めていた。
        // 事前コンパイルへ移したので、実行時にdxcは居ない。代わりに、ビルド時に
        // KurenaiShaderPackerが「どのバリアントを焼けたか」をヘッダーのVariantMaskへ
        // 記録しているので、それを読む。
        // (「dxcのバージョン = Windows SDKのバージョン」という制約は、実行環境ではなく
        //  ビルドマシンの話になった)
        m_ShaderVariantMask = ReadShaderVariantMask();

        const bool hasDxil66 = (m_ShaderVariantMask & (1u << static_cast<uint32_t>(Assets::ShaderVariant::Dxil66))) != 0u;
        const bool hasDxil65 = (m_ShaderVariantMask & (1u << static_cast<uint32_t>(Assets::ShaderVariant::Dxil65))) != 0u;

        if (hasDxil66 && m_HighestShaderModel >= D3D_SHADER_MODEL_6_6)
        {
            m_ShaderVariant = Assets::ShaderVariant::Dxil66;
            Core::Logger::Info("DX12", "事前コンパイル済みシェーダー: DXIL / SM 6.6(bindless有効)を使用します");
        }
        else if (hasDxil65 && m_HighestShaderModel >= D3D_SHADER_MODEL_6_5)
        {
            m_ShaderVariant = Assets::ShaderVariant::Dxil65;
            Core::Logger::Info(
                "DX12",
                std::string("事前コンパイル済みシェーダー: DXIL / SM 6.5 を使用します(bindlessは無効。理由: ") +
                    (hasDxil66 ? "デバイスがSM 6.6に非対応" : "ビルド時のdxcがSM 6.6に非対応でバリアントが焼かれていない") + ")");
        }
        else
        {
            // SM 6.0〜6.4のデバイス、またはSM 6.xのバリアントが1つも焼かれていない場合。
            // DXBCはD3D12でもそのまま受け付けられるため、従来の
            // 「dxcompiler.dllが無いときのd3dcompilerフォールバック」と同じ縮退になる
            // (レイトレーシング・メッシュシェーダー・自前ラスタライザはいずれも無効)
            m_ShaderVariant = Assets::ShaderVariant::Dxbc50;
            Core::Logger::Warning(
                "DX12",
                "事前コンパイル済みシェーダー: DXBC / SM 5.0 へ縮退します"
                "(レイトレーシング・メッシュシェーダー・自前ラスタライザはいずれも無効になります)");
        }
    }

    uint32_t DX12Device::ReadShaderVariantMask()
    {
        // どの.kshaderも同じパッカーの1回の実行で焼かれるため、VariantMaskは全ファイルで同じ。
        // 最初に見つかった1つを読めばよい(3Dと2Dで置き場所が違うので、決め打ちのファイル名は使わない)
        const std::wstring shaderDirectory = Core::GetModuleDirectory() + L"Shaders\\";

        std::error_code ec;
        if (!std::filesystem::is_directory(shaderDirectory, ec))
        {
            Core::Logger::Error(
                "DX12",
                "シェーダーフォルダがありません: " + Core::WideToUtf8(shaderDirectory) +
                    " (ビルド時にKurenaiShaderPackerが.kshaderを生成できていない可能性があります)");
            return 0u;
        }

        for (const auto& entry : std::filesystem::directory_iterator(shaderDirectory, ec))
        {
            if (!entry.is_regular_file() || entry.path().extension() != L".kshader")
            {
                continue;
            }
            try
            {
                const Assets::ShaderPackageData& package = m_ShaderPackages.Get(entry.path().wstring());
                if (package.DebugBuild != kIsDebugBuild)
                {
                    // 気付きにくい取り違え(Releaseの実行ファイルがDebugの.kshaderを掴む等)を明示する。
                    // 動作はするので警告に留める
                    Core::Logger::Warning(
                        "DX12",
                        std::string(".kshaderの構成が実行ファイルと食い違っています(パッケージ: ") +
                            (package.DebugBuild ? "Debug" : "Release") + " / 実行ファイル: " +
                            (kIsDebugBuild ? "Debug" : "Release") + ")");
                }
                return package.VariantMask;
            }
            catch (const std::exception& e)
            {
                Core::Logger::Error(
                    "DX12", std::string("シェーダーパッケージを読めませんでした: ") + e.what());
                return 0u;
            }
        }

        Core::Logger::Error(
            "DX12",
            ".kshaderが1つも見つかりません: " + Core::WideToUtf8(shaderDirectory) +
                " (ビルド時にKurenaiShaderPackerが実行されていない可能性があります)");
        return 0u;
    }

    void DX12Device::DetectBindlessSupport()
    {
        m_SupportsBindless = false;

        // シェーダー側の条件。KURENAI_BINDLESS 付きで焼かれたSM 6.6のバリアントを
        // 実際に使っていること(デバイスの対応状況と、ビルド時にそのバリアントを
        // 焼けたかの両方で決まる。DetectShaderModelAndSelectVariant参照)
        if (m_ShaderVariant != Assets::ShaderVariant::Dxil66)
        {
            Core::Logger::Info(
                "DX12",
                "bindless非対応: SM 6.6のシェーダーバリアントを使用していません"
                "(ResourceDescriptorHeapにはSM 6.6対応のGPUと、ビルド時にSM 6.6を扱えるdxcが必要です)");
            return;
        }

        // ハードウェア側の条件。SM 6.6の動的リソース(ResourceDescriptorHeap)は
        // 「ヒープ全体をシェーダーから直接添字できる」ことが前提で、これはリソースバインディング
        // Tier 3が保証する性質。Tier 2以下はディスクリプタテーブルの範囲を越えたアクセスを
        // 認めていないため、コンパイルは通っても実行時の挙動が未定義になる
        D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
        if (FAILED(m_Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))))
        {
            Core::Logger::Warning("DX12", "bindless非対応: D3D12_FEATURE_D3D12_OPTIONSの問い合わせに失敗しました");
            return;
        }

        if (options.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_3)
        {
            Core::Logger::Info(
                "DX12",
                "bindless非対応: ResourceBindingTierが" + std::to_string(static_cast<int>(options.ResourceBindingTier)) +
                    "でTier 3に達していません");
            return;
        }

        m_SupportsBindless = true;
        Core::Logger::Info("DX12", "bindless(ResourceDescriptorHeap / SM 6.6)が利用可能です");
    }

    void DX12Device::DetectMeshShaderSupport()
    {
        m_SupportsMeshShader = false;

        // このエンジンのメッシュシェーダーはジオメトリをbindlessで引くため、
        // 増幅/メッシュシェーダーのバイトコードはSM 6.6(bindless)のバリアントにしか焼かれていない。
        // したがって条件はbindlessと同じ
        if (m_ShaderVariant != Assets::ShaderVariant::Dxil66)
        {
            Core::Logger::Info(
                "DX12",
                "メッシュシェーダー非対応: SM 6.6のシェーダーバリアントを使用していません"
                "(このエンジンのメッシュシェーダーはジオメトリをbindlessで引くため)");
            return;
        }

        // メッシュシェーダーPSOの作成にはID3D12Device2::CreatePipelineState(パイプラインステート
        // ストリーム)が要る。ID3D12Device2はWindows 10 1709で追加されたインタフェースで、
        // メッシュシェーダー対応GPUなら必ず取得できるが、無ければPSOを作る手段が無い
        if (FAILED(m_Device.As(&m_Device2)))
        {
            Core::Logger::Info("DX12", "メッシュシェーダー非対応: ID3D12Device2を取得できませんでした");
            return;
        }

        D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7{};
        if (FAILED(m_Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7))))
        {
            // OPTIONS7自体がWindows 10 2004で追加された問い合わせのため、
            // それ以前のOSでは失敗する。その場合メッシュシェーダーも存在しない
            Core::Logger::Info("DX12", "メッシュシェーダー非対応: D3D12_FEATURE_D3D12_OPTIONS7の問い合わせに失敗しました");
            m_Device2.Reset();
            return;
        }

        if (options7.MeshShaderTier < D3D12_MESH_SHADER_TIER_1)
        {
            Core::Logger::Info("DX12", "メッシュシェーダー非対応: MeshShaderTierがTier 1に達していません");
            m_Device2.Reset();
            return;
        }

        // 【bindlessを必須にする】このエンジンのメッシュシェーダーは、入力アセンブラの代わりに
        // 頂点・メッシュレットの各バッファをResourceDescriptorHeap経由で読む設計にしてある
        // (Shaders/3D/GBufferMeshlet.hlsl)。bindlessが無い環境向けにSRVテーブル経由の
        // 別実装を持つこともできるが、メッシュシェーダー対応GPUは実質すべてSM 6.6にも
        // 対応しているため、2系統を抱える価値が無い
        if (!m_SupportsBindless)
        {
            Core::Logger::Info(
                "DX12", "メッシュシェーダー非対応: bindlessが利用できないため無効にします");
            m_Device2.Reset();
            return;
        }

        m_SupportsMeshShader = true;
        Core::Logger::Info("DX12", "メッシュシェーダー(Tier 1 / SM 6.5)が利用可能です");
    }

    void DX12Device::DetectSoftwareRasterSupport()
    {
        m_SupportsSoftwareRaster = false;

        // 【bindlessを必須にする】自前ラスタライザは三角形1つにつき頂点バッファと
        // インデックスバッファをResourceDescriptorHeap経由で引く(Shaders/3D/SoftwareRaster.hlsl)。
        // bindlessが立っている時点でシェーダーモデル6.6とリソースバインディングTier 3が
        // 保証されるため、シェーダーモデルの再判定は要らない
        if (!m_SupportsBindless)
        {
            Core::Logger::Info(
                "DX12", "ソフトウェアラスタライザ非対応: bindlessが利用できません");
            return;
        }

        // 深度と三角形IDを1つの64bit値へ詰めてInterlockedMaxで解決するため、
        // 64bit整数のシェーダー演算が要る。SM 6.6に対応していてもこれが無いGPUはあり得るので
        // (機能レベルとは独立した任意機能)、必ず個別に問い合わせる
        D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1{};
        if (FAILED(m_Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options1, sizeof(options1))))
        {
            Core::Logger::Info(
                "DX12", "ソフトウェアラスタライザ非対応: D3D12_FEATURE_D3D12_OPTIONS1の問い合わせに失敗しました");
            return;
        }

        if (!options1.Int64ShaderOps)
        {
            Core::Logger::Info(
                "DX12", "ソフトウェアラスタライザ非対応: 64bit整数のシェーダー演算(Int64ShaderOps)に対応していません");
            return;
        }

        m_SupportsSoftwareRaster = true;
        Core::Logger::Info("DX12", "ソフトウェアラスタライザ(SM 6.6 / 64bitアトミック)が利用可能です");
    }

    void DX12Device::DetectTiledResourcesSupport()
    {
        m_TiledResourcesTier = 0;

        // TiledResourcesTierはbindless判定が引いているのと同じD3D12_FEATURE_D3D12_OPTIONSのメンバ。
        // 判定ごとに独立した関数にする既存の形へ揃えるため、ここでもう一度引く(実害は無い)
        D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
        if (FAILED(m_Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))))
        {
            Core::Logger::Info("DX12", "タイルリソース非対応: D3D12_FEATURE_D3D12_OPTIONSの問い合わせに失敗しました");
            return;
        }

        m_TiledResourcesTier = static_cast<uint32_t>(options.TiledResourcesTier);
        if (m_TiledResourcesTier == 0)
        {
            Core::Logger::Info("DX12", "タイルリソース非対応(TiledResourcesTier 0)。常駐ミップ制御のみで動作します");
            return;
        }

        // 【Tier 1 は使わない】Tier 1 では未マップのタイルを読んだときの結果が未定義で、
        // デバイス削除に至ることもある。全域へダミータイルを貼れば回避できるが、
        // それは「常駐していないミップを読んでも落ちないようにする」ための仕掛けであって
        // 常駐量を減らす目的には貢献しない。Tier 2 以上は「未マップは0を読み、書きは捨てる」と
        // 仕様が保証しているので、こちらだけを対象にする
        if (m_TiledResourcesTier < 2)
        {
            const uint32_t reportedTier = m_TiledResourcesTier;
            // 上位層は「0なら使わない」で分岐するため、採らないと決めた時点で0へ落とす
            m_TiledResourcesTier = 0;
            Core::Logger::Info(
                "DX12",
                "タイルリソースはTier " + std::to_string(reportedTier) +
                    "のため使いません(未マップタイルの読み出しが未定義。Tier 2以上が必要)。常駐ミップ制御のみで動作します");
            return;
        }

        Core::Logger::Info(
            "DX12", "タイルリソース(Tier " + std::to_string(m_TiledResourcesTier) + ")が利用可能です");
    }

    uint32_t DX12Device::RegisterBindless(IRHITexture* texture)
    {
        if (!m_SupportsBindless || !m_BindlessTable || !texture)
        {
            return kInvalidBindlessIndex;
        }

        auto* dx12Texture = static_cast<DX12Texture*>(texture);
        // 既に登録済みなら同じ番号を返す。呼び出し側が重複登録で区画を食い潰さないようにする
        if (dx12Texture->GetBindlessIndex() != kInvalidBindlessIndex)
        {
            return dx12Texture->GetBindlessIndex();
        }

        // SRVを持たないテクスチャ(深度専用など)はbindlessで読めない。
        // 無効なハンドルを渡すとでたらめなディスクリプタが区画へ入るため、ここで弾く
        if (!dx12Texture->HasSrv())
        {
            Core::Logger::Error("DX12", "SRVを持たないテクスチャがbindlessへ登録されようとしました");
            return kInvalidBindlessIndex;
        }

        const uint32_t index = m_BindlessTable->Register(dx12Texture->GetSrvCpuHandle());
        dx12Texture->SetBindlessIndex(index);
        return index;
    }

    uint32_t DX12Device::RegisterBindless(IRHIBuffer* buffer)
    {
        if (!m_SupportsBindless || !m_BindlessTable || !buffer)
        {
            return kInvalidBindlessIndex;
        }

        auto* dx12Buffer = static_cast<DX12Buffer*>(buffer);
        if (dx12Buffer->GetBindlessIndex() != kInvalidBindlessIndex)
        {
            return dx12Buffer->GetBindlessIndex();
        }

        const D3D12_CPU_DESCRIPTOR_HANDLE srv = dx12Buffer->GetSrvCpuHandle();
        if (srv.ptr == 0)
        {
            // SRVを持たないUsage(Vertex/Index/Constant、およびShaderReadableを指定しなかった
            // 頂点バッファ)。BufferDesc::ShaderReadableの指定漏れがここで表面化する
            Core::Logger::Error(
                "DX12", "SRVを持たないバッファがbindlessへ登録されようとしました(BufferDesc::ShaderReadableの指定漏れ?)");
            return kInvalidBindlessIndex;
        }

        const uint32_t index = m_BindlessTable->Register(srv);
        dx12Buffer->SetBindlessIndex(index);
        return index;
    }

    uint32_t DX12Device::GetBindlessUsedCount() const
    {
        return m_BindlessTable ? m_BindlessTable->GetUsedCount() : 0;
    }

    uint32_t DX12Device::GetBindlessCapacity() const
    {
        return m_BindlessTable ? m_BindlessTable->GetCapacity() : 0;
    }

    uint32_t DX12Device::RegisterBindlessUAV(IRHIBuffer* buffer)
    {
        if (!m_SupportsBindless || !m_BindlessTable || !buffer)
        {
            return kInvalidBindlessIndex;
        }

        auto* dx12Buffer = static_cast<DX12Buffer*>(buffer);
        if (dx12Buffer->GetBindlessUavIndex() != kInvalidBindlessIndex)
        {
            return dx12Buffer->GetBindlessUavIndex();
        }

        if (!dx12Buffer->HasUav())
        {
            // UAVを持たないUsage(Vertex/Index/Constant/StructuredReadOnly/StructuredImmutable)。
            // 無効なハンドルを渡すとでたらめなディスクリプタが区画へ入るため、ここで弾く
            Core::Logger::Error("DX12", "UAVを持たないバッファがbindless(UAV)へ登録されようとしました");
            return kInvalidBindlessIndex;
        }

        const uint32_t index = m_BindlessTable->Register(dx12Buffer->GetUavCpuHandle());
        dx12Buffer->SetBindlessUavIndex(index);
        return index;
    }

    uint32_t DX12Device::GetMaxDrawsPerFrame() const
    {
        // AllocateSrvTableBlockが1フレームに払い出せるブロック数と同じ。
        // これを超えるとAllocateSrvTableBlockが例外を投げる
        return kMaxSrvTableBlocksPerFrame;
    }

    void DX12Device::DetectRaytracingSupport()
    {
        m_SupportsRaytracing = false;

        // ID3D12Device5はWindows 10 1809(RS5)で追加されたインタフェース。取得できない場合は
        // OS/ドライバがDXR世代に達していないため、それ以上の判定は行えない
        if (FAILED(m_Device.As(&m_Device5)))
        {
            Core::Logger::Info("DX12", "レイトレーシング非対応: ID3D12Device5を取得できませんでした(OS/ドライバがDXR未対応)");
            return;
        }

        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
        if (FAILED(m_Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5))))
        {
            Core::Logger::Warning("DX12", "レイトレーシング非対応: D3D12_FEATURE_D3D12_OPTIONS5の問い合わせに失敗しました");
            m_Device5.Reset();
            return;
        }

        // インラインレイトレーシング(HLSLのRayQuery)はTier 1.1で追加された機能。
        // Tier 1.0はDispatchRaysによるフルパイプラインのみ対応しており、このエンジンが採る
        // インライン方式では使えないため非対応として扱う
        if (options5.RaytracingTier < D3D12_RAYTRACING_TIER_1_1)
        {
            Core::Logger::Info(
                "DX12",
                "レイトレーシング非対応: RaytracingTierが" + std::to_string(static_cast<int>(options5.RaytracingTier)) +
                    "でTier 1.1(値11)に達していません(インラインレイトレーシングにはTier 1.1が必要)");
            m_Device5.Reset();
            return;
        }

        // インラインレイトレーシングのRayQueryはシェーダーモデル6.5で追加された機能で、
        // DXILでしか表現できない。ハードウェアがTier 1.1でも、DXILのバリアント
        // (Dxil65 / Dxil66)を使っていなければトレースするシェーダーを作れないため
        // 非対応として扱う(Phase 0でdxc/SM 6.xへ移行した理由そのもの)
        if (m_ShaderVariant != Assets::ShaderVariant::Dxil65 && m_ShaderVariant != Assets::ShaderVariant::Dxil66)
        {
            Core::Logger::Warning(
                "DX12",
                "レイトレーシング非対応: DXIL(SM 6.5以上)のシェーダーバリアントを使用していません"
                "(RayQueryにはSM 6.5が必要です。デバイスの対応状況と、ビルド時に.kshaderへSM 6.xのバリアントが"
                "焼かれているかを確認してください)");
            m_Device5.Reset();
            return;
        }

        // AS構築コマンドを積むのはアップロード専用コマンドリスト(Renderスレッド外から呼ばれる
        // LoadSceneと安全に共存させるため)。そちらのList4も取れないと構築できない
        if (FAILED(m_UploadCommandList.As(&m_UploadCommandList4)))
        {
            Core::Logger::Warning(
                "DX12", "レイトレーシング非対応: ID3D12GraphicsCommandList4を取得できませんでした");
            m_Device5.Reset();
            return;
        }

        m_SupportsRaytracing = true;
        Core::Logger::Info("DX12", "レイトレーシング対応: DXR Tier 1.1(インラインレイトレーシングが利用できます)");
    }

    std::unique_ptr<IRHIAccelerationStructure> DX12Device::BuildAccelerationStructure(
        const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& inputs, bool createSrv, const char* debugName,
        bool compact)
    {
        // 圧縮はALLOW_COMPACTIONを立てて構築した結果にしか行えない。
        // 立っていない入力で圧縮を求められたら、黙って素の結果を返さずに理由を残す
        if (compact && (inputs.Flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION) == 0)
        {
            Core::Logger::Error(
                "DX12",
                std::string(debugName) + ": ALLOW_COMPACTIONが立っていないため圧縮を行いません(呼び出し側の指定漏れ)");
            compact = false;
        }

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildInfo{};
        m_Device5->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuildInfo);
        if (prebuildInfo.ResultDataMaxSizeInBytes == 0)
        {
            Core::Logger::Error(
                "DX12", std::string(debugName) + "の必要サイズ問い合わせが0を返しました(入力が空の可能性があります)");
            return nullptr;
        }

        const CD3DX12_HEAP_PROPERTIES defaultHeapProps(D3D12_HEAP_TYPE_DEFAULT);

        // 構築の一時領域。構築完了を同期的に待ってからこの関数を抜けるため、ローカルで持てばよい
        const CD3DX12_RESOURCE_DESC scratchDesc =
            CD3DX12_RESOURCE_DESC::Buffer(prebuildInfo.ScratchDataSizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        Microsoft::WRL::ComPtr<ID3D12Resource> scratch;
        if (FAILED(m_Device->CreateCommittedResource(
                &defaultHeapProps, D3D12_HEAP_FLAG_NONE, &scratchDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&scratch))))
        {
            Core::Logger::Error("DX12", std::string(debugName) + "のスクラッチバッファ作成に失敗しました");
            return nullptr;
        }

        // AS本体。RAYTRACING_ACCELERATION_STRUCTURE状態で作り、以後この状態のまま遷移しない
        // (D3D12の仕様上、ASバッファを他の状態へ移すことはできない)
        const CD3DX12_RESOURCE_DESC resultDesc =
            CD3DX12_RESOURCE_DESC::Buffer(prebuildInfo.ResultDataMaxSizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        Microsoft::WRL::ComPtr<ID3D12Resource> result;
        if (FAILED(m_Device->CreateCommittedResource(
                &defaultHeapProps,
                D3D12_HEAP_FLAG_NONE,
                &resultDesc,
                D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                nullptr,
                IID_PPV_ARGS(&result))))
        {
            Core::Logger::Error("DX12", std::string(debugName) + "の本体バッファ作成に失敗しました");
            return nullptr;
        }

        // 圧縮するときは、構築と同じコマンドリストで「圧縮後に必要なサイズ」を書き出させる。
        // 読み出しはCPUからなのでREADBACKヒープへ受ける
        Microsoft::WRL::ComPtr<ID3D12Resource> compactedSizeReadback;
        if (compact)
        {
            const CD3DX12_HEAP_PROPERTIES readbackProps(D3D12_HEAP_TYPE_READBACK);
            const CD3DX12_RESOURCE_DESC readbackDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(uint64_t));
            if (FAILED(m_Device->CreateCommittedResource(
                    &readbackProps, D3D12_HEAP_FLAG_NONE, &readbackDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                    IID_PPV_ARGS(&compactedSizeReadback))))
            {
                // 読み戻し先が作れないだけなら、圧縮を諦めて素のASで続行する(描画は成立する)
                Core::Logger::Error(
                    "DX12", std::string(debugName) + ": 圧縮後サイズの読み戻しバッファ作成に失敗しました。圧縮せずに続けます");
                compact = false;
            }
        }

        {
            // m_UploadCommandListへの記録は複数スレッドから同時に来うるため、
            // CreateBuffer/CreateTextureFromImageと同じミューテックスで直列化する
            std::lock_guard<std::mutex> lock(m_UploadMutex);

            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc{};
            buildDesc.Inputs = inputs;
            buildDesc.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
            buildDesc.DestAccelerationStructureData = result->GetGPUVirtualAddress();

            if (compact)
            {
                // 【EmitはBuildと同じ呼び出しへ渡す】別途EmitRaytracingAccelerationStructurePostbuildInfoを
                // 呼ぶ形でもよいが、その場合は構築完了を待つUAVバリアを自分で挟む必要がある。
                // BuildRaytracingAccelerationStructureの引数として渡せば順序はドライバが保証する
                D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC postbuildDesc{};
                postbuildDesc.InfoType =
                    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE;
                postbuildDesc.DestBuffer = compactedSizeReadback->GetGPUVirtualAddress();
                m_UploadCommandList4->BuildRaytracingAccelerationStructure(&buildDesc, 1, &postbuildDesc);
            }
            else
            {
                m_UploadCommandList4->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);
            }

            // TLASの構築はBLASの構築完了を前提とするため、UAVバリアで順序を保証する。
            // このエンジンではBLASを1本ずつ同期的に構築するため実際には不要だが、
            // 将来まとめて構築するよう変えたときに落とし穴にならないよう入れておく
            const D3D12_RESOURCE_BARRIER uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(result.Get());
            m_UploadCommandList4->ResourceBarrier(1, &uavBarrier);

            // スクラッチバッファ(ローカル変数)がこの関数を抜けるまでに解放されないよう、
            // 構築の完了をここで同期的に待つ。CreateBufferの初期データアップロードと同じ扱い
            UploadSubmitAndWait();
        }

        if (compact)
        {
            // --- 圧縮後サイズを読み、そのサイズの領域へコピーして元を捨てる ---------------
            uint64_t compactedSize = 0;
            void* mapped = nullptr;
            const D3D12_RANGE readRange{ 0, sizeof(uint64_t) };
            if (SUCCEEDED(compactedSizeReadback->Map(0, &readRange, &mapped)) && mapped)
            {
                std::memcpy(&compactedSize, mapped, sizeof(uint64_t));
                const D3D12_RANGE writeRange{ 0, 0 };
                compactedSizeReadback->Unmap(0, &writeRange);
            }
            else
            {
                Core::Logger::Error(
                    "DX12", std::string(debugName) + ": 圧縮後サイズの読み出しに失敗しました。圧縮せずに続けます");
            }

            // 0や元より大きい値が返ることは無いはずだが、返ってきたら圧縮しない。
            // 信用してバッファを作ると、コピー先が足りずGPUが落ちる
            if (compactedSize > 0 && compactedSize < prebuildInfo.ResultDataMaxSizeInBytes)
            {
                const CD3DX12_RESOURCE_DESC compactedDesc =
                    CD3DX12_RESOURCE_DESC::Buffer(compactedSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
                Microsoft::WRL::ComPtr<ID3D12Resource> compacted;
                if (SUCCEEDED(m_Device->CreateCommittedResource(
                        &defaultHeapProps, D3D12_HEAP_FLAG_NONE, &compactedDesc,
                        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr, IID_PPV_ARGS(&compacted))))
                {
                    {
                        std::lock_guard<std::mutex> lock(m_UploadMutex);
                        m_UploadCommandList4->CopyRaytracingAccelerationStructure(
                            compacted->GetGPUVirtualAddress(), result->GetGPUVirtualAddress(),
                            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT);
                        // コピー元(result)をこの関数の終わりで解放するため、完了を待つ
                        UploadSubmitAndWait();
                    }
                    m_BlasBytesBeforeCompaction.fetch_add(prebuildInfo.ResultDataMaxSizeInBytes, std::memory_order_relaxed);
                    m_BlasBytesAfterCompaction.fetch_add(compactedSize, std::memory_order_relaxed);
                    result = compacted;   // 以降は圧縮版を使う。元はここでの代入で解放される
                }
                else
                {
                    Core::Logger::Error(
                        "DX12", std::string(debugName) + ": 圧縮先バッファの作成に失敗しました。圧縮せずに続けます");
                }
            }
        }

        uint32_t srvIndex = DX12AccelerationStructure::kInvalid;
        if (createSrv)
        {
            // DXRのAS用SRVは他のSRVと作法が異なり、pResourceにnullptrを渡して
            // RaytracingAccelerationStructure.Locationへ「GPU仮想アドレス」を直接書く
            // (ディスクリプタがリソースではなくアドレスを指す)
            // TLASはシーンのジオメトリから作られるアセット由来のリソース
            srvIndex = m_AssetSrvCpuHeap->Allocate();
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = DXGI_FORMAT_UNKNOWN;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.RaytracingAccelerationStructure.Location = result->GetGPUVirtualAddress();
            m_Device->CreateShaderResourceView(nullptr, &srvDesc, m_AssetSrvCpuHeap->GetCpuHandle(srvIndex));
        }

        return std::make_unique<DX12AccelerationStructure>(this, result, srvIndex);
    }

    std::unique_ptr<IRHIAccelerationStructure> DX12Device::CreateBottomLevelAS(const BottomLevelASDesc& desc)
    {
        if (!m_SupportsRaytracing)
        {
            Core::Logger::Error("DX12", "CreateBottomLevelAS: レイトレーシング非対応の環境です。SupportsRaytracing()で分岐してください");
            return nullptr;
        }
        if (desc.Geometries.empty())
        {
            Core::Logger::Error("DX12", "CreateBottomLevelAS: ジオメトリが1つも指定されていません");
            return nullptr;
        }

        std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometryDescs;
        geometryDescs.reserve(desc.Geometries.size());
        for (const auto& geometry : desc.Geometries)
        {
            if (!geometry.VertexBuffer || !geometry.IndexBuffer || geometry.VertexCount == 0 || geometry.IndexCount == 0)
            {
                Core::Logger::Error("DX12", "CreateBottomLevelAS: 頂点/インデックスバッファが不正なジオメトリをスキップします");
                continue;
            }

            auto* vertexBuffer = static_cast<DX12Buffer*>(geometry.VertexBuffer);
            auto* indexBuffer = static_cast<DX12Buffer*>(geometry.IndexBuffer);

            D3D12_RAYTRACING_GEOMETRY_DESC geometryDesc{};
            geometryDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            // 不透明ジオメトリはAnyHit相当の判定を省ける(レイ側のRAY_FLAG_CULL_NON_OPAQUEも効く)。
            // アルファカットアウトのマテリアルはこのフラグを外し、呼び出し側がRayQuery::Proceed()の
            // ループで自前に抜き判定を行う
            geometryDesc.Flags = geometry.IsOpaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
            geometryDesc.Triangles.VertexBuffer.StartAddress =
                vertexBuffer->GetGPUVirtualAddress() + geometry.VertexPositionOffsetInBytes;
            geometryDesc.Triangles.VertexBuffer.StrideInBytes = geometry.VertexStrideInBytes;
            geometryDesc.Triangles.VertexCount = geometry.VertexCount;
            geometryDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
            geometryDesc.Triangles.IndexBuffer = indexBuffer->GetGPUVirtualAddress();
            geometryDesc.Triangles.IndexCount = geometry.IndexCount;
            geometryDesc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
            // 頂点はモデルのローカル空間のまま登録し、ワールドへの配置はTLASのインスタンス変換で行う
            geometryDesc.Triangles.Transform3x4 = 0;
            geometryDescs.push_back(geometryDesc);
        }

        if (geometryDescs.empty())
        {
            Core::Logger::Error("DX12", "CreateBottomLevelAS: 有効なジオメトリが1つも残りませんでした");
            return nullptr;
        }

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
        inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        // シーンは読み込み後に変形しない前提のため、更新(ALLOW_UPDATE)ではなくトレース速度を優先する。
        //
        // 【ALLOW_COMPACTIONを併せて立てる】BLASはドライバが最悪ケースで確保するため、実際に必要な
        // 量より大きい。PLATEAU 東京23区(三角形5,914万)では高速化構造だけで5.16GBを占め、
        // 専用VRAM 11,994MBのRTX 4070 Tiでも予算(9.0〜9.9GB)を超えてGPU待ちが出ていた
        // (docs/ImplementationDetail.md 58章)。圧縮はトレース性能を落とさずにこれを縮める。
        // 代償は構築時間で、圧縮後サイズの読み戻しとコピーのぶんGPUの同期が1回増える
        inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE |
                       D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION;
        inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        inputs.NumDescs = static_cast<UINT>(geometryDescs.size());
        inputs.pGeometryDescs = geometryDescs.data();

        // 【TLASは圧縮しない】インスタンス数ぶんしか無く小さいうえ、ストリーミングで常駐が
        // 変わるたびに作り直すため、圧縮のコストに見合わない
        return BuildAccelerationStructure(inputs, /*createSrv=*/false, "BLAS", /*compact=*/true);
    }

    std::unique_ptr<IRHIAccelerationStructure> DX12Device::CreateTopLevelAS(const TopLevelASDesc& desc)
    {
        if (!m_SupportsRaytracing)
        {
            Core::Logger::Error("DX12", "CreateTopLevelAS: レイトレーシング非対応の環境です。SupportsRaytracing()で分岐してください");
            return nullptr;
        }
        if (desc.Instances.empty())
        {
            Core::Logger::Error("DX12", "CreateTopLevelAS: インスタンスが1つも指定されていません");
            return nullptr;
        }

        std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instanceDescs;
        instanceDescs.reserve(desc.Instances.size());
        for (const auto& instance : desc.Instances)
        {
            auto* bottomLevel = static_cast<DX12AccelerationStructure*>(instance.BottomLevel);
            if (!bottomLevel)
            {
                Core::Logger::Error("DX12", "CreateTopLevelAS: BLASがnullptrのインスタンスをスキップします");
                continue;
            }

            D3D12_RAYTRACING_INSTANCE_DESC instanceDesc{};
            std::memcpy(instanceDesc.Transform, instance.Transform, sizeof(instanceDesc.Transform));
            // InstanceIDは24bitのビットフィールド。上位層が範囲外を渡した場合は静かに切り詰めず検出する
            if (instance.InstanceID > 0x00FFFFFFu)
            {
                Core::Logger::Error(
                    "DX12", "CreateTopLevelAS: InstanceIDが24bitの上限を超えています。このインスタンスをスキップします");
                continue;
            }
            instanceDesc.InstanceID = instance.InstanceID;
            instanceDesc.InstanceMask = 0xFF;
            instanceDesc.InstanceContributionToHitGroupIndex = 0;
            instanceDesc.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
            instanceDesc.AccelerationStructure = bottomLevel->GetGPUVirtualAddress();
            instanceDescs.push_back(instanceDesc);
        }

        if (instanceDescs.empty())
        {
            Core::Logger::Error("DX12", "CreateTopLevelAS: 有効なインスタンスが1つも残りませんでした");
            return nullptr;
        }

        // インスタンス記述子の配列はGPUから読まれるためUPLOADヒープへ置く。
        // 構築完了を同期的に待ってから解放するので、この関数のローカルで持てばよい
        const uint64_t instanceBufferSize = sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * instanceDescs.size();
        Microsoft::WRL::ComPtr<ID3D12Resource> instanceBuffer = CreateUploadBuffer(instanceBufferSize);
        void* mappedPtr = nullptr;
        const D3D12_RANGE readRange{ 0, 0 };
        if (FAILED(instanceBuffer->Map(0, &readRange, &mappedPtr)))
        {
            Core::Logger::Error("DX12", "CreateTopLevelAS: インスタンス記述子バッファのマップに失敗しました");
            return nullptr;
        }
        std::memcpy(mappedPtr, instanceDescs.data(), static_cast<size_t>(instanceBufferSize));
        instanceBuffer->Unmap(0, nullptr);

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
        inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        inputs.NumDescs = static_cast<UINT>(instanceDescs.size());
        inputs.InstanceDescs = instanceBuffer->GetGPUVirtualAddress();

        // 【ここでBLAS圧縮の効果を出す】TLASはシーンにつき1回しか作らないため、
        // 直前に積み上がったBLASの累計をまとめて報告できる。BLAS1本ごとに出すと767行になる
        const uint64_t before = m_BlasBytesBeforeCompaction.exchange(0, std::memory_order_relaxed);
        const uint64_t after = m_BlasBytesAfterCompaction.exchange(0, std::memory_order_relaxed);
        if (before > 0)
        {
            Core::Logger::Info(
                "DX12",
                "BLASの圧縮: " + std::to_string(before / (1024 * 1024)) + "MB -> " +
                    std::to_string(after / (1024 * 1024)) + "MB (" +
                    std::to_string(100 - (after * 100 / before)) + "% 削減)");
        }

        return BuildAccelerationStructure(inputs, /*createSrv=*/true, "TLAS");
    }

    std::unique_ptr<IRHIDevice> CreateDX12Device()
    {
        auto device = std::make_unique<DX12Device>();
        device->Initialize();
        return device;
    }
}
