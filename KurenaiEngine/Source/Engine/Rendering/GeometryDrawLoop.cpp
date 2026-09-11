#include "Rendering/GeometryDrawLoop.h"

#include <string>

#include "Core/Logger.h"

namespace Kurenai
{
    namespace Rendering
    {
        FrustumPlanes ExtractFrustumPlanes(DirectX::FXMMATRIX viewProj)
        {
            using namespace DirectX;

            XMFLOAT4X4 m;
            XMStoreFloat4x4(&m, viewProj);

            const XMFLOAT4 col0(m._11, m._21, m._31, m._41);
            const XMFLOAT4 col1(m._12, m._22, m._32, m._42);
            const XMFLOAT4 col2(m._13, m._23, m._33, m._43);
            const XMFLOAT4 col3(m._14, m._24, m._34, m._44);

            const auto add = [](const XMFLOAT4& a, const XMFLOAT4& b) {
                return XMFLOAT4(a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w);
            };
            const auto sub = [](const XMFLOAT4& a, const XMFLOAT4& b) {
                return XMFLOAT4(a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w);
            };

            FrustumPlanes frustum;
            frustum.Planes[0] = add(col3, col0); // 左   (x >= -w)
            frustum.Planes[1] = sub(col3, col0); // 右   (x <=  w)
            frustum.Planes[2] = add(col3, col1); // 下   (y >= -w)
            frustum.Planes[3] = sub(col3, col1); // 上   (y <=  w)
            frustum.Planes[4] = col2;            // 手前 (z >=  0)
            frustum.Planes[5] = sub(col3, col2); // 奥   (z <=  w)
            return frustum;
        }

        bool IsAABBVisible(
            const FrustumPlanes& frustum, const float (&boundsMin)[3], const float (&boundsMax)[3])
        {
            for (const DirectX::XMFLOAT4& plane : frustum.Planes)
            {
                const float px = (plane.x >= 0.0f) ? boundsMax[0] : boundsMin[0];
                const float py = (plane.y >= 0.0f) ? boundsMax[1] : boundsMin[1];
                const float pz = (plane.z >= 0.0f) ? boundsMax[2] : boundsMin[2];

                if (plane.x * px + plane.y * py + plane.z * pz + plane.w < 0.0f)
                {
                    return false;
                }
            }
            return true;
        }

        bool IsMeshVisibleWithStats(
            bool enabled, const FrustumPlanes& frustum, const Assets::ModelInstance& instance,
            const Assets::Model& model, const Assets::Mesh& mesh, uint32_t& tested, uint32_t& culled)
        {
            if (!enabled)
            {
                // 対照実験用のOFF。判定を1回も呼ばないので統計は「判定なし」になり、
                // 「実行したが間引き0」と区別できる(EngineDefaults.h の MeshCullingEnabled 参照)
                return true;
            }

            // 【AABBは基準の段のぶんしか無い】MeshWorldBoundsListはSceneLoaderが
            // instance.Model(=最も詳細な段)のメッシュに対して1回だけ作る。
            // モデルLODで別の段を描いているあいだと、ストリーミングで後から読み込まれた
            // モデルには対応する要素が無く、ポインタ差で引いた添字も別のvectorのものになる。
            // 判定せず間引かない側(保守側)へ倒す ―― 早さより、見えるものを消さないことを採る。
            // 【++testedより前に返す】分母を「実際に判定したメッシュ」に揃えないと間引き率が薄まる
            if (instance.Model.get() != &model)
            {
                return true;
            }

            ++tested;

            const size_t meshIndex = static_cast<size_t>(&mesh - model.Meshes.data());
            if (meshIndex >= instance.MeshWorldBoundsList.size())
            {
                // SceneLoaderが必ずMeshesと同じ要素数で作るのでここへは来ない。
                // 来た場合は間引かない側(保守側)へ倒す ―― 見えるものを消すより、
                // 間引けないほうが被害が小さい。毎フレーム何千回も呼ばれるので記録は1回だけ
                static bool logged = false;
                if (!logged)
                {
                    logged = true;
                    Core::Logger::Error(
                        "KurenaiEngine3D",
                        "メッシュ単位のワールドAABBが足りません(メッシュ数 " +
                            std::to_string(model.Meshes.size()) + " / AABB " +
                            std::to_string(instance.MeshWorldBoundsList.size()) +
                            ")。メッシュ単位のフラスタムカリングを行いません");
                }
                return true;
            }

            const Assets::MeshWorldBounds& bounds = instance.MeshWorldBoundsList[meshIndex];
            if (!IsAABBVisible(frustum, bounds.Min, bounds.Max))
            {
                ++culled;
                return false;
            }
            return true;
        }
    }

}
