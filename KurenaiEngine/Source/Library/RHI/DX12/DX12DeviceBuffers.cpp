#include "DX12Device.h"
#include "DX12DeviceInternal.h"

#include <d3dx12.h>

#include <stdexcept>
#include <string>

#include "DX12Buffer.h"
#include "DX12Util.h"
#include "Core/StringUtil.h"

// バッファの確保と、フレーム内で使い回すディスクリプタ表の割り当て。
// DX12Device のメンバ関数のまま、翻訳単位だけをここへ分けている
// (宣言は DX12Device.h のまま。IRHIDevice のインターフェースは1行も変えていない)
namespace Kurenai::RHI
{
    using namespace DX12Internal;

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

        // 【区画の末尾をまたぐブロックを返さない】区画の直後にはbindless区画が続いているため、
        // またいだブロックへ書き込むとbindlessのディスクリプタを踏み潰す。
        // そうなるとメッシュシェーダーがジオメトリを引けなくなり、
        // 「G-Bufferだけが空になり、頂点シェーダー経路へ切り替えると直る」という
        // 原因の分かりにくい壊れ方をする(実際に一度この壊し方をしている)。
        //
        // 【以前は問題にならなかった】払い出し単位が常にkComputeTableSlotCount固定で、
        // 区画容量もその倍数だったため末尾がちょうど揃っていた。
        // 1個だけ借りる呼び出し(ClearUnorderedAccessBufferUint)が入った時点でこの前提は消える
        if (m_NextComputeTableIndex + count > totalCapacity)
        {
            m_NextComputeTableIndex = 0;
        }

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
            // 1フレーム内に同じバッファへ複数回UpdateBufferすることがある(例: 3Dエンジンのライトのリストは
            // DirectLightパスとTransparentパスの2回)ため、kFrameCount+1では足りない。
            // 「1フレームあたりの更新回数の上限×kFrameCount」に余裕を足した値にしておく
            // (超過はDX12Buffer::AdvanceUploadRingAndGetWritePtrがログで検出する)。
            //
            // 上限はバッファごとにBufferDesc::MaxUpdatesPerFrameで宣言する。既定値は4で、
            // 明示しないバッファの段数は従来と変わらない
            uint32_t maxUpdatesPerFrame = desc.MaxUpdatesPerFrame;
            if (maxUpdatesPerFrame == 0)
            {
                Core::Logger::Error("DX12", "BufferDesc::MaxUpdatesPerFrameが0です。既定値の4として扱います");
                maxUpdatesPerFrame = 4;
            }
            const uint32_t kStructuredReadOnlyUploadRingCapacity = maxUpdatesPerFrame * kFrameCount + 1;
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

        // GPUが書いた値をCPUで読むための受け皿。READBACKヒープに作り、作成時から
        // マップしたままにしておく(ReadbackDataは単なるmemcpyで済む)。
        // シェーダーからは見えないのでSRVもUAVも張らない
        if (desc.Usage == BufferUsage::Readback)
        {
            if (desc.SizeInBytes == 0)
            {
                Core::Logger::Error("DX12", "Readbackバッファのサイズが0です。作成を中止します");
                throw std::runtime_error("Readbackバッファのサイズが不正です");
            }

            const CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_READBACK);
            const CD3DX12_RESOURCE_DESC resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(desc.SizeInBytes);

            Microsoft::WRL::ComPtr<ID3D12Resource> resource;
            ThrowIfFailed(
                m_Device->CreateCommittedResource(
                    // READBACKヒープのリソースはCOPY_DEST状態でしか作れない(D3D12の仕様)
                    &heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                    IID_PPV_ARGS(&resource)),
                "リードバックバッファの作成に失敗しました");

            // 【永続マップする】読むたびにMap/Unmapすると、Unmapへ渡す書き込み範囲の指定を
            // 誤ったときにドライバがキャッシュを吐き出して遅くなる。読み取り専用なので
            // マップしたままで問題は無い(定数バッファのUPLOADヒープと同じ扱い)
            void* mappedPtr = nullptr;
            // 読み取り範囲は「全体」。CPUが書かないのでUnmapは破棄時のドライバ任せでよい
            const D3D12_RANGE readRange{ 0, desc.SizeInBytes };
            ThrowIfFailed(resource->Map(0, &readRange, &mappedPtr), "リードバックバッファのマップに失敗しました");

