#include "RaytracingScene.h"

#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string>

#include "Core/Logger.h"

#include "Vertex.h"

namespace Kurenai::Assets
{
    namespace
    {
        // 統合バッファをStructuredImmutableとして作る共通処理。
        // 要素数0のバッファはD3D12/D3D11ともに作れないため、呼び出し側で空でないことを保証すること
        template <typename T>
        std::unique_ptr<RHI::IRHIBuffer> CreateImmutableStructuredBuffer(
            RHI::IRHIDevice& device, const std::vector<T>& elements, uint64_t& inOutTotalBytes)
        {
            RHI::BufferDesc desc;
            desc.Usage = RHI::BufferUsage::StructuredImmutable;
            desc.SizeInBytes = static_cast<uint32_t>(elements.size() * sizeof(T));
            desc.StrideInBytes = static_cast<uint32_t>(sizeof(T));
            desc.InitialData = elements.data();
            inOutTotalBytes += desc.SizeInBytes;
            return device.CreateBuffer(desc);
        }

        // マテリアルのテクスチャをbindless区画へ登録し、シェーダーが使う番号を返す。
        // テクスチャが無い(nullptr)場合と、デバイスがbindless非対応の場合はどちらも
        // kInvalidBindlessIndexになり、シェーダー側は白1x1/フラット法線のプレースホルダーへ落ちる。
        //
        // 【同じテクスチャを複数のマテリアルが指しても無駄にならない】RegisterBindlessは
        // 登録済みのリソースに対しては同じ番号を返すため、Sponzaのように1枚のテクスチャを
        // 多数のメッシュが共有する構成でも区画の消費はテクスチャの実枚数どまりになる
        uint32_t RegisterMaterialTexture(RHI::IRHIDevice& device, RHI::IRHITexture* texture)
        {
            if (!texture)
            {
                return RHI::kInvalidBindlessIndex;
            }
            return device.RegisterBindless(texture);
        }
    }

    void RaytracingScene::Reset()
    {
        // TLASはBLASのGPU仮想アドレスを参照しているため、先にTLASを解放してからBLASを解放する
        m_TopLevelAS.reset();
        m_BottomLevelAS.clear();

        m_VertexAttributeBuffer.reset();
        m_IndexBuffer.reset();
        m_MeshInfoBuffer.reset();
        m_InstanceInfoBuffer.reset();
        m_MaterialBuffer.reset();

        m_InstanceCount = 0;
        m_MeshCount = 0;
        m_TriangleCount = 0;
        m_GeometryBufferBytes = 0;
    }

