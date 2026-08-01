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
#include "RHI/TextureImage.h"

namespace Kurenai::RHI
{
    namespace
    {
        // シェーダのレジスタ実測値(Sandbox/Shaders/*.hlsl)に基づく固定のルートシグネチャレイアウト
        // t0〜t16。最大はDeferredLighting.hlsl(G-Buffer4枚+スカイボックス+AO+エミッシブ+法線+
        // グローバルIBL3枚+反射プローブのキューブ配列2枚+プローブ一覧のStructuredBuffer+距離キューブ配列
        // +DDGIのオクタヘドラルアトラス2枚)。
        // DX11CommandList/DX12CommandListの同名の定数と必ず一致させること(3か所)
        constexpr uint32_t kTextureSlotCount = 17;
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
        // 1フレームあたりに払い出せるSRVテーブルブロック(t0〜t14のkTextureSlotCount個ひと組)の最大数。
        // 1フレーム中の(メッシュ数×パス数)を十分上回る値にしておく。実際に確保するヒープ容量は
        // これのkFrameCount倍(CPUがGPU完了を待たずに次フレームを記録し始めるため、直近kFrameCount
        // フレームぶんのブロックがまだGPUに読まれている可能性がある)
        constexpr uint32_t kMaxSrvTableBlocksPerFrame = 4096;
        // 定数バッファ(Usage==Constant)がリングとして持つスロット数。CPUがGPU完了を待たずに次フレームを
        // 記録し始めるため、直近kFrameCountフレームぶんのUpdateBuffer回数(メッシュ数など)を
        // 十分上回る値にしておく
        constexpr uint32_t kConstantBufferRingCapacity = 8192;

        // コンピュートシェーダー用ルートシグネチャのSRV/UAVディスクリプタテーブルレイアウト(t0〜t15, u0〜u3)。
        // SRVが16必要なのはレイトレーシングのパスで、TLAS + G-Buffer(Albedo/Normal/Material/Depth) +
        // SceneColor + スカイボックス + シーンジオメトリ4本(頂点属性・インデックス・メッシュ情報・
        // マテリアル) + インスタンス情報 を1回のディスパッチで同時に読むため。
        // DX12CommandList.h側の同名の定数と必ず一致させること
        constexpr uint32_t kComputeSrvSlotCount = 16;
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

        // 非シェーダー可視のCBV_SRV_UAVヒープ(テクスチャ/構造化バッファ作成時に
        // CreateShaderResourceView等の恒久的なビューを1つずつ確保する)の容量。
        //
        // DX12DescriptorHeapはロックを持たないため、確保・解放するスレッドごとに別のヒープへ
        // 分けてある(DX12Device.hのGetAssetSrvCpuHeap/GetRenderSrvCpuHeapのコメント参照)。
        //
        // アセット側: Bistro Exteriorでテクスチャ182枚 + RT統合バッファ5本 + TLAS 1本。
        // シーン切り替え時は旧シーンを先に破棄してから新シーンを読むため二重確保は起きない。
        // レンダー側: レンダーターゲットのSRV/UAVに加え、Hi-Zとブルームのミップ別UAV、
        // IBL・反射プローブのキューブマップ(プローブ数×6面×ミップ数のUAV)が効く。
        // どちらも非シェーダー可視ヒープでCPUメモリのみを消費するため、余裕を持った値にしておく
        constexpr uint32_t kAssetSrvCpuHeapCapacity = 2048;
        constexpr uint32_t kRenderSrvCpuHeapCapacity = 2048;

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

#if defined(_DEBUG)
        // デバッグレイヤーの指摘はそのままではデバッガの出力ウィンドウにしか出ず、
        // デバッガを繋がずに実行した場合に気付けない。ID3D12InfoQueueに溜まったメッセージを
        // 毎フレーム引き取ってKurenaiEngine.logへ出すことで、通常の実行でも検出できるようにする
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
            Core::Logger::Info("DX12", "デバッグレイヤーの指摘をKurenaiEngine.logへ出力します(警告以上のみ)");
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
        DetectShaderModelAndInitCompiler();
        DetectRaytracingSupport();

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
        m_ShaderVisibleSrvHeap = std::make_unique<DX12DescriptorHeap>(
            m_Device.Get(),
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
            (kGraphicsSrvHeapCapacityPerFrame + kComputeSrvHeapCapacityPerFrame) * kFrameCount,
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

            const uint32_t uavIndex = m_RenderSrvCpuHeap->Allocate();
            D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
            uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            uavDesc.Format = DXGI_FORMAT_UNKNOWN;
            uavDesc.Buffer.NumElements = desc.SizeInBytes / desc.StrideInBytes;
            uavDesc.Buffer.StructureByteStride = desc.StrideInBytes;
            m_Device->CreateUnorderedAccessView(resource.Get(), nullptr, &uavDesc, m_RenderSrvCpuHeap->GetCpuHandle(uavIndex));

            return std::make_unique<DX12Buffer>(this, m_RenderSrvCpuHeap.get(), resource, uavIndex, desc.SizeInBytes, desc.StrideInBytes);
        }

