#pragma once

#include "GeometryDrawHost.h"

#include <string>

#include "Core/Logger.h"

// ジオメトリを描く各パスが共有する「列挙 → カリング → 段の選択 → メッシュのループ」。
//
// 【なぜ1本へ寄せるか】以前は同型のループがシャドウ / 反射プローブ / DDGI / 深度プリパス /
// G-Buffer / 半透明 / 平面反射 / ソフトウェアラスタライザの8箇所へ複製されており、
// 深度プリパスとG-Bufferは非コメント行で86行が一致したうえ、互いのコメントに
// 「まったく同じ組・同じDitherFadeで描くこと」という**手で守る同期義務**が書かれていた。
// 組が食い違うと「深度は書かれているのに色が書かれない」穴が開くが、それは
// 絵を見ても気づけない。列挙をこの1本に寄せると、両者が同じ組を描くことは
// 「同じ関数を呼んでいる」ことで保証される。
//
// 【パスごとに違うのは中身だけ】PSOの選び方・定数バッファ・張るテクスチャ・
// ドローの発行はパスごとに違うので、コールバックとして呼び出し側に残す。

namespace Kurenai
{
    namespace Rendering
    {
        // ビュー射影行列から取り出した視錐台の6平面(左/右/下/上/手前/奥)。
        // 各要素は平面の方程式 dot(n, p) + d の (n.xyz, d)
        struct FrustumPlanes
        {
            DirectX::XMFLOAT4 Planes[6];
        };

        // ビュー射影行列から視錐台の6平面を取り出す(Gribb-Hartmann)。
        //
        // 【必ず「列」から組み立てる】クリップ座標は c = v * M(行ベクトル×行列)なので、
        // c.x は v と M の列0 の内積、c.w は列3 との内積になる。したがって
        // 「c.x + c.w >= 0」という左平面の条件は、列0 + 列3 という平面になる。
        // XMFLOAT4X4 は行優先なので、列0 は (_11, _21, _31, _41) である。
        //
        // 【行と取り違えると、真下を向いたときに全部カリングされる】
        // 実際に一度間違えた。転置した行列の平面になるため、正面付近では
        // それらしい結果が出てカメラを振れば間引き数も動く ―― 対照実験を通ってしまう。
        // ほぼ真下(Pitch -85)を向けて「真下のモデルが間引かれないこと」を見て初めて
        // 100%間引かれていることが分かった。
        //
        // HLSL側(GBufferMeshlet.hlsl の IsSphereInFrustum)も同じ平面を作っている。
        // 向こうが受け取る ViewProj は C++ から転置して渡したもので、HLSLのメモリ
        // レイアウト(列優先)と合わさって論理的には同じ行列になるため、
        // 「_m00,_m10,_m20,_m30 で列0を取る」という同じ形になっている。
        //
        // 【Reverse-Zでもこのままでよい】近平面と遠平面の意味は入れ替わるが、
        // 0 <= z <= w という条件自体は変わらないため式は同じ(HLSL側と同じ理由)。
        //
        // 【正規化しない】球との比較では半径と尺度を合わせる必要があるため向こうは正規化するが、
        // ここが判定するのはAABBで、見るのは符号だけなので不要
        FrustumPlanes ExtractFrustumPlanes(DirectX::FXMMATRIX viewProj);

        // ワールド空間の軸並行バウンディングボックスが視錐台と交わるか。
        // 「完全に外」と確定できたときだけfalseを返す保守的な判定(偽陽性は出るが偽陰性は出ない)。
        //
        // 各平面について、平面の法線方向へ最も進んだ頂点(p-vertex)だけを見る。
        // それが平面の裏側にあるなら、AABBの8頂点すべてが裏側にあることになる
        bool IsAABBVisible(
            const FrustumPlanes& frustum, const float (&boundsMin)[3], const float (&boundsMax)[3]);

        // メッシュ単位のフラスタムカリング判定。モデル単位の判定を通ったあとのメッシュの
        // ループから呼ぶ。
        //
        // 【統計はモデル単位と別のカウンタへ入れる】分母も意味も違うため
        // (KurenaiEngine3D.h の m_MeshCullTested のコメント参照)。呼び出し側が
        // どのカウンタを渡すかを見て取れるよう、メンバではなく引数で受ける。
        //
        // 【メッシュ番号はポインタ差で引く】メッシュのループは添字を持たない。
        // Meshesはvectorで連続しているため、先頭との差がそのまま添字になる
        bool IsMeshVisibleWithStats(
            bool enabled, const FrustumPlanes& frustum, const Assets::ModelInstance& instance,
            const Assets::Model& model, const Assets::Mesh& mesh, uint32_t& tested, uint32_t& culled);