    bool RaytracingScene::Build(RHI::IRHIDevice& device, Scene& scene)
    {
        // 作り直しの前に必ず既存の資源を捨てる。呼び出し側がWaitForGPUIdle済みである前提
        Reset();

        if (!device.SupportsRaytracing())
        {
            // 非対応環境では上位層がそもそも呼ばない想定だが、呼ばれても静かに何もしない
            // (エラーではなく通常の分岐なのでInfoに留める)
            Core::Logger::Info("RaytracingScene", "レイトレーシング非対応の環境のため、高速化構造は構築しません");
            return false;
        }
        if (scene.Instances.empty())
        {
            Core::Logger::Warning("RaytracingScene", "シーンにモデルインスタンスが1つもないため、高速化構造は構築しません");
            return false;
        }

        const auto startTime = std::chrono::steady_clock::now();

        // --- シーン全体の統合バッファをCPU側で組み立てる -------------------------------------
        std::vector<RaytracingVertexAttribute> attributes;
        std::vector<uint32_t> indices;
        std::vector<RaytracingMeshInfo> meshInfos;
        std::vector<RaytracingInstanceInfo> instanceInfos;
        std::vector<RaytracingMaterial> materials;
        instanceInfos.reserve(scene.Instances.size());

        for (ModelInstance& instance : scene.Instances)
        {
            const Model& model = instance.Model;

            RaytracingInstanceInfo instanceInfo;
            std::memcpy(instanceInfo.NormalMatrix, &instance.NormalMatrix, sizeof(instanceInfo.NormalMatrix));
            instanceInfo.MeshInfoOffset = static_cast<uint32_t>(meshInfos.size());
            instanceInfos.push_back(instanceInfo);

            // このインスタンスぶんの頂点属性・インデックスを統合バッファの末尾へ連結する。
            // 連結でずれる分は各メッシュのMeshInfoのオフセットへ足し込むので、
            // インデックスの値そのもの(メッシュ内の相対番号)は書き換えない
            const uint32_t attributeBase = static_cast<uint32_t>(attributes.size());
            const uint32_t indexBase = static_cast<uint32_t>(indices.size());
            attributes.insert(attributes.end(), model.RaytracingAttributes.begin(), model.RaytracingAttributes.end());
            indices.insert(indices.end(), model.RaytracingIndices.begin(), model.RaytracingIndices.end());

            for (const Mesh& mesh : model.Meshes)
            {
                RaytracingMaterial material;
                material.BaseColorFactor[0] = mesh.BaseColorFactor[0];
                material.BaseColorFactor[1] = mesh.BaseColorFactor[1];
                material.BaseColorFactor[2] = mesh.BaseColorFactor[2];
                material.BaseColorFactor[3] = mesh.BaseColorFactor[3];
                material.EmissiveFactor[0] = mesh.EmissiveFactor[0];
                material.EmissiveFactor[1] = mesh.EmissiveFactor[1];
                material.EmissiveFactor[2] = mesh.EmissiveFactor[2];
                material.MetallicFactor = mesh.MetallicFactor;
                material.RoughnessFactor = mesh.RoughnessFactor;
                material.AlphaCutoff = mesh.AlphaCutoff;
                material.Flags = mesh.IsTransparent ? kRaytracingMaterialFlagTransparent : 0u;
                // ヒット面のテクスチャ。bindless非対応環境ではすべて無効値になり、
                // シェーダーは従来どおり定数の係数だけで陰影を決める
                material.BaseColorTextureIndex = RegisterMaterialTexture(device, mesh.BaseColorTexture);
                material.NormalTextureIndex = RegisterMaterialTexture(device, mesh.NormalTexture);
                material.MetallicRoughnessTextureIndex = RegisterMaterialTexture(device, mesh.MetallicRoughnessTexture);
                material.EmissiveTextureIndex = RegisterMaterialTexture(device, mesh.EmissiveTexture);

                RaytracingMeshInfo meshInfo;
                meshInfo.AttributeOffset = attributeBase + mesh.RaytracingAttributeOffset;
                meshInfo.IndexOffset = indexBase + mesh.RaytracingIndexOffset;
                // 現状はメッシュとマテリアルを1対1で持つ(同一マテリアルの共有は将来の最適化)
                meshInfo.MaterialIndex = static_cast<uint32_t>(materials.size());

                materials.push_back(material);
                meshInfos.push_back(meshInfo);

                m_TriangleCount += mesh.IndexCount / 3;
            }
        }

        if (meshInfos.empty() || attributes.empty() || indices.empty())
        {
            Core::Logger::Warning(
                "RaytracingScene", "シーンに三角形ジオメトリが1つもないため、高速化構造は構築しません");
            Reset();
            return false;
        }

        // --- 統合バッファをGPUへ送る ---------------------------------------------------------
        try
        {
            m_VertexAttributeBuffer = CreateImmutableStructuredBuffer(device, attributes, m_GeometryBufferBytes);
            m_IndexBuffer = CreateImmutableStructuredBuffer(device, indices, m_GeometryBufferBytes);
            m_MeshInfoBuffer = CreateImmutableStructuredBuffer(device, meshInfos, m_GeometryBufferBytes);
            m_InstanceInfoBuffer = CreateImmutableStructuredBuffer(device, instanceInfos, m_GeometryBufferBytes);
            m_MaterialBuffer = CreateImmutableStructuredBuffer(device, materials, m_GeometryBufferBytes);
        }
        catch (const std::exception& error)
        {
            // CreateBufferはGPUリソース確保に失敗すると例外を投げる。レイトレーシングは
            // 従来手法へフォールバックできる機能なので、ここでアプリを落とさず無効化に留める
            Core::Logger::Error(
                "RaytracingScene", std::string("統合ジオメトリバッファの作成に失敗しました: ") + error.what());
            Reset();
            return false;
        }

        // CPU側のコピーはGPUへ送った時点で用済み。次のシーンを読むまで抱えると
        // 大規模シーンでは100MB規模の無駄になるため、ここで解放する
        for (ModelInstance& instance : scene.Instances)
        {
            instance.Model.RaytracingAttributes.clear();
            instance.Model.RaytracingAttributes.shrink_to_fit();
            instance.Model.RaytracingIndices.clear();
            instance.Model.RaytracingIndices.shrink_to_fit();
        }

        // --- BLAS(モデルインスタンスごと)を構築する -----------------------------------------
        m_BottomLevelAS.reserve(scene.Instances.size());
        std::vector<RHI::ASInstanceDesc> tlasInstances;
        tlasInstances.reserve(scene.Instances.size());

        for (size_t i = 0; i < scene.Instances.size(); ++i)
        {
            const ModelInstance& instance = scene.Instances[i];

            RHI::BottomLevelASDesc blasDesc;
            blasDesc.Geometries.reserve(instance.Model.Meshes.size());
            for (const Mesh& mesh : instance.Model.Meshes)
            {
                RHI::ASGeometryDesc geometry;
                geometry.VertexBuffer = mesh.VertexBuffer.get();
                geometry.VertexCount = mesh.VertexCount;
                geometry.VertexStrideInBytes = static_cast<uint32_t>(sizeof(Vertex));
                // Assets::Vertexは位置が先頭にあるためオフセットは0
                geometry.VertexPositionOffsetInBytes = 0;
                geometry.IndexBuffer = mesh.IndexBuffer.get();
                geometry.IndexCount = mesh.IndexCount;
                // アルファカットアウト・半透明のマテリアルは「当たっても抜ける可能性がある」ため
                // 不透明として登録しない。レイ側がRAY_FLAG_CULL_NON_OPAQUEを付ければ丸ごと除外され、
                // 付けなければ呼び出し側がRayQuery::Proceed()のループで抜き判定を行える
                geometry.IsOpaque = (mesh.AlphaCutoff <= 0.0f) && !mesh.IsTransparent;
                blasDesc.Geometries.push_back(geometry);
            }

            std::unique_ptr<RHI::IRHIAccelerationStructure> blas = device.CreateBottomLevelAS(blasDesc);
            if (!blas)
            {
                // CreateBottomLevelAS側が理由をログ出力済み。1つでも欠けるとTLASの
                // InstanceIDとInstanceInfoBufferの対応が崩れるため、全体を諦める
                Core::Logger::Error(
                    "RaytracingScene",
                    "インスタンス[" + std::to_string(i) + "]のBLAS構築に失敗しました。レイトレーシングを無効にします");
                Reset();
                return false;
            }

            RHI::ASInstanceDesc tlasInstance;
            tlasInstance.BottomLevel = blas.get();
            // ModelInstance::Worldは転置済み(HLSLへそのまま渡せる形)で保持されているため、
            // その先頭3行がD3D12_RAYTRACING_INSTANCE_DESC::Transformの行優先3x4と一致する
            for (uint32_t row = 0; row < 3; ++row)
            {
                for (uint32_t column = 0; column < 4; ++column)
                {
                    tlasInstance.Transform[row][column] = instance.World.m[row][column];
                }
            }
            // シェーダーはこの値でInstanceInfoBufferを引く。配列の添字と一致させる
            tlasInstance.InstanceID = static_cast<uint32_t>(i);

            m_BottomLevelAS.push_back(std::move(blas));
            tlasInstances.push_back(tlasInstance);
        }

        // --- TLASを構築する -------------------------------------------------------------------
        RHI::TopLevelASDesc tlasDesc;
        tlasDesc.Instances = std::move(tlasInstances);
        m_TopLevelAS = device.CreateTopLevelAS(tlasDesc);
        if (!m_TopLevelAS)
        {
            Core::Logger::Error("RaytracingScene", "TLASの構築に失敗しました。レイトレーシングを無効にします");
            Reset();
            return false;
        }

        m_InstanceCount = static_cast<uint32_t>(scene.Instances.size());
        m_MeshCount = static_cast<uint32_t>(meshInfos.size());

        const float elapsedMs =
            std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - startTime).count();
        Core::Logger::Info(
            "RaytracingScene",
            "高速化構造を構築しました: インスタンス" + std::to_string(m_InstanceCount) + " / メッシュ" +
                std::to_string(m_MeshCount) + " / 三角形" + std::to_string(m_TriangleCount) + " / 統合バッファ" +
                std::to_string(m_GeometryBufferBytes / (1024 * 1024)) + "MB / " + std::to_string(elapsedMs) + "ms");

        return true;
    }
}