        // コンピュートがUAVで書き、ピクセルシェーダがSRVで読む構造化バッファ。CPUからは書き込まないため
        // 初期データもステージングリングも持たず、DEFAULTヒープにUAV+SRVの2つのディスクリプタを作る。
        // BufferUsage::Structuredと同じく、作成時点で直接UNORDERED_ACCESS状態にしておく
        // (m_UploadCommandListはInitialDataがある呼び出しでしかSubmitされないため、
        //  ここでバリアだけ積むと未実行のまま参照される可能性がある)
        if (desc.Usage == BufferUsage::StructuredRW)
        {
            if (desc.StrideInBytes == 0)
            {
                Core::Logger::Error("DX12", "StructuredRWバッファのStrideInBytesが0です。作成を中止します");
                throw std::runtime_error("StructuredRWバッファのStrideInBytesが0です");
            }

            const CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
            const CD3DX12_RESOURCE_DESC resourceDesc =
                CD3DX12_RESOURCE_DESC::Buffer(desc.SizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

            Microsoft::WRL::ComPtr<ID3D12Resource> resource;
            ThrowIfFailed(
                m_Device->CreateCommittedResource(
                    &heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&resource)),
                "読み書き構造化バッファ(StructuredRW)の作成に失敗しました");

            const uint32_t elementCount = desc.SizeInBytes / desc.StrideInBytes;

            const uint32_t uavIndex = m_RenderSrvCpuHeap->Allocate();
            D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
            uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            uavDesc.Format = DXGI_FORMAT_UNKNOWN;
            uavDesc.Buffer.NumElements = elementCount;
            uavDesc.Buffer.StructureByteStride = desc.StrideInBytes;
            m_Device->CreateUnorderedAccessView(resource.Get(), nullptr, &uavDesc, m_RenderSrvCpuHeap->GetCpuHandle(uavIndex));

            const uint32_t srvIndex = m_RenderSrvCpuHeap->Allocate();
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = DXGI_FORMAT_UNKNOWN;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Buffer.NumElements = elementCount;
            srvDesc.Buffer.StructureByteStride = desc.StrideInBytes;
            m_Device->CreateShaderResourceView(resource.Get(), &srvDesc, m_RenderSrvCpuHeap->GetCpuHandle(srvIndex));

            return std::make_unique<DX12Buffer>(this, m_RenderSrvCpuHeap.get(), resource, uavIndex, srvIndex, desc.SizeInBytes, desc.StrideInBytes);
        }

