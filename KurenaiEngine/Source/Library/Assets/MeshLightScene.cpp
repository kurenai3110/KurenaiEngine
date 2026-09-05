#include "MeshLightScene.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>

#include <DirectXMath.h>

#include "Core/Logger.h"

#include "Model.h"

namespace Kurenai::Assets
{
    namespace
    {
        // 影響半径の下限を「三角形自身の大きさ」の何倍にするか。
        //
        // 【下限が要る理由】R が三角形自身の大きさより小さいと、窓関数の切り際が面の上で見え、
        // **発光体の周りに硬い輪**が出る。sqrt(A) が三角形の代表寸法
        constexpr float kMinInfluenceScale = 4.0f;
        // 影響半径の上限[m]。巨大な発光パネルは R が数百mになり、全タイルに入って
        // 枝刈りが効かなくなる。**上限で切るとエネルギーを落とす**ので、切った数を数える
        constexpr float kMaxInfluenceRadius = 200.0f;

        float Luminance(const float rgb[3])
        {
            return 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
        }
    }

    void MeshLightScene::Reset()
    {
        m_TriangleBuffer.reset();
        m_TriangleCount = 0;
        m_ClusterCount = 0;
        m_TotalArea = 0.0;
        m_TotalFlux = 0.0;
        m_TriangleBufferBytes = 0;
        m_RadiusClampedCount = 0;
    }

    bool MeshLightScene::Build(RHI::IRHIDevice& device, const Scene& scene, float cutoffIrradiance)
    {
        Reset();

        if (scene.EmissiveProxies.empty())
        {
            // エミッシブなメッシュが無いシーン。エラーではないのでログも出さない
            return false;
        }
        if (!(cutoffIrradiance > 0.0f))
        {
            Core::Logger::Error(
                "MeshLightScene",
                "打ち切り照度が正の値ではありません(" + std::to_string(cutoffIrradiance) +
                    ")。メッシュライトのテーブルは構築しません");
            return false;
        }

        const auto startTime = std::chrono::steady_clock::now();

        std::vector<GPUEmissiveTriangle> triangles;

        // 【プロキシを起点に走査する】プロキシが指すかたまりの三角形だけを取る。
        // こうしておけば「プロキシは出たのに三角形が出ない」が構造的に起こらない
        for (const EmissiveProxy& proxy : scene.EmissiveProxies)
        {
            if (proxy.InstanceIndex >= scene.Instances.size())
            {
                Core::Logger::Warning(
                    "MeshLightScene",
                    "エミッシブ光源のインスタンス番号がシーンの範囲外です: " +
                        std::to_string(proxy.InstanceIndex));
                continue;
            }
            const ModelInstance& instance = scene.Instances[proxy.InstanceIndex];
            // 【段0だけを見る】プロキシも段0からしか作られない(SceneLoader)。
            // 粗い段を混ぜると、段の切り替わりで面積が跳ねる
            if (!instance.Model || proxy.MeshIndex >= instance.Model->Meshes.size())
            {
                continue;
            }
            const Mesh& mesh = instance.Model->Meshes[proxy.MeshIndex];
            if (proxy.ClusterIndex >= mesh.EmissiveClusters.size())
            {
                continue;
            }
            const EmissiveCluster& cluster = mesh.EmissiveClusters[proxy.ClusterIndex];
            const uint32_t begin = cluster.TriangleOffset;
            const uint32_t end = begin + cluster.TriangleCount;
            if (end > mesh.EmissiveTriangles.size())
            {
                Core::Logger::Warning(
                    "MeshLightScene",
                    "かたまりが指す三角形の範囲がメッシュの外にあります(メッシュ " +
                        std::to_string(proxy.MeshIndex) + " / かたまり " +
                        std::to_string(proxy.ClusterIndex) + ")");
                continue;
            }

            const DirectX::XMMATRIX world =
                DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&instance.World));