        // 共通ループの本体。テンプレートにしてあるのは、メッシュごとに呼ぶコールバックを
        // 間接呼び出しにしないため(1フレームに数千回通る)
        template <typename ModelFn, typename MeshFn>
        void ForEachGeometryDraw(
            const GeometryDrawHost& host, const GeometryDrawLoopDesc& desc, ModelFn&& onModel, MeshFn&& onMesh)
        {
            // 【入れ子の列挙を禁じる】host.DrawList.Scratchは1本しかなく、内側の列挙が
            // 外側の列挙対象を丸ごと書き換えてしまう。段階5から人手のコメントで守ってきた
            // 義務だが、ForEachGeometryDrawがpublicになって呼べる場所が広がったので検査にする。
            //
            // 【打ち切らずに記録だけ残す】ここでreturnすると、入れ子を書いた瞬間に
            // 「描かれないメッシュ」が出る。従来どおり最後まで回して絵は変えない。
            // 毎フレーム何千回も通るので記録は絞る。**ここはテンプレートなのでstaticは
            // インスタンス化ごとに別物**で、記録は呼び出し箇所ごとに1回になる
            // (非テンプレートのIsMeshVisibleWithStatsはプログラム全体で1回。そこは違う)
            if (host.DrawList.ScratchInUse)
            {
                static bool loggedNestedDrawLoop = false;
                if (!loggedNestedDrawLoop)
                {
                    loggedNestedDrawLoop = true;
                    Core::Logger::Error(
                        "KurenaiEngine3D",
                        "ForEachGeometryDrawを入れ子で呼んでいます。host.DrawList.Scratchは1本しか無く、"
                        "内側の列挙が外側の列挙対象を書き換えます");
                }
            }
            // onMeshが偽を返す打ち切りでも必ず戻すためスコープガードにする
            struct ScratchGuard
            {
                bool& InUse;
                bool Previous;
                ~ScratchGuard() { InUse = Previous; }
            } scratchGuard{ host.DrawList.ScratchInUse, host.DrawList.ScratchInUse };
            host.DrawList.ScratchInUse = true;

            const bool coarsest = desc.LODMode == GeometryLODMode::Coarsest;

            // 列挙元をどちらでも InstanceDrawUnit へ揃える。
            // 【バッチを使わないパスも同じ形で回す】DDGI・半透明・ソフトウェアラスタライザは
            // インスタンシングのバッチを使わないが、InstanceCount==1 の単体として
            // 詰め直せば以降の分岐が1本で済む(unit.IsBatch() が常に偽になるだけ)
            if (desc.UseDrawUnits)
            {
                host.DrawList.GetInstanceDrawUnits(host.Scene, coarsest, host.DrawList.Scratch);
            }
            else
            {
                host.DrawList.BuildSingleInstanceDrawUnits(host.Scene, host.DrawList.Scratch);
            }

            for (const InstanceDrawUnit& unit : host.DrawList.Scratch)
            {
                const Assets::ModelInstance& instance = *unit.Instance;

                // 【錐台が無いパスは統計にも入れない】DDGIのラスタ経路はカリングを行わない
                // (プローブの位置ごとに結果が変わり、定数バッファの予算計算と食い違うため)。
                // ここで数えると「判定したが1つも間引けなかった」ように見えてしまう
                if (desc.Frustum)
                {
                    ++host.Counters.FrustumCullTested;
                    if (!IsAABBVisible(*desc.Frustum, unit.WorldBoundsMin, unit.WorldBoundsMax))
                    {
                        ++host.Counters.FrustumCullCulled;
                        continue;
                    }
                }

                // 描く段を決める。バッチはどの段を描くかを既に決めてある
                // (全員が同じ段であることがバッチの条件そのもの。BuildInstanceBatches)
                Scene::LODDraw lodDraws[2];
                uint32_t lodDrawCount = 1;
                if (unit.Model)
                {
                    lodDraws[0] = { unit.Model, 1.0f };
                }
                else if (coarsest)
                {
                    lodDraws[0] = { host.LOD.GetCoarsestLOD(instance), 1.0f };
                }
                else if (desc.LODMode == GeometryLODMode::Current)
                {
                    lodDraws[0] = { host.LOD.GetCurrentLOD(unit.InstanceIndex), 1.0f };
                }
                else
                {
                    lodDrawCount = host.LOD.GetLODDraws(unit.InstanceIndex, lodDraws);
                }

                for (uint32_t lodDrawIndex = 0; lodDrawIndex < lodDrawCount; ++lodDrawIndex)
                {
                    // ストリーミング中でまだ読み込まれていない段は描かない
                    const Assets::Model* const lodModelPtr = lodDraws[lodDrawIndex].Model;
                    if (!lodModelPtr)
                    {
                        continue;
                    }
                    const Assets::Model& lodModel = *lodModelPtr;
                    const float lodDitherFade = lodDraws[lodDrawIndex].DitherFade;

                    // モデル単位で描き切れるパス(メッシュレット経路)は、ここで真を返して
                    // メッシュのループへ入らない
                    if (onModel(unit, lodModel, lodDitherFade))
                    {
                        continue;
                    }

                    for (const Assets::Mesh& mesh : lodModel.Meshes)
                    {
                        // 不透明のパスはBLEND(mesh.IsTransparent)を、半透明のパスはそれ以外を落とす。
                        // G-Bufferのアルファは常に1.0で半透明合成ができないため、BLENDだけは
                        // 専用のフォワードパスへ回る。
                        if (mesh.IsTransparent != (desc.MeshFilter == GeometryMeshFilter::Transparent))
                        {
                            continue;
                        }

                        // 【バッチでは行わない】メッシュ単位のワールドAABBは
                        // 「インスタンス×メッシュ」の値で、まとめた相手のぶんが無い。
                        // 判定を代表インスタンスだけで行うと、他の個体の見えているメッシュまで
                        // 落ちて物が消える
                        if (desc.Frustum && !unit.IsBatch()
                            && !IsMeshVisibleWithStats(
                                desc.MeshCulling && host.MeshCullingEnabled, *desc.Frustum,
                                instance, lodModel, mesh, host.Counters.MeshCullTested, host.Counters.MeshCullCulled))
                        {
                            continue;
                        }

                        // 偽を返すと列挙そのものを打ち切る(ソフトウェアラスタライザの
                        // メッシュ表があふれたときだけ使う)
                        if (!onMesh(unit, lodModel, mesh, lodDitherFade))
                        {
                            return;
                        }
                    }
                }
            }
        }
    }
}