        // 作成時の初期データから変化しない読み取り専用の構造化バッファ。DEFAULTヒープにSRVだけを持ち、
        // CPU書き込み用のステージングリングは持たない(下のStructuredReadOnlyとの違いはそこだけ)。
        // レイトレーシングのシーンジオメトリのように数十MB規模かつシーン読み込み時にしか書かない
        // データで、ステージングリングぶんのUPLOADヒープを浪費しないためのUsage
        if (desc.Usage == BufferUsage::StructuredImmutable)
        {
            if (desc.StrideInBytes == 0)
            {
                Core::Logger::Error("DX12", "StructuredImmutableバッファのStrideInBytesが0です。作成を中止します");
                throw std::runtime_error("StructuredImmutableバッファのStrideInBytesが0です");
            }
            if (!desc.InitialData)
            {
                // 後から書き込む手段が無いUsageのため、初期データが無いと永久に0のままになる
                Core::Logger::Error("DX12", "StructuredImmutableバッファにInitialDataが指定されていません。作成を中止します");
                throw std::runtime_error("StructuredImmutableバッファにInitialDataが指定されていません");
            }

            const CD3DX12_HEAP_PROPERTIES defaultHeapProps(D3D12_HEAP_TYPE_DEFAULT);
            const CD3DX12_RESOURCE_DESC defaultResourceDesc = CD3DX12_RESOURCE_DESC::Buffer(desc.SizeInBytes);

            Microsoft::WRL::ComPtr<ID3D12Resource> resource;
            ThrowIfFailed(
                m_Device->CreateCommittedResource(
                    &defaultHeapProps, D3D12_HEAP_FLAG_NONE, &defaultResourceDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&resource)),
                "不変構造化バッファ(StructuredImmutable)の作成に失敗しました");

            {
                // 頂点/インデックスバッファの初期データアップロードと同じ手順
                // (UPLOADヒープの一時バッファ経由でDEFAULTヒープへコピーし、完了を同期的に待つ)。
                // m_UploadMutexはこの関数の先頭で既に確保済みのため、ここでは取り直さない
                // (std::mutexは再帰ロックできず、取り直すとdevice_or_resource_busyで失敗する)
                Microsoft::WRL::ComPtr<ID3D12Resource> uploadBuffer = CreateUploadBuffer(desc.SizeInBytes);
                void* mappedPtr = nullptr;
                const D3D12_RANGE readRange{ 0, 0 };
                ThrowIfFailed(uploadBuffer->Map(0, &readRange, &mappedPtr), "不変構造化バッファのステージングマップに失敗しました");
                std::memcpy(mappedPtr, desc.InitialData, desc.SizeInBytes);
                uploadBuffer->Unmap(0, nullptr);

                m_UploadCommandList->CopyBufferRegion(resource.Get(), 0, uploadBuffer.Get(), 0, desc.SizeInBytes);

                // 以後このバッファは読み取り専用。コンピュート/ピクセル双方から読めるようGENERIC_READへ移す
                // (DX12BufferのTransitionToは使わず、ここで一度だけ遷移させて固定する)
                const D3D12_RESOURCE_BARRIER toReadBarrier =
                    CD3DX12_RESOURCE_BARRIER::Transition(resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_GENERIC_READ);
                m_UploadCommandList->ResourceBarrier(1, &toReadBarrier);

                // アップロードバッファはこのスコープを抜けるまで生存させる必要があるため同期的に待つ
                UploadSubmitAndWait();
            }

            // レイトレーシングのシーンジオメトリ用。アセット由来なのでアセット側のヒープから確保する
            const uint32_t srvIndex = m_AssetSrvCpuHeap->Allocate();
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = DXGI_FORMAT_UNKNOWN;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Buffer.NumElements = desc.SizeInBytes / desc.StrideInBytes;
            srvDesc.Buffer.StructureByteStride = desc.StrideInBytes;
            m_Device->CreateShaderResourceView(resource.Get(), &srvDesc, m_AssetSrvCpuHeap->GetCpuHandle(srvIndex));

            // ステージングリングを持たない(uploadResource=nullptr、uploadRingCapacity=1)構成で作る。
            // GENERIC_READはPIXEL_SHADER_RESOURCE/NON_PIXEL_SHADER_RESOURCEを含むため、
            // DX12Buffer::TransitionToが呼ばれても余計なバリアが積まれないよう初期状態を合わせておく
            return std::make_unique<DX12Buffer>(this, m_AssetSrvCpuHeap.get(), resource, srvIndex, desc.SizeInBytes, desc.StrideInBytes, D3D12_RESOURCE_STATE_GENERIC_READ);
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