            // 【放射輝度は段階1と同じ値を使う】proxy.RadianceBase は
            // EmissiveFactor × EmissiveTextureAverage で、シーン全体の倍率も露出も
            // 掛かっていない。ここで別の値を使うと、段階1と段階2の突き合わせ
            // (遠方で一致するはず)が成立しなくなる。
            // **テクスチャを三角形ごとに引くのは 2f** ―― いまは面ごとの平均なので、
            // 総フラックスは正しいが「暗い背景に明るいグリフ」の看板では位置が広がりすぎる
            const float luminance = Luminance(proxy.RadianceBase);

            // かたまり全体の軸上光度 I(0) = max(RadianceBase) * A。影響半径をここから解く。
            // 【段階1の MakeGPULightFromEmissiveProxy と同じ量にすること】あちらは
            // peak = max(ColorRange.rgb) = max(RadianceBase) * intensity * Area を使う。
            // 自発光の強度倍率は毎フレーム変わるのでここでは 1 として焼き、
            // シェーダ側で sqrt(intensity) を掛けて伸縮させる(MeshLighting.hlsli)
            const float clusterPeakIntensity =
                std::max({ proxy.RadianceBase[0], proxy.RadianceBase[1], proxy.RadianceBase[2] }) *
                proxy.Area;

            // 両面発光は glTF の doubleSided に対応させたいが、Assets::Mesh はその情報を
            // 持っていない(KurenaiPacker が落としている)。**片面を既定にする**と、
            // 裏から見た面が光らない。いまは全部片面で焼き、2 系の検証で必要になったら
            // フォーマットへ足す
            uint32_t flags = 0u;

