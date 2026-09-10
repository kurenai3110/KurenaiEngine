#include "DX12Device.h"
#include "DX12DeviceInternal.h"

#include <d3dx12.h>

#include <stdexcept>
#include <string>
#include <vector>

#include "DX12AccelerationStructure.h"
#include "DX12Buffer.h"
#include "DX12Util.h"
#include "Core/StringUtil.h"

// レイトレーシングの加速構造(BLAS / TLAS)の構築。
// DX12Device のメンバ関数のまま、翻訳単位だけをここへ分けている
// (宣言は DX12Device.h のまま。IRHIDevice のインターフェースは1行も変えていない)
namespace Kurenai::RHI
{
    using namespace DX12Internal;

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
}