            const uint32_t srvIndex = m_RenderSrvCpuHeap->Allocate();
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = DXGI_FORMAT_UNKNOWN;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Buffer.NumElements = desc.SizeInBytes / desc.StrideInBytes;
            srvDesc.Buffer.StructureByteStride = desc.StrideInBytes;
            m_Device->CreateShaderResourceView(resource.Get(), &srvDesc, m_RenderSrvCpuHeap->GetCpuHandle(srvIndex));

            // CPUはGPU完了を待たずに次フレームの記録を始める(kFrameCount)ため、直近フレームぶんの
            // 書き込みが同時に生存できるだけのステージングリングを持たせる。
            // 1フレーム内に同じバッファへ複数回UpdateBufferすることがある(例: m_LightBufferは
            // DirectLightパスとTransparentパスの2回)ため、kFrameCount+1では足りない。
            // 「1フレームあたりの更新回数の上限×kFrameCount」に余裕を足した値にしておく
            // (超過はDX12Buffer::AdvanceUploadRingAndGetWritePtrがログで検出する)
            constexpr uint32_t kMaxStructuredUpdatesPerFrame = 4;
            constexpr uint32_t kStructuredReadOnlyUploadRingCapacity = kMaxStructuredUpdatesPerFrame * kFrameCount + 1;
            Microsoft::WRL::ComPtr<ID3D12Resource> uploadResource =
                CreateUploadBuffer(static_cast<uint64_t>(desc.SizeInBytes) * kStructuredReadOnlyUploadRingCapacity);

            void* uploadMappedPtr = nullptr;
            const D3D12_RANGE readRange{ 0, 0 };
            ThrowIfFailed(uploadResource->Map(0, &readRange, &uploadMappedPtr), "ステージングバッファのマップに失敗しました");

            return std::make_unique<DX12Buffer>(
                this,
                m_RenderSrvCpuHeap.get(),
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

            return std::make_unique<DX12Buffer>(this, resource, mappedPtr, slotSizeInBytes, desc.StrideInBytes, desc.Usage, ringCapacity);
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
        return std::make_unique<DX12Buffer>(this, resource, nullptr, desc.SizeInBytes, desc.StrideInBytes, desc.Usage, 1);
    }