            for (uint32_t t = begin; t < end; ++t)
            {
                const EmissiveTriangle& src = mesh.EmissiveTriangles[t];
                const DirectX::XMVECTOR localP0 =
                    DirectX::XMVectorSet(src.P0[0], src.P0[1], src.P0[2], 1.0f);
                const DirectX::XMVECTOR localE1 =
                    DirectX::XMVectorSet(src.E1[0], src.E1[1], src.E1[2], 0.0f);
                const DirectX::XMVECTOR localE2 =
                    DirectX::XMVectorSet(src.E2[0], src.E2[1], src.E2[2], 0.0f);

                DirectX::XMFLOAT3 p0;
                DirectX::XMFLOAT3 e1;
                DirectX::XMFLOAT3 e2;
                DirectX::XMStoreFloat3(&p0, DirectX::XMVector3TransformCoord(localP0, world));
                // 【辺はTransformNormalで移す】平行移動を掛けてはいけない。
                // 逆転置ではなくWorldでよい ―― これは法線ではなく2点の差だから
                DirectX::XMStoreFloat3(&e1, DirectX::XMVector3TransformNormal(localE1, world));
                DirectX::XMStoreFloat3(&e2, DirectX::XMVector3TransformNormal(localE2, world));

                const DirectX::XMVECTOR cross = DirectX::XMVector3Cross(
                    DirectX::XMLoadFloat3(&e1), DirectX::XMLoadFloat3(&e2));
                const float area = 0.5f * DirectX::XMVectorGetX(DirectX::XMVector3Length(cross));
                if (!(area > 0.0f))
                {
                    // 縮退した三角形。光束0なので送っても意味が無く、
                    // 面積比のエイリアステーブルでは確率0の要素になるだけ
                    continue;
                }

                // 影響半径は**かたまり全体**の強さから解く。三角形ごとの光束から解いてはいけない。
                //
                // 【なぜ三角形ごとではいけないか】三角形ごとにすると、同じ発光体でも
                // 分割数を増やすほど1枚あたりの光束が減り、**届く距離がテッセレーションの
                // 細かさで変わる**。実測: 2m角のパネル(A=4)を2枚に割ると三角形ごとでは
                // R=22.4m だが、段階1のプロキシは 56.6m 届く。同じ発光体が段階1と段階2で
                // 別の距離まで照らすことになり、遠方で一致するはずの突き合わせが壊れる。
                // しかも減る側なので**エネルギーを黙って捨てる**。
                //
                // 【段階1と同じ式にする】あちらは Range = sqrt(peak / τ) で、
                // peak は光度 max(RadianceBase)*A(片面ランバートの軸上光度 I(0)=L*A)。
                // 同じ量から解けば、両者の定義域が構成上そろう。
                // **等方近似 Φ/(4πd²) は使わない** ―― 片面発光の軸上照度は等方平均の4倍で、
                // 距離にすると2倍の食い違いになる
                // 片面ランバート発光体の光束(輝度換算) Φ = π L A。集計の報告にだけ使う
                // (影響半径はここからは解かない。上の理由を参照)
                const float flux = 3.14159265358979f * luminance * area;

                float radius = std::sqrt(clusterPeakIntensity / cutoffIrradiance);
                const float minRadius = kMinInfluenceScale * std::sqrt(area);
                if (radius < minRadius)
                {
                    radius = minRadius;
                }
                if (radius > kMaxInfluenceRadius)
                {
                    radius = kMaxInfluenceRadius;
                    ++m_RadiusClampedCount;
                }

                GPUEmissiveTriangle dst;
                dst.P0AndRadius[0] = p0.x;
                dst.P0AndRadius[1] = p0.y;
                dst.P0AndRadius[2] = p0.z;
                dst.P0AndRadius[3] = radius;
                dst.E1AndArea[0] = e1.x;
                dst.E1AndArea[1] = e1.y;
                dst.E1AndArea[2] = e1.z;
                dst.E1AndArea[3] = area;
                dst.E2AndFlags[0] = e2.x;
                dst.E2AndFlags[1] = e2.y;
                dst.E2AndFlags[2] = e2.z;
                std::memcpy(&dst.E2AndFlags[3], &flags, sizeof(flags));
                dst.RadianceAndFlux[0] = proxy.RadianceBase[0];
                dst.RadianceAndFlux[1] = proxy.RadianceBase[1];
                dst.RadianceAndFlux[2] = proxy.RadianceBase[2];
                dst.RadianceAndFlux[3] = luminance * area;

                m_TotalArea += area;
                m_TotalFlux += static_cast<double>(flux);
                triangles.push_back(dst);
            }
            ++m_ClusterCount;
        }

        if (triangles.empty())
        {
            Core::Logger::Warning(
                "MeshLightScene",
                "エミッシブ光源は " + std::to_string(scene.EmissiveProxies.size()) +
                    "個あるのに、三角形が1枚も取れませんでした。かたまりと三角形の対応を疑うこと");
            Reset();
            return false;
        }

        RHI::BufferDesc desc;
        desc.Usage = RHI::BufferUsage::StructuredImmutable;
        desc.SizeInBytes = static_cast<uint32_t>(triangles.size() * sizeof(GPUEmissiveTriangle));
        desc.StrideInBytes = static_cast<uint32_t>(sizeof(GPUEmissiveTriangle));
        desc.InitialData = triangles.data();
        m_TriangleBuffer = device.CreateBuffer(desc);
        if (!m_TriangleBuffer)
        {
            Core::Logger::Error("MeshLightScene", "メッシュライトの三角形バッファの作成に失敗しました");
            Reset();
            return false;
        }
        m_TriangleCount = static_cast<uint32_t>(triangles.size());
        m_TriangleBufferBytes = desc.SizeInBytes;

        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - startTime)
                                   .count();
        Core::Logger::Info(
            "MeshLightScene",
            "メッシュライト: 三角形 " + std::to_string(m_TriangleCount) + "枚 / かたまり " +
                std::to_string(m_ClusterCount) + "個 / 総面積 " + std::to_string(m_TotalArea) +
                " m^2 / 総光束(輝度換算) " + std::to_string(m_TotalFlux) + " / " +
                std::to_string(m_TriangleBufferBytes / 1024) + "KB / " + std::to_string(elapsedMs) + "ms");
        if (m_RadiusClampedCount > 0)
        {
            Core::Logger::Warning(
                "MeshLightScene",
                "影響半径が上限(" + std::to_string(kMaxInfluenceRadius) + "m)で切られた三角形が " +
                    std::to_string(m_RadiusClampedCount) + "枚あります。切ったぶんの光は届かなくなります");
        }
        return true;
    }
}
