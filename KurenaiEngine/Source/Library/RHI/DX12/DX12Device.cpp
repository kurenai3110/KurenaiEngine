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