            return std::make_unique<DX12Buffer>(
                this, resource, mappedPtr, desc.SizeInBytes, desc.StrideInBytes, BufferUsage::Readback);
        }

        // 間接ディスパッチの引数バッファ。コンピュートシェーダーがRWByteAddressBufferとして
        // スレッドグループ数を書き、そのままExecuteIndirectへ渡す。
        // CPUからは書かないため初期データもステージングリングも持たず、DEFAULTヒープに
        // raw UAVを1つだけ作る(StructuredRWと同じく作成時点でUNORDERED_ACCESS状態にしておく)
        if (desc.Usage == BufferUsage::IndirectArgs)
        {
            // raw UAVは4バイト単位でアドレスを刻むため、サイズも4の倍数でなければ
            // NumElementsが切り捨てられて末尾が書けなくなる
            if (desc.SizeInBytes == 0 || (desc.SizeInBytes % 4) != 0)
            {
                Core::Logger::Error("DX12", "IndirectArgsバッファのサイズが0か4の倍数ではありません。作成を中止します");
                throw std::runtime_error("IndirectArgsバッファのサイズが不正です");
            }

            const CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
            const CD3DX12_RESOURCE_DESC resourceDesc =
                CD3DX12_RESOURCE_DESC::Buffer(desc.SizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

            Microsoft::WRL::ComPtr<ID3D12Resource> resource;
            ThrowIfFailed(
                m_Device->CreateCommittedResource(
                    &heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&resource)),
                "間接ディスパッチ引数バッファ(IndirectArgs)の作成に失敗しました");

            // raw(ByteAddress)ビュー。DX11がDRAWINDIRECT_ARGSと構造化を同時に指定できない制約に
            // 合わせて、DX12側も同じrawの形にしてHLSLを1本で済ませる(RHIEnums.hのコメント参照)
            const uint32_t uavIndex = m_RenderSrvCpuHeap->Allocate();
            D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
            uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            uavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
            uavDesc.Buffer.NumElements = desc.SizeInBytes / 4;
            uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
            m_Device->CreateUnorderedAccessView(resource.Get(), nullptr, &uavDesc, m_RenderSrvCpuHeap->GetCpuHandle(uavIndex));

            return std::make_unique<DX12Buffer>(
                this, m_RenderSrvCpuHeap.get(), resource, uavIndex, desc.SizeInBytes, desc.StrideInBytes, BufferUsage::IndirectArgs);
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
            // 既定では足りないバッファ(メッシュ数×パス数だけ書かれるObjectConstantsなど)は
            // BufferDesc::MaxConstantUpdatesPerFrameで必要な段数を要求できる
            const uint32_t ringCapacity = (desc.MaxConstantUpdatesPerFrame > 0)
                ? (desc.MaxConstantUpdatesPerFrame * kFrameCount)
                : kConstantBufferRingCapacity;

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

        // シェーダーが頂点/インデックスを自分で読むための追加SRV(BufferDesc::ShaderReadable)。
        // 頂点バッファビュー・インデックスバッファビューと同じリソースへ、
        // StructuredBuffer<Vertex> / StructuredBuffer<uint> としてのビューを重ねて張る
        // (同一リソースに複数のビューを持たせるのはD3D12では通常の使い方で、複製は生じない)。
        // Usage自体はVertex/Indexのままなので、従来の入力アセンブラ経由の描画にも一切影響しない。
        //
        // 【誰が使うか】メッシュシェーダー経路(頂点のみ)と、コンピュートシェーダーによる
        // 自前ラスタライザ経路(頂点+インデックス)。どちらもResourceDescriptorHeap経由で引く
        DX12DescriptorHeap* vertexSrvHeap = nullptr;
        uint32_t vertexSrvIndex = DX12Buffer::kInvalid;
        if (desc.ShaderReadable && (desc.Usage == BufferUsage::Vertex || desc.Usage == BufferUsage::Index))
        {
            if (desc.StrideInBytes == 0)
            {
                Core::Logger::Error(
                    "DX12", "ShaderReadableな頂点/インデックスバッファのStrideInBytesが0です(SRVを作れないためbindlessでは読めません)");
            }
            else
            {
                // 頂点バッファはアセット由来(ModelLoader)なのでアセット側のヒープから確保する
                vertexSrvHeap = m_AssetSrvCpuHeap.get();
                vertexSrvIndex = vertexSrvHeap->Allocate();

                D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
                srvDesc.Format = DXGI_FORMAT_UNKNOWN;
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srvDesc.Buffer.NumElements = desc.SizeInBytes / desc.StrideInBytes;
                srvDesc.Buffer.StructureByteStride = desc.StrideInBytes;
                m_Device->CreateShaderResourceView(resource.Get(), &srvDesc, vertexSrvHeap->GetCpuHandle(vertexSrvIndex));
            }
        }

        // DEFAULTヒープはCPUからマップできないためnullptrを渡す。頂点/インデックスバッファは
        // 初回アップロード後書き換えない(ringCapacity=1でAdvanceRingAndGetWritePtrは呼ばれない)
        return std::make_unique<DX12Buffer>(
            this, resource, nullptr, desc.SizeInBytes, desc.StrideInBytes, desc.Usage, 1, vertexSrvHeap, vertexSrvIndex);
    }
}