    std::unique_ptr<IRHIShader> DX12Device::CreateShader(const ShaderDesc& desc)
    {
        // 通常経路: dxcでDXIL(SM 6.x)へコンパイルする。
        // SM 6.5でしか使えないインラインレイトレーシング(RayQuery)を扱えるようにするため
        if (m_ShaderCompiler.IsAvailable())
        {
            Microsoft::WRL::ComPtr<ID3DBlob> dxil = m_ShaderCompiler.Compile(desc.FilePath, desc.EntryPoint, desc.Stage);
            if (!dxil)
            {
                // 失敗の詳細はDX12ShaderCompiler::Compileがログ済み。
                // シェーダが1つでも作れなければ描画は成立しないため、従来どおり例外で止める
                throw std::runtime_error("シェーダのコンパイルに失敗しました(dxc)");
            }
            return std::make_unique<DX12Shader>(desc.Stage, dxil);
        }

        // フォールバック経路: dxcompiler.dllが無い/デバイスがSM 6.0未満の場合。
        // レイトレーシングは無効(DetectRaytracingSupport)だが、それ以外は従来どおり動作する
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
        // 既定は「時計回りが表・裏面カリング」。ミラーリング(負のスケール)を含むインスタンスは
        // スクリーン上での三角形の向きが反転するため、表裏の判定を入れ替えたPSOで描く
        // (RHIDesc.hのFrontCounterClockwise、docs/Architecture.html 10.2節)
        psoDesc.RasterizerState.FrontCounterClockwise = desc.FrontCounterClockwise ? TRUE : FALSE;
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
        // 実際にDSVがバインドされる描画では、深度テストの有無に関わらずフォーマットを申告する必要がある
        // (エンジン内の深度バッファはオフスクリーン・スワップチェインともD32_FLOATで統一している)
        psoDesc.DSVFormat = (desc.HasDepthStencil || desc.DepthTargetAttached) ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_UNKNOWN;
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

        // ファイル/デコード済み画像から作るテクスチャ(マテリアル・スカイボックス・プレースホルダ)は
        // すべてアセット由来。シーン読み込み専用スレッドが確保・解放するためアセット側のヒープを使う
        const uint32_t srvIndex = m_AssetSrvCpuHeap->Allocate();
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
        m_Device->CreateShaderResourceView(resource.Get(), &srvDesc, m_AssetSrvCpuHeap->GetCpuHandle(srvIndex));

        auto texture = std::make_unique<DX12Texture>(
            this, m_AssetSrvCpuHeap.get(), resource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, srvIndex, DX12Texture::kInvalid, DX12Texture::kInvalid);

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

    // --- レイトレーシング -------------------------------------------------------------------

    void DX12Device::DetectShaderModelAndInitCompiler()
    {
        // D3D12_FEATURE_SHADER_MODELは「HighestShaderModelへ聞きたい上限を入れて呼ぶと、
        // 対応している値まで引き下げて返す」APIだが、ランタイムが知らない列挙値を渡すと
        // E_INVALIDARGを返す。そのため上から順に下げながら問い合わせる
        static constexpr D3D_SHADER_MODEL kCandidates[] = {
            D3D_SHADER_MODEL_6_5, D3D_SHADER_MODEL_6_4, D3D_SHADER_MODEL_6_3,
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
                "DX12", "対応シェーダーモデルを判定できませんでした。d3dcompiler/SM 5.0で動作します");
            return;
        }

        // Initializeは失敗しても例外を投げない(理由はログへ出る)。
        // 戻り値がfalseの場合はCreateShaderがd3dcompiler/SM 5.0へフォールバックする
        m_ShaderCompiler.Initialize(m_HighestShaderModel);
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
        // DXILを出力できるdxcでしかコンパイルできない。ハードウェアがTier 1.1でも
        // ここが満たせなければトレースするシェーダーを作れないため非対応として扱う
        // (Phase 0でdxc/SM 6.xへ移行した理由そのもの)
        if (!m_ShaderCompiler.IsAvailable() || m_ShaderCompiler.GetShaderModel() < D3D_SHADER_MODEL_6_5)
        {
            Core::Logger::Warning(
                "DX12",
                "レイトレーシング非対応: シェーダーモデル6.5でのコンパイルができません"
                "(RayQueryにはdxcとSM 6.5が必要です。dxcompiler.dllの配置とデバイスの対応状況を確認してください)");
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
        const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& inputs, bool createSrv, const char* debugName)
    {
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

        {
            // m_UploadCommandListへの記録は複数スレッドから同時に来うるため、
            // CreateBuffer/CreateTextureFromImageと同じミューテックスで直列化する
            std::lock_guard<std::mutex> lock(m_UploadMutex);

            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc{};
            buildDesc.Inputs = inputs;
            buildDesc.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
            buildDesc.DestAccelerationStructureData = result->GetGPUVirtualAddress();
            m_UploadCommandList4->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

            // TLASの構築はBLASの構築完了を前提とするため、UAVバリアで順序を保証する。
            // このエンジンではBLASを1本ずつ同期的に構築するため実際には不要だが、
            // 将来まとめて構築するよう変えたときに落とし穴にならないよう入れておく
            const D3D12_RESOURCE_BARRIER uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(result.Get());
            m_UploadCommandList4->ResourceBarrier(1, &uavBarrier);

            // スクラッチバッファ(ローカル変数)がこの関数を抜けるまでに解放されないよう、
            // 構築の完了をここで同期的に待つ。CreateBufferの初期データアップロードと同じ扱い
            UploadSubmitAndWait();
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
        // シーンは読み込み後に変形しない前提のため、更新(ALLOW_UPDATE)ではなくトレース速度を優先する
        inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        inputs.NumDescs = static_cast<UINT>(geometryDescs.size());
        inputs.pGeometryDescs = geometryDescs.data();

        return BuildAccelerationStructure(inputs, /*createSrv=*/false, "BLAS");
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

        return BuildAccelerationStructure(inputs, /*createSrv=*/true, "TLAS");
    }

    std::unique_ptr<IRHIDevice> CreateDX12Device()
    {
        auto device = std::make_unique<DX12Device>();
        device->Initialize();
        return device;
    }
}
