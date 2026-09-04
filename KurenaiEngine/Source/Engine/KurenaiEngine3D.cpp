#include "KurenaiEngine3D.h"

#include <imgui.h>

#include <objbase.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <limits>
#include <random>

#include "Assets/SceneLoader.h"
#include "Core/Logger.h"
#include "Core/RenderGraph.h"
#include "Core/StringUtil.h"
#include "UI/UIManager.h"
#include "UI/UITheme.h"

namespace Kurenai
{
    namespace
    {
        using Core::GetModuleDirectory;
        using Core::WideToUtf8;

        // シーン読み込みの進捗をログへ落とす最短間隔[秒]。
        // 1モデルごとに出すと767モデルのシーンで767行になるため間引く。
        // 最初(0/N)と最後(N/N)だけは間隔に関わらず必ず出す
        constexpr float kSceneLoadProgressLogIntervalSeconds = 1.0f;

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

        // ワールド空間の軸並行バウンディングボックスが視錐台と交わるか。
        // 「完全に外」と確定できたときだけfalseを返す保守的な判定(偽陽性は出るが偽陰性は出ない)。
        //
        // 各平面について、平面の法線方向へ最も進んだ頂点(p-vertex)だけを見る。
        // それが平面の裏側にあるなら、AABBの8頂点すべてが裏側にあることになる
        bool IsAABBVisible(const FrustumPlanes& frustum, const float (&boundsMin)[3], const float (&boundsMax)[3])
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

        // メッシュ単位のフラスタムカリング判定。IsAABBVisibleを呼ぶ7つの描画パスすべてが、
        // モデル単位の判定を通ったあとのメッシュのループから同じ形で呼ぶ。
        //
        // 【統計はモデル単位と別のカウンタへ入れる】分母も意味も違うため
        // (KurenaiEngine3D.h の m_MeshCullTested のコメント参照)。呼び出し側が
        // どのカウンタを渡すかを見て取れるよう、メンバではなく引数で受ける。
        //
        // 【メッシュ番号はポインタ差で引く】各パスのループは
        // for (const auto& mesh : instance.Model.Meshes) の形で添字を持たない。
        // Meshesはvectorで連続しているため、先頭との差がそのまま添字になる
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

        // 濁り(タービディティ)からMie(エアロゾル)密度の倍率を求める。
        //
        // 【Preethamの定義をそのまま持ち込んではいけない】Preethamのタービディティは
        // 「エアロゾルを含む全光学的厚さ / 分子だけの光学的厚さ」と定義されており、
        // その定義で現在の既定値2.5を換算すると τ_Mie = 1.5 × τ_Rayleigh となる。
        // Hillaireの標準大気の垂直Mie光学的厚さは 0.003996 × 1.2 = 0.0048、
        // Rayleigh(550nm)は 0.013558 × 8 = 0.1085 なので、比は0.044(タービディティ換算で1.04)。
        // つまり定義どおり換算するとエアロゾルが約34倍になり、空が白く霞んでHillaireを
        // 使う意味そのものが消える。
        //
        // そこでここでのタービディティは**Preethamの定義とは別物**として扱い、
        // 「既定値2.5をHillaireの標準大気とする相対的な濁り」と定義し直す。
        // スライダーを上げれば霞み、下げれば澄むという操作の意味は保たれる
        float ComputeAtmosphereMieDensityScale(float turbidity)
        {
            // この値でMie密度の倍率がちょうど1.0(=Hillaireの標準大気)になる
            constexpr float kReferenceTurbidity = 2.5f;
            return std::max(turbidity, 0.0f) / kReferenceTurbidity;
        }

        // TAAのジッターに使う低食い違い量列(Halton列)。基数baseのradical inverse、
        // すなわちindexを基数base表記にして小数点の左右を反転した値を返す([0,1)に収まる)。
        // 乱数と違い、少ない点数でも区間内へ均等に散らばるのが要点で、8フレームぶん取れば
        // ピクセル内に8点が偏りなく配置される
        float RadicalInverse(uint32_t index, uint32_t base)
        {
            float result = 0.0f;
            float fraction = 1.0f / static_cast<float>(base);
            while (index > 0)
            {
                result += static_cast<float>(index % base) * fraction;
                index /= base;
                fraction /= static_cast<float>(base);
            }
            return result;
        }

        // TAAのジッター周期(フレーム数)。長いほど多くのサンプル位置を踏めるが、
        // その分だけ収束に時間がかかり、カメラが動いている間の見た目が不安定になる。
        // 8はUnreal Engine等でも使われる実用的な妥協点
        constexpr uint32_t kTAAJitterSampleCount = 8;

        // モデル描画(G-Bufferパス)の頂点入力レイアウト。PSOの作り直し
        // (CreatePrecisionDependentPipelineStates)からも使うため関数にしてある
        std::vector<RHI::InputElementDesc> GetModelInputLayout()
        {
            return
            {
                { "POSITION", 0, RHI::Format::R32G32B32_Float, 0 },
                { "NORMAL", 0, RHI::Format::R32G32B32_Float, 12 },
                { "TEXCOORD", 0, RHI::Format::R32G32_Float, 24 },
                { "TANGENT", 0, RHI::Format::R32G32B32A32_Float, 32 },
                // ライトマップUV(遮蔽マップ専用)。Assets::Vertex::UV1
                { "TEXCOORD", 1, RHI::Format::R32G32_Float, 48 },
            };
        }

        // FrameConstantsのDDGILOD配列の要素数。
        //
        // 【クラスのkDDGIMaxLODCountと同じ値でなければならない】この構造体は無名名前空間に
        // あってKurenaiEngine3Dのprivate定数を見られないので、独立に定義している。
        // 食い違いは下のstatic_assertが止める(cbufferのレイアウトが静かにずれるのを防ぐ)
        constexpr uint32_t kFrameConstantsDDGILODCount = 4;

        struct alignas(16) FrameConstants
        {
            DirectX::XMFLOAT4X4 ViewProj;
            DirectX::XMFLOAT4X4 InvViewProj;
            // カスケードシャドウマップ(CSM)用、カスケードごとのライト視点ビュー・プロジェクション行列。
            // (このフィールドより後ろにCameraPosition等が続くため、この配列サイズを変える場合は
            // 全シェーダのFrameConstants宣言を合わせて更新する必要がある)
            DirectX::XMFLOAT4X4 CascadeViewProj[KurenaiEngine3D::kCascadeCount];
            DirectX::XMFLOAT4 CameraPosition;
            DirectX::XMFLOAT4 LightDirection;
            DirectX::XMFLOAT4 LightColor;
            // SSAOパスがView空間でのサンプリングに使う(末尾に追加し、既存シェーダのオフセットは変えない)
            DirectX::XMFLOAT4X4 View;
            DirectX::XMFLOAT4X4 Proj;
            // 昼夜サイクル用(末尾に追加し、既存シェーダのオフセットは変えない)。rgb=環境光の色
            // (m_AmbientScale乗算済み、Render()側のconstants.AmbientColor代入部を参照)、
            // a=昼度(0=夜,1=昼。m_AmbientScaleは掛けない)
            DirectX::XMFLOAT4 AmbientColor;
            // M2: カスケード選択・PCSS用(末尾に追加)。xyzw = 各カスケードのView空間far距離
            DirectX::XMFLOAT4 CascadeSplits;
            // x: PCSSのライトサイズ(m_ShadowLightSize)。y: IBLプリフィルタ済み鏡面マップの
            // 最大ミップレベル(kIBLPrefilterMipLevels-1、DeferredLighting.hlslがラフネス→ミップの
            // 変換に使う)。z: IBL強度倍率(m_IBLEnabled=falseの場合は0.0fを渡し、シェーダ側で
            // EvaluateIBLの代わりに定数色アンビエント(AmbientColor.rgb)へフォールバックする)。
            // w: スペキュラのマルチスキャッタリング・エネルギー補正の方式
            // (m_SpecularCompensationMode。0=Off / 1=Linear / 2=Series / 3=Kulla-Conty。
            // 共有ヘッダーSpecularEnergy.hlsliのKURENAI_SPEC_COMP_*と一致させること。14.9節)
            DirectX::XMFLOAT4 ShadowParams;
            // 半透明パス(Transparent.hlsl)専用。x=t8のライトリストの有効数。DirectLighting.hlslは
            // 専用のLightingConstants(b1)で受け取るためこのフィールドを使わない(末尾に追加のため
            // 既存シェーダのオフセットは変わらない)
            DirectX::XMFLOAT4 ActiveLightCount;
            // 拡散IBLの取得元切り替え(末尾に追加のため既存シェーダのオフセットは変わらない)。
            // x: 0(既定)=プリフィルタ済み鏡面の最終ミップ(roughness=1)、1=従来の専用
            // イラディアンスマップ(t8。検証用に残している経路)。CSPrefilterはV=R=Nを仮定して
            // いるため、roughness=1(α=1)ではGGXインポータンスサンプリングの実効カーネルが
            // コサイン畳み込みへ厳密に退化し、格納値もCSIrradianceと同じE(N)/πになる(14.10節)。
            // 反射プローブの拡散イラディアンスにもまったく同じ規則を適用する(19.7節)。
            // y: 環境光の拡散倍率(m_AmbientDiffuseScale)、z: 同じく鏡面倍率
            // (m_AmbientSpecularScale)。どちらもIBLの有効/無効に関わらず効く。w: 未使用
            DirectX::XMFLOAT4 IBLParams;
            // 反射プローブ用(末尾に追加)。x=有効プローブ数(0ならプローブを使わずグローバルIBLのみ)、
            // y=影響範囲のデバッグ表示フラグ、z=視差補正の有効フラグ、w=プローブ間ブレンドの有効フラグ。
            // DeferredLighting.hlslとSSR.hlslが読む
            DirectX::XMFLOAT4 ProbeParams;
            // 反射プローブの距離キューブ用(末尾に追加、19.12節)。x=視差補正に距離キューブを使うフラグ、
            // y=距離キューブによる遮蔽判定(光漏れ抑制)の有効フラグ、z=距離キューブの1面の解像度
            // (テクセル。ReflectionProbe.hlsliのProbeDistanceBiasが1テクセル幅の見積もりに使う。
            // ハードコードせずここから渡すのは、kProbeCaptureSizeを変えたときに黙ってずれないため)、
            // w=焼いた時点の実効プリ露出から現在の実効プリ露出への換算倍率(19.14節。
            // m_ProbeBakedExposureEV100のコメントに理由がある)
            DirectX::XMFLOAT4 ProbeParams2;
            // TAA用(末尾に追加のため既存シェーダのオフセットは変わらない)。前フレームの
            // ビュー射影行列(TAAのジッターを含んだままのもの)。GBuffer.hlslが頂点をこの行列でも
            // 変換し、今フレームの投影位置との差からモーションベクター(速度)を求める。
            // 初回フレームとTAAの履歴リセット時は今フレームのViewProjと同じ値を入れる
            // (未定義値が速度バッファへ焼き込まれるのを防ぐため)
            DirectX::XMFLOAT4X4 PrevViewProj;
            // TAAのサブピクセルジッター量(末尾に追加)。xy=今フレーム、zw=前フレーム。
            // 単位はUV(=ピクセルオフセット / レンダー解像度)。
            //
            // 【なぜ速度からジッターを引くのか】ジッターは投影行列に入れてあるので、ViewProjと
            // PrevViewProjで投影した位置の差にはジッターの差も混ざる。しかしジッターは
            // 「同じ面のどこをサンプルしたか」の違いであって「ものが動いた量」ではない。
            // 引いておかないとTAAが履歴を引く位置が毎フレーム±0.5px揺れ、いつまでも収束しない
            DirectX::XMFLOAT4 TAAParams;
            // DDGI用(さらに末尾に追加、22章)。サンプリング側(DeferredLighting.hlsl)が必要とする値だけを
            // 置く。ヒステリシスや最大レイ距離は焼く側にしか要らないのでDDGIUpdateConstantsが持つ。
            //   DDGIParams0: xyz=ボリュームの最小コーナー(ワールド)、w=有効フラグ(0なら従来のIBLのまま)
            //   DDGIParams1: xyz=プローブ間隔、w=法線バイアス(遮蔽判定の照会点を面から浮かせる量)
            //   DDGIParams2: xyz=各軸のプローブ数、w=視線バイアス
            //   DDGIParams3: x=イラディアンスの1辺のテクセル数(境界を含まない)、
            //                y=距離モーメントの1辺のテクセル数(同)、z=拡散間接光の強度倍率、w=未使用
            // テクセル数をハードコードせずここから渡すのは、ProbeParams2.zと同じ理由
            // (C++側の定数を変えたときにシェーダーとの対応が黙ってずれないため)
            DirectX::XMFLOAT4 DDGIParams0;
            DirectX::XMFLOAT4 DDGIParams1;
            DirectX::XMFLOAT4 DDGIParams2;
            //                y=距離モーメントの1辺のテクセル数(同)、z=拡散間接光の強度倍率、
            //                w=境界の幅(テクセル)
            DirectX::XMFLOAT4 DDGIParams3;
            // DDGIParams4: x=このフレームの実効プリ露出(m_EffectiveExposureEV100の線形倍率)、
            //             y=1/2解像度で評価するか、z=未使用、
            //             w=プローブ分類のしきい値(裏面ヒット率がこれを超えたら
            //               そのプローブを信用しない。0以下なら分類を無効にする)。
            //
            // 【アトラスは露出非依存の単位で持つ】ライトの色にはCPU側で実効プリ露出が
            // 事前乗算されており(21.5節)、その倍率は時刻に連動して最大18段(約26万倍)動く。
            // アトラスへプリ露出済みの値をそのまま溜めると、時刻が変わった瞬間に
            // 「古い露出で焼かれた数値」を新しい露出の値として読むことになる。
            // DDGIは多重バウンスで自分自身へフィードバックするため、このズレが増幅され続け、
            // 夜を挟んで昼に戻すと画面が数倍明るいまま戻らなくなる(実測で12時の平均輝度が
            // 45.6→132.9)。そこで書き込み時にこの倍率で割り、読み出し時に掛け直して、
            // アトラスの中身を露出に依存しない物理量に保つ。
            // R32で確保してある(22.6節)ので、夜の小さな値でもfp32の範囲に余裕がある
            DirectX::XMFLOAT4 DDGIParams4;
            // DDGIのクリップマップLOD(31.4.2節)。LOD k は間隔が ProbeSpacing * 2^k。
            //
            // DDGILODOrigin[k].xyz … LOD k の格子の原点(ワールド)。k=0はDDGIParams0.xyzと同じ値
            // DDGILODBase[k].xyz   … その原点に対応する格子の整数座標。
            //                        トロイダル(剰余)addressingの基準。
            //
            // 【どちらもCPUで求めて渡す】原点÷間隔をシェーダー側でも計算すると、丸めが
            // 食い違ったときにプローブのワールド位置とアトラスのセルがずれる。
            // ずれても絵は出るので気づけない。「同じ量を2か所で導出しない」ための冗長さである。
            //
            // 【なぜDDGIParams4の直後なのか】末尾へ足すと、DDGIを実際に読む6本が
            // ここから末尾までの14個のフィールドをオフセット合わせのためだけに宣言する羽目になる。
            // ここへ入れれば、DDGIブロックを既に宣言している11本が1行ずつ足すだけで済む。
            // **cbufferは宣言順でオフセットが決まるので、宣言している全シェーダーへ
            // 同じ位置に足すこと**(飛ばすと、後続フィールドを読む側が静かにずれる)
            DirectX::XMFLOAT4 DDGILODOrigin[kFrameConstantsDDGILODCount];
            DirectX::XMFLOAT4 DDGILODBase[kFrameConstantsDDGILODCount];
            // bent normalによる遮蔽用(34章)。
            // x=ディフューズAOの出所   0=従来のベイクAO(Material.b) / 1=aoN = dot(N, bRaw)
            // y=スペキュラ遮蔽の方式   0=Frostbite近似(従来)      / 1=bent normalの錐体交差
            // z=multi-bounce AO       0=無効(既定)                / 1=有効
            // w=未使用
            //
            // xとyは同じ積分の別推定量どうしの切り替えなので、0と1で見た目がほぼ変わらないことが
            // そのまま検証になる。zだけは見た目を大きく変えるため既定を無効にしてある。
            //
            // 【この位置を動かしてはいけない】ここまでのオフセットは他ブランチと共有している。
            // 新しいフィールドは下のTimeParams以降と同じく末尾へ足すこと。
            // **cbufferは宣言順レイアウトなので、この並びを変えたら
            // DeferredLighting/SSR/RTReflection/Transparent/ProbeCapture/GBufferCommon/
            // AerialPerspective/PlanarReflection/Water/Present の宣言を1フィールドずつ
            // 突き合わせて直すこと**(ずれても絵は出るが値が全部おかしくなる)
            DirectX::XMFLOAT4 OcclusionParams;
            // 水面用(さらに末尾に追加)。x=水面法線マップのスクロール
            // オフセット(0〜1、CPU側で既にfmod済み)、y=波のスケール倍率(m_WaterWaveScale)、
            // z=波の強さ(m_WaterWaveStrength、0〜1)、w=未使用。Water.hlslのPSMainが読む。
            // 末尾に足す限り、既に宣言済みのシェーダのcbufferオフセットは1バイトも動かない
            // (DDGIParams0〜4を末尾に追加したときと同じ規約)
            DirectX::XMFLOAT4 TimeParams;
            // 空の解析評価用(さらに末尾に追加)。DeferredLighting.hlslが背景画素で
            // Sky.hlsliのSkyColorを画面解像度で評価するために使う。太陽方向以外の値
            // (ティント4本・天頂輝度)は手続き空のベイクと同じタイミングでSkyIntegrate.hlslが
            // m_SkyParametersBufferへ書き、両者が同じ空を描くことを保証する。
            // SkySunDirection: xyz=太陽が「ある」向き、w=未使用。
            //   【正規化はシェーダ側で行う】sunLighting.SunPositionは解析的にはほぼ単位長だが、
            //   SkyGenerate.hlsl側の慣習(呼び出し側=SkyParameters組み立て時にnormalizeする)に
            //   合わせ、C++側では正規化せずそのまま渡す(DeferredLighting.hlslのMakeSkyParameters参照)。
            //   **LightDirectionでは代用できない**——あちらは支配ライトの向きで、月が支配的な
            //   夜には月の向きになる。Perez分布のcircumsolar項は常に太陽を基準にする
            DirectX::XMFLOAT4 SkySunDirection;
            // SkyParams: x=未使用(天頂輝度はSkyParametersBufferにある)、
            //   y=背景を解析評価するかのフラグ(1=解析、0=キューブマップをサンプル)、
            //   z=太陽照度/空照度比(SunToSkyIlluminanceRatio。sunLighting.KeyIlluminanceLux /
            //   sunLighting.SkyIlluminanceLuxから求める。Sky.hlsliのEvaluateCloudLayerが雲の
            //   明るさの基準を太陽の照度にするために使う。雲を照らしているのは空ではなく
            //   太陽であるため、天頂輝度基準では雲が原理的に空より暗くしかならなかった
            //   問題への対処)、w=未使用。
            //   yは手続き空が無効(.ksceneのDDSスカイボックス使用時)は常に0にする
            //   (DDSは任意の絵でPerezモデルとは無関係なため、解析評価してはいけない)。
            //   ティント4本(SkyZenithTint/SkyHorizonTint/SkyGroundTint/SkySunGlowTint)は
            //   m_SkyParametersBuffer(GPUSkyParameters、SkyIntegrate.hlslが書く)にあり、
            //   このFrameConstantsには持たない(DeferredLighting.hlsl/SSR.hlslのFrameConstants
            //   宣言も同じ。フィールドを増減すると後続のオフセットが全部ずれるため、
            //   末尾のCloudParams0/1・PlanarReflectionPlaneまで含めて3シェーダーと1フィールドずつ
            //   突き合わせて一致を確認すること)
            DirectX::XMFLOAT4 SkyParams;
            // 雲(さらに末尾に追加)。DeferredLighting.hlsl/SSR.hlslのFrameConstants宣言と
            // 同じ順・同じ型であること(2つのシェーダーが背景と水面反射で同じ雲を描くための前提。
            // Sky.hlsli冒頭のコメント・各シェーダーのMakeSkyParametersのコメント参照)。
            // CloudParams0: x=被覆率(0で雲なし。Sky.hlsliのSkyColorが早期脱出する)、
            //               y=雲底の高度[m](カメラのワールドY基準)、
            //               z=UVスケール[ノイズ空間の距離/m]、w=消散係数
            DirectX::XMFLOAT4 CloudParams0;
            // CloudParams1: xy=風によるノイズ空間の移動量(CPU側でSky.hlsliのkCloudNoisePeriodと
            //               同じ周期でstd::fmod済み。m_CloudScrollOffset参照)、
            //               z=Henyey-Greensteinの非対称パラメータ、w=未使用
            DirectX::XMFLOAT4 CloudParams1;
            // 巻雲(さらに末尾に追加)。DeferredLighting.hlsl/SSR.hlsl/PlanarReflection.hlslの
            // FrameConstants宣言と同じ順・同じ型であること(3シェーダーすべてを更新すること。
            // 末尾のPlanarReflectionPlaneを含めて1フィールドずつ突き合わせて一致を確認すること)。
            // CloudParams2: x=巻雲の被覆率(0で巻雲なし。Sky.hlsliのSkyColorが早期脱出する)、
            //               y=雲底の高度[m](カメラのワールドY基準)、
            //               z=UVスケール[ノイズ空間の距離/m]、w=消散係数
            DirectX::XMFLOAT4 CloudParams2;
            // CloudParams3: xy=風によるノイズ空間の移動量(CPU側でSky.hlsliのkCloudNoisePeriodと
            //               同じ周期でstd::fmod済み。m_CirrusScrollOffset参照)、
            //               z=fBmのUV(U方向)を伸ばす異方性スケール(m_CirrusAnisotropy)、w=未使用
            DirectX::XMFLOAT4 CloudParams3;
            // 平面反射(さらに末尾に追加)。xyz=水面平面の法線(現状は常に(0,1,0))、
            // w=平面の距離項。PlanarReflection.hlslのVSMainが
            // SV_ClipDistance0 = dot(worldPos, xyz) + w として使い、水面より上で正になるようにする
            // (水面より下のジオメトリを反射に映さないため)。このシェーダー以外は参照しない
            DirectX::XMFLOAT4 PlanarReflectionPlane;
            // 大気遠近(さらに末尾に追加)。AerialPerspective.hlsl/PlanarReflection.hlslが読む。
            // x=基準高度での消散係数[1/m]、y=スケールハイト[m]、z=基準高度[m](ワールドY)、
            // w=有効フラグ(0で無効。UIでオフ、またはシーンが手続き空を使っていない場合に0にする。
            // 手続き空が無効なシーンでは大気遠近のin-scatter項(SkyColorの解析評価)が意味を持たない
            // ため、SSR.hlslのwaterAnalyticSkyFlagと同じ判断をRender()側で行う)
            DirectX::XMFLOAT4 FogParams0;
            // x=不透明度の上限(1.0で完全に空の色まで行く)、yzw=未使用
            DirectX::XMFLOAT4 FogParams1;
            // 水中項(さらに末尾に追加)。xyz=水体の色(リニア)、w=未使用。Water.hlslのPSMainが
            // メッシュ自身のBaseColorFactorの代わりにこの色を出力Albedoに使う
            // (見下ろした水面がFresnel最小(約0.02)でほぼ真っ黒になる問題への対処。詳細はWater.hlsl参照)
            DirectX::XMFLOAT4 WaterBodyColor;
            // 星空(さらに末尾に追加)。Sky.hlsliのEvaluateStarfieldが読む。
            // x=強度(0で完全に無効。昼はCPU側で0にする)、y=密度(天球を割るセルの細かさ)、
            // z=またたきの強さ、w=1画素が張る角度[rad](星がこれを下回らないようにする)。
            //
            // 【読むのはDeferredLighting.hlslとSSR.hlslだけ】背景と水面の映り込みにしか
            // 星を出さないため。AerialPerspective.hlsl(フォグのin-scatter)と
            // SkyGenerate.hlsl(IBLキューブ)は自分のMakeSkyParametersで0を入れる
            DirectX::XMFLOAT4 StarsParams;
            // 雲の品質(さらに末尾に追加)。x=積雲のボリュームレイマーチの段数、yzwは予備。
            //
            // 【読むのはSkyCloud.hlslだけ】ボリューム経路を持つのがこのシェーダーだけだからである。
            // 他のシェーダー(SSR/PlanarReflection/AerialPerspective)は厚みゼロの平面経路を通り、
            // レイマーチそのものを行わないのでこの値を必要としない。
            //
            // 【オクターブ数と自己影の段数はここへ入れない】あれらはfBmの値そのものを変えるため、
            // 実行時に動かすとボリューム経路と平面経路で雲の形が食い違い、
            // 背景の雲と水面に映る雲が別物になる(Sky.hlsliのCloudRaymarchStepsのコメント参照)
            DirectX::XMFLOAT4 CloudQualityParams;
            // Hi-Zオクルージョンカリング(さらに末尾に追加、Stage 5-2)。
            // 読むのはGBufferMeshlet.hlslの増幅シェーダーだけ。
            //
            // x=有効フラグ(0で判定そのものを行わない)、y=バウンディング球の半径倍率
            // (m_OcclusionCullRadiusScale)、z=前フレームからのカメラ移動距離[m]、
            // w=Hi-Zのミップ段数(m_HiZMipLevels)。
            //
            // 【xを明示的なフラグにする理由】判定はPrevViewProjで投影するが、プローブ
            // キャプチャや平面反射のパスはPrevViewProjへViewProjを入れて潰している
            // (前フレームという意味を持たない)。そこで判定が動くと、Hi-Zの中身とは
            // 別の視点の行列で投影して見えているものを消す。行列の中身から推し量るのではなく、
            // 「メインカメラのG-Bufferパスか」をCPU側で決めてここへ渡す
            DirectX::XMFLOAT4 OcclusionCullParams;
            // 同じくHi-Zオクルージョンカリング用。xy=Hi-Zのミップ0の解像度[画素]、zw=その逆数。
            // NDC→UV→テクセル座標の変換に要る(FrameConstantsはレンダー解像度を持っていない)
            DirectX::XMFLOAT4 HiZScreenParams;
            // メッシュレットカリングの統計(さらに末尾に追加、Stage 5-2)。
            // x=有効フラグ、y=カウンタバッファのbindless番号(UAVの側)、zw=未使用。
            //
            // 【なぜbindlessで渡すのか】メッシュシェーダー用ルートシグネチャはSRVテーブルと
            // サンプラーテーブルしか持たず、UAVレンジが無い。増幅シェーダーから書き込むには
            // ResourceDescriptorHeap経由しかない(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXEDは立っている)
            DirectX::XMFLOAT4 MeshletCullStatsParams;

        };

        // DDGIのプローブ更新CS(DDGIProbeUpdate.hlsl)専用の定数バッファ。
        // 焼く側にしか要らない値(どのプローブを焼いているか・ヒステリシス・距離のクランプ上限)を持つ
        struct alignas(16) DDGIUpdateConstants
        {
            // x=いま焼いているプローブの通し番号、y=ヒステリシス、z=距離モーメントのクランプ上限、
            // w=キャプチャキューブの1面の解像度(レイの立体角の重み付けに使う)
            DirectX::XMFLOAT4 Params0;
            // x=イラディアンスの1辺のテクセル数(境界を含まない)、y=距離モーメントの1辺のテクセル数、
            // z=境界の幅、w=履歴を無視して上書きするフラグ(初回ベイク時に1。
            // ヒステリシスは「前の値」があって初めて意味を持つため、未初期化のアトラスと混ぜてはいけない)
            DirectX::XMFLOAT4 Params1;
            // xyz=アトラス上でのプローブ格子の並び(x=各軸のプローブ数)。アトラスの列数は
            // ProbeCounts.x * ProbeCounts.y、行数はProbeCounts.zになる。
            // w=このフレームの実効プリ露出。積分した放射輝度をこれで割ってから格納する
            // (理由はFrameConstants::DDGIParams4のコメント参照)
            DirectX::XMFLOAT4 Params2;
        };

        // DDGIProbeTrace.hlsl側のcbuffer DDGITraceConstants(register b1)と一致させる必要がある
        struct alignas(16) DDGITraceConstants
        {
            // xyz=いま焼いているプローブのワールド座標、w=処理対象の面(D3Dのキューブ標準順)
            DirectX::XMFLOAT4 Params0;
            // x=キャプチャキューブの1面の解像度、y=エミッシブ強度(ラスタ経路のObjectConstantsへ
            // 掛かっているのと同じ倍率)、z=太陽の影レイを撃つか(0/1。対照実験用)、w=未使用
            DirectX::XMFLOAT4 Params1;
        };

        // シャドウパスの各カスケード描画専用の定数バッファ(FrameConstantsとは別バッファ)
        struct alignas(16) CascadeConstants
        {
            DirectX::XMFLOAT4X4 ViewProj;
        };

        // IBLConvolve.hlsl(CSIrradiance/CSPrefilter)へ、処理対象の面(キューブマップは面ごとに
        // 個別ディスパッチが必要)とCSPrefilterのみが使うラフネス値を渡す専用の定数バッファ
        struct alignas(16) IBLFaceConstants
        {
            uint32_t Face = 0;
            float Roughness = 0.0f;
            // SH経路用。IBLConvolve.hlslのIBLFaceConstantsコメント参照。
            // CSIrradiance/CSPrefilterはどちらも参照しないため、設定しなくても既定の値初期化(0)で動く
            float SHWindowLambda = 0.0f;
            float SHProjectionSize = 0.0f;
        };

        // DeferredLighting.hlsl側のstruct GPUReflectionProbeと並び・ストライド(48バイト)を
        // 一致させる必要がある
        struct alignas(16) GPUReflectionProbe
        {
            DirectX::XMFLOAT4 PositionRadius; // xyz=ワールド座標(Box形状では箱の中心), w=Sphere形状の影響半径
            DirectX::XMFLOAT4 BoxExtents;     // xyz=Box形状の各軸の半径(ハーフエクステント), w=ブレンド距離
            DirectX::XMFLOAT4 ShapeParams;    // x=形状(0=Sphere,1=Box), y=sin(Yaw), z=cos(Yaw), w=未使用
        };

        // キューブマップの1面を撮るためのビュー行列(左手系)。前方向・上方向の組は
        // IBLConvolve.hlslのCubeFaceDirectionが定める面→方向の対応と一致していなければならない
        // (ずれると焼いた面が回転・反転する)。D3Dのキューブマップ標準順(+X,-X,+Y,-Y,+Z,-Z)
        DirectX::XMMATRIX ComputeCubeFaceView(const DirectX::XMFLOAT3& position, uint32_t face)
        {
            using namespace DirectX;

            static const XMFLOAT3 kForward[6] =
            {
                {  1.0f,  0.0f,  0.0f }, // +X
                { -1.0f,  0.0f,  0.0f }, // -X
                {  0.0f,  1.0f,  0.0f }, // +Y
                {  0.0f, -1.0f,  0.0f }, // -Y
                {  0.0f,  0.0f,  1.0f }, // +Z
                {  0.0f,  0.0f, -1.0f }, // -Z
            };
            static const XMFLOAT3 kUp[6] =
            {
                { 0.0f, 1.0f,  0.0f },
                { 0.0f, 1.0f,  0.0f },
                { 0.0f, 0.0f, -1.0f },
                { 0.0f, 0.0f,  1.0f },
                { 0.0f, 1.0f,  0.0f },
                { 0.0f, 1.0f,  0.0f },
            };

            return XMMatrixLookToLH(XMLoadFloat3(&position), XMLoadFloat3(&kForward[face]), XMLoadFloat3(&kUp[face]));
        }

        // プローブのキャプチャ用プロジェクション(画角90度・アスペクト1)。Core::Cameraの
        // 遠近投影と同じReverse-Z(近平面=NDC z=1.0、遠平面=NDC z=0.0)で作る必要がある
        // (深度クリア値・PipelineStateDesc::ReverseZが同じ前提で組まれているため)
        DirectX::XMMATRIX ComputeCubeFaceProjection(float nearZ, float farZ)
        {
            // 画角90度なのでtan(45度)=1、すなわちw=h=1になる
            const float a = nearZ / (nearZ - farZ);
            const float b = -a * farZ;

            return DirectX::XMMatrixSet(
                1.0f, 0.0f, 0.0f, 0.0f,
                0.0f, 1.0f, 0.0f, 0.0f,
                0.0f, 0.0f, a, 1.0f,
                0.0f, 0.0f, b, 0.0f);
        }

        // 太陽光の向き・色・環境光を時刻(0〜24時)から計算する
        struct SunLighting
        {
            // 支配ライト(太陽 or 月)の、光が進む向き(サーフェスに当たる方向)。
            // カスケードシャドウの行列もこの向きから作ること
            DirectX::XMFLOAT3 Direction;
            DirectX::XMFLOAT4 Color;
            DirectX::XMFLOAT4 Ambient; // rgb=環境光の色, a=昼度(0=夜,1=昼)
            // 支配ライトが太陽か月か(ImGuiの表示とデバッグ用)
            bool DominantIsSun;
            // 手続き空の天頂輝度を正規化する際の目標照度[lx]。薄明係数と月明かりで変調済み
            float SkyIlluminanceLux;
            // 薄明係数(仰角[-15°,+15°] = 時刻でちょうど5-7時/17-19時)
            float TwilightFactor;
            // 太陽が「ある」向き。手続き空(SkyGenerate.hlsl)がPerez分布のcircumsolar項の
            // 基準に使う。月が支配的なときも**常に太陽の位置**であることに注意
            DirectX::XMFLOAT3 SunPosition;
            // このフレームのキーとなる照度[lx]。可変プリ露出の基準になる
            float KeyIlluminanceLux;
        };

        // 直射日光(正午・快晴)の照度[lx]。Lagarde & de Rousiers 2014の照度参照テーブルに
        // 掲載される代表値
        constexpr float kSunIlluminanceLux = 100000.0f;
        // 空光(直射日光を除いた間接照度)の照度[lx]。同テーブルの曇天相当値を、直射日光に対する
        // 空光の比率(おおむね1〜2割)としても妥当な範囲であることの根拠として採用する。
        // 手続き空の天頂輝度の正規化目標にもなるためRender()からも参照する
        constexpr float kSkylightIlluminanceLux = 20000.0f;
        // 満月が地表へ与える照度[lx]。太陽(10万lx)の約40万分の1という実測値。
        // 満ち欠けは未実装(常に満月)。位置は時刻に連動せず、ImGuiで手動指定する
        constexpr float kMoonIlluminanceLux = 0.25f;
        // 満月時に夜空全体が散乱で持つ照度[lx]。地表照度0.25lxのうち空由来の寄与にあたる概算値。
        //
        // 【月と夜空の比が夜の影の見え方を決める】影の濃さは「平行光(月) : 環境光(夜空)」の比で
        // 決まる。物理値の0.25:0.05は5:1で、影は十分な濃さを持つ。この比を保ったまま
        // 表示上の明るさだけを調整したい場合は、照度ではなく自動露出の
        // m_AutoExposureNightRolloffEV(夜の露出切り詰め量)を動かすこと
        constexpr float kMoonSkyIlluminanceLux = 0.05f;
        // 星明かりだけの夜空の照度[lx]。月が地平線下にあるときの下限になる。
        // 月の位置は手動指定なので「月の出ていない夜」もスライダー一つで作れる。
        // そこで夜空の目標照度が厳密に0になると空が真っ黒になり、
        // 自動露出が持ち上げようのない画になる。星明かりは実在する量(約0.001lx)なので、
        // アート的な下駄ではなく物理値としてここに置く
        constexpr float kStarlightIlluminanceLux = 0.001f;

        // edge0とedge1の間をなめらかに0→1で補間する(edge0以下は0、edge1以上は1)
        float Smoothstep(float edge0, float edge1, float x)
        {
            const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        }

        // --- 手続き空(SkyGenerate.hlsl)の色味・照度正規化はGPU側(SkyIntegrate.hlsl)に
        //     一本化してある。**ここへCPUミラーを置いてはいけない**(Sky.hlsliの同じ式と
        //     二重実装になり、「片方を直したら必ずもう片方も直す」規約でしか整合が保てない)。
        //     GPUSkyParameters/m_SkyParametersBufferの定義とコメントは
        //     このファイル内の該当箇所(GPU用構造体の宣言、Render()のbakeSkyThisFrameブロック)を
        //     参照。式の実体はShaders/3D/Sky.hlsliのComputeSkyTintSet/PerezRelativeLuminance/
        //     SkyTintFromSetと、それを呼ぶShaders/3D/SkyIntegrate.hlslにある ---

        // Sky.hlsliのkCloudNoisePeriodと同じ値であること。CPU側(RenderThreadMainの
        // m_CloudScrollOffset更新)がこの値でstd::fmodして風のスクロール位相を巻き戻しており、
        // ずれるとCPU側で巻き戻した位置とシェーダー側の周期境界が食い違い、風が吹くたびに
        // 雲がジャンプする
        constexpr float kCloudNoisePeriod = 256.0f;

        // 被覆率から求める全天の平均透過率(判断B)。IBL用キューブマップには雲を焼き込まない
        // (Sky.hlsliの雲セクション、判断Aのコメント参照)ため、被覆率が上がってもキューブの
        // 明るさが晴天のまま据え置かれてしまう。これを補うため、キューブへ焼く天頂輝度にだけ
        // この平均透過率を掛けて全体を暗くする。
        // 【物理的な導出ではない】実際の曇天は多重散乱・雲の厚みで複雑に減光するが、ここでは
        // 「被覆率0で1.0(無変化)、被覆率1でkCloudOvercastTransmittanceまで直線的に落ちる」という
        // 単純な線形補間で済ませている。目的はIBLの明るさが被覆率に応じて定性的に下がることであり、
        // 精密な値は求めていない(実測で調整可能)
        constexpr float kCloudOvercastTransmittance = 0.35f;

        // 巻雲側の「全天が巻雲のときの透過率」。積雲のkCloudOvercastTransmittance(0.35)より
        // 1に近い値にしてある。巻雲は光学的に薄く(CirrusDensityが積雲の1桁下)、全天を覆っても
        // 積雲ほど大きくは減光しないという定性的な近似であり、精密な値は求めていない
        // (実測で調整可能)
        constexpr float kCirrusOvercastTransmittance = 0.75f;

        // 1層ぶんの「被覆率→平均透過率」の線形補間。ComputeCloudAverageTransmittanceが
        // 積雲・巻雲の両方でこの1つの式を共有する
        float ComputeCloudLayerTransmittance(bool layerEnabled, float coverage, float overcastTransmittance)
        {
            if (!layerEnabled)
            {
                return 1.0f;
            }
            const float clampedCoverage = std::clamp(coverage, 0.0f, 1.0f);
            // lerp(1.0f, overcastTransmittance, clampedCoverage)と同じ
            return 1.0f + (overcastTransmittance - 1.0f) * clampedCoverage;
        }

        // 被覆率から求める全天の平均透過率(判断B)。巻雲(2層目)も加味し、
        // T = T_cumulus(積雲の被覆率) * T_cirrus(巻雲の被覆率) という2層の積で求める。
        // 巻雲を無効化・被覆率0にした場合はT_cirrus=1.0になり、積雲だけの値になる
        float ComputeCloudAverageTransmittance(
            bool cloudEnabled, float coverage, bool cirrusEnabled, float cirrusCoverage)
        {
            const float cumulusTransmittance =
                ComputeCloudLayerTransmittance(cloudEnabled, coverage, kCloudOvercastTransmittance);
            const float cirrusTransmittance =
                ComputeCloudLayerTransmittance(cirrusEnabled, cirrusCoverage, kCirrusOvercastTransmittance);
            return cumulusTransmittance * cirrusTransmittance;
        }

        // 実在の写真露出値(EV100)から露出係数を求める。絞り値・シャッター速度・ISO感度から一意に
        // 定まる実在の量で、Lagarde & de Rousiers, "Moving Frostbite to Physically Based Rendering"
        // (SIGGRAPH 2014 course notes)やGoogle FilamentのPhysically Based Cameraドキュメントが
        // 採る標準式。カンデラ/ルクスの測光量に直接掛けることで表示レンジへ変換する
        // (放射量(W)への変換は行わない。本エンジンには放射量ベースの大気モデルが無く、
        // 変換段を増やす意味が無いため)
        float ComputeExposure(float ev100)
        {
            return 1.0f / (1.2f * std::pow(2.0f, ev100));
        }

        // 環境の照度[lx]から「そのシーンの基準EV100」を求める。
        //
        // 自動露出のヒストグラムと違い、これは**画面に何が写っているかに一切依存しない**。
        // 測光値が構図で振れる(空が画面に占める割合で2〜3.5段動く)のを抑えるための
        // 足がかりとして使う(AutoExposure.hlsl の KeyReferenceEV100 参照)。
        //
        // 導出: 反射率ρのLambertian面が照度Eを受けたときの輝度は L = E·ρ/π。
        // EV100と輝度の関係は L = 2^EV100 · K/S(反射光式露出計の標準、K=12.5・S=100)
        // すなわち EV100 = log2(8L)。ρには中庸なグレーの18%を使う。
        // 検算: E=100,000lx(直射日光) → EV100=15.5、E=0.3lx(満月の夜) → EV100=-2.9。
        // どちらも実写の露出値と一致する
        float ComputeReferenceEV100(float illuminanceLux)
        {
            constexpr float kMiddleGreyReflectance = 0.18f;
            const float luminance =
                std::max(illuminanceLux, 1e-6f) * kMiddleGreyReflectance / DirectX::XM_PI;
            return std::log2(8.0f * luminance);
        }

        // 太陽・月・空の状態を時刻から求める。
        // **露出は一切掛けない**(すべて絶対的な測光量[lx]のまま返す)。露出はこの結果から
        // 決まる実効EV100を使ってRender()側で掛ける(可変プリ露出。KurenaiEngine3D.h参照)
        SunLighting ComputeSunLighting(
            float timeOfDayHours, float sunAzimuthDegrees, float moonAzimuthDegrees, float moonElevationDegrees)
        {
            using namespace DirectX;

            // 日の出(東)側の水平方向。太陽はこの方向と天頂(真上)を通る鉛直面内で、
            // 東→天頂(正午)→西→天底(真夜中)と一日一周する半円軌道を描く。
            // 方位角(sunAzimuthDegrees)はX軸を0度、Z軸(+方向)を90度としてImGuiで調整する
            const float azimuthRadians = XMConvertToRadians(sunAzimuthDegrees);
            const XMFLOAT3 kSunriseHorizontal{ std::cos(azimuthRadians), 0.0f, std::sin(azimuthRadians) };

            // 6時=0度(日の出/東)、12時=90度(天頂)、18時=180度(日の入り/西)、24時=270度(天底/真夜中)
            const float hourAngle = (timeOfDayHours / 24.0f) * XM_2PI - XM_PIDIV2;
            const float sinHour = std::sin(hourAngle);
            const float cosHour = std::cos(hourAngle);

            // 太陽の方向(地面から見て太陽がある向き)。kSunriseHorizontalとY軸(天頂)を結ぶ円軌道上の点
            const XMFLOAT3 sunDirection{ kSunriseHorizontal.x * cosHour, sinHour, kSunriseHorizontal.z * cosHour };

            SunLighting result{};

            // === 昼夜の遷移係数を「時刻」ではなく「太陽の仰角」で決める ===
            // sinHour がそのまま太陽仰角のサインになる(軌道が単位円のため)。
            //
            // 【なぜ時刻ベースではいけないか】Smoothstep(6,7) * (1 - Smoothstep(17,18)) という
            // 時刻の窓は仰角0度〜15度にちょうど一致するため成立するが、遷移を長くしようと
            // 窓を5-7時/17-19時へ広げると
            // 5.5時(仰角-7.5度)で dayFactor≈0.156 となり、**地平線下の太陽が15,600 lx で照らす**
            // ことになる。LightDirection.y > 0 となってカスケードシャドウが地面の下から
            // 影を焼き、物体の裏側が照らされる。
            //
            // そこで遷移を2本に分ける:
            //   SunFactor      … 直接光と影。仰角[0°,15°]。地平線下では厳密に0
            //   TwilightFactor … 空の輝度と環境光。仰角[-15°,+15°] = 時刻でちょうど5-7時/17-19時
            // 「2時間かけて遷移する」という見た目の要求は TwilightFactor が満たし、
            // 直接光は物理的に成立する範囲(地平線より上)に留まる。
            // 実際の市民薄明(太陽が地平線下0〜-6度)もこの構造になっている。
            const float sunElevationSin = sinHour;
            const float kSin15Deg = std::sin(XMConvertToRadians(15.0f));
            const float sunFactor = Smoothstep(0.0f, kSin15Deg, sunElevationSin);
            const float twilightFactor = Smoothstep(-kSin15Deg, kSin15Deg, sunElevationSin);

            // === 月は時刻に連動せず、方位角と仰角で手動指定する ===
            // 実際の月は太陽とは独立した周期(朔望月)で動くため、反太陽方向に固定するのは
            // 「常に満月かつ常に真夜中に南中する」という二重の簡略化になる。
            // 位置を手動指定にすることで、任意の月齢・任意の時刻の見え方を作れるようにする。
            // 方位角の規約は太陽と同じ(X軸が0度、Z軸(+方向)が90度)
            const float moonAzimuthRadians = XMConvertToRadians(moonAzimuthDegrees);
            const float moonElevationRadians = XMConvertToRadians(moonElevationDegrees);
            const float moonCosElevation = std::cos(moonElevationRadians);
            const XMFLOAT3 moonDirection{
                moonCosElevation * std::cos(moonAzimuthRadians),
                std::sin(moonElevationRadians),
                moonCosElevation * std::sin(moonAzimuthRadians),
            };

            // 月が地平線より上にあるかどうか(太陽と同じく仰角[0°,15°]で立ち上げる)
            const float moonElevationFactor = Smoothstep(0.0f, kSin15Deg, moonDirection.y);
            // 【なぜ太陽の高度でも月を絞るのか】平行光源の枠は1つしかないので、
            // 太陽と月は「支配的な方」を選んで切り替える。月を反太陽方向に固定するなら、
            // 切替点(太陽の仰角0度)で月の係数もちょうど0になり、向きが反転しても
            // 何も見えないためポップは原理的に起きない。
            // 月の位置が独立だとこの保証が無く、太陽が沈む瞬間に月が高く昇っていると
            // 0.25lxの直接光が向きだけ突然入れ替わる(夜の影が見える明るさなので実際に目に付く)。
            // そこで月の立ち上がりを太陽の仰角0°→-5°に遅らせ、
            // **切替点では太陽も月も厳密に0**という性質を保つ
            const float kSin5Deg = std::sin(XMConvertToRadians(5.0f));
            const float moonNightGate = Smoothstep(0.0f, kSin5Deg, -sunElevationSin);
            const float moonFactor = moonElevationFactor * moonNightGate;

            // 太陽の色味(ティント)。ピーク照度はkSunIlluminanceLuxが持つので、ここは相対比のみ。
            // 仰角が低いほど暖色へ寄せる(大気の光路長が伸びて短波長が散乱で失われる現象の
            // アート的な近似。朝焼け・夕焼けの赤みはこれで出る)
            const XMFLOAT3 kSunColorTintHigh{ 1.0f, 0.967f, 0.9f };
            const XMFLOAT3 kSunColorTintHorizon{ 1.0f, 0.55f, 0.30f };
            const float warmth = 1.0f - sunFactor; // 仰角15度以上で0、地平線で1
            const XMFLOAT3 kSunColorTint{
                kSunColorTintHigh.x + (kSunColorTintHorizon.x - kSunColorTintHigh.x) * warmth,
                kSunColorTintHigh.y + (kSunColorTintHorizon.y - kSunColorTintHigh.y) * warmth,
                kSunColorTintHigh.z + (kSunColorTintHorizon.z - kSunColorTintHigh.z) * warmth,
            };
            // 満月の照度[lx]。太陽(10万lx)の40万分の1という実測値。
            // 月光は分光的には太陽光とほぼ同じだが、暗所視で青く感じられる(プルキンエ現象)ため
            // 慣例に従って寒色のティントを当てる(物理ではなくアート的な選択)
            const XMFLOAT3 kMoonColorTint{ 0.75f, 0.85f, 1.0f };
            // 夜間の環境光は天文学的な実測値(星明かり~0.001lx、満月~0.1〜0.3lx)をそのまま使うと
            // ほぼ完全な黒になり視認性が失われるため、視認性確保のためのアート的な下限値のまま残す
            // (物理値ではないことを明記した上での意図的な妥協)
            const XMFLOAT3 kNightAmbientArt{ 0.006f, 0.008f, 0.015f };

            // === 平行光源1枠を太陽と月で共有し、支配的な方を選ぶ ===
            // 太陽10万lx と満月0.25lx は40万倍違うので、両者の照度が入れ替わるのは
            // 実質的に太陽の仰角0度ちょうどの一点だけ。そこでは SunFactor も MoonFactor も
            // 厳密に0(=どちらの色もゼロ)になるよう moonNightGate で仕込んであるので、
            // 光源の向きが突然変わっても直接光も影も一切見えず、ポップは原理的に発生しない。
            // このためヒステリシスのような追加の対策は要らない
            const float sunIlluminance = kSunIlluminanceLux * sunFactor;
            const float moonIlluminance = kMoonIlluminanceLux * moonFactor;
            result.DominantIsSun = (sunIlluminance >= moonIlluminance);

            const float dominantPeak = result.DominantIsSun ? sunIlluminance : moonIlluminance;
            const XMFLOAT3& dominantTint = result.DominantIsSun ? kSunColorTint : kMoonColorTint;
            result.Color = {
                dominantTint.x * dominantPeak, dominantTint.y * dominantPeak, dominantTint.z * dominantPeak, 0.0f
            };
            // 支配ライトの向き(光が進む向き)。天体が「ある」向きの符号を反転したもの。
            // **カスケードシャドウの行列もこの向きから作ること**(LightColorだけ切り替えると
            // 月夜に太陽方向の影が残ってしまう)
            result.Direction = result.DominantIsSun
                ? XMFLOAT3{ -sunDirection.x, -sunDirection.y, -sunDirection.z }
                : XMFLOAT3{ -moonDirection.x, -moonDirection.y, -moonDirection.z };

            // 非IBLフォールバック用の定数色アンビエント(Enable IBL 無効時のみ使われる)。
            // 昼度は薄明係数をそのまま使う
            const float dayFactor = twilightFactor;
            const float skyPeak = kSkylightIlluminanceLux;
            const XMFLOAT3 dayAmbient{ kSunColorTint.x * skyPeak, kSunColorTint.y * skyPeak, kSunColorTint.z * skyPeak };
            // 夜間の下限値もここでは絶対値のまま持つ(露出はRender()側で掛ける)
            const float kNightAmbientScale = kMoonSkyIlluminanceLux;
            result.Ambient =
            {
                kNightAmbientArt.x * kNightAmbientScale + (dayAmbient.x - kNightAmbientArt.x * kNightAmbientScale) * dayFactor,
                kNightAmbientArt.y * kNightAmbientScale + (dayAmbient.y - kNightAmbientArt.y * kNightAmbientScale) * dayFactor,
                kNightAmbientArt.z * kNightAmbientScale + (dayAmbient.z - kNightAmbientArt.z * kNightAmbientScale) * dayFactor,
                dayFactor,
            };

            // === 手続き空(SkyGenerate.hlsl)へ渡す値 ===
            // 空が届ける照度は薄明係数で変調する。GPU側の照度正規化積分(SkyIntegrate.hlsl)
            // が「目標照度ちょうど」を保証するので、時刻による空の明るさは
            // ここの係数だけで素直に制御できる。
            // 夜側は月明かりで散乱する空の照度を足す(満月時の夜空はおよそ0.05lx相当)。
            // 月が地平線下でも星明かりぶんは残る(月の位置は手動指定で
            // 「月の出ていない夜」も作れるため、そこで0にしない)
            const float nightFactor = 1.0f - twilightFactor;
            result.SkyIlluminanceLux = kSkylightIlluminanceLux * twilightFactor +
                                       kMoonSkyIlluminanceLux * moonFactor +
                                       kStarlightIlluminanceLux * nightFactor;
            result.TwilightFactor = twilightFactor;
            // 空生成が使うのは**常に太陽の位置**(月が支配的でもPerez分布の基準は太陽のまま)。
            // result.Direction は支配ライトの向きなので、そこから逆算してはいけない
            result.SunPosition = sunDirection;

            // このフレームの「キーとなる照度」。可変プリ露出の基準にする(Render()参照)。
            // 支配ライトと空の両方を足すのは、太陽が沈んだ直後のように
            // 直接光がほぼ0でも空がまだ明るい時間帯を正しく拾うため
            result.KeyIlluminanceLux = std::max(sunIlluminance, moonIlluminance) + result.SkyIlluminanceLux;

            return result;
        }

        // Shaders/GBuffer.hlsl・Shaders/Shadow.hlslのObjectConstants(register b1)と
        // レイアウトを一致させる必要がある。DX12のルートシグネチャがCBVをb0/b1の2枠しか
        // 持たないため、モデル行列もマテリアル係数(Emissive/AlphaCutoff含む)と同居させている
        // (Architecture.html参照)。float3(EmissiveFactor)以降が16バイト境界をまたがないよう、
        // 直前のMetallicFactor/RoughnessFactor/TangentSignFlip/AlphaCutoffで先に16バイトを
        // 埋めてからEmissiveFactor+OcclusionStrengthで次の16バイトを埋める配置にしている
        struct alignas(16) ObjectConstants
        {
            DirectX::XMFLOAT4X4 World;
            DirectX::XMFLOAT4X4 NormalMatrix;
            float MetallicFactor;
            float RoughnessFactor;
            float TangentSignFlip;
            // 0以下ならアルファカットアウト無効
            float AlphaCutoff;
            float EmissiveFactor[3];
            // glTFのocclusionTexture.strength(既定1.0)
            float OcclusionStrength;
            // glTFのbaseColorFactor(既定[1,1,1,1])。BaseColorTextureと乗算して使う。
            // GBuffer.hlsl(不透明)・Transparent.hlsl(半透明)・ProbeCapture.hlsl(プローブ焼き込み)
            // が同じ位置で宣言している。Shadow.hlslは深度しか書かないため先頭までしか宣言していないが、
            // 定数バッファの末尾を読まないだけなのでレイアウトの不一致にはならない(14章参照)
            float BaseColorFactor[4];
            // マテリアル種別ID(末尾に追加)。0=通常マテリアル、
            // 1=水面(kMaterialIDWater、Shaders/3D/GBufferCommon.hlsliの値と一致させること)。
            // 末尾に足す限り、既に宣言済みのシェーダのcbufferオフセットは1バイトも動かない
            // (Shadow.hlsl等が先頭までしか宣言していなくても影響しない、という上のBaseColorFactorの
            // コメントと同じ理由)
            float MaterialID;
            // メッシュシェーダー経路(Shaders/3D/GBufferMeshlet.hlsl)がジオメトリを引くための
            // bindlessディスクリプタ番号。頂点シェーダー経路では読まれない。
            // すべて4バイトのスカラーなので、末尾に足しても既存フィールドのオフセットは動かない。
            //
            // 【3本ともモデル単位】かつてメッシュ単位のバッファを指していたが、
            // 1回のDispatchMeshでモデル全体を描けるようにするためモデル単位へ統合した
            // (Assets::GpuMeshletのコメント参照)。頂点バッファの番号はメッシュレット1件ごとに
            // 持たせてあるので、ここでは渡さない。
            //
            // 【MeshletOffsetは旧VertexBufferIndexの枠】読むのはGBufferMeshlet.hlslだけで、
            // かつ同時に直すため、枠を使い回してもレイアウトのずれは起きない
            uint32_t MeshletOffset;
            uint32_t MeshletBufferIndex;
            uint32_t MeshletVertexBufferIndex;
            uint32_t MeshletTriangleBufferIndex;
            // このドローで見るメッシュレット数(増幅シェーダーの範囲外判定用)
            uint32_t MeshletCount;
            // 透過率(0=不透明)。GBufferパスがG-BufferのAlbedo.aへ書き、
            // DirectLighting.hlslの透過項が読む(45章)。
            // 4バイトのスカラーを末尾に足しているだけなので、既存フィールドのオフセットは動かない
            float Translucency;
            // モデルLODのクロスディザ係数。1.0=切替中でない(全画素を描く)、
            // 0<f<1=切り替え先、-1<f<0=切り替え元。意味と対称性の理由は
            // Shaders/3D/GBufferCommon.hlsli の DitherFade のコメントを参照。
            // 既定を1.0にしたいので、MakeObjectConstantsが明示的に代入する
            // (ObjectConstants{}のゼロ初期化のままだと全画素が捨てられる)
            float DitherFade;

            // --- マテリアルテーブル経路(1モデル1ドロー)専用 -------------------------------
            //
            // 1回のDispatchMeshでモデル全体を描くと、上のMetallicFactor〜Translucencyのような
            // 「メッシュごとに違う値」を定数バッファでは渡せない。代わりにマテリアルを
            // 構造化バッファ(Assets::GpuMaterial)へ載せ、その番号をここで渡す。
            // kInvalidBindlessIndexならピクセルシェーダーは従来の定数+t0〜t6経路を使う
            uint32_t MaterialTableIndex;
            // 増幅シェーダーがメッシュレットを取捨するマスク(Assets::kGpuMaterialFlag*)
            uint32_t MeshletFilterReject;
            uint32_t MeshletFilterRequire;
            // シーン全体の自発光倍率と遮蔽マップの有効/無効(1.0 or 0.0)。
            //
            // 【従来経路では必ず1.0を入れる】これまでこの2つはMakeObjectConstantsが
            // 係数へ掛けてから渡していた。ピクセルシェーダーはどちらの経路でも必ず
            // 掛けるようにしてあるので、既に織り込み済みの従来経路では1.0でなければ
            // 二重に掛かる
            float EmissiveIntensity;
            float OcclusionMapScale;
            // このドローでメッシュレットカリングの統計を数えるか(0/1)。
            // 深度プリパスは G-Buffer と同じ増幅シェーダーを使うため、
            // フレーム全体のフラグだけだと同じ塊を1フレームに2回数えてしまう
            uint32_t MeshletStatsEnabled;

            // --- メッシュレットLOD(Stage 6) ---
            //
            // 【GBufferCommon.hlsliのObjectConstantsと1バイトも違ってはいけない】
            // 向こうがfloat3ではなくスカラー3つで宣言しているのは、定数バッファのfloat3が
            // 16バイト境界をまたげず、手前に暗黙のパディングが入りうるため。こちらも同じ並びにする
            float ModelBoundsCenter[3];
            float ModelBoundsRadius;
            float MeshletLODCameraPos[3];
            float MeshletLODPixelScale;
            float MeshletLODScreenSize;
            int32_t MeshletLODForced;
            // メッシュレットの色分けを「塊ごと」ではなく「段ごと」にするか(0/1)
            uint32_t MeshletDebugColorByLOD;
            // このモデルが選べる最も粗い段(Assets::Model::MeshletLODLevelCap)
            uint32_t MeshletLODLevelCap;
            // インスタンシング。InstancingEnabledが0以外のとき、頂点シェーダーは
            // World/NormalMatrix/TangentSignFlipを上の値ではなく
            // ModelInstances[InstanceBase + SV_InstanceID]から取る
            // (Shaders/3D/ObjectConstants.hlsliのFetchModelInstance)。
            // 0のときは従来どおりここの値を使うので、既存の描画は1ビットも変わらない
            uint32_t InstanceBase;
            uint32_t InstancingEnabled;
            // このドローでHi-Zオクルージョン判定をどう行うか(0=しない / 1=前フレームのHi-Z /
            // 2=今フレームのHi-Z)。値の意味と、パスで分ける必要がある理由は
            // Shaders/3D/GBufferCommon.hlsli の MeshletOcclusionMode を参照
            uint32_t MeshletOcclusionMode;
        };

        // モデルのAABBから外接球を作り、段の選択に要る値を定数へ書き込む。
        //
        // 【AABBの外接球を使う】メッシュ単位ではなくモデル単位にするのは、
        // 1つのモデルの中で段を混ぜないため。段が混ざると、簡略化で頂点が動いた側と
        // 動いていない側で辺が一致せず、境目に穴が開く
        void ApplyMeshletLODConstants(
            ObjectConstants& constants, const Assets::Model& model,
            const MeshletLODFrameConstants& lod)
        {
            for (int axis = 0; axis < 3; ++axis)
            {
                constants.ModelBoundsCenter[axis] =
                    (model.BoundsMin[axis] + model.BoundsMax[axis]) * 0.5f;
            }
            const float halfX = (model.BoundsMax[0] - model.BoundsMin[0]) * 0.5f;
            const float halfY = (model.BoundsMax[1] - model.BoundsMin[1]) * 0.5f;
            const float halfZ = (model.BoundsMax[2] - model.BoundsMin[2]) * 0.5f;
            constants.ModelBoundsRadius = std::sqrt(halfX * halfX + halfY * halfY + halfZ * halfZ);

            constants.MeshletLODCameraPos[0] = lod.CameraPos.x;
            constants.MeshletLODCameraPos[1] = lod.CameraPos.y;
            constants.MeshletLODCameraPos[2] = lod.CameraPos.z;
            constants.MeshletLODPixelScale = lod.PixelScale;

            // しきい値はモデルごとに決める。
            //
            // 【なぜ画素数の定数を全モデルへ当てはめないか】三角形数はモデルによって3桁違う
            // (小道具の数千 ⇔ PLATEAUの地形タイルの134万)。単一の値にすると、
            // 小さいモデルでは早く粗くなりすぎ、地形では一度も段が落ちない。
            // 基準は「原寸の三角形1つが画面上で1画素を切ったら段を落とす」で、
            // 直径D画素の円にN個の三角形があるとき平均面積は (πD²/4)/N なので
            // 1画素を切る直径は sqrt(4N/π)。Qualityはその倍率(大きいほど原寸を保つ)
            constexpr float kInvPi = 0.31830988618379067f;
            const float triangles = static_cast<float>(model.TotalTriangleCount);
            constants.MeshletLODScreenSize =
                (lod.Quality > 0.0f && triangles > 0.0f)
                    ? lod.Quality * std::sqrt(4.0f * triangles * kInvPi)
                    : 0.0f;
            constants.MeshletLODForced = lod.Forced;
            constants.MeshletDebugColorByLOD = lod.DebugColorByLOD ? 1u : 0u;
            // 【全メッシュの共通部分まで畳んだ値を渡す】メッシュごとの段数で
            // 増幅シェーダーが読み替えると、段を1つしか持たないメッシュだけが
            // 原寸のまま残り、1つのモデルの中で段が混ざる(境目に穴が開く)
            constants.MeshletLODLevelCap = model.MeshletLODLevelCap;
        }

        // instance.World/NormalMatrix/TangentSignFlipはAssets::LoadScene(SceneLoader.cpp)が
        // TRS(平行移動・回転・スケール)から計算済み(HLSL側のmul(vec, matrix)規約に合わせて
        // 転置済み)なので、ここでは単純にコピーするだけでよい
        // emissiveIntensity: シーン全体の自発光の強度倍率(m_EmissiveIntensity)。glTFの
        // emissiveFactorは通常1.0以下に収まるため、これを掛けないとG-Bufferのエミッシブを
        // HDR化しても照明器具の輝度が1.0を超えず、ブルームが効かない
        // occlusionMapEnabled: マテリアルの遮蔽マップを使うか(m_OcclusionMapEnabled)。
        // 各パスは lerp(1, occlusionSample, OcclusionStrength) で遮蔽率を求めるため、
        // ここで0を渡せばシェーダー側に手を入れずに遮蔽マップの寄与だけを消せる
        // ditherFade: モデルLODの切り替え中だけ1.0以外を渡す(既定の1.0は「全画素を描く」)。
        // 呼び出し箇所7つのうち、2段を重ねるのはG-Bufferと深度プリパスだけなので既定値を持たせている。
        // シャドウ・プローブ・DDGIは常に最も粗い段を1つだけ描くためフェードそのものが起きない
        // 【モデルは引数で受け取る】meshが属する段のメッシュレット表を指す必要がある。
        // instance.Modelは最も詳細な段でしかなく、シャドウや粗い段を描くときは食い違う
        ObjectConstants MakeObjectConstants(
            const Assets::ModelInstance& instance, const Assets::Model& model, const Assets::Mesh& mesh,
            float emissiveIntensity, bool occlusionMapEnabled, const MeshletLODFrameConstants& meshletLOD,
            float ditherFade = 1.0f)
        {
            ObjectConstants constants{};
            constants.DitherFade = ditherFade;
            constants.World = instance.World;
            constants.NormalMatrix = instance.NormalMatrix;
            constants.MetallicFactor = mesh.MetallicFactor;
            constants.RoughnessFactor = mesh.RoughnessFactor;
            constants.TangentSignFlip = instance.TangentSignFlip;
            constants.AlphaCutoff = mesh.AlphaCutoff;
            constants.EmissiveFactor[0] = mesh.EmissiveFactor[0] * emissiveIntensity;
            constants.EmissiveFactor[1] = mesh.EmissiveFactor[1] * emissiveIntensity;
            constants.EmissiveFactor[2] = mesh.EmissiveFactor[2] * emissiveIntensity;
            constants.OcclusionStrength = occlusionMapEnabled ? mesh.OcclusionStrength : 0.0f;
            constants.BaseColorFactor[0] = mesh.BaseColorFactor[0];
            constants.BaseColorFactor[1] = mesh.BaseColorFactor[1];
            constants.BaseColorFactor[2] = mesh.BaseColorFactor[2];
            constants.BaseColorFactor[3] = mesh.BaseColorFactor[3];
            // 水面(kMaterialIDWater、Shaders/3D/GBufferCommon.hlsliと一致させること)。
            // 水面以外は0.0f(通常マテリアル)のまま
            constants.MaterialID = instance.IsWater ? 1.0f : 0.0f;
            constants.Translucency = mesh.Translucency;

            // メッシュレット。ModelLoaderが登録済みの番号をそのまま渡す。
            // メッシュシェーダー非対応・メッシュレット未生成の場合は
            // バッファ自体が無く、GetBindlessIndexはkInvalidBindlessIndexを返す
            // (MeshletCountが0ならメッシュシェーダー経路には入らないため、その値は使われない)。
            // 表はモデル単位なので、このメッシュのぶんの範囲をMeshletOffset/MeshletCountで示す
            const auto bindlessIndexOf = [](const RHI::IRHIBuffer* buffer) {
                return buffer ? buffer->GetBindlessIndex() : RHI::kInvalidBindlessIndex;
            };
            constants.MeshletOffset = mesh.MeshletOffset;
            constants.MeshletBufferIndex = bindlessIndexOf(model.MeshletBuffer.get());
            constants.MeshletVertexBufferIndex = bindlessIndexOf(model.MeshletVertexBuffer.get());
            constants.MeshletTriangleBufferIndex = bindlessIndexOf(model.MeshletTriangleBuffer.get());
            // 【LOD0の個数ではなく全段の合計】表には全段が並んでおり、増幅シェーダーが
            // 段を選ぶには選ばれうる段すべてが走査範囲に入っていなければならない。
            // LOD0の個数のままだと、粗い段を選んでも表の後ろ半分に届かず何も描かれない
            constants.MeshletCount = mesh.MeshletTotalCount;
            ApplyMeshletLODConstants(constants, model, meshletLOD);

            // メッシュ単位の経路。マテリアルは上の定数とt0〜t6から読むため、
            // テーブルは使わない(=無効番号)。EmissiveFactorとOcclusionStrengthには
            // 既にシーン全体の倍率が織り込まれているので、シェーダー側の乗算は1.0にする
            constants.MaterialTableIndex = RHI::kInvalidBindlessIndex;
            constants.MeshletFilterReject = 0;
            constants.MeshletFilterRequire = 0;
            constants.EmissiveIntensity = 1.0f;
            constants.OcclusionMapScale = 1.0f;
            return constants;
        }

        // 1回のDispatchMeshでモデル全体を描くときの定数。
        //
        // 【メッシュ単位の値を入れない】マテリアルの係数もテクスチャもモデル内で
        // メッシュごとに違うため、定数バッファでは渡せない。ピクセルシェーダーは
        // メッシュシェーダーが出力したMaterialIndexでマテリアルテーブルを引く。
        // World/NormalMatrix/TangentSignFlip/MaterialIDだけがインスタンス単位の値で、
        // これらはモデル全体で共通なので従来どおり定数バッファで渡してよい。
        //
        // rejectMask/requireMask: このパスで描くマテリアルの選び方
        // (Assets::kGpuMaterialFlag*。GBufferCommon.hlsliのMeshletFilter*参照)
        // 【モデルは引数で受け取る】モデルLODが入り、instance.Modelは「最も詳細な段」でしかない。
        // シャドウは最も粗い段、G-Buffer/プリパスはそのフレームで選ばれた段を描くので、
        // どの段のメッシュレット表を指すかは呼び出し側にしか決められない
        ObjectConstants MakeModelObjectConstants(
            const Assets::ModelInstance& instance, const Assets::Model& model, float emissiveIntensity,
            bool occlusionMapEnabled, uint32_t rejectMask, uint32_t requireMask,
            const MeshletLODFrameConstants& meshletLOD, bool countCullStats = false,
            float ditherFade = 1.0f, uint32_t occlusionMode = 0u)
        {
            ObjectConstants constants{};
            constants.DitherFade = ditherFade;
            constants.World = instance.World;
            constants.NormalMatrix = instance.NormalMatrix;
            constants.TangentSignFlip = instance.TangentSignFlip;
            // 水面はメッシュレット経路に載せない(ShouldUseMeshletPath)ので常に通常マテリアル
            constants.MaterialID = 0.0f;

            const auto bindlessIndexOf = [](const RHI::IRHIBuffer* buffer) {
                return buffer ? buffer->GetBindlessIndex() : RHI::kInvalidBindlessIndex;
            };
            // モデル全体の塊を1回で回すので、範囲は表の先頭から全件
            constants.MeshletOffset = 0;
            constants.MeshletBufferIndex = bindlessIndexOf(model.MeshletBuffer.get());
            constants.MeshletVertexBufferIndex = bindlessIndexOf(model.MeshletVertexBuffer.get());
            constants.MeshletTriangleBufferIndex = bindlessIndexOf(model.MeshletTriangleBuffer.get());
            // TotalMeshletCountは全段の合計(ModelLoaderが表へ全段を載せている)
            constants.MeshletCount = model.TotalMeshletCount;
            ApplyMeshletLODConstants(constants, model, meshletLOD);

            constants.MaterialTableIndex = bindlessIndexOf(model.MaterialTableBuffer.get());
            constants.MeshletFilterReject = rejectMask;
            constants.MeshletFilterRequire = requireMask;
            // マテリアルテーブルは読み込み時に焼くため、シーン全体の倍率は焼き込めない。
            // ピクセルシェーダーがここの値を掛ける
            constants.EmissiveIntensity = emissiveIntensity;
            constants.OcclusionMapScale = occlusionMapEnabled ? 1.0f : 0.0f;
            // 統計を数えるのは G-Buffer パスだけ。深度プリパスとシャドウは同じ
            // 増幅シェーダーを使うので、ここで切らないと同じ塊を何度も数えてしまう
            constants.MeshletStatsEnabled = countCullStats ? 1u : 0u;
            constants.MeshletOcclusionMode = occlusionMode;
            return constants;
        }

        // Present.hlsl側のModeと一致させる必要がある
        struct alignas(16) PresentConstants
        {
            int32_t Mode;
            float MipLevel; // Mode==6(Hi-Z)でSampleLevelに渡すミップレベル
            // Mode==10(シャドウマップ配列)ではカスケード番号、
            // Mode==12(反射プローブのキューブマップ配列)では表示するプローブ番号として使う
            float ArraySlice;
            // デバッグ表示の輝度倍率(m_DebugViewGain)。色として表示するMode 0/3/4にだけ効く
            float Gain;
            // Mode==11(タイルライトカリングのヒートマップ)専用。
            // x=タイル数X, y=タイルの1辺のピクセル数, z=1タイルあたりの容量, w=ヒートマップの上限ライト数
            DirectX::XMFLOAT4 TileParams;
            // Mode==11専用。xy=レンダー解像度(UVからタイル座標を求めるのに使う), zw=未使用
            DirectX::XMFLOAT4 TileRenderSize;
            // Mode==22(MegaLightsの蓄積平均)専用。x=これまでに足したフレーム数, yzw=未使用。
            // **末尾に足すこと** ―― cbufferは宣言順レイアウトなので、途中へ挿すと
            // Present.hlsl側の以降のフィールドがすべてずれる
            DirectX::XMFLOAT4 AccumParams;
        };

        // Tonemap.hlsl側のcbuffer TonemapConstantsと一致させる必要がある
        struct alignas(16) TonemapConstants
        {
            // KurenaiEngine3D::TonemapCurve(0=Reinhard, 1=ACES, 2=AgX)
            int32_t Curve;
            // 手動露出時に掛ける倍率。プリ露出は時刻連動で変動するため、ユーザー設定EV100との
            // 差分 2^(実効EV100 - 設定EV100) を割り戻して固定露出の絵に戻す(1.0固定ではない)
            float ExposureScale;
            // ディザの強さ(0=無効、1=±1LSB)
            float DitherStrength;
            // 1.0=自動露出、0.0=手動
            float UseAutoExposure;
            // CPU側でライト強度へ事前乗算済みのEV100(プリ露出)
            float PreExposureEV100;
            // ブルームの合成比(0で無効)
            float BloomStrength;
            // 薄明視の適用量(0で無効、1で完全適用)
            float MesopicStrength;
            // 目が順応している明るさ(EV100)。構図にも露出設定にも依存しない
            float MesopicAdaptationEV100;
            // TAAの蓄積で失われた高域を戻すシャープネス(0で無効)。TAAが無効のときは常に0。
            //
            // 【なぜTAAではなくここなのか】TAAの入力へ掛けると、アンシャープマスクが
            // 増幅する高域は「ジッターで毎フレーム変動する成分」そのものなので静止時のちらつきが
            // 大きく増える。ここはトーンマップ後のLDR値に対して掛かるだけで
            // どこへもフィードバックされないため、ちらつきにもリンギングの累積にも寄与しない
            float Sharpness;
            // シャープネスの近傍タップに使う1テクセルぶんのUV(1/レンダー解像度)
            float InvRenderWidth;
            float InvRenderHeight;
            // 黒の締め(ブラックポイント)。0で恒等。詳細はTonemap.hlsl側のコメント参照
            float BlackPoint;
        };

        // Upscale.hlsl側のcbuffer UpscaleConstantsと一致させる必要がある
        struct alignas(16) UpscaleConstants
        {
            // EASUの事前計算定数。ComputeEasuConstants()が入力/出力解像度から作る
            DirectX::XMFLOAT4 EasuCon0;
            DirectX::XMFLOAT4 EasuCon1;
            DirectX::XMFLOAT4 EasuCon2;
            DirectX::XMFLOAT4 EasuCon3;
            // 書き込み先のサイズ(出力解像度)
            DirectX::XMUINT2 OutputSize;
            // RCASのシャープネス(ComputeRcasSharpnessScaleで変換済みの線形値)
            float RcasSharpnessScale;
            float UpscalePadding;
        };

        // FSR1のFsrEasuCon()と同じ内容。出力画素の整数座標から入力画像の再構成位置を求めるための
        // スケール/オフセットと、12タップぶんの4回のGather4の中心へのオフセットを作る。
        //
        // 参照実装はこれらをuintへビットキャストして渡すが、それはFP16パック経路(A_HALF)と
        // 定数を共用するためで、SM5.0でも動く必要がある(=16bitパック経路を使わない)このエンジンでは
        // floatのまま持つほうがCPU側の構造体と素直に対応する
        void ComputeEasuConstants(
            UpscaleConstants& constants, uint32_t inputWidth, uint32_t inputHeight,
            uint32_t outputWidth, uint32_t outputHeight)
        {
            const float inputW = static_cast<float>(inputWidth);
            const float inputH = static_cast<float>(inputHeight);
            const float outputW = static_cast<float>(outputWidth);
            const float outputH = static_cast<float>(outputHeight);

            // 出力の整数座標 → 入力の画素座標。0.5を引いているのはテクセル中心合わせ
            constants.EasuCon0 = {
                inputW / outputW,
                inputH / outputH,
                0.5f * inputW / outputW - 0.5f,
                0.5f * inputH / outputH - 0.5f,
            };
            // 入力の画素座標 → 正規化UV。zwは12タップの左上ブロック('F'タップ)へのオフセット
            constants.EasuCon1 = { 1.0f / inputW, 1.0f / inputH, 1.0f / inputW, -1.0f / inputH };
            // 残り3つのGather中心へのオフセット(いずれも'F'ではなく1つめのGather中心からの相対)
            constants.EasuCon2 = { -1.0f / inputW, 2.0f / inputH, 1.0f / inputW, 2.0f / inputH };
            constants.EasuCon3 = { 0.0f, 4.0f / inputH, 0.0f, 0.0f };
        }

        // SkyGenerate.hlsl側のcbuffer SkyBakeConstantsと一致させる必要がある
        // SkyIntegrate.hlsl が書き、SkyGenerate.hlsl / DeferredLighting.hlsl / SSR.hlsl が読む
        // 構造化バッファ(要素数1)の1要素。Sky.hlsliのGPUSkyParametersと完全に一致させること
        struct alignas(16) GPUSkyParameters
        {
            DirectX::XMFLOAT4 ZenithTint;    // xyz
            DirectX::XMFLOAT4 HorizonTint;   // xyz
            DirectX::XMFLOAT4 GroundTint;    // xyz
            DirectX::XMFLOAT4 SunGlowTint;   // xyz=色、w=強さ
            DirectX::XMFLOAT4 Luminance;     // x=天頂輝度(実効プリ露出込み、雲の減光は含まない)
                                              // y=余弦重み積分の値(ログ・検証用)、zw=予備
            // Preetham xyYモデル用のパラメータ。x=タービディティ、y=Preethamの重み
            // (0=従来ティントのみ、1=Preethamのみ)、zw=予備
            DirectX::XMFLOAT4 ModelParams;
        };

        // SkyIntegrate.hlsl側のcbuffer SkyIntegrateConstantsと一致させる必要がある
        struct alignas(16) SkyIntegrateConstants
        {
            // xyz=太陽が「ある」向き(正規化済み。光が進む向きとは符号が逆)、w=未使用
            DirectX::XMFLOAT4 SunDirection;
            // x=目標照度[lx](SunLighting::SkyIlluminanceLux)、y=実効プリ露出(effectiveExposure)、
            // z=タービディティ(m_SkyTurbidity)、w=未使用
            DirectX::XMFLOAT4 IntegrateParams;
        };

        // AtmosphereLUT.hlsl側のcbuffer AtmosphereConstantsと一致させる必要がある。
        // 3つのエントリポイント(Transmittance/MultiScattering/SkyView)が共通で読む
        struct alignas(16) AtmosphereConstants
        {
            // xyz=太陽が「ある」向き(正規化済み)、w=未使用。CSSkyViewのみが使う
            DirectX::XMFLOAT4 SunDirection;
            // x=Mie(エアロゾル)密度の倍率(濁りのスライダー由来)、yzw=未使用
            DirectX::XMFLOAT4 Params0;
        };

        // SkyGenerate.hlsl側のcbuffer SkyBakeConstantsと一致させる必要がある
        struct alignas(16) SkyBakeConstants
        {
            // 処理対象の面(D3D標準順: +X=0,-X=1,+Y=2,-Y=3,+Z=4,-Z=5)
            uint32_t Face;
            // 雲(判断B)による平均透過率。SkyParametersBuffer[0].Luminance.x
            // (雲を考慮しない晴天基準の天頂輝度)にこの値を掛けてからキューブへ焼く
            float CloudTransmittance;
            float Padding0[2];
            // 太陽が「ある」向き(正規化済み。光が進む向きとは符号が逆)
            DirectX::XMFLOAT4 SunDirection;
        };

        // Bloom.hlsl側のcbuffer BloomConstantsと一致させる必要がある
        struct alignas(16) BloomConstants
        {
            DirectX::XMUINT2 SrcSize;
            DirectX::XMUINT2 DstSize;

            float Threshold;
            float SoftKnee;
            // 1.0なら最初のダウンサンプル(Karis平均としきい値を適用する)
            float ApplyKarisAndThreshold;
            // 1.0=自動露出、0.0=手動(Tonemapと同じ意味)
            float UseAutoExposure;

            // CPU側でライト強度へ事前乗算済みのEV100(プリ露出)
            float PreExposureEV100;
            // 手動露出時に掛ける倍率(TonemapConstants::ExposureScaleと同じ値)
            float ExposureScale;
            float Padding[2];
        };

        // AutoExposure.hlsl側のcbuffer AutoExposureConstantsと一致させる必要がある
        struct alignas(16) AutoExposureConstants
        {
            DirectX::XMUINT2 InputSize;
            float MinEV100;
            float MaxEV100;

            float PreExposureEV100;
            float DeltaTime;
            float AdaptationSpeedUp;
            float AdaptationSpeedDown;

            float LowPercentile;
            float HighPercentile;
            float ExposureCompensation;

            // 暗いシーンをわざと暗いまま写すための補正カーブ(AutoExposure.hlsl参照)
            float NightRolloffEV;
            float NightRolloffDarkEV100;
            float NightRolloffBrightEV100;

            // 測光値の上側クランプ(構図依存を抑える。AutoExposure.hlsl参照)
            float KeyReferenceEV100;
            float KeyCeilingEV;

            // 0以外なら順応を飛ばして測光値へ即座に合わせる(シーン切り替え時。
            // m_AutoExposureResetRequested参照)
            float ResetAdaptation;
            float Padding[3];
        };

        // HiZ.hlsl側のcbuffer HiZConstantsと一致させる必要がある
        struct alignas(16) HiZConstants
        {
            DirectX::XMUINT2 SrcSize;
            DirectX::XMUINT2 DstSize;
        };

        // ModelCull.hlsl の cbuffer ModelCullConstants と並びを一致させること
        struct alignas(16) ModelCullConstants
        {
            // 視錐台判定に使う。**今フレームの**ビュー射影行列(CPU側の判定と揃える)
            DirectX::XMFLOAT4X4 CullViewProj;
            // Hi-Z判定に使う。そのHi-Zの元になった深度を描いた行列。
            // 深度プリパスから作る経路では今フレーム、そうでなければ前フレームのもの
            DirectX::XMFLOAT4X4 CullPrevViewProj;
            // x=候補数、y=Hi-Zのミップ段数、z=オクルージョン判定の有効フラグ、
            // w=引数配列の先頭オフセット[バイト]
            DirectX::XMUINT4 CullParams;
            // x=区画1つぶんのバイト数、y=区画数、
            // z=このディスパッチが受け持つ候補の先頭番号、
            // w=統計を数え始める候補番号(G-Bufferぶんの先頭)
            DirectX::XMUINT4 CullRegionParams;
            // xy=Hi-Zのミップ0の解像度[画素]、zw=未使用
            DirectX::XMFLOAT4 CullHiZScreenParams;
            // x=AABBを膨らませる量[m](前フレームからのカメラ移動距離)、yzw=未使用
            DirectX::XMFLOAT4 CullExpandParams;
        };

        // 間接描画の行き先の区画。**PSOごとに1区画**で、1区画につき1回ExecuteIndirectする。
        // 1回のExecuteIndirectで切り替えられるのは引数に含めたルートパラメータだけで、
        // PSOは切り替えられないため、まとめられない。
        //
        // 深度プリパスとG-Bufferの両方をここで面倒を見るのは、**片方だけ間引くと絵が壊れる**
        // ため。プリパスが深度を書いたものをG-Bufferが描かないと、その画素は
        // 「深度はあるのに色が無い」穴になる(逆向き ―― プリパスが描かずG-Bufferが描く ――
        // は早期Zが効かなくなるだけで絵は正しい)
        enum ModelCullRegion : uint32_t
        {
            kModelCullRegionGBuffer = 0,
            kModelCullRegionGBufferMirrored,
            kModelCullRegionPrepassOpaque,
            kModelCullRegionPrepassOpaqueMirrored,
            kModelCullRegionPrepassCutout,
            kModelCullRegionPrepassCutoutMirrored,
            kModelCullRegionCount,
        };

        // 引数バッファの先頭に置く「区画ごとの発行数」の領域。ExecuteIndirectの
        // 件数バッファとしてそのまま渡す(1区画あたりuint1つ)。
        //
        // 【256バイトに切り上げる】後ろに続く引数配列の先頭を、GPU仮想アドレスが
        // 8バイト境界に載る位置から始めるため。24バイト刻みの配列は先頭さえ揃えば
        // 以降もすべて8の倍数になる(24は8の倍数)
        constexpr uint32_t kModelCullArgsBaseOffset = 256;
        static_assert(
            kModelCullArgsBaseOffset >= sizeof(uint32_t) * kModelCullRegionCount,
            "区画ごとの発行数が引数配列の領域へはみ出している");
        static_assert(
            (RHI::IRHICommandList::kDispatchMeshIndirectArgStride % 8) == 0,
            "引数の刻みが8の倍数でないと、2件目以降のGPU仮想アドレスが境界を割る");

        // 区画1つぶんのバイト数。区画の境目も8バイト境界に載せたいので256へ切り上げる
        uint32_t ComputeModelCullRegionStride(uint32_t capacity)
        {
            const uint32_t bytes = RHI::IRHICommandList::kDispatchMeshIndirectArgStride * capacity;
            return (bytes + 255u) & ~255u;
        }

        // 間接描画の候補1件。カリング前の列挙結果で、行き先の区画(= PSO)まで確定している。
        //
        // 【ObjectConstantsの中身はここに持たない】GPUへ載せる直前(ModelCullパスの中)で作る。
        // 引数に書き込むのは定数バッファのリングスロットのGPUアドレスで、それは
        // UpdateBufferを呼んだ後にしか分からないため
        struct ModelCullDrawCandidate
        {
            const Assets::ModelInstance* Instance;
            const Assets::Model* Model;
            uint32_t Region;
            uint32_t GroupCount;
            // 増幅シェーダーがメッシュレットを取捨するマスク(Assets::kGpuMaterialFlag*)
            uint32_t RejectMask;
            uint32_t RequireMask;
            float DitherFade;
            // メッシュレット単位のカリング統計を数えるか。
            // **G-Bufferの区画だけtrue** ―― プリパスでも数えると同じメッシュレットを二重に数える
            bool CountCullStats;
            // Hi-Zオクルージョン判定のモード(0=しない / 1=前フレーム / 2=今フレーム)。
            // 深度プリパスとG-Bufferで読むHi-Zの中身が違うため区画ごとに変わる
            uint32_t OcclusionMode;
        };

        // widthとheightのうち大きい方が1になるまでのミップ数(width/heightそのものを含む)を返す。
        // 例: 1280x720 -> max=1280 -> 1280,640,320,160,80,40,20,10,5,2,1 の11ミップ
        uint32_t ComputeMipLevelCount(uint32_t width, uint32_t height)
        {
            uint32_t levels = 1;
            uint32_t size = std::max(width, height);
            while (size > 1)
            {
                size /= 2;
                ++levels;
            }
            return levels;
        }

        // SSAO.hlsl側のkSSAOKernelSizeMaxと一致させる必要がある。
        // 定数バッファに確保する数であって、実際に回す段数(m_SSAOKernelSize)ではない
        constexpr uint32_t kSSAOKernelSizeMax = 16;

        struct alignas(16) SSAOConstants
        {
            DirectX::XMFLOAT4 Samples[kSSAOKernelSizeMax]; // タンジェント空間の半球カーネル
            DirectX::XMFLOAT4 Params;                      // x: 半径, y: バイアス, z: 強さ(べき乗), w: 使うサンプル数
        };

        // SSIL_VisibilityBitmask.hlsl側のcbuffer SSILConstantsと一致させる必要がある
        struct alignas(16) SSILConstants
        {
            DirectX::XMFLOAT4 Params0; // x: 半径, y: 厚み(Thickness Heuristic), z: 間接光の強さ, w: AOのべき乗
            DirectX::XMUINT4 Params1;  // x: スライス数, y: スライスあたりのステップ数, z/w: 未使用
        };

        // SSR.hlsl側のcbuffer SSRConstantsと一致させる必要がある
        struct alignas(16) SSRConstants
        {
            // w: 水面の解析空フォールバックを使うか(1=使う)。Render()側で
            // m_WaterAnalyticSkyReflection && usingProceduralSky の両方が立っているときだけ1にする
            // (手続き空が無効なシーンではDDSは任意の絵でPerezモデルとは無関係なため、
            // このトグルの値に関わらず必ず0にする)
            DirectX::XMFLOAT4 Params0; // x: 最大レイ距離, y: ヒット判定の厚み, z: ラフネスカットオフ, w: 水面の解析空フォールバック
            // 平面反射(末尾に追加)。x: 平面反射が有効か(1=使う。m_PlanarReflectionEnabled &&
            // 水面インスタンスが存在するときのみ1)、y: 波の法線による画面UVのずらし量
            // (m_PlanarReflectionDistortion)、zw: 未使用
            DirectX::XMFLOAT4 Params1;
        };

        // RTReflection.hlsl側のcbuffer RTReflectionConstantsと一致させる必要がある
        struct alignas(16) RTReflectionConstants
        {
            DirectX::XMFLOAT4 Params0; // xy: 出力サイズ(ピクセル), z: 最大レイ距離, w: ラフネスカットオフ
            // x: 影レイを撃つか(1で撃つ)
            // y: メッシュレットのデバッグ表示(1で、反射に映る面をメッシュレット色で塗る)
            // zw: 未使用
            DirectX::XMFLOAT4 Params1;
        };

        // RTShadow.hlsl側のcbuffer RTShadowConstantsと一致させる必要がある
        struct alignas(16) RTShadowConstants
        {
            // xy: 出力サイズ(ピクセル), z: 太陽の見かけの半径(ラジアン), w: 1ピクセルあたりのレイ本数
            DirectX::XMFLOAT4 Params0;
        };

        // MegaLightsTilePool.hlsl側のcbuffer MegaLightsTilePoolConstantsと並びを一致させること。
        // 先頭4つはLightCullingConstantsと同じ並びだが、TileParams.wの意味が違う
        // (あちらは1タイルの容量、こちらは抽出する候補数K)ので構造体は分けてある
        struct alignas(16) MegaLightsTilePoolConstants
        {
            DirectX::XMFLOAT4X4 View;
            // x=タイル数X, y=タイル数Y, z=有効ライト数, w=1タイルあたりの候補数K
            DirectX::XMUINT4 TileParams;
            // x=レンダー解像度の幅, y=同 高さ, zw=未使用
            DirectX::XMUINT4 RenderSize;
            // x=射影行列の(0,0)成分, y=同(1,1)成分、z=深度リニアライズ定数a, w=同b
            DirectX::XMFLOAT4 ProjParams;
            // x=フレーム番号(候補を毎フレーム引き直すための乱数の種)、yzw=未使用
            DirectX::XMUINT4 PoolParams;
        };

        // MegaLightsAccum.hlsl側のcbuffer MegaLightsAccumConstantsと一致させる必要がある
        struct alignas(16) MegaLightsAccumConstants
        {
            // x=出力幅, y=出力高, z=足す前に0で始めるか(1でリセット), w=未使用
            DirectX::XMUINT4 Params0;
        };

        // 確率的サンプリング側の cbuffer MegaLightsStochasticConstants と一致させる必要がある。
        // 読むのは MegaLightsInitialSample.hlsl / MegaLightsTemporal.hlsl /
        // MegaLightsSpatial.hlsl / MegaLightsShade.hlsl / MegaLightsResolve.hlsl の5本で、
        // **宣言をどこまで書くかはファイルごとに違う**(Shade は Params2 まで)。
        // したがって**新しい項目は必ず末尾へ足すこと**
        struct alignas(16) MegaLightsStochasticConstants
        {
            // x=出力幅, y=出力高, z=1ピクセルあたりの初期候補数M, w=影レイを撃つか(0で撃たない)
            DirectX::XMUINT4 Params0;
            // x=タイル数X, y=タイルの1辺のピクセル数, z=1タイルあたりの候補数K, w=フレーム番号
            DirectX::XMUINT4 Params1;
            // x=借りる近傍の数, y=探す半径(ピクセル),
            // z=空間再利用の結合方式(0=confidence重み, 1=不偏化のZ),
            // w=初期可視レイでリザーバを殺すか(Initialが読む)。
            // **末尾に足すこと** ―― Shade は Params1 までしか宣言していないので、
            // 途中へ挿すとあちらのオフセットがずれる
            DirectX::XMUINT4 Params2;
            // x=射影行列の(0,0)成分, y=同(1,1)成分(空間再利用のMIS用。
            // 「その灯が隣のタイルへ届くか」を判定するために隣のタイルの錐台を組み立て直す。
            // **候補プールが使ったのと同じ行列から取ること**。ずれると定義域がずれる)、
            // z=プリ露出の補正倍率(時間再利用用。今の露出 / 前フレームの露出)、
            // w=履歴のMの上限(同)
            DirectX::XMFLOAT4 Params3;
            // x=履歴が使えるか(時間再利用用。0なら履歴を読まない。Initialは
            //   遮蔽が確定した灯のキャッシュを信用してよいかの判定にも使う)、
            // y=空間再利用の反復番号(0起点。近傍の型板の種に混ぜて反復ごとに別の近傍を選ばせる)、
            // z=クアッド共有を行うか(手法3。Resolveが読む。0なら自分の標本だけを使う)、
            // w=クアッドで候補スロットを分けて引くか(手法3の層化。Initialが読む)
            DirectX::XMUINT4 Params4;
            // x=1画素あたりの標本数(リザーバの本数。Initialが書きResolveが読む)。
            // 手法3だけが1より大きくなる ―― 手法2の時間・空間再利用は
            // 「1画素1リザーバ」を前提に添字を組み立てているため。
            // yzw=未使用
            DirectX::XMUINT4 Params5;
        };

        // MegaLightsDenoise.hlsl側のcbuffer MegaLightsDenoiseConstantsと一致させること
        struct alignas(16) MegaLightsDenoiseConstants
        {
            // x=出力幅, y=出力高, z=履歴が使えるか, w=à-trousの段(0起点)
            DirectX::XMUINT4 Params0;
            // x=à-trousのステップ幅, y=時間累積の上限フレーム数,
            // z=輝度のエッジ停止の強さ, w=法線のエッジ停止の指数
            DirectX::XMFLOAT4 Params1;
            // x=深度のエッジ停止の強さ, yzw=未使用
            DirectX::XMFLOAT4 Params2;
        };

        // MegaLightsReference.hlsl側のcbuffer MegaLightsConstantsと一致させる必要がある
        struct alignas(16) MegaLightsConstants
        {
            // x: 出力幅, y: 出力高, z: 1灯あたりに撃つ影レイの本数(0なら影を撃たず可視率1。恒等テスト用),
            // w: 有効ライト数
            DirectX::XMUINT4 Params0;
            // x: フレーム番号。球光源のサンプル列を毎フレーム回すのに使う。
            // 【混ぜないと蓄積が効かない】固定すると毎フレーム同じ点を引き、
            // 何枚足しても可視率のばらつきが残る(MegaLightsReference.hlsl)
            DirectX::XMUINT4 Params1;
        };

        // RTAO.hlsl側のcbuffer RTAOConstantsと一致させる必要がある
        struct alignas(16) RTAOConstants
        {
            // xy: 出力サイズ(ピクセル), z: レイの最大距離, w: 遮蔽率のコントラスト(べき乗)
            DirectX::XMFLOAT4 Params0;
            // x: レイ本数, y: 間接光の強さ, z: バウンス面へ影レイを撃つか, w: 未使用
            DirectX::XMFLOAT4 Params1;
        };

        // TAA.hlsl側のcbuffer TAAConstants(register b1)と並びを一致させる必要がある。
        // TAAパスはb0(FrameConstants)を使わず、必要な行列もすべてこちらへ入れている。
        // FrameConstantsは末尾追加を重ねて700バイトを超えており、cbufferは途中のフィールドを
        // 飛ばせないため、末尾の2つを読むためだけに全フィールドを宣言する羽目になるのを避けている
        struct alignas(16) TAAConstants
        {
            DirectX::XMFLOAT4X4 InvViewProj;  // 今フレームのジッター済み逆VP(空の速度の補完に使う)
            DirectX::XMFLOAT4X4 PrevViewProj; // 前フレームのジッター済みVP
            DirectX::XMFLOAT4 JitterUv;       // xy=今フレームのジッター(UV単位), zw=前フレーム
            DirectX::XMFLOAT4 ScreenParams;   // xy=レンダー解像度, zw=その逆数
            // x: 今フレームの色を混ぜる割合(m_TAABlendWeight)
            // y: 近傍クリップのボックス幅(標準偏差の何倍か。m_TAAClipGamma)
            // z: 履歴が使えるか(0=使えない。TAA.hlslは履歴をサンプルすらしない)
            // w: プリ露出の変化を打ち消す倍率(今フレームの露出 / 前フレームの露出)
            DirectX::XMFLOAT4 Params0;
            // x: 近傍クリップの方式(TAAClipMode)
            // y: 静止時のちらつき抑制の強さ(m_TAAAntiFlicker)。zwは未使用
            DirectX::XMFLOAT4 Params1;
        };

        // DirectLighting.hlsl側のstruct GPULightと並び・ストライド(64バイト)を一致させる必要がある
        struct alignas(16) GPULight
        {
            DirectX::XMFLOAT4 PositionType;   // xyz=ワールド座標, w=LightType
            DirectX::XMFLOAT4 ColorRange;     // rgb=露出済み放射輝度, w=Range
            DirectX::XMFLOAT4 DirectionAngle; // xyz=向き(正規化済み), w=spotAngleScale
            // x=spotAngleOffset
            // y=影のフラグ(bit0=画面空間シャドウ / bit1=レイトレース影レイ。kLightShadow* を使う)
            // z=SourceRadius(球光源の半径 / エミッシブ光源プロキシでは面積等価の円板半径)
            // w=指向性κ(エミッシブ光源プロキシのみ。それ以外は0)
            DirectX::XMFLOAT4 Params;
        };
        static_assert(sizeof(GPULight) == 64, "GPULightはDirectLighting.hlsl側と64バイトで一致させる必要がある");

        // t5の構造化バッファに詰めるライトの最大数。実データ(BistroInterior.fbxで4灯)に対しては
        // 十分すぎる余裕を持たせてあるが、構造化バッファなのでこの容量自体がGPU時間へ影響することはない
        // (シェーダはLightCount.xまでしかループしないため)
        constexpr uint32_t kMaxLights = 1024;

        // GPULight.Params.y のビット。**Shaders/3D/LightAttenuation.hlsli と一致させること**
        constexpr uint32_t kLightShadowScreenSpace = 1u;
        constexpr uint32_t kLightShadowRaytraced = 2u;

        // MegaLightsTilePool.hlsl の kMegaLightsMaxLights と同じ値。あちらはライトごとの重みを
        // groupshared配列に置くためコンパイル時定数である必要があり、C++からの受け渡しでは代用できない。
        //
        // 【なぜ静的検査で縛るのか】候補プールは走査するライト数をこの値で頭打ちにするが、
        // タイルライトカリング(LightCulling.hlsl)は頭打ちしない。kMaxLightsをこれより大きくすると、
        // **判定を共有しているのに定義域だけが黙ってずれる**(あぶれた灯はカリングには入るが
        // 候補プールには入らない)。到達判定の共有では防げない食い違いなので、ここで止める
        constexpr uint32_t kMegaLightsTilePoolMaxLights = 1024;
        static_assert(
            kMaxLights <= kMegaLightsTilePoolMaxLights,
            "kMaxLightsを増やすなら MegaLightsTilePool.hlsl の kMegaLightsMaxLights も同じ値へ上げること"
            "(候補プールが走査するライト数の上限。超えるとタイルライトカリングと定義域がずれる)");

        // ドローンショーの機体数の上限。構造化バッファをこの容量で固定確保する
        // (32バイト×4096 = 128KB。DEFAULTヒープ本体とステージングリングを足しても
        //  1.3MB程度で、機体数を増減しても作り直さずに済む)
        constexpr uint32_t kMaxDrones = 4096;

        // DroneShow.hlsl側のcbuffer DroneShowConstantsと一致させる必要がある。
        // b0のFrameConstantsには相乗りさせない(理由はDroneShow.hlsl冒頭。
        // 巨大なcbufferの途中のフィールドを宣言し忘れるとオフセットが静かにずれる)
        struct alignas(16) DroneShowConstants
        {
            // 転置済み。メイン描画ではカメラのビュー行列、平面反射では鏡映×カメラのビュー行列
            DirectX::XMFLOAT4X4 View;
            // 転置済み。どちらのパスでもメインカメラのジッター済みProj
            DirectX::XMFLOAT4X4 Proj;
            // x=明るさ倍率(実効プリ露出を乗算済み)、y=画面上の最小半径(NDC)、
            // z=射影行列の[0][0]成分、w=未使用
            DirectX::XMFLOAT4 Params0;
            // 平面反射で水面より下の機体を落とすクリップ平面(xyz=法線、w=距離項)
            DirectX::XMFLOAT4 ClipPlane;
            // x=クリップ平面を使うか(0=メイン描画、1=平面反射)、yzw=未使用
            DirectX::XMFLOAT4 Params1;
        };

        // DirectLighting.hlsl側のcbuffer LightingConstantsと一致させる必要がある。
        // b0はFrameConstantsが使っており定数バッファスロットは2本しか無いため、
        // 直接光パス固有のパラメータはすべてここへ足していく
        struct alignas(16) LightingConstants
        {
            // x=有効ライト数, y=ピクセルあたりに撃つスクリーンスペースシャドウのレイ数の上限,
            // z=太陽の影の手法(KurenaiEngine3D::ShadowMode。2のときだけRTShadowTexture(t6)を読む),
            // w=MegaLightsの寄与を使うか(1なら t7 のテクスチャを読み、ライトループを回さない)
            DirectX::XMUINT4 LightCount;
            // スクリーンスペースシャドウ(ScreenSpaceShadow.hlsli)のパラメータ。
            // x=レイマーチのステップ数, y=最大レイ長(ワールド単位), z=遮蔽とみなす深度差の上限(thickness),
            // w=有効フラグ(0で無効)
            DirectX::XMFLOAT4 SSSParams0;
            // x=深度リニアライズ定数a, y=同b(viewZ = b / (depth - a))、
            // z=レイ始点の法線方向への押し出し量(View空間深度に比例させる係数)、w=画面端フェード幅(UV)
            DirectX::XMFLOAT4 SSSParams1;
            // タイルライトカリング(LightCulling.hlsl)のパラメータ。
            // x=タイル数X, y=タイルの1辺のピクセル数, z=1タイルあたりの容量, w=カリング有効フラグ
            DirectX::XMUINT4 TileParams;
        };

        // kLightTileSize / kLightTileCapacity / kLightTileStride はKurenaiEngine3Dのstatic constexprへ
        // 移した(DebugViewPanelがヒートマップの上限として参照するため)。定義はKurenaiEngine3D.h

        // LightCulling.hlsl側のcbuffer LightCullingConstantsと一致させる必要がある
        struct alignas(16) LightCullingConstants
        {
            DirectX::XMFLOAT4X4 View;
            // x=タイル数X, y=タイル数Y, z=有効ライト数, w=1タイルあたりの容量
            DirectX::XMUINT4 TileParams;
            // x=レンダー解像度の幅, y=同 高さ, zw=未使用
            DirectX::XMUINT4 RenderSize;
            // x=射影行列の(0,0)成分, y=同(1,1)成分, z=深度リニアライズ定数a, w=同b
            DirectX::XMFLOAT4 ProjParams;
        };

        // 自前ソフトウェアラスタライザ用。Shaders/3D/SoftwareRasterCommon.hlsliの
        // cbuffer SWRasterConstants(b1)と並び・サイズを一致させること
        struct alignas(16) SWRasterConstants
        {
            DirectX::XMFLOAT4X4 ViewProj;
            // xy=レンダー解像度(画素)、zw=その逆数
            DirectX::XMFLOAT4 RenderSize;
            // xyz=太陽光が進む向き(正規化済み)、w=未使用
            DirectX::XMFLOAT4 SunDirection;
            // x=CSRasterのX方向グループ数(2D分解の復元用)、y=シーン全体の三角形数、
            // z=メッシュレコード数、w=巨大三角形とみなすbbox画素面積のしきい値
            DirectX::XMUINT4 DispatchParams;
            // x=巨大三角形リストの容量、yzw=未使用
            DirectX::XMUINT4 LargeParams;
        };

        // 自前ソフトウェアラスタライザが読むメッシュ1件ぶんの情報。
        // Shaders/3D/SoftwareRasterCommon.hlsliのSWRasterMeshInfoと並び・サイズを一致させること。
        //
        // 【構造化バッファは詰めて並ぶ】定数バッファと違いHLSLのStructuredBuffer<T>は
        // C++と同じ詰め方になるため、このままのレイアウトで一致する
        struct SWRasterMeshInfo
        {
            DirectX::XMFLOAT4X4 World;
            DirectX::XMFLOAT4X4 NormalMatrix;
            // 頂点/インデックスバッファのbindless番号(IRHIBuffer::GetBindlessIndex)
            uint32_t VertexBufferIndex;
            uint32_t IndexBufferIndex;
            // シーン全体の通し三角形番号における、このメッシュの先頭。シェーダー側の二分探索のキー
            uint32_t FirstTriangle;
            uint32_t TriangleCount;
            // ミラーリングされたインスタンス(ModelInstance::IsMirrored)なら-1。
            // 表裏判定の符号を反転させる
            float FrontFaceSign;
            // bit0 = アルファカットアウト(フェーズ2で使う。現在は常に0)
            uint32_t Flags;
            uint32_t Padding[2];
        };

        static_assert(sizeof(SWRasterMeshInfo) == 160, "HLSL側のSWRasterMeshInfoと一致させるため160バイト固定");

        // Assets::LightをGPU側のGPULightへ変換する。カンデラ/ルクスの測光量にEV100露出を直接掛けて
        // 表示レンジへ変換する(設計判断は「強度の単位」節を参照)。Frostbiteのスポット角度減衰用
        // lightAngleScale/lightAngleOffsetもここでCPU事前計算する
        GPULight MakeGPULight(const Assets::Light& light, float exposureEV100)
        {
            const float exposure = ComputeExposure(exposureEV100);
            const float radiance = light.Intensity * exposure;

            GPULight gpuLight{};
            gpuLight.PositionType = { light.Position[0], light.Position[1], light.Position[2], static_cast<float>(light.Type) };
            gpuLight.ColorRange = { light.Color[0] * radiance, light.Color[1] * radiance, light.Color[2] * radiance, light.Range };

            float angleScale = 0.0f;
            float angleOffset = 0.0f;
            if (light.Type == Assets::LightType::Spot)
            {
                // Frostbiteのスポット減衰式: t = saturate(dot(spotDir,-L)*scale + offset), atten = t*t
                const float cosOuter = std::cos(light.SpotOuterConeAngle);
                const float cosInner = std::cos(light.SpotInnerConeAngle);
                angleScale = 1.0f / std::max(0.001f, cosInner - cosOuter);
                angleOffset = -cosOuter * angleScale;
            }
            gpuLight.DirectionAngle = { light.Direction[0], light.Direction[1], light.Direction[2], angleScale };
            // Params.y = このライトが影を落とすか。ライトごとに切れるようにしてあるのは、
            // ピクセルあたりのシャドウレイ数に上限(LightingConstants.LightCount.y)があり、
            // 「影を出したいライト」に予算を回せるようにするため
            // Params.z = 光源そのものの半径[m]。0なら点光源。予約枠だった zw のうち z を使う。
            // 【平行光には入れない】太陽は MegaLights の対象外で、円盤サンプリングは
            // RTShadow.hlsl が別に持っている
            const float sourceRadius =
                (light.Type == Assets::LightType::Directional) ? 0.0f : std::max(0.0f, light.SourceRadius);
            // Params.y は影のフラグ。**bit0 = スクリーンスペースシャドウ / bit1 = レイトレース影レイ**
            // (Shaders/3D/LightAttenuation.hlsli と一致させること)。作者が置いたライトは
            // 両方を立てる ―― 1つの真偽値だった頃と挙動が変わらない。
            // 【リテラルで 3.0f と書かない】ビットの定義を変えたときに追随しない
            const float shadowFlags = light.CastShadow
                                          ? static_cast<float>(kLightShadowScreenSpace | kLightShadowRaytraced)
                                          : 0.0f;
            gpuLight.Params = { angleOffset, shadowFlags, sourceRadius, 0.0f };
            return gpuLight;
        }

        // エミッシブなメッシュから起こした光源プロキシを GPULight へ変換する。
        //
        // 【なぜ MakeGPULight と別関数なのか。そして exposure を引数に取らないのか】
        // ライトとエミッシブは単位系が違う。ライトの Intensity はカンデラで、
        // MakeGPULight が ComputeExposure(EV100) を掛けて表示空間へ持ち込む。
        // 一方エミッシブは GBuffer.hlsl が EmissiveFactor をそのまま G-Buffer へ書き、
        // DeferredLighting が**露出を通さずに**加算する(EV100 のツールチップ自身が
        // 「太陽・環境光・ポイント/スポットライト」にしか掛からないと書いている)。
        //
        // したがって面の測光輝度は L_v = E / exposure で、面積 A の放射強度は I = L_v * A。
        // これを GPULight へ入れるときに exposure を掛け直すと**約束どおり相殺して消える**:
        //
        //     ColorRange.rgb = I * exposure = (E / exposure) * A * exposure = E * A
        //
        // 露出が式から消えるので、自動露出が動いても TAA のプリ露出補正が入っても
        // プロキシと発光面の見た目の対応が崩れない。
        //
        // **MakeGPULight へ「Intensity = E*A/exposure」を渡す形にはしない。** 相殺に依存した
        // 割り算が2箇所へ散り、片方だけ直したときにコンパイルも通り絵も「それらしく」出る。
        // ここが exposure を受け取らないこと自体が、その事故を構造的に防いでいる。
        //
        // 【指向の分配はシェーダ側が持つ】I(θ) = I * [(1-κ)/4 + κ*max(0,cosθ)] の括弧の中は
        // LightAttenuation.hlsli の型3の枝にある。ここは向きによらない強さだけを入れる
        GPULight MakeGPULightFromEmissiveProxy(
            const Assets::EmissiveProxy& proxy, float emissiveIntensity, float cutoffIrradiance,
            float maxRange)
        {
            GPULight gpuLight{};

            const float intensity[3] = {
                proxy.RadianceBase[0] * emissiveIntensity * proxy.Area,
                proxy.RadianceBase[1] * emissiveIntensity * proxy.Area,
                proxy.RadianceBase[2] * emissiveIntensity * proxy.Area,
            };

            // Range は「最も強い向きでも打ち切り照度τまで落ちる距離」から解く。
            // 【上界の余弦ローブを使う】タイルカリング(LightAttenuationUpperBound)が同じ
            // 上界で判定するので、定義域を一致させないと届く灯を取りこぼす
            const float peak = std::max({ intensity[0], intensity[1], intensity[2] });
            const float lobeMax = (1.0f - proxy.Directionality) * 0.25f + proxy.Directionality;
            const float radiusSq = proxy.SourceRadius * proxy.SourceRadius;
            const float solved = peak * lobeMax / std::max(cutoffIrradiance, 1e-9f) - radiusSq;
            float range = (solved > 0.0f) ? std::sqrt(solved) : 0.0f;
            // 下限は 2R。プロキシが自分の発光体の広がりすら覆わないと、
            // 器具の筐体が真っ暗なまま光っている見た目になる
            range = std::max(range, 2.0f * proxy.SourceRadius);
            // 上限。自発光の強度を上げたときに Range が数kmまで伸びて、
            // タイルカリングが全タイルにヒットするのを止める安全弁
            if (maxRange > 0.0f)
            {
                range = std::min(range, maxRange);
            }

            gpuLight.PositionType = {
                proxy.Position[0], proxy.Position[1], proxy.Position[2],
                static_cast<float>(Assets::LightType::EmissiveProxy)
            };
            gpuLight.ColorRange = { intensity[0], intensity[1], intensity[2], range };
            // w(spotAngleScale)は使わない。xyz は発光面の平均法線で、余弦ローブの軸になる
            gpuLight.DirectionAngle = { proxy.Direction[0], proxy.Direction[1], proxy.Direction[2], 0.0f };
            // x=spotAngleOffset(未使用) / y=影のフラグ / z=面積等価の円板半径 / w=指向性κ
            //
            // 【レイトレース影レイだけを立てる】スクリーンスペースシャドウは画素あたりの
            // レイ数に上限(既定4灯)があり、プロキシは数百灯になりうる。両方立てると
            // プロキシが予算を食い尽くし、手置きライトの接触影が消える
            gpuLight.Params = {
                0.0f, static_cast<float>(kLightShadowRaytraced), proxy.SourceRadius, proxy.Directionality
            };
            return gpuLight;
        }

        // タンジェント空間(Z軸=法線方向)の半球状にランダムなカーネルサンプルを生成する。
        // John Chapmanのチュートリアルにならい、原点付近にサンプルが偏るようスケーリングして
        // 近距離のディテールを優先的に拾う
        std::vector<DirectX::XMFLOAT4> GenerateSSAOKernel(uint32_t kernelSize)
        {
            std::mt19937 rng(12345);
            std::uniform_real_distribution<float> dist(0.0f, 1.0f);

            std::vector<DirectX::XMFLOAT4> kernel;
            kernel.reserve(kernelSize);
            for (uint32_t i = 0; i < kernelSize; ++i)
            {
                DirectX::XMVECTOR sample = DirectX::XMVectorSet(
                    dist(rng) * 2.0f - 1.0f,
                    dist(rng) * 2.0f - 1.0f,
                    dist(rng),
                    0.0f);
                sample = DirectX::XMVector3Normalize(sample);
                sample = DirectX::XMVectorScale(sample, dist(rng));

                float scale = static_cast<float>(i) / static_cast<float>(kernelSize);
                scale = 0.1f + 0.9f * scale * scale;
                sample = DirectX::XMVectorScale(sample, scale);

                DirectX::XMFLOAT4 sampleF;
                DirectX::XMStoreFloat4(&sampleF, sample);
                sampleF.w = 0.0f;
                kernel.push_back(sampleF);
            }
            return kernel;
        }

        // レンダー解像度(renderWidth x renderHeight)のアスペクト比を保ったまま、
        // windowWidth x windowHeight の中央に収まるビューポート(レターボックス/ピラーボックス)を求める
        RHI::Viewport ComputeLetterboxViewport(uint32_t windowWidth, uint32_t windowHeight, uint32_t renderWidth, uint32_t renderHeight)
        {
            const float windowAspect = static_cast<float>(windowWidth) / static_cast<float>(windowHeight);
            const float renderAspect = static_cast<float>(renderWidth) / static_cast<float>(renderHeight);

            float viewportWidth;
            float viewportHeight;
            if (windowAspect > renderAspect)
            {
                // ウィンドウの方が横長 -> 高さいっぱいに合わせ、左右に余白(ピラーボックス)
                viewportHeight = static_cast<float>(windowHeight);
                viewportWidth = viewportHeight * renderAspect;
            }
            else
            {
                // ウィンドウの方が縦長 -> 幅いっぱいに合わせ、上下に余白(レターボックス)
                viewportWidth = static_cast<float>(windowWidth);
                viewportHeight = viewportWidth / renderAspect;
            }

            RHI::Viewport viewport;
            viewport.TopLeftX = (static_cast<float>(windowWidth) - viewportWidth) * 0.5f;
            viewport.TopLeftY = (static_cast<float>(windowHeight) - viewportHeight) * 0.5f;
            viewport.Width = viewportWidth;
            viewport.Height = viewportHeight;
            return viewport;
        }
    }

    KurenaiEngine3D::KurenaiEngine3D(
        GraphicsAPI api, uint32_t renderWidth, uint32_t renderHeight, size_t initialSceneIndex)
        : KurenaiEngineBase(L"Kurenai Engine", 1280, 720, api)
        , m_GraphicsAPI(api)
        , m_InitialSceneIndex(initialSceneIndex)
        , m_RenderWidth(std::max(1u, renderWidth))
        , m_RenderHeight(std::max(1u, renderHeight))
        // 超解像の出力解像度は、無効なうちは内部レンダー解像度と同じ意味を持つ。
        // ここを揃えておかないと、UIで初めて超解像を有効にした瞬間に
        // 出力解像度が既定値(1280x720)へ飛んでしまう
        , m_UpscaleOutputWidth(std::max(1u, renderWidth))
        , m_UpscaleOutputHeight(std::max(1u, renderHeight))
    {
        m_ImGuiBackend = m_Device->CreateImGuiBackend(m_Window->GetHandle());
        m_GPUProfiler = m_Device->CreateGPUProfiler();

        // imgui.iniの保存先を起動時の作業ディレクトリに依存させず、KurenaiEngine.dllと同じフォルダに固定する。
        // ImGuiはIniFilenameのポインタを保持するだけでコピーしないため、m_ImGuiIniPathで寿命を維持する
        m_ImGuiIniPath = WideToUtf8(GetModuleDirectory() + L"imgui.ini");
        ImGui::GetIO().IniFilename = m_ImGuiIniPath.c_str();

        // UIパネル群はImGuiコンテキストの生成後に作る(パネルの構築自体はImGuiを呼ばないが、
        // 以降の段階でスタイル・フォント設定をここへ足す前提で順序を固定しておく)
        m_UIManager = std::make_unique<UI::UIManager>(*this);

        // アスペクト比はm_RenderAspectを唯一の出所にする(解像度は実行時に変わるため)。
        // ここではまだUpdateスレッドが動いていないのでm_Cameraへ直接書いてよい
        m_RenderAspect.store(
            static_cast<float>(m_RenderWidth) / static_cast<float>(m_RenderHeight), std::memory_order_relaxed);
        m_Camera.SetAspectRatio(m_RenderAspect.load(std::memory_order_relaxed));

        CreateSceneResources();

        m_LastFrameTime = std::chrono::steady_clock::now();
    }

    KurenaiEngine3D::~KurenaiEngine3D()
    {
        // このクラスが持つGPUリソース(レンダーターゲット・G-Buffer・各種バッファ・
        // シーンのテクスチャ)を1つも壊す前に、GPUの実行完了を待つ。
        // 基底のKurenaiEngineBaseも待つが、そちらが走るのはこのクラスのメンバが
        // すべて破棄された後なので間に合わない(WaitForGPUIdleの宣言側コメント参照)。
        //
        // ここへ来る時点でRun()がRender/Loaderの両スレッドをjoin済みのため、
        // 待った後に新しいコマンドが積まれることはない
        WaitForGPUIdle();
    }

    void KurenaiEngine3D::CreateSceneResources()
    {
        // Shaders/AssetsはビルドでKurenaiEngine.dllと同じフォルダにコピーされる
        const std::wstring dataRoot = GetModuleDirectory();
        const std::wstring shaderDirectory = dataRoot + L"Shaders\\";

        const std::vector<RHI::InputElementDesc> modelInputLayout = GetModelInputLayout();

        // ジオメトリパス(G-Buffer書き込み)
        RHI::ShaderDesc gbufferVsDesc;
        gbufferVsDesc.Stage = RHI::ShaderStage::Vertex;
        gbufferVsDesc.FilePath = shaderDirectory + L"GBuffer.kshader";
        gbufferVsDesc.EntryPoint = "VSMain";
        m_GBufferVertexShader = m_Device->CreateShader(gbufferVsDesc);

        RHI::ShaderDesc gbufferPsDesc;
        gbufferPsDesc.Stage = RHI::ShaderStage::Pixel;
        gbufferPsDesc.FilePath = shaderDirectory + L"GBuffer.kshader";
        gbufferPsDesc.EntryPoint = "PSMain";
        m_GBufferPixelShader = m_Device->CreateShader(gbufferPsDesc);

        // 水面(ModelInstance::IsWater)専用のピクセルシェーダー(水面マテリアル基盤)。
        // 頂点シェーダーはWater.hlslもGBufferCommon.hlsli由来の同じVSMainを使うため、
        // m_GBufferVertexShaderをそのまま共有する(専用のVSは作らない)
        RHI::ShaderDesc gbufferWaterPsDesc;
        gbufferWaterPsDesc.Stage = RHI::ShaderStage::Pixel;
        gbufferWaterPsDesc.FilePath = shaderDirectory + L"Water.kshader";
        gbufferWaterPsDesc.EntryPoint = "PSMain";
        m_GBufferWaterPixelShader = m_Device->CreateShader(gbufferWaterPsDesc);

        // 深度プリパス(41.22節)のアルファカットアウト用。頂点シェーダーはG-Bufferと共有する
        // (プリパスとG-Bufferで深度が1ulpでもずれると面が消えるため。PSO作成側のコメント参照)
        try
        {
            RHI::ShaderDesc depthPrepassCutoutPsDesc;
            depthPrepassCutoutPsDesc.Stage = RHI::ShaderStage::Pixel;
            depthPrepassCutoutPsDesc.FilePath = shaderDirectory + L"DepthPrepass.kshader";
            depthPrepassCutoutPsDesc.EntryPoint = "PSMainCutout";
            m_DepthPrepassCutoutPixelShader = m_Device->CreateShader(depthPrepassCutoutPsDesc);
        }
        catch (const std::exception& e)
        {
            // 作れなくてもプリパス自体は成立する(カットアウトのメッシュをプリパスから
            // 除外して従来どおりG-Bufferだけで描く)ため、致命的とはしない
            m_DepthPrepassCutoutPixelShader.reset();
            Core::Logger::Error(
                "KurenaiEngine3D",
                std::string("深度プリパスのアルファカットアウト用ピクセルシェーダーの作成に失敗しました。"
                            "カットアウトのメッシュはプリパスから除外します: ") + e.what());
        }

        // メッシュシェーダー版のG-Bufferパス(GBufferMeshlet.hlsl)。
        // 対応環境でのみ作る ―― 非対応環境ではas/msプロファイルのコンパイル自体ができず、
        // 毎回エラーログが出てしまうため。ピクセルシェーダーはGBuffer.hlslのものを共有する
        // (メッシュレットのON/OFFで見た目が変わらないことがこのパスの前提)
        if (m_Device->SupportsMeshShader())
        {
            RHI::ShaderDesc gbufferAsDesc;
            gbufferAsDesc.Stage = RHI::ShaderStage::Amplification;
            gbufferAsDesc.FilePath = shaderDirectory + L"GBufferMeshlet.kshader";
            gbufferAsDesc.EntryPoint = "ASMain";
            m_GBufferAmplificationShader = m_Device->CreateShader(gbufferAsDesc);

            RHI::ShaderDesc gbufferMsDesc;
            gbufferMsDesc.Stage = RHI::ShaderStage::Mesh;
            gbufferMsDesc.FilePath = shaderDirectory + L"GBufferMeshlet.kshader";
            gbufferMsDesc.EntryPoint = "MSMain";
            m_GBufferMeshShader = m_Device->CreateShader(gbufferMsDesc);

            // メッシュレットごとに色分けするデバッグ表示用
            RHI::ShaderDesc gbufferMeshletDebugPsDesc;
            gbufferMeshletDebugPsDesc.Stage = RHI::ShaderStage::Pixel;
            // 実体はGBuffer.hlsl側(PSMainをそのまま呼んでアルベドだけ差し替えるため)
            gbufferMeshletDebugPsDesc.FilePath = shaderDirectory + L"GBuffer.kshader";
            gbufferMeshletDebugPsDesc.EntryPoint = "PSMainMeshletDebug";
            m_GBufferMeshletDebugPixelShader = m_Device->CreateShader(gbufferMeshletDebugPsDesc);

            // シャドウパスのメッシュシェーダー版。G-Buffer版と分けているのは、
            // シャドウのb0がFrameConstantsではなくCascadeConstantsで、cbufferの
            // レイアウトが違うため(ShadowMeshlet.hlsl冒頭のコメント参照)
            RHI::ShaderDesc shadowAsDesc;
            shadowAsDesc.Stage = RHI::ShaderStage::Amplification;
            shadowAsDesc.FilePath = shaderDirectory + L"ShadowMeshlet.kshader";
            shadowAsDesc.EntryPoint = "ASMain";
            m_ShadowAmplificationShader = m_Device->CreateShader(shadowAsDesc);

            RHI::ShaderDesc shadowMsDesc;
            shadowMsDesc.Stage = RHI::ShaderStage::Mesh;
            shadowMsDesc.FilePath = shaderDirectory + L"ShadowMeshlet.kshader";
            shadowMsDesc.EntryPoint = "MSMain";
            m_ShadowMeshShader = m_Device->CreateShader(shadowMsDesc);
        }

        // G-BufferのPSOはEmissiveのフォーマットがバッファ精度に依存するため、
        // この関数の末尾でCreatePrecisionDependentPipelineStates()がまとめて作る

        // 直接光パス(頂点バッファなしのフルスクリーン三角形。G-Buffer+シャドウマップからPBRの
        // 直接光を計算しHDRで書き出す)
        RHI::ShaderDesc directLightVsDesc;
        directLightVsDesc.Stage = RHI::ShaderStage::Vertex;
        directLightVsDesc.FilePath = shaderDirectory + L"DirectLighting.kshader";
        directLightVsDesc.EntryPoint = "VSMain";
        m_DirectLightVertexShader = m_Device->CreateShader(directLightVsDesc);

        RHI::ShaderDesc directLightPsDesc;
        directLightPsDesc.Stage = RHI::ShaderStage::Pixel;
        directLightPsDesc.FilePath = shaderDirectory + L"DirectLighting.kshader";
        directLightPsDesc.EntryPoint = "PSMain";
        m_DirectLightPixelShader = m_Device->CreateShader(directLightPsDesc);

        RHI::PipelineStateDesc directLightPipelineDesc;
        directLightPipelineDesc.VertexShader = m_DirectLightVertexShader.get();
        directLightPipelineDesc.PixelShader = m_DirectLightPixelShader.get();
        directLightPipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
        directLightPipelineDesc.RenderTargetFormats = { RHI::Format::R32G32B32A32_Float };
        m_DirectLightPipelineState = m_Device->CreatePipelineState(directLightPipelineDesc);

        // AO/GI共通の頂点シェーダ(頂点バッファなしのフルスクリーン三角形)。SSAO/SSIL/共通ブラーの
        // 3つのピクセルシェーダで使い回す
        RHI::ShaderDesc aoVsDesc;
        aoVsDesc.Stage = RHI::ShaderStage::Vertex;
        aoVsDesc.FilePath = shaderDirectory + L"SSAO.kshader";
        aoVsDesc.EntryPoint = "VSMain";
        m_AOVertexShader = m_Device->CreateShader(aoVsDesc);

        // SSAOパス
        RHI::ShaderDesc ssaoPsDesc;
        ssaoPsDesc.Stage = RHI::ShaderStage::Pixel;
        ssaoPsDesc.FilePath = shaderDirectory + L"SSAO.kshader";
        ssaoPsDesc.EntryPoint = "PSMain";
        m_SSAOPixelShader = m_Device->CreateShader(ssaoPsDesc);

        // SSAO/SSIL/AOブラーのPSOは出力先(AO/GIバッファ)のフォーマットがバッファ精度に依存するため、
        // この関数の末尾でCreatePrecisionDependentPipelineStates()がまとめて作る

        m_SSAOKernel = GenerateSSAOKernel(m_SSAOKernelSize);

        RHI::BufferDesc ssaoConstantBufferDesc;
        ssaoConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        ssaoConstantBufferDesc.SizeInBytes = sizeof(SSAOConstants);
        m_SSAOConstantBuffer = m_Device->CreateBuffer(ssaoConstantBufferDesc);

        // SSILパス(Visibility Bitmask)
        RHI::ShaderDesc ssilPsDesc;
        ssilPsDesc.Stage = RHI::ShaderStage::Pixel;
        ssilPsDesc.FilePath = shaderDirectory + L"SSIL_VisibilityBitmask.kshader";
        ssilPsDesc.EntryPoint = "PSMain";
        m_SSILPixelShader = m_Device->CreateShader(ssilPsDesc);

        RHI::BufferDesc ssilConstantBufferDesc;
        ssilConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        ssilConstantBufferDesc.SizeInBytes = sizeof(SSILConstants);
        m_SSILConstantBuffer = m_Device->CreateBuffer(ssilConstantBufferDesc);

        // AO/GI共通のブラーパス(SSAO.hlslのPSMainBlurを、rgbaフォーマットが同じSSAO/SSIL両方で使い回す)
        RHI::ShaderDesc aoBlurPsDesc;
        aoBlurPsDesc.Stage = RHI::ShaderStage::Pixel;
        aoBlurPsDesc.FilePath = shaderDirectory + L"SSAO.kshader";
        aoBlurPsDesc.EntryPoint = "PSMainBlur";
        m_AOBlurPixelShader = m_Device->CreateShader(aoBlurPsDesc);

        // AO/GI無効時はこの常に黒・不透明(遮蔽なし=a:1、間接光なし=rgb:0)のテクスチャをライティングパスに渡す
        m_AODisabledTexture = m_Device->CreateSolidColorTexture(0, 0, 0, 255);

        // ライティングパス(頂点バッファなしのフルスクリーン三角形)
        RHI::ShaderDesc lightingVsDesc;
        lightingVsDesc.Stage = RHI::ShaderStage::Vertex;
        lightingVsDesc.FilePath = shaderDirectory + L"DeferredLighting.kshader";
        lightingVsDesc.EntryPoint = "VSMain";
        m_LightingVertexShader = m_Device->CreateShader(lightingVsDesc);

        RHI::ShaderDesc lightingPsDesc;
        lightingPsDesc.Stage = RHI::ShaderStage::Pixel;
        lightingPsDesc.FilePath = shaderDirectory + L"DeferredLighting.kshader";
        lightingPsDesc.EntryPoint = "PSMain";
        m_LightingPixelShader = m_Device->CreateShader(lightingPsDesc);

        RHI::PipelineStateDesc lightingPipelineDesc;
        lightingPipelineDesc.VertexShader = m_LightingVertexShader.get();
        lightingPipelineDesc.PixelShader = m_LightingPixelShader.get();
        lightingPipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
        lightingPipelineDesc.RenderTargetFormats = { RHI::Format::R16G16B16A16_Float };
        m_LightingPipelineState = m_Device->CreatePipelineState(lightingPipelineDesc);

        // 半透明フォワードパス(Transparent.hlsl)。頂点入力・トポロジはGBufferパスと共通で、
        // 出力先はLightingパスと同じSceneColor(R16G16B16A16_Float)
        RHI::ShaderDesc transparentVsDesc;
        transparentVsDesc.Stage = RHI::ShaderStage::Vertex;
        transparentVsDesc.FilePath = shaderDirectory + L"Transparent.kshader";
        transparentVsDesc.EntryPoint = "VSMain";
        m_TransparentVertexShader = m_Device->CreateShader(transparentVsDesc);

        RHI::ShaderDesc transparentPsDesc;
        transparentPsDesc.Stage = RHI::ShaderStage::Pixel;
        transparentPsDesc.FilePath = shaderDirectory + L"Transparent.kshader";
        transparentPsDesc.EntryPoint = "PSMain";
        m_TransparentPixelShader = m_Device->CreateShader(transparentPsDesc);

        RHI::PipelineStateDesc transparentPipelineDesc;
        transparentPipelineDesc.InputLayout = modelInputLayout;
        transparentPipelineDesc.VertexShader = m_TransparentVertexShader.get();
        transparentPipelineDesc.PixelShader = m_TransparentPixelShader.get();
        transparentPipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
        transparentPipelineDesc.RenderTargetFormats = { RHI::Format::R16G16B16A16_Float };
        transparentPipelineDesc.HasDepthStencil = true;
        // 既存の不透明物体には隠れさせたいが(テストは有効)、奥から手前に描く半透明同士が互いの深度で
        // 隠し合わないよう書き込みは行わない
        transparentPipelineDesc.DepthWriteEnabled = false;
        transparentPipelineDesc.ReverseZ = true;
        // 事前乗算済みアルファ(src.rgb + dst.rgb * (1 - src.a))。標準アルファブレンドではなく
        // こちらを使うのは、ガラスの鏡面反射(スペキュラ)を不透明度で減衰させないため。
        // 標準アルファブレンドはシェーダーの出力色全体にsrc.aを掛けるので、Bistroの酒瓶のように
        // 不透明度が0.04しかないマテリアルではハイライトまで1/25に潰れ、ガラスが「透明」ではなく
        // 「何も無い」ように見えてしまう。Transparent.hlsl側で拡散光にのみ不透明度を乗じ、
        // 鏡面反射は減衰させずに加算した色を出力する(詳細はdocs/Architecture.htmlの半透明描画の章を参照)
        transparentPipelineDesc.BlendMode = RHI::BlendMode::PremultipliedAlpha;
        m_TransparentPipelineState = m_Device->CreatePipelineState(transparentPipelineDesc);
        transparentPipelineDesc.FrontCounterClockwise = true;
        m_TransparentPipelineStateMirrored = m_Device->CreatePipelineState(transparentPipelineDesc);

        // ドローンショーパス(DroneShow.hlsl)。頂点バッファを持たず、Draw(6*機体数, 0)と
        // SV_VertexIDでビルボードのクアッドを展開する(InputLayoutは空のまま)
        RHI::ShaderDesc droneShowVsDesc;
        droneShowVsDesc.Stage = RHI::ShaderStage::Vertex;
        droneShowVsDesc.FilePath = shaderDirectory + L"DroneShow.kshader";
        droneShowVsDesc.EntryPoint = "VSMain";
        m_DroneShowVertexShader = m_Device->CreateShader(droneShowVsDesc);

        RHI::ShaderDesc droneShowPsDesc;
        droneShowPsDesc.Stage = RHI::ShaderStage::Pixel;
        droneShowPsDesc.FilePath = shaderDirectory + L"DroneShow.kshader";
        droneShowPsDesc.EntryPoint = "PSMain";
        m_DroneShowPixelShader = m_Device->CreateShader(droneShowPsDesc);

        RHI::PipelineStateDesc droneShowPipelineDesc;
        droneShowPipelineDesc.VertexShader = m_DroneShowVertexShader.get();
        droneShowPipelineDesc.PixelShader = m_DroneShowPixelShader.get();
        droneShowPipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
        // SceneColorと平面反射(m_PlanarReflectionColor)はどちらもR16G16B16A16_Floatなので、
        // 同じPSOを両方のパスで使える
        droneShowPipelineDesc.RenderTargetFormats = { RHI::Format::R16G16B16A16_Float };
        // 島や地形の後ろに回った機体を隠すため深度テストは行うが、
        // 機体同士は隠し合わせない(加算合成は順序に依存しないのでソートも不要)
        droneShowPipelineDesc.HasDepthStencil = true;
        droneShowPipelineDesc.DepthWriteEnabled = false;
        droneShowPipelineDesc.ReverseZ = true;
        // 【Additiveではなくこちらを使う理由 ― アルファ(カバレッジ)を書かないため】
        // Additiveは SrcBlendAlpha=ONE / DestBlendAlpha=ONE なので、機体を描くたびに
        // レンダーターゲットのアルファへ1.0が積まれる。SceneColorではアルファを誰も読まないので
        // 実害が無いが、平面反射(m_PlanarReflectionColor)ではアルファが
        // 「そのテクセルにジオメトリが描かれたか」のカバレッジとして使われており
        // (SSR.hlslのApplyPlanarReflection)、機体のクアッド全域でカバレッジが1になってしまう。
        // すると水面はクアッドの円の内側で解析空の映り込みを失い、裾(glowがほぼ0の外周)が
        // 黒い円として抜ける。機体が重なるとアルファは1を超え、解析空の係数(1-a)が負へ振れる。
        // PremultipliedAlphaは SrcBlend=ONE / DestBlend=INV_SRC_ALPHA なので、
        // PSMainがアルファ0を返せば rgb=src+dst(加算合成のまま)・alpha=dst(据え置き)になり、
        // 「光は足すが遮蔽はしない」という発光点の正しい意味になる
        droneShowPipelineDesc.BlendMode = RHI::BlendMode::PremultipliedAlpha;
        // 【平面反射用にワインディングを反転したPSOは要らない】
        // メッシュの描画(m_TransparentPipelineStateMirrored等)では鏡映ビュー行列が頂点そのものを
        // 変換するため画面上の巻きが反転するが、このパスのビルボードは
        // 「ワールド座標をViewで変換した"後"に、ビュー空間で四隅のオフセットを足す」
        // という作り方をしている(DroneShow.hlslのVSMain)。四隅のオフセットは鏡映行列を
        // 一度も通らないので、Viewが鏡映を含んでいてもクアッド自身の巻きは変わらない。
        // 反転したPSOで描くと1機残らず裏面として捨てられ、水面に何も映らなくなる
        m_DroneShowPipelineState = m_Device->CreatePipelineState(droneShowPipelineDesc);

        RHI::BufferDesc droneShowConstantBufferDesc;
        droneShowConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        droneShowConstantBufferDesc.SizeInBytes = sizeof(DroneShowConstants);
        m_DroneShowConstantBuffer = m_Device->CreateBuffer(droneShowConstantBufferDesc);

        RHI::BufferDesc droneBufferDesc;
        droneBufferDesc.Usage = RHI::BufferUsage::StructuredReadOnly;
        droneBufferDesc.SizeInBytes = sizeof(GPUDrone) * kMaxDrones;
        droneBufferDesc.StrideInBytes = sizeof(GPUDrone);
        m_DroneBuffer = m_Device->CreateBuffer(droneBufferDesc);

        // Hi-Zミップチェーン構築パス(コンピュートシェーダー)。CSCopyでG-Buffer深度をミップ0へコピーし、
        // CSDownsampleをミップ数-1回ディスパッチして1x1まで縮小する
        RHI::ShaderDesc hizCopyCsDesc;
        hizCopyCsDesc.Stage = RHI::ShaderStage::Compute;
        hizCopyCsDesc.FilePath = shaderDirectory + L"HiZ.kshader";
        hizCopyCsDesc.EntryPoint = "CSCopy";
        m_HiZCopyComputeShader = m_Device->CreateShader(hizCopyCsDesc);
        m_HiZCopyPipelineState = m_Device->CreateComputePipelineState({ m_HiZCopyComputeShader.get() });

        RHI::ShaderDesc hizDownsampleCsDesc;
        hizDownsampleCsDesc.Stage = RHI::ShaderStage::Compute;
        hizDownsampleCsDesc.FilePath = shaderDirectory + L"HiZ.kshader";
        hizDownsampleCsDesc.EntryPoint = "CSDownsample";
        m_HiZDownsampleComputeShader = m_Device->CreateShader(hizDownsampleCsDesc);
        m_HiZDownsamplePipelineState = m_Device->CreateComputePipelineState({ m_HiZDownsampleComputeShader.get() });

        RHI::BufferDesc hizConstantBufferDesc;
        hizConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        hizConstantBufferDesc.SizeInBytes = sizeof(HiZConstants);
        m_HiZConstantBuffer = m_Device->CreateBuffer(hizConstantBufferDesc);

        // タイルライトカリングパス(コンピュートシェーダー)。タイルごとに届くライトのインデックスリストを作る。
        // ライトグリッド本体(m_LightTileBuffer)は解像度に依存するためCreateRenderTargetsで作る
        RHI::ShaderDesc lightCullingCsDesc;
        lightCullingCsDesc.Stage = RHI::ShaderStage::Compute;
        lightCullingCsDesc.FilePath = shaderDirectory + L"LightCulling.kshader";
        lightCullingCsDesc.EntryPoint = "CSMain";
        m_LightCullingComputeShader = m_Device->CreateShader(lightCullingCsDesc);
        m_LightCullingPipelineState = m_Device->CreateComputePipelineState({ m_LightCullingComputeShader.get() });

        RHI::BufferDesc lightCullingConstantBufferDesc;
        lightCullingConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        lightCullingConstantBufferDesc.SizeInBytes = sizeof(LightCullingConstants);
        m_LightCullingConstantBuffer = m_Device->CreateBuffer(lightCullingConstantBufferDesc);

        // 自前ソフトウェアラスタライザ(46章)。比較用の独立した経路で、既存の描画には寄与しない。
        // 解像度に依存するリソース(visibility buffer・出力テクスチャ)はCreateRenderTargetsで作る。
        //
        // 【失敗しても他を巻き込まない】シェーダーはSM 6.6の64bitアトミックとbindlessを使うため、
        // デバイス判定を通っていてもコンパイル環境によっては落ち得る。ここで捕まえて
        // 機能だけ無効化する(レイトレーシングと同じ扱い)
        if (m_Device->SupportsSoftwareRaster())
        {
            try
            {
                RHI::ShaderDesc swRasterCsDesc;
                swRasterCsDesc.Stage = RHI::ShaderStage::Compute;
                swRasterCsDesc.FilePath = shaderDirectory + L"SoftwareRaster.kshader";
                swRasterCsDesc.EntryPoint = "CSRaster";
                m_SoftwareRasterComputeShader = m_Device->CreateShader(swRasterCsDesc);

                RHI::ShaderDesc swRasterLargeCsDesc;
                swRasterLargeCsDesc.Stage = RHI::ShaderStage::Compute;
                swRasterLargeCsDesc.FilePath = shaderDirectory + L"SoftwareRaster.kshader";
                swRasterLargeCsDesc.EntryPoint = "CSRasterLarge";
                m_SoftwareRasterLargeComputeShader = m_Device->CreateShader(swRasterLargeCsDesc);

                RHI::ShaderDesc swRasterResolveCsDesc;
                swRasterResolveCsDesc.Stage = RHI::ShaderStage::Compute;
                swRasterResolveCsDesc.FilePath = shaderDirectory + L"SoftwareRasterResolve.kshader";
                swRasterResolveCsDesc.EntryPoint = "CSResolve";
                m_SoftwareRasterResolveComputeShader = m_Device->CreateShader(swRasterResolveCsDesc);

                m_SoftwareRasterPipelineState =
                    m_Device->CreateComputePipelineState({ m_SoftwareRasterComputeShader.get() });
                m_SoftwareRasterLargePipelineState =
                    m_Device->CreateComputePipelineState({ m_SoftwareRasterLargeComputeShader.get() });
                m_SoftwareRasterResolvePipelineState =
                    m_Device->CreateComputePipelineState({ m_SoftwareRasterResolveComputeShader.get() });

                RHI::BufferDesc swRasterConstantBufferDesc;
                swRasterConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
                swRasterConstantBufferDesc.SizeInBytes = sizeof(SWRasterConstants);
                m_SoftwareRasterConstantBuffer = m_Device->CreateBuffer(swRasterConstantBufferDesc);

                // メッシュレコード。毎フレームCPUから書き直すためStructuredReadOnly
                RHI::BufferDesc swRasterMeshInfoDesc;
                swRasterMeshInfoDesc.Usage = RHI::BufferUsage::StructuredReadOnly;
                swRasterMeshInfoDesc.SizeInBytes =
                    static_cast<uint32_t>(sizeof(SWRasterMeshInfo)) * kSWRasterMaxMeshes;
                swRasterMeshInfoDesc.StrideInBytes = static_cast<uint32_t>(sizeof(SWRasterMeshInfo));
                m_SoftwareRasterMeshInfoBuffer = m_Device->CreateBuffer(swRasterMeshInfoDesc);

                // 巨大三角形リスト。CSRasterがUAVで書き、CSRasterLargeがSRVで読むためStructuredRW
                RHI::BufferDesc swRasterLargeEntriesDesc;
                swRasterLargeEntriesDesc.Usage = RHI::BufferUsage::StructuredRW;
                swRasterLargeEntriesDesc.SizeInBytes =
                    static_cast<uint32_t>(sizeof(uint32_t)) * kSWRasterLargeListCapacity;
                swRasterLargeEntriesDesc.StrideInBytes = static_cast<uint32_t>(sizeof(uint32_t));
                m_SoftwareRasterLargeEntriesBuffer = m_Device->CreateBuffer(swRasterLargeEntriesDesc);

                // 間接ディスパッチ引数(uint3)。16バイトにしているのは4の倍数の要件と
                // アライメントを揃えるためで、実際に使うのは先頭12バイト
                RHI::BufferDesc swRasterIndirectArgsDesc;
                swRasterIndirectArgsDesc.Usage = RHI::BufferUsage::IndirectArgs;
                swRasterIndirectArgsDesc.SizeInBytes = 16;
                swRasterIndirectArgsDesc.StrideInBytes = static_cast<uint32_t>(sizeof(uint32_t));
                m_SoftwareRasterIndirectArgsBuffer = m_Device->CreateBuffer(swRasterIndirectArgsDesc);

                m_SoftwareRasterAvailable = true;
            }
            catch (const std::exception& e)
            {
                Core::Logger::Warning(
                    "KurenaiEngine3D",
                    std::string("ソフトウェアラスタライザの初期化に失敗したため無効にします: ") + e.what());
                m_SoftwareRasterAvailable = false;
                m_SoftwareRasterComputeShader.reset();
                m_SoftwareRasterLargeComputeShader.reset();
                m_SoftwareRasterResolveComputeShader.reset();
                m_SoftwareRasterPipelineState.reset();
                m_SoftwareRasterLargePipelineState.reset();
                m_SoftwareRasterResolvePipelineState.reset();
                m_SoftwareRasterConstantBuffer.reset();
                m_SoftwareRasterMeshInfoBuffer.reset();
                m_SoftwareRasterLargeEntriesBuffer.reset();
                m_SoftwareRasterIndirectArgsBuffer.reset();
            }
        }
        else
        {
            Core::Logger::Info(
                "KurenaiEngine3D",
                "ソフトウェアラスタライザは利用できません(DX12・シェーダーモデル6.6・"
                "64bit整数アトミック・bindlessのすべてが必要です)");
        }

        // SSRパス(頂点バッファなしのフルスクリーン三角形。SceneColorとG-Bufferから鏡面反射を計算し加算する)
        RHI::ShaderDesc ssrVsDesc;
        ssrVsDesc.Stage = RHI::ShaderStage::Vertex;
        ssrVsDesc.FilePath = shaderDirectory + L"SSR.kshader";
        ssrVsDesc.EntryPoint = "VSMain";
        m_SSRVertexShader = m_Device->CreateShader(ssrVsDesc);

        RHI::ShaderDesc ssrPsDesc;
        ssrPsDesc.Stage = RHI::ShaderStage::Pixel;
        ssrPsDesc.FilePath = shaderDirectory + L"SSR.kshader";
        ssrPsDesc.EntryPoint = "PSMain";
        m_SSRPixelShader = m_Device->CreateShader(ssrPsDesc);

        RHI::PipelineStateDesc ssrPipelineDesc;
        ssrPipelineDesc.VertexShader = m_SSRVertexShader.get();
        ssrPipelineDesc.PixelShader = m_SSRPixelShader.get();
        ssrPipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
        ssrPipelineDesc.RenderTargetFormats = { RHI::Format::R16G16B16A16_Float };
        m_SSRPipelineState = m_Device->CreatePipelineState(ssrPipelineDesc);

        RHI::BufferDesc ssrConstantBufferDesc;
        ssrConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        ssrConstantBufferDesc.SizeInBytes = sizeof(SSRConstants);
        m_SSRConstantBuffer = m_Device->CreateBuffer(ssrConstantBufferDesc);

        // 大気遠近パス(頂点バッファなしのフルスクリーン三角形。反射パスの出力とG-Buffer深度から
        // フォグを合成する)。専用のb1定数バッファは持たない(パラメータはFrameConstants末尾の
        // FogParams0/1に入れているため。AerialPerspective.hlsl冒頭参照)
        RHI::ShaderDesc aerialPerspectiveVsDesc;
        aerialPerspectiveVsDesc.Stage = RHI::ShaderStage::Vertex;
        aerialPerspectiveVsDesc.FilePath = shaderDirectory + L"AerialPerspective.kshader";
        aerialPerspectiveVsDesc.EntryPoint = "VSMain";
        m_AerialPerspectiveVertexShader = m_Device->CreateShader(aerialPerspectiveVsDesc);

        RHI::ShaderDesc aerialPerspectivePsDesc;
        aerialPerspectivePsDesc.Stage = RHI::ShaderStage::Pixel;
        aerialPerspectivePsDesc.FilePath = shaderDirectory + L"AerialPerspective.kshader";
        aerialPerspectivePsDesc.EntryPoint = "PSMain";
        m_AerialPerspectivePixelShader = m_Device->CreateShader(aerialPerspectivePsDesc);

        RHI::PipelineStateDesc aerialPerspectivePipelineDesc;
        aerialPerspectivePipelineDesc.VertexShader = m_AerialPerspectiveVertexShader.get();
        aerialPerspectivePipelineDesc.PixelShader = m_AerialPerspectivePixelShader.get();
        aerialPerspectivePipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
        aerialPerspectivePipelineDesc.RenderTargetFormats = { RHI::Format::R16G16B16A16_Float };
        m_AerialPerspectivePipelineState = m_Device->CreatePipelineState(aerialPerspectivePipelineDesc);

        // 雲パス(頂点バッファなしのフルスクリーン三角形。積雲と巻雲だけを1/2解像度で評価し、
        // 透過率と事前乗算済みの散乱光を書く)。専用のb1定数バッファは持たない
        // (パラメータはFrameConstants末尾のCloudParams0-3等に入っているため)
        RHI::ShaderDesc skyCloudVsDesc;
        skyCloudVsDesc.Stage = RHI::ShaderStage::Vertex;
        skyCloudVsDesc.FilePath = shaderDirectory + L"SkyCloud.kshader";
        skyCloudVsDesc.EntryPoint = "VSMain";
        m_SkyCloudVertexShader = m_Device->CreateShader(skyCloudVsDesc);

        RHI::ShaderDesc skyCloudPsDesc;
        skyCloudPsDesc.Stage = RHI::ShaderStage::Pixel;
        skyCloudPsDesc.FilePath = shaderDirectory + L"SkyCloud.kshader";
        skyCloudPsDesc.EntryPoint = "PSMain";
        m_SkyCloudPixelShader = m_Device->CreateShader(skyCloudPsDesc);

        RHI::PipelineStateDesc skyCloudPipelineDesc;
        skyCloudPipelineDesc.VertexShader = m_SkyCloudVertexShader.get();
        skyCloudPipelineDesc.PixelShader = m_SkyCloudPixelShader.get();
        skyCloudPipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
        skyCloudPipelineDesc.RenderTargetFormats = { RHI::Format::R16G16B16A16_Float };
        m_SkyCloudPipelineState = m_Device->CreatePipelineState(skyCloudPipelineDesc);

        // DDGIの低解像度解決パス(雲パスと同じ作り。拡散イラディアンスとinsideWeightを書く)
        RHI::ShaderDesc ddgiResolveVsDesc;
        ddgiResolveVsDesc.Stage = RHI::ShaderStage::Vertex;
        ddgiResolveVsDesc.FilePath = shaderDirectory + L"DDGIResolve.kshader";
        ddgiResolveVsDesc.EntryPoint = "VSMain";
        m_DDGIResolveVertexShader = m_Device->CreateShader(ddgiResolveVsDesc);

        RHI::ShaderDesc ddgiResolvePsDesc;
        ddgiResolvePsDesc.Stage = RHI::ShaderStage::Pixel;
        ddgiResolvePsDesc.FilePath = shaderDirectory + L"DDGIResolve.kshader";
        ddgiResolvePsDesc.EntryPoint = "PSMain";
        m_DDGIResolvePixelShader = m_Device->CreateShader(ddgiResolvePsDesc);

        RHI::PipelineStateDesc ddgiResolvePipelineDesc;
        ddgiResolvePipelineDesc.VertexShader = m_DDGIResolveVertexShader.get();
        ddgiResolvePipelineDesc.PixelShader = m_DDGIResolvePixelShader.get();
        ddgiResolvePipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
        // 2枚目はこのテクセルが代表している全解像度の深度(41.24節)。並びはDDGIResolve.hlslの
        // PSOutputおよびDDGIResolveパスのRenderTargetsと一致させること。
        // Reverse-Zの生値をそのまま持つのでR32_Float(合成側の相対差の判定に十分な精度が要る)
        ddgiResolvePipelineDesc.RenderTargetFormats = {
            RHI::Format::R16G16B16A16_Float,
            RHI::Format::R32_Float,
        };
        m_DDGIResolvePipelineState = m_Device->CreatePipelineState(ddgiResolvePipelineDesc);

        // RT反射パス(コンピュートシェーダー。TLASへ鏡面レイを撃ち反射色を求める)。
        // RTReflection.hlslはRayQueryを含むためシェーダーモデル6.5でしかコンパイルできない。
        // 非対応環境ではシェーダー自体を作らず、UIからもRaytracedを選べないようにする
        m_RaytracingAvailable = m_Device->SupportsRaytracing();
        // メッシュシェーダーの可否もここで控える(UIパネルが参照する)
        m_MeshShaderAvailable = m_Device->SupportsMeshShader();
        // bindless区画の容量も同じ理由でここへ控える(使用数はフレームごとに更新する)
        m_BindlessCapacity = m_Device->GetBindlessCapacity();

        // メッシュレットカリングの統計(Stage 5-2)。増幅シェーダーがカウンタへ数え上げ、
        // それを数フレーム遅れでCPUへ読み戻してPerfログへ出す。
        // 増幅シェーダーが走らない環境では一切使わないので、そもそも作らない
        if (m_MeshShaderAvailable)
        {
            try
            {
                RHI::BufferDesc cullStatsDesc;
                cullStatsDesc.Usage = RHI::BufferUsage::Structured;
                cullStatsDesc.SizeInBytes = static_cast<uint32_t>(sizeof(uint32_t)) * kMeshletCullStatsCount;
                cullStatsDesc.StrideInBytes = static_cast<uint32_t>(sizeof(uint32_t));
                m_MeshletCullStatsBuffer = m_Device->CreateBuffer(cullStatsDesc);

                // 【SRVではなくUAVを登録する】増幅シェーダーは読むのではなく書く。
                // RegisterBindless(SRV)の番号を渡すと読み取り専用のビューへ書き込むことになる
                m_MeshletCullStatsBindlessIndex = m_Device->RegisterBindlessUAV(m_MeshletCullStatsBuffer.get());
                if (m_MeshletCullStatsBindlessIndex == RHI::kInvalidBindlessIndex)
                {
                    Core::Logger::Warning(
                        "KurenaiEngine3D",
                        "メッシュレットカリングの統計バッファをbindlessへ登録できませんでした(統計を無効にします)");
                    m_MeshletCullStatsBuffer.reset();
                }
                else
                {
                    for (uint32_t i = 0; i < kMeshletCullStatsRingSize; ++i)
                    {
                        RHI::BufferDesc readbackDesc;
                        readbackDesc.Usage = RHI::BufferUsage::Readback;
                        readbackDesc.SizeInBytes = cullStatsDesc.SizeInBytes;
                        readbackDesc.StrideInBytes = cullStatsDesc.StrideInBytes;
                        m_MeshletCullStatsReadback[i] = m_Device->CreateBuffer(readbackDesc);
                    }
                }
            }
            catch (const std::exception& e)
            {
                // 統計が作れないだけで描画は成立する。カリング本体は止めない
                Core::Logger::Warning(
                    "KurenaiEngine3D",
                    std::string("メッシュレットカリングの統計の初期化に失敗したため無効にします: ") + e.what());
                m_MeshletCullStatsBuffer.reset();
                for (auto& readback : m_MeshletCullStatsReadback)
                {
                    readback.reset();
                }
                m_MeshletCullStatsBindlessIndex = RHI::kInvalidBindlessIndex;
            }
        }

        // モデル単位のGPUカリング(Stage 5-3)。判定はコンピュートシェーダーで行い、
        // 生き残りの DispatchMesh 引数と統計をGPU上に作る。
        //
        // 【メッシュレット経路が使えるときだけ作る】判定結果の行き先(ExecuteIndirect)も、
        // 判定に使うHi-Zも、メッシュシェーダー経路の話でしか意味を持たない
        if (m_MeshShaderAvailable)
        {
            try
            {
                RHI::ShaderDesc modelCullCsDesc;
                modelCullCsDesc.Stage = RHI::ShaderStage::Compute;
                modelCullCsDesc.FilePath = shaderDirectory + L"ModelCull.kshader";
                modelCullCsDesc.EntryPoint = "CSMain";
                m_ModelCullComputeShader = m_Device->CreateShader(modelCullCsDesc);
                m_ModelCullPipelineState =
                    m_Device->CreateComputePipelineState({ m_ModelCullComputeShader.get() });

                RHI::BufferDesc modelCullConstantDesc;
                modelCullConstantDesc.Usage = RHI::BufferUsage::Constant;
                modelCullConstantDesc.SizeInBytes = sizeof(ModelCullConstants);
                m_ModelCullConstantBuffer = m_Device->CreateBuffer(modelCullConstantDesc);

                RHI::BufferDesc modelCullCounterDesc;
                modelCullCounterDesc.Usage = RHI::BufferUsage::Structured;
                modelCullCounterDesc.SizeInBytes =
                    static_cast<uint32_t>(sizeof(uint32_t)) * kModelCullCounterCount;
                modelCullCounterDesc.StrideInBytes = static_cast<uint32_t>(sizeof(uint32_t));
                m_ModelCullCounterBuffer = m_Device->CreateBuffer(modelCullCounterDesc);

                for (uint32_t i = 0; i < kMeshletCullStatsRingSize; ++i)
                {
                    RHI::BufferDesc readbackDesc;
                    readbackDesc.Usage = RHI::BufferUsage::Readback;
                    readbackDesc.SizeInBytes = modelCullCounterDesc.SizeInBytes;
                    readbackDesc.StrideInBytes = modelCullCounterDesc.StrideInBytes;
                    m_ModelCullReadback[i] = m_Device->CreateBuffer(readbackDesc);
                }
            }
            catch (const std::exception& e)
            {
                // カリングが作れないだけで描画は成立する(CPU側のループがそのまま描く)
                Core::Logger::Warning(
                    "KurenaiEngine3D",
                    std::string("モデル単位のGPUカリングの初期化に失敗したため無効にします: ") + e.what());
                m_ModelCullComputeShader.reset();
                m_ModelCullPipelineState.reset();
                m_ModelCullConstantBuffer.reset();
                m_ModelCullCounterBuffer.reset();
                for (auto& readback : m_ModelCullReadback)
                {
                    readback.reset();
                }
            }
        }

        if (m_RaytracingAvailable)
        {
            RHI::ShaderDesc rtReflectionCsDesc;
            rtReflectionCsDesc.Stage = RHI::ShaderStage::Compute;
            rtReflectionCsDesc.FilePath = shaderDirectory + L"RTReflection.kshader";
            rtReflectionCsDesc.EntryPoint = "CSMain";
            m_RTReflectionComputeShader = m_Device->CreateShader(rtReflectionCsDesc);
            m_RTReflectionPipelineState = m_Device->CreateComputePipelineState({ m_RTReflectionComputeShader.get() });

            RHI::BufferDesc rtReflectionConstantBufferDesc;
            rtReflectionConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
            rtReflectionConstantBufferDesc.SizeInBytes = sizeof(RTReflectionConstants);
            m_RTReflectionConstantBuffer = m_Device->CreateBuffer(rtReflectionConstantBufferDesc);

            // RTシャドウパス(コンピュートシェーダー。TLASへ太陽の円盤方向の影レイを撃ち可視率を求める)。
            // RTReflectionと同じくRayQueryを含むためシェーダーモデル6.5が必要
            RHI::ShaderDesc rtShadowCsDesc;
            rtShadowCsDesc.Stage = RHI::ShaderStage::Compute;
            rtShadowCsDesc.FilePath = shaderDirectory + L"RTShadow.kshader";
            rtShadowCsDesc.EntryPoint = "CSMain";
            m_RTShadowComputeShader = m_Device->CreateShader(rtShadowCsDesc);
            m_RTShadowPipelineState = m_Device->CreateComputePipelineState({ m_RTShadowComputeShader.get() });

            RHI::BufferDesc rtShadowConstantBufferDesc;
            rtShadowConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
            rtShadowConstantBufferDesc.SizeInBytes = sizeof(RTShadowConstants);
            m_RTShadowConstantBuffer = m_Device->CreateBuffer(rtShadowConstantBufferDesc);

            // MegaLightsの参照実装(コンピュートシェーダー。ポイント/スポットライトを全灯
            // 総当たりし、届いた1灯ごとに光源までの影レイを撃つ)。以降の確率的サンプリングを
            // 評価するときの真値を作るためのパスで、RayQueryを含むためシェーダーモデル6.5が要る
            RHI::ShaderDesc megaLightsRefCsDesc;
            megaLightsRefCsDesc.Stage = RHI::ShaderStage::Compute;
            megaLightsRefCsDesc.FilePath = shaderDirectory + L"MegaLightsReference.kshader";
            megaLightsRefCsDesc.EntryPoint = "CSMain";
            m_MegaLightsReferenceComputeShader = m_Device->CreateShader(megaLightsRefCsDesc);
            m_MegaLightsReferencePipelineState =
                m_Device->CreateComputePipelineState({ m_MegaLightsReferenceComputeShader.get() });

            RHI::BufferDesc megaLightsConstantBufferDesc;
            megaLightsConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
            megaLightsConstantBufferDesc.SizeInBytes = sizeof(MegaLightsConstants);
            m_MegaLightsConstantBuffer = m_Device->CreateBuffer(megaLightsConstantBufferDesc);

            // MegaLightsの候補プール(コンピュートシェーダー。タイルごとに届くライトを走査して
            // 重みつきでK灯を抽出する)。レイを撃たないのでRayQueryは要らないが、
            // MegaLightsと同時にしか使わないためここで一緒に作る
            RHI::ShaderDesc megaLightsTilePoolCsDesc;
            megaLightsTilePoolCsDesc.Stage = RHI::ShaderStage::Compute;
            megaLightsTilePoolCsDesc.FilePath = shaderDirectory + L"MegaLightsTilePool.kshader";
            megaLightsTilePoolCsDesc.EntryPoint = "CSMain";
            m_MegaLightsTilePoolComputeShader = m_Device->CreateShader(megaLightsTilePoolCsDesc);
            m_MegaLightsTilePoolPipelineState =
                m_Device->CreateComputePipelineState({ m_MegaLightsTilePoolComputeShader.get() });

            RHI::BufferDesc megaLightsTilePoolConstantBufferDesc;
            megaLightsTilePoolConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
            megaLightsTilePoolConstantBufferDesc.SizeInBytes = sizeof(MegaLightsTilePoolConstants);
            m_MegaLightsTilePoolConstantBuffer = m_Device->CreateBuffer(megaLightsTilePoolConstantBufferDesc);

            // MegaLightsの確率的サンプリング本体(2パス)。
            // 【この4本はすべて RayQuery を含む】Initial は初期可視レイ、Temporal は
            // 時間検証レイ、Spatial は目標関数の可視性とバイアス補正レイ、Shade は影レイ。
            // したがってシェーダーモデル6.5が要る(パッカーの kSkipDxbc50Files を参照)。
            // レイを撃たないのは TilePool / Denoise / Accum / Resolve の4本だけで、
            // そちらは3バリアントすべてで焼かれる
            RHI::ShaderDesc megaLightsInitialCsDesc;
            megaLightsInitialCsDesc.Stage = RHI::ShaderStage::Compute;
            megaLightsInitialCsDesc.FilePath = shaderDirectory + L"MegaLightsInitialSample.kshader";
            megaLightsInitialCsDesc.EntryPoint = "CSMain";
            m_MegaLightsInitialComputeShader = m_Device->CreateShader(megaLightsInitialCsDesc);
            m_MegaLightsInitialPipelineState =
                m_Device->CreateComputePipelineState({ m_MegaLightsInitialComputeShader.get() });

            RHI::ShaderDesc megaLightsShadeCsDesc;
            megaLightsShadeCsDesc.Stage = RHI::ShaderStage::Compute;
            megaLightsShadeCsDesc.FilePath = shaderDirectory + L"MegaLightsShade.kshader";
            megaLightsShadeCsDesc.EntryPoint = "CSMain";
            m_MegaLightsShadeComputeShader = m_Device->CreateShader(megaLightsShadeCsDesc);
            m_MegaLightsShadePipelineState =
                m_Device->CreateComputePipelineState({ m_MegaLightsShadeComputeShader.get() });

            // クアッド共有(手法3)の解決パス。2x2の仲間が撃った標本を自分の面で評価し直して
            // 平均する。**レイを1本も撃たない**ので3バリアントすべてで焼ける
            // (パッカーの kSkipDxbc50Files には入れない)
            RHI::ShaderDesc megaLightsResolveCsDesc;
            megaLightsResolveCsDesc.Stage = RHI::ShaderStage::Compute;
            megaLightsResolveCsDesc.FilePath = shaderDirectory + L"MegaLightsResolve.kshader";
            megaLightsResolveCsDesc.EntryPoint = "CSMain";
            m_MegaLightsResolveComputeShader = m_Device->CreateShader(megaLightsResolveCsDesc);
            m_MegaLightsResolvePipelineState =
                m_Device->CreateComputePipelineState({ m_MegaLightsResolveComputeShader.get() });

            // 空間再利用。目標関数に可視性を入れるレイと、不偏化の分母のためのバイアス補正レイを撃つ
            RHI::ShaderDesc megaLightsSpatialCsDesc;
            megaLightsSpatialCsDesc.Stage = RHI::ShaderStage::Compute;
            megaLightsSpatialCsDesc.FilePath = shaderDirectory + L"MegaLightsSpatial.kshader";
            megaLightsSpatialCsDesc.EntryPoint = "CSMain";
            m_MegaLightsSpatialComputeShader = m_Device->CreateShader(megaLightsSpatialCsDesc);
            m_MegaLightsSpatialPipelineState =
                m_Device->CreateComputePipelineState({ m_MegaLightsSpatialComputeShader.get() });

            // 時間再利用。採用した履歴サンプルが今も見えるかを確かめる時間検証レイを1本撃つ
            RHI::ShaderDesc megaLightsTemporalCsDesc;
            megaLightsTemporalCsDesc.Stage = RHI::ShaderStage::Compute;
            megaLightsTemporalCsDesc.FilePath = shaderDirectory + L"MegaLightsTemporal.kshader";
            megaLightsTemporalCsDesc.EntryPoint = "CSMain";
            m_MegaLightsTemporalComputeShader = m_Device->CreateShader(megaLightsTemporalCsDesc);
            m_MegaLightsTemporalPipelineState =
                m_Device->CreateComputePipelineState({ m_MegaLightsTemporalComputeShader.get() });

            // デノイザ。3エントリ(時間累積 / à-trous / 復調戻し)を1ファイルに置く。
            // パッカーは1ファイル内の複数の[numthreads]を自動で見つける
            {
                RHI::ShaderDesc denoiseDesc;
                denoiseDesc.Stage = RHI::ShaderStage::Compute;
                denoiseDesc.FilePath = shaderDirectory + L"MegaLightsDenoise.kshader";
                denoiseDesc.EntryPoint = "CSTemporalAccum";
                m_MegaLightsDenoiseTemporalShader = m_Device->CreateShader(denoiseDesc);
                m_MegaLightsDenoiseTemporalPSO =
                    m_Device->CreateComputePipelineState({ m_MegaLightsDenoiseTemporalShader.get() });
                denoiseDesc.EntryPoint = "CSAtrous";
                m_MegaLightsDenoiseAtrousShader = m_Device->CreateShader(denoiseDesc);
                m_MegaLightsDenoiseAtrousPSO =
                    m_Device->CreateComputePipelineState({ m_MegaLightsDenoiseAtrousShader.get() });
                denoiseDesc.EntryPoint = "CSRemodulate";
                m_MegaLightsDenoiseRemodulateShader = m_Device->CreateShader(denoiseDesc);
                m_MegaLightsDenoiseRemodulatePSO =
                    m_Device->CreateComputePipelineState({ m_MegaLightsDenoiseRemodulateShader.get() });

                RHI::BufferDesc denoiseCbDesc;
                denoiseCbDesc.Usage = RHI::BufferUsage::Constant;
                denoiseCbDesc.SizeInBytes = sizeof(MegaLightsDenoiseConstants);
                m_MegaLightsDenoiseConstantBuffer = m_Device->CreateBuffer(denoiseCbDesc);
            }

            RHI::BufferDesc megaLightsStochasticConstantBufferDesc;
            megaLightsStochasticConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
            megaLightsStochasticConstantBufferDesc.SizeInBytes = sizeof(MegaLightsStochasticConstants);
            m_MegaLightsStochasticConstantBuffer =
                m_Device->CreateBuffer(megaLightsStochasticConstantBufferDesc);
            // 空間再利用の反復ごとに1本ずつ。中身は共有分と同じで反復番号だけが違う
            for (uint32_t spatialIteration = 0u; spatialIteration < kMegaLightsMaxSpatialIterations;
                 ++spatialIteration)
            {
                m_MegaLightsSpatialConstantBuffer[spatialIteration] =
                    m_Device->CreateBuffer(megaLightsStochasticConstantBufferDesc);
            }

            // 蓄積平均(計測専用)。レイを撃たないがMegaLightsと同時にしか使わないのでここで作る
            RHI::ShaderDesc megaLightsAccumCsDesc;
            megaLightsAccumCsDesc.Stage = RHI::ShaderStage::Compute;
            megaLightsAccumCsDesc.FilePath = shaderDirectory + L"MegaLightsAccum.kshader";
            megaLightsAccumCsDesc.EntryPoint = "CSMain";
            m_MegaLightsAccumComputeShader = m_Device->CreateShader(megaLightsAccumCsDesc);
            m_MegaLightsAccumPipelineState =
                m_Device->CreateComputePipelineState({ m_MegaLightsAccumComputeShader.get() });

            RHI::BufferDesc megaLightsAccumConstantBufferDesc;
            megaLightsAccumConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
            megaLightsAccumConstantBufferDesc.SizeInBytes = sizeof(MegaLightsAccumConstants);
            m_MegaLightsAccumConstantBuffer = m_Device->CreateBuffer(megaLightsAccumConstantBufferDesc);

            // RTAOパス(コンピュートシェーダー。半球へレイを撃ち遮蔽率と間接拡散光を求める)
            RHI::ShaderDesc rtAOCsDesc;
            rtAOCsDesc.Stage = RHI::ShaderStage::Compute;
            rtAOCsDesc.FilePath = shaderDirectory + L"RTAO.kshader";
            rtAOCsDesc.EntryPoint = "CSMain";
            m_RTAOComputeShader = m_Device->CreateShader(rtAOCsDesc);
            m_RTAOPipelineState = m_Device->CreateComputePipelineState({ m_RTAOComputeShader.get() });

            RHI::BufferDesc rtAOConstantBufferDesc;
            rtAOConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
            rtAOConstantBufferDesc.SizeInBytes = sizeof(RTAOConstants);
            m_RTAOConstantBuffer = m_Device->CreateBuffer(rtAOConstantBufferDesc);

            // DDGIのプローブ取得(コンピュートシェーダー。プローブから6面ぶんのレイを撃ち、
            // ラスタ経路と同じ形のスクラッチキューブを直接埋める)
            //
            // 【失敗しても他のRTパスを巻き込まない】このシェーダーはコンピュートシェーダーの中で
            // テクスチャを微分付きにサンプルするため、DXILの検証がSM 6.6を要求する
            // (Derivatives in CS/MS/AS is SM 6.6+)。RayQuery自体はSM 6.5で足りるので、
            // 「DXR Tier 1.1に対応していて、かつSM 6.5のバリアントで動いている」環境
            // (ビルドマシンのWindows SDKが古くbindlessバリアントを焼けなかった場合など)では、
            // 上のRT反射/RTシャドウ/RTAOは作れるのにこれだけ作れない。
            // ここで捕まえてDDGIのレイ取得だけをラスタ経路へ戻す(自前ラスタライザと同じ扱い)
            try
            {
                RHI::ShaderDesc ddgiTraceCsDesc;
                ddgiTraceCsDesc.Stage = RHI::ShaderStage::Compute;
                ddgiTraceCsDesc.FilePath = shaderDirectory + L"DDGIProbeTrace.kshader";
                ddgiTraceCsDesc.EntryPoint = "CSMain";
                m_DDGIProbeTraceComputeShader = m_Device->CreateShader(ddgiTraceCsDesc);
                m_DDGIProbeTracePipelineState =
                    m_Device->CreateComputePipelineState({ m_DDGIProbeTraceComputeShader.get() });

                RHI::BufferDesc ddgiTraceConstantBufferDesc;
                ddgiTraceConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
                ddgiTraceConstantBufferDesc.SizeInBytes = sizeof(DDGITraceConstants);
                // プローブ1個につき6面ぶん書き換えるため、既定の段数では
                // 更新プローブ数を増やしたときに足りなくなる(1フレーム最大64プローブ×6面=384回)
                ddgiTraceConstantBufferDesc.MaxConstantUpdatesPerFrame = 1024;
                m_DDGITraceConstantBuffer = m_Device->CreateBuffer(ddgiTraceConstantBufferDesc);

                m_DDGIRaytracedTraceAvailable = true;
            }
            catch (const std::exception& e)
            {
                // ShouldRunRaytracedDDGITraceがパイプラインステートのnullを見ているため、
                // ここで捨てておけばレイ取得はラスタ経路のまま動く
                m_DDGIProbeTracePipelineState.reset();
                m_DDGIProbeTraceComputeShader.reset();
                m_DDGITraceConstantBuffer.reset();
                m_DDGIRaytracedTraceAvailable = false;
                Core::Logger::Error(
                    "KurenaiEngine3D",
                    std::string("DDGIのレイ取得(DXR)を用意できませんでした。ラスタライズ経路で動作します"
                                "(DDGIProbeTrace.hlslはシェーダーモデル6.6を要求します): ") + e.what());
            }

            // レイトレーシングが使える環境ではDDGIのレイ取得も既定でDXRにする。
            // 更新コストが下がり、カメラから遠いプローブにも影が落ちるようになるため
            m_DDGIRayMode = DDGIRayModeForCapability(m_DDGIRaytracedTraceAvailable);

            Core::Logger::Info(
                "KurenaiEngine3D",
                m_DDGIRaytracedTraceAvailable
                    ? "レイトレーシングを利用できます(反射・シャドウ・AO/GI・DDGIでRaytracedを選択可能)"
                    : "レイトレーシングを利用できます(反射・シャドウ・AOでRaytracedを選択可能。DDGIのレイ取得は"
                      "ラスタライズのみ)");
        }
        else
        {
            Core::Logger::Info(
                "KurenaiEngine3D",
                "レイトレーシングは利用できません(反射・シャドウ・AO/GIはいずれもスクリーンスペース手法のみ)");
        }

        // TAAパス(頂点バッファなしのフルスクリーン三角形。前フレームの結果をモーションベクターで
        // 再投影して蓄積する)。出力は履歴バッファ(常にfp16)で、バッファ精度の設定に依存しないため
        // CreatePrecisionDependentPipelineStatesではなくここで一度だけ作ればよい
        RHI::ShaderDesc taaVsDesc;
        taaVsDesc.Stage = RHI::ShaderStage::Vertex;
        taaVsDesc.FilePath = shaderDirectory + L"TAA.kshader";
        taaVsDesc.EntryPoint = "VSMain";
        m_TAAVertexShader = m_Device->CreateShader(taaVsDesc);

        RHI::ShaderDesc taaPsDesc;
        taaPsDesc.Stage = RHI::ShaderStage::Pixel;
        taaPsDesc.FilePath = shaderDirectory + L"TAA.kshader";
        taaPsDesc.EntryPoint = "PSMain";
        m_TAAPixelShader = m_Device->CreateShader(taaPsDesc);

        RHI::PipelineStateDesc taaPipelineDesc;
        taaPipelineDesc.VertexShader = m_TAAVertexShader.get();
        taaPipelineDesc.PixelShader = m_TAAPixelShader.get();
        taaPipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
        taaPipelineDesc.RenderTargetFormats = { RHI::Format::R16G16B16A16_Float };
        m_TAAPipelineState = m_Device->CreatePipelineState(taaPipelineDesc);

        RHI::BufferDesc taaConstantBufferDesc;
        taaConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        taaConstantBufferDesc.SizeInBytes = sizeof(TAAConstants);
        m_TAAConstantBuffer = m_Device->CreateBuffer(taaConstantBufferDesc);

        // Tonemapパス(頂点バッファなしのフルスクリーン三角形。HDRのSceneColorをLDRへ変換する)
        RHI::ShaderDesc tonemapVsDesc;
        tonemapVsDesc.Stage = RHI::ShaderStage::Vertex;
        tonemapVsDesc.FilePath = shaderDirectory + L"Tonemap.kshader";
        tonemapVsDesc.EntryPoint = "VSMain";
        m_TonemapVertexShader = m_Device->CreateShader(tonemapVsDesc);

        RHI::ShaderDesc tonemapPsDesc;
        tonemapPsDesc.Stage = RHI::ShaderStage::Pixel;
        tonemapPsDesc.FilePath = shaderDirectory + L"Tonemap.kshader";
        tonemapPsDesc.EntryPoint = "PSMain";
        m_TonemapPixelShader = m_Device->CreateShader(tonemapPsDesc);

        RHI::PipelineStateDesc tonemapPipelineDesc;
        tonemapPipelineDesc.VertexShader = m_TonemapVertexShader.get();
        tonemapPipelineDesc.PixelShader = m_TonemapPixelShader.get();
        tonemapPipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
        tonemapPipelineDesc.RenderTargetFormats = { RHI::Format::R8G8B8A8_UNorm };
        m_TonemapPipelineState = m_Device->CreatePipelineState(tonemapPipelineDesc);

        RHI::BufferDesc tonemapConstantBufferDesc;
        tonemapConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        tonemapConstantBufferDesc.SizeInBytes = sizeof(TonemapConstants);
        m_TonemapConstantBuffer = m_Device->CreateBuffer(tonemapConstantBufferDesc);

        // 超解像パス(EASU=拡大、RCAS=シャープ化。どちらもコンピュートシェーダー)。
        // レンダーターゲットではなくUAVへ書くのでPSOにフォーマットの指定は要らない
        RHI::ShaderDesc upscaleEasuCsDesc;
        upscaleEasuCsDesc.Stage = RHI::ShaderStage::Compute;
        upscaleEasuCsDesc.FilePath = shaderDirectory + L"Upscale.kshader";
        upscaleEasuCsDesc.EntryPoint = "CSEASU";
        m_UpscaleEASUComputeShader = m_Device->CreateShader(upscaleEasuCsDesc);
        m_UpscaleEASUPipelineState = m_Device->CreateComputePipelineState({ m_UpscaleEASUComputeShader.get() });

        RHI::ShaderDesc upscaleRcasCsDesc;
        upscaleRcasCsDesc.Stage = RHI::ShaderStage::Compute;
        upscaleRcasCsDesc.FilePath = shaderDirectory + L"Upscale.kshader";
        upscaleRcasCsDesc.EntryPoint = "CSRCAS";
        m_UpscaleRCASComputeShader = m_Device->CreateShader(upscaleRcasCsDesc);
        m_UpscaleRCASPipelineState = m_Device->CreateComputePipelineState({ m_UpscaleRCASComputeShader.get() });

        RHI::BufferDesc upscaleConstantBufferDesc;
        upscaleConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        upscaleConstantBufferDesc.SizeInBytes = sizeof(UpscaleConstants);
        m_UpscaleConstantBuffer = m_Device->CreateBuffer(upscaleConstantBufferDesc);

        // 自動露出パス(輝度ヒストグラムの構築→縮約→時間方向の順応。すべてコンピュートシェーダー)
        RHI::ShaderDesc autoExposureClearCsDesc;
        autoExposureClearCsDesc.Stage = RHI::ShaderStage::Compute;
        autoExposureClearCsDesc.FilePath = shaderDirectory + L"AutoExposure.kshader";
        autoExposureClearCsDesc.EntryPoint = "CSClearHistogram";
        m_AutoExposureClearComputeShader = m_Device->CreateShader(autoExposureClearCsDesc);
        m_AutoExposureClearPipelineState =
            m_Device->CreateComputePipelineState({ m_AutoExposureClearComputeShader.get() });

        RHI::ShaderDesc autoExposureHistogramCsDesc;
        autoExposureHistogramCsDesc.Stage = RHI::ShaderStage::Compute;
        autoExposureHistogramCsDesc.FilePath = shaderDirectory + L"AutoExposure.kshader";
        autoExposureHistogramCsDesc.EntryPoint = "CSHistogram";
        m_AutoExposureHistogramComputeShader = m_Device->CreateShader(autoExposureHistogramCsDesc);
        m_AutoExposureHistogramPipelineState =
            m_Device->CreateComputePipelineState({ m_AutoExposureHistogramComputeShader.get() });

        RHI::ShaderDesc autoExposureResolveCsDesc;
        autoExposureResolveCsDesc.Stage = RHI::ShaderStage::Compute;
        autoExposureResolveCsDesc.FilePath = shaderDirectory + L"AutoExposure.kshader";
        autoExposureResolveCsDesc.EntryPoint = "CSResolve";
        m_AutoExposureResolveComputeShader = m_Device->CreateShader(autoExposureResolveCsDesc);
        m_AutoExposureResolvePipelineState =
            m_Device->CreateComputePipelineState({ m_AutoExposureResolveComputeShader.get() });

        RHI::BufferDesc exposureHistogramBufferDesc;
        exposureHistogramBufferDesc.Usage = RHI::BufferUsage::Structured;
        exposureHistogramBufferDesc.SizeInBytes = sizeof(uint32_t) * kExposureHistogramBins;
        exposureHistogramBufferDesc.StrideInBytes = sizeof(uint32_t);
        m_ExposureHistogramBuffer = m_Device->CreateBuffer(exposureHistogramBufferDesc);

        RHI::BufferDesc autoExposureConstantBufferDesc;
        autoExposureConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        autoExposureConstantBufferDesc.SizeInBytes = sizeof(AutoExposureConstants);
        m_AutoExposureConstantBuffer = m_Device->CreateBuffer(autoExposureConstantBufferDesc);

        // 露出の保存先。フレームをまたいで順応の履歴を保持するため、ウィンドウリサイズで
        // 作り直されるCreateRenderTargetsではなくここで一度だけ作る。
        // 生成直後はゼロクリアされており、texel(1,0)=0が「未初期化」を意味する
        // (CSResolveがこれを見て初回だけ順応を飛ばして即座に目標値へ合わせる)
        m_ExposureTexture = m_Device->CreateUAVTexture(2, 1, RHI::Format::R32_Float);

        // ブルームパス(ダウンサンプル/アップサンプルの2エントリ。テクスチャはCreateRenderTargetsで作る)
        RHI::ShaderDesc bloomDownCsDesc;
        bloomDownCsDesc.Stage = RHI::ShaderStage::Compute;
        bloomDownCsDesc.FilePath = shaderDirectory + L"Bloom.kshader";
        bloomDownCsDesc.EntryPoint = "CSDownsample";
        m_BloomDownsampleComputeShader = m_Device->CreateShader(bloomDownCsDesc);
        m_BloomDownsamplePipelineState =
            m_Device->CreateComputePipelineState({ m_BloomDownsampleComputeShader.get() });

        RHI::ShaderDesc bloomUpCsDesc;
        bloomUpCsDesc.Stage = RHI::ShaderStage::Compute;
        bloomUpCsDesc.FilePath = shaderDirectory + L"Bloom.kshader";
        bloomUpCsDesc.EntryPoint = "CSUpsample";
        m_BloomUpsampleComputeShader = m_Device->CreateShader(bloomUpCsDesc);
        m_BloomUpsamplePipelineState =
            m_Device->CreateComputePipelineState({ m_BloomUpsampleComputeShader.get() });

        RHI::BufferDesc bloomConstantBufferDesc;
        bloomConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        bloomConstantBufferDesc.SizeInBytes = sizeof(BloomConstants);
        m_BloomConstantBuffer = m_Device->CreateBuffer(bloomConstantBufferDesc);

        // Presentパス(頂点バッファなしのフルスクリーン三角形。SceneColorをバックバッファへ拡大縮小表示)
        RHI::ShaderDesc presentVsDesc;
        presentVsDesc.Stage = RHI::ShaderStage::Vertex;
        presentVsDesc.FilePath = shaderDirectory + L"Present.kshader";
        presentVsDesc.EntryPoint = "VSMain";
        m_PresentVertexShader = m_Device->CreateShader(presentVsDesc);

        RHI::ShaderDesc presentPsDesc;
        presentPsDesc.Stage = RHI::ShaderStage::Pixel;
        presentPsDesc.FilePath = shaderDirectory + L"Present.kshader";
        presentPsDesc.EntryPoint = "PSMain";
        m_PresentPixelShader = m_Device->CreateShader(presentPsDesc);

        RHI::PipelineStateDesc presentPipelineDesc;
        presentPipelineDesc.VertexShader = m_PresentVertexShader.get();
        presentPipelineDesc.PixelShader = m_PresentPixelShader.get();
        presentPipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
        presentPipelineDesc.RenderTargetFormats = { RHI::Format::R8G8B8A8_UNorm };
        // スワップチェインへ描くパスは深度テストこそ使わないが、SetRenderTarget(swapChain)が
        // スワップチェインのDSVをバインドするため、DSVフォーマットの申告だけは必要になる
        presentPipelineDesc.DepthTargetAttached = true;
        m_PresentPipelineState = m_Device->CreatePipelineState(presentPipelineDesc);

        // シャドウパス(ライト視点への深度のみの描画。頂点入力はPOSITIONのみ使用)
        RHI::ShaderDesc shadowVsDesc;
        shadowVsDesc.Stage = RHI::ShaderStage::Vertex;
        shadowVsDesc.FilePath = shaderDirectory + L"Shadow.kshader";
        shadowVsDesc.EntryPoint = "VSMain";
        m_ShadowVertexShader = m_Device->CreateShader(shadowVsDesc);

        RHI::ShaderDesc shadowPsDesc;
        shadowPsDesc.Stage = RHI::ShaderStage::Pixel;
        shadowPsDesc.FilePath = shaderDirectory + L"Shadow.kshader";
        shadowPsDesc.EntryPoint = "PSMain";
        m_ShadowPixelShader = m_Device->CreateShader(shadowPsDesc);

        // アルファカットアウト用。切り抜きを反映しないと、葉や柵のように
        // テクスチャで抜く前提のマテリアルが板ポリゴンのまま影を落とす
        RHI::ShaderDesc shadowCutoutVsDesc;
        shadowCutoutVsDesc.Stage = RHI::ShaderStage::Vertex;
        shadowCutoutVsDesc.FilePath = shaderDirectory + L"Shadow.kshader";
        shadowCutoutVsDesc.EntryPoint = "VSMainCutout";
        m_ShadowCutoutVertexShader = m_Device->CreateShader(shadowCutoutVsDesc);

        RHI::ShaderDesc shadowCutoutPsDesc;
        shadowCutoutPsDesc.Stage = RHI::ShaderStage::Pixel;
        shadowCutoutPsDesc.FilePath = shaderDirectory + L"Shadow.kshader";
        shadowCutoutPsDesc.EntryPoint = "PSMainCutout";
        m_ShadowCutoutPixelShader = m_Device->CreateShader(shadowCutoutPsDesc);

        const std::vector<RHI::InputElementDesc> shadowInputLayout =
        {
            { "POSITION", 0, RHI::Format::R32G32B32_Float, 0 },
        };

        // カットアウトはベースカラーのアルファを引くためUVも要る。
        // オフセット24はAssets::Vertexの並び(Position 0 / Normal 12 / UV 24)から
        const std::vector<RHI::InputElementDesc> shadowCutoutInputLayout =
        {
            { "POSITION", 0, RHI::Format::R32G32B32_Float, 0 },
            { "TEXCOORD", 0, RHI::Format::R32G32_Float, 24 },
        };

        RHI::PipelineStateDesc shadowPipelineDesc;
        shadowPipelineDesc.InputLayout = shadowInputLayout;
        shadowPipelineDesc.VertexShader = m_ShadowVertexShader.get();
        shadowPipelineDesc.PixelShader = m_ShadowPixelShader.get();
        shadowPipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
        shadowPipelineDesc.HasDepthStencil = true;
        m_ShadowPipelineState = m_Device->CreatePipelineState(shadowPipelineDesc);
        // 影も同様に、ミラーリングされたインスタンスは表裏が入れ替わる。放置すると
        // シャドウマップへ内側の面の深度が書かれ、影の形と自己遮蔽の出方がずれる
        shadowPipelineDesc.FrontCounterClockwise = true;
        m_ShadowPipelineStateMirrored = m_Device->CreatePipelineState(shadowPipelineDesc);

        // アルファカットアウト用(頂点シェーダー経路)。切り抜きを反映して深度を書く。
        // 【DX11でも効く】bindlessもメッシュシェーダーも要らないので、両バックエンドで同じ影になる
        if (m_ShadowCutoutVertexShader && m_ShadowCutoutPixelShader)
        {
            RHI::PipelineStateDesc shadowCutoutDesc;
            shadowCutoutDesc.InputLayout = shadowCutoutInputLayout;
            shadowCutoutDesc.VertexShader = m_ShadowCutoutVertexShader.get();
            shadowCutoutDesc.PixelShader = m_ShadowCutoutPixelShader.get();
            shadowCutoutDesc.Topology = RHI::PrimitiveTopology::TriangleList;
            shadowCutoutDesc.HasDepthStencil = true;
            shadowCutoutDesc.FrontCounterClockwise = false;
            m_ShadowCutoutPipelineState = m_Device->CreatePipelineState(shadowCutoutDesc);
            shadowCutoutDesc.FrontCounterClockwise = true;
            m_ShadowCutoutPipelineStateMirrored = m_Device->CreatePipelineState(shadowCutoutDesc);
        }

        // メッシュシェーダー版のシャドウPSO。
        //
        // 【これが無いと1ドロー化が片手落ちになる】メッシュレット経路はG-Bufferにしか
        // 無かったため、モデルを1ドローで描けるようになってもシャドウは従来どおり
        // メッシュ単位で、しかもカスケード4枚ぶん発行され続ける。
        // PLATEAU LOD2の1タイル(メッシュ1,715個)ならG-Bufferが1ドローになる一方で
        // シャドウは6,860ドローのまま、ということになる。
        //
        // ピクセルシェーダーは持たない(深度だけを書く)。頂点シェーダー版が
        // 空のPSMainを渡しているのに合わせず段ごと省いているのは、深度プリパスの
        // 不透明用PSOと同じ理由(RHIDesc.hのPixelShader=nullptrの扱い)
        if (m_ShadowAmplificationShader && m_ShadowMeshShader)
        {
            RHI::MeshPipelineStateDesc shadowMeshDesc;
            shadowMeshDesc.AmplificationShader = m_ShadowAmplificationShader.get();
            shadowMeshDesc.MeshShader = m_ShadowMeshShader.get();
            shadowMeshDesc.PixelShader = nullptr;
            shadowMeshDesc.HasDepthStencil = true;
            shadowMeshDesc.FrontCounterClockwise = false;
            m_ShadowMeshletPipelineState = m_Device->CreateMeshPipelineState(shadowMeshDesc);
            shadowMeshDesc.FrontCounterClockwise = true;
            m_ShadowMeshletPipelineStateMirrored = m_Device->CreateMeshPipelineState(shadowMeshDesc);

            // カットアウト用。ピクセルシェーダーは頂点シェーダー経路と共有する
            // (ShadowMeshlet.hlslのShadowPSInputとShadow.hlslのCutoutPSInputは
            //  同じ並び・同じセマンティクスにしてある)
            if (m_ShadowCutoutPixelShader)
            {
                shadowMeshDesc.PixelShader = m_ShadowCutoutPixelShader.get();
                shadowMeshDesc.FrontCounterClockwise = false;
                m_ShadowMeshletCutoutPipelineState = m_Device->CreateMeshPipelineState(shadowMeshDesc);
                shadowMeshDesc.FrontCounterClockwise = true;
                m_ShadowMeshletCutoutPipelineStateMirrored = m_Device->CreateMeshPipelineState(shadowMeshDesc);
            }
        }

        // シャドウマップはG-Bufferと異なりウィンドウ/レンダー解像度に依存しないため固定サイズで一度だけ作成する。
        // 全カスケードを1つのTexture2DArrayにまとめ、スライスごとのDSVで1カスケードずつ描き込む
        m_ShadowCascadeArray = m_Device->CreateDepthTextureArray(kShadowMapSize, kShadowMapSize, kCascadeCount);

        // 既定のスカイボックス。.ksceneの[Scene]Skyboxで差し替えられる(LoadScene参照)ため、
        // 現在読み込んでいるパスを覚えておき、同じパスなら読み直さない
        m_DefaultSkyboxPath = dataRoot + L"Assets\\Skybox\\Sky.dds";
        m_CurrentSkyboxPath = m_DefaultSkyboxPath;
        m_SkyboxTexture = m_Device->CreateTextureFromFile(m_CurrentSkyboxPath, false);

        // 水面法線マップの既定。.ksceneに[Water]NormalMapが無いシーンではこのフラット法線
        // (128,128,255,255=接線空間で真上を向く法線)がWater.hlslのt6へバインドされ続ける。
        // ModelLoader.cppが法線マップ未指定のマテリアルに使うプレースホルダーと同じ値
        m_CurrentWaterNormalMapPath.clear();
        m_WaterNormalMapTexture = m_Device->CreateSolidColorTexture(128, 128, 255, 255);

        CreateSamplerSets();

        // IBL(Image Based Lighting)の3つの畳み込み結果を保持するテクスチャと、それを生成する
        // コンピュートシェーダー一式。実際の畳み込み(スカイボックスのサンプリング)はRender()の
        // 最初のフレームで一度だけ行う(m_IBLBaked参照)。ここではリソースの作成のみ行う
        m_IrradianceTexture = m_Device->CreateUAVTextureCube(kIBLIrradianceSize, RHI::Format::R16G16B16A16_Float);
        m_PrefilteredEnvTexture = m_Device->CreateMippedUAVTextureCube(
            kIBLPrefilterBaseSize, RHI::Format::R16G16B16A16_Float, kIBLPrefilterMipLevels);
        // BRDF積分LUTは2パスで焼く。パス1(CSMain)が(A, B)をスクラッチへ書き、
        // パス2(CSCombineEavg)がそれを読んでEavgを足した float4(A, B, Eavg, 0) を最終LUTへ書く。
        // 同一リソースをSRVとUAVへ同時バインドできないためスクラッチが要る(BRDFLUT.hlsl参照)
        m_BRDFLUTScratchTexture = m_Device->CreateUAVTexture(kIBLBRDFLUTSize, kIBLBRDFLUTSize, RHI::Format::R16G16_Float);
        m_BRDFLUTTexture = m_Device->CreateUAVTexture(kIBLBRDFLUTSize, kIBLBRDFLUTSize, RHI::Format::R16G16B16A16_Float);
        if (!m_BRDFLUTScratchTexture || !m_BRDFLUTTexture)
        {
            Core::Logger::Error("KurenaiEngine3D",
                "BRDF積分LUTのテクスチャ作成に失敗しました(スペキュラのエネルギー補正が正しく動作しません)");
        }

        RHI::ShaderDesc brdfLutCsDesc;
        brdfLutCsDesc.Stage = RHI::ShaderStage::Compute;
        brdfLutCsDesc.FilePath = shaderDirectory + L"BRDFLUT.kshader";
        brdfLutCsDesc.EntryPoint = "CSMain";
        m_BRDFLUTComputeShader = m_Device->CreateShader(brdfLutCsDesc);
        m_BRDFLUTPipelineState = m_Device->CreateComputePipelineState({ m_BRDFLUTComputeShader.get() });

        RHI::ShaderDesc brdfLutCombineCsDesc;
        brdfLutCombineCsDesc.Stage = RHI::ShaderStage::Compute;
        brdfLutCombineCsDesc.FilePath = shaderDirectory + L"BRDFLUT.kshader";
        brdfLutCombineCsDesc.EntryPoint = "CSCombineEavg";
        m_BRDFLUTCombineComputeShader = m_Device->CreateShader(brdfLutCombineCsDesc);
        if (!m_BRDFLUTCombineComputeShader)
        {
            Core::Logger::Error("KurenaiEngine3D",
                "BRDFLUT.hlsl CSCombineEavg のコンパイルに失敗しました"
                "(Kulla-Conty方式が必要とするEavgが焼かれず、同方式が正しく動作しません)");
        }
        m_BRDFLUTCombinePipelineState =
            m_Device->CreateComputePipelineState({ m_BRDFLUTCombineComputeShader.get() });

        // ボリュメトリック雲の3Dノイズ。カメラにも太陽にも空の状態にも依存しない
        // 純粋な手続き生成なので、BRDF積分LUTと同じく起動後に一度だけ焼く(m_CloudNoiseBaked)。
        // ここではリソースとパイプラインの作成だけを行う
        m_CloudShapeNoiseTexture = m_Device->CreateUAVTexture3D(
            kCloudShapeNoiseSize, kCloudShapeNoiseSize, kCloudShapeNoiseSize, RHI::Format::R8G8B8A8_UNorm);
        m_CloudDetailNoiseTexture = m_Device->CreateUAVTexture3D(
            kCloudDetailNoiseSize, kCloudDetailNoiseSize, kCloudDetailNoiseSize, RHI::Format::R8G8B8A8_UNorm);
        if (!m_CloudShapeNoiseTexture || !m_CloudDetailNoiseTexture)
        {
            Core::Logger::Error("KurenaiEngine3D",
                "雲の3Dノイズテクスチャの作成に失敗しました(ボリュメトリック雲が正しく描画されません)");
        }

        RHI::ShaderDesc cloudShapeNoiseCsDesc;
        cloudShapeNoiseCsDesc.Stage = RHI::ShaderStage::Compute;
        cloudShapeNoiseCsDesc.FilePath = shaderDirectory + L"CloudNoiseGenerate.kshader";
        cloudShapeNoiseCsDesc.EntryPoint = "CSGenerateShape";
        m_CloudShapeNoiseComputeShader = m_Device->CreateShader(cloudShapeNoiseCsDesc);
        if (!m_CloudShapeNoiseComputeShader)
        {
            Core::Logger::Error("KurenaiEngine3D",
                "CloudNoiseGenerate.hlsl CSGenerateShape のコンパイルに失敗しました"
                "(雲の形状ノイズが焼かれません)");
        }
        m_CloudShapeNoisePipelineState =
            m_Device->CreateComputePipelineState({ m_CloudShapeNoiseComputeShader.get() });

        RHI::ShaderDesc cloudDetailNoiseCsDesc;
        cloudDetailNoiseCsDesc.Stage = RHI::ShaderStage::Compute;
        cloudDetailNoiseCsDesc.FilePath = shaderDirectory + L"CloudNoiseGenerate.kshader";
        cloudDetailNoiseCsDesc.EntryPoint = "CSGenerateDetail";
        m_CloudDetailNoiseComputeShader = m_Device->CreateShader(cloudDetailNoiseCsDesc);
        if (!m_CloudDetailNoiseComputeShader)
        {
            Core::Logger::Error("KurenaiEngine3D",
                "CloudNoiseGenerate.hlsl CSGenerateDetail のコンパイルに失敗しました"
                "(雲のディテールノイズが焼かれません)");
        }
        m_CloudDetailNoisePipelineState =
            m_Device->CreateComputePipelineState({ m_CloudDetailNoiseComputeShader.get() });

        // 大気散乱のLUT(Hillaire 2020)。TransmittanceとMultiScatteringはカメラにも太陽にも
        // 依存せず、大気パラメータ(濁りを含む)だけの関数なので、濁りが変わらない限り焼き直さない
        // (m_AtmosphereLUTBakedTurbidity)。SkyViewは太陽の位置と濁りで変わるため、
        // そのどちらかが動いたときに焼き直す(m_SkyViewBakedSunPosition)。
        // HDRの放射輝度を格納するためR16G16B16A16_Float
        m_TransmittanceLUT = m_Device->CreateUAVTexture(
            kTransmittanceLUTWidth, kTransmittanceLUTHeight, RHI::Format::R16G16B16A16_Float);
        m_MultiScatteringLUT = m_Device->CreateUAVTexture(
            kMultiScatteringLUTSize, kMultiScatteringLUTSize, RHI::Format::R16G16B16A16_Float);
        m_SkyViewLUT = m_Device->CreateUAVTexture(
            kSkyViewLUTWidth, kSkyViewLUTHeight, RHI::Format::R16G16B16A16_Float);
        if (!m_TransmittanceLUT || !m_MultiScatteringLUT || !m_SkyViewLUT)
        {
            Core::Logger::Error("KurenaiEngine3D",
                "大気散乱のLUTテクスチャの作成に失敗しました(日中の空が黒くなります)");
        }
        // 【ここで焼き直し要求を必ず立てる】3枚とも中身が未初期化の新しいテクスチャになったので、
        // 「前回焼いたときの条件」を捨てないと、APIをDX11/DX12で切り替えた直後など
        // この関数が再度呼ばれた場合に一度も焼かれないまま読まれて空が黒くなる
        m_AtmosphereLUTBakedTurbidity = -1.0f;
        m_SkyViewBakedTurbidity = -1.0f;
        m_SkyViewBakedSunPosition = { 0.0f, 0.0f, 0.0f };

        RHI::BufferDesc atmosphereConstantBufferDesc;
        atmosphereConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        atmosphereConstantBufferDesc.SizeInBytes = sizeof(AtmosphereConstants);
        m_AtmosphereConstantBuffer = m_Device->CreateBuffer(atmosphereConstantBufferDesc);
        if (!m_AtmosphereConstantBuffer)
        {
            Core::Logger::Error("KurenaiEngine3D",
                "大気散乱の定数バッファの作成に失敗しました(日中の空が黒くなります)");
        }

        RHI::ShaderDesc transmittanceCsDesc;
        transmittanceCsDesc.Stage = RHI::ShaderStage::Compute;
        transmittanceCsDesc.FilePath = shaderDirectory + L"AtmosphereLUT.kshader";
        transmittanceCsDesc.EntryPoint = "CSTransmittance";
        m_TransmittanceComputeShader = m_Device->CreateShader(transmittanceCsDesc);
        if (!m_TransmittanceComputeShader)
        {
            Core::Logger::Error("KurenaiEngine3D",
                "AtmosphereLUT.hlsl CSTransmittance のコンパイルに失敗しました");
        }
        m_TransmittancePipelineState =
            m_Device->CreateComputePipelineState({ m_TransmittanceComputeShader.get() });

        RHI::ShaderDesc multiScatteringCsDesc;
        multiScatteringCsDesc.Stage = RHI::ShaderStage::Compute;
        multiScatteringCsDesc.FilePath = shaderDirectory + L"AtmosphereLUT.kshader";
        multiScatteringCsDesc.EntryPoint = "CSMultiScattering";
        m_MultiScatteringComputeShader = m_Device->CreateShader(multiScatteringCsDesc);
        if (!m_MultiScatteringComputeShader)
        {
            Core::Logger::Error("KurenaiEngine3D",
                "AtmosphereLUT.hlsl CSMultiScattering のコンパイルに失敗しました");
        }
        m_MultiScatteringPipelineState =
            m_Device->CreateComputePipelineState({ m_MultiScatteringComputeShader.get() });

        RHI::ShaderDesc skyViewCsDesc;
        skyViewCsDesc.Stage = RHI::ShaderStage::Compute;
        skyViewCsDesc.FilePath = shaderDirectory + L"AtmosphereLUT.kshader";
        skyViewCsDesc.EntryPoint = "CSSkyView";
        m_SkyViewComputeShader = m_Device->CreateShader(skyViewCsDesc);
        if (!m_SkyViewComputeShader)
        {
            Core::Logger::Error("KurenaiEngine3D",
                "AtmosphereLUT.hlsl CSSkyView のコンパイルに失敗しました(日中の空が黒くなります)");
        }
        m_SkyViewPipelineState =
            m_Device->CreateComputePipelineState({ m_SkyViewComputeShader.get() });

        RHI::ShaderDesc irradianceCsDesc;
        irradianceCsDesc.Stage = RHI::ShaderStage::Compute;
        irradianceCsDesc.FilePath = shaderDirectory + L"IBLConvolve.kshader";
        irradianceCsDesc.EntryPoint = "CSIrradiance";
        m_IrradianceComputeShader = m_Device->CreateShader(irradianceCsDesc);
        m_IrradiancePipelineState = m_Device->CreateComputePipelineState({ m_IrradianceComputeShader.get() });

        RHI::ShaderDesc prefilterCsDesc;
        prefilterCsDesc.Stage = RHI::ShaderStage::Compute;
        prefilterCsDesc.FilePath = shaderDirectory + L"IBLConvolve.kshader";
        prefilterCsDesc.EntryPoint = "CSPrefilter";
        m_PrefilterComputeShader = m_Device->CreateShader(prefilterCsDesc);
        m_PrefilterPipelineState = m_Device->CreateComputePipelineState({ m_PrefilterComputeShader.get() });

        // 拡散イラディアンスの球面調和関数(SH L2)経路。CSIrradianceの
        // 高速な代替で、A/B比較用にトグルで切り替える(m_IBLUseSHIrradiance、既定false)。
        // 詳細はIBLConvolve.hlsl冒頭のコメント参照
        RHI::ShaderDesc projectShCsDesc;
        projectShCsDesc.Stage = RHI::ShaderStage::Compute;
        projectShCsDesc.FilePath = shaderDirectory + L"IBLConvolve.kshader";
        projectShCsDesc.EntryPoint = "CSProjectSH";
        m_ProjectSHComputeShader = m_Device->CreateShader(projectShCsDesc);
        m_ProjectSHPipelineState = m_Device->CreateComputePipelineState({ m_ProjectSHComputeShader.get() });

        RHI::ShaderDesc projectShFinalCsDesc;
        projectShFinalCsDesc.Stage = RHI::ShaderStage::Compute;
        projectShFinalCsDesc.FilePath = shaderDirectory + L"IBLConvolve.kshader";
        projectShFinalCsDesc.EntryPoint = "CSProjectSHFinal";
        m_ProjectSHFinalComputeShader = m_Device->CreateShader(projectShFinalCsDesc);
        m_ProjectSHFinalPipelineState = m_Device->CreateComputePipelineState({ m_ProjectSHFinalComputeShader.get() });

        RHI::ShaderDesc evaluateShCsDesc;
        evaluateShCsDesc.Stage = RHI::ShaderStage::Compute;
        evaluateShCsDesc.FilePath = shaderDirectory + L"IBLConvolve.kshader";
        evaluateShCsDesc.EntryPoint = "CSEvaluateSH";
        m_EvaluateSHComputeShader = m_Device->CreateShader(evaluateShCsDesc);
        m_EvaluateSHPipelineState = m_Device->CreateComputePipelineState({ m_EvaluateSHComputeShader.get() });

        // SHの部分和(CSProjectSHのグループごとの出力)と最終係数(CSProjectSHFinalの出力)。
        // グループ数は (kSHProjectionSize/8)² × 6面で固定(射影解像度はSourceSkyboxの実解像度と
        // 無関係な固定値。kSHProjectionSizeのコメント参照)
        {
            const uint32_t groupsPerSide = (kSHProjectionSize + 7) / 8;
            const uint32_t maxSHGroups = groupsPerSide * groupsPerSide * kCubeFaceCount;
            RHI::BufferDesc shPartialSumsDesc;
            shPartialSumsDesc.Usage = RHI::BufferUsage::StructuredRW;
            shPartialSumsDesc.StrideInBytes = static_cast<uint32_t>(sizeof(DirectX::XMFLOAT4));
            shPartialSumsDesc.SizeInBytes = shPartialSumsDesc.StrideInBytes * maxSHGroups * kSHCoeffCount;
            m_SHPartialSumsBuffer = m_Device->CreateBuffer(shPartialSumsDesc);

            RHI::BufferDesc shCoefficientsDesc;
            shCoefficientsDesc.Usage = RHI::BufferUsage::StructuredRW;
            shCoefficientsDesc.StrideInBytes = static_cast<uint32_t>(sizeof(DirectX::XMFLOAT4));
            shCoefficientsDesc.SizeInBytes = shCoefficientsDesc.StrideInBytes * kSHCoeffCount;
            m_SHCoefficientsBuffer = m_Device->CreateBuffer(shCoefficientsDesc);
        }

        // 手続き空(SkyGenerate.hlsl)。太陽が動くたびに焼き直すため、IBLのプリフィルタと同じく
        // 面ごとに1回ずつディスパッチする。プリフィルタの入力にしかならないので解像度は
        // オフラインDDS(512)より小さい256で足りる(生成コストが1/4になる)
        m_ProceduralSkyTexture =
            m_Device->CreateUAVTextureCube(kProceduralSkySize, RHI::Format::R16G16B16A16_Float);

        RHI::ShaderDesc skyGenerateCsDesc;
        skyGenerateCsDesc.Stage = RHI::ShaderStage::Compute;
        skyGenerateCsDesc.FilePath = shaderDirectory + L"SkyGenerate.kshader";
        skyGenerateCsDesc.EntryPoint = "CSGenerateSky";
        m_SkyGenerateComputeShader = m_Device->CreateShader(skyGenerateCsDesc);
        m_SkyGeneratePipelineState = m_Device->CreateComputePipelineState({ m_SkyGenerateComputeShader.get() });

        RHI::BufferDesc skyBakeConstantBufferDesc;
        skyBakeConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        skyBakeConstantBufferDesc.SizeInBytes = sizeof(SkyBakeConstants);
        m_SkyBakeConstantBuffer = m_Device->CreateBuffer(skyBakeConstantBufferDesc);

        // 空パラメータ(ティント4本+照度正規化済みの天頂輝度)の積分をGPUで行うコンピュートシェーダー
        // 。SkyGenerateより前に実行し、結果をm_SkyParametersBufferへ書く
        RHI::ShaderDesc skyIntegrateCsDesc;
        skyIntegrateCsDesc.Stage = RHI::ShaderStage::Compute;
        skyIntegrateCsDesc.FilePath = shaderDirectory + L"SkyIntegrate.kshader";
        skyIntegrateCsDesc.EntryPoint = "CSIntegrateSky";
        m_SkyIntegrateComputeShader = m_Device->CreateShader(skyIntegrateCsDesc);
        m_SkyIntegratePipelineState = m_Device->CreateComputePipelineState({ m_SkyIntegrateComputeShader.get() });

        RHI::BufferDesc skyIntegrateConstantBufferDesc;
        skyIntegrateConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        skyIntegrateConstantBufferDesc.SizeInBytes = sizeof(SkyIntegrateConstants);
        m_SkyIntegrateConstantBuffer = m_Device->CreateBuffer(skyIntegrateConstantBufferDesc);

        // SkyIntegrate.hlslが書き、SkyGenerate.hlsl/DeferredLighting.hlsl/SSR.hlslが読む
        // 要素数1のStructuredRWバッファ(m_LightTileBufferと同じ作法)。
        //
        // 【CPU側からのゼロ初期化はできない】UpdateBuffer(CPU→GPU書き込み)でゼロ埋めする案を
        // 最初に採ったが、DX12のStructuredRWバッファはUAV/SRVでのGPUアクセス専用にDEFAULTヒープへ
        // 作成しており(DX12Device::CreateBuffer参照)、CPUから書き込むためのマップ済みポインタ・
        // ステージングリングを一切持たない。DX12CommandList::UpdateBufferの非対応分岐
        // (StructuredReadOnly/StructuredImmutable以外の既定経路)はAdvanceRingAndGetWritePtrで
        // nullptrへ書き込もうとしてクラッシュする。そのため未初期化対策はCPUからのゼロ埋めではなく、
        // 「SkyIntegrateパスをまだ一度も実行していないフレームでは、手続き空が無効でも1回だけ
        // 実行する」という形でGPU側から埋める(Render()のskyIntegrateThisFrame・
        // m_SkyParametersBufferInitialized参照)
        RHI::BufferDesc skyParametersBufferDesc;
        skyParametersBufferDesc.Usage = RHI::BufferUsage::StructuredRW;
        skyParametersBufferDesc.SizeInBytes = sizeof(GPUSkyParameters);
        skyParametersBufferDesc.StrideInBytes = sizeof(GPUSkyParameters);
        m_SkyParametersBuffer = m_Device->CreateBuffer(skyParametersBufferDesc);

        RHI::BufferDesc iblPrefilterConstantBufferDesc;
        iblPrefilterConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        iblPrefilterConstantBufferDesc.SizeInBytes = sizeof(IBLFaceConstants);
        m_IBLPrefilterConstantBuffer = m_Device->CreateBuffer(iblPrefilterConstantBufferDesc);

        // --- 反射プローブ(19章) ---
        // キャプチャ先(1面ぶんを6面で使い回す)。キューブへ写す前のHDR値を保つためFloatにする
        m_ProbeCaptureColor = m_Device->CreateRenderTexture(kProbeCaptureSize, kProbeCaptureSize, RHI::Format::R16G16B16A16_Float);
        // 同じキャプチャの2枚目(SV_TARGET1)。プローブからのワールド距離をそのまま入れるため、
        // [0,1]に収まらず精度も必要になる。R32_Floatなら室内スケールでも十分な絶対精度がある
        m_ProbeCaptureDistance = m_Device->CreateRenderTexture(kProbeCaptureSize, kProbeCaptureSize, RHI::Format::R32_Float);
        // Reverse-Zのため遠平面側(0.0)でクリアする(G-Buffer深度と同じ)
        m_ProbeCaptureDepth = m_Device->CreateDepthTexture(kProbeCaptureSize, kProbeCaptureSize, 0.0f);
        // 畳み込みの入力になるスクラッチのキューブマップ(TextureCubeとして読めること
        // = 配列ではないことが必須。理由はヘッダのm_ProbeRadianceCubeのコメント参照)
        m_ProbeRadianceCube = m_Device->CreateUAVTextureCube(kProbeCaptureSize, RHI::Format::R16G16B16A16_Float);
        // 畳み込み結果はプローブごとに保持するためキューブマップ配列で確保する。
        // 反射プローブは鏡面専任なので拡散イラディアンス側の配列は持たない
        // (拡散はDDGIへ一本化。ReflectionProbe.hlsli冒頭のコメント参照)
        m_ProbePrefilteredArray = m_Device->CreateMippedUAVTextureCubeArray(
            kIBLPrefilterBaseSize, RHI::Format::R16G16B16A16_Float, kIBLPrefilterMipLevels, kMaxReflectionProbes);
        // 距離キューブ(19.12節)。畳み込まないためミップは1段だけでよく、スクラッチのキューブも要らない
        // (キャプチャからこの配列のスライスへ直接書き込む)。
        // 128²×6面×8枚×4バイト = 3.1MB
        m_ProbeDistanceArray = m_Device->CreateMippedUAVTextureCubeArray(
            kProbeCaptureSize, RHI::Format::R32_Float, 1, kMaxReflectionProbes);

        RHI::ShaderDesc probeCaptureVsDesc;
        probeCaptureVsDesc.Stage = RHI::ShaderStage::Vertex;
        probeCaptureVsDesc.FilePath = shaderDirectory + L"ProbeCapture.kshader";
        probeCaptureVsDesc.EntryPoint = "VSMain";
        m_ProbeCaptureVertexShader = m_Device->CreateShader(probeCaptureVsDesc);

        RHI::ShaderDesc probeCapturePsDesc;
        probeCapturePsDesc.Stage = RHI::ShaderStage::Pixel;
        probeCapturePsDesc.FilePath = shaderDirectory + L"ProbeCapture.kshader";
        probeCapturePsDesc.EntryPoint = "PSMain";
        m_ProbeCapturePixelShader = m_Device->CreateShader(probeCapturePsDesc);

        RHI::PipelineStateDesc probeCapturePipelineDesc;
        probeCapturePipelineDesc.InputLayout = modelInputLayout;
        probeCapturePipelineDesc.VertexShader = m_ProbeCaptureVertexShader.get();
        probeCapturePipelineDesc.PixelShader = m_ProbeCapturePixelShader.get();
        probeCapturePipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
        // レンダーターゲットは2枚(放射輝度と距離)。ProbeCapture.hlslのPSOutputと並びを一致させること
        probeCapturePipelineDesc.RenderTargetFormats = { RHI::Format::R16G16B16A16_Float, RHI::Format::R32_Float };
        probeCapturePipelineDesc.HasDepthStencil = true;
        probeCapturePipelineDesc.ReverseZ = true;
        m_ProbeCapturePipelineState = m_Device->CreatePipelineState(probeCapturePipelineDesc);

        RHI::ShaderDesc probeCubeCopyCsDesc;
        probeCubeCopyCsDesc.Stage = RHI::ShaderStage::Compute;
        probeCubeCopyCsDesc.FilePath = shaderDirectory + L"IBLConvolve.kshader";
        probeCubeCopyCsDesc.EntryPoint = "CSCopyCaptureToCubeFace";
        m_ProbeCubeCopyComputeShader = m_Device->CreateShader(probeCubeCopyCsDesc);
        m_ProbeCubeCopyPipelineState = m_Device->CreateComputePipelineState({ m_ProbeCubeCopyComputeShader.get() });

        // プローブの影響範囲(位置・半径)を渡すStructuredBuffer(t13)。ライトリストと同じく
        // ピクセルシェーダからは読み取り専用でよい
        RHI::BufferDesc probeBufferDesc;
        probeBufferDesc.Usage = RHI::BufferUsage::StructuredReadOnly;
        probeBufferDesc.SizeInBytes = sizeof(GPUReflectionProbe) * kMaxReflectionProbes;
        probeBufferDesc.StrideInBytes = sizeof(GPUReflectionProbe);
        m_ProbeBuffer = m_Device->CreateBuffer(probeBufferDesc);

        // キャプチャの面ごとに更新するFrameConstants(共有のm_FrameConstantBufferとは別インスタンス)
        RHI::BufferDesc probeCaptureConstantBufferDesc;
        probeCaptureConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        probeCaptureConstantBufferDesc.SizeInBytes = sizeof(FrameConstants);
        m_ProbeCaptureConstantBuffer = m_Device->CreateBuffer(probeCaptureConstantBufferDesc);

        // --- 平面反射 ---
        // 水面に不透明ジオメトリの鏡像を映す専用フォワードパス。設計判断はPlanarReflection.hlsl
        // 冒頭のコメントを参照。反射先のテクスチャはレンダー解像度に依存するため、実際の確保は
        // CreatePlanarReflectionTargets(CreateRenderTargetsと同じ呼び出し箇所)が行う。
        // ここではProbeCaptureと同様、解像度に依存しないシェーダー・PSO・定数バッファのみ作る
        RHI::ShaderDesc planarReflectionVsDesc;
        planarReflectionVsDesc.Stage = RHI::ShaderStage::Vertex;
        planarReflectionVsDesc.FilePath = shaderDirectory + L"PlanarReflection.kshader";
        planarReflectionVsDesc.EntryPoint = "VSMain";
        m_PlanarReflectionVertexShader = m_Device->CreateShader(planarReflectionVsDesc);

        RHI::ShaderDesc planarReflectionPsDesc;
        planarReflectionPsDesc.Stage = RHI::ShaderStage::Pixel;
        planarReflectionPsDesc.FilePath = shaderDirectory + L"PlanarReflection.kshader";
        planarReflectionPsDesc.EntryPoint = "PSMain";
        m_PlanarReflectionPixelShader = m_Device->CreateShader(planarReflectionPsDesc);

        RHI::PipelineStateDesc planarReflectionPipelineDesc;
        planarReflectionPipelineDesc.InputLayout = modelInputLayout;
        planarReflectionPipelineDesc.VertexShader = m_PlanarReflectionVertexShader.get();
        planarReflectionPipelineDesc.PixelShader = m_PlanarReflectionPixelShader.get();
        planarReflectionPipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
        // レンダーターゲットは1枚(放射輝度のみ。ProbeCaptureと違い視差補正用の距離は要らない。
        // PlanarReflection.hlsl冒頭参照)。バッファ精度(Legacy8bit)の対象外にしてあり常にHDR固定
        planarReflectionPipelineDesc.RenderTargetFormats = { RHI::Format::R16G16B16A16_Float };
        planarReflectionPipelineDesc.HasDepthStencil = true;
        planarReflectionPipelineDesc.ReverseZ = true;
        m_PlanarReflectionPipelineState = m_Device->CreatePipelineState(planarReflectionPipelineDesc);

        // 鏡映カメラで描くとワインディングが全反転するため、m_GBufferPipelineStateMirroredと
        // 同じ仕組み(FrontCounterClockwiseの反転)で吸収する。選択条件はinstance.IsMirroredの
        // 否定になる点がGBufferパスと異なる(Render()側のExecute内参照)
        planarReflectionPipelineDesc.FrontCounterClockwise = true;
        m_PlanarReflectionPipelineStateMirrored = m_Device->CreatePipelineState(planarReflectionPipelineDesc);

        // captureProbeFaceと同じ役割の専用FrameConstants
        RHI::BufferDesc planarReflectionConstantBufferDesc;
        planarReflectionConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        planarReflectionConstantBufferDesc.SizeInBytes = sizeof(FrameConstants);
        m_PlanarReflectionConstantBuffer = m_Device->CreateBuffer(planarReflectionConstantBufferDesc);

        // --- DDGI(22章) ---
        // キャプチャ経路は反射プローブとまったく同じ(ProbeCapture.hlslとm_ProbeCapturePipelineStateを
        // そのまま使う)で、解像度だけkDDGICaptureSizeへ落とす。レンダーターゲットのフォーマットは
        // PSOと一致していなければならないため、反射プローブ側と同じ組み合わせにする
        m_DDGICaptureColor = m_Device->CreateRenderTexture(kDDGICaptureSize, kDDGICaptureSize, RHI::Format::R16G16B16A16_Float);
        m_DDGICaptureDistance = m_Device->CreateRenderTexture(kDDGICaptureSize, kDDGICaptureSize, RHI::Format::R32_Float);
        m_DDGICaptureDepth = m_Device->CreateDepthTexture(kDDGICaptureSize, kDDGICaptureSize, 0.0f);
        // 6面を組み上げるスクラッチのキューブ。更新CSは1テクセル(=1つの方向)を出力するのに
        // 6面ぶん1536本のレイを全て走査するため、面ごとの2Dテクスチャではキューブとして
        // 引けず具合が悪い。放射輝度と距離で2本要る
        m_DDGICaptureRadianceCube = m_Device->CreateUAVTextureCube(kDDGICaptureSize, RHI::Format::R16G16B16A16_Float);
        m_DDGICaptureDistanceCube = m_Device->CreateUAVTextureCube(kDDGICaptureSize, RHI::Format::R32_Float);

        RHI::ShaderDesc ddgiUpdateCsDesc;
        ddgiUpdateCsDesc.Stage = RHI::ShaderStage::Compute;
        ddgiUpdateCsDesc.FilePath = shaderDirectory + L"DDGIProbeUpdate.kshader";
        ddgiUpdateCsDesc.EntryPoint = "CSUpdateProbe";
        m_DDGIProbeUpdateComputeShader = m_Device->CreateShader(ddgiUpdateCsDesc);
        m_DDGIProbeUpdatePipelineState = m_Device->CreateComputePipelineState({ m_DDGIProbeUpdateComputeShader.get() });

        // 境界の複製は本体の書き込みが全て終わってからでなければ正しい値を読めないため、
        // 同じディスパッチ内では行えず別パスになる(オクタヘドラルの縁は対辺へ折り返して繋がるので、
        // 自分のセルの反対側のテクセルを読む必要がある)
        RHI::ShaderDesc ddgiBorderCsDesc;
        ddgiBorderCsDesc.Stage = RHI::ShaderStage::Compute;
        ddgiBorderCsDesc.FilePath = shaderDirectory + L"DDGIProbeUpdate.kshader";
        ddgiBorderCsDesc.EntryPoint = "CSCopyBorder";
        m_DDGIBorderCopyComputeShader = m_Device->CreateShader(ddgiBorderCsDesc);
        m_DDGIBorderCopyPipelineState = m_Device->CreateComputePipelineState({ m_DDGIBorderCopyComputeShader.get() });

        RHI::BufferDesc ddgiUpdateConstantBufferDesc;
        ddgiUpdateConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        ddgiUpdateConstantBufferDesc.SizeInBytes = sizeof(DDGIUpdateConstants);
        m_DDGIUpdateConstantBuffer = m_Device->CreateBuffer(ddgiUpdateConstantBufferDesc);

        // スクロールで未確定になったプローブを、焼き直されるまでサンプリングから外すパス
        RHI::ShaderDesc ddgiInvalidateCsDesc;
        ddgiInvalidateCsDesc.Stage = RHI::ShaderStage::Compute;
        ddgiInvalidateCsDesc.FilePath = shaderDirectory + L"DDGIProbeUpdate.kshader";
        ddgiInvalidateCsDesc.EntryPoint = "CSInvalidateProbes";
        m_DDGIInvalidateProbesComputeShader = m_Device->CreateShader(ddgiInvalidateCsDesc);
        m_DDGIInvalidateProbesPipelineState =
            m_Device->CreateComputePipelineState({ m_DDGIInvalidateProbesComputeShader.get() });

        // 焼き直し待ちのスロット番号を渡す。最悪ケース(全プローブが一度に未確定)でも足りる大きさ。
        // 1フレームに1回しか書かないのでリングの段数は既定のままでよい
        RHI::BufferDesc ddgiDirtyBufferDesc;
        ddgiDirtyBufferDesc.Usage = RHI::BufferUsage::StructuredReadOnly;
        ddgiDirtyBufferDesc.SizeInBytes = static_cast<uint32_t>(sizeof(uint32_t)) * kDDGIMaxProbes;
        ddgiDirtyBufferDesc.StrideInBytes = static_cast<uint32_t>(sizeof(uint32_t));
        m_DDGIDirtyProbeBuffer = m_Device->CreateBuffer(ddgiDirtyBufferDesc);

        // ここまでで全シェーダーの生成が終わっている。読み込んだ.kshaderはもう誰も読まないので、
        // バイトコードをプロセスの寿命ぶん抱え続けないよう明示的に捨てる
        // (このあとCreateShaderを呼ぶことがあれば、必要なパッケージが読み直されるだけ)
        m_Device->ReleaseShaderPackages();

        // シーン読み込み前でもSRVをバインドできるよう、この時点で1プローブぶんのダミーを確保しておく
        RecreateDDGIAtlases();

        RHI::BufferDesc constantBufferDesc;
        constantBufferDesc.Usage = RHI::BufferUsage::Constant;
        constantBufferDesc.SizeInBytes = sizeof(FrameConstants);
        m_FrameConstantBuffer = m_Device->CreateBuffer(constantBufferDesc);

        // シャドウパスはカスケードごとに異なるビュー・プロジェクション行列で同じメッシュ群を描き直すため、
        // 共有のFrameConstantsとは別に、この1個の行列だけを持つ専用バッファを使い回す
        RHI::BufferDesc cascadeConstantBufferDesc;
        cascadeConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        cascadeConstantBufferDesc.SizeInBytes = sizeof(CascadeConstants);
        m_ShadowCascadeConstantBuffer = m_Device->CreateBuffer(cascadeConstantBufferDesc);

        RHI::BufferDesc objectConstantBufferDesc;
        objectConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        objectConstantBufferDesc.SizeInBytes = sizeof(ObjectConstants);
        // このバッファだけは「メッシュごと・パスごと」に書かれるため、既定の段数では足りない。
        // 1フレームの最悪ケースは、本編のパス(深度プリパス・G-Buffer・シャドウ4枚・半透明ほか)に
        // 加えて、プローブのキャプチャが「プローブ数 × 6面 × 不透明メッシュ数」を積む。
        // BistroInteriorLit(不透明59メッシュ)で既定の16プローブ/フレームだと
        // 59 × 6 × 16 = 5664 回に達し、既定の4096回では一周して描画が壊れていた。
        // 16384にしておけば同シーンで3倍近い余裕がある(1スロット256Bなので約8MB)
        objectConstantBufferDesc.MaxConstantUpdatesPerFrame = kObjectConstantUpdatesPerFrame;
        m_ObjectConstantBuffer = m_Device->CreateBuffer(objectConstantBufferDesc);

        // ポイント/スポットライトのリスト(t8)。CPUから毎フレーム更新するが、ピクセルシェーダから
        // 読み取り専用でよいためStructuredReadOnly(RWStructuredBufferではなくStructuredBuffer)で作成する
        RHI::BufferDesc lightBufferDesc;
        lightBufferDesc.Usage = RHI::BufferUsage::StructuredReadOnly;
        lightBufferDesc.SizeInBytes = sizeof(GPULight) * kMaxLights;
        lightBufferDesc.StrideInBytes = sizeof(GPULight);
        m_LightBuffer = m_Device->CreateBuffer(lightBufferDesc);

        RHI::BufferDesc lightingConstantBufferDesc;
        lightingConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        lightingConstantBufferDesc.SizeInBytes = sizeof(LightingConstants);
        m_LightingConstantBuffer = m_Device->CreateBuffer(lightingConstantBufferDesc);

        RHI::BufferDesc presentConstantBufferDesc;
        presentConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        presentConstantBufferDesc.SizeInBytes = sizeof(PresentConstants);
        m_PresentConstantBuffer = m_Device->CreateBuffer(presentConstantBufferDesc);

        // レンダーターゲットを先に作る。CreateRenderTargetsはHDRフォーマットの作成に失敗した場合に
        // m_BufferPrecisionをLegacy8bitへ落とすフォールバックを持つため、PSOはその結果が
        // 確定した後に作らなければフォーマットがずれる
        CreateRenderTargets(m_RenderWidth, m_RenderHeight);
        // 平面反射専用のレンダーターゲットも、レンダー解像度が確定したこのタイミングで作る
        // (呼び出し箇所はCreateRenderTargetsと同じ2か所。もう1か所はRender()の解像度変更ハンドリング)
        CreatePlanarReflectionTargets();
        CreatePrecisionDependentPipelineStates();

        DiscoverScenes();

        // 起動時の1シーン目だけは同期的に読み込む。この時点ではRender/Loaderのどちらのスレッドも
        // まだ動いていないため、通常のハンドオフを経由せず直接読み込んで反映してよい
        // (初回フレームより前にシーンが揃う従来の挙動を保つ)。
        // m_LoaderSkyboxPathはCreateSceneResourcesが読み込んだ既定スカイボックスに合わせておく
        m_LoaderSkyboxPath = m_CurrentSkyboxPath;
        // m_LoaderWaterNormalMapPathも同様。CreateSceneResourcesはフラット法線フォールバック
        // (m_CurrentWaterNormalMapPath = 空文字列)から始めるため、ここも空文字列で揃える
        m_LoaderWaterNormalMapPath = m_CurrentWaterNormalMapPath;
        // 通常は0(ファイル名昇順の先頭)。グラフィックスAPIの切り替えで作り直された場合だけ、
        // 呼び出し側が切り替え前のシーン番号を渡してくる。範囲外なら先頭へ落とす
        // (シーン一覧はDiscoverScenesが空でないことを保証済み)
        if (m_InitialSceneIndex >= m_SceneFilePaths.size())
        {
            Core::Logger::Warning(
                "KurenaiEngine3D",
                "指定された起動シーン番号" + std::to_string(m_InitialSceneIndex) + "が範囲外(シーン数: " +
                    std::to_string(m_SceneFilePaths.size()) + ")のため先頭のシーンを読み込みます");
            m_InitialSceneIndex = 0;
        }
        if (std::unique_ptr<LoadedScene> initialScene = LoadSceneOnLoaderThread(m_InitialSceneIndex))
        {
            ApplyLoadedScene(*initialScene);
            // ApplyLoadedSceneはUpdateスレッドへの引き渡しとして公開するだけなので、
            // まだUpdateスレッドが回っていないここでは自分で取り込む
            UpdateAppliedSceneHandoff();
        }
    }

    RHI::Format KurenaiEngine3D::GetEmissiveFormat() const
    {
        // Emissive: 1.0でクリップされると照明器具がHDRな輝度を持てず、ブルームが成立しない。
        // アルファを使わないためR11G11B10_Floatで足りる(帯域はR16G16B16A16_Floatの半分)
        return m_BufferPrecision == BufferPrecision::Legacy8bit ? RHI::Format::R8G8B8A8_UNorm
                                                                : RHI::Format::R11G11B10_Float;
    }

    RHI::Format KurenaiEngine3D::GetAOFormat() const
    {
        // AO/GIバッファ: rgb=間接拡散光(HDR)、a=遮蔽率。間接光は暗い室内では0.02〜0.1に収まり、
        // UNorm8ではコード5〜26の約20階調しか使えずポスタリゼーションする。
        // aに遮蔽率を持つためアルファ付きのR16G16B16A16_Floatを使う
        return m_BufferPrecision == BufferPrecision::Legacy8bit ? RHI::Format::R8G8B8A8_UNorm
                                                                : RHI::Format::R16G16B16A16_Float;
    }

    bool KurenaiEngine3D::ShouldRunRaytracedReflection() const
    {
        return m_ReflectionMode == ReflectionMode::Raytraced && m_RaytracingScene.IsValid() &&
               m_RTReflectionPipelineState != nullptr && m_RTReflectionTexture != nullptr;
    }

    bool KurenaiEngine3D::ShouldRunRaytracedShadow() const
    {
        return m_ShadowMode == ShadowMode::Raytraced && m_RaytracingScene.IsValid() &&
               m_RTShadowPipelineState != nullptr && m_RTShadowTexture != nullptr;
    }

    bool KurenaiEngine3D::ShouldRunMegaLights() const
    {
        if (m_MegaLightsMode == MegaLightsMode::Off || !m_RaytracingScene.IsValid() || m_MegaLightsTexture == nullptr)
        {
            return false;
        }
        // 手法ごとに必要なパイプラインが違う。確率的サンプリングもクアッド共有も
        // 候補プールと初期RIS(リザーバ)を共有し、そのあとの段だけが違う
        if (m_MegaLightsMode == MegaLightsMode::Stochastic)
        {
            return m_MegaLightsInitialPipelineState != nullptr && m_MegaLightsShadePipelineState != nullptr &&
                   m_MegaLightsTilePoolPipelineState != nullptr && m_MegaLightsTilePoolBuffer != nullptr &&
                   m_MegaLightsReservoirBuffer != nullptr;
        }
        if (m_MegaLightsMode == MegaLightsMode::QuadShared)
        {
            // Shade ではなく Resolve が色を書く。時間・空間再利用は使わないので、
            // 履歴バッファや空間再利用のping-pongが無くても走れる
            return m_MegaLightsInitialPipelineState != nullptr && m_MegaLightsResolvePipelineState != nullptr &&
                   m_MegaLightsTilePoolPipelineState != nullptr && m_MegaLightsTilePoolBuffer != nullptr &&
                   m_MegaLightsReservoirBuffer != nullptr && m_MegaLightsHistoryGuide[0] != nullptr;
        }
        return m_MegaLightsReferencePipelineState != nullptr;
    }

    bool KurenaiEngine3D::ShouldRunRaytracedAO() const
    {
        return m_AOTechnique == AOTechnique::Raytraced && m_RaytracingScene.IsValid() &&
               m_RTAOPipelineState != nullptr && m_RTAORawTexture != nullptr && m_RTAOTexture != nullptr;
    }

    void KurenaiEngine3D::SetDebugViewIndex(int index)
    {
        // 総数はenumのすぐ隣で定義してある(KurenaiEngine3D.h)
        if (index < 0 || index >= kDebugViewCount)
        {
            Core::Logger::Warning(
                "KurenaiEngine3D",
                "デバッグ表示の番号が範囲外のため無視します: " + std::to_string(index) + " (0〜" +
                    std::to_string(kDebugViewCount - 1) + ")");
            return;
        }

        m_DebugView = static_cast<DebugView>(index);
        Core::Logger::Info("KurenaiEngine3D", "デバッグ表示を番号で選択しました: " + std::to_string(index));
    }

    bool KurenaiEngine3D::ShouldSuppressEmissiveForGI() const
    {
        // 【プロキシが1つも無いなら抑止しない】発光面を光源にしていないのに
        // DDGIから自発光だけ抜くと、その面の照明が丸ごと落ちる
        return m_EmissiveLightsEnabled && !m_EmissiveLightsDoubleCountGI && !m_EmissiveProxies.empty();
    }

    void KurenaiEngine3D::SetEmissiveLights(int enabled, float cutoffIrradiance, int maxCount, int doubleCountGI)
    {
        // 負は「既定のまま」。しきい値だけ差し替えたいときに状態を巻き添えで倒さないため
        if (enabled >= 0)
        {
            m_EmissiveLightsEnabled = (enabled > 0);
        }
        // 0以下は「既定のまま」。OverrideMegaLightsの負値と同じ約束にしてある
        if (cutoffIrradiance > 0.0f)
        {
            m_EmissiveLightsCutoffIrradiance = cutoffIrradiance;
        }
        if (maxCount > 0)
        {
            m_EmissiveLightsMaxCount = maxCount;
        }
        if (doubleCountGI >= 0)
        {
            m_EmissiveLightsDoubleCountGI = (doubleCountGI > 0);
        }
        // 上限の警告は設定を変えたら出し直す(τを上げてRangeを縮めた結果を見たいため)
        m_EmissiveLightsCapLogged = false;
        Core::Logger::Info(
            "KurenaiEngine3D",
            std::string("エミッシブ光源: ") + (m_EmissiveLightsEnabled ? "有効" : "無効") +
                " / 打ち切り照度 " + std::to_string(m_EmissiveLightsCutoffIrradiance) + " / 上限 " +
                std::to_string(m_EmissiveLightsMaxCount) + "個 / プロキシ " +
                std::to_string(m_EmissiveProxies.size()) + "個 / DDGIの自発光 " +
                (ShouldSuppressEmissiveForGI() ? "抑止" : "そのまま(二重計上)"));
    }

    void KurenaiEngine3D::SetEmissiveIntensity(float intensity)
    {
        if (intensity <= 0.0f)
        {
            return;
        }
        m_EmissiveIntensity = intensity;
        // 倍率を変えるとRangeも変わる(強さから解いているため)。上限の警告を出し直す
        m_EmissiveLightsCapLogged = false;
        m_EmissiveLightsValuesLogged = false;
        Core::Logger::Info(
            "KurenaiEngine3D", "自発光の強度: " + std::to_string(m_EmissiveIntensity) + "倍");
    }

    void KurenaiEngine3D::OverrideMegaLights(int mode, int shadowRayCount, int sampleCount)
    {
        // 手法の総数はenumの末尾で決まる。値はUIのコンボの並びとも一致している
        constexpr int kMegaLightsModeCount = static_cast<int>(MegaLightsMode::QuadShared) + 1;
        if (mode >= kMegaLightsModeCount)
        {
            Core::Logger::Warning(
                "KurenaiEngine3D",
                "MegaLightsの手法の番号が範囲外のため無視します: " + std::to_string(mode) + " (0〜" +
                    std::to_string(kMegaLightsModeCount - 1) + ")");
        }
        // 負の値は「既定のまま」。本数側と同じ約束にしてある
        else if (mode >= 0)
        {
            m_MegaLightsMode = static_cast<MegaLightsMode>(mode);
            Core::Logger::Info(
                "KurenaiEngine3D", "MegaLightsの手法を番号で選択しました: " + std::to_string(mode));

            if (m_MegaLightsMode != MegaLightsMode::Off && !m_RaytracingAvailable)
            {
                // 黙って何も起きないと「効かないバグ」に見えるので、必ず理由を残す
                Core::Logger::Warning(
                    "KurenaiEngine3D",
                    "この環境ではレイトレーシングが使えないため、MegaLightsのパスは実行されません"
                    "(DX12かつDXR Tier 1.1が必要)");
            }
        }

        // 負の値は「既定のまま」。0は恒等テストとして意味のある値なので弾かない
        if (shadowRayCount >= 0)
        {
            m_MegaLightsShadowRayCount = shadowRayCount;
            Core::Logger::Info(
                "KurenaiEngine3D",
                "MegaLightsの1灯あたりの影レイ本数を設定しました: " + std::to_string(shadowRayCount));
        }

        // RISのM。0以下は意味を成さないので1以上に丸める
        if (sampleCount > 0)
        {
            m_MegaLightsSampleCount = sampleCount;
            Core::Logger::Info(
                "KurenaiEngine3D",
                "MegaLightsの初期候補数Mを設定しました: " + std::to_string(sampleCount));
        }
    }

    void KurenaiEngine3D::SetMegaLightsAccumFrames(int frames)
    {
        if (frames < 0)
        {
            Core::Logger::Warning(
                "KurenaiEngine3D",
                "MegaLightsの蓄積フレーム数が負のため無視します: " + std::to_string(frames));
            return;
        }

        m_MegaLightsAccumTargetFrames = frames;
        // 枚数を変えたら取り直す。途中まで足した状態に継ぎ足すと、
        // 「何サンプルの平均か」が分からなくなる
        m_MegaLightsAccumFrames = 0;
        Core::Logger::Info(
            "KurenaiEngine3D", "MegaLightsの蓄積フレーム数を設定しました: " + std::to_string(frames));
    }

    void KurenaiEngine3D::SetMegaLightsSpatial(int enabled, int neighborCount, int radius, int useMIS)
    {
        if (enabled >= 0)
        {
            m_MegaLightsSpatialEnabled = (enabled != 0);
            Core::Logger::Info(
                "KurenaiEngine3D",
                std::string("MegaLightsの空間再利用を") + (m_MegaLightsSpatialEnabled ? "有効" : "無効") +
                    "にしました");
        }
        // 0は「近傍を借りない」= 実質無効として意味があるので弾かない
        if (neighborCount >= 0)
        {
            m_MegaLightsSpatialNeighborCount = neighborCount;
            Core::Logger::Info(
                "KurenaiEngine3D",
                "MegaLightsの空間再利用で借りる近傍の数を設定しました: " + std::to_string(neighborCount));
        }
        if (radius > 0)
        {
            m_MegaLightsSpatialRadius = radius;
            Core::Logger::Info(
                "KurenaiEngine3D",
                "MegaLightsの空間再利用の半径を設定しました: " + std::to_string(radius));
        }
        if (useMIS >= 0)
        {
            m_MegaLightsSpatialMIS = (useMIS != 0);
            Core::Logger::Info(
                "KurenaiEngine3D",
                std::string("MegaLightsの空間再利用の結合を") +
                    (m_MegaLightsSpatialMIS ? "生成化バランスヒューリスティック" : "confidence重み") +
                    "にしました");
        }
    }

    void KurenaiEngine3D::SetAutoExposureEnabled(bool enabled)
    {
        m_AutoExposureEnabled = enabled;
        Core::Logger::Info(
            "KurenaiEngine3D",
            std::string("自動露出を起動オプションで設定しました: ") + (enabled ? "有効" : "無効"));
    }

    void KurenaiEngine3D::SetOcclusionCullingEnabled(bool enabled)
    {
        m_OcclusionCullingEnabled = enabled;
        Core::Logger::Info(
            "KurenaiEngine3D",
            std::string("Hi-Zオクルージョンカリングを起動オプションで設定しました: ")
                + (enabled ? "有効" : "無効"));
    }

    void KurenaiEngine3D::SetMeshletRenderingEnabled(bool enabled)
    {
        m_MeshletRenderingEnabled = enabled;
        Core::Logger::Info(
            "KurenaiEngine3D",
            std::string("メッシュレット描画を起動オプションで設定しました: ")
                + (enabled ? "有効" : "無効"));
    }

    void KurenaiEngine3D::SetTAAEnabled(bool enabled)
    {
        m_TAAEnabled = enabled;
        Core::Logger::Info(
            "KurenaiEngine3D",
            std::string("TAAを起動オプションで設定しました: ") + (enabled ? "有効" : "無効"));
    }

    void KurenaiEngine3D::SetPerfDump(const wchar_t* path, int frames)
    {
        if (path == nullptr || path[0] == L'\0' || frames <= 0)
        {
            m_PerfDumpPath.clear();
            m_PerfDumpTargetFrames = 0;
            return;
        }
        m_PerfDumpPath = path;
        m_PerfDumpTargetFrames = frames;
        m_PerfDumpWarmupFrames = 0;
        m_PerfDumpCollected = 0;
        m_PerfDumpDone = false;
        m_PerfDumpTotals.clear();
        Core::Logger::Info(
            "KurenaiEngine3D",
            "GPU計測の書き出しを設定しました(計測用): " + Core::WideToUtf8(m_PerfDumpPath) + " / " +
                std::to_string(frames) + "フレーム");
    }

    void KurenaiEngine3D::SetMegaLightsSpatialIterations(int iterations)
    {
        if (iterations < 1)
        {
            Core::Logger::Warning(
                "KurenaiEngine3D",
                "MegaLightsの空間再利用の反復回数に1未満が指定されたため、既定のままにします: " +
                    std::to_string(iterations));
            return;
        }
        const int clamped = std::min(iterations, static_cast<int>(kMegaLightsMaxSpatialIterations));
        if (clamped != iterations)
        {
            Core::Logger::Warning(
                "KurenaiEngine3D",
                "MegaLightsの空間再利用の反復回数が上限を超えたため頭打ちにしました: " +
                    std::to_string(iterations) + " -> " + std::to_string(clamped));
        }
        m_MegaLightsSpatialIterations = clamped;
        Core::Logger::Info(
            "KurenaiEngine3D",
            "MegaLightsの空間再利用の反復回数を設定しました: " + std::to_string(clamped));
    }

    void KurenaiEngine3D::SetMegaLightsDenoiseFireflyClamp(float k)
    {
        if (k < 0.0f)
        {
            Core::Logger::Warning(
                "KurenaiEngine3D",
                "MegaLightsのファイアフライのクランプに負の値が指定されたため、既定のままにします: " +
                    std::to_string(k));
            return;
        }
        m_MegaLightsDenoiseFireflyClamp = k;
        Core::Logger::Info(
            "KurenaiEngine3D",
            "MegaLightsのファイアフライのクランプを設定しました: " + std::to_string(k));
    }

    void KurenaiEngine3D::SetMegaLightsDenoiseSigmaLuminance(float sigma)
    {
        if (!(sigma > 0.0f))
        {
            Core::Logger::Warning(
                "KurenaiEngine3D",
                "MegaLightsのデノイザのσ(輝度)に正でない値が指定されたため、既定のままにします: " +
                    std::to_string(sigma));
            return;
        }
        m_MegaLightsDenoiseSigmaLuminance = sigma;
        Core::Logger::Info(
            "KurenaiEngine3D",
            "MegaLightsのデノイザのσ(輝度)を設定しました: " + std::to_string(sigma));
    }

    void KurenaiEngine3D::SetMegaLightsDenoise(int enabled, int atrousPasses, int maxFrames)
    {
        // 負の値・0は「既定のまま」。他のMegaLightsオプションと同じ約束
        if (enabled >= 0)
        {
            m_MegaLightsDenoiseEnabled = (enabled != 0);
            // 切り替えた瞬間の履歴は今の設定で作られたものではないので捨てる
            m_MegaLightsDenoiseHistoryValid = false;
            Core::Logger::Info(
                "KurenaiEngine3D",
                std::string("MegaLightsのデノイザを") + (m_MegaLightsDenoiseEnabled ? "有効" : "無効") +
                    "にしました");
        }
        // 0段は「時間累積だけ」で意味があるので弾かない
        if (atrousPasses >= 0)
        {
            m_MegaLightsDenoiseAtrousPasses = atrousPasses;
            Core::Logger::Info(
                "KurenaiEngine3D",
                "MegaLightsのデノイザのa-trousの段数を設定しました: " + std::to_string(atrousPasses));
        }
        if (maxFrames > 0)
        {
            // 【両方の手法へ入れる】計測用のつまみなので、指定したのに走っている手法の
            // ほうが読まれない、という取りこぼしを作らない
            m_MegaLightsDenoiseMaxFrames = maxFrames;
            m_MegaLightsQuadDenoiseMaxFrames = maxFrames;
            Core::Logger::Info(
                "KurenaiEngine3D",
                "MegaLightsのデノイザの時間累積の上限を設定しました: " + std::to_string(maxFrames));
        }
    }

    void KurenaiEngine3D::SetMegaLightsPerturb(int mode)
    {
        if (mode < 0 || mode > 2)
        {
            Core::Logger::Warning(
                "KurenaiEngine3D",
                "MegaLightsの摂動モードが範囲外のため無視します: " + std::to_string(mode) + " (0〜2)");
            return;
        }
        m_MegaLightsPerturbMode = mode;
        m_MegaLightsPerturbApplied = false;
        Core::Logger::Info(
            "KurenaiEngine3D", "MegaLightsの摂動モードを設定しました(検証用): " + std::to_string(mode));
    }

    void KurenaiEngine3D::SetMegaLightsTemporal(int enabled, int mClamp)
    {
        // 負の値は「既定のまま」。他のMegaLightsオプションと同じ約束
        if (enabled >= 0)
        {
            m_MegaLightsTemporalEnabled = (enabled != 0);
            // 切り替えた瞬間の履歴は今の設定で作られたものではないので捨てる
            m_MegaLightsHistoryValid = false;
            Core::Logger::Info(
                "KurenaiEngine3D",
                std::string("MegaLightsの時間再利用を") + (m_MegaLightsTemporalEnabled ? "有効" : "無効") +
                    "にしました");
        }
        if (mClamp > 0)
        {
            m_MegaLightsTemporalMClamp = mClamp;
            Core::Logger::Info(
                "KurenaiEngine3D",
                "MegaLightsの時間再利用のMの上限を設定しました: " + std::to_string(mClamp));
        }
    }

    void KurenaiEngine3D::SetMegaLightsInitialVisibility(int enabled)
    {
        // 負の値は「既定のまま」。他のMegaLightsオプションと同じ約束
        if (enabled >= 0)
        {
            m_MegaLightsInitialVisibility = (enabled != 0);
            Core::Logger::Info(
                "KurenaiEngine3D",
                std::string("MegaLightsの初期可視レイを") + (m_MegaLightsInitialVisibility ? "有効" : "無効") +
                    "にしました");
        }
    }

    void KurenaiEngine3D::SetMegaLightsQuadShare(int share, int stratify, int blockedCache)
    {
        // 負の値は「既定のまま」。他のMegaLightsオプションと同じ約束
        if (share >= 0)
        {
            m_MegaLightsQuadShareEnabled = (share != 0);
            Core::Logger::Info(
                "KurenaiEngine3D",
                std::string("MegaLightsのクアッド共有を") + (m_MegaLightsQuadShareEnabled ? "有効" : "無効") +
                    "にしました");
        }
        if (stratify >= 0)
        {
            m_MegaLightsQuadStratify = (stratify != 0);
            Core::Logger::Info(
                "KurenaiEngine3D",
                std::string("MegaLightsのクアッド層化を") + (m_MegaLightsQuadStratify ? "有効" : "無効") +
                    "にしました");
        }
        if (blockedCache >= 0)
        {
            m_MegaLightsBlockedCacheEnabled = (blockedCache != 0);
            Core::Logger::Info(
                "KurenaiEngine3D",
                std::string("MegaLightsの遮蔽キャッシュを") + (m_MegaLightsBlockedCacheEnabled ? "有効" : "無効") +
                    "にしました");
        }
    }

    void KurenaiEngine3D::SetMegaLightsQuadSamples(int samples)
    {
        // 負の値は「既定のまま」。他のMegaLightsオプションと同じ約束
        if (samples < 0)
        {
            return;
        }
        if (samples < 1 || samples > kMegaLightsMaxSamplesPerPixel)
        {
            Core::Logger::Warning(
                "KurenaiEngine3D",
                "MegaLightsのクアッド標本数が範囲外のため無視します: " + std::to_string(samples) +
                    " (1〜" + std::to_string(kMegaLightsMaxSamplesPerPixel) + ")");
            return;
        }
        if (samples == m_MegaLightsQuadSamplesPerPixel)
        {
            return;
        }
        m_MegaLightsQuadSamplesPerPixel = samples;
        // リザーババッファの大きさが変わる。GPUが参照していない状態で作り直す必要があるので、
        // 解像度変更と同じ「フレームの先頭でまとめて作り直す」経路に乗せる
        m_MegaLightsReservoirDirty = true;
        Core::Logger::Info(
            "KurenaiEngine3D",
            "MegaLightsのクアッド標本数を " + std::to_string(m_MegaLightsQuadSamplesPerPixel) +
                " にしました(影レイの本数も同じ数になります)");
    }

    int32_t KurenaiEngine3D::MegaLightsSamplesPerPixel() const
    {
        // 【手法3以外は必ず1】手法2の時間・空間再利用は「1画素1リザーバ」を前提に
        // 添字を組み立てているので、ここを1より大きくすると別画素の標本を読む
        if (m_MegaLightsMode != MegaLightsMode::QuadShared)
        {
            return 1;
        }
        return std::clamp(m_MegaLightsQuadSamplesPerPixel, 1, kMegaLightsMaxSamplesPerPixel);
    }

    void KurenaiEngine3D::SetMegaLightsDumpPath(const wchar_t* path)
    {
        if (path == nullptr || path[0] == L'\0')
        {
            m_MegaLightsDumpPath.clear();
            return;
        }

        m_MegaLightsDumpPath = path;
        m_MegaLightsDumpIssued = false;
        m_MegaLightsDumpDone = false;
        m_MegaLightsDumpCopyFrame = 0;
        Core::Logger::Info(
            "KurenaiEngine3D", "MegaLightsの蓄積平均の書き出し先を設定しました: " + Core::WideToUtf8(path));
    }

    void KurenaiEngine3D::ForceDDGIRayModeRaster()
    {
        m_DDGIRayMode = DDGIRayMode::Raster;
        Core::Logger::Info("KurenaiEngine3D", "DDGIのレイ取得をラスタライズへ固定しました(起動オプション)");
    }

    void KurenaiEngine3D::SetDDGIBackfaceThreshold(float threshold)
    {
        if (threshold <= 0.0f)
        {
            m_DDGIProbeClassificationEnabled = false;
            Core::Logger::Info("KurenaiEngine3D", "DDGIのプローブ分類を無効にしました(起動オプション)");
            return;
        }

        m_DDGIProbeClassificationEnabled = true;
        m_DDGIBackfaceThreshold = threshold;
        Core::Logger::Info(
            "KurenaiEngine3D",
            "DDGIのプローブ分類のしきい値を設定しました(起動オプション): " + std::to_string(threshold));
    }

    void KurenaiEngine3D::OverrideDDGILOD(uint32_t lodCount, bool followCamera)
    {
        if (!m_HasGIVolume)
        {
            Core::Logger::Warning("KurenaiEngine3D", "[GIVolume]が無いためLODの上書きは効きません");
            return;
        }

        // 0は「.ksceneの指定のまま」を意味する(追従だけを切り替えたいとき)
        const uint32_t requested = (lodCount == 0u) ? m_GIVolume.LODCount : lodCount;
        const uint32_t clamped = std::clamp(requested, 1u, kDDGIMaxLODCount);
        const uint64_t probeCount =
            static_cast<uint64_t>(m_GIVolume.ProbeCounts[0]) *
            static_cast<uint64_t>(m_GIVolume.ProbeCounts[1]) *
            static_cast<uint64_t>(m_GIVolume.ProbeCounts[2]) *
            static_cast<uint64_t>(clamped);
        if (probeCount > kDDGIMaxProbes)
        {
            Core::Logger::Error(
                "KurenaiEngine3D",
                "LODの上書きでプローブ数が上限(" + std::to_string(kDDGIMaxProbes) + ")を超えるため無視します: " +
                    std::to_string(probeCount) + "個");
            return;
        }

        m_GIVolume.LODCount = clamped;
        m_GIVolume.FollowCamera = followCamera;
        // 段数が変わるとアトラスの行数が変わるので確保し直す(中身も作り直しになる)
        RecreateDDGIAtlases();
        Core::Logger::Info(
            "KurenaiEngine3D",
            "DDGIのLODを上書きしました(起動オプション): 段数 " + std::to_string(clamped) +
                " / カメラ追従 " + (followCamera ? "有効" : "無効"));
    }

    bool KurenaiEngine3D::ShouldRunRaytracedDDGITrace() const
    {
        return m_DDGIRayMode == DDGIRayMode::Raytraced && m_RaytracingScene.IsValid() &&
               m_DDGIProbeTracePipelineState != nullptr && m_DDGITraceConstantBuffer != nullptr;
    }

    bool KurenaiEngine3D::ShouldUseMeshletPath(
        const Assets::Model& model, const Assets::Mesh& mesh, bool isWater) const
    {
        // 【水面はメッシュレット経路に載せない】水面のピクセルシェーダーはWater.hlslの
        // PSMainで、G-Buffer本体のPSMainとは別物。メッシュシェーダー版を用意するには
        // PSOをもう2本(通常/ミラー)増やすことになるが、水面は.ksceneが置く平面1枚で
        // 三角形数が少なく、メッシュレットカリングの利得がほとんど無い
        if (isWater)
        {
            return false;
        }

        // メッシュレットが焼かれていない(--no-meshletsでパックされた.kmodel)、
        // またはデバイスが非対応でGPUバッファを作っていない場合はnullptrになる。
        // 【MeshletCountで判定しないこと】あちらはアセットが持つ数そのもので、
        // メッシュシェーダー非対応の環境でも(レイトレーシングが使うため)0にはならない。
        // 表はモデル単位なので、このメッシュ自身が塊を持っているかも併せて見る
        // (モデル内に塊を持たないメッシュが混ざりうる)
        if (!model.MeshletBuffer || !model.MaterialTableBuffer || mesh.MeshletCount == 0)
        {
            return false;
        }

        return m_MeshletRenderingEnabled && m_GBufferMeshletPipelineState != nullptr;
    }

    void KurenaiEngine3D::EnsureModelCullCapacity(uint32_t candidateCount)
    {
        if (candidateCount == 0 || !m_ModelCullPipelineState)
        {
            return;
        }
        if (m_ModelCullInstanceBuffer && m_ModelCullDrawArgsBuffer && candidateCount <= m_ModelCullCapacity)
        {
            return;
        }

        // 作り直しの頻度を下げるため、必要数ぴったりではなく少し余裕を持たせる。
        // シーン切り替えとストリーミングで候補数は増減する
        const uint32_t capacity = std::max<uint32_t>(64u, candidateCount + candidateCount / 4u);

        try
        {
            // 候補の配列。毎フレームCPUから書き直すのでStructuredReadOnly。
            // 1フレームに1回しか書かないためMaxUpdatesPerFrameは既定のままでよい
            RHI::BufferDesc instanceDesc;
            instanceDesc.Usage = RHI::BufferUsage::StructuredReadOnly;
            instanceDesc.SizeInBytes = static_cast<uint32_t>(sizeof(GpuModelCullInstance)) * capacity;
            instanceDesc.StrideInBytes = static_cast<uint32_t>(sizeof(GpuModelCullInstance));
            instanceDesc.MaxUpdatesPerFrame = 1;
            auto instanceBuffer = m_Device->CreateBuffer(instanceDesc);

            // 生き残りの DispatchMesh 引数。そのままExecuteIndirectへ渡すのでIndirectArgs。
            //
            // 【区画ごとに配列を分ける】PSOはExecuteIndirectの引数では切り替えられないため、
            // ミラーリングの有無・プリパスの不透明/カットアウトを別の配列へ詰め、
            // PSOごとに1回ずつ発行する。
            // 先頭のkModelCullArgsBaseOffsetバイトは区画ごとの発行数(uint)が占める
            RHI::BufferDesc drawArgsDesc;
            drawArgsDesc.Usage = RHI::BufferUsage::IndirectArgs;
            drawArgsDesc.SizeInBytes =
                kModelCullArgsBaseOffset + ComputeModelCullRegionStride(capacity) * kModelCullRegionCount;
            drawArgsDesc.StrideInBytes = RHI::IRHICommandList::kDispatchMeshIndirectArgStride;
            auto drawArgsBuffer = m_Device->CreateBuffer(drawArgsDesc);

            // 【作り終えてから差し替える】途中で例外が出たときに、古いバッファを
            // 手放した状態で戻ってしまうのを避ける
            m_ModelCullInstanceBuffer = std::move(instanceBuffer);
            m_ModelCullDrawArgsBuffer = std::move(drawArgsBuffer);
            m_ModelCullCapacity = capacity;
            m_ModelCullRegionStride = ComputeModelCullRegionStride(capacity);
        }
        catch (const std::exception& e)
        {
            Core::Logger::Warning(
                "KurenaiEngine3D",
                std::string("モデル単位のGPUカリングのバッファを作れませんでした(この機能を止めます): ") + e.what());
            m_ModelCullInstanceBuffer.reset();
            m_ModelCullDrawArgsBuffer.reset();
            m_ModelCullCapacity = 0;
            m_ModelCullRegionStride = 0;
        }
    }

    bool KurenaiEngine3D::IssueModelCullIndirect(
        RHI::IRHICommandList* cmd, uint32_t region, RHI::IRHIPipelineState* pipelineState,
        RHI::IRHIPipelineState*& currentPipelineState)
    {
        if (!cmd || region >= kModelCullRegionCount || !pipelineState || !m_ModelCullDrawArgsBuffer)
        {
            return false;
        }
        // GPUが書く発行数の上限。候補が1件も無い区画はExecuteIndirectごと省く
        const uint32_t maxCommandCount = m_ModelCullRegionCandidates[region];
        if (maxCommandCount == 0)
        {
            return false;
        }

        if (pipelineState != currentPipelineState)
        {
            cmd->SetPipelineState(pipelineState);
            cmd->SetConstantBuffer(0, m_FrameConstantBuffer.get());
            cmd->SetSamplerSet(m_MaterialSamplers.get());
            currentPipelineState = pipelineState;
        }

        // 【b1(ObjectConstants)はここでは張らない】コマンドシグネチャがドローごとに
        // 差し替える。ここで張っても最初のドローで上書きされるだけで、意味が無いどころか
        // 「張ってあるから大丈夫」という誤解の元になる
        cmd->DispatchMeshIndirect(
            m_ModelCullDrawArgsBuffer.get(),
            kModelCullArgsBaseOffset + region * m_ModelCullRegionStride,
            maxCommandCount,
            region * static_cast<uint32_t>(sizeof(uint32_t)));
        return true;
    }

    bool KurenaiEngine3D::ShouldUseModelMeshletPath(
        const Assets::ModelInstance& instance, const Assets::Model& model) const
    {
        // モデル内の1メッシュでも従来経路へ落ちる条件があるなら、モデル全体を従来経路にする。
        // 混ぜると「1ドローで描いたぶん」と「メッシュ単位で描いたぶん」が同じフレームに
        // 同居し、食い違いが出たときにどちらのせいか切り分けられなくなる
        if (!model.AllMeshesHaveMeshlets)
        {
            return false;
        }
        if (model.Meshes.empty())
        {
            return false;
        }

        // 代表として先頭のメッシュで判定する。AllMeshesHaveMeshletsが真なら
        // メッシュ間で結果は変わらない(残りの条件はすべてモデル単位/インスタンス単位)
        return ShouldUseMeshletPath(model, model.Meshes.front(), instance.IsWater);
    }

    RHI::IRHITexture* KurenaiEngine3D::GetActiveAOTexture() const
    {
        if (!m_AOEnabled)
        {
            return m_AODisabledTexture.get();
        }
        if (ShouldRunRaytracedAO())
        {
            return m_RTAOTexture.get();
        }
        if (m_AOTechnique == AOTechnique::SSILVisibilityBitmask)
        {
            return m_SSILTexture.get();
        }
        // SSAO、およびRaytracedを選んでいても実行できないフレーム(高速化構造が無い等)
        return m_SSAOTexture.get();
    }

    RHI::IRHITexture* KurenaiEngine3D::GetActiveAORawTexture() const
    {
        if (!m_AOEnabled)
        {
            return m_AODisabledTexture.get();
        }
        if (ShouldRunRaytracedAO())
        {
            return m_RTAORawTexture.get();
        }
        if (m_AOTechnique == AOTechnique::SSILVisibilityBitmask)
        {
            return m_SSILRawTexture.get();
        }
        return m_SSAORawTexture.get();
    }

    RHI::IRHITexture* KurenaiEngine3D::GetActiveReflectionOutput() const
    {
        if (m_ReflectionMode == ReflectionMode::ScreenSpace)
        {
            return m_SSRTexture.get();
        }
        if (ShouldRunRaytracedReflection())
        {
            return m_RTReflectionTexture.get();
        }
        // 反射なし、またはRT反射を実行しなかった場合はLightingパスの結果をそのまま後段へ渡す
        return m_SceneColor.get();
    }

    void KurenaiEngine3D::CreatePrecisionDependentPipelineStates()
    {
        const RHI::Format emissiveFormat = GetEmissiveFormat();
        const RHI::Format aoFormat = GetAOFormat();

        try
        {
            // ジオメトリパス(G-Buffer書き込み)
            RHI::PipelineStateDesc gbufferPipelineDesc;
            gbufferPipelineDesc.InputLayout = GetModelInputLayout();
            gbufferPipelineDesc.VertexShader = m_GBufferVertexShader.get();
            gbufferPipelineDesc.PixelShader = m_GBufferPixelShader.get();
            gbufferPipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
            gbufferPipelineDesc.RenderTargetFormats =
            {
                RHI::Format::R8G8B8A8_UNorm, // Albedo
                RHI::Format::R16G16_Float,   // Normal(オクタヘドラルエンコード)
                RHI::Format::R8G8B8A8_UNorm, // Material(R=Metallic, G=Roughness)
                emissiveFormat,              // Emissive(バッファ精度に依存)
                RHI::Format::R16G16_Float,   // Velocity(モーションベクター。UV単位の2Dベクトル)
                RHI::Format::R16G16B16A16_Float, // BentNormal(.rgb = bRaw、.a = 有効フラグ)
            };
            gbufferPipelineDesc.HasDepthStencil = true;
            gbufferPipelineDesc.ReverseZ = true;
            // 深度プリパス(41.22節)を通したとき、プリパスが書いた深度と同じ値になる最前面の
            // 断片だけを通すため、比較をGREATER_EQUALへ緩める。プリパスを切っていても
            // 不透明G-Bufferでは絵が変わらない(理由はRHIDesc.hのDepthAllowEqualのコメント)ので、
            // 有効/無効でPSOを2組に増やさず常にこちらにしてある
            gbufferPipelineDesc.DepthAllowEqual = true;
            m_GBufferPipelineState = m_Device->CreatePipelineState(gbufferPipelineDesc);

            // ミラーリングされたインスタンス用に、表裏判定だけを入れ替えた同じパイプラインを用意する。
            // DX12はラスタライザステートがPSOに焼き込まれ描画中に差し替えられないため、DX11/DX12で
            // 同じ構成にできるよう両バックエンドともPSOを2本持つ方式にしている
            gbufferPipelineDesc.FrontCounterClockwise = true;
            m_GBufferPipelineStateMirrored = m_Device->CreatePipelineState(gbufferPipelineDesc);

            // 水面(ModelInstance::IsWater)用。頂点シェーダー・入力レイアウト・レンダーターゲット
            // フォーマットは通常のG-Bufferとまったく同じで、ピクセルシェーダーだけをWater.hlslへ
            // 差し替える。ミラーリングとの組み合わせも通常PSOと同じ方式で2本持つ
            gbufferPipelineDesc.FrontCounterClockwise = false;
            gbufferPipelineDesc.PixelShader = m_GBufferWaterPixelShader.get();
            m_GBufferWaterPipelineState = m_Device->CreatePipelineState(gbufferPipelineDesc);
            gbufferPipelineDesc.FrontCounterClockwise = true;
            m_GBufferWaterPipelineStateMirrored = m_Device->CreatePipelineState(gbufferPipelineDesc);

            // メッシュシェーダー版のG-Bufferパス(GBufferMeshlet.hlsl)。
            // 入力レイアウトを持たない以外は上の通常PSOと同じ設定にする ―― ラスタライザ・
            // 深度・レンダーターゲットのどれか1つでもずれると、メッシュレットのON/OFFで
            // 見た目が変わってしまい「切り替えても一致するはず」という検証が成立しなくなる。
            //
            // 非対応環境ではCreateMeshPipelineStateがnullptrを返す。ポインタが空なら
            // 描画側が従来経路を使うため、ここで分岐して作成をスキップする必要はない
            if (m_Device->SupportsMeshShader() && m_GBufferMeshShader && m_GBufferAmplificationShader)
            {
                RHI::MeshPipelineStateDesc meshPipelineDesc;
                meshPipelineDesc.AmplificationShader = m_GBufferAmplificationShader.get();
                meshPipelineDesc.MeshShader = m_GBufferMeshShader.get();
                meshPipelineDesc.PixelShader = m_GBufferPixelShader.get();
                meshPipelineDesc.RenderTargetFormats = gbufferPipelineDesc.RenderTargetFormats;
                meshPipelineDesc.HasDepthStencil = true;
                meshPipelineDesc.ReverseZ = true;
                // 頂点シェーダー版と1つでもずれると切り替えで見た目が変わるため、深度比較も揃える
                meshPipelineDesc.DepthAllowEqual = true;
                meshPipelineDesc.FrontCounterClockwise = false;
                m_GBufferMeshletPipelineState = m_Device->CreateMeshPipelineState(meshPipelineDesc);

                meshPipelineDesc.FrontCounterClockwise = true;
                m_GBufferMeshletPipelineStateMirrored = m_Device->CreateMeshPipelineState(meshPipelineDesc);

                // メッシュレットの分かれ方を色で確かめるデバッグ表示用。
                // ピクセルシェーダーだけを差し替えた同じパイプライン
                if (m_GBufferMeshletDebugPixelShader)
                {
                    meshPipelineDesc.PixelShader = m_GBufferMeshletDebugPixelShader.get();
                    meshPipelineDesc.FrontCounterClockwise = false;
                    m_GBufferMeshletDebugPipelineState = m_Device->CreateMeshPipelineState(meshPipelineDesc);
                    meshPipelineDesc.FrontCounterClockwise = true;
                    m_GBufferMeshletDebugPipelineStateMirrored = m_Device->CreateMeshPipelineState(meshPipelineDesc);
                }
            }

            // 深度プリパス(41.22節)。G-Bufferとまったく同じ頂点シェーダー・入力レイアウトで
            // 深度だけを書く。レンダーターゲットは持たず、不透明マテリアル用は
            // ピクセルシェーダーそのものを持たない(段ごと省く)。
            //
            // 【頂点シェーダーを共有する理由】プリパスとG-Bufferで頂点の変換結果が
            // 1ulpでも違うと、深度が一致せずGREATER_EQUALのテストを通らなくなり、
            // その面がまるごと消える。別のシェーダーに写すと最適化の差で容易にずれる
            RHI::PipelineStateDesc depthPrepassPipelineDesc;
            depthPrepassPipelineDesc.InputLayout = GetModelInputLayout();
            depthPrepassPipelineDesc.VertexShader = m_GBufferVertexShader.get();
            depthPrepassPipelineDesc.PixelShader = nullptr;
            depthPrepassPipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
            depthPrepassPipelineDesc.HasDepthStencil = true;
            depthPrepassPipelineDesc.ReverseZ = true;
            m_DepthPrepassPipelineState = m_Device->CreatePipelineState(depthPrepassPipelineDesc);
            depthPrepassPipelineDesc.FrontCounterClockwise = true;
            m_DepthPrepassPipelineStateMirrored = m_Device->CreatePipelineState(depthPrepassPipelineDesc);

            // アルファカットアウト(glTFのalphaMode=MASK)用。切り抜かれる部分の深度まで
            // 書いてしまうとG-Buffer側のclipと食い違って穴が開くため、こちらだけ
            // 同じ判定のclipを持つピクセルシェーダーを通す(DepthPrepass.hlsl)
            if (m_DepthPrepassCutoutPixelShader)
            {
                depthPrepassPipelineDesc.PixelShader = m_DepthPrepassCutoutPixelShader.get();
                depthPrepassPipelineDesc.FrontCounterClockwise = false;
                m_DepthPrepassCutoutPipelineState = m_Device->CreatePipelineState(depthPrepassPipelineDesc);
                depthPrepassPipelineDesc.FrontCounterClockwise = true;
                m_DepthPrepassCutoutPipelineStateMirrored = m_Device->CreatePipelineState(depthPrepassPipelineDesc);
            }

            // メッシュシェーダー版の深度プリパス。
            //
            // 【これが無いとプリパスがまるごと止まる】かつてプリパスはメッシュレット経路と
            // 排他だった。プリパスが頂点シェーダーで深度を書き、G-Bufferがメッシュシェーダーで
            // 描くと、同じ頂点でも変換の丸めが一致する保証が無く、深度が1ulpずれた面が
            // GREATER_EQUALを通らずに消えるため。**G-Bufferと同じ増幅/メッシュシェーダーを
            // そのまま使えば変換は文字どおり同一のコードになり、この問題自体が消える。**
            //
            // 不透明用はピクセルシェーダーを持たない(段ごと省く)。カットアウト用は
            // G-Bufferとまったく同じ判定のclipを通す(DepthPrepass.hlsl)
            if (m_GBufferMeshShader && m_GBufferAmplificationShader)
            {
                RHI::MeshPipelineStateDesc prepassMeshDesc;
                prepassMeshDesc.AmplificationShader = m_GBufferAmplificationShader.get();
                prepassMeshDesc.MeshShader = m_GBufferMeshShader.get();
                prepassMeshDesc.PixelShader = nullptr;
                prepassMeshDesc.HasDepthStencil = true;
                prepassMeshDesc.ReverseZ = true;
                prepassMeshDesc.FrontCounterClockwise = false;
                m_DepthPrepassMeshletPipelineState = m_Device->CreateMeshPipelineState(prepassMeshDesc);
                prepassMeshDesc.FrontCounterClockwise = true;
                m_DepthPrepassMeshletPipelineStateMirrored = m_Device->CreateMeshPipelineState(prepassMeshDesc);

                if (m_DepthPrepassCutoutPixelShader)
                {
                    prepassMeshDesc.PixelShader = m_DepthPrepassCutoutPixelShader.get();
                    prepassMeshDesc.FrontCounterClockwise = false;
                    m_DepthPrepassMeshletCutoutPipelineState = m_Device->CreateMeshPipelineState(prepassMeshDesc);
                    prepassMeshDesc.FrontCounterClockwise = true;
                    m_DepthPrepassMeshletCutoutPipelineStateMirrored =
                        m_Device->CreateMeshPipelineState(prepassMeshDesc);
                }
            }

            // SSAOパス
            RHI::PipelineStateDesc ssaoPipelineDesc;
            ssaoPipelineDesc.VertexShader = m_AOVertexShader.get();
            ssaoPipelineDesc.PixelShader = m_SSAOPixelShader.get();
            ssaoPipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
            ssaoPipelineDesc.RenderTargetFormats = { aoFormat };
            m_SSAOPipelineState = m_Device->CreatePipelineState(ssaoPipelineDesc);

            // SSILパス(Visibility Bitmask)
            RHI::PipelineStateDesc ssilPipelineDesc;
            ssilPipelineDesc.VertexShader = m_AOVertexShader.get();
            ssilPipelineDesc.PixelShader = m_SSILPixelShader.get();
            ssilPipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
            ssilPipelineDesc.RenderTargetFormats = { aoFormat };
            m_SSILPipelineState = m_Device->CreatePipelineState(ssilPipelineDesc);

            // AO/GI共通のブラーパス(SSAO/SSILのどちらの出力にも同じフォーマットで書き戻す)
            RHI::PipelineStateDesc aoBlurPipelineDesc;
            aoBlurPipelineDesc.VertexShader = m_AOVertexShader.get();
            aoBlurPipelineDesc.PixelShader = m_AOBlurPixelShader.get();
            aoBlurPipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
            aoBlurPipelineDesc.RenderTargetFormats = { aoFormat };
            m_AOBlurPipelineState = m_Device->CreatePipelineState(aoBlurPipelineDesc);
        }
        catch (const std::exception& e)
        {
            // ここで失敗するとG-Buffer/AOパスが描けず復旧手段が無いため、ログを残して投げ直す
            Core::Logger::Error(
                "KurenaiEngine3D",
                std::string("バッファ精度に依存するパイプラインステートの作成に失敗しました (バッファ精度=") +
                    (m_BufferPrecision == BufferPrecision::Legacy8bit ? "Legacy8bit" : "HDR") + "): " + e.what());
            throw;
        }
    }

    void KurenaiEngine3D::DiscoverScenes()
    {
        const std::wstring sceneDirectory = GetModuleDirectory() + L"Assets\\Scenes\\";

        std::vector<std::wstring> fileNames;
        WIN32_FIND_DATAW findData{};
        HANDLE findHandle = FindFirstFileW((sceneDirectory + L"*.kscene").c_str(), &findData);
        if (findHandle != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                {
                    fileNames.push_back(findData.cFileName);
                }
            } while (FindNextFileW(findHandle, &findData));
            FindClose(findHandle);
        }

        // ImGuiのシーン一覧・LoadSceneのインデックスをビルドのたびに変わらないようにする
        std::sort(fileNames.begin(), fileNames.end(), [](const std::wstring& a, const std::wstring& b)
        {
            return _wcsicmp(a.c_str(), b.c_str()) < 0;
        });

        m_SceneFilePaths.clear();
        m_SceneDisplayNames.clear();
        for (const std::wstring& fileName : fileNames)
        {
            const std::wstring fullPath = sceneDirectory + fileName;
            try
            {
                m_SceneDisplayNames.push_back(Assets::ReadSceneName(fullPath));
                m_SceneFilePaths.push_back(fullPath);
            }
            catch (const std::exception& e)
            {
                // 1ファイルの不備でアプリ全体が起動できなくなるのを避け、そのファイルだけ除外して続行する
                Core::Logger::Error("KurenaiEngine3D", "シーンファイルの読み込みに失敗したため一覧から除外します (" + WideToUtf8(fullPath) + "): " + e.what());
            }
        }

        if (m_SceneFilePaths.empty())
        {
            const std::string message = "有効なシーンファイル(.kscene)が見つかりませんでした: " + WideToUtf8(sceneDirectory);
            Core::Logger::Error("KurenaiEngine3D", message);
            throw std::runtime_error(message);
        }
    }

    void KurenaiEngine3D::CreateSamplerSets()
    {
        // スロットの並びはShaders/3D/Samplers.hlsliの役割定義と一致させること
        // (s0 = MaterialSampler、s1 = ColorSampler、s2 = DataSampler、s3 = VolumeSampler)。

        // 色バッファ・LUT用。UVの端が定義域の端なのでClamp、拡縮でブロック状にならないようLinear。
        // BRDF積分LUTをWrapで引くと何が起きるかはdocs/Architecture.html 14.2.1節
        RHI::SamplerDesc colorSampler{};
        colorSampler.Filter = RHI::SamplerFilter::Linear;
        colorSampler.AddressMode = RHI::SamplerAddressMode::Clamp;

        // 深度・エンコード法線・metallic/roughness・シャドウマップ用。
        // 補間するとシルエット跨ぎで実在しない値になるためPoint、
        // カーネルのタップが[0,1]を出たときに反対側の端を読まないためClamp
        RHI::SamplerDesc dataSampler{};
        dataSampler.Filter = RHI::SamplerFilter::Point;
        dataSampler.AddressMode = RHI::SamplerAddressMode::Clamp;

        // マテリアル用。タイリング前提のWrapと、浅い角度で見る床・路面のボケを抑える異方性16x
        RHI::SamplerDesc materialSampler{};
        materialSampler.Filter = RHI::SamplerFilter::Anisotropic;
        materialSampler.AddressMode = RHI::SamplerAddressMode::Wrap;

        // ボリュームテクスチャ(3Dノイズ)用。ワールド空間で無限にタイリングして引くためWrapが必須で、
        // Clampだと周期の境界でトライリニア補間のタップが端のテクセルに張り付き継ぎ目が出る
        // (シェーダー側でfrac()しても補間がテクスチャの端を跨げないため消せない)。
        // レイマーチで等方的に刻んで引くので異方性フィルタは意味を持たずLinearでよい
        RHI::SamplerDesc volumeSampler{};
        volumeSampler.Filter = RHI::SamplerFilter::Linear;
        volumeSampler.AddressMode = RHI::SamplerAddressMode::Wrap;

        const RHI::SamplerDesc materialSet[] = { materialSampler, colorSampler, dataSampler, volumeSampler };
        m_MaterialSamplers = m_Device->CreateSamplerSet(materialSet, static_cast<uint32_t>(std::size(materialSet)));

        // スクリーン空間パスは画面内の中間バッファしか読まないため、s0にもWrapを置かない。
        // 万一シェーダ側で役割を選び違えても、画面端でUVが反対側へ回り込む不具合が起きないようにする。
        // 【s3のVolumeSamplerだけはこの原則の例外】引くのは画面UVではなくワールド空間の3D座標から
        // 作ったUVWなので、回り込む先の「反対側の画面端」がそもそも存在しない。詳細はSamplers.hlsliの
        // VolumeSamplerの宣言に書いてある
        const RHI::SamplerDesc screenSpaceSet[] = { colorSampler, colorSampler, dataSampler, volumeSampler };
        m_ScreenSpaceSamplers = m_Device->CreateSamplerSet(screenSpaceSet, static_cast<uint32_t>(std::size(screenSpaceSet)));
    }

    RHI::IRHITexture* KurenaiEngine3D::ActiveSkyTexture() const
    {
        // .ksceneが[Scene]Skyboxを明示しているシーンは、そのDDSでなければ意味を成さない
        // (White Furnace Testの一様放射輝度キューブマップが該当する)。手続き空で
        // 上書きしてしまうと検証そのものが壊れるため、明示指定があるときは必ずDDSを使う
        const bool useProcedural = m_ProceduralSkyEnabled && m_Scene.SkyboxPath.empty();
        return useProcedural ? m_ProceduralSkyTexture.get() : m_SkyboxTexture.get();
    }

    void KurenaiEngine3D::CreateRenderTargets(uint32_t width, uint32_t height)
    {
        if (width == 0 || height == 0)
        {
            return;
        }

        // 中間バッファのフォーマットはm_BufferPrecisionで切り替える(A/B比較用。BufferPrecision参照)。
        // Legacy8bitは「中間バッファをすべてR8G8B8A8_UNorm」にする構成
        const bool legacyPrecision = (m_BufferPrecision == BufferPrecision::Legacy8bit);

        // Albedoは両構成ともリニアのR8G8B8A8_UNormのままにする。
        // sRGB格納(R8G8B8A8_UNorm_SRGB)にすれば符号点が暗部へ寄り、暗いマテリアルの量子化は
        // 細かくなる(リニア反射率L=0.02で約4.3倍)。しかし実測すると最終画像への寄与は
        // 平均0.03/255と測定限界以下である。アルベドの量子化は面ごとの一定オフセットとして出るため、
        // 狙っていた暗部のバンディング(=照明の滑らかな変化が最終8bitで潰れる現象)には
        // そもそも効かない。加えてL>0.244では逆に粗くなり、金属はアルベドバッファの値を
        // F0として使う(DeferredLighting.hlsl)ぶん確実にその領域へ入るため、
        // 利点が確認できないまま欠点だけを抱えることになる。詳細はArchitecture.html 17.4節
        // フォーマットの決定はGetEmissiveFormat/GetAOFormatに一本化している。ここへ直接書くと
        // 同じ値を宣言するPSO側(CreatePrecisionDependentPipelineStates)とずれ、
        // D3D12では仕様違反になる
        const RHI::Format emissiveFormat = GetEmissiveFormat();
        const RHI::Format aoFormat = GetAOFormat();

        try
        {
            m_GBufferAlbedo = m_Device->CreateRenderTexture(width, height, RHI::Format::R8G8B8A8_UNorm);
            m_GBufferNormal = m_Device->CreateRenderTexture(width, height, RHI::Format::R16G16_Float);
            m_GBufferMaterial = m_Device->CreateRenderTexture(width, height, RHI::Format::R8G8B8A8_UNorm);
            m_GBufferEmissive = m_Device->CreateRenderTexture(width, height, emissiveFormat);
            // Reverse-Zのため近平面側(NDC z=1.0)ではなく遠平面側(NDC z=0.0)にクリアする
            m_GBufferDepth = m_Device->CreateDepthTexture(width, height, 0.0f);
            m_DirectLightTexture = m_Device->CreateRenderTexture(width, height, RHI::Format::R32G32B32A32_Float);
            m_SSAORawTexture = m_Device->CreateRenderTexture(width, height, aoFormat);
            m_SSAOTexture = m_Device->CreateRenderTexture(width, height, aoFormat);
            m_SSILRawTexture = m_Device->CreateRenderTexture(width, height, aoFormat);
            m_SSILTexture = m_Device->CreateRenderTexture(width, height, aoFormat);
            m_SceneColor = m_Device->CreateRenderTexture(width, height, RHI::Format::R16G16B16A16_Float);
            m_SSRTexture = m_Device->CreateRenderTexture(width, height, RHI::Format::R16G16B16A16_Float);
            // 大気遠近パスの出力。m_SSRTextureと同じ作法(HDR、R16G16B16A16_Float)で永続確保する
            m_AerialPerspectiveTexture = m_Device->CreateRenderTexture(width, height, RHI::Format::R16G16B16A16_Float);
            // 雲パスの出力(rgb=事前乗算済みの散乱光、a=透過率)。内部レンダー解像度の1/2で持つ。
            // 【R16G16B16A16_Float固定にする理由】平面反射(CreatePlanarReflectionTargets)と同じで、
            // 散乱光はHDRの輝度をそのまま持つためLegacy8bitでは飽和して雲が白く潰れる。
            // また透過率は乗算に使うので8bitの量子化がそのままバンディングになる
            m_SkyCloudWidth = std::max(1u, width / 2);
            m_SkyCloudHeight = std::max(1u, height / 2);
            m_SkyCloudTexture =
                m_Device->CreateRenderTexture(m_SkyCloudWidth, m_SkyCloudHeight, RHI::Format::R16G16B16A16_Float);
            // DDGIの低解像度解決パスの出力(rgb=イラディアンス、a=insideWeight)。雲と同じく1/2解像度。
            // 【常に確保する】m_DDGIHalfResolutionが無効でもシェーダーのt19には何かを
            // バインドしておく必要がある(DX12のディスクリプタテーブルを埋め切るため)。
            // フォーマットを雲と揃えているのも同じ理由 ―― イラディアンスはHDRの物理量で、
            // 8bitでは飽和と量子化がそのまま間接光のバンディングになる
            m_DDGIResolveWidth = std::max(1u, width / 2);
            m_DDGIResolveHeight = std::max(1u, height / 2);
            m_DDGIResolveTexture = m_Device->CreateRenderTexture(
                m_DDGIResolveWidth, m_DDGIResolveHeight, RHI::Format::R16G16B16A16_Float);
            // 上のパスが同時に書く「そのテクセルが代表している全解像度の深度」(41.24節)。
            // 合成側(DeferredLighting.hlsl)がGatherRed 1回で4テクセルぶんを取るためのもので、
            // t19と同じ理由で常に確保する(t21を空のままにできない)
            m_DDGIResolveDepthTexture = m_Device->CreateRenderTexture(
                m_DDGIResolveWidth, m_DDGIResolveHeight, RHI::Format::R32_Float);
            // RT反射はコンピュートシェーダーがUAVで書くため、レンダーターゲットではなくUAVテクスチャを作る。
            // 非対応環境ではパス自体が実行されないので確保しない
            if (m_RaytracingAvailable)
            {
                m_RTReflectionTexture = m_Device->CreateUAVTexture(width, height, RHI::Format::R16G16B16A16_Float);
                // RTシャドウの可視率(0〜1のスカラー)。RWTexture2D<float>として書くため単チャンネルの
                // R32_Floatにする(型付きUAVの読み書きが保証されているのはR32系のみ。AutoExposure.hlsl参照)
                m_RTShadowTexture = m_Device->CreateUAVTexture(width, height, RHI::Format::R32_Float);
                // RTAOの生バッファはコンピュートがUAVで書くためUAVテクスチャ、ブラー後は
                // 従来どおりピクセルシェーダーが書くレンダーターゲット。
                // フォーマットはSSAO/SSILと同じaoFormat(バッファ精度の設定に追従する)
                m_RTAORawTexture = m_Device->CreateUAVTexture(width, height, aoFormat);
                m_RTAOTexture = m_Device->CreateRenderTexture(width, height, aoFormat);
                // MegaLightsが書くポイント/スポットライトの直接光(HDR)。DirectLighting.hlslが
                // t7で読んで加算する。
                //
                // 【fp16ではなくfp32にしてある】RT反射やSceneColorと同じR16G16B16A16_Floatで
                // 十分に見えるが、このテクスチャは参照実装の出力 ―― 以降の段階すべての
                // 「真値」になる物差しでもある。fp16に落とすと、恒等テスト
                // (影レイ0本で従来のライトループと一致するか)で**片側だけに寄った差**が出た。
                // 実測: 3840x2088のManyLightsTestで、fp16は11661画素が1/255だけ暗い側へずれ、
                // 逆向きは0画素。fp32では差のある画素が13まで減り、符号も両側(9/4)に散った。
                // 物差し自体が系統的に暗い側へ寄っていると、確率的サンプリングの
                // バイアス検査(N枚平均が真値へ寄るか)がそのぶん汚染される。
                // 帯域が問題になったら、参照実装とは別の出力先を用意して測ってから決めること
                m_MegaLightsTexture = m_Device->CreateUAVTexture(width, height, RHI::Format::R32G32B32A32_Float);
            }
            m_TonemapTexture = m_Device->CreateRenderTexture(width, height, RHI::Format::R8G8B8A8_UNorm);

            // モーションベクター(速度バッファ)。G-Bufferの5枚目として、GBuffer.hlslが
            // 「この画素に映っているものが前フレームでは画面のどこにいたか」をUV単位の2Dベクトルで書く。
            // 2成分しか要らないのでR16G16_Float。1画素ぶんの移動量が1/解像度(1920幅なら約0.00052)と
            // 小さいため、絶対精度ではなく相対精度で効く浮動小数点フォーマットが適している
            m_GBufferVelocity = m_Device->CreateRenderTexture(width, height, RHI::Format::R16G16_Float);

            // bent normal(正規化しない可視方向の平均、ワールド空間)。G-Bufferの6枚目。
            // .rgb = bRaw、.a = 有効フラグ。
            //
            // R11G11B10_Floatにはできない ―― 符号なしのため負の成分が落ち、
            // 半球の半分の方向を表現できなくなる。1080pで約16MB増える(34章)
            m_GBufferBentNormal = m_Device->CreateRenderTexture(width, height, RHI::Format::R16G16B16A16_Float);

            // TAAの履歴バッファ2枚。読みながら同じテクスチャへ書けないので、毎フレーム役割を入れ替える
            // (m_TAAHistoryIndexが今フレームの書き込み先)。バッファ精度をLegacy8bitに落としても
            // m_SceneColorと同じく常にfp16のままにする。履歴は何十フレームぶんもの蓄積結果であり、
            // ここを8bitにすると量子化誤差が積み上がってバンディングになるため
            m_TAAHistory[0] = m_Device->CreateRenderTexture(width, height, RHI::Format::R16G16B16A16_Float);
            m_TAAHistory[1] = m_Device->CreateRenderTexture(width, height, RHI::Format::R16G16B16A16_Float);

            m_HiZMipLevels = ComputeMipLevelCount(width, height);
            m_HiZTexture = m_Device->CreateHiZTexture(width, height, m_HiZMipLevels);
            m_HiZDebugMipLevel = 0;
            // 作り直した直後の中身は未定義。Hi-Zパスが1回走るまでオクルージョン判定を止める
            m_HiZValid = false;

            // タイルライトカリングのライトグリッド。タイル数は解像度に依存するためここで作り直す。
            // 端のタイルは部分的にしか埋まらないので切り上げる
            m_LightTileCountX = (width + kLightTileSize - 1) / kLightTileSize;
            m_LightTileCountY = (height + kLightTileSize - 1) / kLightTileSize;
            RHI::BufferDesc lightTileBufferDesc;
            lightTileBufferDesc.Usage = RHI::BufferUsage::StructuredRW;
            lightTileBufferDesc.SizeInBytes =
                static_cast<uint32_t>(sizeof(uint32_t)) * kLightTileStride * m_LightTileCountX * m_LightTileCountY;
            lightTileBufferDesc.StrideInBytes = static_cast<uint32_t>(sizeof(uint32_t));
            m_LightTileBuffer = m_Device->CreateBuffer(lightTileBufferDesc);

            // MegaLightsの候補プール。タイルの切り方はライトグリッドと同じで、1タイルあたりの
            // 要素数だけが違う。非対応環境ではパス自体が走らないので確保しない
            if (m_RaytracingAvailable)
            {
                RHI::BufferDesc tilePoolBufferDesc;
                tilePoolBufferDesc.Usage = RHI::BufferUsage::StructuredRW;
                tilePoolBufferDesc.SizeInBytes = static_cast<uint32_t>(sizeof(uint32_t)) *
                                                 kMegaLightsTilePoolStride * m_LightTileCountX * m_LightTileCountY;
                tilePoolBufferDesc.StrideInBytes = static_cast<uint32_t>(sizeof(uint32_t));
                m_MegaLightsTilePoolBuffer = m_Device->CreateBuffer(tilePoolBufferDesc);

                // 1画素につきN本のリザーバ(1本16バイト)。MegaLightsCommon.hlsli の
                // MegaLightsReservoir と**ストライドを一致させること**。
                //
                // 【手法に関わらずクアッドの標本数で確保する】ここで手法を見て 1 と N を
                // 切り替えると、手法を切り替えるたびに確保し直しが要る。常に大きい側で
                // 取っておけば、手法2は先頭の 幅x高さ 本だけを使う形になり無駄なだけで安全。
                // 定数バッファへ渡す値(MegaLightsSamplesPerPixel())は手法3以外で1になるので、
                // **確保 >= 実際に使う本数** が常に成り立つ
                m_MegaLightsAllocatedSamplesPerPixel =
                    std::clamp(m_MegaLightsQuadSamplesPerPixel, 1, kMegaLightsMaxSamplesPerPixel);
                RHI::BufferDesc reservoirBufferDesc;
                reservoirBufferDesc.Usage = RHI::BufferUsage::StructuredRW;
                reservoirBufferDesc.SizeInBytes = static_cast<uint32_t>(sizeof(uint32_t) * 4) * width * height *
                                                  static_cast<uint32_t>(m_MegaLightsAllocatedSamplesPerPixel);
                reservoirBufferDesc.StrideInBytes = static_cast<uint32_t>(sizeof(uint32_t) * 4);
                m_MegaLightsReservoirBuffer = m_Device->CreateBuffer(reservoirBufferDesc);

                // 画素ごとの「遮蔽が確定した灯」のキャッシュ(uint。0xFFFFFFFFで無し)。
                // 殺しの持ち回りより寿命が長く、影の縁の暗いフリンジを消すのに要る
                // (MegaLightsInitialSample.hlsl の BlockedLights のコメント)
                RHI::BufferDesc blockedBufferDesc;
                blockedBufferDesc.Usage = RHI::BufferUsage::StructuredRW;
                blockedBufferDesc.SizeInBytes = static_cast<uint32_t>(sizeof(uint32_t)) * width * height;
                blockedBufferDesc.StrideInBytes = static_cast<uint32_t>(sizeof(uint32_t));
                m_MegaLightsBlockedLightBuffer = m_Device->CreateBuffer(blockedBufferDesc);
                // 空間再利用の出力先。近傍を読むので入力と同じバッファへは書けない。
                // 2回以上回すときは2本を ping-pong する
                m_MegaLightsReservoirSpatialBuffer = m_Device->CreateBuffer(reservoirBufferDesc);
                m_MegaLightsReservoirSpatialBuffer2 = m_Device->CreateBuffer(reservoirBufferDesc);

                // 時間再利用の履歴。**2本のping-pongにするのは、RenderGraphがWARの辺を
                // 張らないため**。1本で済ませると「今フレームのTemporalが読んだ直後に
                // 同じバッファへ書く」形になり、条件分岐でパスが1つ消えた瞬間に静かに壊れる。
                // 2本なら全ての辺がRAWで張れる(前フレームが書いた側を読み、今フレームは
                // もう片方へ書く)
                for (auto& buffer : m_MegaLightsReservoirHistory)
                {
                    buffer = m_Device->CreateBuffer(reservoirBufferDesc);
                }

                // 履歴の幾何(前フレームの法線・線形深度・材質)。
                // 【なぜ専用に持つのか】G-Bufferは毎フレーム上書きされ、前フレームの写しは
                // どこにも残らない。再投影先が「同じ面か」を判定するには前フレームの幾何が要る。
                // 1画素12バイト(法線oct 4 + View空間Z 4 + 材質 4)。
                // MegaLightsCommon.hlsli の MegaLightsHistoryGuide とストライドを一致させること
                RHI::BufferDesc guideBufferDesc;
                guideBufferDesc.Usage = RHI::BufferUsage::StructuredRW;
                guideBufferDesc.SizeInBytes = static_cast<uint32_t>(sizeof(uint32_t) * 3) * width * height;
                guideBufferDesc.StrideInBytes = static_cast<uint32_t>(sizeof(uint32_t) * 3);
                for (auto& buffer : m_MegaLightsHistoryGuide)
                {
                    buffer = m_Device->CreateBuffer(guideBufferDesc);
                }
                // 【履歴を無効にする】解像度が変わると添字の意味が変わり、前フレームの内容は
                // 別の画素のものになる。RHIにバッファのクリアが無いので、初回は
                // シェーダ側で「履歴を使わない」と判断させる
                m_MegaLightsHistoryValid = false;
                // デノイザの作業用テクスチャ。整数フォーマットが無いRHIなのですべてfloat。
                // 【履歴もping-pongにする】RenderGraphはWARの辺を張らないので、
                // 読む側と書く側が同じだと条件分岐でパスが消えた瞬間に静かに壊れる
                for (int denoiseIndex = 0; denoiseIndex < 2; ++denoiseIndex)
                {
                    m_MegaLightsDenoiseHistory[denoiseIndex] =
                        m_Device->CreateUAVTexture(width, height, RHI::Format::R32G32B32A32_Float);
                    m_MegaLightsDenoiseMoments[denoiseIndex] =
                        m_Device->CreateUAVTexture(width, height, RHI::Format::R32G32B32A32_Float);
                    m_MegaLightsDenoisePing[denoiseIndex] =
                        m_Device->CreateUAVTexture(width, height, RHI::Format::R32G32B32A32_Float);
                    m_MegaLightsDenoiseMomentPing[denoiseIndex] =
                        m_Device->CreateUAVTexture(width, height, RHI::Format::R32G32B32A32_Float);
                }
                m_MegaLightsDenoisedTexture =
                    m_Device->CreateUAVTexture(width, height, RHI::Format::R32G32B32A32_Float);
                // 解像度が変わると履歴の添字の意味が変わる。バッファのクリアが無いRHIなので、
                // シェーダ側へ「履歴を読むな」と伝える
                m_MegaLightsDenoiseHistoryValid = false;
            }

            // MegaLightsの蓄積バッファ(計測専用)。1画素につきfloat4。
            // 非対応環境でも、Presentがt6へ張るための1要素のダミーとして必ず作る
            // (DX12はSetPipelineStateのたびにルート引数が無効化されるため、シェーダが
            // 宣言しているリソースを未バインドのままDrawできない)
            {
                const uint32_t accumElements = m_RaytracingAvailable ? (width * height) : 1u;
                RHI::BufferDesc accumBufferDesc;
                accumBufferDesc.Usage = RHI::BufferUsage::StructuredRW;
                accumBufferDesc.SizeInBytes = static_cast<uint32_t>(sizeof(float) * 4) * accumElements;
                accumBufferDesc.StrideInBytes = static_cast<uint32_t>(sizeof(float) * 4);
                m_MegaLightsAccumBuffer = m_Device->CreateBuffer(accumBufferDesc);
            }
            // 解像度が変わると添字の意味が変わるので、蓄積も書き出しも必ず取り直す。
            // 【書き出し済みフラグも戻すこと】起動直後は既定解像度から実際のウィンドウサイズへ
            // 切り替わる。戻さないと、切り替わる前の低解像度のまま1回書き出して終わってしまう
            m_MegaLightsAccumFrames = 0;
            m_MegaLightsAccumWarmupFrames = 0;
            m_MegaLightsDumpIssued = false;
            m_MegaLightsDumpDone = false;
            m_MegaLightsDumpCopyFrame = 0;
            m_MegaLightsAccumReadback.reset();
            m_LightTileOverflowLogged = false;

            // ブルームのピラミッド。第0段が半解像度で、以降1段ごとに半分になる。
            // 1x1まで落とさず段数を固定しているのは、これ以上小さくしても裾の広がりが
            // 見た目に寄与しないため(解像度が低いと逆にアップサンプル時のちらつき源になる)。
            // レベルごとに独立したテクスチャにしている理由はBloom.hlsl冒頭を参照
            m_BloomLevelSizes.clear();
            m_BloomDownTextures.clear();
            m_BloomUpTextures.clear();
            uint32_t bloomWidth = std::max(1u, width / 2);
            uint32_t bloomHeight = std::max(1u, height / 2);
            for (uint32_t level = 0; level < kBloomLevelCount; ++level)
            {
                m_BloomLevelSizes.push_back({ bloomWidth, bloomHeight });
                // アルファを使わないHDRバッファなのでR11G11B10_Floatで足りる。
                // Legacy8bit構成でもブルームはHDR値を扱う必要があるためここは常にHDRのままにする
                // (8bitにすると1.0でクリップされ、ブルームの意味が失われる)
                m_BloomDownTextures.push_back(
                    m_Device->CreateUAVTexture(bloomWidth, bloomHeight, RHI::Format::R16G16B16A16_Float));
                m_BloomUpTextures.push_back(
                    m_Device->CreateUAVTexture(bloomWidth, bloomHeight, RHI::Format::R16G16B16A16_Float));

                bloomWidth = std::max(1u, bloomWidth / 2);
                bloomHeight = std::max(1u, bloomHeight / 2);
            }

            // 自前ソフトウェアラスタライザ(46章)の解像度依存リソース。
            //
            // 【内側で捕まえる】ここが落ちてもエンジン全体を止める理由が無い比較用の機能なので、
            // 外側のLegacy8bitフォールバックへ持ち出さず、この機能だけ無効化して続行する
            // (フォールバックしたところでVRAM不足は解決しない。m_PlanarReflectionColorと同じ判断)
            if (m_SoftwareRasterAvailable)
            {
                try
                {
                    // visibility buffer。画素あたり64bit(上位32bit=深度、下位32bit=三角形番号)。
                    // CSRasterがUAVで書き、CSResolveがSRVで読むためStructuredRW
                    RHI::BufferDesc visibilityDesc;
                    visibilityDesc.Usage = RHI::BufferUsage::StructuredRW;
                    visibilityDesc.SizeInBytes = static_cast<uint32_t>(sizeof(uint64_t)) * width * height;
                    visibilityDesc.StrideInBytes = static_cast<uint32_t>(sizeof(uint64_t));
                    m_SoftwareRasterVisibilityBuffer = m_Device->CreateBuffer(visibilityDesc);

                    // 【フォーマットはハードウェア側と揃える】色はHDR(Present Mode 4)、
                    // 深度は生値(Mode 5)、法線はm_GBufferNormalと同じR16G16_Floatの
                    // オクタヘドラル符号化(Mode 7)。揃えていないと差分が取れない
                    m_SoftwareRasterColor =
                        m_Device->CreateUAVTexture(width, height, RHI::Format::R16G16B16A16_Float);
                    m_SoftwareRasterDepth = m_Device->CreateUAVTexture(width, height, RHI::Format::R32_Float);
                    m_SoftwareRasterNormal = m_Device->CreateUAVTexture(width, height, RHI::Format::R16G16_Float);
                }
                catch (const std::exception& e)
                {
                    Core::Logger::Error(
                        "KurenaiEngine3D",
                        std::string("ソフトウェアラスタライザのリソース作成に失敗したため無効にします (") +
                            std::to_string(width) + "x" + std::to_string(height) + "): " + e.what());
                    m_SoftwareRasterAvailable = false;
                    m_SoftwareRasterVisibilityBuffer.reset();
                    m_SoftwareRasterColor.reset();
                    m_SoftwareRasterDepth.reset();
                    m_SoftwareRasterNormal.reset();
                }
            }
        }
        catch (const std::exception& e)
        {
            // Legacy8bit構成でも失敗する場合は、このエンジンが前提とする最低限の
            // フォーマット(R8G8B8A8_UNorm等)すら作れていないため復旧手段が無い
            if (legacyPrecision)
            {
                Core::Logger::Error(
                    "KurenaiEngine3D",
                    std::string("レンダーターゲットの作成に失敗しました (") + std::to_string(width) + "x" +
                        std::to_string(height) + ", バッファ精度=Legacy8bit): " + e.what());
                throw;
            }

            // R11G11B10_Float / R16G16B16A16_Float のいずれかが
            // このデバイスでレンダーターゲットとして使えない場合の保険。8bit構成へ落として続行する
            // (画質は落ちるが起動できなくなるよりはよい)
            Core::Logger::Error(
                "KurenaiEngine3D",
                std::string("HDR精度のレンダーターゲット作成に失敗したためLegacy8bit構成へフォールバックします (") +
                    std::to_string(width) + "x" + std::to_string(height) + "): " + e.what());
            m_BufferPrecision = BufferPrecision::Legacy8bit;
            CreateRenderTargets(width, height);
            return;
        }

        // 履歴バッファを作り直した直後は中身が未定義なので、TAAへ「今フレームは履歴を使うな」と伝える。
        // fp16の未初期化領域はNaNのことがあり、lerp(NaN, x, 1.0)もNaNになるため、
        // ブレンド率を0にするだけでは足りず「サンプルそのものを行わない」必要がある(TAA.hlsl参照)
        m_TAAHistoryValid = false;
        m_TAAHistoryIndex = 0;

        // A/B比較の記録用。どちらの構成で描かれたスクリーンショットなのかをログから追えるようにする
        Core::Logger::Info(
            "KurenaiEngine3D",
            std::string("レンダーターゲットを作成しました (") + std::to_string(width) + "x" + std::to_string(height) +
                ", バッファ精度=" + (legacyPrecision ? "Legacy8bit" : "HDR") + ")");
    }

    void KurenaiEngine3D::CreatePlanarReflectionTargets()
    {
        if (m_RenderWidth == 0 || m_RenderHeight == 0)
        {
            return;
        }

        // 反射解像度 = レンダー解像度 × 倍率。最低でも1x1は確保する
        // (倍率が非常に小さい・レンダー解像度が非常に小さい場合でもテクスチャ作成自体は失敗させない)
        const uint32_t width = std::max(
            1u, static_cast<uint32_t>(static_cast<float>(m_RenderWidth) * m_PlanarReflectionResolutionScale));
        const uint32_t height = std::max(
            1u, static_cast<uint32_t>(static_cast<float>(m_RenderHeight) * m_PlanarReflectionResolutionScale));

        try
        {
            // SceneColorと同じHDR形式(R16G16B16A16_Float)。水面はラフネスが低く反射がそのまま
            // 見えるため、CreateRenderTargetsのLegacy8bitフォールバックの対象外にして常にHDR固定にする
            m_PlanarReflectionColor = m_Device->CreateRenderTexture(width, height, RHI::Format::R16G16B16A16_Float);
            // Reverse-Zのため遠平面側(NDC z=0.0)にクリアする(G-Buffer/ProbeCapture深度と同じ)
            m_PlanarReflectionDepth = m_Device->CreateDepthTexture(width, height, 0.0f);
        }
        catch (const std::exception& e)
        {
            Core::Logger::Error(
                "KurenaiEngine3D",
                std::string("平面反射のレンダーターゲット作成に失敗しました (") + std::to_string(width) + "x" +
                    std::to_string(height) + "): " + e.what());
            throw;
        }

        m_PlanarReflectionWidth = width;
        m_PlanarReflectionHeight = height;

        Core::Logger::Info(
            "KurenaiEngine3D",
            std::string("平面反射のレンダーターゲットを作成しました (") + std::to_string(width) + "x" +
                std::to_string(height) + ")");
    }

    void KurenaiEngine3D::ExecuteSoftwareRasterPass(
        RHI::IRHICommandList* cmd,
        const DirectX::XMMATRIX& viewProj,
        const DirectX::XMFLOAT3& sunDirection)
    {
        // --- メッシュレコードを組み直す ---------------------------------------------------
        //
        // 描画用の頂点/インデックスバッファはbindlessで直接引けるので(ModelLoader参照)、
        // ここで作るのは「どのメッシュがどのbindless番号を持ち、通し三角形番号のどこから
        // 始まるか」の表だけ。数百件のオーダーなので毎フレーム組み直して構わない
        std::vector<SWRasterMeshInfo> meshInfos;
        meshInfos.reserve(64);

        uint32_t firstTriangle = 0;
        bool overflowed = false;

        const FrustumPlanes swRasterFrustum = ExtractFrustumPlanes(viewProj);

        for (size_t instanceIndex = 0; instanceIndex < m_Scene.Instances.size(); ++instanceIndex)
        {
            const Assets::ModelInstance& instance = m_Scene.Instances[instanceIndex];
            ++m_FrustumCullTested;
            if (!IsAABBVisible(swRasterFrustum, instance.WorldBoundsMin, instance.WorldBoundsMax))
            {
                ++m_FrustumCullCulled;
                continue;
            }

            // クロスディザ非対応の経路なので、フェード中でも段は1つに決め打つ
            // ストリーミング中で未読み込みなら描かない
            const Assets::Model* const currentModel = GetCurrentLOD(instanceIndex);
            if (!currentModel) { continue; }
            for (const auto& mesh : currentModel->Meshes)
            {
                // 半透明(alphaMode=BLEND)はハードウェア側でもG-Bufferに描かれないため揃える
                if (mesh.IsTransparent || mesh.IndexCount < 3)
                {
                    continue;
                }

                // メッシュ単位のカリング。統計はモデル単位とは別カウンタへ入れる。
                // 【描かないメッシュを弾いた後に置く】分母を「このパスが実際に描くメッシュ」に
                // 揃えないと、間引き率が薄まって効きが読めなくなる
                if (!IsMeshVisibleWithStats(
                        m_MeshCullingEnabled, swRasterFrustum, instance, *currentModel, mesh, m_MeshCullTested, m_MeshCullCulled))
                {
                    continue;
                }

                const uint32_t vertexBufferIndex =
                    mesh.VertexBuffer ? mesh.VertexBuffer->GetBindlessIndex() : RHI::kInvalidBindlessIndex;
                const uint32_t indexBufferIndex =
                    mesh.IndexBuffer ? mesh.IndexBuffer->GetBindlessIndex() : RHI::kInvalidBindlessIndex;
                // bindless登録が無いメッシュ(ShaderReadableを指定せずに作られた等)は引けない。
                // シェーダー側で無効番号を判定する手段が無いため、ここで落とす
                if (vertexBufferIndex == RHI::kInvalidBindlessIndex ||
                    indexBufferIndex == RHI::kInvalidBindlessIndex)
                {
                    continue;
                }

                if (meshInfos.size() >= kSWRasterMaxMeshes)
                {
                    overflowed = true;
                    break;
                }

                SWRasterMeshInfo info{};
                info.World = instance.World;
                info.NormalMatrix = instance.NormalMatrix;
                info.VertexBufferIndex = vertexBufferIndex;
                info.IndexBufferIndex = indexBufferIndex;
                info.FirstTriangle = firstTriangle;
                info.TriangleCount = mesh.IndexCount / 3;
                // ミラーリングされたインスタンスはワインディングが反転する。ハードウェア側が
                // FrontCounterClockwise=trueの別PSOで描いているのと同じ対処をしないと、
                // 鏡像配置のモデルだけ表裏が入れ替わって消える
                info.FrontFaceSign = instance.IsMirrored ? -1.0f : 1.0f;
                info.Flags = 0;

                firstTriangle += info.TriangleCount;
                meshInfos.push_back(info);
            }

            if (overflowed)
            {
                break;
            }
        }

        if (overflowed && !m_SoftwareRasterMeshOverflowLogged)
        {
            // 毎フレーム出続けるのを避けるため最初の1回だけ報告する(m_LightTileOverflowLoggedと同じ作法)
            m_SoftwareRasterMeshOverflowLogged = true;
            Core::Logger::Warning(
                "KurenaiEngine3D",
                "ソフトウェアラスタライザのメッシュ数が上限(" + std::to_string(kSWRasterMaxMeshes) +
                    ")を超えました。超過分は描画されません");
        }

        if (meshInfos.empty())
        {
            // 描くものが1つも無くても、visibility bufferは必ずクリアしてから戻る。
            //
            // 【クリアせずに戻ってはいけない】このバッファは散布書き込みで、三角形が当たらなかった
            // 画素には前フレームの値が残る(下の「0. クリア」のコメント参照)。カリングで全インスタンスが
            // 落ちたフレームだけ前フレームの絵が焼き付いて残る、という形で出る
            cmd->ClearUnorderedAccessBufferUint(m_SoftwareRasterVisibilityBuffer.get(), 0);
            cmd->ClearUnorderedAccessBufferUint(m_SoftwareRasterIndirectArgsBuffer.get(), 0);
            return;
        }

        cmd->UpdateBuffer(
            m_SoftwareRasterMeshInfoBuffer.get(),
            meshInfos.data(),
            meshInfos.size() * sizeof(SWRasterMeshInfo));

        // --- 定数バッファ -----------------------------------------------------------------

        const uint32_t totalTriangles = firstTriangle;

        // Dispatchの1次元あたりの上限は65535。三角形数はシーン読み込み時に確定する静的な値なので
        // CPUが持てばよく、ここを間接ディスパッチにする理由は無い(巨大三角形の個数と違って
        // GPU上でしか分からない値ではない)
        const uint32_t groupsTotal = (totalTriangles + kSWRasterGroupSize - 1) / kSWRasterGroupSize;
        const uint32_t groupsX = std::min(groupsTotal, kSWRasterMaxGroupsPerAxis);
        const uint32_t groupsY = (groupsTotal + groupsX - 1) / groupsX;

        SWRasterConstants constants{};
        DirectX::XMStoreFloat4x4(&constants.ViewProj, DirectX::XMMatrixTranspose(viewProj));
        constants.RenderSize = {
            static_cast<float>(m_RenderWidth),
            static_cast<float>(m_RenderHeight),
            1.0f / static_cast<float>(m_RenderWidth),
            1.0f / static_cast<float>(m_RenderHeight),
        };
        constants.SunDirection = { sunDirection.x, sunDirection.y, sunDirection.z, 0.0f };
        constants.DispatchParams = {
            groupsX,
            totalTriangles,
            static_cast<uint32_t>(meshInfos.size()),
            static_cast<uint32_t>(std::clamp(
                m_SoftwareRasterLargeTriangleArea,
                static_cast<int>(kSWRasterMinLargeTriangleArea),
                static_cast<int>(kSWRasterMaxLargeTriangleArea))),
        };
        constants.LargeParams = { kSWRasterLargeListCapacity, 0u, 0u, 0u };

        cmd->UpdateBuffer(m_SoftwareRasterConstantBuffer.get(), &constants, sizeof(constants));

        // --- 0. クリア --------------------------------------------------------------------
        //
        // visibility bufferは散布書き込みなので、三角形が当たらなかった画素には前フレームの値が
        // 残る。0は「深度0 = 遠平面 = 当たり無し」を意味する(SWRasterPackVisibility参照)。
        // 間接ディスパッチ引数も、X成分をカウンタとして使うため毎フレーム0へ戻す必要がある
        cmd->ClearUnorderedAccessBufferUint(m_SoftwareRasterVisibilityBuffer.get(), 0);
        cmd->ClearUnorderedAccessBufferUint(m_SoftwareRasterIndirectArgsBuffer.get(), 0);

        // --- 1. CSRaster: 1スレッド = 1三角形 ---------------------------------------------
        //
        // 【UAVはディスパッチごとに張り直す】Dispatch直後に全スロットが自動解除されるため
        // (IRHICommandList::SetComputeUnorderedAccessTextureのコメント)
        cmd->SetComputePipelineState(m_SoftwareRasterPipelineState.get());
        cmd->SetComputeConstantBuffer(1, m_SoftwareRasterConstantBuffer.get());
        cmd->SetComputeShaderResourceBuffer(0, m_SoftwareRasterMeshInfoBuffer.get());
        cmd->SetComputeUnorderedAccessBuffer(0, m_SoftwareRasterVisibilityBuffer.get());
        cmd->SetComputeUnorderedAccessBuffer(1, m_SoftwareRasterLargeEntriesBuffer.get());
        cmd->SetComputeUnorderedAccessBuffer(2, m_SoftwareRasterIndirectArgsBuffer.get());
        cmd->Dispatch(groupsX, groupsY, 1);

        // --- 2. CSRasterLarge: 1スレッドグループ = 巨大三角形1個 --------------------------
        //
        // 巨大三角形の個数はGPU上でしか分からないため、グループ数をCPUから書けない。
        // これが間接ディスパッチをRHIへ足した理由。
        // 【引数バッファをUAVに張らない】DispatchIndirectは引数バッファを
        // INDIRECT_ARGUMENT状態へ遷移させるので、同じディスパッチのUAVスロットに
        // 張ったままにはできない(DX12CommandList::DispatchIndirectのコメント)
        cmd->SetComputePipelineState(m_SoftwareRasterLargePipelineState.get());
        cmd->SetComputeConstantBuffer(1, m_SoftwareRasterConstantBuffer.get());
        cmd->SetComputeShaderResourceBuffer(0, m_SoftwareRasterMeshInfoBuffer.get());
        cmd->SetComputeShaderResourceBuffer(1, m_SoftwareRasterLargeEntriesBuffer.get());
        cmd->SetComputeUnorderedAccessBuffer(0, m_SoftwareRasterVisibilityBuffer.get());
        cmd->DispatchIndirect(m_SoftwareRasterIndirectArgsBuffer.get(), 0);

        // --- 3. CSResolve: 1スレッド = 1画素 ----------------------------------------------
        //
        // visibility bufferの三角形番号からジオメトリを引き直し、深度・法線・陰影を書く
        constexpr uint32_t kResolveGroupSize = 8; // SoftwareRasterResolve.hlslと一致させること
        cmd->SetComputePipelineState(m_SoftwareRasterResolvePipelineState.get());
        cmd->SetComputeConstantBuffer(1, m_SoftwareRasterConstantBuffer.get());
        cmd->SetComputeShaderResourceBuffer(0, m_SoftwareRasterMeshInfoBuffer.get());
        cmd->SetComputeShaderResourceBuffer(1, m_SoftwareRasterVisibilityBuffer.get());
        cmd->SetComputeUnorderedAccessTexture(0, m_SoftwareRasterColor.get());
        cmd->SetComputeUnorderedAccessTexture(1, m_SoftwareRasterDepth.get());
        cmd->SetComputeUnorderedAccessTexture(2, m_SoftwareRasterNormal.get());
        cmd->SetComputeUnorderedAccessBuffer(3, m_SoftwareRasterIndirectArgsBuffer.get());
        cmd->Dispatch(
            (m_RenderWidth + kResolveGroupSize - 1) / kResolveGroupSize,
            (m_RenderHeight + kResolveGroupSize - 1) / kResolveGroupSize,
            1);
    }

    void KurenaiEngine3D::RequestRenderResolution(uint32_t width, uint32_t height)
    {
        // 上限はHi-Zのミップ構築・ライトタイル・ブルームピラミッドがいずれも
        // D3Dのテクスチャ上限(16384)以内で完結することを保証するための保険。
        // 実際にはそのはるか手前でVRAMが尽きるが、その場合はRender()側が元の解像度へ戻す
        constexpr uint32_t kMaxRenderSize = 16384;
        if (width == 0 || height == 0 || width > kMaxRenderSize || height > kMaxRenderSize)
        {
            Core::Logger::Error(
                "KurenaiEngine3D",
                "RequestRenderResolution: 解像度" + std::to_string(width) + "x" + std::to_string(height) +
                    "が範囲外です(1〜" + std::to_string(kMaxRenderSize) + ")。要求を無視します");
            return;
        }

        if (width == m_RenderWidth && height == m_RenderHeight)
        {
            // 同じ解像度への要求はレンダーターゲットの作り直し(とTAA履歴の破棄)を伴うだけで
            // 何も変わらないため無視する
            return;
        }

        m_PendingRenderWidth = width;
        m_PendingRenderHeight = height;
        m_RenderResolutionDirty = true;
    }

    float KurenaiEngine3D::GetUpscaleRatio(UpscaleQualityMode mode)
    {
        // FSR1が定義している4段。倍率は「出力の一辺 ÷ 入力の一辺」
        switch (mode)
        {
        case UpscaleQualityMode::UltraQuality: return 1.3f;
        case UpscaleQualityMode::Quality:      return 1.5f;
        case UpscaleQualityMode::Balanced:     return 1.7f;
        case UpscaleQualityMode::Performance:  return 2.0f;
        default:
            Core::Logger::Error(
                "KurenaiEngine3D",
                "GetUpscaleRatio: 未知の品質モード(" + std::to_string(static_cast<int>(mode)) +
                    ")です。Quality(1.5倍)として扱います");
            return 1.5f;
        }
    }

    void KurenaiEngine3D::ComputeUpscaleRenderResolution(
        uint32_t outputWidth, uint32_t outputHeight, UpscaleQualityMode mode,
        uint32_t& outRenderWidth, uint32_t& outRenderHeight)
    {
        const float ratio = GetUpscaleRatio(mode);

        // 8の倍数へ切り捨てる。LightCullのタイル・Hi-Zのミップ連鎖・Bloomのピラミッド・
        // SkyCloud/DDGIResolveの1/2解像度がいずれも2の冪で割っていくため、
        // 半端な解像度にすると端の1〜2画素の扱いがパスごとにずれる。
        // 丸めた結果アスペクト比が出力とわずかにずれる(1920x1080の1.7倍で1128x632、
        // 1.7848対1.7778で0.4%)が、EASUは入力矩形を出力矩形へ写すだけなのでこの差は
        // 微小な引き伸ばしとして吸収され、視認できない
        constexpr uint32_t kMinRenderSize = 320;
        constexpr uint32_t kMinRenderHeight = 180;
        const uint32_t rawWidth = static_cast<uint32_t>(static_cast<float>(outputWidth) / ratio);
        const uint32_t rawHeight = static_cast<uint32_t>(static_cast<float>(outputHeight) / ratio);
        outRenderWidth = std::max(kMinRenderSize, rawWidth & ~7u);
        outRenderHeight = std::max(kMinRenderHeight, rawHeight & ~7u);
    }

    float KurenaiEngine3D::ComputeRcasSharpnessScale(float sharpness)
    {
        // FSR1のsharpnessは「シャープさを何ストップ(=半分に)落とすか」で、0が最大・大きいほど弱い。
        // UI側は「0で無効、1で最強」のほうが直感的なので、ここで向きと尺度を変換する。
        // 2ストップ(=1/4)を弱い側の端にしているのは、それ以上落とすと見た目の変化が無くなるため
        const float clamped = std::clamp(sharpness, 0.0f, 1.0f);
        if (clamped <= 0.0f)
        {
            // 完全に0のときはlobeごと0になるようにする(exp2(-2)=0.25では弱いシャープが残る)
            return 0.0f;
        }
        return std::exp2(-2.0f * (1.0f - clamped));
    }

    void KurenaiEngine3D::RequestUpscaleSettings(
        bool enabled, UpscaleQualityMode mode, uint32_t outputWidth, uint32_t outputHeight)
    {
        if (outputWidth == 0 || outputHeight == 0)
        {
            Core::Logger::Error(
                "KurenaiEngine3D",
                "RequestUpscaleSettings: 出力解像度" + std::to_string(outputWidth) + "x" +
                    std::to_string(outputHeight) + "が不正です。要求を無視します");
            return;
        }

        m_UpscaleEnabled = enabled;
        m_UpscaleQualityMode = mode;
        m_UpscaleOutputWidth = outputWidth;
        m_UpscaleOutputHeight = outputHeight;

        if (enabled)
        {
            uint32_t renderWidth = 0;
            uint32_t renderHeight = 0;
            ComputeUpscaleRenderResolution(outputWidth, outputHeight, mode, renderWidth, renderHeight);
            RequestRenderResolution(renderWidth, renderHeight);
            // 出力解像度用のテクスチャがまだ無い、またはサイズが変わったときだけ作り直す
            if (m_UpscaleTargetWidth != outputWidth || m_UpscaleTargetHeight != outputHeight)
            {
                m_UpscaleTargetsDirty = true;
            }
        }
        else
        {
            // 無効化したときは内部解像度を出力解像度と同じに戻す。こうしないと
            // 「超解像を切ったのに低解像度のまま」という状態が残る
            RequestRenderResolution(outputWidth, outputHeight);
            // 使わなくなったテクスチャは解放する(1080pで約8MBが2枚)
            if (m_UpscaleTargetWidth != 0 || m_UpscaleTargetHeight != 0)
            {
                m_UpscaleTargetsDirty = true;
            }
        }
    }

    void KurenaiEngine3D::CreateUpscaleTargets(uint32_t width, uint32_t height)
    {
        // 無効化された場合は解放だけして戻る
        if (!m_UpscaleEnabled)
        {
            m_UpscaleTexture.reset();
            m_UpscaleSharpTexture.reset();
            m_UpscaleTargetWidth = 0;
            m_UpscaleTargetHeight = 0;
            return;
        }

        // Tonemapの出力と同じR8G8B8A8_UNorm。EASU/RCASはどちらも表示レンジの値を前提にしており、
        // ここをHDRフォーマットにしても情報は増えない(入力が既にLDRのため)。
        //
        // 【型付きUAVのフォーマット制約には当たらない】このエンジンが各所で注記している
        // 「R32系しか保証されていない」という制約は型付きUAVからの"読み出し"のもので、
        // EASU/RCASはUAVへ書くだけである(RCASがEASUの結果を読むのはSRV経由)。
        // Bloomが同じくR16G16B16A16_FloatのUAVへ書けているのと同じ理屈
        m_UpscaleTexture = m_Device->CreateUAVTexture(width, height, RHI::Format::R8G8B8A8_UNorm);
        m_UpscaleSharpTexture = m_Device->CreateUAVTexture(width, height, RHI::Format::R8G8B8A8_UNorm);
        m_UpscaleTargetWidth = width;
        m_UpscaleTargetHeight = height;
    }

    bool KurenaiEngine3D::IsUpscaleActive() const
    {
        // テクスチャの確保に失敗している場合にパスを登録すると、バインドするリソースが無いまま
        // Dispatchすることになるため、確保済みであることまで条件に入れる
        return m_UpscaleEnabled && m_UpscaleTexture && m_UpscaleSharpTexture &&
               m_UpscaleTargetWidth > 0 && m_UpscaleTargetHeight > 0;
    }

    void KurenaiEngine3D::RequestPlanarReflectionResolutionScale(float scale)
    {
        // 0以下はテクスチャが確保できない。上限を1.0(等倍)にしているのは、水面はラフネスが
        // 低くても波の法線で歪むため等倍を超える解像度に意味が無いため(EngineDefaults.h参照)
        if (scale <= 0.0f || scale > 1.0f)
        {
            Core::Logger::Error(
                "KurenaiEngine3D",
                "RequestPlanarReflectionResolutionScale: 倍率" + std::to_string(scale) +
                    "が範囲外です(0より大きく1.0以下)。要求を無視します");
            return;
        }

        if (scale == m_PlanarReflectionResolutionScale)
        {
            return;
        }

        // レンダーターゲットの作り直しはGPUがまだ参照しているかもしれない状態では行えないため、
        // RequestRenderResolutionと同じく要求を記録するだけにしてRender()の先頭でまとめて反映する
        m_PendingPlanarReflectionResolutionScale = scale;
        m_PlanarReflectionResolutionDirty = true;
    }

    KurenaiEngine3D::QualitySettings KurenaiEngine3D::CaptureQualitySettings() const
    {
        QualitySettings settings;
        settings.Reflection = m_ReflectionMode;
        settings.PlanarReflectionEnabled = m_PlanarReflectionEnabled;
        settings.PlanarReflectionResolutionScale = m_PlanarReflectionResolutionScale;
        settings.CloudVolumetric = m_CloudVolumetric;
        settings.CirrusEnabled = m_CirrusEnabled;
        settings.StarsEnabled = m_StarsEnabled;
        settings.TAAEnabled = m_TAAEnabled;
        settings.BloomEnabled = m_BloomEnabled;
        settings.ScreenSpaceShadowEnabled = m_ScreenSpaceShadowEnabled;
        settings.DDGIProbesPerFrame = m_DDGIProbesPerFrame;
        settings.SSAOKernelSize = m_SSAOKernelSize;
        settings.CloudRaymarchSteps = m_CloudRaymarchSteps;
        settings.DDGIUpdate = m_DDGIUpdateMode;
        settings.DDGIHalfResolution = m_DDGIHalfResolution;
        return settings;
    }

    void KurenaiEngine3D::ApplyQualitySettings(const QualitySettings& settings)
    {
        m_ReflectionMode = settings.Reflection;
        m_PlanarReflectionEnabled = settings.PlanarReflectionEnabled;
        m_CloudVolumetric = settings.CloudVolumetric;
        m_CirrusEnabled = settings.CirrusEnabled;
        m_StarsEnabled = settings.StarsEnabled;
        m_TAAEnabled = settings.TAAEnabled;
        m_BloomEnabled = settings.BloomEnabled;
        m_ScreenSpaceShadowEnabled = settings.ScreenSpaceShadowEnabled;
        m_DDGIProbesPerFrame = settings.DDGIProbesPerFrame;
        // カーネル自体の作り直しはSSAOパスの中で行う(段数が変わったことを見て作り直す)
        m_SSAOKernelSize = settings.SSAOKernelSize;
        m_CloudRaymarchSteps = settings.CloudRaymarchSteps;
        m_DDGIHalfResolution = settings.DDGIHalfResolution;

        // 更新モードを変えたら停止状態は倒しておく。倒さないと「常時更新へ戻したのに
        // 止まったまま」になる(署名が変わるまで再開しないため)
        if (m_DDGIUpdateMode != settings.DDGIUpdate)
        {
            m_DDGIUpdateMode = settings.DDGIUpdate;
            m_DDGIUpdateSuspended = false;
            m_DDGIStableCycles = 0;
        }

        // 平面反射の解像度倍率だけはレンダーターゲットの作り直しを伴う。GPUがまだ参照している
        // 可能性があるためここでは直接代入せず、要求として積んでRender()の先頭で反映させる
        // (UI関数の中で直接リソースを作り直さない、という既存の作法に合わせる)
        RequestPlanarReflectionResolutionScale(settings.PlanarReflectionResolutionScale);
    }

    void KurenaiEngine3D::ApplyQualityPreset(QualityPreset preset)
    {
        m_QualityPreset = preset;

        // 「高」はシーンを読み込んだ直後の状態へ戻す(QualitySettingsのコメント参照)。
        // 静的な既定へ戻すと、SSRやTAAを自分で指定しているシーンの意図を壊す
        if (preset == QualityPreset::High)
        {
            ApplyQualitySettings(m_SceneDefaultQuality);
            Core::Logger::Info("KurenaiEngine3D", "品質プリセット「高」を適用しました(シーン読み込み直後の状態へ戻しました)");
            return;
        }

        // 「低」「中」はシーン既定を出発点にして、そこから重い項目だけを落とす。
        // シーンが元から無効にしているものを勝手に有効化しないよう、有効化は一切行わない
        QualitySettings settings = m_SceneDefaultQuality;

        // 実測でGIVolumeを持つシーンの最大負荷(40〜47ms、フレームの約4割)。
        // 1プローブにつきシーンを6回描くため、この値にほぼ比例する
        settings.DDGIProbesPerFrame = (preset == QualityPreset::Low) ? 2 : 4;
        // 焼き上がりが落ち着いたら止める。低は一巡だけ(最速で止まる代わりに間接光の
        // バウンスが1回ぶん)、中はkDDGIBounceCycles巡だけ焼いてから止める
        settings.DDGIUpdate = (preset == QualityPreset::Low) ? DDGIUpdateMode::OverwriteThenStop
                                                            : DDGIUpdateMode::ConvergeThenStop;
        // DDGIのサンプリングは実測でLightingパス23.9msのうち10.2msを占めていた。
        // 低解像度化は輪郭で滲む近似なので既定は無効だが、プリセットでは有効にする
        settings.DDGIHalfResolution = true;
        // 実測でジオメトリが画面を占めるシーンの4.8〜11.0ms。コストはほぼ段数に比例する。
        // シーン既定より増やすことはしない(プリセットは落とす方向のみ)
        settings.SSAOKernelSize = std::min(
            m_SceneDefaultQuality.SSAOKernelSize, (preset == QualityPreset::Low) ? 4u : 8u);
        // 実測31ms(水面のあるシーン)。低・中とも切る
        settings.Reflection = ReflectionMode::Off;
        settings.TAAEnabled = false;
        settings.BloomEnabled = false;
        settings.ScreenSpaceShadowEnabled = false;

        if (preset == QualityPreset::Low)
        {
            // ボリュメトリック積雲は実測で約10ms。低ではまるごと切る
            // (手続き雲そのものは残るので、空が真っ青になるわけではない。平面レイヤーへ落ちる)
            settings.CloudVolumetric = false;
            settings.PlanarReflectionEnabled = false;
            settings.CirrusEnabled = false;
            settings.StarsEnabled = false;
        }
        else
        {
            // 中はボリュームを残したまま段数だけ落とす。コストはほぼ段数に比例するため、
            // 「立体的な雲は残しつつ半分の値段にする」という中間段が作れる
            // (段数を実行時に変えられるようにしたのはこのため)。
            // SSAOの段数と同じく、シーン既定より増やすことはしない
            settings.CloudRaymarchSteps =
                std::min(m_SceneDefaultQuality.CloudRaymarchSteps, 6u);
        }
        // 平面反射は残す場合でも解像度を落とす。1.0を超える指定は
        // RequestPlanarReflectionResolutionScaleが弾くため、下げる方向のみで安全
        settings.PlanarReflectionResolutionScale =
            std::min(m_SceneDefaultQuality.PlanarReflectionResolutionScale, 0.25f);

        ApplyQualitySettings(settings);
        Core::Logger::Info(
            "KurenaiEngine3D",
            std::string("品質プリセット「") + (preset == QualityPreset::Low ? "低" : "中") + "」を適用しました");
    }

    void KurenaiEngine3D::RequestSceneLoad(size_t sceneIndex)
    {
        if (sceneIndex >= m_SceneFilePaths.size())
        {
            Core::Logger::Error(
                "KurenaiEngine3D",
                "RequestSceneLoad: シーン番号" + std::to_string(sceneIndex) + "が範囲外です(シーン数: " +
                    std::to_string(m_SceneFilePaths.size()) + ")。要求を無視します");
            return;
        }

        // UIパネルもRenderスレッドで動くため、ここは単なるRenderスレッド内の受け渡しでよい。
        // 実際の発注はUpdateSceneStreaming(フレーム先頭)がまとめて行う
        m_PendingSceneRequest = static_cast<int>(sceneIndex);
    }

    uint64_t KurenaiEngine3D::GetCurrentSceneFileWriteTime() const
    {
        if (m_CurrentSceneIndex >= m_SceneFilePaths.size())
        {
            return 0;
        }

        // DiscoverScenesがFindFirstFileWを使っているのと同じWin32の流儀に揃える。
        // <filesystem>は例外を投げるうえ、このコードベースでは1箇所でしか使っていない
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        if (!GetFileAttributesExW(m_SceneFilePaths[m_CurrentSceneIndex].c_str(), GetFileExInfoStandard, &attributes))
        {
            // 保存の瞬間にエディタがファイルを置き換えていると一時的に開けないことがある。
            // 0を返して「今回は見送る」ことで、次のポーリングが正しい値を拾う
            return 0;
        }

        return (static_cast<uint64_t>(attributes.ftLastWriteTime.dwHighDateTime) << 32) |
               static_cast<uint64_t>(attributes.ftLastWriteTime.dwLowDateTime);
    }

    void KurenaiEngine3D::UpdateSceneHotReloadWatch()
    {
        // 読み込み中・要求が既に積まれている場合は何もしない(多重発注を避ける)
        if (!m_SceneAutoReloadEnabled || m_SceneLoadInFlight || m_PendingSceneRequest >= 0)
        {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now < m_NextSceneWatchTime)
        {
            return;
        }
        m_NextSceneWatchTime = now + std::chrono::milliseconds(250);

        const uint64_t writeTime = GetCurrentSceneFileWriteTime();
        if (writeTime == 0 || writeTime == m_WatchedSceneWriteTime)
        {
            return;
        }

        // 【書式の検証を門番にする】保存の途中で書きかけのファイルを掴むと、LoadSceneが失敗して
        // シーンが空のまま残る(UpdateSceneStreamingはVRAMの二重常駐を避けるため、読み込みを
        // 始める前に旧シーンを手放す設計のため)。ValidateSceneはデバイスもジオメトリも要らない
        // 軽い検証なので、通ったときだけ発注することでこれを防ぐ。
        // 失敗した更新時刻は覚えておき、同じ内容で警告を繰り返さない(保存し直せば次の変更で拾う)
        const std::wstring assetRootDirectory = GetModuleDirectory() + L"Assets\\";
        try
        {
            Assets::ValidateScene(m_SceneFilePaths[m_CurrentSceneIndex], assetRootDirectory);
        }
        catch (const std::exception& e)
        {
            if (writeTime != m_SceneReloadRejectedWriteTime)
            {
                m_SceneReloadRejectedWriteTime = writeTime;
                Core::Logger::Warning(
                    "KurenaiEngine3D",
                    "シーンファイルの変更を検出しましたが、書式が不正なため再読み込みを見送りました("
                    "シーンはそのまま残ります): " + WideToUtf8(m_SceneFilePaths[m_CurrentSceneIndex]) + " : " + e.what());
            }
            return;
        }

        // 検証を通った時点で基準時刻を進める。発注が消費されるまでの数フレームで
        // 同じ変更を何度も拾わないようにするため、ApplyLoadedSceneの取り直しより先に行う
        m_WatchedSceneWriteTime = writeTime;
        Core::Logger::Info(
            "KurenaiEngine3D",
            "シーンファイルの変更を検出したため再読み込みします: " +
                WideToUtf8(m_SceneFilePaths[m_CurrentSceneIndex]));
        RequestSceneLoad(m_CurrentSceneIndex);
    }

    void KurenaiEngine3D::UpdateSceneStreaming()
    {
        // .ksceneのホットリロード。実際の読み込みは下の既存の経路にそのまま乗せる
        UpdateSceneHotReloadWatch();

        // --- 出来上がったシーンがあれば取り込む ---
        std::unique_ptr<LoadedScene> loaded;
        {
            std::lock_guard<std::mutex> lock(m_LoadedSceneMutex);
            loaded = std::move(m_LoadedScene);
        }
        if (loaded)
        {
            ApplyLoadedScene(*loaded);
            m_SceneLoadInFlight = false;
        }

        // --- 保留中の切り替え要求をLoaderスレッドへ発注する ---
        // 読み込み中は発注しない(最後の要求はm_PendingSceneRequestに残るので取りこぼさない)
        if (m_PendingSceneRequest < 0 || m_SceneLoadInFlight)
        {
            return;
        }

        const size_t sceneIndex = static_cast<size_t>(m_PendingSceneRequest);
        m_PendingSceneRequest = -1;

        // 【WaitForGPUIdleより前に止める】テクスチャストリーミングのワーカーは
        // 旧シーンのIRHITexture*を掴んだままGPUリソースを作っている。旧シーンを手放す前に
        // 必ず止めて、走っている要求を捨てる
        m_TextureStreaming.Reset();

        // 旧シーンのGPUリソースを手放す前に、GPUが旧シーンを参照するコマンド(直前まで提出されていた
        // 描画コマンド)の実行を終えるまで待つ。特にDX12はCPUがGPU完了を待たずに次フレームの記録を
        // 始める多重バッファリング設計のため、これを省くとGPUがまだ読んでいるバッファ/テクスチャを
        // 解放してしまう(詳細はIRHIDevice::WaitForGPUIdleのコメント参照)。
        // このフレームのGPUコマンドはまだ1つも積んでいないため、待ち時間は前フレームぶんだけで済む
        m_Device->WaitForGPUIdle();

        // 読み込み開始と同時に旧シーンを手放す。読み込み完了まで待ってから捨てると新旧の
        // GPUリソースが同時に載ってVRAMがほぼ2倍になるため、先に空にする方を選んでいる。
        // その代わり読み込み中はシーンが描かれない(UIとスカイボックスのみになる)
        RetiredAssets retired;
        retired.Scene = std::move(m_Scene);
        retired.RaytracingScene = std::move(m_RaytracingScene);
        m_Scene = Assets::Scene{};
        m_RaytracingScene = Assets::RaytracingScene{};
        RetireAssets(std::move(retired));

        {
            std::lock_guard<std::mutex> lock(m_LoadRequestMutex);
            m_LoadRequestSceneIndex = static_cast<int>(sceneIndex);
        }
        m_LoadRequestCV.notify_one();
        m_SceneLoadInFlight = true;
        // 進捗表示にシーン名を出すために、いま読ませているシーンを控える
        m_SceneLoadingIndex = sceneIndex;
    }

    // 同じモデルを指すインスタンスを1回のDrawIndexedへまとめるバッチを作り直す。
    //
    // 【レンダーグラフの構築より前に1フレーム1回だけ呼ぶこと】UpdateModelLODが決めた段を読むので
    // その後、かつどのパスより前。パスごとに組み直すと、深度プリパスとG-Bufferが違うまとめ方をして
    // 同じ画素を別の経路で描くことになる。
    //
    // 【バッチに入れないもの】
    //   - まだ読み込まれていない段(ストリーミング中)
    //   - LOD切替のフェード中。DitherFadeはインスタンスごとに違い、定数バッファで渡す値なので
    //     1ドローにまとめられない。フェードは短時間で終わるので、そのあいだ個別に描けばよい
    //   - メッシュシェーダー経路に載るモデル。DispatchMeshにインスタンス数の概念が無い
    //   - まとめる相手がいないもの(1体だけのグループ)。この場合は従来とまったく同じ描画になる
    // このフレームの描画単位を組み立てる。バッチに入ったインスタンスはバッチとして1回、
    // 入らなかったものは1体ずつ現れる ―― 全インスタンスがちょうど1回ずつ現れることが要点で、
    // 取りこぼすと物が消え、二重に出すと同じ場所へ2回描いてZファイティングになる
    void KurenaiEngine3D::GetInstanceDrawUnits(bool coarsestLOD, std::vector<InstanceDrawUnit>& outUnits) const
    {
        const std::vector<InstanceBatch>& batches =
            coarsestLOD ? m_InstanceBatchesCoarsestLOD : m_InstanceBatchesCurrentLOD;
        const std::vector<uint8_t>& batched =
            coarsestLOD ? m_InstanceBatchedCoarsestLOD : m_InstanceBatchedCurrentLOD;

        outUnits.clear();
        outUnits.reserve(m_Scene.Instances.size());

        for (const InstanceBatch& batch : batches)
        {
            InstanceDrawUnit unit;
            // 代表はバッチの先頭。IsMirrored/IsWaterはバッチ内で同一(グループ化のキー)なので、
            // どれを代表にしても同じ値になる
            unit.Instance = &m_Scene.Instances[batch.RepresentativeIndex];
            unit.InstanceIndex = batch.RepresentativeIndex;
            unit.Model = batch.Model;
            unit.InstanceBase = batch.InstanceBase;
            unit.InstanceCount = batch.InstanceCount;
            for (int axis = 0; axis < 3; ++axis)
            {
                unit.WorldBoundsMin[axis] = batch.WorldBoundsMin[axis];
                unit.WorldBoundsMax[axis] = batch.WorldBoundsMax[axis];
            }
            outUnits.push_back(unit);
        }

        for (size_t i = 0; i < m_Scene.Instances.size(); ++i)
        {
            if (i < batched.size() && batched[i] != 0)
            {
                continue;   // バッチとして既に積んである
            }
            InstanceDrawUnit unit;
            unit.Instance = &m_Scene.Instances[i];
            unit.InstanceIndex = i;
            unit.Model = nullptr;   // 段は呼び出し側が決める(フェード中は2段になる)
            unit.InstanceBase = 0;
            unit.InstanceCount = 1;
            for (int axis = 0; axis < 3; ++axis)
            {
                unit.WorldBoundsMin[axis] = m_Scene.Instances[i].WorldBoundsMin[axis];
                unit.WorldBoundsMax[axis] = m_Scene.Instances[i].WorldBoundsMax[axis];
            }
            outUnits.push_back(unit);
        }
    }

    void KurenaiEngine3D::BuildInstanceBatches(RHI::IRHICommandList* commandList)
    {
        m_InstanceBatchesCurrentLOD.clear();
        m_InstanceBatchesCoarsestLOD.clear();
        m_ModelInstanceRecords.clear();
        m_InstanceBatchedCurrentLOD.assign(m_Scene.Instances.size(), 0u);
        m_InstanceBatchedCoarsestLOD.assign(m_Scene.Instances.size(), 0u);
        m_InstancedBatchCount = 0;
        m_InstancedInstanceCount = 0;

        if (!m_InstancingEnabled || m_Scene.Instances.empty() || !m_ModelInstanceBuffer)
        {
            return;
        }

        // グループ化のキー。ワインディング(IsMirrored)と水面(IsWater)はパイプラインステートが
        // 分かれるため、違うものを同じドローへまとめてはいけない
        struct GroupKey
        {
            const Assets::Model* Model;
            bool IsMirrored;
            bool IsWater;
            bool operator==(const GroupKey& other) const
            {
                return Model == other.Model && IsMirrored == other.IsMirrored && IsWater == other.IsWater;
            }
        };

        // キーごとのインスタンス番号。シーンの並び順で走査するので、同じシーンなら毎フレーム同じ順になる
        // (順序が揺れるとフレーム間でバッチの内容が変わり、A/B比較の再現性が落ちる)
        std::vector<std::pair<GroupKey, std::vector<size_t>>> groups;

        // 1つの組(段の選び方)ぶんのバッチを作る。
        // modelOf: そのインスタンスがこの組で描く段を返す。nullptrならこの組の対象外
        const auto buildFor =
            [this, &groups](
                const std::function<const Assets::Model*(size_t)>& modelOf,
                std::vector<InstanceBatch>& outBatches, std::vector<uint8_t>& outBatched)
        {
            groups.clear();
            for (size_t i = 0; i < m_Scene.Instances.size(); ++i)
            {
                const Assets::ModelInstance& instance = m_Scene.Instances[i];
                const Assets::Model* const model = modelOf(i);
                if (!model)
                {
                    continue;
                }
                // メッシュシェーダー経路はDispatchMeshで描くのでまとめられない
                if (ShouldUseModelMeshletPath(instance, *model))
                {
                    continue;
                }

                const GroupKey key{ model, instance.IsMirrored, instance.IsWater };
                bool found = false;
                for (auto& group : groups)
                {
                    if (group.first == key)
                    {
                        group.second.push_back(i);
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    groups.push_back({ key, { i } });
                }
            }

            // 【空間セルでソートしてから刻む】上限なしで1バッチにすると、広く散らばった
            // グループが1つの巨大AABBになり、どのパスからも一度も間引かれなくなる。
            // セルの幅はシーン対角の1/64を目安にする(バッチの粒度がシーンの規模に追随する)
            const float diagonalX = m_Scene.BoundsMax[0] - m_Scene.BoundsMin[0];
            const float diagonalY = m_Scene.BoundsMax[1] - m_Scene.BoundsMin[1];
            const float diagonalZ = m_Scene.BoundsMax[2] - m_Scene.BoundsMin[2];
            const float diagonal =
                std::sqrt(diagonalX * diagonalX + diagonalY * diagonalY + diagonalZ * diagonalZ);
            const float cellSize = std::max(diagonal / 64.0f, 1.0f);

            for (auto& group : groups)
            {
                if (group.second.size() < 2)
                {
                    // まとめる相手がいない。従来どおり個別に描く(コマンド列は今までと同一)
                    continue;
                }

                std::stable_sort(
                    group.second.begin(), group.second.end(),
                    [this, cellSize](size_t a, size_t b)
                    {
                        const auto cell = [this, cellSize](size_t index, int axis)
                        {
                            const float center =
                                (m_Scene.Instances[index].WorldBoundsMin[axis]
                                 + m_Scene.Instances[index].WorldBoundsMax[axis]) * 0.5f;
                            return static_cast<int64_t>(std::floor(center / cellSize));
                        };
                        // Z→X→Y の順に見る。格子状の配置ではこれで行ごとにまとまる
                        const int axes[3] = { 2, 0, 1 };
                        for (const int axis : axes)
                        {
                            const int64_t ca = cell(a, axis);
                            const int64_t cb = cell(b, axis);
                            if (ca != cb)
                            {
                                return ca < cb;
                            }
                        }
                        return a < b;
                    });

                for (size_t offset = 0; offset < group.second.size(); offset += kMaxInstancesPerBatch)
                {
                    const size_t count = std::min<size_t>(kMaxInstancesPerBatch, group.second.size() - offset);
                    if (count < 2)
                    {
                        // 刻んだ余りが1体だけになった場合。まとめる意味が無いので個別へ回す
                        continue;
                    }

                    InstanceBatch batch;
                    batch.Model = group.first.Model;
                    batch.IsMirrored = group.first.IsMirrored;
                    batch.IsWater = group.first.IsWater;
                    batch.InstanceBase = static_cast<uint32_t>(m_ModelInstanceRecords.size());
                    batch.InstanceCount = static_cast<uint32_t>(count);
                    batch.RepresentativeIndex = group.second[offset];
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        batch.WorldBoundsMin[axis] = (std::numeric_limits<float>::max)();
                        batch.WorldBoundsMax[axis] = std::numeric_limits<float>::lowest();
                    }

                    for (size_t k = 0; k < count; ++k)
                    {
                        const size_t instanceIndex = group.second[offset + k];
                        const Assets::ModelInstance& instance = m_Scene.Instances[instanceIndex];

                        GPUModelInstance record{};
                        record.World = instance.World;
                        record.NormalMatrix = instance.NormalMatrix;
                        record.TangentSignFlip = instance.TangentSignFlip;
                        m_ModelInstanceRecords.push_back(record);

                        for (int axis = 0; axis < 3; ++axis)
                        {
                            batch.WorldBoundsMin[axis] =
                                std::min(batch.WorldBoundsMin[axis], instance.WorldBoundsMin[axis]);
                            batch.WorldBoundsMax[axis] =
                                std::max(batch.WorldBoundsMax[axis], instance.WorldBoundsMax[axis]);
                        }
                        outBatched[instanceIndex] = 1u;
                    }

                    outBatches.push_back(batch);
                }
            }
        };

        // 組1: そのフレームに選ばれた段(深度プリパス / G-Buffer / 平面反射)。
        // フェード中(段が2つ)は個別に描くのでバッチへ入れない
        buildFor(
            [this](size_t i) -> const Assets::Model*
            {
                LODDraw draws[2];
                if (GetLODDraws(i, draws) != 1)
                {
                    return nullptr;
                }
                // 【GetCurrentLODと一致するときだけまとめる】GetLODDrawsは、フェード中でも
                // 片方の段が未ストリーミングなら「読めているほうを全画素で描く」として1を返す。
                // その段は PreviousLOD のことがあり、GetCurrentLOD は nullptr を返す。
                // 平面反射パスは単体を GetCurrentLOD で引くので、そこを一致させておかないと
                // 「まとめられた個体は水面に映るが、同じ状態でまとめられなかった個体は映らない」
                // という食い違いが出る
                if (draws[0].Model != GetCurrentLOD(i))
                {
                    return nullptr;
                }
                return draws[0].Model;
            },
            m_InstanceBatchesCurrentLOD, m_InstanceBatchedCurrentLOD);

        // 組2: 常に最も粗い段(シャドウ / 反射プローブ)
        buildFor(
            [this](size_t i) -> const Assets::Model* { return GetCoarsestLOD(m_Scene.Instances[i]); },
            m_InstanceBatchesCoarsestLOD, m_InstanceBatchedCoarsestLOD);

        m_InstancedBatchCount =
            static_cast<uint32_t>(m_InstanceBatchesCurrentLOD.size() + m_InstanceBatchesCoarsestLOD.size());
        m_InstancedInstanceCount = static_cast<uint32_t>(m_ModelInstanceRecords.size());

        if (m_ModelInstanceRecords.empty())
        {
            return;
        }

        // 容量はシーン読み込み時に「インスタンス数×2組」で確保してある。超えることは無いが、
        // 超えたときに黙って壊れないよう検査してログを残す
        const size_t capacity = m_Scene.Instances.size() * 2;
        if (m_ModelInstanceRecords.size() > capacity)
        {
            Core::Logger::Error(
                "KurenaiEngine3D",
                "インスタンスバッファの容量(" + std::to_string(capacity) + "件)を超えました("
                    + std::to_string(m_ModelInstanceRecords.size()) + "件)。このフレームはインスタンシングを見送ります");
            m_InstanceBatchesCurrentLOD.clear();
            m_InstanceBatchesCoarsestLOD.clear();
            std::fill(m_InstanceBatchedCurrentLOD.begin(), m_InstanceBatchedCurrentLOD.end(), 0u);
            std::fill(m_InstanceBatchedCoarsestLOD.begin(), m_InstanceBatchedCoarsestLOD.end(), 0u);
            m_InstancedBatchCount = 0;
            m_InstancedInstanceCount = 0;
            return;
        }

        // 【1フレームに1回だけ】どのパスもこの1本を読む。バインドは各パスがDraw直前に張り直す
        // (頂点シェーダー用SRVはt0の1本しかなく、ドローンショーが同じスロットを使うため)
        commandList->UpdateBuffer(
            m_ModelInstanceBuffer.get(), m_ModelInstanceRecords.data(),
            m_ModelInstanceRecords.size() * sizeof(GPUModelInstance));
    }

    void KurenaiEngine3D::UpdateModelLOD(const DirectX::XMFLOAT3& cameraPosition, float deltaSeconds)
    {
        m_LODSwitchCount = 0;
        m_LODFadingCount = 0;

        if (m_InstanceLODStates.size() != m_Scene.Instances.size())
        {
            // シーンが差し替わった直後。状態を作り直す(全インスタンスが最も詳細な段から始まる)
            m_InstanceLODStates.assign(m_Scene.Instances.size(), InstanceLODState{});
        }

        for (size_t i = 0; i < m_Scene.Instances.size(); ++i)
        {
            Assets::ModelInstance& instance = m_Scene.Instances[i];
            InstanceLODState& state = m_InstanceLODStates[i];

            const size_t levelCount = instance.LODModels.size() + 1;
            if (levelCount <= 1)
            {
                // LODを持たないインスタンス。従来どおり1段だけ
                state.CurrentLOD = 0;
                state.PreviousLOD = 0;
                state.FadeT = 1.0f;
                instance.LODLevel = 0;
                continue;
            }

            // 【AABBの最近接点までの距離】中心距離だと1.1km四方のPLATEAUタイルで破綻する。
            // タイルの上に立っていても中心までは500m以上あるため、近景なのに粗い段が選ばれる。
            // 点がAABBの内側なら距離0になる(各軸の食い込み量が0になるため)
            float squaredDistance = 0.0f;
            const float cameraXYZ[3] = { cameraPosition.x, cameraPosition.y, cameraPosition.z };
            for (int axis = 0; axis < 3; ++axis)
            {
                const float outside = (std::max)(
                    { instance.WorldBoundsMin[axis] - cameraXYZ[axis],
                      cameraXYZ[axis] - instance.WorldBoundsMax[axis], 0.0f });
                squaredDistance += outside * outside;
            }
            const float distance = std::sqrt(squaredDistance);

            // 【1フレームに1段だけ動かす】ヒステリシスを素直に書ける。段数の上限は4なので、
            // 遠くから一気に近づいても数フレームで追いつく
            uint32_t desired = state.CurrentLOD;
            if (desired < instance.LODDistances.size() &&
                distance > instance.LODDistances[desired] * (1.0f + m_LODHysteresis))
            {
                desired = desired + 1;
            }
            else if (desired > 0 &&
                     distance < instance.LODDistances[desired - 1] * (1.0f - m_LODHysteresis))
            {
                desired = desired - 1;
            }

            if (desired != state.CurrentLOD)
            {
                // フェード中に次の切り替えが来たら、いま描いている「先」を新しい「元」にする。
                // 3段以上を同時に重ねることはしない(ディザが排他にならず穴が開く)
                state.PreviousLOD = state.CurrentLOD;
                state.CurrentLOD = desired;
                state.FadeT = 0.0f;
                ++m_LODSwitchCount;
            }
            else if (state.FadeT < 1.0f)
            {
                state.FadeT = (m_LODFadeDuration > 0.0f)
                    ? (std::min)(1.0f, state.FadeT + deltaSeconds / m_LODFadeDuration)
                    : 1.0f;
            }

            if (state.FadeT < 1.0f)
            {
                ++m_LODFadingCount;
            }

            // 常駐マップ(StreamingPanel)が色分けに使う。ここが唯一の書き込み元
            instance.LODLevel = state.CurrentLOD;
        }
    }

    void KurenaiEngine3D::RequestRaytracingRebuild()
    {
        if (!m_Device->SupportsRaytracing() || !m_Scene.HasStreamingDistance)
        {
            return;
        }
        m_RaytracingRebuildPending = true;
        m_RaytracingRebuildAfter = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(static_cast<int>(kRaytracingRebuildQuietSeconds * 1000.0f));
    }

    void KurenaiEngine3D::UpdateRaytracingRebuild()
    {
        // --- 出来上がったものを差し替える ---------------------------------------------------
        {
            std::unique_ptr<Assets::RaytracingScene> rebuilt;
            uint64_t generation = 0;
            {
                std::lock_guard<std::mutex> lock(m_RaytracingRebuiltMutex);
                rebuilt = std::move(m_RaytracingRebuilt);
                generation = m_RaytracingRebuiltGeneration;
            }
            if (rebuilt && generation == m_StreamingGeneration)
            {
                auto retired = std::make_unique<Assets::RaytracingScene>(std::move(m_RaytracingScene));
                m_RaytracingPendingRelease.push_back({ std::move(retired), kStreamingReleaseDelayFrames });
                m_RaytracingScene = std::move(*rebuilt);
                ++m_RaytracingRebuildCount;
            }
        }

        // --- 寝かせ終えたものをLoaderスレッドへ渡す -------------------------------------------
        //
        // 【ここでresetしてはいけない】RaytracingSceneが持つディスクリプタは、ロックを持たない
        // アセット用ヒープから取られている。Loaderスレッドがストリーミングで確保している最中に
        // Renderスレッドが解放するとフリーリストが壊れる。モデルの破棄と同じ経路へ寄せる
        if (!m_RaytracingPendingRelease.empty())
        {
            std::vector<std::unique_ptr<Assets::RaytracingScene>> ready;
            for (PendingRaytracingRelease& pending : m_RaytracingPendingRelease)
            {
                if (pending.FramesRemaining > 0)
                {
                    --pending.FramesRemaining;
                    continue;
                }
                ready.push_back(std::move(pending.Scene));
            }
            m_RaytracingPendingRelease.erase(
                std::remove_if(
                    m_RaytracingPendingRelease.begin(), m_RaytracingPendingRelease.end(),
                    [](const PendingRaytracingRelease& pending) { return !pending.Scene; }),
                m_RaytracingPendingRelease.end());

            if (!ready.empty())
            {
                {
                    std::lock_guard<std::mutex> lock(m_RaytracingReleaseMutex);
                    for (auto& scene : ready)
                    {
                        m_RaytracingRelease.push_back(std::move(scene));
                    }
                }
                m_LoadRequestCV.notify_one();
            }
        }

        // --- 静かになったら発注する -----------------------------------------------------------
        if (!m_RaytracingRebuildPending || std::chrono::steady_clock::now() < m_RaytracingRebuildAfter)
        {
            return;
        }
        m_RaytracingRebuildPending = false;
        m_RaytracingRebuildInFlight.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(m_LoadRequestMutex);
            m_RaytracingRebuildRequested = true;
        }
        m_LoadRequestCV.notify_one();
    }

    void KurenaiEngine3D::UpdateModelStreaming(const DirectX::XMFLOAT3& cameraPosition)
    {
        m_StreamingResidentCount = 0;
        m_StreamingTargetCount = 0;

        // 破棄待ちを1フレーム進める。0になったものだけLoaderスレッドへ渡す。
        // 【ストリーミングを使わないシーンでも回す】シーンを切り替えた直後に、
        // 前のシーンで積んだ分が残っていることがある
        if (!m_StreamingPendingRelease.empty())
        {
            std::vector<std::shared_ptr<Assets::Model>> ready;
            for (PendingModelRelease& pending : m_StreamingPendingRelease)
            {
                if (pending.FramesRemaining > 0)
                {
                    --pending.FramesRemaining;
                    continue;
                }
                // 【常駐ミップの読み直しが終わるまで待つ】Loaderスレッドがこのモデルの
                // IRHITexture*を掴んでいる間に解放すると解放済みを触る。
                // フレーム数の遅延では足りない(1件が数十msかかることがある)
                if (m_TextureStreaming.IsModelBusy(pending.Model.get()))
                {
                    continue;
                }
                ready.push_back(std::move(pending.Model));
            }
            m_StreamingPendingRelease.erase(
                std::remove_if(
                    m_StreamingPendingRelease.begin(), m_StreamingPendingRelease.end(),
                    [](const PendingModelRelease& pending) { return !pending.Model; }),
                m_StreamingPendingRelease.end());

            if (!ready.empty())
            {
                {
                    std::lock_guard<std::mutex> lock(m_StreamingReleaseMutex);
                    for (std::shared_ptr<Assets::Model>& model : ready)
                    {
                        m_StreamingRelease.push_back(std::move(model));
                    }
                }
                // Loaderスレッドが寝ていると破棄が溜まり続けるので起こす
                m_LoadRequestCV.notify_one();
            }
        }

        if (!m_Scene.HasStreamingDistance)
        {
            return;
        }

        // --- Loaderスレッドが仕上げたものを取り込む -----------------------------------------
        {
            std::vector<StreamingLoaded> loaded;
            {
                std::lock_guard<std::mutex> lock(m_StreamingLoadedMutex);
                loaded.swap(m_StreamingLoaded);
            }
            // 再構築中はLoaderスレッドが m_Scene を走査しているので差し込まない
            if (m_RaytracingRebuildInFlight.load(std::memory_order_acquire))
            {
                std::lock_guard<std::mutex> lock(m_StreamingLoadedMutex);
                for (StreamingLoaded& item : loaded)
                {
                    m_StreamingLoaded.push_back(std::move(item));
                }
                loaded.clear();
            }

            for (StreamingLoaded& item : loaded)
            {
                m_StreamingInFlight.erase(item.Path);
                // 【古い世代は捨てる】シーンを切り替えた後に前のシーンのモデルが届くことがある
                if (item.Generation != m_StreamingGeneration || !item.Model)
                {
                    continue;
                }
                ++m_StreamingLoadedTotal;
                RequestRaytracingRebuild();
                // 同じパスを指すすべての段へ差し込む(モデル共有。2-1と同じ考え方)
                auto shared = std::shared_ptr<const Assets::Model>(item.Model);
                m_Scene.ModelCache[item.Path] = std::move(item.Model);
                for (Assets::ModelInstance& instance : m_Scene.Instances)
                {
                    bool referenced = false;
                    for (size_t level = 0; level < instance.ModelPaths.size(); ++level)
                    {
                        if (instance.ModelPaths[level] != item.Path)
                        {
                            continue;
                        }
                        referenced = true;
                        if (level == 0)
                        {
                            instance.Model = shared;
                        }
                        else
                        {
                            instance.LODModels[level - 1] = shared;
                        }
                    }
                    // 常駐ミップ制御の追跡表へ入れる。
                    // 【インスタンスごとに1回だけ】メッシュのAABBはインスタンスのWorldで
                    // ワールド空間へ移して持つので、参照はインスタンスの数だけ要る。
                    // 逆に同じインスタンスで2回呼ぶと、同じ参照が二重に積まれる
                    // (同じパスが複数の段に入っているシーンで起きうる)
                    if (referenced)
                    {
                        m_TextureStreaming.AttachModel(*shared, instance.World, *m_Device);
                    }
                }
            }
        }

        // --- 距離を見て、足りないものを近い順に発注する -------------------------------------
        //
        // 【段ごとに要否が違う】いま選ばれている段だけを読めばよい。遠くて粗い段しか使わない
        // タイルの詳細な段まで読むと、ストリーミングの意味が無くなる
        struct Candidate
        {
            float DistanceSq = 0.0f;
            const std::wstring* Path = nullptr;
        };
        std::vector<Candidate> candidates;

        // 破棄しない(=まだ要る)パスの集合。読み込みの判定より広い距離で集める
        std::unordered_set<std::wstring> neededPaths;

        const float limit = m_Scene.StreamingDistance;
        const float limitSq = limit * limit;
        // 【破棄は読み込みより遠くで行う】同じ距離でやると、境界上でカメラが揺れるたびに
        // 読み込みと破棄が交互に起きて、ディスクアクセスが止まらなくなる。
        // 1.25倍の不感帯を置く(モデルLODのヒステリシスと同じ考え方)
        const float evictLimitSq = (limit * 1.25f) * (limit * 1.25f);
        const float cameraXYZ[3] = { cameraPosition.x, cameraPosition.y, cameraPosition.z };

        for (size_t i = 0; i < m_Scene.Instances.size(); ++i)
        {
            Assets::ModelInstance& instance = m_Scene.Instances[i];
            if (instance.ModelPaths.empty())
            {
                continue;
            }

            // 常駐マップ(StreamingPanel)が色分けに使う3値。
            // 【距離で抜ける前に書く】範囲外のインスタンスもここを通らなければ
            // 古い値が残り、破棄されたものが「常駐」の色のまま地図に出る
            const uint32_t level = (i < m_InstanceLODStates.size()) ? m_InstanceLODStates[i].CurrentLOD : 0u;
            const size_t levelIndex = (level < instance.ModelPaths.size()) ? level : 0u;
            instance.Residency =
                instance.IsLODLoaded(levelIndex)                             ? Assets::ResidencyState::Loaded
                : (m_StreamingInFlight.count(instance.ModelPaths[levelIndex]) != 0)
                                                                            ? Assets::ResidencyState::Loading
                                                                            : Assets::ResidencyState::Unloaded;

            // モデルLODと同じ「AABBの最近接点まで」の距離
            float squaredDistance = 0.0f;
            for (int axis = 0; axis < 3; ++axis)
            {
                const float outside = (std::max)(
                    { instance.WorldBoundsMin[axis] - cameraXYZ[axis],
                      cameraXYZ[axis] - instance.WorldBoundsMax[axis], 0.0f });
                squaredDistance += outside * outside;
            }
            // 破棄の不感帯(1.25倍)の内側にあるものは、読み込み対象でなくても捨てない
            if (squaredDistance <= evictLimitSq)
            {
                for (const std::wstring& path : instance.ModelPaths)
                {
                    neededPaths.insert(path);
                }
            }

            if (squaredDistance > limitSq)
            {
                continue;
            }
            ++m_StreamingTargetCount;

            if (instance.IsLODLoaded(levelIndex))
            {
                ++m_StreamingResidentCount;
                continue;
            }

            const std::wstring& path = instance.ModelPaths[levelIndex];
            if (m_StreamingInFlight.count(path) != 0)
            {
                continue;
            }
            candidates.push_back({ squaredDistance, &path });
        }

        // --- 遠ざかったものを破棄する ---------------------------------------------------------
        //
        // 【モデルは共有されている】同じ.kmodelを複数のインスタンスが指しうるので、
        // 「どれか1つでもまだ要る」なら捨てられない。インスタンス単位ではなく
        // ModelCacheをパス単位で見て、needed に無いものだけを外す
        // 再構築中は破棄しない(理由は上の差し込みと同じ)
        if (!m_RaytracingRebuildInFlight.load(std::memory_order_acquire))
        {
            std::vector<std::wstring> evictPaths;
            for (const auto& entry : m_Scene.ModelCache)
            {
                if (neededPaths.count(entry.first) == 0)
                {
                    evictPaths.push_back(entry.first);
                }
            }

            for (const std::wstring& path : evictPaths)
            {
                // インスタンス側の参照を外す。描画ループは未読み込みとして飛ばす
                for (Assets::ModelInstance& instance : m_Scene.Instances)
                {
                    for (size_t level = 0; level < instance.ModelPaths.size(); ++level)
                    {
                        if (instance.ModelPaths[level] != path)
                        {
                            continue;
                        }
                        if (level == 0)
                        {
                            instance.Model.reset();
                        }
                        else
                        {
                            instance.LODModels[level - 1].reset();
                        }
                    }
                }

                auto cached = m_Scene.ModelCache.find(path);
                if (cached == m_Scene.ModelCache.end())
                {
                    continue;
                }
                // 【破棄より前に追跡表から外す】これ以降このモデルへ新しい読み直しは発注されない。
                // 発注済みのものは破棄待ちの側(IsModelBusy)で待つ
                m_TextureStreaming.DetachModel(*cached->second);
                // 実体はここで消さず、GPUが読み終わるまで寝かせる
                m_StreamingPendingRelease.push_back(
                    { std::move(cached->second), kStreamingReleaseDelayFrames });
                m_Scene.ModelCache.erase(cached);
                ++m_StreamingEvictedTotal;
                RequestRaytracingRebuild();
            }
        }

        if (candidates.empty())
        {
            return;
        }

        // 近い順に発注する。手前のものから絵が埋まるので、遠くの読み込みで手前が待たされない
        std::sort(candidates.begin(), candidates.end(),
                  [](const Candidate& a, const Candidate& b) { return a.DistanceSq < b.DistanceSq; });

        // 【1フレームの発注数に上限を置く】Loaderスレッドは1本で、シーン切り替えもここを通る。
        // 際限なく積むと、切り替え要求が数百件の読み込みの後ろで待たされる
        constexpr size_t kMaxStreamingRequestsPerFrame = 8;
        const size_t requestCount = (std::min)(candidates.size(), kMaxStreamingRequestsPerFrame);

        {
            std::lock_guard<std::mutex> lock(m_LoadRequestMutex);
            for (size_t i = 0; i < requestCount; ++i)
            {
                m_StreamingRequests.push_back({ *candidates[i].Path, m_StreamingGeneration });
                m_StreamingInFlight.insert(*candidates[i].Path);
            }
        }
        m_LoadRequestCV.notify_one();
    }

    uint32_t KurenaiEngine3D::GetLODDraws(size_t instanceIndex, LODDraw (&outDraws)[2]) const
    {
        const Assets::ModelInstance& instance = m_Scene.Instances[instanceIndex];
        // ストリーミング中はまだ読み込まれていない段がある。nullptrの段は描画対象から外す
        const auto modelAt = [&instance](uint32_t level) -> const Assets::Model*
        {
            return (level == 0) ? instance.Model.get() : instance.LODModels[level - 1].get();
        };

        if (instanceIndex >= m_InstanceLODStates.size())
        {
            outDraws[0] = { instance.Model.get(), 1.0f };
            return instance.Model ? 1u : 0u;
        }

        const InstanceLODState& state = m_InstanceLODStates[instanceIndex];
        if (state.FadeT >= 1.0f)
        {
            outDraws[0] = { modelAt(state.CurrentLOD), 1.0f };
            return outDraws[0].Model ? 1u : 0u;
        }

        // 切り替え「先」は +FadeT、「元」は -FadeT。同じノイズをしきい値の両側で分け合うので、
        // 2段が同じ画素に重ならず(Zファイティングにならず)、隙間もできない。
        //
        // 【片方が未読み込みなら、もう片方を全画素で描く】ディザで分け合う相手がいないのに
        // 半分だけ描くと、その間だけモデルに穴が開く
        const Assets::Model* const toModel = modelAt(state.CurrentLOD);
        const Assets::Model* const fromModel = modelAt(state.PreviousLOD);
        if (!toModel || !fromModel)
        {
            const Assets::Model* const only = toModel ? toModel : fromModel;
            outDraws[0] = { only, 1.0f };
            return only ? 1u : 0u;
        }
        outDraws[0] = { toModel, state.FadeT };
        outDraws[1] = { fromModel, -state.FadeT };
        return 2;
    }

    const Assets::Model* KurenaiEngine3D::GetCurrentLOD(size_t instanceIndex) const
    {
        const Assets::ModelInstance& instance = m_Scene.Instances[instanceIndex];
        const uint32_t level = (instanceIndex < m_InstanceLODStates.size())
            ? m_InstanceLODStates[instanceIndex].CurrentLOD
            : 0u;
        // ストリーミング中はまだ読み込まれていないことがある。nullptrを返し、呼び出し側が飛ばす
        return (level == 0) ? instance.Model.get() : instance.LODModels[level - 1].get();
    }

    const Assets::Model* KurenaiEngine3D::GetCoarsestLOD(const Assets::ModelInstance& instance) const
    {
        // 【影と間接光は常に最も粗い段】どちらもテクスチャを読まないので、詳細な段を描く意味が無い。
        // PLATEAUではLOD2(約1715メッシュ)がLOD1(1メッシュ)になるため、
        // シャドウのドローコールが4カスケード分まとめて桁で減る
        return instance.LODModels.empty() ? instance.Model.get() : instance.LODModels.back().get();
    }

    void KurenaiEngine3D::RetireAssets(RetiredAssets&& retired)
    {
        std::lock_guard<std::mutex> lock(m_RetiredAssetsMutex);
        m_RetiredAssets.push_back(std::move(retired));
    }

    void KurenaiEngine3D::LoaderThreadMain()
    {
        // TextureImage::LoadFromFileがWICを使う経路(.dds/.tga以外)に備えてCOMを初期化しておく。
        // COMはスレッドごとに初期化が必要で、未初期化のままWICを呼ぶとハングする
        // (packedアセットは.ktex=DDSなので通常この経路には入らないが、保険として揃えておく)
        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

        // 破棄依頼を引き取って実際に解放する。アセット用ディスクリプタヒープを触るのは
        // このスレッドだけ、という不変条件を保つための処理(RetiredAssetsのコメント参照)
        const auto destroyRetiredAssets = [this]()
        {
            std::vector<RetiredAssets> retired;
            {
                std::lock_guard<std::mutex> lock(m_RetiredAssetsMutex);
                retired.swap(m_RetiredAssets);
            }
            // retiredのデストラクタでGPUリソースが解放される
        };

        // ストリーミングで遠ざかったモデルの破棄。Renderスレッドが
        // kStreamingReleaseDelayFrames フレーム寝かせたものだけがここへ来る
        // (RetiredAssetsと違いWaitForGPUIdleは通っていない。遅延がその代わり)
        const auto destroyStreamedModels = [this]()
        {
            std::vector<std::shared_ptr<Assets::Model>> release;
            {
                std::lock_guard<std::mutex> lock(m_StreamingReleaseMutex);
                release.swap(m_StreamingRelease);
            }
            // releaseのデストラクタでGPUリソースが解放される

            // 差し替えられた旧RaytracingSceneも同じ理由でこのスレッドで解放する
            // (BLAS/TLASと統合バッファのディスクリプタはアセット用ヒープから取られている)
            std::vector<std::unique_ptr<Assets::RaytracingScene>> scenes;
            {
                std::lock_guard<std::mutex> lock(m_RaytracingReleaseMutex);
                scenes.swap(m_RaytracingRelease);
            }
        };

        for (;;)
        {
            int sceneIndex = -1;
            std::vector<StreamingRequest> streamingRequests;
            bool raytracingRebuild = false;
            {
                std::unique_lock<std::mutex> lock(m_LoadRequestMutex);
                m_LoadRequestCV.wait(lock, [this] {
                    if (m_LoadRequestSceneIndex >= 0 || !m_StreamingRequests.empty() ||
                        m_RaytracingRebuildRequested || m_StopLoaderThread)
                    {
                        return true;
                    }
                    // 常駐ミップの読み直しもこのスレッドが行う(専用スレッドは立てない)。
                    // モデルの発注が無い間もこれだけで起きる必要がある
                    if (m_TextureStreaming.HasPendingRequests())
                    {
                        return true;
                    }
                    // 破棄だけが積まれている場合も起きる(読み込みが止まっている間に
                    // 破棄が溜まり続けると、遠ざかったモデルのVRAMが解放されない)
                    {
                        std::lock_guard<std::mutex> releaseLock(m_StreamingReleaseMutex);
                        if (!m_StreamingRelease.empty()) { return true; }
                    }
                    std::lock_guard<std::mutex> rtLock(m_RaytracingReleaseMutex);
                    return !m_RaytracingRelease.empty();
                });
                if (m_StopLoaderThread && m_LoadRequestSceneIndex < 0)
                {
                    break;
                }
                sceneIndex = m_LoadRequestSceneIndex;
                m_LoadRequestSceneIndex = -1;
                // 【シーン切り替えが来たら、溜まっているストリーミング発注は捨てる】
                // それらは切り替え前のシーンのもので、読んでも差し込む先が無い
                if (sceneIndex >= 0)
                {
                    m_StreamingRequests.clear();
                    // 切り替え前のシーンへの再構築要求は無意味。
                    // 【フラグを降ろすのを忘れない】立てたままだとRenderスレッドの
                    // 差し込みと破棄が永久に止まる
                    m_RaytracingRebuildRequested = false;
                    m_RaytracingRebuildInFlight.store(false, std::memory_order_release);
                }
                else
                {
                    streamingRequests.swap(m_StreamingRequests);
                    raytracingRebuild = m_RaytracingRebuildRequested;
                    m_RaytracingRebuildRequested = false;
                }
            }

            // 破棄は毎ループ引き取る。読み込みより先に行うことでVRAMのピークを下げる
            destroyStreamedModels();

            // 常駐ミップの読み直し。**モデルの読み込みより先に、そして1件ごとに挟む**
            // (下のループの中でも呼ぶ)。モデル1件の読み込みはPLATEAUのLOD2タイルで
            // 秒の単位かかるため、まとめて後回しにすると街を流している間じゅう
            // ミップの差し替えが止まり、近づいた面がぼけたまま残る。
            // 1回あたりの件数を絞ってあるので、逆にモデルの読み込みが待たされることもない
            constexpr size_t kTextureRequestsPerSlice = 4;
            m_TextureStreaming.ProcessRequests(*m_Device, kTextureRequestsPerSlice);

            // --- ストリーミングの読み込み ---------------------------------------------------
            if (!streamingRequests.empty())
            {
                if (!m_StreamingTexturePool)
                {
                    m_StreamingTexturePool = std::make_unique<Assets::SharedTexturePool>();
                }
                for (const StreamingRequest& request : streamingRequests)
                {
                    std::shared_ptr<Assets::Model> model;
                    try
                    {
                        model = std::make_shared<Assets::Model>(
                            Assets::LoadModel(*m_Device, request.Path, m_StreamingTexturePool.get()));
                    }
                    catch (const std::exception& error)
                    {
                        // 1件の失敗でストリーミング全体を止めない。そのモデルだけが出ないまま続く
                        Core::Logger::Error(
                            "KurenaiEngine3D",
                            "ストリーミングの読み込みに失敗しました: " + WideToUtf8(request.Path) + " (" +
                                error.what() + ")");
                    }
                    {
                        std::lock_guard<std::mutex> lock(m_StreamingLoadedMutex);
                        // 失敗しても空のまま返す。Renderスレッドが「発注中」から外せないと
                        // 同じものを永久に再発注し続ける
                        m_StreamingLoaded.push_back({ request.Path, std::move(model), request.Generation });
                    }
                    // 1件読むごとにミップの差し替えを挟む(このループの外のコメント参照)
                    m_TextureStreaming.ProcessRequests(*m_Device, kTextureRequestsPerSlice);
                }
                // 【ここでcontinueしない】読み込みと再構築が同時に積まれることがある。
                // 抜けると再構築要求だけが失われ、m_RaytracingRebuildInFlightが立ったまま戻らない
            }

            // --- レイトレーシングの作り直し(Loaderスレッドで行う) ---------------------------
            if (raytracingRebuild)
            {
                // 【計測用の一時スイッチ】KURENAI_NO_RT が設定されていたら高速化構造を作らない。
                // VRAMの内訳(BLAS/TLASがどれだけ占めているか)を切り分けるためだけのもの
                size_t envLength = 0;
                char envValue[8] = {};
                const bool skipRaytracing =
                    (getenv_s(&envLength, envValue, sizeof(envValue), "KURENAI_NO_RT") == 0 && envLength > 0);
                if (skipRaytracing)
                {
                    Core::Logger::Warning(
                        "KurenaiEngine3D",
                        "KURENAI_NO_RTが設定されているため、レイトレーシングの高速化構造を構築しません(計測用)");
                }
                const auto startTime = std::chrono::steady_clock::now();
                auto rebuilt = std::make_unique<Assets::RaytracingScene>();
                if (!skipRaytracing && rebuilt->Build(*m_Device, m_Scene))
                {
                    const double elapsedMs =
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - startTime).count();
                    std::lock_guard<std::mutex> lock(m_RaytracingRebuiltMutex);
                    m_RaytracingRebuildLastMs = elapsedMs;
                    m_RaytracingRebuilt = std::move(rebuilt);
                    m_RaytracingRebuiltGeneration = m_StreamingGeneration;
                }
                // 【成否にかかわらず必ず降ろす】
                m_RaytracingRebuildInFlight.store(false, std::memory_order_release);
            }

            if (sceneIndex < 0)
            {
                continue;
            }

            // 先に破棄を済ませてから読み込む(Renderスレッドは手放す前にWaitForGPUIdle済み)。
            // 新シーンを作る前に旧シーンを解放することで、VRAMの二重常駐を避ける
            destroyRetiredAssets();

            if (sceneIndex < 0)
            {
                continue;
            }

            std::unique_ptr<LoadedScene> loaded = LoadSceneOnLoaderThread(static_cast<size_t>(sceneIndex));
            if (!loaded)
            {
                // 読み込みに失敗した場合も「読み込み中」状態を解除しないとUIが固まるため、
                // 空の完成品を渡してRenderスレッドに終了を知らせる(シーンは空のままになる)
                loaded = std::make_unique<LoadedScene>();
                loaded->SceneIndex = static_cast<size_t>(sceneIndex);
                loaded->Camera = ComputeInitialCamera(loaded->Scene);
            }

            {
                std::lock_guard<std::mutex> lock(m_LoadedSceneMutex);
                m_LoadedScene = std::move(loaded);
            }
        }

        // 停止時に残っている破棄依頼をこのスレッドで片付ける
        destroyRetiredAssets();

        // 破棄待ちの残りもここで片付ける
        destroyStreamedModels();

        // ストリーミング用の共有テクスチャも、確保したのと同じLoaderスレッドで解放する
        // (アセット用ディスクリプタヒープはロックを持たない。RetiredAssetsのコメント参照)
        m_StreamingTexturePool.reset();

        if (SUCCEEDED(comResult))
        {
            CoUninitialize();
        }
    }

    std::unique_ptr<KurenaiEngine3D::LoadedScene> KurenaiEngine3D::LoadSceneOnLoaderThread(size_t sceneIndex)
    {
        if (sceneIndex >= m_SceneFilePaths.size())
        {
            Core::Logger::Error("KurenaiEngine3D", "LoadSceneOnLoaderThread: シーン番号が範囲外です");
            return nullptr;
        }

        // [Model]Pathの基準ディレクトリ(Assetsルート)。.kmodel自身の内部パス(.kmodelがある
        // ディレクトリからの相対)とは基準が異なる点に注意(SceneLoader.h参照)
        const std::wstring assetRootDirectory = GetModuleDirectory() + L"Assets\\";

        auto loaded = std::make_unique<LoadedScene>();
        loaded->SceneIndex = sceneIndex;

        // 読み込み進捗。UIの進捗ウィンドウ(UIManager)がatomicを読んで出す。
        //
        // 【ログにも出す】UIを開いていない・F1で隠している・ヘッドレスに近い確認では
        // 画面の表示が見えない。一定間隔でログへ落としておけば後からでも追える。
        // 1件ごとに出すと767行になるため、間隔を空けて間引く
        m_SceneLoadProgressLoaded.store(0, std::memory_order_relaxed);
        m_SceneLoadProgressTotal.store(0, std::memory_order_relaxed);
        const std::wstring& progressSceneFileName = m_SceneFilePaths[sceneIndex];
        auto lastProgressLogTime = std::chrono::steady_clock::now();
        const auto onProgress =
            [this, &lastProgressLogTime, &progressSceneFileName](size_t loadedModels, size_t totalModels)
        {
            m_SceneLoadProgressLoaded.store(static_cast<uint32_t>(loadedModels), std::memory_order_relaxed);
            m_SceneLoadProgressTotal.store(static_cast<uint32_t>(totalModels), std::memory_order_relaxed);

            const auto now = std::chrono::steady_clock::now();
            const bool isFirstOrLast = (loadedModels == 0) || (loadedModels == totalModels);
            const bool intervalElapsed =
                std::chrono::duration<float>(now - lastProgressLogTime).count() >= kSceneLoadProgressLogIntervalSeconds;
            if (!isFirstOrLast && !intervalElapsed)
            {
                return;
            }
            lastProgressLogTime = now;
            Core::Logger::Info(
                "KurenaiEngine3D",
                "シーン読み込み: " + std::to_string(loadedModels) + " / " + std::to_string(totalModels) +
                    " モデル (" + WideToUtf8(progressSceneFileName) + ")");
        };

        try
        {
            loaded->Scene = Assets::LoadScene(*m_Device, m_SceneFilePaths[sceneIndex], assetRootDirectory, onProgress);
        }
        catch (const std::exception& e)
        {
            Core::Logger::Error(
                "KurenaiEngine3D",
                "シーンの読み込みに失敗しました: " + WideToUtf8(m_SceneFilePaths[sceneIndex]) + " : " + e.what());
            return nullptr;
        }

        // [Scene]Skyboxでスカイボックスを差し替える(指定が無ければ既定へ戻す)。
        // 「今どのスカイボックスを読み込み済みか」を知っているのはこのスレッドだけなので、
        // 差し替えが要るかの判定もここで行う(不要ならSkyboxTextureをnullptrのままにして
        // Renderスレッドへ「現状維持」を伝える)
        const std::wstring desiredSkyboxPath =
            loaded->Scene.SkyboxPath.empty() ? m_DefaultSkyboxPath : loaded->Scene.SkyboxPath;
        if (desiredSkyboxPath != m_LoaderSkyboxPath)
        {
            try
            {
                loaded->SkyboxTexture = m_Device->CreateTextureFromFile(desiredSkyboxPath, false);
                loaded->SkyboxPath = desiredSkyboxPath;
                m_LoaderSkyboxPath = desiredSkyboxPath;
                Core::Logger::Info("KurenaiEngine3D", "スカイボックスを差し替えました: " + WideToUtf8(desiredSkyboxPath));
            }
            catch (const std::exception& e)
            {
                // 読み込みに失敗しても現在のスカイボックスのまま描画を続ける(シーン切り替え自体は成立させる)
                Core::Logger::Error(
                    "KurenaiEngine3D",
                    "スカイボックスの読み込みに失敗しました。現在のスカイボックスを維持します: " +
                        WideToUtf8(desiredSkyboxPath) + " : " + e.what());
            }
        }

        // [Water]NormalMapで水面法線マップを差し替える(水面マテリアル基盤)。
        // スカイボックスと同じ「このスレッドだけが現在の読み込み済みパスを知っている」方式だが、
        // 空文字列が「1x1のフラット法線フォールバックを使う」という有効な指定である点が異なる
        // (スカイボックスの空文字列は「既定のSky.ddsを使う」という意味で、常に何らかのファイルを
        // 読む。水面はファイルを読まない状態そのものが正しいシーンがあるため、ここは分岐が要る)
        const std::wstring& desiredWaterNormalMapPath = loaded->Scene.WaterNormalMapPath;
        if (desiredWaterNormalMapPath != m_LoaderWaterNormalMapPath)
        {
            if (desiredWaterNormalMapPath.empty())
            {
                // フラット法線(128,128,255,255=接線空間で真上を向く法線)へ戻す。
                // ModelLoader.cppが法線マップ未指定のマテリアルに使うプレースホルダーと同じ値
                loaded->WaterNormalMapTexture = m_Device->CreateSolidColorTexture(128, 128, 255, 255);
                loaded->WaterNormalMapPath.clear();
                m_LoaderWaterNormalMapPath.clear();
                Core::Logger::Info("KurenaiEngine3D", "水面法線マップをフラットへ戻しました(NormalMap未指定)");
            }
            else
            {
                try
                {
                    loaded->WaterNormalMapTexture = m_Device->CreateTextureFromFile(desiredWaterNormalMapPath, false);
                    loaded->WaterNormalMapPath = desiredWaterNormalMapPath;
                    m_LoaderWaterNormalMapPath = desiredWaterNormalMapPath;
                    Core::Logger::Info(
                        "KurenaiEngine3D", "水面法線マップを差し替えました: " + WideToUtf8(desiredWaterNormalMapPath));
                }
                catch (const std::exception& e)
                {
                    // 読み込みに失敗しても現在の水面法線マップ(またはフラット法線)のまま描画を続ける
                    Core::Logger::Error(
                        "KurenaiEngine3D",
                        "水面法線マップの読み込みに失敗しました。現在の状態を維持します: " +
                            WideToUtf8(desiredWaterNormalMapPath) + " : " + e.what());
                }
            }
        }

        // レイトレーシングの高速化構造(BLAS/TLAS)とシーンジオメトリの統合バッファを構築する。
        // 非対応環境(DX11、Tier 1.1未満のアダプタ)では何も作らず、描画側は従来の
        // スクリーンスペース手法のまま動く。構築に失敗しても描画は継続する
        //
        // 【ストリーミング中のシーンでは構築しない】読み込み時点でモデルの実体が1つも無く、
        // BLASを作る材料が無い。常駐が増減するたびにTLASと統合バッファを作り直す仕組みは
        // まだ入れていないため、いまは構築を見送って理由をログに残す
        // (ストリーミングは既定で無効なので、既存シーンのレイトレーシングは何も変わらない)
        if (m_Device->SupportsRaytracing())
        {
            if (loaded->Scene.HasStreamingDistance)
            {
                Core::Logger::Info(
                    "KurenaiEngine3D",
                    "ストリーミング対象のシーンでは、モデルが常駐してからレイトレーシングの"
                    "高速化構造を構築します(常駐が変わるたびに作り直します)");
            }
            else
            {
                loaded->RaytracingScene.Build(*m_Device, loaded->Scene);
            }
        }

        loaded->Camera = ComputeInitialCamera(loaded->Scene);
        return loaded;
    }

    void KurenaiEngine3D::ApplyLoadedScene(LoadedScene& loaded)
    {
        // カメラ保持は「同じシーンをもう一度読む」ときにだけ効かせる。
        // 別のシーンへ切り替えたときまで前のカメラを引き継ぐと、まったく違う縮尺・位置の
        // シーンの外へ放り出される(near/farも前のシーンのAABB由来のまま残る)。
        // m_CurrentSceneIndexはこの直後に上書きされるため、比較はここで済ませておく
        const bool isSameSceneReload = (loaded.SceneIndex == m_CurrentSceneIndex);

        m_Scene = std::move(loaded.Scene);
        m_RaytracingScene = std::move(loaded.RaytracingScene);
        m_CurrentSceneIndex = loaded.SceneIndex;

        // ストリーミングの状態もシーンに紐づく。世代を進めることで、切り替え前に発注して
        // まだ届いていない完成品を確実に捨てる(そのまま差し込むと別シーンのモデルが混ざる)
        ++m_StreamingGeneration;
        m_StreamingInFlight.clear();
        m_RaytracingRebuildPending = false;
        {
            // 【ここでresetしてはいけない】Renderスレッドでの解放になる。
            // 受け取り待ちの完成品も、破棄はLoaderスレッドへ回す
            std::unique_ptr<Assets::RaytracingScene> stale;
            {
                std::lock_guard<std::mutex> lock(m_RaytracingRebuiltMutex);
                stale = std::move(m_RaytracingRebuilt);
            }
            if (stale)
            {
                std::lock_guard<std::mutex> lock(m_RaytracingReleaseMutex);
                m_RaytracingRelease.push_back(std::move(stale));
            }
        }
        {
            std::lock_guard<std::mutex> lock(m_StreamingLoadedMutex);
            m_StreamingLoaded.clear();
        }

        // モデルLODの状態はシーンに紐づくので必ず捨てる。
        // 【要素数の一致だけを見て使い回してはいけない】たまたま同じインスタンス数の
        // シーンへ切り替えたときに、前のシーンの段とフェード途中の状態が残る
        m_InstanceLODStates.assign(m_Scene.Instances.size(), InstanceLODState{});

        // インスタンシング用のインスタンスバッファをシーンの規模で作り直す。
        //
        // 【容量はインスタンス数×2】バッチの組は「そのフレームに選ばれた段」と
        // 「最も粗い段」の2組あり、同じインスタンスが両方に載る。最悪でも全インスタンスが
        // 両組でバッチに入るだけなので、これで足りる。
        // インスタンスが1つも無いシーンではバッファを作らない
        // (BuildInstanceBatchesがnullptrを見て何もしない)
        m_ModelInstanceBuffer.reset();
        if (!m_Scene.Instances.empty())
        {
            RHI::BufferDesc instanceBufferDesc;
            instanceBufferDesc.Usage = RHI::BufferUsage::StructuredReadOnly;
            instanceBufferDesc.SizeInBytes =
                static_cast<uint32_t>(sizeof(GPUModelInstance) * m_Scene.Instances.size() * 2);
            instanceBufferDesc.StrideInBytes = sizeof(GPUModelInstance);
            // 更新はBuildInstanceBatchesの1フレーム1回だけ。DX12のステージングリングは
            // この値×kFrameCount+1段を常時確保するので、必要最小限にしておく
            instanceBufferDesc.MaxUpdatesPerFrame = 1;
            m_ModelInstanceBuffer = m_Device->CreateBuffer(instanceBufferDesc);
        }
        m_InstanceBatchesCurrentLOD.clear();
        m_InstanceBatchesCoarsestLOD.clear();
        m_InstanceBatchedCurrentLOD.clear();
        m_InstanceBatchedCoarsestLOD.clear();
        m_ModelInstanceRecords.clear();

        // [Sun]/[Camera]セクションが無いシーンでは、Sceneの側でこのメンバの既定値
        // (従来のKurenaiEngine3Dの初期値と同じ)が使われるため、常にそのまま反映してよい
        m_TimeOfDay = m_Scene.SunTimeOfDay;
        m_SunAzimuthDegrees = m_Scene.SunAzimuthDegrees;
        // .ksceneが持つのは「影を出すか」の真偽値だけなので、手法の選択はエンジン側で決める
        // (反射のm_ReflectionModeと同じ扱い)。規則はDefaultShadowModeに1か所だけ置いてある
        m_ShadowMode = m_Scene.ShadowEnabled ? DefaultShadowMode(m_RaytracingAvailable) : ShadowMode::Off;
        m_SunEnabled = m_Scene.SunEnabled;
        m_AOEnabled = m_Scene.AOEnabled;
        // .ksceneが持つのは「反射を使うか」の真偽値だけなので、手法の選択はエンジン側で決める。
        //
        // 【キーを書いたシーンと書いていないシーンを区別する】書いていなければエンジンの既定
        // (DefaultReflectionMode。RTが使えない環境では反射なし)に従い、書いてあればその指定を
        // 優先して手法だけを環境から選ぶ(ReflectionModeForCapability)。
        // 区別せずに「= true」のときもエンジンの既定へ問い合わせ直すと、DX11ではシーンの指定が
        // 握り潰されて反射が出なくなる(両関数のコメント参照)
        m_ReflectionMode = m_Scene.HasSSREnabledOverride
            ? (m_Scene.SSREnabled ? ReflectionModeForCapability(m_RaytracingAvailable) : ReflectionMode::Off)
            : DefaultReflectionMode(m_RaytracingAvailable);
        // UIの「既定値に戻す」はエンジンの既定ではなくここへ戻す(m_SceneDefaultReflectionMode参照)
        m_SceneDefaultReflectionMode = m_ReflectionMode;
        // TAAと内部レンダー解像度。どちらも反射と同じく「キーを書いたシーンだけ」上書きし、
        // 書いていないシーンはエンジンの既定のまま(Assets::Scene の Has〜Override のコメント参照)
        if (m_Scene.HasTAAOverride)
        {
            m_TAAEnabled = m_Scene.TAAEnabled;
        }
        if (m_Scene.HasRenderResolutionOverride)
        {
            // 即時に作り直すとGPUがまだ参照しているテクスチャを壊すので、
            // UIのシステムパネルと同じく要求だけ記録してRender()の先頭で反映させる。
            //
            // 超解像が有効なときはシーンの指定を「出力解像度」として解釈する。シーンが意図して
            // いるのは「この絵をこの大きさで見せたい」であって内部で何画素描くかではないため。
            // 超解像が無効ならRequestUpscaleSettingsは中でRequestRenderResolutionを呼ぶだけなので、
            // 従来とまったく同じ動作になる
            RequestUpscaleSettings(
                m_UpscaleEnabled, m_UpscaleQualityMode, m_Scene.RenderWidth, m_Scene.RenderHeight);
        }
        // トーンマップのカーブと空の彩度(アート指定)をシーンから受け取る。
        // Source/LibraryはSource/Engineに依存できないため、Scene側は同じ並びの独立した列挙を持つ。
        // 【並びを変えたら両方直すこと】(Assets/Scene.h の TonemapCurveSetting)
        switch (m_Scene.Tonemap)
        {
        case Assets::Scene::TonemapCurveSetting::Reinhard: m_TonemapCurve = TonemapCurve::Reinhard; break;
        case Assets::Scene::TonemapCurveSetting::ACES:     m_TonemapCurve = TonemapCurve::ACES;     break;
        case Assets::Scene::TonemapCurveSetting::AgX:      m_TonemapCurve = TonemapCurve::AgX;      break;
        }
        // 黒の締め。Tonemap/SkySaturationと同じく無条件に反映する(既定0で恒等のため)
        m_TonemapBlackPoint = m_Scene.TonemapBlackPoint;
        m_SkySaturation = m_Scene.SkySaturation;
        if (m_Scene.HasIBLIntensityOverride)
        {
            m_IBLIntensity = m_Scene.IBLIntensity;
        }
        // シーン全体の露出。IBLIntensityと同じく指定されたときだけ上書きする。
        // 屋外の風景と屋内では被写体の輝度が桁で違うため、エンジンの既定値(屋内基準)を
        // 動かさずにシーン側で持てるようにしてある(Scene.h の HasExposureOverride 参照)
        if (m_Scene.HasExposureOverride)
        {
            m_SceneExposureEV100 = m_Scene.ExposureEV100;
        }
        // 雲。天候はシーンの性質なので[Cloud]セクションで持てるようにした。
        // 露出と同じく指定されたキーだけを上書きする。CellSizeだけは.kscene側が「雲の塊1つの
        // 大きさ[m]」で持ち、エンジン側はその逆数(UVスケール)を持つので変換する
        if (m_Scene.HasCloudCoverage)  { m_CloudCoverage = m_Scene.CloudCoverage; }
        if (m_Scene.HasCloudAltitude)  { m_CloudAltitude = m_Scene.CloudAltitude; }
        if (m_Scene.HasCloudThickness) { m_CloudThickness = m_Scene.CloudThickness; }
        if (m_Scene.HasCloudDensity)   { m_CloudDensity = m_Scene.CloudDensity; }
        if (m_Scene.HasCloudCellSize)  { m_CloudUvScale = 1.0f / std::max(m_Scene.CloudCellSize, 1.0f); }
        if (m_Scene.HasCirrusCoverage) { m_CirrusCoverage = m_Scene.CirrusCoverage; }
        // 大気遠近。[Cloud]と同じく指定されたキーだけを上書きする。
        // 【この値は遠景の霞だけの設定ではない】消散係数は雲がどれだけ空から浮き上がって
        // 見えるかも一手に決める(Scene.h の HasFogDensity 付近のコメントに実測を残してある)
        if (m_Scene.HasFogEnabled)     { m_FogEnabled = m_Scene.FogEnabled; }
        if (m_Scene.HasFogDensity)     { m_FogDensity = m_Scene.FogDensity; }
        if (m_Scene.HasFogScaleHeight) { m_FogScaleHeight = m_Scene.FogScaleHeight; }
        if (m_Scene.HasFogRefHeight)   { m_FogRefHeight = m_Scene.FogRefHeight; }
        // ブルーム。エンジンの既定は無効なので、夜景で光源が主役になるシーンは
        // ここで有効にしないと発光体に光芒が出ない
        if (m_Scene.HasBloomEnabled)   { m_BloomEnabled = m_Scene.BloomEnabled; }
        if (m_Scene.HasBloomStrength)  { m_BloomStrength = m_Scene.BloomStrength; }
        if (m_Scene.HasBloomThreshold) { m_BloomThreshold = m_Scene.BloomThreshold; }
        // 星空。[Cloud]/[Fog]と同じく指定されたキーだけを上書きする
        if (m_Scene.HasStarsEnabled)    { m_StarsEnabled = m_Scene.StarsEnabled; }
        if (m_Scene.HasStarsDensity)    { m_StarsDensity = m_Scene.StarsDensity; }
        if (m_Scene.HasStarsBrightness) { m_StarsBrightness = m_Scene.StarsBrightness; }
        if (m_Scene.HasStarsTwinkle)    { m_StarsTwinkle = m_Scene.StarsTwinkle; }
        // ドローンショー。[Cloud]/[Fog]と同じく指定されたキーだけを上書きする。
        // ショーの中身(点・機体数・秒数・明るさ)は.kshowが持ち、Loaderスレッドで読み込み済み
        if (m_Scene.HasDroneShowEnabled) { m_DroneShowEnabled = m_Scene.DroneShowEnabled; }
        if (m_Scene.HasDroneShowCenter)
        {
            m_DroneShowCenter = { m_Scene.DroneShowCenter[0], m_Scene.DroneShowCenter[1], m_Scene.DroneShowCenter[2] };
        }
        if (m_Scene.HasDroneShowScale)          { m_DroneShowScale = m_Scene.DroneShowScale; }
        // 【Formationsが空でもSetDataを呼ぶ】呼ばなければ前のシーンのショーがそのまま残る。
        // 空を渡せばDroneShow側がエラーを出してm_HasDataをfalseにするので、
        // 「ショーを持たないシーンへ切り替えたのに前の編隊が飛び続ける」を構造的に防げる
        m_DroneShow.SetData(m_Scene.DroneShowData);
        // シーンを跨いでショーの進行が引き継がれると、切り替えるたびに違う編隊から始まって
        // A/B比較の対照が取れなくなる(EV100が引き継がれるのと同じ落とし穴)。必ず0へ戻す
        m_DroneShowTime = 0.0f;

        // 【ドローンショーの有無で反射手法を書き換えてはいけない】
        // 1つの機能の有効/無効が、それとは別の機能の設定(反射手法)を黙って書き換えると、
        // シーンの指定が機能側の都合で覆り、「なぜこのシーンだけ反射手法が違うのか」を
        // 追えなくなる。機体は手続き的に展開するビルボードでTLASに入っておらず、
        // RT反射のレイからは原理的に見えないため、DXR対応環境(DX12)では水面に編隊が映らない。
        // これは既知の制限として受け入れる。映したい場合はUIの「反射」セクションから
        // 手動でScreenSpaceへ切り替える。

        // 水面。[Water]が無いシーンでもScene::WaterWaveScale等はリテラル既定値
        // (EngineDefaults.hを複製したもの、Scene.h参照)を持っているため、常にそのまま反映してよい
        // (m_TimeOfDay/m_SunAzimuthDegreesと同じ扱い)
        m_WaterWaveScale = m_Scene.WaterWaveScale;
        m_WaterWaveSpeed = m_Scene.WaterWaveSpeed;
        m_WaterWaveStrength = m_Scene.WaterWaveStrength;

        // スカイボックスが差し替わった場合のみ非nullptr。IBLの拡散イラディアンス・プリフィルタ済み
        // 鏡面はスカイボックスから焼かれるため、差し替えたらm_IBLBakedを倒して焼き直させる
        if (loaded.SkyboxTexture)
        {
            // 旧スカイボックスもアセット由来なのでLoaderスレッドへ破棄を委ねる。
            // 直前(UpdateSceneStreaming)のWaitForGPUIdleによりGPUはもう参照していない
            RetiredAssets retiredSkybox;
            retiredSkybox.SkyboxTexture = std::move(m_SkyboxTexture);
            RetireAssets(std::move(retiredSkybox));

            m_SkyboxTexture = std::move(loaded.SkyboxTexture);
            m_CurrentSkyboxPath = loaded.SkyboxPath;
            m_IBLBaked = false;
            // 検証用の拡散イラディアンスマップも古いスカイボックス由来のものになるため倒す
            // (実際に焼き直すのは検証トグル・デバッグ表示が有効なときだけ)
            m_IBLIrradianceBaked = false;
        }

        // 水面法線マップが差し替わった場合のみ非nullptr。スカイボックスとまったく同じ方式で
        // 旧テクスチャをLoaderスレッドへ破棄依頼する
        if (loaded.WaterNormalMapTexture)
        {
            RetiredAssets retiredWaterNormalMap;
            retiredWaterNormalMap.WaterNormalMapTexture = std::move(m_WaterNormalMapTexture);
            RetireAssets(std::move(retiredWaterNormalMap));

            m_WaterNormalMapTexture = std::move(loaded.WaterNormalMapTexture);
            m_CurrentWaterNormalMapPath = loaded.WaterNormalMapPath;
        }

        // アセット由来のライトをユーザー編集用のコピーへ複製する(m_Scene.Lightsは直接編集しない。
        // シーンを再読み込みすればアセット既定値に戻るようにするため)。m_Scene.Lightsは
        // SceneLoaderが各ModelInstanceのModel::Lightsをワールド空間へ変換し、.kscene自身の
        // [Light]セクションのライトと合成済みのシーン全体のライト一覧(Scene.h参照)
        m_Lights = m_Scene.Lights;
        m_SelectedLightIndex = m_Lights.empty() ? -1 : 0;
        m_LightOverflowLogged = false;

        // エミッシブ光源のプロキシ(ワールド空間)。**m_Lightsへは混ぜない**(宣言側の注記参照)。
        // ImGuiのライト一覧にも出さないので、m_SelectedLightIndexの範囲は変わらない
        m_EmissiveProxies = m_Scene.EmissiveProxies;
        // インスタンスごとの「プロキシを起こしたか」。DDGIのラスタ経路で引く。
        // ストリーミング中のインスタンスはプロキシを作らないので、ここも自動的に立たない
        m_EmissiveProxyInstances.assign(m_Scene.Instances.size(), false);
        for (const Assets::EmissiveProxy& proxy : m_EmissiveProxies)
        {
            if (proxy.InstanceIndex < m_EmissiveProxyInstances.size())
            {
                m_EmissiveProxyInstances[proxy.InstanceIndex] = true;
            }
            else
            {
                Core::Logger::Warning(
                    "KurenaiEngine3D",
                    "エミッシブ光源のインスタンス番号がシーンの範囲外です: " +
                        std::to_string(proxy.InstanceIndex) + " / " +
                        std::to_string(m_Scene.Instances.size()) + "個");
            }
        }
        m_EmissiveLightsUsedCount = 0;
        m_EmissiveLightsCapLogged = false;
        m_EmissiveLightsValuesLogged = false;
        // Rangeの上限。自発光の強度を上げたときにRangeが数kmまで伸びて、タイルカリングが
        // 全タイルにヒットするのを止める安全弁。シーンAABBの対角より長いRangeに意味は無い
        {
            float diagonalSq = 0.0f;
            for (int axis = 0; axis < 3; ++axis)
            {
                const float extent = m_Scene.BoundsMax[axis] - m_Scene.BoundsMin[axis];
                diagonalSq += extent * extent;
            }
            m_EmissiveLightsMaxRange = (diagonalSq > 0.0f) ? std::sqrt(diagonalSq) : 0.0f;
        }
        // 平面反射。新しいシーンでは水面の構成が変わるため、複数水面高さの警告も仕切り直す
        m_PlanarReflectionMultipleWaterLogged = false;

        // 反射プローブもライトと同じ方針でユーザー編集用のコピーへ複製する。
        // プローブの中身(キューブマップ)はシーンのジオメトリ・ライトに依存するため、
        // シーンを読み込んだら必ず焼き直す必要がある
        m_ReflectionProbes = m_Scene.ReflectionProbes;
        if (m_ReflectionProbes.size() > kMaxReflectionProbes)
        {
            Core::Logger::Warning(
                "KurenaiEngine3D",
                "反射プローブ数が上限(" + std::to_string(kMaxReflectionProbes) + ")を超えたため、先頭から" +
                    std::to_string(kMaxReflectionProbes) + "個のみ使用します: " + std::to_string(m_ReflectionProbes.size()) + "個");
            m_ReflectionProbes.resize(kMaxReflectionProbes);
        }
        // テクスチャの常駐ミップ制御。既定はoffで、.ksceneが明示したシーンだけが有効になる
        // (未指定のシーンは従来どおり全ミップ常駐のままで、見え方もVRAMも変わらない)
        m_TextureStreaming.Configure(m_Scene.TextureStreamingEnabled, m_Scene.TextureStreamingBias);
        // 【読み出しはLoaderスレッドに相乗りする】専用スレッドは立てない(TextureStreaming.h参照)。
        // 要求が積まれたらLoaderスレッドを起こす必要があるので、その手段を渡しておく
        m_TextureStreaming.SetRequestNotifier([this] {
            // 【notifyの前に必ずm_LoadRequestMutexを取る】Loaderスレッドは
            // このミューテックスを持ったまま述語を評価してからwaitへ入る。
            // 取らずにnotifyすると、述語がfalseと出てからwaitへ入るまでの隙間に通知が落ちる。
            // カメラが止まっていてモデルの発注が無いシーンでは、
            // 次に起こす材料が他に無いのでミップの差し替えがそのまま止まる
            { std::lock_guard<std::mutex> lock(m_LoadRequestMutex); }
            m_LoadRequestCV.notify_one();
        });
        m_TextureStreaming.Build(m_Scene, *m_Device);

        m_SelectedProbeIndex = m_ReflectionProbes.empty() ? -1 : 0;
        m_ProbeDebugIndex = 0;
        m_ProbeBaked = false;
        m_ProbeBakeRequested = !m_ReflectionProbes.empty();
        // Realtimeのラウンドロビンは先頭から仕切り直す(シーンが変わればプローブの数も並びも変わる)
        m_ProbeRealtimeProbeIndex = 0;
        m_ProbeRealtimeFace = 0;

        // DDGIボリューム(22章)。現状は先頭の1つだけを使う。複数ボリュームは重なりと優先順位を
        // 決める仕組みがまだ無いため、2つ目以降は警告を出して切り捨てる
        m_HasGIVolume = !m_Scene.GIVolumes.empty();
        if (m_Scene.GIVolumes.size() > 1)
        {
            Core::Logger::Warning(
                "KurenaiEngine3D",
                "[GIVolume]が複数ありますが、現状は先頭の1つだけを使用します: " +
                    std::to_string(m_Scene.GIVolumes.size()) + "個");
        }
        if (m_HasGIVolume)
        {
            m_GIVolume = m_Scene.GIVolumes.front();
            const uint32_t lodCount = std::clamp(m_GIVolume.LODCount, 1u, kDDGIMaxLODCount);
            if (m_GIVolume.LODCount > kDDGIMaxLODCount)
            {
                Core::Logger::Warning(
                    "KurenaiEngine3D",
                    "[GIVolume]のLODCountが上限(" + std::to_string(kDDGIMaxLODCount) + ")を超えているため丸めます: " +
                        std::to_string(m_GIVolume.LODCount));
                m_GIVolume.LODCount = lodCount;
            }
            // 【段数ぶん掛けること】クリップマップLODはプローブ数が段数倍になる。
            // 掛け忘れると上限のチェックが素通りし、確保だけが膨らむ
            const uint64_t probeCount =
                static_cast<uint64_t>(m_GIVolume.ProbeCounts[0]) *
                static_cast<uint64_t>(m_GIVolume.ProbeCounts[1]) *
                static_cast<uint64_t>(m_GIVolume.ProbeCounts[2]) *
                static_cast<uint64_t>(lodCount);
            if (probeCount > kDDGIMaxProbes)
            {
                // 切り捨てでは格子が歪んで意味を成さない(反射プローブのように「先頭N個」で
                // 済ませられない)ため、ボリュームごと無効にして従来のIBLのまま描く
                Core::Logger::Error(
                    "KurenaiEngine3D",
                    "[GIVolume]のプローブ数が上限(" + std::to_string(kDDGIMaxProbes) + ")を超えたためDDGIを無効にします: " +
                        std::to_string(probeCount) + "個(格子 × LOD" + std::to_string(lodCount) +
                        "段)。ProbeCountsかLODCountを減らすかProbeSpacingを広げてください");
                m_HasGIVolume = false;
            }
        }
        RecreateDDGIAtlases();

        // SSAO/SSILの半径やSSRの距離はシーンの規模から決まるため、差し替え後のm_Sceneで計算し直す
        ResetSceneDependentParams();

        // 露出の追従状態はシーンをまたいで持ち越さない。時刻が入れ替わると実効プリ露出は
        // 最大18段跳ぶため、追従の途中で反射プローブが焼かれると桁違いの明るさで固定される
        // (m_ProbeBakedExposureEV100・m_AutoExposureResetRequestedのコメント参照)
        m_EffectiveExposureInitialized = false;
        m_AutoExposureResetRequested = true;

        // TAAの履歴には前のシーンの絵が入っており、この後カメラも新シーンの初期位置へ飛ぶため、
        // 再投影しても対応する画素が存在しない。捨てて今フレームの色から積み直す。
        // ApplyLoadedSceneはRenderスレッドから呼ばれるため、m_Cameraは直接書けないがatomicなら書ける
        // (Renderスレッドが読む。カメラ自体はこの後m_AppliedSceneCamera経由でUpdateスレッドへ渡す)
        m_TAAHistoryValid.store(false, std::memory_order_relaxed);

        // Hi-Zにも前のシーンの深度が入っている。カメラが新シーンの初期位置へ飛ぶ以上、
        // それで遮蔽を判定すると見えているものを消しうる。TAAの履歴と同じ理由で捨てる
        // (ApplyLoadedSceneはRenderスレッドから呼ばれ、m_HiZValidもRenderスレッドしか触らない)
        m_HiZValid = false;

        // ホットリロードの基準時刻を、いま読んだファイルの更新時刻で取り直す。
        // これをしないと (1)シーンを切り替えたあとも前のファイルを見続ける
        // (2)手動の再読み込み直後に「変更あり」と誤検出して延々と再読み込みし続ける
        m_WatchedSceneWriteTime = GetCurrentSceneFileWriteTime();
        m_SceneReloadRejectedWriteTime = 0;

        // 品質プリセット「高」が戻る先を、いまの状態(= .ksceneの指定をすべて反映し終えた状態)で
        // 控える。【この関数内のm_Scene.Has〜による上書きより後で呼ぶこと】先に控えると
        // シーンがSSR/TAA/ブルーム/星を指定していても、エンジンの既定を控えることになる。
        // シーンを切り替えたらプリセットの選択も「高」へ戻す(新しいシーンに対して前のシーンで
        // 選んだ「低」が適用されたままになるわけではなく、実際に高相当の状態になっているため)
        m_SceneDefaultQuality = CaptureQualitySettings();
        m_QualityPreset = QualityPreset::High;

        // 初期カメラとウィンドウタイトルはUpdateスレッドが適用する。m_Cameraの書き込み手を
        // 1スレッドに保ち、ウィンドウタイトルもウィンドウを所有するスレッドから設定するため
        // (UpdateAppliedSceneHandoff参照)
        {
            const wchar_t* apiName = (m_GraphicsAPI == GraphicsAPI::DX12) ? L"DX12" : L"DX11";
            std::lock_guard<std::mutex> lock(m_AppliedSceneMutex);
            // カメラを適用するかどうか。「現在のカメラを保持する」が入っていても
            // ウィンドウタイトルは更新したいので、引き渡し自体は毎回行う。
            // 保持が効くのは同じシーンの読み直しのときだけ(上のisSameSceneReload参照)
            m_AppliedSceneApplyCamera = !(m_SceneReloadKeepsCamera && isSameSceneReload);
            m_AppliedSceneCamera = loaded.Camera;
            m_AppliedSceneTitle = std::wstring(L"Kurenai Engine [") + apiName + L"] - " + m_Scene.Name;
        }
        m_AppliedScenePending.store(true, std::memory_order_release);
    }

    void KurenaiEngine3D::RecreateDDGIAtlases()
    {
        // アトラスの並び: 列 = ProbeCounts.x * ProbeCounts.y、行 = ProbeCounts.z。
        // XY平面のスライスを横に並べ、Zをそのまま行にする(RTXGIと同じ並び)。
        // プローブ番号との対応は index = x + y*Cx + z*Cx*Cy で、シェーダー側の
        // DDGIProbeAtlasCoord()と一致させること
        const uint32_t countX = m_HasGIVolume ? m_GIVolume.ProbeCounts[0] : 1u;
        const uint32_t countY = m_HasGIVolume ? m_GIVolume.ProbeCounts[1] : 1u;
        const uint32_t countZ = m_HasGIVolume ? m_GIVolume.ProbeCounts[2] : 1u;

        m_DDGILODCount = m_HasGIVolume
            ? std::clamp(m_GIVolume.LODCount, 1u, kDDGIMaxLODCount)
            : 1u;
        m_DDGIProbesPerLOD = countX * countY * countZ;
        m_DDGIProbeCount = m_DDGIProbesPerLOD * m_DDGILODCount;

        // 【LODは縦に積むだけ】列は変えず、行だけ段数倍にする。こうすると通し番号
        // slot = k*(Cx*Cy*Cz) + z*Cx*Cy + y*Cx + x に対して
        // 「行 = slot/(Cx*Cy)、列 = slot%(Cx*Cy)」がそのまま LOD k の行 [k*Cz, (k+1)*Cz) を指すので、
        // 更新CS(DDGIProbeUpdate.hlsl)のアトラス座標式を1文字も変えずに済む
        const uint32_t columns = countX * countY;
        const uint32_t rows = countZ * m_DDGILODCount;

        // 【R32系である必要がある】更新CSはヒステリシス(前の値と新しい値のlerp)のために
        // アトラスをRWTexture2Dとして読んでから書く。型付きUAV読み出しはR32系しか保証されておらず、
        // fp16で読むにはTypedUAVLoadAdditionalFormatsが要る(AutoExposure.hlslが同じ理由で
        // R32_Floatを2テクセル並べる構成にしている)。
        // アトラスは455プローブでも合計1.4MB程度と小さいため、精度と可搬性を取って素直にR32にする
        m_DDGIIrradianceAtlas = m_Device->CreateUAVTexture(
            columns * kDDGIIrradianceCell, rows * kDDGIIrradianceCell, RHI::Format::R32G32B32A32_Float);
        // R=平均距離、G=平均二乗距離
        m_DDGIDistanceAtlas = m_Device->CreateUAVTexture(
            columns * kDDGIDistanceCell, rows * kDDGIDistanceCell, RHI::Format::R32G32_Float);

        // 確保し直した直後のアトラスは中身が未定義なので、全スロットを「未確定」として持つ。
        // 【あり得ない座標で埋める】0で埋めると、たまたまその座標を担当するスロットが
        // 「もう焼いてある」と誤判定される
        m_DDGIProbeBakedCoord.assign(
            m_DDGIProbeCount, DirectX::XMINT3{ INT32_MIN, INT32_MIN, INT32_MIN });
        m_DDGIDirtyProbeList.clear();

        // 確保し直した直後のアトラスは中身が未定義なので、一巡目からやり直す
        m_DDGIBaked = false;
        m_DDGIWarmingUp = true;
        m_DDGIUpdateCursor = 0;
        // シーンが変わればメッシュ数も変わるので、クランプの報告も出し直す
        m_DDGIProbesPerFrameClampReported = false;
        m_DDGIOverwriteRemaining = 0;
        m_DDGILastExposureValid = false;

        if (m_HasGIVolume)
        {
            Core::Logger::Info(
                "KurenaiEngine3D",
                "DDGIボリューム '" + m_GIVolume.Name + "' を確保しました: " +
                    std::to_string(countX) + "x" + std::to_string(countY) + "x" + std::to_string(countZ) +
                    " = " + std::to_string(m_DDGIProbeCount) + "プローブ, アトラス " +
                    std::to_string(columns * kDDGIIrradianceCell) + "x" + std::to_string(rows * kDDGIIrradianceCell) +
                    " / " + std::to_string(columns * kDDGIDistanceCell) + "x" + std::to_string(rows * kDDGIDistanceCell));
        }
    }

    uint32_t KurenaiEngine3D::ClampDDGIProbesPerFrameToConstantRing(uint32_t requested)
    {
        // DX11はどちらの上限も持たない(UINT32_MAXが返る)
        const uint32_t maxDraws = m_Device->GetMaxDrawsPerFrame();
        const uint32_t maxWrites =
            m_ObjectConstantBuffer ? m_ObjectConstantBuffer->GetSafeUpdatesPerFrame() : UINT32_MAX;
        // 描画1回につきObjectConstantsを1回書くので、厳しい方に合わせれば両方を満たす
        const uint32_t maxWork = std::min(maxDraws, maxWrites);
        if (maxWork == UINT32_MAX)
        {
            return requested;
        }

        // ラスタ経路が1プローブあたり描き直す不透明メッシュの数。
        // captureDDGIProbeFaceの描画ループと同じ条件で数えること
        uint32_t opaqueMeshCount = 0;
        for (const auto& instance : m_Scene.Instances)
        {
            // 【7545行目のDDGIラスタ経路と同じ段を数えること】ここの数が定数バッファリングの
            // 予算(ClampDDGIProbesPerFrameToConstantRing)を決めるため、実際に描く段と食い違うと
            // 予算の見積もりが狂う
            // ストリーミング中で未読み込みなら描かない
            const Assets::Model* const coarsestModel = GetCoarsestLOD(instance);
            if (!coarsestModel) { continue; }
            for (const auto& mesh : coarsestModel->Meshes)
            {
                if (!mesh.IsTransparent)
                {
                    ++opaqueMeshCount;
                }
            }
        }
        if (opaqueMeshCount == 0)
        {
            return requested;
        }

        const uint32_t reserved = opaqueMeshCount * kDDGIFrameBudgetReserveDrawsPerMesh;
        const uint32_t perProbe = kCubeFaceCount * opaqueMeshCount;
        // 本編のパスだけで予算を使い切る規模のシーンでは、DDGIへ回せる分が残らない。
        // それでも0にはせず1プローブは進める(進めないと永久に焼き上がらないため)。
        // 予算そのものの超過は、それぞれの上限側がエラーとして報告する
        const uint32_t budget = (maxWork > reserved) ? (maxWork - reserved) : 0u;
        const uint32_t allowed = std::max<uint32_t>(1u, budget / perProbe);

        if (requested <= allowed)
        {
            return requested;
        }

        if (!m_DDGIProbesPerFrameClampReported)
        {
            m_DDGIProbesPerFrameClampReported = true;
            Core::Logger::Warning(
                "KurenaiEngine3D",
                "DDGIのラスタ経路が1フレームの予算を超えるため、更新プローブ数を " +
                    std::to_string(requested) + " から " + std::to_string(allowed) +
                    " へ制限しました(不透明メッシュ " + std::to_string(opaqueMeshCount) +
                    " × 6面 × プローブ数。1フレームの上限は 描画 " + std::to_string(maxDraws) +
                    " 回 / 定数書き込み " + std::to_string(maxWrites) +
                    " 回)。収束は遅くなりますが描画は壊れません。レイトレース経路にはこの制限は掛かりません");
        }
        return allowed;
    }

    DirectX::XMFLOAT3 KurenaiEngine3D::ComputeDDGILODSpacing(uint32_t lod) const
    {
        // LODが1つ上がるごとに間隔が2倍(=覆う範囲が2倍)
        const float scale = static_cast<float>(1u << lod);
        return DirectX::XMFLOAT3{
            m_GIVolume.ProbeSpacing[0] * scale,
            m_GIVolume.ProbeSpacing[1] * scale,
            m_GIVolume.ProbeSpacing[2] * scale,
        };
    }

    DirectX::XMINT3 KurenaiEngine3D::ComputeDDGILODBaseIndex(uint32_t lod) const
    {
        const DirectX::XMFLOAT3 spacing = ComputeDDGILODSpacing(lod);
        const int32_t countX = static_cast<int32_t>(m_GIVolume.ProbeCounts[0]);
        const int32_t countY = static_cast<int32_t>(m_GIVolume.ProbeCounts[1]);
        const int32_t countZ = static_cast<int32_t>(m_GIVolume.ProbeCounts[2]);

        if (!m_GIVolume.FollowCamera)
        {
            // 追従しないので格子は動かない。基準は0でよい
            // (トロイダルの写像は基準が何であっても自己整合するが、動かないなら0が素直)
            return DirectX::XMINT3{ 0, 0, 0 };
        }

        // 【そのLOD自身の格子へスナップする】スナップすれば、カメラが動いてもプローブの
        // ワールド座標は動かない。動くのは「どのプローブが範囲に入っているか」だけになり、
        // 範囲に残ったプローブの焼き上がりをそのまま使える。
        // スナップしないと毎フレーム全プローブが焼き直しになる
        const DirectX::XMFLOAT3 camera = m_DDGIFollowCenter;
        const auto snap = [](float centerValue, float spacingValue, int32_t count) -> int32_t
        {
            if (spacingValue <= 0.0f)
            {
                return 0;
            }
            const int32_t centerIndex = static_cast<int32_t>(std::floor(centerValue / spacingValue));
            // カメラを格子の中央へ置く
            return centerIndex - (count - 1) / 2;
        };

        return DirectX::XMINT3{
            snap(camera.x, spacing.x, countX),
            snap(camera.y, spacing.y, countY),
            snap(camera.z, spacing.z, countZ),
        };
    }

    DirectX::XMFLOAT3 KurenaiEngine3D::ComputeDDGILODOrigin(uint32_t lod) const
    {
        const DirectX::XMFLOAT3 spacing = ComputeDDGILODSpacing(lod);

        if (m_GIVolume.FollowCamera)
        {
            const DirectX::XMINT3 base = ComputeDDGILODBaseIndex(lod);
            return DirectX::XMFLOAT3{
                static_cast<float>(base.x) * spacing.x,
                static_cast<float>(base.y) * spacing.y,
                static_cast<float>(base.z) * spacing.z,
            };
        }

        // 追従しない場合、LOD0は.ksceneのOriginをそのまま使う(既存シーンの挙動を1ビットも
        // 変えないため)。上のLODは同じ中心を保ったまま広がるように置く
        if (lod == 0)
        {
            return DirectX::XMFLOAT3{ m_GIVolume.Origin[0], m_GIVolume.Origin[1], m_GIVolume.Origin[2] };
        }

        const auto centered = [this, &spacing](int axis) -> float
        {
            const float count = static_cast<float>(m_GIVolume.ProbeCounts[axis]);
            const float extent0 = (count - 1.0f) * m_GIVolume.ProbeSpacing[axis];
            const float extentK = (count - 1.0f) * (&spacing.x)[axis];
            return m_GIVolume.Origin[axis] + (extent0 - extentK) * 0.5f;
        };
        return DirectX::XMFLOAT3{ centered(0), centered(1), centered(2) };
    }

    DirectX::XMINT3 KurenaiEngine3D::ComputeDDGIProbeWorldCoord(uint32_t probeIndex) const
    {
        const uint32_t countX = m_GIVolume.ProbeCounts[0];
        const uint32_t countY = m_GIVolume.ProbeCounts[1];

        const uint32_t perLOD = std::max(1u, m_DDGIProbesPerLOD);
        const uint32_t lod = std::min(probeIndex / perLOD, m_DDGILODCount - 1u);
        const uint32_t local = probeIndex - lod * perLOD;

        const int32_t atlasX = static_cast<int32_t>(local % countX);
        const int32_t atlasY = static_cast<int32_t>((local / countX) % countY);
        const int32_t atlasZ = static_cast<int32_t>(local / (countX * countY));

        const DirectX::XMINT3 base = ComputeDDGILODBaseIndex(lod);
        const auto unwrap = [](int32_t atlasCoord, int32_t baseCoord, int32_t count) -> int32_t
        {
            return baseCoord + ((atlasCoord - baseCoord) % count + count) % count;
        };

        return DirectX::XMINT3{
            unwrap(atlasX, base.x, static_cast<int32_t>(countX)),
            unwrap(atlasY, base.y, static_cast<int32_t>(m_GIVolume.ProbeCounts[1])),
            unwrap(atlasZ, base.z, static_cast<int32_t>(m_GIVolume.ProbeCounts[2])),
        };
    }

    DirectX::XMFLOAT3 KurenaiEngine3D::ComputeDDGIProbePosition(uint32_t probeIndex) const
    {
        const uint32_t countX = m_GIVolume.ProbeCounts[0];
        const uint32_t countY = m_GIVolume.ProbeCounts[1];
        const uint32_t countZ = m_GIVolume.ProbeCounts[2];

        // 通し番号 → LOD段 → その段の中の位置
        const uint32_t perLOD = std::max(1u, m_DDGIProbesPerLOD);
        const uint32_t lod = std::min(probeIndex / perLOD, m_DDGILODCount - 1u);
        const uint32_t local = probeIndex - lod * perLOD;

        // アトラス上の格子座標(セルの並びそのもの)
        const int32_t atlasX = static_cast<int32_t>(local % countX);
        const int32_t atlasY = static_cast<int32_t>((local / countX) % countY);
        const int32_t atlasZ = static_cast<int32_t>(local / (countX * countY));

        const DirectX::XMFLOAT3 spacing = ComputeDDGILODSpacing(lod);
        const DirectX::XMFLOAT3 origin = ComputeDDGILODOrigin(lod);
        const DirectX::XMINT3 base = ComputeDDGILODBaseIndex(lod);

        // 【トロイダル(剰余)addressingの逆変換】アトラスのセル番号は
        // 「ワールド格子座標 mod プローブ数」なので、いま範囲に入っている区間
        // [base, base + count) の中で、その剰余に一致する格子座標を1つ選び直す。
        // これがそのセルがいま担当しているプローブのワールド位置になる
        const auto unwrap = [](int32_t atlasCoord, int32_t baseCoord, int32_t count) -> int32_t
        {
            const int32_t offset = ((atlasCoord - baseCoord) % count + count) % count;
            return offset;
        };

        const int32_t localX = unwrap(atlasX, base.x, static_cast<int32_t>(countX));
        const int32_t localY = unwrap(atlasY, base.y, static_cast<int32_t>(countY));
        const int32_t localZ = unwrap(atlasZ, base.z, static_cast<int32_t>(countZ));

        return DirectX::XMFLOAT3{
            origin.x + static_cast<float>(localX) * spacing.x,
            origin.y + static_cast<float>(localY) * spacing.y,
            origin.z + static_cast<float>(localZ) * spacing.z,
        };
    }

    uint64_t KurenaiEngine3D::ComputeProbeBakeSignature() const
    {
        // FNV-1a(64bit)。焼き上がりに影響する値だけを順に混ぜる。衝突しても起きるのは
        // 「本来必要な焼き直しを1回取りこぼす」だけで破綻はしないため、この程度の強度で足りる
        uint64_t hash = 1469598103934665603ull;
        const auto mixBytes = [&hash](const void* data, size_t size)
        {
            const auto* bytes = static_cast<const unsigned char*>(data);
            for (size_t i = 0; i < size; ++i)
            {
                hash ^= bytes[i];
                hash *= 1099511628211ull;
            }
        };
        const auto mixFloat = [&mixBytes](float value) { mixBytes(&value, sizeof(value)); };
        const auto mixBool = [&mixBytes](bool value) { const unsigned char v = value ? 1u : 0u; mixBytes(&v, sizeof(v)); };

        // 太陽と昼夜サイクル。ProbeCapture.hlslは共有のFrameConstantsから太陽の向き・色を読むため、
        // 時刻を動かすと焼き上がりが変わる
        mixFloat(m_TimeOfDay);
        mixFloat(m_SunAzimuthDegrees);
        mixBool(m_SunEnabled);
        // 影の手法ではなく「影を落とすかどうか」だけを混ぜる。ProbeCapture.hlslが読むのは
        // 常にカスケードシャドウマップで、そのシャドウマップはRTシャドウ選択時も同じように
        // 描かれるため、CascadedShadowMapとRaytracedでプローブの焼き上がりは変わらない
        mixBool(m_ShadowMode != ShadowMode::Off);
        // DDGIのレイの取得(ラスタライズ / レイトレーシング)と、その影レイの有無。
        //
        // 【混ぜ忘れると「つまみが効かない」型の不具合になる】切り替えても署名が変わらないため
        // 焼き直しが起きず、収束済みで停止しているモードでは絵が一切変わらない。
        // 反射プローブはこの2つの影響を受けないが、署名を共有しているため一緒に焼き直しになる
        // (余分な焼き直しが1回起きるだけで、破綻はしない)
        mixBool(m_DDGIRayMode == DDGIRayMode::Raytraced);
        mixBool(m_DDGISunShadowRayEnabled);
        // 月は時刻に連動せず手動指定なので、太陽とは別に混ぜる必要がある。太陽が沈むと
        // 平行光源の枠が月へ切り替わり、キャプチャの直接光がそのまま変わる
        mixFloat(m_MoonAzimuthDegrees);
        mixFloat(m_MoonElevationDegrees);
        // キャプチャ内の環境項はグローバルIBLを引くため、その強度も焼き上がりに影響する。
        // 手続き空か.ksceneのDDSかで空そのものが変わるため、その切り替えも含める。
        // 拡散・鏡面の倍率もProbeCapture.hlslが同じように適用するため署名へ含める
        // (含め忘れると、つまみを動かしてもプローブの中身だけ古い倍率のまま残る)
        mixFloat(m_IBLEnabled ? m_IBLIntensity : 0.0f);
        mixFloat(m_AmbientDiffuseScale);
        mixFloat(m_AmbientSpecularScale);
        mixBool(m_ProceduralSkyEnabled);
        // 自発光の強度倍率はキャプチャのエミッシブ項へそのまま乗る
        mixFloat(m_EmissiveIntensity);
        // エミッシブ光源(62章)。プロキシはProbeCapture.hlslのライトループ(t8)にも入るので、
        // 有効/無効・打ち切り照度・採用数の上限はどれも焼き上がりを変える。
        // 二重計上の抑止はDDGIのキャプチャから自発光を抜くので、これも焼き上がりを変える。
        // **混ぜ忘れると「つまみが効かない」型の不具合になる**(このすぐ上の注記と同じ)
        mixBool(m_EmissiveLightsEnabled);
        mixFloat(m_EmissiveLightsCutoffIrradiance);
        mixFloat(static_cast<float>(m_EmissiveLightsMaxCount));
        mixBool(m_EmissiveLightsDoubleCountGI);
        // 【上限に当たると採用集合がカメラ依存になる】採用順はカメラからの照度で決まるため、
        // 上の4つだけでは「カメラを動かしただけで焼く光源が変わったのに署名は同じ」になる。
        // 切り捨てが起きていないフレームでは0で固定なので、余分な焼き直しは起きない
        mixBytes(&m_EmissiveLightsSelectionHash, sizeof(m_EmissiveLightsSelectionHash));

        // bent normalによる遮蔽(34章)。ProbeCapture.hlslが同じ分岐を持つため、
        // 含め忘れるとつまみを動かしてもプローブの中身だけ古いまま残る
        mixBool(m_BentNormalAOSource);
        mixFloat(static_cast<float>(m_SpecularOcclusionMode));
        mixBool(m_MultiBounceAOEnabled);

        // ライトは構造体ごとダンプすると詰め物(padding)の未初期化バイトを拾い得るため、
        // 使うフィールドだけを明示的に混ぜる
        for (const Assets::Light& light : m_Lights)
        {
            mixBytes(&light.Type, sizeof(light.Type));
            for (int i = 0; i < 3; ++i) mixFloat(light.Position[i]);
            for (int i = 0; i < 3; ++i) mixFloat(light.Direction[i]);
            for (int i = 0; i < 3; ++i) mixFloat(light.Color[i]);
            mixFloat(light.Intensity);
            mixFloat(light.Range);
            mixFloat(light.SpotInnerConeAngle);
            mixFloat(light.SpotOuterConeAngle);
            mixBool(light.Enabled);
        }

        // プローブの位置はキャプチャ地点そのものなので含める(影響範囲は含めない。
        // 形状・半径・ブレンド距離を変えてもどこから撮るかは変わらないため)
        for (const Assets::ReflectionProbe& probe : m_ReflectionProbes)
        {
            for (int i = 0; i < 3; ++i) mixFloat(probe.Position[i]);
        }

        return hash;
    }

    void KurenaiEngine3D::ResetSceneDependentParams()
    {
        const float sizeY = m_Scene.BoundsMax[1] - m_Scene.BoundsMin[1];
        const float dx = m_Scene.BoundsMax[0] - m_Scene.BoundsMin[0];
        const float dz = m_Scene.BoundsMax[2] - m_Scene.BoundsMin[2];
        const float diagonal = std::sqrt(dx * dx + sizeY * sizeY + dz * dz);

        // SSAO/SSILのサンプリング半径はシーンの規模に応じて変わるべきなので、対角線に比例させる
        // (小さすぎる/大きすぎるシーンでも遮蔽表現が破綻しないよう妥当な範囲にクランプする)
        m_SSAORadius = std::clamp(diagonal * 0.01f, 0.05f, 2.0f);
        m_SSILRadius = m_SSAORadius;
        m_SSILThickness = m_SSILRadius * 0.2f;

        // SSRの最大レイ距離もシーンの規模に応じて変わるべきなので、対角線に比例させる。
        // ヒット判定の厚みはSSAO/SSILと同様、遮蔽・接触判定として妥当な小さい値にする
        m_SSRMaxDistance = std::clamp(diagonal * 0.5f, 1.0f, 100.0f);
        m_SSRThickness = m_SSAORadius * 0.2f;

        // RT反射のレイ距離はSSRより長く取る。SSRは「画面外へ出たら打ち切り」で早々に確信度0へ
        // 落ちるためシーン対角の半分でも足りるが、RTは画面外も追えるので短く切ると
        // 本来映るはずの建物を通り越して空が映ってしまう。シーン対角そのものを上限にする
        m_RTReflectionMaxDistance = std::clamp(diagonal, 1.0f, 500.0f);

        // RTAOのレイ距離はSSAO/SSILの半径より長く取る。スクリーンスペース手法は
        // 半径を伸ばすほど画面上のサンプル間隔が粗くなって破綻するが、RTには
        // その制約が無く、部屋の広さ程度まで伸ばしたほうがバウンス光が正しく回る
        m_RTAOMaxDistance = std::clamp(diagonal * 0.03f, 0.1f, 10.0f);

        // カメラの移動速度。.ksceneが[Scene]CameraSpeedを持っていればそれを使い、
        // 無ければシーン対角から決める。
        //
        // 【比例と下限の2段】基準はEmeraldSquare(対角344.6m)で従来どおりの5 m/sになる比例式。
        // それより小さいシーンは従来の5 m/sで既に使いやすいので下限で据え置く
        // (比例だけだとSponza(対角37.1m)が0.54 m/sになり、逆に遅くなる)。
        // 根拠と実測はEngineDefaults.hのCameraSpeed一式のコメントに置いてある
        m_CameraSpeed = m_Scene.HasCameraSpeed
            ? m_Scene.CameraSpeed
            : (std::max)(
                  Defaults::CameraSpeedMin,
                  diagonal / Defaults::CameraSpeedReferenceDiagonal * Defaults::CameraSpeed);

        // 【必ずログに出す】速度は絵に写らないため、「効いていない」と「効いているが
        // 想定と違う値になっている」を見た目では区別できない。シーンごとの実効値を残しておく
        char cameraSpeedText[192];
        std::snprintf(
            cameraSpeedText, sizeof(cameraSpeedText),
            "カメラ移動速度: %.2f m/s (Shift時 %.2f m/s) [シーン対角 %.1f m / %s]",
            m_CameraSpeed, m_CameraSpeed * Defaults::CameraSpeedShiftMultiplier, diagonal,
            m_Scene.HasCameraSpeed ? "[Scene]CameraSpeedの指定" : "対角からの自動決定");
        Core::Logger::Info("KurenaiEngine3D", cameraSpeedText);
    }

    // 歩き回る視点のカメラの近平面を求める。シーン対角に比例させつつ、上限で頭打ちにする。
    //
    // 【比例させるだけでは足元が丸ごと消える】diagonal * 0.0005 は「near:far比を一定に保って
    // 深度精度を確保する」という経験則で、深度をNDCへほぼ1/zで写す従来のZバッファを前提にしている。
    // このエンジンはReverse-Z + D32_FLOATで、1/zが近平面側へ寄せる分布と浮動小数点の指数が
    // 0付近で細かくなる性質がちょうど噛み合うため、近平面を小さくしても遠方の精度がほとんど落ちない
    // (Reverse-Zを採る目的がまさにこれ)。一方で近平面が大きいままだと、その距離より手前の
    // ジオメトリはラスタライズ前に丸ごと捨てられる。
    //
    // 実測: 6000m四方の干潟のシーン(対角約8487m)ではこの式が near = 4.24m を返し、水面の
    // 1.45m上に置いたカメラを俯角19.9度より下へ向けると水面が画面から丸ごと消えた
    // (G-Bufferのアルベドも水面マスクも0、つまり「暗く描かれている」のではなく「何も描かれて
    // いない」状態になり、背景として空モデルの下半球の色が見えていた)。
    // 上限は視点の高さ(人の目線で1.6m前後)に対して十分小さい値として0.1mを採る。
    // 対角200m以下のシーンでは元の式が0.1mを下回るため、この上限は効かない(挙動が変わらない)。
    float ComputeWalkableNearZ(float diagonal)
    {
        return std::clamp(diagonal * 0.0005f, 0.01f, 0.1f);
    }

    Core::Camera KurenaiEngine3D::ComputeInitialCamera(const Assets::Scene& scene)
    {
        Core::Camera camera;
        const float sizeY = scene.BoundsMax[1] - scene.BoundsMin[1];
        const float dx = scene.BoundsMax[0] - scene.BoundsMin[0];
        const float dz = scene.BoundsMax[2] - scene.BoundsMin[2];
        const float diagonal = std::sqrt(dx * dx + sizeY * sizeY + dz * dz);

        if (scene.HasCameraOverride)
        {
            camera.SetPosition({ scene.CameraPosition[0], scene.CameraPosition[1], scene.CameraPosition[2] });
            camera.SetYawPitch(scene.CameraYaw, scene.CameraPitch);
            camera.SetLens(DirectX::XM_PIDIV4, ComputeWalkableNearZ(diagonal), std::max(100.0f, diagonal * 4.0f));
            return camera;
        }

        const float centerX = (scene.BoundsMin[0] + scene.BoundsMax[0]) * 0.5f;
        const float centerY = (scene.BoundsMin[1] + scene.BoundsMax[1]) * 0.5f;
        const float centerZ = (scene.BoundsMin[2] + scene.BoundsMax[2]) * 0.5f;
        const float eyeHeight = scene.BoundsMin[1] + sizeY * 0.15f;

        const float longAxis = std::max(dx, dz);
        const float shortAxis = std::min(dx, dz);
        // 短辺が長辺に対して極端に短い場合は、歩いて回れる建物内部ではなく横に並んだ物体と判断し、
        // 内部に入り込む配置ではなく外側から全体を見渡す配置にする
        const bool isThinProp = shortAxis < longAxis * 0.15f;

        float posX;
        float posY;
        float posZ;
        float yaw;
        float nearZ;
        const float farZ = std::max(100.0f, diagonal * 4.0f);

        if (isThinProp)
        {
            // 縦FOVの半角のtanを使い、アスペクト比に依らず長辺全体が収まる距離を保守的に求める
            const float halfFovTan = std::tan(DirectX::XM_PIDIV4 * 0.5f);
            const float requiredDistance = (longAxis * 0.5f) / halfFovTan * 1.25f;

            posX = centerX;
            posY = centerY;
            posZ = centerZ + requiredDistance;
            yaw = DirectX::XM_PI;

            // カメラは物体から離れた位置にあるため、near平面をdiagonal基準の極小値のままにすると
            // 深度バッファの精度が視距離全体で失われてしまう(near:distance比が極端になるため)。
            // 実際の視距離に応じたスケールにして深度精度を確保する
            nearZ = std::max(0.05f, requiredDistance * 0.02f);
        }
        else if (dx >= dz)
        {
            // ホールの長辺方向の端寄りから中心を見る位置を初期視点にする(中央の装飾物や壁に埋まらないように)
            posX = scene.BoundsMin[0] + dx * 0.2f;
            posY = eyeHeight;
            posZ = centerZ;
            yaw = DirectX::XM_PIDIV2;
            nearZ = ComputeWalkableNearZ(diagonal);
        }
        else
        {
            posX = centerX;
            posY = eyeHeight;
            posZ = scene.BoundsMin[2] + dz * 0.2f;
            yaw = 0.0f;
            nearZ = ComputeWalkableNearZ(diagonal);
        }

        camera.SetPosition({ posX, posY, posZ });
        camera.SetYawPitch(yaw, 0.0f);
        camera.SetLens(DirectX::XM_PIDIV4, nearZ, farZ);
        return camera;
    }

    // カメラ視錐台をkCascadeCount個の深度範囲に分割する境界(View空間でのカメラからの距離)を求める。
    // 対数分割(遠くのカスケードほど急激に広がる。人間の目の距離知覚・遠近感に合う)と均等分割
    // (どのカスケードも同じ奥行きを持つ)を按分するPractical Split Scheme(GPU Gems 3, Dimitrov 2007)を使う。
    // 対数分割のみだと手前のカスケードが極端に狭くなり、均等分割のみだと遠方のテクセル密度が
    // 不足するため、両者を混ぜることで手前の精度と遠方のカバレッジを両立する
    void KurenaiEngine3D::ComputeCascadeSplits(const Core::Camera& camera, float (&outSplits)[kCascadeCount]) const
    {
        const float nearZ = camera.GetNearZ();
        // [Scene]ShadowDistanceが指定されていれば、そこでカスケードの分割範囲を打ち切る。
        //
        // 【なぜ必要か】遠クリップ面はシーンAABBの対角から自動で決まる(farZ = max(100, 対角×4))。
        // 数十km規模のシーンではfarZが100km級になり、分割範囲がそのまま伸びるため
        // 第1カスケードが数kmを2048x2048の1枚で覆うことになって近景の影が消える。
        // 【未指定なら従来どおり】書かなかったシーンの見え方は1ピクセルも変えない
        const float farZ = m_Scene.HasShadowDistance
            ? (std::min)(camera.GetFarZ(), m_Scene.ShadowDistance)
            : camera.GetFarZ();
        const float lambda = 0.75f;

        for (uint32_t i = 0; i < kCascadeCount; ++i)
        {
            const float p = static_cast<float>(i + 1) / static_cast<float>(kCascadeCount);
            const float logSplit = nearZ * std::pow(farZ / nearZ, p);
            const float uniformSplit = nearZ + (farZ - nearZ) * p;
            outSplits[i] = lambda * logSplit + (1.0f - lambda) * uniformSplit;
        }
    }

    // 平行光のライト視点から、カメラ視錐台のうち[splitNear, splitFar]の範囲(View空間距離)だけを
    // 覆う正射影のビュー・プロジェクション行列を求める(カスケードシャドウマップの1カスケード分)。
    // その深度範囲の視錐台スライスの8頂点を求め、外接球を基準にライト視点を配置する
    DirectX::XMMATRIX KurenaiEngine3D::ComputeCascadeLightViewProj(
        const DirectX::XMFLOAT3& lightDirection, const Core::Camera& camera, float splitNear, float splitFar) const
    {
        using namespace DirectX;

        const XMFLOAT3 positionF = camera.GetPosition();
        const XMFLOAT3 forwardF = camera.GetForward();
        const XMFLOAT3 rightF = camera.GetRight();
        const XMVECTOR position = XMLoadFloat3(&positionF);
        const XMVECTOR forward = XMLoadFloat3(&forwardF);
        const XMVECTOR right = XMLoadFloat3(&rightF);
        const XMVECTOR camUp = XMVector3Normalize(XMVector3Cross(right, forward));

        const float tanHalfFovY = std::tan(camera.GetFovY() * 0.5f);
        const float aspect = camera.GetAspectRatio();

        // splitNear/splitFarそれぞれの断面の4隅(ワールド座標)を求め、視錐台スライスの8頂点とする
        XMVECTOR corners[8];
        int cornerIndex = 0;
        for (const float dist : { splitNear, splitFar })
        {
            const float halfHeight = dist * tanHalfFovY;
            const float halfWidth = halfHeight * aspect;
            const XMVECTOR centerAtDist = XMVectorAdd(position, XMVectorScale(forward, dist));
            for (const float sy : { -1.0f, 1.0f })
            {
                for (const float sx : { -1.0f, 1.0f })
                {
                    corners[cornerIndex++] = XMVectorAdd(
                        centerAtDist,
                        XMVectorAdd(XMVectorScale(right, halfWidth * sx), XMVectorScale(camUp, halfHeight * sy)));
                }
            }
        }

        // 8頂点の外接球を使う(タイトなAABBだとカメラの向きによって毎フレーム形が変わり、
        // シャドウマップの見かけのサイズが揺れてちらつく。半径ベースにすることで回転に対して安定する)
        XMVECTOR centerSum = XMVectorZero();
        for (const XMVECTOR& corner : corners)
        {
            centerSum = XMVectorAdd(centerSum, corner);
        }
        const XMVECTOR sphereCenter = XMVectorScale(centerSum, 1.0f / 8.0f);

        float sphereRadius = 0.01f;
        for (const XMVECTOR& corner : corners)
        {
            sphereRadius = std::max(sphereRadius, XMVectorGetX(XMVector3Length(XMVectorSubtract(corner, sphereCenter))));
        }

        const XMVECTOR lightDirVec = XMVector3Normalize(XMLoadFloat3(&lightDirection));

        // ライト方向がほぼ真上/真下(upベクトルと平行)だとLookAt行列が縮退するため、そのときだけ別軸を使う
        XMVECTOR lightUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        if (std::abs(XMVectorGetY(lightDirVec)) > 0.99f)
        {
            lightUp = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
        }

        // シャドウマップのテクセル単位に中心位置をスナップし、カメラが動いた際にシャドウの縁が
        // 1テクセル未満の単位でちらつく(シャドウシマー)のを抑える。ライトの「向き」だけを持つ
        // (平行移動のない)基準行列でワールド座標をライト空間へ変換してからテクセル単位に丸め、
        // 再度ワールド空間へ戻す。フレームごとに視点位置から作り直す行列を直接使うと、常に
        // 中心が原点近辺の値になってしまい意味がないため、この向きだけの基準行列を使う
        const XMMATRIX lightRotation = XMMatrixLookAtLH(XMVectorZero(), lightDirVec, lightUp);
        const float orthoSize = sphereRadius * 2.0f;
        const float texelSize = orthoSize / static_cast<float>(kShadowMapSize);

        XMFLOAT3 centerLightSpace;
        XMStoreFloat3(&centerLightSpace, XMVector3TransformCoord(sphereCenter, lightRotation));
        centerLightSpace.x = std::floor(centerLightSpace.x / texelSize) * texelSize;
        centerLightSpace.y = std::floor(centerLightSpace.y / texelSize) * texelSize;

        const XMMATRIX lightRotationInv = XMMatrixInverse(nullptr, lightRotation);
        const XMVECTOR snappedCenter = XMVector3TransformCoord(XMLoadFloat3(&centerLightSpace), lightRotationInv);

        // ライトが進む方向と逆側に球の半径分だけ余裕を持って離れた位置に仮想的なライトカメラを置く
        const float margin = 1.5f;
        const XMVECTOR eye = XMVectorSubtract(snappedCenter, XMVectorScale(lightDirVec, sphereRadius * margin));
        const XMMATRIX lightView = XMMatrixLookAtLH(eye, snappedCenter, lightUp);

        const float nearZ = 0.1f;
        const float farZ = sphereRadius * margin * 2.0f + sphereRadius;
        const XMMATRIX lightProj = XMMatrixOrthographicLH(orthoSize, orthoSize, nearZ, farZ);

        return lightView * lightProj;
    }

    float KurenaiEngine3D::GetLastFrameGPUWaitTimeMs() const
    {
        return m_Device->GetLastFrameGPUWaitTimeMs();
    }

    float KurenaiEngine3D::GetMonitorDpiScale() const
    {
        return m_Window->GetDpiScale();
    }

    void KurenaiEngine3D::Run()
    {
        // シーン読み込み専用スレッドを起動する。ファイルI/O・デコード・アセット由来のGPUリソースの
        // 作成と破棄をこのスレッドが担い、読み込み中もRenderスレッドがフレームを進められるようにする
        m_LoaderThread = std::thread(&KurenaiEngine3D::LoaderThreadMain, this);

        // 描画専用スレッドを起動する。以後このスレッドがRender()の呼び出しとPresentを担当し、
        // 呼び出し元スレッド(以下Updateスレッド)はPumpMessages/Updateに専念する
        m_RenderThread = std::thread(&KurenaiEngine3D::RenderThreadMain, this);

        // 注意: ウィンドウのドラッグ中(移動・リサイズ)はWindowsが自前のモーダルループを回すため、
        // このループのPumpMessages()は戻ってこない。その間は1フレームも進まず画面が固まる
        // (ドラッグ中は描画不要という方針のためこのままにしている)。
        // その結果、モニタをまたいだときのUI拡大率の変化はマウスを離した時点でまとめて反映される。
        //
        // HasPendingGraphicsAPIChange()でも抜ける。この場合ウィンドウは閉じられておらず、
        // 呼び出し側がこのオブジェクトを破棄して別のAPIで作り直す(ヘッダのコメント参照)
        while (!m_Window->ShouldClose() && !HasPendingGraphicsAPIChange())
        {
            m_Window->PumpMessages();
            if (m_Window->ShouldClose())
            {
                break;
            }

            TickFrame();
        }

        {
            std::lock_guard<std::mutex> lock(m_FrameStateMutex);
            m_StopRenderThread = true;
        }
        m_FrameStateCV.notify_one();
        m_RenderThread.join();

        // Renderスレッドが止まった後にLoaderスレッドを止める。この順序により、Loaderの停止後に
        // 新しい破棄依頼が積まれることはない。Loaderは終了前に残った破棄依頼を片付けるため、
        // アセット用ディスクリプタヒープを触るのはこのスレッドだけ、という不変条件が保たれる
        {
            std::lock_guard<std::mutex> lock(m_LoadRequestMutex);
            m_StopLoaderThread = true;
        }
        m_LoadRequestCV.notify_one();
        m_LoaderThread.join();

        // Loaderが作り終えていたが取り込まれなかったシーンをここで解放する。
        // この時点で動いているのはこのスレッドだけなので、どのヒープを触っても競合しない
        {
            std::lock_guard<std::mutex> lock(m_LoadedSceneMutex);
            m_LoadedScene.reset();
        }
    }

    void KurenaiEngine3D::RequestGraphicsAPIChange(GraphicsAPI api)
    {
        if (api == m_GraphicsAPI)
        {
            return;
        }

        Core::Logger::Info(
            "KurenaiEngine3D",
            std::string("グラフィックスAPIの切り替えが要求されました: ") +
                (m_GraphicsAPI == GraphicsAPI::DX12 ? "DX12" : "DX11") + " -> " +
                (api == GraphicsAPI::DX12 ? "DX12" : "DX11"));

        m_RequestedGraphicsAPI.store(static_cast<int>(api), std::memory_order_relaxed);
    }

    bool KurenaiEngine3D::HasPendingGraphicsAPIChange() const
    {
        return m_RequestedGraphicsAPI.load(std::memory_order_relaxed) >= 0;
    }

    GraphicsAPI KurenaiEngine3D::GetPendingGraphicsAPI() const
    {
        const int requested = m_RequestedGraphicsAPI.load(std::memory_order_relaxed);
        // 要求が無いときは現在のAPIを返す(呼び出し側がHasPendingGraphicsAPIChangeを
        // 見ずに呼んでも、少なくとも同じAPIで作り直すだけで済むようにする)
        return requested < 0 ? m_GraphicsAPI : static_cast<GraphicsAPI>(requested);
    }

    void KurenaiEngine3D::SetExtraImGuiCallback(std::function<void()> callback)
    {
        // 【Run()の前に呼ぶこと】Renderスレッドが走り出した後にここを書き換えると、
        // 描画中のstd::functionを差し替えることになる。エディタはエンジンを構築した直後、
        // Run()を呼ぶ前に一度だけ登録する
        m_ExtraImGuiCallback = std::move(callback);
    }

    void KurenaiEngine3D::ApplyDroneShowData(const Assets::ShowData& data)
    {
        // 呼び出しスレッドの前提はヘッダー側のコメントを参照(SetExtraImGuiCallbackで
        // 登録したコールバックの中から呼ぶこと)。
        // 時刻は戻さない ―― プレビュー中に点をいじるたびにショーが先頭へ飛ぶと、
        // 「いま見ている瞬間の形」を直せなくなるため
        m_DroneShow.SetData(data);
    }

    void KurenaiEngine3D::TickFrame()
    {
        const auto now = std::chrono::steady_clock::now();
        const float deltaTime = std::chrono::duration<float>(now - m_LastFrameTime).count();
        m_LastFrameTime = now;

        Update(deltaTime);

        // m_CameraはUpdateスレッド(UpdateMouseLook/UpdateMovement/UpdateAppliedSceneHandoff)
        // のみが書き込み、Render()はframeStateのスナップショット経由でしか読まないため、
        // ここでの読み取りに追加のロックは不要
        FrameState newFrameState;
        newFrameState.Camera = m_Camera;
        newFrameState.ImGuiVisible = m_ImGuiVisible;

        // Renderスレッドが直前フレーム分を取り込み終えるまで待つ(キュー深度1)。
        // 取り込み自体はスナップショットのコピーだけなので即座に完了し、その後の重いGPU発行は
        // このUpdateスレッドの次フレーム処理と並行して進む
        {
            std::unique_lock<std::mutex> lock(m_FrameStateMutex);
            m_FrameStateCV.wait(lock, [this] { return m_FrameStateTaken; });
            m_FrameState = newFrameState;
            m_FrameStateReady = true;
            m_FrameStateTaken = false;
        }
        m_FrameStateCV.notify_one();
    }

    void KurenaiEngine3D::RenderThreadMain()
    {
        // LoadScene(RenderSceneSwitchUI経由でこのスレッドから呼ばれる)がWICテクスチャ読み込みで
        // COMを使用する。COMはスレッドごとに初期化が必要(wWinMainでのCoInitializeExはUpdate
        // スレッド=呼び出し元スレッドにしか適用されない)なため、この描画スレッドでも初期化しておく。
        // 未初期化のままだとWIC呼び出しがハングする(Main.cppと同じAPARTMENTTHREADEDに揃える)
        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

        m_LastRenderFrameTime = std::chrono::steady_clock::now();

        for (;;)
        {
            FrameState frameState;
            {
                std::unique_lock<std::mutex> lock(m_FrameStateMutex);
                m_FrameStateCV.wait(lock, [this] { return m_FrameStateReady || m_StopRenderThread; });
                if (m_StopRenderThread && !m_FrameStateReady)
                {
                    break;
                }
                frameState = m_FrameState;
                m_FrameStateReady = false;
                m_FrameStateTaken = true;
            }
            m_FrameStateCV.notify_one();

            const auto now = std::chrono::steady_clock::now();
            const float renderDeltaTime = std::chrono::duration<float>(now - m_LastRenderFrameTime).count();
            m_LastRenderFrameTime = now;
            // 自動露出の時間方向の順応で使う(次フレームのRender()が読む)
            m_RenderDeltaTime = renderDeltaTime;

            // 昼夜サイクルの自動進行はUpdateスレッドではなくこちら(Renderスレッド)で行う。
            // m_TimeOfDay/m_TimeAutoAdvance/m_TimeAdvanceSpeedはImGuiパネル(RenderLightingUI、
            // Renderスレッドから描画)でも書き換えられるため、両方をRenderスレッド専有にすることで
            // 追加の排他制御なしに済ませられる
            if (m_TimeAutoAdvance)
            {
                m_TimeOfDay = std::fmod(m_TimeOfDay + m_TimeAdvanceSpeed * renderDeltaTime, 24.0f);
                if (m_TimeOfDay < 0.0f)
                {
                    m_TimeOfDay += 24.0f;
                }
            }

            // 水面のスクロール位相。太陽の自動進行とまったく同じ場所・同じ理由
            // (m_TimeAutoAdvance/m_WaterTimeFrozenがRenderingパネル(Renderスレッドから描画)でも
            // 書き換えられるため、両方をRenderスレッド専有にすることで追加の排他制御なしに済ませる)
            if (!m_WaterTimeFrozen)
            {
                m_WaterScrollOffset = std::fmod(m_WaterScrollOffset + renderDeltaTime * m_WaterWaveSpeed, 1.0f);
            }

            // 雲のスクロール位相。水面とまったく同じ場所・同じ理由でRenderスレッド専有のまま進める。
            // 【風速の単位について】m_CloudWindSpeedは実世界の速度[m/s]として持つ(UIで直感的に
            // 扱えるようにするため)。Sky.hlsliのノイズ空間はワールド距離にCloudUvScaleを掛けた
            // ものなので、ノイズ空間上の移動量へ換算するにはここでCloudUvScaleを掛ける必要がある。
            // 【なぜベイクをdirtyにしないのか】風のスクロールはIBLキューブの明るさに一切影響しない
            // (判断A: キューブには雲を焼かない)。ここでm_SkyBakeDirtyを立てると、風が吹くたびに
            // 毎フレーム空生成6回+プリフィルタ36回のディスパッチが走ってしまい、判断Aの利点が
            // 丸ごと消える。被覆率のような「キューブの明るさに効く」パラメータだけがdirtyを立てる
            // (RenderingPanel::DrawCloudSection参照)
            if (!m_CloudTimeFrozen)
            {
                const float windRadians = DirectX::XMConvertToRadians(m_CloudWindDirectionDegrees);
                const float windDirX = std::cos(windRadians);
                const float windDirZ = std::sin(windRadians);
                const float advanceNoiseSpace = m_CloudWindSpeed * m_CloudUvScale * renderDeltaTime;
                // Sky.hlsliのkCloudNoisePeriodと同じ値でwrapする(このファイル冒頭近くの
                // kCloudNoisePeriod定数のコメント参照)
                m_CloudScrollOffset.x =
                    std::fmod(m_CloudScrollOffset.x + windDirX * advanceNoiseSpace, kCloudNoisePeriod);
                m_CloudScrollOffset.y =
                    std::fmod(m_CloudScrollOffset.y + windDirZ * advanceNoiseSpace, kCloudNoisePeriod);

                // 巻雲。積雲とまったく同じ形(kCloudNoisePeriodでstd::fmod)で進める。
                // 風向はm_CloudWindDirectionDegreesを積雲と共有し、速度・UVスケールだけ
                // 巻雲側の値(m_CirrusWindSpeed/m_CirrusUvScale)を使う。凍結トグル
                // (m_CloudTimeFrozen)も積雲と共有する——片方にしか効かないとA/B比較で
                // スクロールが揺れる側だけ残ってしまい対照が取れなくなるため
                const float cirrusAdvanceNoiseSpace = m_CirrusWindSpeed * m_CirrusUvScale * renderDeltaTime;
                m_CirrusScrollOffset.x =
                    std::fmod(m_CirrusScrollOffset.x + windDirX * cirrusAdvanceNoiseSpace, kCloudNoisePeriod);
                m_CirrusScrollOffset.y =
                    std::fmod(m_CirrusScrollOffset.y + windDirZ * cirrusAdvanceNoiseSpace, kCloudNoisePeriod);
            }

            // ドローンショーの進行時刻。水面・雲のスクロール位相とまったく同じ場所・同じ理由で
            // Renderスレッド専有のまま進める。
            //
            // 【1巡ぶんで必ず折り返すこと】DroneShow::Evaluate自身も1巡の周期でstd::fmodするので
            // 絵の上は折り返さなくても正しく出る。折り返しが要るのは**floatの精度**のためである。
            // 仮数は24bitなので、1日(86,400秒)積むとULPが約0.010秒になり、60fpsのdt(0.0167秒)が
            // まともに積めなくなってショーが止まる。以前はUIの「ショー時刻」スライダーで
            // 手動で戻せることを逃げ道にしていたが、そのUIごと無くなったのでここで閉じる
            m_DroneShowTime += renderDeltaTime * m_DroneShow.Data().Speed;
            const float showLoopDuration = m_DroneShow.LoopDuration();
            if (showLoopDuration > 0.0f)
            {
                // 未初期化(LoopDuration()==0)のときに割るとNaNになるのでガードする
                m_DroneShowTime = std::fmod(m_DroneShowTime, showLoopDuration);
            }

            // m_Scene・ポストプロセスのパラメータ・UIの状態はすべてこのRenderスレッド専有に
            // なっているため、ミューテックスによる保護は要らない
            // (経緯はdocs/ImplementationHistory.md 23章)
            const auto cpuStart = std::chrono::steady_clock::now();
            Render(frameState);
            const auto cpuEnd = std::chrono::steady_clock::now();
            // GPUの完了待ち(DX12のフレームパイプライン化に伴うフェンス待ち)は実際のCPU負荷ではなく
            // GPU側の処理時間の反映なので差し引く(DX11は常に0が返るため影響しない)
            const float rawCPUTimeMs = std::chrono::duration<float, std::milli>(cpuEnd - cpuStart).count();
            m_CPUFrameTimeMs = std::max(0.0f, rawCPUTimeMs - m_Device->GetLastFrameGPUWaitTimeMs());

            // 固定FPSモード: このフレームの処理(Time of Day更新+Render+Present)が目標フレーム時間
            // より短く終わった場合、余った時間だけ待機して間隔を揃える。CPU/GPU計測(上記)の後に
            // 行うことで、この待機時間自体がプロファイラの計測値に混ざらないようにしている
            if (m_FixedFPSEnabled && m_TargetFPS > 0.0f)
            {
                const auto targetFrameDuration = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(1.0 / m_TargetFPS));
                const auto frameDeadline = now + targetFrameDuration;
                if (std::chrono::steady_clock::now() < frameDeadline)
                {
                    std::this_thread::sleep_until(frameDeadline);
                }
            }

            // FPSは指数移動平均で平滑化する(生の1/deltaTimeだとフレームごとの揺れが大きく読み取りにくいため)
            if (renderDeltaTime > 0.0f)
            {
                const float instantFPS = 1.0f / renderDeltaTime;
                m_FPS = (m_FPS == 0.0f) ? instantFPS : (m_FPS * 0.9f + instantFPS * 0.1f);
            }

            LogFrameStatsIfDue(renderDeltaTime);
        }

        if (SUCCEEDED(comResult))
        {
            CoUninitialize();
        }
    }

    // 性能の記録をログファイルへ残す。ProfilerPanelの表示は実行中しか見えず、後から
    // 「この変更でフレーム時間がどう変わったか」を比較できない。集計期間ぶんを1行に
    // まとめて出すことで、フレーム時間への影響(Logger::Infoはflushを伴う)を
    // 1秒に1回に抑えつつ、実行ごとの記録が残るようにしている
    void KurenaiEngine3D::LogFrameStatsIfDue(float renderDeltaTime)
    {
        if (!m_FrameStatsLoggingEnabled)
        {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if (m_FrameStatsFrameCount == 0)
        {
            m_FrameStatsWindowStart = now;
        }

        ++m_FrameStatsFrameCount;
        m_FrameStatsCPUTimeSumMs += m_CPUFrameTimeMs;
        m_FrameStatsGPUTimeSumMs += m_GPUProfiler ? m_GPUProfiler->GetTotalFrameTimeMs() : 0.0f;
        m_FrameStatsGPUWaitSumMs += m_Device->GetLastFrameGPUWaitTimeMs();
        m_FrameStatsWorstFrameTimeMs = std::max(m_FrameStatsWorstFrameTimeMs, renderDeltaTime * 1000.0f);
        m_FrameStatsCullTestedSum += m_FrustumCullTested;
        m_FrameStatsCullCulledSum += m_FrustumCullCulled;
        m_FrameStatsLODSwitchSum += m_LODSwitchCount;
        m_FrameStatsLODFadingSum += m_LODFadingCount;
        m_FrameStatsMeshCullTestedSum += m_MeshCullTested;
        m_FrameStatsMeshCullCulledSum += m_MeshCullCulled;
        m_FrameStatsDrawCallsGBufferSum += m_DrawCallsGBuffer;
        m_FrameStatsDrawCallsShadowSum += m_DrawCallsShadow;
        m_FrameStatsDrawCallsDepthPrepassSum += m_DrawCallsDepthPrepass;
        m_FrameStatsInstancedBatchSum += m_InstancedBatchCount;
        m_FrameStatsInstancedInstanceSum += m_InstancedInstanceCount;

        const float elapsedSeconds = std::chrono::duration<float>(now - m_FrameStatsWindowStart).count();
        if (elapsedSeconds < Defaults::FrameStatsLogIntervalSeconds)
        {
            return;
        }

        // 集計期間の実測フレーム数から求める。m_FPS(指数移動平均)と違い、この値は
        // 期間中に落ちたフレームがそのまま反映される
        const float averageFPS = static_cast<float>(m_FrameStatsFrameCount) / std::max(elapsedSeconds, 1e-6f);
        const double frameCount = static_cast<double>(m_FrameStatsFrameCount);

        char buffer[256];
        std::snprintf(
            buffer,
            sizeof(buffer),
            "%ux%u %s | FPS %.1f (%u frames / %.2fs) | CPU %.2fms | GPU %.2fms | GPU待ち %.2fms | 最悪フレーム %.2fms",
            m_RenderWidth,
            m_RenderHeight,
            m_GraphicsAPI == GraphicsAPI::DX12 ? "DX12" : "DX11",
            averageFPS,
            m_FrameStatsFrameCount,
            elapsedSeconds,
            m_FrameStatsCPUTimeSumMs / frameCount,
            m_FrameStatsGPUTimeSumMs / frameCount,
            m_FrameStatsGPUWaitSumMs / frameCount,
            m_FrameStatsWorstFrameTimeMs);
        Core::Logger::Info("Perf", buffer);

        // パス別の内訳。どのパスを削れば効くのかは合計値からは分からないため、
        // 集計期間の最後のフレームぶんを重い順に並べて残す。
        // (毎フレーム平均を取るにはパス構成がフレームごとに変わりうるので、
        //  代表として1フレームぶんを出す。ベイクパスが走ったフレームに当たると
        //  その分だけ大きく出るが、常時走るパスの比較には十分)
        if (m_GPUProfiler)
        {
            std::vector<RHI::GPUTimingResult> passes = m_GPUProfiler->GetResults();
            std::sort(passes.begin(), passes.end(), [](const auto& a, const auto& b) { return a.TimeMs > b.TimeMs; });

            std::string breakdown;
            for (const auto& pass : passes)
            {
                // 0.05ms未満は並べても判断材料にならず、行が長くなるだけなので落とす
                if (pass.TimeMs < 0.05f)
                {
                    break;
                }
                char passText[64];
                std::snprintf(passText, sizeof(passText), "%s %.2f", pass.Name.c_str(), pass.TimeMs);
                if (!breakdown.empty())
                {
                    breakdown += " / ";
                }
                breakdown += passText;
            }

            if (!breakdown.empty())
            {
                Core::Logger::Info("Perf", "  GPU内訳[ms]: " + breakdown);
            }
        }

        // CPU側の内訳も同じ形で残す。GIVolumeを持つシーンではCPUフレーム時間が24〜28msあり、
        // 60fpsの予算(16.7ms)をCPU単独で超えている。GPUの内訳だけでは、その時間が
        // どのパスのドローコール発行に消えているのかが分からない
        {
            std::vector<Core::CPUTimingResult> cpuPasses = m_CPUProfiler.GetResults();
            std::sort(
                cpuPasses.begin(), cpuPasses.end(), [](const auto& a, const auto& b) { return a.TimeMs > b.TimeMs; });

            std::string breakdown;
            for (const auto& pass : cpuPasses)
            {
                // GPU側と同じ理由で0.05ms未満は落とす
                if (pass.TimeMs < 0.05f)
                {
                    break;
                }
                char passText[64];
                std::snprintf(passText, sizeof(passText), "%s %.2f", pass.Name.c_str(), pass.TimeMs);
                if (!breakdown.empty())
                {
                    breakdown += " / ";
                }
                breakdown += passText;
            }

            if (!breakdown.empty())
            {
                Core::Logger::Info("Perf", "  CPU内訳[ms]: " + breakdown);
            }
        }

        // フラスタムカリングの効き。「間引いた数が0」は、判定式が常に通しているのか
        // 本当に全部が視界内なのかを区別できないため、テストした数と併せて出す。
        //
        // 【モデル単位とメッシュ単位を別の行にする】分母も、効くシーンも違う。
        // モデル単位は.kmodelを多数並べるシーンで効き、1モデルに数千メッシュを持つ
        // アセットでは1つも間引けない。メッシュ単位はその逆。合算すると、どちらが効いたのか
        // ―― あるいは片方が一度も実行されていないのか ―― が読めなくなる
        const auto logCullStats = [this](const char* label, uint64_t testedSum, uint64_t culledSum)
        {
            if (testedSum == 0 || m_FrameStatsFrameCount == 0)
            {
                // 判定が1回も走っていない。「間引き0」と区別が付くよう、行そのものを出さない
                return;
            }
            const double testedPerFrame = static_cast<double>(testedSum) / m_FrameStatsFrameCount;
            const double culledPerFrame = static_cast<double>(culledSum) / m_FrameStatsFrameCount;
            const double ratio = 100.0 * static_cast<double>(culledSum) / static_cast<double>(testedSum);

            char cullText[224];
            std::snprintf(
                cullText, sizeof(cullText), "  %s: 判定 %.1f / 間引き %.1f (%.1f%%) [1フレームあたり・全パス合計]",
                label, testedPerFrame, culledPerFrame, ratio);
            Core::Logger::Info("Perf", cullText);
        };
        logCullStats("フラスタムカリング(モデル単位)", m_FrameStatsCullTestedSum, m_FrameStatsCullCulledSum);
        logCullStats("フラスタムカリング(メッシュ単位)", m_FrameStatsMeshCullTestedSum, m_FrameStatsMeshCullCulledSum);

        // モデルLOD。【切り替え0回なら一度も効いていない】距離のしきい値が実際の
        // カメラの動く範囲から外れているか、そもそもLODPathが指定されていない
        {
            char lodText[192];
            std::snprintf(
                lodText, sizeof(lodText),
                "  モデルLOD: 切り替え %llu回 / フェード %llu インスタンス×フレーム [いずれも集計期間の合計]",
                static_cast<unsigned long long>(m_FrameStatsLODSwitchSum),
                static_cast<unsigned long long>(m_FrameStatsLODFadingSum));
            Core::Logger::Info("Perf", lodText);
        }

        // モデルのストリーミング。【常駐0や読み込み0なら効いていない】
        // 範囲内なのに常駐していないものが残り続けるなら、発注か受け取りのどこかで詰まっている
        if (m_Scene.HasStreamingDistance)
        {
            char streamText[192];
            std::snprintf(
                streamText, sizeof(streamText),
                "  ストリーミング: 常駐 %u / 範囲内 %u (距離 %.0fm) / 読み込み累計 %llu件 / 破棄累計 %llu件"
                " / RT再構築 %llu回(直近 %.1fms)",
                m_StreamingResidentCount, m_StreamingTargetCount, m_Scene.StreamingDistance,
                static_cast<unsigned long long>(m_StreamingLoadedTotal),
                static_cast<unsigned long long>(m_StreamingEvictedTotal),
                static_cast<unsigned long long>(m_RaytracingRebuildCount), m_RaytracingRebuildLastMs);
            Core::Logger::Info("Perf", streamText);
        }

        // パス別のドローコール数。**「G-Bufferは減ったがシャドウは減っていない」**のような
        // 片手落ちは合計値では見えない(シャドウはカスケード4回ぶんが積み上がる)
        if (m_FrameStatsFrameCount > 0)
        {
            const double frames = static_cast<double>(m_FrameStatsFrameCount);
            char drawText[224];
            std::snprintf(
                drawText, sizeof(drawText),
                "  ドローコール: G-Buffer %.1f / シャドウ %.1f (4カスケード計) / 深度プリパス %.1f "
                "[1フレームあたり]",
                static_cast<double>(m_FrameStatsDrawCallsGBufferSum) / frames,
                static_cast<double>(m_FrameStatsDrawCallsShadowSum) / frames,
                static_cast<double>(m_FrameStatsDrawCallsDepthPrepassSum) / frames);
            Core::Logger::Info("Perf", drawText);
        }

        // インスタンシングの効き。**ドローコール数とは別建てにする** ――
        // 「バッチ0」は「まとめられる相手がいない」のか「一度も実行されていない」のかを
        // 区別できないので、まとめた数(バッチ)とまとめた対象(インスタンス)の両方を出す。
        // まとめたことで減ったドロー数は (インスタンス数 - バッチ数) x そのモデルのメッシュ数
        if (m_FrameStatsFrameCount > 0 && m_FrameStatsInstancedBatchSum > 0)
        {
            const double frames = static_cast<double>(m_FrameStatsFrameCount);
            char instText[192];
            std::snprintf(
                instText, sizeof(instText),
                "  インスタンシング: バッチ %.1f / まとめたインスタンス %.1f [1フレームあたり・2組の合計]",
                static_cast<double>(m_FrameStatsInstancedBatchSum) / frames,
                static_cast<double>(m_FrameStatsInstancedInstanceSum) / frames);
            Core::Logger::Info("Perf", instText);
        }

        // bindless区画の使用状況。**満杯になっても例外は飛ばず、エラーログ1行と
        // kInvalidBindlessIndex(=白1x1へ落ちる)しか残らない**ため、上限へ近づいていることを
        // 定期的に見えるようにしておく(IRHIDevice::GetBindlessUsedCountのコメント参照)
        if (m_Device)
        {
            const uint32_t bindlessCapacity = m_Device->GetBindlessCapacity();
            if (bindlessCapacity > 0)
            {
                const uint32_t bindlessUsed = m_Device->GetBindlessUsedCount();
                char bindlessText[160];
                std::snprintf(
                    bindlessText, sizeof(bindlessText), "  bindless: %u / %u ディスクリプタ (%.1f%%)",
                    bindlessUsed, bindlessCapacity,
                    100.0 * static_cast<double>(bindlessUsed) / static_cast<double>(bindlessCapacity));
                Core::Logger::Info("Perf", bindlessText);
            }
        }

        // メッシュレット単位のカリング(増幅シェーダー)の効き。上のCPU側とは粒度も判定の種類も
        // 違うので別の行に出す。
        //
        // 【オクルージョンを視錐台+コーンと分けて出す】完了条件がここにある ――
        // 俯瞰(遮蔽が少ない)と街路(遮蔽が多い)でオクルージョンの割合に差が出ることが、
        // 判定が実際に効いていることの証拠になる。合算すると視錐台の変動に埋もれて分からない
        if (m_FrameStatsMeshletSampleCount > 0 && m_FrameStatsMeshletTestedSum > 0)
        {
            const double samples = static_cast<double>(m_FrameStatsMeshletSampleCount);
            const double tested = static_cast<double>(m_FrameStatsMeshletTestedSum);
            const double frustumRatio = 100.0 * static_cast<double>(m_FrameStatsMeshletFrustumCulledSum) / tested;
            const double occlusionRatio = 100.0 * static_cast<double>(m_FrameStatsMeshletOcclusionCulledSum) / tested;

            char meshletCullText[256];
            std::snprintf(
                meshletCullText, sizeof(meshletCullText),
                "  メッシュレットカリング: 判定 %.1f / 視錐台+コーン %.1f (%.1f%%) / オクルージョン %.1f (%.1f%%)"
                " [1フレームあたり・%u フレーム分]",
                tested / samples,
                static_cast<double>(m_FrameStatsMeshletFrustumCulledSum) / samples, frustumRatio,
                static_cast<double>(m_FrameStatsMeshletOcclusionCulledSum) / samples, occlusionRatio,
                m_FrameStatsMeshletSampleCount);
            Core::Logger::Info("Perf", meshletCullText);
        }

        // モデル単位のGPUカリング(Stage 5-3)。
        //
        // 【判定数と視錐台の間引き数がCPUと一致することが合格条件】GPUは同じAABBを
        // 同じ視錐台で判定しているので、一致しなければ平面の作り方か候補の積み方が壊れている。
        // **間接描画はこの数を信じて描く**ので、食い違ったまま進むと絵が消えてから
        // 原因を探すことになる。だから食い違いは警告として残す
        if (m_ModelCullTested > 0)
        {
            char modelCullText[256];
            std::snprintf(
                modelCullText, sizeof(modelCullText),
                "  モデル単位GPUカリング: 判定 %u (CPU候補 %u) / 視錐台 %u (CPU %u) / オクルージョン %u / 生存 %u",
                m_ModelCullTested, m_ModelCullComparedCandidateCount,
                m_ModelCullFrustumCulled, m_ModelCullComparedCpuFrustumCulled,
                m_ModelCullOcclusionCulled, m_ModelCullSurvived);
            Core::Logger::Info("Perf", modelCullText);

            // 区画ごとの発行数。**間引きの数だけ見ても、間接描画が本当に描いているかは分からない** ――
            // 描画発行に繋がっていなければここは全部0のままで、絵はCPUループが出している
            char modelCullRegionText[256];
            std::snprintf(
                modelCullRegionText, sizeof(modelCullRegionText),
                "  モデル単位GPU発行(%s): G-Buffer %u+%u / プリパス不透明 %u+%u / プリパスカットアウト %u+%u",
                m_ModelCullIndirectActiveLastFrame ? "間接描画" : "計数のみ",
                m_ModelCullRegionIssued[kModelCullRegionGBuffer],
                m_ModelCullRegionIssued[kModelCullRegionGBufferMirrored],
                m_ModelCullRegionIssued[kModelCullRegionPrepassOpaque],
                m_ModelCullRegionIssued[kModelCullRegionPrepassOpaqueMirrored],
                m_ModelCullRegionIssued[kModelCullRegionPrepassCutout],
                m_ModelCullRegionIssued[kModelCullRegionPrepassCutoutMirrored]);
            Core::Logger::Info("Perf", modelCullRegionText);

            // どの経路で判定したか。**間引き数だけでは切り替わったか分からない** ――
            // カメラが止まっていれば前フレームのHi-Zと今フレームのHi-Zは同じ内容になり、
            // 新旧どちらの経路でも同じ数が出る。経路そのものを出しておく
            char modelCullPathText[192];
            std::snprintf(
                modelCullPathText, sizeof(modelCullPathText),
                "  Hi-Zの出どころ: %s / 判定ディスパッチ: プリパスぶん %u + G-Bufferぶん %u",
                m_HiZFromDepthPrepassLastFrame ? "深度プリパス(今フレーム)" : "G-Bufferの後(前フレーム)",
                m_ModelCullDispatchCounts[0], m_ModelCullDispatchCounts[1]);
            Core::Logger::Info("Perf", modelCullPathText);

            if (m_ModelCullTested != m_ModelCullComparedCandidateCount ||
                m_ModelCullFrustumCulled != m_ModelCullComparedCpuFrustumCulled)
            {
                // 【黙って進めない】食い違ったままExecuteIndirectへ繋ぐと、
                // 絵が消えてから原因を探すことになる
                Core::Logger::Warning(
                    "Perf",
                    "モデル単位GPUカリングの判定がCPUと食い違っています(判定 " +
                        std::to_string(m_ModelCullTested) + " vs " +
                        std::to_string(m_ModelCullComparedCandidateCount) + " / 視錐台 " +
                        std::to_string(m_ModelCullFrustumCulled) + " vs " +
                        std::to_string(m_ModelCullComparedCpuFrustumCulled) + ")");
            }
        }

        m_FrameStatsFrameCount = 0;
        m_FrameStatsCPUTimeSumMs = 0.0;
        m_FrameStatsGPUTimeSumMs = 0.0;
        m_FrameStatsGPUWaitSumMs = 0.0;
        m_FrameStatsWorstFrameTimeMs = 0.0f;
        m_FrameStatsCullTestedSum = 0;
        m_FrameStatsCullCulledSum = 0;
        m_FrameStatsLODSwitchSum = 0;
        m_FrameStatsLODFadingSum = 0;
        m_FrameStatsMeshCullTestedSum = 0;
        m_FrameStatsMeshCullCulledSum = 0;
        m_FrameStatsDrawCallsGBufferSum = 0;
        m_FrameStatsDrawCallsShadowSum = 0;
        m_FrameStatsDrawCallsDepthPrepassSum = 0;
        m_FrameStatsInstancedBatchSum = 0;
        m_FrameStatsInstancedInstanceSum = 0;
        m_FrameStatsMeshletTestedSum = 0;
        m_FrameStatsMeshletFrustumCulledSum = 0;
        m_FrameStatsMeshletOcclusionCulledSum = 0;
        m_FrameStatsMeshletSampleCount = 0;

        // テクスチャの常駐ミップの内訳。**サイズ帯ごとに分けて出す** ――
        // 64KBタイルはBC7で256x256テクセルを覆うため、ミップ/タイル単位の制御が効くのは
        // 大きいテクスチャに偏る。「入れたから減った」ではなくどの帯に効いたかで語るため
        if (m_TextureStreaming.IsEnabled())
        {
            m_TextureStreaming.LogStats("periodic");
        }

        // 【自己申告と実測を並べる】常駐管理が積算したバイト数だけを見ていると、
        // 物差し自体が間違っていても気付けない。OSから見たVRAM使用量と一緒に出す
        uint64_t usedBytes = 0;
        uint64_t budgetBytes = 0;
        if (m_Device->GetVideoMemoryUsage(usedBytes, budgetBytes))
        {
            constexpr double kBytesPerMiB = 1024.0 * 1024.0;
            char vramLine[160];
            std::snprintf(
                vramLine, sizeof(vramLine), "VRAM: 使用 %.1f MB / 予算 %.1f MB",
                static_cast<double>(usedBytes) / kBytesPerMiB, static_cast<double>(budgetBytes) / kBytesPerMiB);
            Core::Logger::Info("Perf", vramLine);
        }
    }

    void KurenaiEngine3D::UpdateMouseLook(bool imguiWantsMouse)
    {
        // このメソッドだけは意図的にGetAsyncKeyState/GetCursorPos/SetCursorPosを使い続けている。
        // カーソルを画面中央へ強制的に固定し続ける(SetCursorPos)ことで無限ドラッグを実現しており、
        // これは実カーソルを動かす・隠す操作そのものであるため、メッセージベース化(PostMessageで
        // WM_RBUTTONDOWN/WM_MOUSEMOVEを送るだけで発火する形)にしてしまうと、動作確認用の
        // PostMessage送信が実デスクトップのカーソルを意図せず動かし・隠してしまう経路になる。
        // GetAsyncKeyState(VK_RBUTTON)はPostMessageでは変化しない実ハードウェアの状態のため、
        // このままにしておくことでPostMessageによる動作確認が誤ってカーソル操作を引き起こさない
        // (=実カーソル・他ウィンドウに影響を与えない)ことを構造的に保証している
        //
        // GetAsyncKeyStateはウィンドウフォーカスに関係なくグローバルなキー状態を返すため、
        // フォアグラウンドウィンドウチェックがないとデスクトップ上の右クリックでも
        // カーソルがウィンドウ中央へ強制移動してしまう
        const bool isForeground = GetForegroundWindow() == m_Window->GetHandle();
        if (isForeground && (GetAsyncKeyState(VK_RBUTTON) & 0x8000))
        {
            if (!m_MouseCaptured)
            {
                // ImGuiパネルの上で右ボタンを押し始めた場合は視点回転を開始しない
                // (ウィジェットの右クリックメニューと衝突するため)。
                // 一度キャプチャに入った後はカーソルが画面中央へ固定され続けてImGui側の判定が
                // 変わりうるため、この判定は開始時にだけ行う
                if (imguiWantsMouse)
                {
                    return;
                }

                m_MouseCaptured = true;
                ShowCursor(FALSE);

                RECT clientRect;
                GetClientRect(m_Window->GetHandle(), &clientRect);
                POINT center{ (clientRect.right - clientRect.left) / 2, (clientRect.bottom - clientRect.top) / 2 };
                ClientToScreen(m_Window->GetHandle(), &center);
                m_MouseCaptureCenter = center;
                SetCursorPos(center.x, center.y);
            }
            else
            {
                POINT currentPos;
                GetCursorPos(&currentPos);
                const float deltaX = static_cast<float>(currentPos.x - m_MouseCaptureCenter.x);
                const float deltaY = static_cast<float>(currentPos.y - m_MouseCaptureCenter.y);

                const float mouseSensitivity = 0.0025f;
                m_Camera.Rotate(deltaX * mouseSensitivity, -deltaY * mouseSensitivity);

                SetCursorPos(m_MouseCaptureCenter.x, m_MouseCaptureCenter.y);
            }
        }
        else if (m_MouseCaptured)
        {
            m_MouseCaptured = false;
            ShowCursor(TRUE);
        }
    }

    void KurenaiEngine3D::UpdateMovement(float deltaTime)
    {
        // メッセージベースの入力API(IsKeyDown)を使う。GetAsyncKeyStateと異なりウィンドウが
        // フォーカスを失っている間は反応せず、PostMessageによるテスト自動化とも整合する
        //
        // 【速度は即値ではなくシーンから決まる】m_CameraSpeedは.ksceneの[Scene]CameraSpeed、
        // 無ければシーン対角から自動で決まる(ResetSceneDependentParams)。Shiftの倍率は
        // 従来の 20/5 = 4倍をそのまま保つ
        const float moveSpeed =
            m_CameraSpeed * (IsKeyDown(VK_SHIFT) ? Defaults::CameraSpeedShiftMultiplier : 1.0f);
        const float moveAmount = moveSpeed * deltaTime;

        const DirectX::XMFLOAT3 forward = m_Camera.GetForward();
        const DirectX::XMFLOAT3 right = m_Camera.GetRight();

        DirectX::XMFLOAT3 move{ 0.0f, 0.0f, 0.0f };
        auto add = [&move](const DirectX::XMFLOAT3& v, float sign)
        {
            move.x += v.x * sign;
            move.y += v.y * sign;
            move.z += v.z * sign;
        };

        if (IsKeyDown('W')) add(forward, 1.0f);
        if (IsKeyDown('S')) add(forward, -1.0f);
        if (IsKeyDown('D')) add(right, 1.0f);
        if (IsKeyDown('A')) add(right, -1.0f);
        if (IsKeyDown('E')) move.y += 1.0f;
        if (IsKeyDown('Q')) move.y -= 1.0f;

        DirectX::XMVECTOR moveVec = DirectX::XMLoadFloat3(&move);
        if (DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(moveVec)) > 0.0001f)
        {
            moveVec = DirectX::XMVectorScale(DirectX::XMVector3Normalize(moveVec), moveAmount);
            DirectX::XMFLOAT3 delta;
            DirectX::XMStoreFloat3(&delta, moveVec);
            m_Camera.Move(delta);
        }
    }

    void KurenaiEngine3D::UpdateImGuiToggle()
    {
        // WasKeyPressedはウィンドウメッセージ由来のエッジ検出を内蔵しているため、
        // 前フレームの押下状態を自前で保持する必要がない
        if (WasKeyPressed(VK_F1))
        {
            m_ImGuiVisible = !m_ImGuiVisible;
        }
    }

    void KurenaiEngine3D::UpdateAppliedSceneHandoff()
    {
        // ロックを取る前にatomicで有無を判定する(publishされるのはシーン切り替え時だけなので、
        // ほとんどのフレームはここで抜ける)
        if (!m_AppliedScenePending.load(std::memory_order_acquire))
        {
            return;
        }

        Core::Camera camera;
        std::wstring title;
        bool applyCamera = true;
        {
            std::lock_guard<std::mutex> lock(m_AppliedSceneMutex);
            camera = m_AppliedSceneCamera;
            title = m_AppliedSceneTitle;
            applyCamera = m_AppliedSceneApplyCamera;
        }
        m_AppliedScenePending.store(false, std::memory_order_relaxed);

        // m_Cameraの書き込み手はこのUpdateスレッド1つに保つ(Renderスレッドは触らない)。
        // ホットリロードで「現在のカメラを保持する」が入っているときはここを飛ばす。
        // このとき位置・向きだけでなくnear/far(ComputeInitialCameraがシーンのAABBから決める)も
        // 前のまま残る。同じ.ksceneを読み直す用途では[Model]が変わらない限りAABBも変わらないので
        // 実害は無いが、モデルを差し替えたときはこのトグルを外して読み直すこと
        if (applyCamera)
        {
            m_Camera = camera;
        }
        // ウィンドウタイトルの変更もウィンドウを所有するこのスレッドから行う
        m_Window->SetTitle(title);
    }

    void KurenaiEngine3D::Update(float deltaTime)
    {
        // ImGui(Renderスレッド)が入力を掴んでいるかを読む。Renderは1フレーム遅れて描くため
        // この値も1フレーム遅れるが、WantCaptureKeyboardはInputTextがアクティブな間ずっと
        // trueであり続けるため、実用上ずれるのは押し始めの1フレームだけ
        const bool imguiWantsKeyboard = m_ImGuiWantCaptureKeyboard.load(std::memory_order_relaxed);
        const bool imguiWantsMouse = m_ImGuiWantCaptureMouse.load(std::memory_order_relaxed);

        // 内部レンダー解像度が変わったときのアスペクト比の反映。m_CameraはこのUpdateスレッドしか
        // 書けないため、解像度を変えるRenderスレッドはm_RenderAspectへ置くだけにしてある
        // (m_RenderAspectの宣言のコメント参照)。同じ値なら再設定しても副作用は無いので毎フレーム呼ぶ
        m_Camera.SetAspectRatio(m_RenderAspect.load(std::memory_order_relaxed));

        UpdateMouseLook(imguiWantsMouse);

        // ライト名のInputTextを編集中にWASDがカメラ移動として解釈されるのを防ぐ
        if (!imguiWantsKeyboard)
        {
            UpdateMovement(deltaTime);
        }


        // F1(ImGuiパネルの表示/非表示)はWantCaptureKeyboardに関係なく常に効かせる。
        // ここも抑止すると、テキスト入力中にパネルを畳んで戻す手段が無くなり、入力欄から
        // フォーカスを外す方法(Esc / 別の場所をクリック)を知らないと詰むため。
        // ImGuiのInputTextはF1を消費しないので、通しても入力内容には影響しない
        UpdateImGuiToggle();
        // 新しいシーンが反映されていれば、その初期カメラとウィンドウタイトルをここで取り込む
        UpdateAppliedSceneHandoff();
        // 昼夜サイクルの自動進行(m_TimeOfDay)はRenderThreadMain側で行う(RenderThreadMain参照)
    }

    void KurenaiEngine3D::Render(const FrameState& frameState)
    {
        // フラスタムカリングの統計はフレーム単位。ここで0に戻し、各描画パスが積み上げる。
        // モデル単位とメッシュ単位は別のカウンタで、混ぜない(KurenaiEngine3D.h参照)。
        //
        // 【0に戻す前に前フレームの値を控える】ドローコール数と同じ理由で、UIパネルは
        // Renderの外で描かれるため現在のカウンタを読むと必ずリセット直後の0になる
        m_FrustumCullTestedLastFrame = m_FrustumCullTested;
        m_FrustumCullCulledLastFrame = m_FrustumCullCulled;
        m_MeshCullTestedLastFrame = m_MeshCullTested;
        m_MeshCullCulledLastFrame = m_MeshCullCulled;
        m_FrustumCullTested = 0;
        m_FrustumCullCulled = 0;
        m_MeshCullTested = 0;
        m_MeshCullCulled = 0;
        // ドローコール数も同じくフレーム単位。各パスが自分のカウンタを積み上げる。
        //
        // 【0に戻す前に前フレームの値を控える】UIパネルはRenderの外で描かれるため、
        // 現在のカウンタを読むと必ずリセット直後の0になる(実際にそう表示されていた)。
        // 完成した最後のフレームの値を別に持たせる
        m_DrawCallsGBufferLastFrame = m_DrawCallsGBuffer;
        m_DrawCallsShadowLastFrame = m_DrawCallsShadow;
        m_DrawCallsDepthPrepassLastFrame = m_DrawCallsDepthPrepass;
        m_DrawCallsGBuffer = 0;
        m_DrawCallsShadow = 0;
        m_DrawCallsDepthPrepass = 0;
        // bindless区画の使用数を控える(UIパネルは m_Device へ直接触れないため。
        // m_MeshShaderAvailable と同じ扱い)。登録はシーン読み込み時にしか起きないので、
        // フレームごとに1回問い合わせるだけで足りる
        m_BindlessUsedCount = m_Device ? m_Device->GetBindlessUsedCount() : 0;

        // WM_SIZE(Updateスレッド)が記録しておいたリサイズ要求を、スワップチェーンを実際に使う
        // このスレッドで反映する。このフレームのGPUコマンドをまだ1つも積んでいないこの位置で
        // 呼ぶこと(DX12SwapChain::Resizeは内部でWaitForGPUIdleを呼び、コマンドリストが
        // 記録待ちの状態であることを前提としているため)
        ApplyPendingResize();

        // Loaderスレッドが出来上がったシーンを置いていれば取り込み、保留中の切り替え要求があれば発注する。
        // 旧シーンの破棄(WaitForGPUIdleを伴う)もここで行うため、このフレームのGPUコマンドを
        // まだ1つも積んでいないこの位置で呼ぶこと
        UpdateSceneStreaming();

        // テクスチャの常駐ミップ差し替えを確定する。
        // **このフレームで最初にSetTextureを呼ぶより前でなければならない** ――
        // DX12CommandList::SetTextureは描画を記録するたびにテクスチャのSRVディスクリプタを
        // コピー元として読むため、記録が始まってから書き換えると読みながら書くことになる
        // (詳細はIRHIDevice::PrepareTextureContentsのコメント)
        m_TextureStreaming.CommitReady(*m_Device);

        if (m_Window->GetWidth() == 0 || m_Window->GetHeight() == 0)
        {
            return;
        }

        // WndProc(Updateスレッド)でキューイングされたメッセージを、ImGuiの状態を実際に読み書きする
        // このRenderスレッド自身からImGui_ImplWin32_WndProcHandlerへ転送する。ImGui::NewFrame()より前に
        // 行うことで、このフレームのNewFrame()が最新のマウス/キーボード状態を反映できる
        m_Window->ForwardQueuedMessagesToImGui();

        // モニタの拡大率に合わせてUIの大きさを揃える。ImGuiの状態を触るのはこのRenderスレッド
        // だけという不変条件を守るため、Window側は値をatomicへ置くだけにし、
        // 実際のスタイル再適用はここで行う
        m_UIManager->OnUIScaleChanged(m_Window->GetDpiScale() * UI::UITheme::kUIScaleMultiplier);

        m_ImGuiBackend->NewFrame();
        if (frameState.ImGuiVisible)
        {
            UI::PanelDrawContext panelContext;
            panelContext.Camera = &frameState.Camera;
            m_UIManager->Draw(panelContext);

            // オーサリングツール(Tools/KurenaiShowEditor)が足す追加のUI。
            // 【ImGuiVisibleの中に置くこと】F1で全パネルを隠したときに、これだけが
            // 画面に残ってしまうとスクリーンショットでの計測が壊れる
            if (m_ExtraImGuiCallback)
            {
                m_ExtraImGuiCallback();
            }
        }

        // ImGuiがマウス/キーボードを掴んでいるかをUpdateスレッドへ返す(Update()が読む)。
        // パネル非表示のときは掴んでいないので明示的にfalseを書く
        {
            const ImGuiIO& io = ImGui::GetIO();
            m_ImGuiWantCaptureKeyboard.store(
                frameState.ImGuiVisible && io.WantCaptureKeyboard, std::memory_order_relaxed);
            m_ImGuiWantCaptureMouse.store(frameState.ImGuiVisible && io.WantCaptureMouse, std::memory_order_relaxed);
        }

        // バッファ精度(デバッグ表示パネルのラジオボタン)と内部レンダー解像度(システムパネル)の
        // 切り替え要求をここで処理する。
        // レンダーターゲットを破棄する前に、DX12がまだ実行中かもしれない直前数フレームの
        // 描画コマンドを完了させる必要がある(LoadSceneがGPUリソースを破棄する前に
        // WaitForGPUIdleを呼ぶのと同じ理由)。このフレームのGPUコマンドはまだ1つも
        // 積んでいないため、ここで待っても待ち時間は前フレームぶんだけで済む。
        //
        // ここはApplyPendingResizeの後、かつこのフレームでm_RenderWidth/m_RenderHeightを
        // 読み始めるより前(最初の読み取りはTAAジッター)なので、解像度をまとめて差し替えてよい
        if (m_BufferPrecisionDirty || m_RenderResolutionDirty || m_PlanarReflectionResolutionDirty ||
            m_UpscaleTargetsDirty || m_MegaLightsReservoirDirty)
        {
            const bool precisionChanged = m_BufferPrecisionDirty;
            m_BufferPrecisionDirty = false;
            // MegaLightsのリザーババッファは CreateRenderTargets の中で作り直される。
            // 標本数の変更だけでもここを通す(GPUが参照していない状態が要るため)
            m_MegaLightsReservoirDirty = false;

            const uint32_t previousWidth = m_RenderWidth;
            const uint32_t previousHeight = m_RenderHeight;
            if (m_RenderResolutionDirty)
            {
                m_RenderResolutionDirty = false;
                m_RenderWidth = m_PendingRenderWidth;
                m_RenderHeight = m_PendingRenderHeight;
            }
            if (m_PlanarReflectionResolutionDirty)
            {
                m_PlanarReflectionResolutionDirty = false;
                m_PlanarReflectionResolutionScale = m_PendingPlanarReflectionResolutionScale;
            }

            m_Device->WaitForGPUIdle();
            try
            {
                CreateRenderTargets(m_RenderWidth, m_RenderHeight);
                // 平面反射専用のレンダーターゲットも、呼び出し箇所をCreateRenderTargetsと
                // 揃えてここで作り直す(反射解像度の倍率変更だけの要求でもここを通る)
                CreatePlanarReflectionTargets();
            }
            catch (const std::exception& e)
            {
                // CreateRenderTargets自身がHDR→Legacy8bitのフォールバックを持つため、ここへ来るのは
                // 要求した解像度そのものが確保できない場合(高解像度でのVRAM不足など)。
                // 元の解像度へ戻して作り直す。それも失敗するなら復旧手段が無いのでそのまま送出する
                Core::Logger::Error(
                    "KurenaiEngine3D",
                    "内部レンダー解像度" + std::to_string(m_RenderWidth) + "x" + std::to_string(m_RenderHeight) +
                        "のレンダーターゲット作成に失敗したため、" + std::to_string(previousWidth) + "x" +
                        std::to_string(previousHeight) + "へ戻します: " + e.what());
                m_RenderWidth = previousWidth;
                m_RenderHeight = previousHeight;
                CreateRenderTargets(m_RenderWidth, m_RenderHeight);
                CreatePlanarReflectionTargets();
            }

            // 超解像の出力解像度用テクスチャ。内部解像度用とは作り直す契機が違うため
            // CreateRenderTargetsとは別に持っているが、GPUがそれらを参照していない状態で
            // 作り直す必要があるのは同じなので、上のWaitForGPUIdle()の後のここで行う
            if (m_UpscaleTargetsDirty)
            {
                m_UpscaleTargetsDirty = false;
                try
                {
                    CreateUpscaleTargets(m_UpscaleOutputWidth, m_UpscaleOutputHeight);
                }
                catch (const std::exception& e)
                {
                    // 確保できなければ超解像を諦めて等倍表示へ落とす。内部解像度は下がったままだが、
                    // Presentがバイリニアで拡大するので絵は出続ける(41.23節以前と同じ経路)
                    Core::Logger::Error(
                        "KurenaiEngine3D",
                        "超解像の出力解像度" + std::to_string(m_UpscaleOutputWidth) + "x" +
                            std::to_string(m_UpscaleOutputHeight) +
                            "のテクスチャ作成に失敗したため、超解像を無効にします: " + e.what());
                    m_UpscaleEnabled = false;
                    m_UpscaleTexture.reset();
                    m_UpscaleSharpTexture.reset();
                    m_UpscaleTargetWidth = 0;
                    m_UpscaleTargetHeight = 0;
                }
            }

            // カメラのアスペクト比はUpdateスレッドが読み取って反映する(m_RenderAspectの宣言参照)
            m_RenderAspect.store(
                static_cast<float>(m_RenderWidth) / static_cast<float>(m_RenderHeight), std::memory_order_relaxed);

            // PSOはレンダーターゲットのフォーマットだけに依存し解像度には依存しないため、
            // 作り直すのは精度が変わったときだけでよい。
            // G-Buffer(Emissive)とAO/GIのフォーマットが変わるため、それらへ描くPSOも作り直す。
            // 作り直さないとPSOが宣言するRenderTargetFormatsと実際のRTVがずれ、D3D12では
            // 仕様違反になる(DX11は検証しないため露見しない)
            if (precisionChanged)
            {
                CreatePrecisionDependentPipelineStates();
            }
        }

        auto* commandList = m_Device->GetImmediateCommandList();
        m_GPUProfiler->BeginFrame();
        m_CPUProfiler.BeginFrame();

        // 太陽・月・空の状態を求める(すべて絶対的な測光量[lx]。露出はまだ掛かっていない)
        const SunLighting sunLighting = ComputeSunLighting(
            m_TimeOfDay, m_SunAzimuthDegrees, m_MoonAzimuthDegrees, m_MoonElevationDegrees);

        // === 可変プリ露出の決定 ===
        // 昼(直射日光10万lx)を基準0として、そのフレームのキー照度が何段暗いかを求め、
        // ユーザー設定のEV100へ足す。これによりHDRバッファへ流れる値のレンジが
        // 昼でも夜でもおおむね一定に保たれ、夜がfp16でつぶれなくなる
        // (詳細な理由はm_EffectiveExposureEV100の宣言コメント)。
        // 露出はTonemap/Bloom/AutoExposureが同じ値で割り戻すため、これを動かしても絵は変わらない
        {
            const float keyIlluminance = std::max(sunLighting.KeyIlluminanceLux, 1e-6f);
            const float autoBias = std::log2(keyIlluminance / kSunIlluminanceLux);
            // 【下限-12段の根拠 ― 表示レンジの両端をfp16に収める】
            // このバイアスは「HDRバッファの値」と「トーンマップが受け取る表示値」の橋渡しで、
            //   表示値 = バッファの値 × 2^bias
            // という関係にある。したがってバッファの上限(fp16の65504)と下限(最小正規化数6.1e-5)は、
            // そのまま**表示できる明るさの上限と下限**になる。
            //
            //   上側: 表示16(ACESの白より十分上。ここまで出せれば発光物が白へ振り切れる)を
            //         表すには 65504 × 2^bias >= 16 → bias >= -12
            //   下側: 表示1e-4(sRGBで1階調にも満たない=見えない)がfp16の正規化域に残るには
            //         1e-4 × 2^-bias >= 6.1e-5 → bias <= 0.7
            //
            // よって-12が両立点になる。**要件が表示値で書けているので、シーンのExposureにも
            // その夜の照度にも依存しない**のがこの値の性質である。
            //
            // 【かつて-18だった理由と、それが上限を潰していたこと】
            // 元の-18は「照度の差(満月なら-18.34段)をできるだけ打ち消す」という下側だけの
            // 発想で決まっており、上側は見ていなかった。その結果 表示上限が
            // 65504 × 2^-18 = 0.25 に落ち、**夜はどんな発光物も表示0.25(sRGBで132前後)より
            // 明るくできない**状態になっていた。ドローンショーで[DroneShow]Brightnessを
            // 0.30から45へ150倍にしても画素値が1段も動かなかったのはこれが原因で、
            // 上限に張り付いたまま裾だけが飽和して色を失っていた。
            // -12にすると上限は65504 × 2^-12 = 16へ64倍広がり、Brightnessが再び効くようになる
            // (実測: 0.30/1.0/3.0 で編隊の最大画素値が 166/211/237 と動く。-18では全部153だった)。
            //
            // 【絵が変わらないことの保証】Tonemap/Bloom/AutoExposureはいずれもこの値を
            // 割り戻すので、fp16の範囲に収まっている画素は1つも動かない。実測でも
            // 地形・水面・空の画素値は-18のときと完全に一致し、飽和していた機体だけが変わった。
            // 上限0段は「昼より明るくはしない」の意味で従来どおり
            const float targetEV100 = m_SceneExposureEV100 + std::clamp(autoBias, -12.0f, 0.0f);

            if (!m_EffectiveExposureInitialized)
            {
                // 起動直後・シーン切り替え直後は平滑化せず即座に合わせる
                m_EffectiveExposureEV100 = targetEV100;
                m_EffectiveExposureInitialized = true;
            }
            else
            {
                // 一時停止や巨大なdtで飛ばないよう上限を設ける
                const float deltaTime = std::clamp(m_RenderDeltaTime, 0.0f, 0.1f);
                const float t = std::clamp(1.0f - std::exp(-deltaTime * m_EffectiveExposureAdaptSpeed), 0.0f, 1.0f);
                m_EffectiveExposureEV100 += (targetEV100 - m_EffectiveExposureEV100) * t;
            }
        }

        // 【検証専用】蓄積が始まる瞬間に1回だけ摂動を加える。
        // 時間再利用の「追従」(灯を消したら何フレームで消えるか、露出が跳んでも
        // 明るさが暴れないか)は、静止した絵をいくら撮っても測れない。
        // 蓄積ダンプは総和を書くので、Nを変えた2本の差が1フレームぶんになる ――
        // これで追従の時間変化を、フレームごとのGPU読み戻し無しで測れる
        if (m_MegaLightsPerturbMode != 0 && !m_MegaLightsPerturbApplied && m_MegaLightsAccumTargetFrames > 0 &&
            m_MegaLightsAccumWarmupFrames >= kMegaLightsAccumWarmup)
        {
            m_MegaLightsPerturbApplied = true;
            if (m_MegaLightsPerturbMode == 1)
            {
                // 全ライトを消す。次フレーム以降のGPULight配列から外れるので、
                // 真値は「ローカルライトの寄与が0」になる。時間再利用が履歴を抱えていると
                // すぐには0にならず、その残り方がゴーストそのもの
                for (Assets::Light& light : m_Lights)
                {
                    light.Enabled = false;
                }
                Core::Logger::Info("KurenaiEngine3D", "【検証】全ライトを消しました(ゴースト測定)");
            }
            else if (m_MegaLightsPerturbMode == 2)
            {
                // 実効プリ露出を+2段跳ばす。ライトの放射輝度は露出を掛け込んで作られるので、
                // 履歴のWは前フレームの露出のままになる。補正が効いていれば絵は変わらない
                m_EffectiveExposureEV100 += 2.0f;
                Core::Logger::Info(
                    "KurenaiEngine3D", "【検証】実効プリ露出を+2段跳ばしました(プリ露出補正の確認)");
            }
        }

        const float effectiveExposure = ComputeExposure(m_EffectiveExposureEV100);

        // 手動露出時にTonemap/Bloomが掛ける倍率。
        // HDRバッファには「実効EV100」でプリ露出された値が入っているが、ユーザーが見たいのは
        // 「設定EV100で撮った絵」なので、その差分を割り戻す。
        // 実効EV100は夜に最大18段下がる(=バッファ上の値が26万倍明るくなる)ため、
        // ここを1.0に固定していると夜が昼と同じ明るさで出てしまい、
        // 自動露出をオフにしても露出が時刻に追従し続ける状態になる
        const float manualExposureScale = std::exp2(m_EffectiveExposureEV100 - m_SceneExposureEV100);

        // 自動露出の測光値を上側で止めるための、構図に依存しない基準EV。
        // キー照度は画面に何が写っていようと変わらないので、
        // 「空が画面のどれだけを占めるか」で露出が振れるのを抑えられる
        const float keyReferenceEV100 = ComputeReferenceEV100(sunLighting.KeyIlluminanceLux);

        // カスケードシャドウマップ: カメラ視錐台をkCascadeCount個の深度範囲に分割し、
        // それぞれ専用のライト正射影ビュー・プロジェクション行列を求める
        float cascadeSplits[kCascadeCount];
        ComputeCascadeSplits(frameState.Camera, cascadeSplits);
        DirectX::XMMATRIX cascadeViewProj[kCascadeCount];
        float cascadeNear = frameState.Camera.GetNearZ();
        for (uint32_t cascade = 0; cascade < kCascadeCount; ++cascade)
        {
            cascadeViewProj[cascade] =
                ComputeCascadeLightViewProj(sunLighting.Direction, frameState.Camera, cascadeNear, cascadeSplits[cascade]);
            cascadeNear = cascadeSplits[cascade];
        }

        const DirectX::XMFLOAT3 cameraPosition = frameState.Camera.GetPosition();

        // モデルLODの段を、レンダーグラフを組む前にこの1回だけ決める。
        // 【パスごとに測り直してはいけない】深度プリパスとG-Bufferが違う段を選ぶと、
        // プリパスが深度を書いた画素をG-Bufferが描かず、画面に穴が開く。
        // G-Bufferパスのラムダはそもそもカメラ位置をキャプチャしていない(半透明パスだけが持つ)ので、
        // ここで決めてm_InstanceLODStatesへ置く形にしてある
        UpdateModelLOD(cameraPosition, m_RenderDeltaTime);

        // モデルのストリーミング。【LODの後に呼ぶ】どの段を読むかは選ばれた段で決まる
        UpdateModelStreaming(cameraPosition);

        // インスタンシングのバッチを組み直す。【LODとストリーミングの後】まとめられるかどうかは
        // 「そのフレームに選ばれた段」と「読み込み済みか」で決まる。レンダーグラフの構築より前に
        // 1回だけ呼ぶこと ―― パスごとに組み直すと、深度プリパスとG-Bufferが違うまとめ方をする
        BuildInstanceBatches(commandList);

        // レイトレーシングを常駐の増減へ追随させる(ストリーミング時のみ働く)
        UpdateRaytracingRebuild();

        // テクスチャの常駐ミップの目標を更新し、差のあるものをワーカーへ積む。
        // 実際の差し替えは次フレーム以降のCommitReady(このフレームの先頭で呼んだもの)で確定する。
        // 画面の高さは内部レンダー解像度を使う(ウィンドウ解像度ではない。
        // 内部解像度を下げているときは必要なテクセル密度もその分下がる)
        m_TextureStreaming.UpdateTargets(
            cameraPosition, std::tan(frameState.Camera.GetFovY() * 0.5f), m_RenderHeight, m_RenderDeltaTime);

        // --- TAAのサブピクセルジッター ---
        // 投影行列を1ピクセル未満だけずらして、同じ画素が毎フレームわずかに違う位置をサンプルする
        // ようにする。TAAが複数フレームぶんを蓄積することで実質的なスーパーサンプリングになる。
        // TAA無効時はジッターも必ず0にすること(ジッターだけ残ると画面が振動するだけになる)
        ++m_TAAFrameIndex;
        DirectX::XMFLOAT2 jitterOffsetPixels{ 0.0f, 0.0f };
        if (m_TAAEnabled)
        {
            // Halton列の添字は1から始める。添字0はradical inverseの定義上どの基数でも0となり、
            // オフセットがピクセルの角(-0.5, -0.5)へ偏ってしまう
            const uint32_t haltonIndex = (m_TAAFrameIndex % kTAAJitterSampleCount) + 1;
            jitterOffsetPixels.x = (RadicalInverse(haltonIndex, 2) - 0.5f) * m_TAAJitterScale;
            jitterOffsetPixels.y = (RadicalInverse(haltonIndex, 3) - 0.5f) * m_TAAJitterScale;
        }
        // ピクセル単位のオフセットをNDCとUVの2つの単位へ直す。
        // ピクセル座標は右が+x・下が+yなのに対しNDCは上が+yなので、yだけ符号が反転する
        // (この符号を落とすと縦方向のジッターと速度が逆向きになる)
        const DirectX::XMFLOAT2 jitterNdc{
            2.0f * jitterOffsetPixels.x / static_cast<float>(m_RenderWidth),
            -2.0f * jitterOffsetPixels.y / static_cast<float>(m_RenderHeight),
        };
        // NDC→UVは xy * (0.5, -0.5) + 0.5 なので、ジッターのUV換算はピクセル数/解像度そのものになる
        const DirectX::XMFLOAT2 jitterUv{
            jitterOffsetPixels.x / static_cast<float>(m_RenderWidth),
            jitterOffsetPixels.y / static_cast<float>(m_RenderHeight),
        };

        // ビュー行列と「ジッター済み」射影行列をここで一度だけ確定させ、以降のカメラ由来の行列は
        // すべてこれらから作る。
        //
        // 【なぜ行列の掛け算でジッターを入れられるのか】Camera::GetProjectionMatrixは行ベクトル規約
        // (clip = view * P)で、第3列が(0,0,1,0)すなわち clip.w = viewZ である。
        // XMMatrixTranslationは行ベクトル規約では第3行が(jx, jy, 0, 1)になるので、P * T を展開すると
        // 変化するのは要素[2][0]と[2][1]、つまり clip.xy += jitterNdc * clip.w だけになる。
        // w除算後には ndc.xy += jitterNdc という定数オフセットになり、狙いどおり平行移動として効く。
        //
        // 【なぜ全パスで統一するのか】深度バッファはこのジッター済み行列でラスタライズされる。
        // 深度から位置を復元する側(SSAO/SSIL/SSR/スクリーンスペースシャドウ)がジッター前の行列を
        // 使うと、再構成した位置がサブピクセルぶんずれて自己遮蔽やハローの原因になる。
        // なお射影行列の_33/_43(深度のリニアライズ係数)はジッターでは変化しない
        const DirectX::XMMATRIX viewMatrix = frameState.Camera.GetViewMatrix();
        const DirectX::XMMATRIX jitteredProj =
            frameState.Camera.GetProjectionMatrix() * DirectX::XMMatrixTranslation(jitterNdc.x, jitterNdc.y, 0.0f);

        // --- メッシュレットLODの段を選ぶ入力を、このフレームぶん一度だけ確定させる ---
        //
        // 【全パスへ同じものを配る】シャドウと深度プリパスは同じ増幅シェーダーを使うが、
        // ViewProjは光源やカスケードのものに差し替わっている。各パスのカメラで段を選ぶと
        // 影を落とす形と本体の形が違う段になるため、主カメラの値をここで決めて配る。
        //
        // 【ジッターの影響を受けない値を使う】拡大率_22はジッター(平行移動)では変化しない。
        // 仮に変化する量を使うと、段の境目でTAAのジッター周期に合わせて段が振動する
        {
            DirectX::XMFLOAT4X4 projForLOD;
            DirectX::XMStoreFloat4x4(&projForLOD, frameState.Camera.GetProjectionMatrix());
            m_MeshletLODFrame.CameraPos = frameState.Camera.GetPosition();
            // 射影行列の_22 = 1/tan(fovY/2)。画面の高さ全体が 2*tan(fovY/2) なので、
            // 距離1メートルの1メートルは _22 * 高さ / 2 画素になる
            m_MeshletLODFrame.PixelScale = 0.5f * projForLOD._22 * static_cast<float>(m_RenderHeight);
            m_MeshletLODFrame.Quality = m_MeshletLODEnabled ? m_MeshletLODQuality : 0.0f;
            m_MeshletLODFrame.Forced = m_MeshletLODEnabled ? m_MeshletLODForcedLevel : -1;
            m_MeshletLODFrame.DebugColorByLOD = m_MeshletLODDebugColorEnabled;
        }

        // 有効なライトだけを詰めてt8のライトリストへ渡す。シェーダはLightCount(・ActiveLightCount)の
        // 数までしかループしないため、無効なライトはそもそもGPUへ送らない。DirectLight/Transparentの
        // 両パスがこの1つのリストを共有する(FrameConstants.ActiveLightCountに人数を書き込むため、
        // 各パスのExecute内ではなくFrameConstants確定より前にここで組み立てる必要がある)
        std::vector<GPULight> gpuLights;
        gpuLights.reserve(m_Lights.size());
        for (const Assets::Light& light : m_Lights)
        {
            if (!light.Enabled)
            {
                continue;
            }
            gpuLights.push_back(MakeGPULight(light, m_EffectiveExposureEV100));
        }
        // ここまでが作者の置いたライト。以降のプロキシと切り分けるために数を控える
        const size_t manualLightCount = gpuLights.size();

        // --- エミッシブ光源のプロキシを後ろへ連結する ---
        //
        // 【手置きの後ろに置く】容量超過の切り捨ては下でプロキシ側だけに掛ける。
        // 全体をカメラ距離でソートして切ると、**手置きの遠いライトが黙って消える**。
        //
        // 【毎フレーム作り直す】m_EmissiveIntensity のスライダーとτを即座に反映するため。
        // プロキシ側は倍率も露出も持たない値(RadianceBase)で保持してある
        m_EmissiveLightsUsedCount = 0;
        // 切り捨てが起きたときだけ、採用した集合の指紋を残す(起きなければ0のまま)。
        //
        // 【プローブの署名に要る】採用順はカメラからの照度で決まるので、**カメラを動かすだけで
        // プローブが焼く光源の集合が変わる**。署名が変わらないと反射プローブはOnDemandで
        // 焼き直さず、DDGIは更新を止めたまま、収束済みのプローブだけ古い集合で残る。
        // 切り捨てが起きない限り集合はシーン固定なので、そのときは0で十分
        m_EmissiveLightsSelectionHash = 0;
        if (m_EmissiveLightsEnabled && !m_EmissiveProxies.empty() && manualLightCount < kMaxLights)
        {
            const size_t budget = std::min<size_t>(
                static_cast<size_t>(std::max(0, m_EmissiveLightsMaxCount)), kMaxLights - manualLightCount);

            if (m_EmissiveProxies.size() <= budget)
            {
                for (const Assets::EmissiveProxy& proxy : m_EmissiveProxies)
                {
                    gpuLights.push_back(MakeGPULightFromEmissiveProxy(
                        proxy, m_EmissiveIntensity, m_EmissiveLightsCutoffIrradiance,
                        m_EmissiveLightsMaxRange));
                }
            }
            else
            {
                // 【スコアはカメラ位置に届く表示空間の照度】単なるカメラ距離だと、
                // 遠くの明るい看板より近くの暗い豆電球が残る。
                // 同値のときは (インスタンス, メッシュ, かたまり) の辞書順で決める ――
                // 順序が揺れるとライトが出入りしてちらつく
                std::vector<size_t> order(m_EmissiveProxies.size());
                for (size_t i = 0; i < order.size(); ++i)
                {
                    order[i] = i;
                }
                const auto scoreOf = [this, &cameraPosition](size_t index)
                {
                    const Assets::EmissiveProxy& p = m_EmissiveProxies[index];
                    const float dx = p.Position[0] - cameraPosition.x;
                    const float dy = p.Position[1] - cameraPosition.y;
                    const float dz = p.Position[2] - cameraPosition.z;
                    const float distSq = dx * dx + dy * dy + dz * dz;
                    const float peak = std::max({ p.RadianceBase[0], p.RadianceBase[1], p.RadianceBase[2] }) *
                                       m_EmissiveIntensity * p.Area;
                    return peak / std::max(distSq, p.SourceRadius * p.SourceRadius + 1e-6f);
                };
                std::stable_sort(
                    order.begin(), order.end(),
                    [this, &scoreOf](size_t a, size_t b)
                    {
                        const float sa = scoreOf(a);
                        const float sb = scoreOf(b);
                        if (sa != sb) { return sa > sb; }
                        const Assets::EmissiveProxy& pa = m_EmissiveProxies[a];
                        const Assets::EmissiveProxy& pb = m_EmissiveProxies[b];
                        if (pa.InstanceIndex != pb.InstanceIndex) { return pa.InstanceIndex < pb.InstanceIndex; }
                        if (pa.MeshIndex != pb.MeshIndex) { return pa.MeshIndex < pb.MeshIndex; }
                        return pa.ClusterIndex < pb.ClusterIndex;
                    });
                // FNV-1a(64bit)。採用したプロキシの識別子だけを順に混ぜる。
                // 位置や強さは混ぜない ―― それらはシーン固定で、変わるのは「どれを採ったか」だけ
                uint64_t selectionHash = 1469598103934665603ull;
                const auto mixIndex = [&selectionHash](uint32_t value)
                {
                    const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
                    for (size_t b = 0; b < sizeof(value); ++b)
                    {
                        selectionHash ^= bytes[b];
                        selectionHash *= 1099511628211ull;
                    }
                };
                for (size_t i = 0; i < budget; ++i)
                {
                    const Assets::EmissiveProxy& proxy = m_EmissiveProxies[order[i]];
                    mixIndex(proxy.InstanceIndex);
                    mixIndex(proxy.MeshIndex);
                    mixIndex(proxy.ClusterIndex);
                    gpuLights.push_back(MakeGPULightFromEmissiveProxy(
                        proxy, m_EmissiveIntensity, m_EmissiveLightsCutoffIrradiance, m_EmissiveLightsMaxRange));
                }
                m_EmissiveLightsSelectionHash = selectionHash;

                // 【切り捨ては発光を捨てている】併合で減らせないか先に疑うこと。
                // EmeraldSquare の実測では、面積の大きい順に上位256個を残しても
                // 総面積の46.7%にしかならない(上位1024個でも84.9%)
                if (!m_EmissiveLightsCapLogged)
                {
                    Core::Logger::Warning(
                        "KurenaiEngine3D",
                        "エミッシブ光源が上限(" + std::to_string(budget) + ")を超えたため" +
                            std::to_string(m_EmissiveProxies.size() - budget) +
                            "個を捨てました。捨てたぶんの発光は絵から消えます" +
                            (ShouldSuppressEmissiveForGI()
                                 ? "。**しかもDDGIからは抑止されたまま**です ―― 捨てた面は"
                                   "直接光にも間接光にも入らず、純粋なエネルギー損失になります"
                                 : ""));
                    m_EmissiveLightsCapLogged = true;
                }
            }
            m_EmissiveLightsUsedCount = static_cast<uint32_t>(gpuLights.size() - manualLightCount);

            // 【「効いていない」と「暗すぎて見えない」を切り分けられるようにする】
            // 絵の差だけを見ていると、経路が走っていないのか寄与が小さいだけなのかが分からない。
            // 実際に送った灯数と、代表1灯の強さ・Range・κ を1回だけ出す
            if (!m_EmissiveLightsValuesLogged && m_EmissiveLightsUsedCount > 0)
            {
                const GPULight& sample = gpuLights[manualLightCount];
                // 【RGBの最大を出す。Rだけを出さない】Rangeはmax(R,G,B)から解いているので、
                // 色付きの自発光(赤い看板など)でRだけを見るとログからRangeを検算できない
                const float samplePeak =
                    std::max({ sample.ColorRange.x, sample.ColorRange.y, sample.ColorRange.z });
                Core::Logger::Info(
                    "KurenaiEngine3D",
                    "エミッシブ光源を送信: " + std::to_string(m_EmissiveLightsUsedCount) + "灯(手置き " +
                        std::to_string(manualLightCount) + "灯) / 先頭の灯 強さ(RGBの最大) " +
                        std::to_string(samplePeak) + " Range " + std::to_string(sample.ColorRange.w) +
                        "m 半径 " + std::to_string(sample.Params.z) + "m κ " + std::to_string(sample.Params.w));
                m_EmissiveLightsValuesLogged = true;
            }
        }

        // 容量(kMaxLights)を超える場合は、カメラに近い順に先頭kMaxLights灯のみ採用する。
        // 全画面ディファードなのでフラスタムカリングは効果が薄く、これは容量超過時の
        // 安全弁としてのみ機能する。
        //
        // 【ここへ来るのは手置きライトだけで超えたとき】プロキシは上で別枠に収めてある
        if (gpuLights.size() > kMaxLights)
        {
            std::sort(
                gpuLights.begin(), gpuLights.end(),
                [&cameraPosition](const GPULight& a, const GPULight& b)
                {
                    const float dxA = a.PositionType.x - cameraPosition.x;
                    const float dyA = a.PositionType.y - cameraPosition.y;
                    const float dzA = a.PositionType.z - cameraPosition.z;
                    const float dxB = b.PositionType.x - cameraPosition.x;
                    const float dyB = b.PositionType.y - cameraPosition.y;
                    const float dzB = b.PositionType.z - cameraPosition.z;
                    return (dxA * dxA + dyA * dyA + dzA * dzA) < (dxB * dxB + dyB * dyB + dzB * dzB);
                });
            gpuLights.resize(kMaxLights);

            if (!m_LightOverflowLogged)
            {
                Core::Logger::Warning(
                    "KurenaiEngine3D",
                    "ライト数が上限(" + std::to_string(kMaxLights) + ")を超えたため、カメラに近い順に描画します");
                m_LightOverflowLogged = true;
            }
        }

        // タイルライトカリングの1タイルあたりの容量超過の可能性を早めに知らせる。
        // 実際に超過したかどうかはGPU側にしか無く(バッファのリードバック経路がRHIに無い)、
        // ここで分かるのは「シーンのライト数が容量を超えているので、1つのタイルに集中すれば
        // 超過し得る」という条件までである。実際の超過はデバッグ表示(DebugView::LightTiles)の
        // マゼンタで確認する
        if (m_LightCullingEnabled && gpuLights.size() > kLightTileCapacity && !m_LightTileOverflowLogged)
        {
            Core::Logger::Warning(
                "KurenaiEngine3D",
                "有効ライト数(" + std::to_string(gpuLights.size()) + ")がタイルの容量(" +
                    std::to_string(kLightTileCapacity) +
                    ")を超えています。1タイルへ集中した場合そのタイルではライトが欠落します"
                    "(Render TargetsのLight Tiles表示でマゼンタのタイルとして確認できます)");
            m_LightTileOverflowLogged = true;
        }

        // このフレームで空として使うキューブマップ。手続き空(SkyGenerate)か.ksceneのDDSかが
        // ここで確定する。**RenderGraphのReads宣言と実際のバインドの両方でこのローカルを使うこと**
        // (ActiveSkyTexture()を都度呼ぶと両者が食い違って依存解決が壊れる)。
        // 【ここで確定させる理由】この下のFrameConstants(constants.SkyParams.y)が
        // usingProceduralSkyを必要とするため、FrameConstantsを埋めるより前に確定させる
        RHI::IRHITexture* const skyTexture = ActiveSkyTexture();
        const bool usingProceduralSky = (skyTexture == m_ProceduralSkyTexture.get());

        // 太陽が閾値以上動いていたら手続き空を焼き直す。毎フレーム焼くと
        // 空生成6回+プリフィルタ36回のディスパッチが常時走って無駄になる。
        // 空はプリ露出済みの値で焼かれるため、実効プリ露出が動いたときも焼き直す必要がある
        // (焼き直さないと空だけ古い露出のまま取り残される)
        if (usingProceduralSky && !m_SkyBakeDirty)
        {
            const DirectX::XMVECTOR current = DirectX::XMLoadFloat3(&sunLighting.SunPosition);
            const DirectX::XMVECTOR baked = DirectX::XMLoadFloat3(&m_LastBakedSunPosition);
            const float cosAngle = DirectX::XMVectorGetX(DirectX::XMVector3Dot(current, baked));
            const bool sunMoved =
                cosAngle < std::cos(DirectX::XMConvertToRadians(m_SkyBakeAngleThresholdDegrees));
            // 露出が0.05段(約3.5%)以上動いたら焼き直す。時刻変化に伴う露出の追従でも
            // 動くため、太陽の角度閾値とあわせて実質的に連続した更新になる
            const bool exposureMoved =
                std::abs(m_EffectiveExposureEV100 - m_LastBakedExposureEV100) > 0.05f;
            // タービディティが動いたら焼き直す。PreethamのxyYモデルの形自体が変わるため、
            // exposureMovedと同じ形の判定をここへ追加する
            const bool turbidityMoved = std::abs(m_SkyTurbidity - m_LastBakedTurbidity) > 0.01f;
            // 空の彩度(アート指定)もPreethamの色度を動かすため、タービディティと同じ扱いで焼き直す
            const bool saturationMoved = std::abs(m_SkySaturation - m_LastBakedSkySaturation) > 0.005f;
            if (sunMoved || exposureMoved || turbidityMoved || saturationMoved)
            {
                m_SkyBakeDirty = true;
            }
        }

        // このフレームで手続き空を焼くかどうか。下のSkyGenerateパス登録とキャッシュ更新の
        // 両方をこのフラグで判定する
        const bool bakeSkyThisFrame = usingProceduralSky && m_SkyBakeDirty;

        // このフレームでSkyIntegrateパス(m_SkyParametersBufferへ書く)を実行するかどうか。
        // 通常はbakeSkyThisFrameと同じタイミングだが、m_SkyParametersBufferが一度も書かれていない
        // 場合はusingProceduralSkyがfalse(.ksceneのDDSスカイボックス使用時)でも1回だけ実行する。
        //
        // 【なぜCPU側からのゼロ初期化ではなくこの形にしたのか】DX12のStructuredRWバッファは
        // UAV/SRVでのGPUアクセス専用にDEFAULTヒープへ作成されており、CPUから書き込むための
        // マップ済みポインタ・ステージングリングを一切持たない(m_SkyParametersBuffer作成箇所の
        // コメント参照)。そのためUpdateBufferでのゼロ埋めはDX12でクラッシュする。GPU側のパスを
        // 1回だけ走らせれば、DX11/DX12のどちらでも安全に(積分結果自体は使われないが)未初期化状態を
        // 解消できる。太陽方向・目標照度はusingProceduralSkyに関わらず既に計算済みのsunLightingを
        // そのまま使えるため、余分な分岐を増やさずに済む
        const bool skyIntegrateThisFrame = bakeSkyThisFrame || !m_SkyParametersBufferInitialized;

        // --- 空パラメータ(tintと天頂輝度)の確定はGPU側(SkyIntegrate.hlsl)で行う ---
        // 【なぜベイクと同じタイミングか】背景の解析評価(DeferredLighting.hlsl)は、下のFrameConstants
        // (SkySunDirection)とm_SkyParametersBuffer(SkyIntegrate.hlslの出力)を組み合わせて使う。
        // ベイクと同じタイミングでSkyIntegrateパスを実行することで、背景とキューブマップ
        // (IBL・反射)が常に同一の空パラメータを見る。毎フレーム走らせると、太陽の角度閾値で
        // ベイクを間引いている間だけ背景とIBLの空がずれてしまう。実際のディスパッチとcbuffer更新は
        // 下のSkyIntegrateパス登録側で行うため、ここではフラグ更新のみ済ませる
        if (bakeSkyThisFrame)
        {
            // 雲(判断B)による平均透過率をベイクと同じタイミングで確定させ、メンバへキャッシュする。
            // **この値はm_SkyParametersBuffer側の天頂輝度には掛けない**——キューブへ焼く
            // SkyBakeConstants::CloudTransmittance(下のSkyGenerateパス参照)にだけ掛ける。
            // SkyParametersBufferの天頂輝度を減光すると、雲の隙間から見える青空まで暗くなり、
            // Sky.hlsli側のSkyColorがそこへさらに雲を重ねることで二重に暗くなってしまう
            m_ActiveCloudTransmittance = ComputeCloudAverageTransmittance(
                m_CloudEnabled, m_CloudCoverage, m_CirrusEnabled, m_CirrusCoverage);

            // 空が変わったのでプリフィルタ済み鏡面も焼き直す必要がある。
            // 焼き直し要否のフラグ更新はここ(キャッシュを書いた場所)に一本化し、
            // 下のSkyGenerateパス登録側では行わない(二重更新・更新漏れを避けるため)
            m_SkyBakeDirty = false;
            m_LastBakedSunPosition = sunLighting.SunPosition;
            m_LastBakedExposureEV100 = m_EffectiveExposureEV100;
            m_LastBakedTurbidity = m_SkyTurbidity;
            m_LastBakedSkySaturation = m_SkySaturation;
            m_IBLBaked = false;
            m_IBLIrradianceBaked = false;
        }

        // 平面反射: 水面インスタンスを探し、その高さ(ワールドY)を水面の平面とする。
        // 水面メッシュはローカルY=0の水平な板(Tools/generate_water_plane.py参照)なので、
        // ワールド変換の平行移動Y(instance.World._24。転置済みのため列に入っている。
        // Transparentパスの距離ソートと同じ規約)がそのまま水面の高さになる。
        // 複数の水面インスタンスが異なる高さで見つかった場合は最初のものだけを使い、警告を1度だけ出す
        // (「水面は単一の水平な平面である」という前提を明示する)
        bool hasWaterInstance = false;
        float waterPlaneY = 0.0f;
        for (const auto& instance : m_Scene.Instances)
        {
            if (!instance.IsWater)
            {
                continue;
            }
            const float instanceWaterY = instance.World._24;
            if (!hasWaterInstance)
            {
                hasWaterInstance = true;
                waterPlaneY = instanceWaterY;
            }
            else if (std::abs(instanceWaterY - waterPlaneY) > 0.01f && !m_PlanarReflectionMultipleWaterLogged)
            {
                Core::Logger::Warning(
                    "KurenaiEngine3D",
                    "複数の水面インスタンスが異なる高さ(Y=" + std::to_string(waterPlaneY) + "とY=" +
                        std::to_string(instanceWaterY) +
                        ")で見つかりました。平面反射は最初の水面のみを使用します"
                        "(水面は単一の水平な平面である前提のため)");
                m_PlanarReflectionMultipleWaterLogged = true;
            }
        }
        // このフレームで平面反射パスを実行するか。
        // 【反射の手法がSSRのときだけ実行する】このパスの出力(m_PlanarReflectionColor)を読むのは
        // SSR.hlslだけである。手法がRaytracedやOffのときに走らせても、不透明メッシュ全体を
        // もう1回フォワードで描いた結果を誰も読まないまま捨てることになる
        // (DXR対応環境ではDefaultReflectionModeがRaytracedを返すため、この条件が無いと
        //  DX12では常に丸ごと無駄になる。実測でもDX12起動時に水面へ映っていたのはRT反射の結果で、
        //  平面反射パスの出力ではなかった)
        const bool planarReflectionPassRuns =
            m_PlanarReflectionEnabled && hasWaterInstance && m_ReflectionMode == ReflectionMode::ScreenSpace;

        // 大気遠近パスを実行するか。UIで無効化されているか、密度が0以下(効果が無い)なら
        // パス自体を登録しない(GetActiveReflectionOutput()の結果がそのままTAA/Tonemapへ渡る)。
        // 手続き空が無効なシーンかどうかの判断(FogParams0.w)はパスの実行有無とは別に、
        // 下のconstants.FogParams0組み立て時にusingProceduralSkyを見て決める
        // (SSRパスのwaterAnalyticSkyFlagと同じ、パスの実行可否とシェーダー内の有効フラグを分ける設計)
        const bool fogPassRuns = m_FogEnabled && m_FogDensity > 0.0f;

        // メッシュレット(増幅シェーダー + メッシュシェーダー)経路でG-Bufferを描くか。
        // メッシュシェーダー非対応のデバイスではPSOが作られないためnullptrになる。
        //
        // 【他の「PassRuns」と並べてここに置く理由】この値はG-Bufferパスの登録時だけでなく、
        // その手前で書き上げるFrameConstantsも見る(オクルージョンカリングの有効フラグ)。
        // 定数バッファの更新はパス登録より前に一度だけ行うため、判断もそこより前で確定させる
        const bool meshletPathActive = m_MeshletRenderingEnabled && m_GBufferMeshletPipelineState != nullptr;

        // 増幅シェーダーのHi-Zオクルージョンカリング(Stage 5-2)をこのフレームで行うか。
        // 判定を書いてあるのは増幅シェーダーだけなので、メッシュレット経路に乗らないフレームでは
        // 1つも間引けず、Hi-Zを構築する意味も無い(下のHi-Zパスの登録条件がこれを見る)
        const bool occlusionCullingActive = m_OcclusionCullingEnabled && meshletPathActive;

        // メッシュレットカリングの統計をこのフレームで数えるか。
        // 増幅シェーダーが走らなければ数える相手がいない
        const bool meshletCullStatsActive =
            m_MeshletCullStatsEnabled && meshletPathActive && m_MeshletCullStatsBuffer != nullptr;

        FrameConstants constants;
        const DirectX::XMMATRIX viewProj = viewMatrix * jitteredProj;
        DirectX::XMStoreFloat4x4(&constants.ViewProj, DirectX::XMMatrixTranspose(viewProj));

        // 平面反射用の鏡映カメラ。水面平面 y=waterPlaneY に対する反射行列を、通常のView×Projへ
        // 左から掛ける(PlanarReflection.hlsl冒頭参照)。XMMatrixReflectが受け取る平面の規約は
        // 「点PがAx+By+Cz+D=0を満たす」形(ドキュメント準拠)で、これは
        // FrameConstants.PlanarReflectionPlaneのSV_ClipDistance計算(dot(worldPos, xyz) + w)と
        // 完全に同じ規約なので、同じベクトル(0,1,0,-waterPlaneY)がどちらにもそのまま使える
        // (水面より上のworldPosでdot結果が正になることも、この式から導ける)。
        // 水面が無いシーンでもwaterPlaneY=0で計算はできるが、パスを登録しないため使われない
        const DirectX::XMMATRIX reflectMatrix =
            DirectX::XMMatrixReflect(DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, -waterPlaneY));
        // メインカメラと同じジッター済みProjを使う(PlanarReflection.hlsl冒頭参照。ジッターが
        // 異なると反射がメインの画面UVとサブピクセル単位でずれてしまう)
        const DirectX::XMMATRIX reflectedViewProj = reflectMatrix * viewMatrix * jitteredProj;
        DirectX::XMVECTOR determinant;
        const DirectX::XMMATRIX invViewProj = DirectX::XMMatrixInverse(&determinant, viewProj);
        DirectX::XMStoreFloat4x4(&constants.InvViewProj, DirectX::XMMatrixTranspose(invViewProj));
        for (uint32_t cascade = 0; cascade < kCascadeCount; ++cascade)
        {
            DirectX::XMStoreFloat4x4(&constants.CascadeViewProj[cascade], DirectX::XMMatrixTranspose(cascadeViewProj[cascade]));
        }
        // 【DDGIのクリップマップの追従中心をここで固定する】このあと組み立てるFrameConstantsの
        // 各LODの原点も、後段のプローブのキャプチャ位置も、すべてこの値を基準に決まる。
        // 1フレームの途中で動かすと「シェーダーが見ている格子」と「実際に焼いた位置」が
        // 食い違い、間接光が別の場所のものになる
        m_DDGIFollowCenter = DirectX::XMFLOAT3{ cameraPosition.x, cameraPosition.y, cameraPosition.z };

        constants.CameraPosition = { cameraPosition.x, cameraPosition.y, cameraPosition.z, 0.0f };
        constants.LightDirection = { sunLighting.Direction.x, sunLighting.Direction.y, sunLighting.Direction.z, 0.0f };
        // 太陽を無効にする場合は色をゼロにするだけでよい(シェーダー側は太陽の寄与に
        // LightColor.rgbを乗算するため、これで完全に消える)。TimeOfDayを夜にする方法と違い
        // 昼度(AmbientColor.a)は下がらないので、環境光だけで照らす状態を作れる
        // sunLighting.Color は絶対的な測光量[lx]なので、ここで実効プリ露出を掛けて表示レンジへ移す
        constants.LightColor = m_SunEnabled
            ? DirectX::XMFLOAT4{
                  sunLighting.Color.x * effectiveExposure,
                  sunLighting.Color.y * effectiveExposure,
                  sunLighting.Color.z * effectiveExposure,
                  0.0f }
            : DirectX::XMFLOAT4{ 0.0f, 0.0f, 0.0f, 0.0f };
        DirectX::XMStoreFloat4x4(&constants.View, DirectX::XMMatrixTranspose(viewMatrix));
        // ジッター済みの射影行列を渡す。SSAO/SSILはこの行列でView空間の点を画面へ投影して
        // 深度バッファと突き合わせるため、深度を描いたときと同じ行列でなければサブピクセルぶんずれる
        DirectX::XMStoreFloat4x4(&constants.Proj, DirectX::XMMatrixTranspose(jitteredProj));
        // rgb(環境光の色)にm_AmbientScaleを乗算する。Enable IBL無効時のフォールバックアンビエント
        // (DeferredLighting.hlsl)の強度調整用で、alpha(dayFactor、IBLの夜間減光・背景スカイの
        // 昼夜ブレンドに使う)には掛けない
        constants.AmbientColor =
        {
            sunLighting.Ambient.x * m_AmbientScale * effectiveExposure,
            sunLighting.Ambient.y * m_AmbientScale * effectiveExposure,
            sunLighting.Ambient.z * m_AmbientScale * effectiveExposure,
            sunLighting.Ambient.w,
        };
        constants.CascadeSplits = { cascadeSplits[0], cascadeSplits[1], cascadeSplits[2], cascadeSplits[3] };
        const float iblIntensity = m_IBLEnabled ? m_IBLIntensity : 0.0f;
        const float specularEnergyCompensation = static_cast<float>(m_SpecularCompensationMode);
        constants.ShadowParams = {
            m_ShadowLightSize,
            static_cast<float>(kIBLPrefilterMipLevels - 1),
            iblIntensity,
            specularEnergyCompensation,
        };
        constants.ActiveLightCount = { static_cast<float>(gpuLights.size()), 0.0f, 0.0f, 0.0f };
        constants.IBLParams = {
            m_IBLUseDedicatedIrradiance ? 1.0f : 0.0f,
            m_AmbientDiffuseScale,
            m_AmbientSpecularScale,
            0.0f,
        };
        constants.OcclusionParams = {
            m_BentNormalAOSource ? 1.0f : 0.0f,
            static_cast<float>(m_SpecularOcclusionMode),
            m_MultiBounceAOEnabled ? 1.0f : 0.0f,
            0.0f };

        // 空の解析評価用。DeferredLighting.hlslが背景画素でSky.hlsliのSkyColorを画面解像度で
        // 評価するために使う。ティントと天頂輝度はm_SkyParametersBuffer(直近の手続き空ベイクで
        // SkyIntegrate.hlslが書いた値。上のbakeSkyThisFrameブロック参照)にあり、DeferredLighting.hlsl/
        // SSR.hlslがStructuredBufferとして直接読むため、ここでFrameConstantsへは詰めない。
        // SunDirectionはここで毎フレーム最新のsunLightingから渡す
        // (太陽は角度閾値以下でも連続的に動くため。天頂輝度・色味と違い積分を伴わず、
        // 毎フレーム渡してもコストが無い)。
        // 正規化はSkyGenerate.hlsl側の慣習(呼び出し側=シェーダのSkyParameters組み立て時に
        // normalizeする)に合わせ、C++側では正規化しない(DeferredLighting.hlsl側で行う)
        constants.SkySunDirection = {
            sunLighting.SunPosition.x, sunLighting.SunPosition.y, sunLighting.SunPosition.z, 0.0f
        };
        // 太陽照度と空照度の比。Sky.hlsliのEvaluateCloudLayerが雲の明るさの基準を
        // 「空の天頂輝度」から「太陽の照度」へ切り替えるために使う(雲を照らしているのは
        // 空ではなく太陽であるため。詳細はSky.hlsli側のkCumulusSingleScatterScale等のコメント参照)。
        // SkyIlluminanceLuxが0近傍(理論上は起こらないが)のときのゼロ除算を避けてある
        const float sunToSkyIlluminanceRatio =
            (sunLighting.SkyIlluminanceLux > 1e-6f)
                ? (sunLighting.KeyIlluminanceLux / sunLighting.SkyIlluminanceLux)
                : 0.0f;
        constants.SkyParams = {
            // x=未使用(天頂輝度はSkyParametersBufferにある)
            0.0f,
            // 手続き空が無効(.ksceneのDDSスカイボックス使用時)は、この設定に関わらず
            // 常にキューブマップを使う。DDSは任意の絵でPerezモデルとは無関係なため、
            // 解析評価してはいけない
            (m_SkyAnalyticBackground && usingProceduralSky) ? 1.0f : 0.0f,
            // z=太陽照度/空照度比(SunToSkyIlluminanceRatio、雲の明るさの基準に使う)
            sunToSkyIlluminanceRatio,
            0.0f,
        };

        // === 実効プリ露出が大きく動いたら、更新モードに関わらずプローブを焼き直す(19.14節) ===
        // 下のProbeParams2.wは「焼いた時点の露出→現在の露出」の換算倍率で、これだけでも
        // プローブの値の解釈は常に正しくなる。ただし換算はあくまで**焼いた時点の環境**を
        // 正しい明るさで見せるだけなので、昼に焼いたプローブを夜の場面へ持ち込めば
        // 「夜の部屋に昼の環境が正しい明るさで映り込む」ことになり、換算前より派手に破綻する
        // (実測: ProbeTestを夜にしたときの平均輝度が213.6→253.9、白飽和78%)。
        //
        // 実効プリ露出が大きく動くのは時刻が大きく動いたときなので、そのときは環境そのものが
        // 古くなっている。Bakedモードが凍結すると宣言しているのはライトやマテリアルの編集に
        // 対してであって、場面全体の明るさが2倍以上変わってもなお昼の映り込みを保持することでは
        // ない。手続き空が同じ理由で焼き直しているのと揃える(閾値は空の0.05段よりずっと粗く
        // 取ってある。フルベイクはプローブ数×6面の描画になるため)。
        // Realtimeは毎フレーム焼き直しているので対象外
        if (m_ProbeUpdateMode != ProbeUpdateMode::Realtime && m_ProbeBaked && !m_ReflectionProbes.empty() &&
            std::abs(m_EffectiveExposureEV100 - m_ProbeBakedExposureEV100) > kProbeRebakeExposureEV)
        {
            m_ProbeBakeRequested = true;
            // このフレームの後半で今の露出で焼かれるため、換算倍率もここで合わせておく。
            // ここで合わせないと、焼き直したフレームだけ1フレーム古い倍率が掛かって明滅する
            m_ProbeBakedExposureEV100 = m_EffectiveExposureEV100;
        }

        // 反射プローブの影響範囲をt13のStructuredBufferへ渡す。まだ一度も焼けていない場合
        // (m_ProbeBaked=false)や機能を無効にしている場合はプローブ数を0にして、シェーダー側の
        // 選択ループ自体を回さない=中身が未定義のキューブマップを引かせないようにする
        std::vector<GPUReflectionProbe> gpuProbes;
        if (m_ReflectionProbeEnabled && m_ProbeBaked)
        {
            gpuProbes.reserve(m_ReflectionProbes.size());
            for (const Assets::ReflectionProbe& probe : m_ReflectionProbes)
            {
                // Yawはシェーダー側で毎ピクセル三角関数を回さずに済むよう、ここでsin/cosへ展開しておく
                const float yawRadians = DirectX::XMConvertToRadians(probe.YawDegrees);
                const bool isBox = probe.Shape == Assets::ReflectionProbeShape::Box;

                GPUReflectionProbe gpuProbe{};
                gpuProbe.PositionRadius = { probe.Position[0], probe.Position[1], probe.Position[2], probe.Radius };
                gpuProbe.BoxExtents = {
                    probe.BoxExtents[0], probe.BoxExtents[1], probe.BoxExtents[2], probe.BlendDistance
                };
                gpuProbe.ShapeParams = {
                    isBox ? 1.0f : 0.0f, std::sin(yawRadians), std::cos(yawRadians), 0.0f
                };
                gpuProbes.push_back(gpuProbe);
            }
        }
        if (!gpuProbes.empty())
        {
            commandList->UpdateBuffer(m_ProbeBuffer.get(), gpuProbes.data(), sizeof(GPUReflectionProbe) * gpuProbes.size());
        }

        const float probeInfluenceDebug = (m_DebugView == DebugView::ProbeInfluence) ? 1.0f : 0.0f;
        constants.ProbeParams = {
            static_cast<float>(gpuProbes.size()),
            probeInfluenceDebug,
            m_ProbeParallaxCorrectionEnabled ? 1.0f : 0.0f,
            m_ProbeBlendingEnabled ? 1.0f : 0.0f,
        };
        constants.ProbeParams2 = {
            m_ProbeDepthParallaxEnabled ? 1.0f : 0.0f,
            m_ProbeOcclusionEnabled ? 1.0f : 0.0f,
            static_cast<float>(kProbeCaptureSize),
            // 焼いた時点の実効プリ露出から現在の実効プリ露出への換算倍率
            // (m_ProbeBakedExposureEV100のコメント参照)。ComputeExposure(ev)=1/(1.2*2^ev)
            // なので、比は 2^(焼いたEV - 現在のEV) になる。
            // フルベイクが走るフレームだけは1フレームぶん古い倍率になるが、それが問題になるのは
            // 「焼き直しと大きな露出変化が同じフレームで起きる」ときだけで、シーン読み込み時は
            // 上のm_EffectiveExposureInitialized=falseで露出が既に確定しているため起きない
            std::exp2(m_ProbeBakedExposureEV100 - m_EffectiveExposureEV100),
        };

        // モーションベクター用の前フレーム情報。初回フレームは前フレームの行列が未定義なので、
        // 今フレームと同じものを入れて速度を0にしておく。そうしないとゴミの速度が速度バッファへ
        // 焼き込まれ、画面全体が一度だけゴーストする
        if (m_TAAPrevViewProjValid)
        {
            constants.PrevViewProj = m_TAAPrevViewProj;
            constants.TAAParams = { jitterUv.x, jitterUv.y, m_TAAPrevJitterUv.x, m_TAAPrevJitterUv.y };
        }
        else
        {
            constants.PrevViewProj = constants.ViewProj;
            constants.TAAParams = { jitterUv.x, jitterUv.y, jitterUv.x, jitterUv.y };
        }

        // DDGI(22章)。一度も焼けていない間はアトラスの中身が未定義なので無効にしておく
        // (反射プローブのm_ProbeBakedと同じ方針)
        const bool ddgiActive = m_DDGIEnabled && m_HasGIVolume && m_DDGIBaked;
        constants.DDGIParams0 = {
            m_GIVolume.Origin[0], m_GIVolume.Origin[1], m_GIVolume.Origin[2],
            ddgiActive ? 1.0f : 0.0f,
        };
        constants.DDGIParams1 = {
            m_GIVolume.ProbeSpacing[0], m_GIVolume.ProbeSpacing[1], m_GIVolume.ProbeSpacing[2],
            m_GIVolume.NormalBias,
        };
        constants.DDGIParams2 = {
            static_cast<float>(m_GIVolume.ProbeCounts[0]),
            static_cast<float>(m_GIVolume.ProbeCounts[1]),
            static_cast<float>(m_GIVolume.ProbeCounts[2]),
            m_GIVolume.ViewBias,
        };
        constants.DDGIParams3 = {
            static_cast<float>(kDDGIIrradianceTexels),
            static_cast<float>(kDDGIDistanceTexels),
            m_DDGIIntensity,
            static_cast<float>(kDDGIProbeBorder),
        };
        // y = DeferredLightingがDDGIを低解像度パス(DDGIResolve)から引くか。
        // 【パスが実際に走る条件と一致させること】走らないのに1を渡すと、前フレームの
        // (あるいは未初期化の)低解像度バッファを読んで間接光が固まる/壊れる。
        // 条件はDDGIResolveパスの登録側(ddgiResolvePassRuns)と同じものを並べている
        const bool ddgiHalfResolutionActive =
            m_DDGIHalfResolution && m_DDGIResolveTexture && m_DDGIEnabled && m_HasGIVolume && m_DDGIBaked;
        // プローブ分類のしきい値。裏面の情報を持てるのはレイトレース経路だけなので、
        // ラスタ経路では分類そのものを無効(0)にして従来どおりの挙動に保つ
        // (ラスタ経路のαは常に0なのでどのしきい値でも有効側に倒れるが、
        //  「分類は掛かっていない」ことを値として明示しておく)
        const float ddgiBackfaceThreshold =
            (m_DDGIProbeClassificationEnabled && ShouldRunRaytracedDDGITrace()) ? m_DDGIBackfaceThreshold : 0.0f;
        constants.DDGIParams4 = {
            effectiveExposure, ddgiHalfResolutionActive ? 1.0f : 0.0f,
            static_cast<float>(m_DDGILODCount), ddgiBackfaceThreshold
        };

        // クリップマップLODの各段の原点と、トロイダルaddressingの基準になる格子座標。
        // 使わない段も0で埋めておく(未初期化のまま渡すと、段数を増やした瞬間に
        // ゴミを読んで見当違いの場所からプローブを引く)
        static_assert(
            kFrameConstantsDDGILODCount == kDDGIMaxLODCount,
            "FrameConstantsのDDGILOD配列の要素数とkDDGIMaxLODCountを一致させること"
            "(ずれるとcbufferのレイアウトが静かに食い違う)");
        for (uint32_t lod = 0; lod < kDDGIMaxLODCount; ++lod)
        {
            if (m_HasGIVolume && lod < m_DDGILODCount)
            {
                const DirectX::XMFLOAT3 lodOrigin = ComputeDDGILODOrigin(lod);
                const DirectX::XMINT3 lodBase = ComputeDDGILODBaseIndex(lod);
                constants.DDGILODOrigin[lod] = { lodOrigin.x, lodOrigin.y, lodOrigin.z, 0.0f };
                constants.DDGILODBase[lod] = {
                    static_cast<float>(lodBase.x), static_cast<float>(lodBase.y),
                    static_cast<float>(lodBase.z), 0.0f
                };
            }
            else
            {
                constants.DDGILODOrigin[lod] = { 0.0f, 0.0f, 0.0f, 0.0f };
                constants.DDGILODBase[lod] = { 0.0f, 0.0f, 0.0f, 0.0f };
            }
        }
        // 水面。スクロール位相はRenderThreadMainがm_WaterTimeFrozen/m_WaterWaveSpeedに
        // 応じて毎フレーム進める(m_TimeOfDayの自動進行と同じ場所・同じ方式)。
        // y=波のスケール倍率(m_WaterWaveScale)、z=波の強さ(m_WaterWaveStrength、0〜1)を
        // Water.hlslへ渡す(UIのスライダーが見た目へ反映されるようにするため)
        constants.TimeParams = { m_WaterScrollOffset, m_WaterWaveScale, m_WaterWaveStrength, 0.0f };

        // 雲。DeferredLighting.hlsl(背景)とSSR.hlsl(水面反射)の両方が同じ値を読むため、
        // ここで一度だけ組み立てる。m_CloudEnabled=falseのときはCloudParams0.xへ0を渡し、
        // Sky.hlsli側のSkyColorが早期脱出する経路(判断C)を通す
        constants.CloudParams0 = {
            m_CloudEnabled ? m_CloudCoverage : 0.0f,
            m_CloudAltitude,
            m_CloudUvScale,
            m_CloudDensity,
        };
        // wには積雲の厚み[m]を詰めてある(FrameConstantsを増やさずに済ませるため)。
        // 0ならシェーダー側はレイマーチせず平面として扱う
        constants.CloudParams1 = {
            m_CloudScrollOffset.x, m_CloudScrollOffset.y, m_CloudForwardG,
            m_CloudVolumetric ? m_CloudThickness : 0.0f,
        };
        // 巻雲。積雲と同じ理由でここで一度だけ組み立てる。m_CirrusEnabled=falseのときは
        // CloudParams2.xへ0を渡し、Sky.hlsli側のSkyColorが早期脱出する経路(判断C)を通す
        constants.CloudParams2 = {
            m_CirrusEnabled ? m_CirrusCoverage : 0.0f,
            m_CirrusAltitude,
            m_CirrusUvScale,
            m_CirrusDensity,
        };
        constants.CloudParams3 = { m_CirrusScrollOffset.x, m_CirrusScrollOffset.y, m_CirrusAnisotropy, 0.0f };
        // 平面反射。このフィールドを参照するのはPlanarReflection.hlslだけで、そちらは
        // 専用のm_PlanarReflectionConstantBufferで明示的に上書きした値を使う(下のPlanarReflection
        // パス登録箇所参照)。共有のm_FrameConstantBufferにも一貫した値を入れておく
        constants.PlanarReflectionPlane = { 0.0f, 1.0f, 0.0f, hasWaterInstance ? -waterPlaneY : 0.0f };

        // 大気遠近。AerialPerspective.hlsl/PlanarReflection.hlslの両方が読む。
        // 手続き空が無効(.ksceneのDDSスカイボックス使用時)は、m_FogEnabledの値に関わらず
        // 常に無効化する――DDSは任意の絵でPerezモデルとは無関係なため、in-scatter項の
        // 解析評価(SkyColor)をしてはいけない(SSRパスのwaterAnalyticSkyFlagと同じ判断)
        const float fogEnabledFlag = (m_FogEnabled && m_FogDensity > 0.0f && usingProceduralSky) ? 1.0f : 0.0f;
        constants.FogParams0 = { m_FogDensity, m_FogScaleHeight, m_FogRefHeight, fogEnabledFlag };
        constants.FogParams1 = { m_FogMaxOpacity, 0.0f, 0.0f, 0.0f };
        // 水中項。Water.hlslのPSMainが読む
        constants.WaterBodyColor = { m_WaterBodyColor.x, m_WaterBodyColor.y, m_WaterBodyColor.z, 0.0f };

        // 星空。
        // 【昼は強度0にしてしまう】星は太陽が地平線下にあるときしか見えない。ここで0に
        // 落としておけば、Sky.hlsli側は最初のif文で抜けるので昼のシーンの絵は1画素も動かない
        // (m_StarsEnabledを切ったときとまったく同じ経路を通る)。
        // sunLighting.SunPositionは太陽が「ある」向きなので、yが負なら地平線下。
        // 仰角0度から-8度にかけて滑らかに立ち上げ、市民薄明のあいだに星が出そろう形にする
        const float sunElevationSin = sunLighting.SunPosition.y;
        const float starsNightFactor = std::clamp((-sunElevationSin - 0.005f) * 8.0f, 0.0f, 1.0f);
        // 手続き空を使わないシーン(DDSスカイボックス指定)ではSkyColorの解析評価自体を
        // 通らないため、フォグの有効フラグと同じ判断で0にしておく
        const float starsIntensity =
            (m_StarsEnabled && usingProceduralSky) ? (m_StarsBrightness * starsNightFactor) : 0.0f;
        // 1画素が張る角度[rad]。射影行列の_22 = 1/tan(fovY/2) から
        // 画面の高さ全体が 2*tan(fovY/2) なので、1画素あたりはそれを縦解像度で割ればよい。
        // 解像度やFOVを変えても星の見かけの下限が追従する
        DirectX::XMFLOAT4X4 projForPixelAngle;
        DirectX::XMStoreFloat4x4(&projForPixelAngle, jitteredProj);
        const float pixelAngle =
            (projForPixelAngle._22 > 0.0f && m_RenderHeight > 0)
                ? (2.0f / (projForPixelAngle._22 * static_cast<float>(m_RenderHeight)))
                : 0.001f;
        constants.StarsParams = { starsIntensity, m_StarsDensity, m_StarsTwinkle, pixelAngle };

        // 積雲のボリュームレイマーチの段数。シェーダー側でも上限へ丸めるが、
        // 0以下を渡すと「コンパイル時の既定を使う」の意味になってしまうため下限はここで効かせる
        constants.CloudQualityParams = {
            static_cast<float>(std::clamp(m_CloudRaymarchSteps, 1u, kCloudRaymarchStepsMax)),
            0.0f, 0.0f, 0.0f
        };

        // --- Hi-Zオクルージョンカリング(Stage 5-2)の判定パラメータ ---
        //
        // 判定に使うHi-Zは前フレームのもの(構築パスがG-Bufferパスより後に登録されるため)。
        // したがって「前フレームのHi-Zが実際に作られている」ことと「前フレームのビュー射影行列が
        // 本物である」ことの両方が要る。どちらかが欠けたフレームでは判定を丸ごと止める ――
        // 初回フレームや解像度変更の直後にここを通すと、未定義の深度で視界内をまとめて消す
        const bool occlusionCullEnabledThisFrame =
            occlusionCullingActive && m_HiZValid && m_TAAPrevViewProjValid;

        // 深度プリパスが走るなら、その深度からHi-Zを作れる。**そのフレームのG-Bufferは
        // 前フレームのHi-Zを待たなくてよい** ―― 上の2条件はどちらも要らなくなる。
        // 条件の意味と、プリパス自身が今フレームのHi-Zを使えない理由は、
        // 下の hiZFromDepthPrepass を定義している箇所のコメントにある
        const bool depthPrepassRuns = m_DepthPrepassEnabled
            && m_DepthPrepassPipelineState && m_DepthPrepassCutoutPipelineState;
        const bool hiZFromDepthPrepass =
            m_HiZFromDepthPrepassEnabled && occlusionCullingActive && depthPrepassRuns;

        // 前フレームからのカメラ移動距離。シーンが静的である以上、1フレームぶんの視差ずれの
        // 原因はカメラの移動だけなので、その距離をバウンディング球の半径へ足せば
        // 保守側(間引きすぎない側)へ倒せる。前フレームが無いフレームでは0でよい
        // (そのフレームは上のフラグで判定自体が止まっている)
        float cameraMoveDistance = 0.0f;
        if (m_TAAPrevViewProjValid)
        {
            const float dx = cameraPosition.x - m_PrevCameraPosition.x;
            const float dy = cameraPosition.y - m_PrevCameraPosition.y;
            const float dz = cameraPosition.z - m_PrevCameraPosition.z;
            cameraMoveDistance = std::sqrt(dx * dx + dy * dy + dz * dz);
        }

        // 【フレーム全体の「判定するか」はここ、「どのHi-Zで判定するか」はドローごと】
        // 深度プリパスから作る経路では、前フレームのHi-Zが無いフレームでも
        // G-Bufferは今フレームのHi-Zで判定できる。どちらの経路も無いときだけ全体を止める
        // (ドローごとの選択は ObjectConstants::MeshletOcclusionMode)
        constants.OcclusionCullParams = {
            (occlusionCullEnabledThisFrame || hiZFromDepthPrepass) ? 1.0f : 0.0f,
            m_OcclusionCullRadiusScale,
            cameraMoveDistance,
            static_cast<float>(m_HiZMipLevels),
        };
        // Hi-Zのミップ0はG-Buffer深度と同じ解像度で作られる(CreateRenderTargets)
        constants.HiZScreenParams = {
            static_cast<float>(m_RenderWidth),
            static_cast<float>(m_RenderHeight),
            1.0f / static_cast<float>(std::max(1u, m_RenderWidth)),
            1.0f / static_cast<float>(std::max(1u, m_RenderHeight)),
        };

        // bindless番号をfloatで運ぶ。番号はkBindlessDescriptorCapacity(8192)未満で、
        // float32が誤差なく表せる整数の範囲(2^24)に十分収まる
        constants.MeshletCullStatsParams = {
            meshletCullStatsActive ? 1.0f : 0.0f,
            meshletCullStatsActive ? static_cast<float>(m_MeshletCullStatsBindlessIndex) : 0.0f,
            0.0f,
            0.0f,
        };

        commandList->UpdateBuffer(m_FrameConstantBuffer.get(), &constants, sizeof(constants));

        // スクリーンスペースシャドウ(ScreenSpaceShadow.hlsli)が深度値からView空間Zを1除算で
        // 復元するための定数。Camera::GetProjectionMatrixの射影行列(行ベクトル規約)は
        // clip.z = viewZ * a + b、clip.w = viewZ なので depth = a + b / viewZ となり、
        // 逆に解いて viewZ = b / (depth - a)。近平面・遠平面から直接組み立てず射影行列の要素を
        // 読むのは、Reverse-Zの組み方が変わっても自動的に追従させるため
        // (XMFLOAT4X4の_rcは1始まりの行・列なので、_33が行2列2=a、_43が行3列2=b)。
        // ジッター済みの行列から読むが、TAAのジッターが書き換えるのは_31/_32だけなので
        // _33/_43の値そのものはジッターの有無で変わらない。それでもジッター済みを使うのは、
        // 「深度バッファに関わる計算はすべて深度を描いたときと同じ行列から導く」という
        // 不変条件を1箇所も破らないため(将来ジッターの入れ方を変えたときに黙ってずれない)
        DirectX::XMFLOAT4X4 projectionForDepthLinearize;
        DirectX::XMStoreFloat4x4(&projectionForDepthLinearize, jitteredProj);
        const float depthLinearizeA = projectionForDepthLinearize._33;
        const float depthLinearizeB = projectionForDepthLinearize._43;

        // 直接光パスのb1へ渡すスクリーンスペースシャドウのパラメータ。パスのラムダから
        // 値キャプチャできるようここで組み立てておく
        // 太陽の影の手法。RTシャドウを選んでいてもパスを実行できない状況(高速化構造が無い等)では
        // カスケードシャドウマップへ落とす。シャドウマップは手法によらず描いてあるため、
        // 落ちても影が消えることはない
        const ShadowMode effectiveShadowMode =
            (m_ShadowMode == ShadowMode::Raytraced && !ShouldRunRaytracedShadow())
                ? ShadowMode::CascadedShadowMap
                : m_ShadowMode;

        LightingConstants lightingConstants{};
        lightingConstants.LightCount =
        {
            static_cast<uint32_t>(gpuLights.size()),
            static_cast<uint32_t>(std::max(0, m_ScreenSpaceShadowMaxLightsPerPixel)),
            static_cast<uint32_t>(effectiveShadowMode),
            // MegaLightsが走るフレームは、ポイント/スポットの寄与をあちらが計算済みなので
            // 直接光パス側のライトループを止める。**「パスを積むか」と同じ述語で決めること** ――
            // ずれると二重加算(2倍明るい)か、ローカルライトが全部消えるかのどちらかになる
            ShouldRunMegaLights() ? 1u : 0u,
        };
        lightingConstants.SSSParams0 =
        {
            static_cast<float>(m_ScreenSpaceShadowStepCount),
            m_ScreenSpaceShadowMaxRayLength,
            m_ScreenSpaceShadowThickness,
            m_ScreenSpaceShadowEnabled ? 1.0f : 0.0f,
        };
        lightingConstants.SSSParams1 =
        {
            depthLinearizeA,
            depthLinearizeB,
            m_ScreenSpaceShadowNormalBias,
            m_ScreenSpaceShadowEdgeFade,
        };
        lightingConstants.TileParams =
        {
            m_LightTileCountX,
            kLightTileSize,
            kLightTileCapacity,
            m_LightCullingEnabled ? 1u : 0u,
        };

        // ライトリストの中身の更新。**直接光パスや半透明パスの中で呼んではいけない** ――
        // タイルライトカリングパスが両者より先にこのバッファを読むため、
        // グラフを組み立てる前に1箇所でまとめて済ませる(更新回数も1回で済む)。
        // 0灯のフレームでは更新自体を省略してよい(シェーダはライト数までしかループしないため)
        if (!gpuLights.empty())
        {
            commandList->UpdateBuffer(m_LightBuffer.get(), gpuLights.data(), gpuLights.size() * sizeof(GPULight));
        }

        // --- ドローンショーの機体を評価し、GPUへ送る ---
        // ライトリストとまったく同じ理由でグラフ構築の前に1回だけ更新する。このバッファは
        // 本描画パスと平面反射パスの2箇所から読まれるため、パスの中で更新すると
        // 先に走る側が未更新の内容を読んでしまう
        m_DroneInstances.clear();
        if (m_DroneShowEnabled)
        {
            m_DroneShow.Evaluate(m_DroneShowTime, m_DroneShowCenter, m_DroneShowScale, m_DroneInstances);
            // 【バッファの容量を超える機体は描かない】m_DroneBufferはkMaxDrones分を固定確保して
            // いるので、それを超えた分をUpdateBufferへ渡すと書き込みが範囲外になる。
            // .kshowの機体数はエディタ側で上限を掛けているが、外から来たファイルでも
            // 壊れないよう、GPUへ渡す直前のここで切り詰める
            if (m_DroneInstances.size() > kMaxDrones)
            {
                m_DroneInstances.resize(kMaxDrones);
            }
            if (!m_DroneInstances.empty())
            {
                commandList->UpdateBuffer(
                    m_DroneBuffer.get(), m_DroneInstances.data(), m_DroneInstances.size() * sizeof(GPUDrone));
            }
        }

        // 各パスをリソースの読み書き依存関係から自動的に順序付けて実行するレンダーグラフ。
        // トランジェントリソースの確保は行わず、既存の永続確保済みテクスチャ(G-Buffer・SceneColor等)を
        // そのまま読み書きする(詳細はRenderGraph.h参照)
        Core::RenderGraph graph(commandList, m_GPUProfiler.get(), &m_CPUProfiler);

        // --- 大気散乱のLUTのベイクパス ---
        //
        //     AtmosphereLUTBake: Transmittance → MultiScattering の順。MultiScatteringは
        //     TransmittanceをSRVで読むため順序が意味を持つ(BRDF積分LUTの2パス構成と同じ形)。
        //     どちらも大気パラメータだけの関数なので、濁りが変わったときだけ焼き直す。
        //
        //     SkyViewBake: 空そのもの。太陽が動くと変わるので毎フレーム焼く。
        //
        //     【この2つは必ずSkyIntegrateより前に「登録」すること】RenderGraphの依存解決は
        //     登録順に1回だけ舐める前方走査で、あるパスのReadsは**自分より前に登録された
        //     書き手**しか見つけられない(RenderGraph::ResolveExecutionOrderのlastWriter)。
        //     つまりグラフはパスを後ろへ遅らせることはできても前へ動かすことはできない。
        //     この2つをSkyIntegrateより後ろに置くと、SkyIntegrateが.Reads = { m_SkyViewLUT }を
        //     宣言していても辺が張られず、**未初期化のLUTを積分してしまう**。
        //     太陽が静止したシーンではSkyIntegrateは起動直後の1回しか走らないため、
        //     壊れた天頂輝度がそのまま最後まで残る(実測: 積分値が5.29ではなく1.58になり、
        //     空が3.3倍明るくなって青が白く飛んでいた)。
        //
        //     【定数バッファは3つのエントリポイント共通】濁りはMieの密度としてTransmittanceにも
        //     MultiScatteringにも効くため、AtmosphereConstantsを3者で共有している
        const float atmosphereMieDensityScale = ComputeAtmosphereMieDensityScale(m_SkyTurbidity);
        const auto updateAtmosphereConstants = [this, &sunLighting, atmosphereMieDensityScale]
            (RHI::IRHICommandList* cmd)
        {
            AtmosphereConstants atmosphereConstants{};
            atmosphereConstants.SunDirection = {
                sunLighting.SunPosition.x, sunLighting.SunPosition.y, sunLighting.SunPosition.z, 0.0f
            };
            atmosphereConstants.Params0 = { atmosphereMieDensityScale, 0.0f, 0.0f, 0.0f };
            cmd->UpdateBuffer(m_AtmosphereConstantBuffer.get(), &atmosphereConstants, sizeof(atmosphereConstants));
            cmd->SetComputeConstantBuffer(0, m_AtmosphereConstantBuffer.get());
        };

        if (m_AtmosphereLUTBakedTurbidity != m_SkyTurbidity &&
            m_TransmittancePipelineState && m_MultiScatteringPipelineState)
        {
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "AtmosphereLUTBake",
                .Writes = { m_TransmittanceLUT.get(), m_MultiScatteringLUT.get() },
                .Execute = [this, updateAtmosphereConstants](RHI::IRHICommandList* cmd)
                {
                    cmd->SetComputePipelineState(m_TransmittancePipelineState.get());
                    updateAtmosphereConstants(cmd);
                    cmd->SetComputeUnorderedAccessTexture(0, m_TransmittanceLUT.get(), 0);
                    cmd->Dispatch((kTransmittanceLUTWidth + 7) / 8, (kTransmittanceLUTHeight + 7) / 8, 1);

                    // UAVはDispatch直後に自動で解除されるため張り直す。
                    // ここでTransmittanceをSRV(t0)として読むので、上のDispatchより後でなければならない
                    cmd->SetComputePipelineState(m_MultiScatteringPipelineState.get());
                    updateAtmosphereConstants(cmd);
                    cmd->SetComputeTexture(0, m_TransmittanceLUT.get());
                    cmd->SetComputeSamplerSet(m_ScreenSpaceSamplers.get());
                    cmd->SetComputeUnorderedAccessTexture(0, m_MultiScatteringLUT.get(), 0);
                    const uint32_t groups = (kMultiScatteringLUTSize + 7) / 8;
                    cmd->Dispatch(groups, groups, 1);
                },
            });
            m_AtmosphereLUTBakedTurbidity = m_SkyTurbidity;
        }

        // SkyView LUTを焼き直すかどうか。CSSkyViewの入力は太陽の向きと濁りだけで、
        // 視点位置はkSkyViewHeightKm固定(カメラ非依存)なので、この2つが動かなければ
        // まったく同じ内容を焼き直すことになる。実測1.15〜1.53ms/フレームがまるごと無駄だった。
        // 濁りは上のAtmosphereLUTBakeとまったく同じ条件で判定するため、濁りが動いたフレームでは
        // Transmittance/MultiScatteringとSkyViewが同じフレームで焼き直され、実行順序は
        // Reads/Writesの依存からレンダーグラフが決める
        bool bakeSkyViewThisFrame = m_SkyViewBakedTurbidity != m_SkyTurbidity;
        if (!bakeSkyViewThisFrame)
        {
            const DirectX::XMVECTOR current = DirectX::XMLoadFloat3(&sunLighting.SunPosition);
            const DirectX::XMVECTOR baked = DirectX::XMLoadFloat3(&m_SkyViewBakedSunPosition);
            const float cosAngle = DirectX::XMVectorGetX(DirectX::XMVector3Dot(current, baked));
            bakeSkyViewThisFrame =
                cosAngle < std::cos(DirectX::XMConvertToRadians(kSkyViewRebakeAngleDegrees));
        }

        if (m_SkyViewPipelineState && bakeSkyViewThisFrame)
        {
            m_SkyViewBakedSunPosition = sunLighting.SunPosition;
            m_SkyViewBakedTurbidity = m_SkyTurbidity;
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "SkyViewBake",
                .Reads = { m_TransmittanceLUT.get(), m_MultiScatteringLUT.get() },
                .Writes = { m_SkyViewLUT.get() },
                .Execute = [this, updateAtmosphereConstants](RHI::IRHICommandList* cmd)
                {
                    cmd->SetComputePipelineState(m_SkyViewPipelineState.get());
                    updateAtmosphereConstants(cmd);
                    cmd->SetComputeTexture(0, m_TransmittanceLUT.get());
                    cmd->SetComputeTexture(1, m_MultiScatteringLUT.get());
                    cmd->SetComputeSamplerSet(m_ScreenSpaceSamplers.get());
                    cmd->SetComputeUnorderedAccessTexture(0, m_SkyViewLUT.get(), 0);
                    cmd->Dispatch((kSkyViewLUTWidth + 7) / 8, (kSkyViewLUTHeight + 7) / 8, 1);
                },
            });
        }

        // --- 空パラメータの積分パス: 色味の決定とθ64×φ256=16,384サンプルの照度正規化積分を
        //     GPUで行い、結果(ティント4本+正規化済みの天頂輝度)をm_SkyParametersBufferへ書く。
        //     **CPU側で計算してはいけない**(Sky.hlsli側の式と二重実装になる)。
        //     このバッファをSkyGenerate/DeferredLighting/SSRの3者が読むため、下のSkyGenerateパスより
        //     必ず先に実行する必要がある。実行条件はbakeSkyThisFrameではなくskyIntegrateThisFrame
        //     (手続き空が無効なシーンでも初回の1回だけは走らせ、未初期化状態を解消する。
        //     理由はm_SkyParametersBuffer作成箇所とskyIntegrateThisFrame宣言のコメント参照) ---
        if (skyIntegrateThisFrame)
        {
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "SkyIntegrate",
                // 日中の空はSkyView LUTを引くため、このパスもLUTを読む。
                // これによりレンダーグラフがSkyViewBakeより後へ自動で並べてくれる
                .Reads = { m_SkyViewLUT.get() },
                .BufferWrites = { m_SkyParametersBuffer.get() },
                .Execute = [this, &sunLighting, effectiveExposure](RHI::IRHICommandList* cmd)
                {
                    SkyIntegrateConstants integrateConstants{};
                    integrateConstants.SunDirection = {
                        sunLighting.SunPosition.x, sunLighting.SunPosition.y, sunLighting.SunPosition.z, 0.0f
                    };
                    integrateConstants.IntegrateParams = {
                        sunLighting.SkyIlluminanceLux, effectiveExposure, m_SkyTurbidity, m_SkySaturation
                    };
                    cmd->UpdateBuffer(m_SkyIntegrateConstantBuffer.get(), &integrateConstants, sizeof(integrateConstants));

                    cmd->SetComputePipelineState(m_SkyIntegratePipelineState.get());
                    cmd->SetComputeConstantBuffer(0, m_SkyIntegrateConstantBuffer.get());
                    // SkyView LUT(t0)とサンプラー(s1 ColorSampler)。日中の空はこのLUTから
                    // 引くため、積分側も同じLUTを読む必要がある
                    cmd->SetComputeTexture(0, m_SkyViewLUT.get());
                    cmd->SetComputeSamplerSet(m_ScreenSpaceSamplers.get());
                    cmd->SetComputeUnorderedAccessBuffer(0, m_SkyParametersBuffer.get());
                    // 1グループ×256スレッド固定(SkyIntegrate.hlsl参照)
                    cmd->Dispatch(1, 1, 1);
                },
            });
            m_SkyParametersBufferInitialized = true;
        }

        // --- 手続き空の生成パス: Perez分布をGPUで評価してキューブマップを焼く。
        //     太陽が動くと空の輝度分布の形も変わるため、オフラインDDSと違い焼き直しが要る
        //     (詳細はSkyGenerate.hlsl冒頭)。焼き直しの要否・雲の平均透過率のキャッシュ・
        //     m_SkyBakeDirty等のフラグ更新はすべて上のbakeSkyThisFrameブロックで済ませてあるため、
        //     ここではそのキャッシュ(m_ActiveCloudTransmittance)と、直前のSkyIntegrateパスが
        //     書いたm_SkyParametersBufferを使ってパスを登録するだけでよい ---
        if (bakeSkyThisFrame)
        {
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "SkyGenerate",
                // IBLキューブへ焼く空もSkyView LUT経由なので読み手に加わる
                .Reads = { m_SkyViewLUT.get() },
                .Writes = { m_ProceduralSkyTexture.get() },
                .BufferReads = { m_SkyParametersBuffer.get() },
                .Execute = [this, &sunLighting](RHI::IRHICommandList* cmd)
                {
                    cmd->SetComputePipelineState(m_SkyGeneratePipelineState.get());
                    cmd->SetComputeShaderResourceBuffer(0, m_SkyParametersBuffer.get());
                    // SkyView LUT(t1)とサンプラー(s1 ColorSampler)。**サンプラーのバインドを
                    // 外してはいけない** ―― LUTを線形補間で引くため、このパスにもサンプラーが要る
                    cmd->SetComputeTexture(1, m_SkyViewLUT.get());
                    cmd->SetComputeSamplerSet(m_MaterialSamplers.get());
                    for (uint32_t face = 0; face < kCubeFaceCount; ++face)
                    {
                        SkyBakeConstants skyConstants{};
                        skyConstants.Face = face;
                        // 雲(判断B)。SkyParametersBuffer側の天頂輝度は雲を考慮しない晴天の値の
                        // ままで、キューブへ焼く値にだけm_ActiveCloudTransmittance(被覆率が
                        // 変わらない限り1.0)を掛ける。理由はm_ActiveCloudTransmittanceの
                        // 代入元(上のbakeSkyThisFrameブロック)のコメント参照
                        skyConstants.CloudTransmittance = m_ActiveCloudTransmittance;
                        skyConstants.SunDirection = {
                            sunLighting.SunPosition.x, sunLighting.SunPosition.y, sunLighting.SunPosition.z, 0.0f
                        };
                        cmd->UpdateBuffer(m_SkyBakeConstantBuffer.get(), &skyConstants, sizeof(skyConstants));
                        cmd->SetComputeConstantBuffer(0, m_SkyBakeConstantBuffer.get());
                        cmd->SetComputeUnorderedAccessTextureCubeFace(0, m_ProceduralSkyTexture.get(), face, 0);
                        cmd->Dispatch((kProceduralSkySize + 7) / 8, (kProceduralSkySize + 7) / 8, 1);
                    }
                },
            });
        }

        // --- BRDF積分LUTのベイクパス: (NdotV, ラフネス)の2Dテーブルで、スカイボックスにも
        //     太陽の位置にも一切依存しないため起動後に一度だけ焼く。
        //     プリフィルタ済み鏡面(下記)が空の変化へ追従して焼き直されるようになっても、
        //     こちらが巻き込まれないよう別パス・別フラグに分離してある(m_BRDFLUTBaked参照) ---
        if (!m_BRDFLUTBaked)
        {
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "BRDFLUTBake",
                // 2パス構成のためスクラッチも書き込み対象として挙げる(RenderGraphが
                // パス内の依存を追えるように)。中身の説明はBRDFLUT.hlsl参照
                .Writes = { m_BRDFLUTTexture.get(), m_BRDFLUTScratchTexture.get() },
                .Execute = [this](RHI::IRHICommandList* cmd)
                {
                    // パス1: (A, B)をスクラッチへ焼く
                    cmd->SetComputePipelineState(m_BRDFLUTPipelineState.get());
                    cmd->SetComputeUnorderedAccessTexture(0, m_BRDFLUTScratchTexture.get(), 0);
                    cmd->Dispatch((kIBLBRDFLUTSize + 7) / 8, (kIBLBRDFLUTSize + 7) / 8, 1);

                    // パス2: スクラッチをSRVで読み、Eavgを足した float4(A, B, Eavg, 0) を最終LUTへ。
                    // UAVはDispatch直後に自動で解除されるため、ここで張り直す必要がある
                    // (IRHICommandList.hのバインド寿命の説明を参照)
                    cmd->SetComputePipelineState(m_BRDFLUTCombinePipelineState.get());
                    cmd->SetComputeTexture(0, m_BRDFLUTScratchTexture.get());
                    cmd->SetComputeUnorderedAccessTexture(0, m_BRDFLUTTexture.get(), 0);
                    cmd->Dispatch((kIBLBRDFLUTSize + 7) / 8, (kIBLBRDFLUTSize + 7) / 8, 1);
                },
            });
            m_BRDFLUTBaked = true;
        }

        // --- 雲の3Dノイズのベイクパス: 形状(128^3)とディテール(32^3)を起動後に一度だけ焼く。
        //     カメラにも太陽にも空の状態にも依存しない純粋な手続き生成なので、BRDF積分LUTと
        //     まったく同じ理由で焼き直さない。2枚は互いに独立なので1パスの中で連続して
        //     ディスパッチしてよい(SRVとして読み合う関係が無く、BRDFLUTの2パス構成のような
        //     中間バッファも要らない) ---
        if (!m_CloudNoiseBaked && m_CloudShapeNoisePipelineState && m_CloudDetailNoisePipelineState)
        {
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "CloudNoiseBake",
                .Writes = { m_CloudShapeNoiseTexture.get(), m_CloudDetailNoiseTexture.get() },
                .Execute = [this](RHI::IRHICommandList* cmd)
                {
                    // スレッドグループは4x4x4。3次元なのでグループあたり64スレッドで、
                    // 2次元パスの8x8(=64)と同じ粒度になる
                    constexpr uint32_t kGroupSize = 4;

                    cmd->SetComputePipelineState(m_CloudShapeNoisePipelineState.get());
                    cmd->SetComputeUnorderedAccessTexture(0, m_CloudShapeNoiseTexture.get(), 0);
                    const uint32_t shapeGroups = (kCloudShapeNoiseSize + kGroupSize - 1) / kGroupSize;
                    cmd->Dispatch(shapeGroups, shapeGroups, shapeGroups);

                    // UAVはDispatch直後に自動で解除されるため張り直す
                    // (IRHICommandList.hのバインド寿命の説明を参照)
                    cmd->SetComputePipelineState(m_CloudDetailNoisePipelineState.get());
                    cmd->SetComputeUnorderedAccessTexture(0, m_CloudDetailNoiseTexture.get(), 0);
                    const uint32_t detailGroups = (kCloudDetailNoiseSize + kGroupSize - 1) / kGroupSize;
                    cmd->Dispatch(detailGroups, detailGroups, detailGroups);
                },
            });
            m_CloudNoiseBaked = true;
        }

        // --- プリフィルタ済み鏡面の畳み込みパス: スカイボックスを入力に、ミップごとに異なる
        //     ラフネスで畳み込む(面×ミップの組み合わせごとに1回ずつディスパッチ) ---
        if (!m_IBLBaked)
        {
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "IBLPrefilter",
                .Reads = { skyTexture },
                .Writes = { m_PrefilteredEnvTexture.get() },
                .Execute = [this, skyTexture](RHI::IRHICommandList* cmd)
                {
                    cmd->SetComputePipelineState(m_PrefilterPipelineState.get());
                    cmd->SetComputeTexture(0, skyTexture);
                    cmd->SetComputeSamplerSet(m_MaterialSamplers.get());
                    for (uint32_t mip = 0; mip < kIBLPrefilterMipLevels; ++mip)
                    {
                        const uint32_t mipSize = std::max(1u, kIBLPrefilterBaseSize >> mip);
                        const float roughness = static_cast<float>(mip) / static_cast<float>(kIBLPrefilterMipLevels - 1);
                        for (uint32_t face = 0; face < kCubeFaceCount; ++face)
                        {
                            IBLFaceConstants faceConstants{};
                            faceConstants.Face = face;
                            faceConstants.Roughness = roughness;
                            cmd->UpdateBuffer(m_IBLPrefilterConstantBuffer.get(), &faceConstants, sizeof(faceConstants));
                            cmd->SetComputeConstantBuffer(0, m_IBLPrefilterConstantBuffer.get());
                            cmd->SetComputeUnorderedAccessTextureCubeFace(0, m_PrefilteredEnvTexture.get(), face, mip);
                            cmd->Dispatch((mipSize + 7) / 8, (mipSize + 7) / 8, 1);
                        }
                    }
                },
            });
            m_IBLBaked = true;
        }

        // --- 専用の拡散イラディアンスマップ(検証用に残している経路) ---
        // 既定の描画経路はプリフィルタ済み鏡面の最終ミップ(roughness=1)である。CSPrefilterは
        // V=R=Nを仮定しているためroughness=1のGGXはコサイン畳み込みへ厳密に退化し、専用マップと
        // 同じE(N)/πを与える(14.10節。White Furnace Testで画素一致を確認済み)。そのため通常の
        // 描画では1テクセルあたり約15,876サンプル(全体で約9,750万サンプル)のCSIrradianceを
        // 一切実行しない。この畳み込み処理自体はいつでも検証できるよう残してあり、ImGuiの
        // 「Use Dedicated Irradiance Map」トグルか、Render Targetsでイラディアンス表示を選んだ
        // ときだけ焼く。RenderGraphがReads/Writesから順序付けるため、トグルを入れたその同じ
        // フレームでLightingパスより先に実行される
        const bool needIrradianceBake =
            m_IBLUseDedicatedIrradiance || m_DebugView == DebugView::IBLIrradiance;
        if (needIrradianceBake && !m_IBLIrradianceBaked)
        {
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "IBLIrradianceBake",
                .Reads = { skyTexture },
                .Writes = { m_IrradianceTexture.get() },
                .Execute = [this, skyTexture](RHI::IRHICommandList* cmd)
                {
                    // 拡散イラディアンス(本物のTextureCube、32x32x6面)。HLSLはリソースを動的に
                    // スライス選択できないため、面ごとに1回ずつディスパッチする。
                    //
                    // m_IBLUseSHIrradianceでCSIrradiance(総当たり積分、約9,750万
                    // サンプル)とSH L2経路(CSProjectSH→CSProjectSHFinal→CSEvaluateSH、
                    // 射影は24,576テクセルを1回ずつ読むだけ)を切り替えられる。
                    // 出力(m_IrradianceTexture)の形・規約はどちらの経路でも完全に同一
                    if (m_IBLUseSHIrradiance)
                    {
                        IBLFaceConstants shConstants{};
                        shConstants.SHProjectionSize = static_cast<float>(kSHProjectionSize);
                        shConstants.SHWindowLambda = m_SHWindowLambda;

                        // --- 1. 射影: ソースキューブ全体を1回だけ読んで9個の係数(RGB)へ集約する ---
                        cmd->UpdateBuffer(m_IBLPrefilterConstantBuffer.get(), &shConstants, sizeof(shConstants));
                        cmd->SetComputeConstantBuffer(0, m_IBLPrefilterConstantBuffer.get());
                        cmd->SetComputePipelineState(m_ProjectSHPipelineState.get());
                        cmd->SetComputeTexture(0, skyTexture);
                        cmd->SetComputeSamplerSet(m_MaterialSamplers.get());
                        cmd->SetComputeUnorderedAccessBuffer(0, m_SHPartialSumsBuffer.get());
                        const uint32_t groupsPerSide = (kSHProjectionSize + 7) / 8;
                        cmd->Dispatch(groupsPerSide, groupsPerSide, kCubeFaceCount);

                        // --- 2. 最終合算: 全グループぶんの部分和を1ディスパッチでまとめる ---
                        // (SHProjectionSizeはCSProjectSHと同じ値でなければグループ番号の対応がずれる)
                        cmd->UpdateBuffer(m_IBLPrefilterConstantBuffer.get(), &shConstants, sizeof(shConstants));
                        cmd->SetComputeConstantBuffer(0, m_IBLPrefilterConstantBuffer.get());
                        cmd->SetComputePipelineState(m_ProjectSHFinalPipelineState.get());
                        cmd->SetComputeShaderResourceBuffer(1, m_SHPartialSumsBuffer.get());
                        cmd->SetComputeUnorderedAccessBuffer(0, m_SHCoefficientsBuffer.get());
                        cmd->Dispatch(1, 1, 1);

                        // --- 3. 評価: 9個の係数から出力テクセルごとのirradianceを求める ---
                        cmd->SetComputePipelineState(m_EvaluateSHPipelineState.get());
                        cmd->SetComputeShaderResourceBuffer(1, m_SHCoefficientsBuffer.get());
                        for (uint32_t face = 0; face < kCubeFaceCount; ++face)
                        {
                            IBLFaceConstants faceConstants{};
                            faceConstants.Face = face;
                            faceConstants.SHWindowLambda = m_SHWindowLambda;
                            cmd->UpdateBuffer(m_IBLPrefilterConstantBuffer.get(), &faceConstants, sizeof(faceConstants));
                            cmd->SetComputeConstantBuffer(0, m_IBLPrefilterConstantBuffer.get());
                            cmd->SetComputeUnorderedAccessTextureCubeFace(0, m_IrradianceTexture.get(), face, 0);
                            cmd->Dispatch((kIBLIrradianceSize + 7) / 8, (kIBLIrradianceSize + 7) / 8, 1);
                        }
                    }
                    else
                    {
                        cmd->SetComputePipelineState(m_IrradiancePipelineState.get());
                        cmd->SetComputeTexture(0, skyTexture);
                        cmd->SetComputeSamplerSet(m_MaterialSamplers.get());
                        for (uint32_t face = 0; face < kCubeFaceCount; ++face)
                        {
                            IBLFaceConstants faceConstants{};
                            faceConstants.Face = face;
                            cmd->UpdateBuffer(m_IBLPrefilterConstantBuffer.get(), &faceConstants, sizeof(faceConstants));
                            cmd->SetComputeConstantBuffer(0, m_IBLPrefilterConstantBuffer.get());
                            cmd->SetComputeUnorderedAccessTextureCubeFace(0, m_IrradianceTexture.get(), face, 0);
                            cmd->Dispatch((kIBLIrradianceSize + 7) / 8, (kIBLIrradianceSize + 7) / 8, 1);
                        }
                    }
                },
            });
            m_IBLIrradianceBaked = true;
            Core::Logger::Info("KurenaiEngine3D", "検証用の拡散イラディアンスマップを焼きました(通常の描画経路では使用しません)");
        }

        RHI::Viewport shadowViewport;
        shadowViewport.Width = static_cast<float>(kShadowMapSize);
        shadowViewport.Height = static_cast<float>(kShadowMapSize);

        RHI::Viewport gbufferViewport;
        gbufferViewport.Width = static_cast<float>(m_RenderWidth);
        gbufferViewport.Height = static_cast<float>(m_RenderHeight);

        // --- シャドウパス: ライト視点から深度のみを描画する(常に固定のシャドウマップ解像度)。
        //     カスケードごとに1回ずつ、同じメッシュ群を異なるライト正射影で描き直す ---
        for (uint32_t cascade = 0; cascade < kCascadeCount; ++cascade)
        {
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "Shadow" + std::to_string(cascade),
                .DepthTarget = m_ShadowCascadeArray.get(),
                .DepthTargetArraySlice = cascade,
                .Execute = [this, &shadowViewport, cascade, &cascadeViewProj](RHI::IRHICommandList* cmd)
                {
                    cmd->SetViewport(shadowViewport);
                    // 深度1.0(最遠)にクリアしておく。無効時はこの後の描画をスキップするため、
                    // シェーダー側は深度比較で常に「影なし」と判定する(ComputeShadowFactor参照)
                    cmd->ClearDepth(1.0f);

                    // RTシャドウ選択時もここは描く。半透明(Transparent.hlsl)と反射プローブの
                    // キャプチャ(ProbeCapture.hlsl)はカメラ視点の可視率テクスチャを使えず、
                    // カスケードシャドウマップを必要とするため(26章)
                    if (m_ShadowMode != ShadowMode::Off)
                    {
                        CascadeConstants cascadeConstants{};
                        DirectX::XMStoreFloat4x4(&cascadeConstants.ViewProj, DirectX::XMMatrixTranspose(cascadeViewProj[cascade]));
                        cmd->UpdateBuffer(m_ShadowCascadeConstantBuffer.get(), &cascadeConstants, sizeof(cascadeConstants));

                        cmd->SetPipelineState(m_ShadowPipelineState.get());
                        cmd->SetConstantBuffer(0, m_ShadowCascadeConstantBuffer.get());

                        // ミラーリングされたインスタンスは表裏が入れ替わるため、GBufferパスと同じく
                        // 表裏判定を反転したパイプラインへ切り替える(切り替え時はb0も張り直す)
                        RHI::IRHIPipelineState* currentPipelineState = m_ShadowPipelineState.get();
                        const auto bindShadowPipelineState = [&](RHI::IRHIPipelineState* wanted)
                        {
                            if (!wanted || wanted == currentPipelineState)
                            {
                                return;
                            }
                            cmd->SetPipelineState(wanted);
                            cmd->SetConstantBuffer(0, m_ShadowCascadeConstantBuffer.get());
                            // カットアウトのピクセルシェーダーがベースカラーを引くためサンプラーが要る。
                            // 不透明用のPSOはピクセルシェーダーを持たないので無害
                            cmd->SetSamplerSet(m_MaterialSamplers.get());
                            currentPipelineState = wanted;
                        };
                        // アルファカットアウトのマテリアルは切り抜きを反映して深度を書く。
                        // PSOが作れていない場合は従来どおり切り抜きを見ない(影が板のままになる)
                        const auto selectShadowPipelineState = [&](bool mirrored, bool cutout)
                        {
                            if (cutout && m_ShadowCutoutPipelineState)
                            {
                                return mirrored ? m_ShadowCutoutPipelineStateMirrored.get()
                                                : m_ShadowCutoutPipelineState.get();
                            }
                            return mirrored ? m_ShadowPipelineStateMirrored.get() : m_ShadowPipelineState.get();
                        };

                        // このカスケードのライト正射影に対して視錐台カリングする。
                        // カメラではなくライト側の錐台なので、画面外でも影を落とすものは残る
                        const FrustumPlanes cascadeFrustum = ExtractFrustumPlanes(cascadeViewProj[cascade]);

                        // インスタンシングのバッチと、まとめられなかった1体を同じ形で回す。
                        // シャドウは常に最も粗い段なので、その組(coarsestLOD=true)を使う
                        GetInstanceDrawUnits(/*coarsestLOD=*/true, m_DrawUnitScratch);
                        for (const InstanceDrawUnit& unit : m_DrawUnitScratch)
                        {
                            const Assets::ModelInstance& instance = *unit.Instance;
                            ++m_FrustumCullTested;
                            if (!IsAABBVisible(cascadeFrustum, unit.WorldBoundsMin, unit.WorldBoundsMax))
                            {
                                ++m_FrustumCullCulled;
                                continue;
                            }

                            // 【影は常に最も粗い段】影はテクスチャを読まないので詳細な段を描く
                            // 意味が無い。ストリーミング中で未読み込みなら描かない。
                            // バッチはどの段を描くかを既に決めてある(全員が同じ段であることが
                            // バッチの条件そのもの)
                            const Assets::Model* const coarsestModel =
                                unit.Model ? unit.Model : GetCoarsestLOD(instance);
                            if (!coarsestModel) { continue; }

                            // G-Bufferが1ドローで描くモデルは、シャドウも1ドローで描く。
                            //
                            // 【半透明は落とさない】このパスは従来から、BLENDのメッシュも
                            // 実体のまま影を落としている。ここでふるい分けると影の出方が変わって
                            // しまうため、意図的に何も落とさない(カットアウトの切り抜きだけは
                            // 下で反映する ―― そちらは板ポリゴンの影が出る明確な不具合だった)。
                            //
                            // 【カットアウトを持つモデルだけ2回に分ける】不透明ぶんは
                            // ピクセルシェーダーを持たないPSOで描きたいので、
                            // 切り抜きが要るぶんとは同じドローにまとめられない
                            if (m_ShadowMeshletPipelineState && ShouldUseModelMeshletPath(instance, *coarsestModel))
                            {
                                constexpr uint32_t kAmplificationGroupSize = 32;
                                const uint32_t groupCount =
                                    (coarsestModel->TotalMeshletCount + kAmplificationGroupSize - 1)
                                    / kAmplificationGroupSize;

                                const auto dispatchShadowMeshlets =
                                    [&](RHI::IRHIPipelineState* pipelineState, uint32_t rejectMask,
                                        uint32_t requireMask)
                                {
                                    if (!pipelineState)
                                    {
                                        return;
                                    }
                                    bindShadowPipelineState(pipelineState);

                                    const ObjectConstants objectConstants = MakeModelObjectConstants(
                                        instance, *coarsestModel, m_EmissiveIntensity, m_OcclusionMapEnabled, rejectMask,
                                        requireMask, m_MeshletLODFrame);
                                    cmd->UpdateBuffer(
                                        m_ObjectConstantBuffer.get(), &objectConstants, sizeof(objectConstants));
                                    cmd->SetConstantBuffer(1, m_ObjectConstantBuffer.get());
                                    cmd->DispatchMesh(groupCount, 1, 1);
                                    ++m_DrawCallsShadow;
                                };

                                // カットアウト用のPSOが作れていない場合は、従来どおり
                                // 切り抜きを見ずに全部を1回で描く(影が板のままになる)
                                const bool splitCutout =
                                    coarsestModel->HasCutoutMaterial && m_ShadowMeshletCutoutPipelineState;

                                dispatchShadowMeshlets(
                                    instance.IsMirrored ? m_ShadowMeshletPipelineStateMirrored.get()
                                                        : m_ShadowMeshletPipelineState.get(),
                                    splitCutout ? Assets::kGpuMaterialFlagCutout : 0u, 0u);

                                if (splitCutout)
                                {
                                    dispatchShadowMeshlets(
                                        instance.IsMirrored ? m_ShadowMeshletCutoutPipelineStateMirrored.get()
                                                            : m_ShadowMeshletCutoutPipelineState.get(),
                                        0u, Assets::kGpuMaterialFlagCutout);
                                }
                                continue;
                            }

                            for (const auto& mesh : coarsestModel->Meshes)
                            {
                                // メッシュ単位のカリング。錐台はこのカスケードのライト正射影で、
                                // カスケードごとに4回走る(=統計もカスケードぶん積み上がる)。
                                //
                                // 【バッチでは行わない】メッシュ単位のワールドAABBは
                                // 「インスタンス×メッシュ」の値で、まとめた相手のぶんが無い。
                                // 判定を代表インスタンスだけで行うと、他の個体の見えている
                                // メッシュまで落ちて物が消える
                                if (!unit.IsBatch()
                                    && !IsMeshVisibleWithStats(
                                        m_MeshCullingEnabled, cascadeFrustum, instance, *coarsestModel, mesh, m_MeshCullTested,
                                        m_MeshCullCulled))
                                {
                                    continue;
                                }
                                // アルファカットアウトは切り抜きを反映して深度を書く。
                                // 見ないままだと、葉や柵のようにテクスチャで抜く前提の
                                // マテリアルが板ポリゴンのまま影を落とす
                                const bool cutout = mesh.AlphaCutoff > 0.0f;
                                bindShadowPipelineState(selectShadowPipelineState(instance.IsMirrored, cutout));

                                // シャドウパスはWorld以外を使わないが、GBufferパスと同じルートシグネチャ/
                                // 定数バッファ(b1)を共有しているため必ずバインドする必要がある
                                ObjectConstants objectConstants =
                                    MakeObjectConstants(instance, *coarsestModel, mesh, m_EmissiveIntensity, m_OcclusionMapEnabled, m_MeshletLODFrame);
                                objectConstants.InstanceBase = unit.InstanceBase;
                                objectConstants.InstancingEnabled = unit.IsBatch() ? 1u : 0u;
                                cmd->UpdateBuffer(m_ObjectConstantBuffer.get(), &objectConstants, sizeof(objectConstants));
                                cmd->SetConstantBuffer(1, m_ObjectConstantBuffer.get());

                                if (cutout)
                                {
                                    cmd->SetTexture(0, mesh.BaseColorTexture);
                                }

                                // 【毎回張り直す】頂点シェーダー用SRVはt0の1本しかなく、
                                // ドローンショーが同じスロットを使う。上書きされたまま描くと
                                // 全インスタンスがドローンの座標を行列として読んで画面外へ飛ぶ
                                if (unit.IsBatch())
                                {
                                    cmd->SetVertexShaderResourceBuffer(0, m_ModelInstanceBuffer.get());
                                }

                                cmd->SetVertexBuffer(mesh.VertexBuffer.get());
                                cmd->SetIndexBuffer(mesh.IndexBuffer.get());
                                cmd->DrawIndexed(mesh.IndexCount, 0, 0, unit.InstanceCount);
                                ++m_DrawCallsShadow;
                            }
                        }
                    }
                },
            });
        }

        // --- 反射プローブの更新(19章・19.10節) ---
        // 更新モードに応じて「フルベイク(全プローブの全面を1フレームで焼く)」か
        // 「時間分割(1フレームに1面だけ焼く)」のどちらかを実行する。両者はスクラッチの
        // キューブマップ(m_ProbeRadianceCube)を共有するため、同じフレームで両方を走らせてはならない

        const DirectX::XMMATRIX probeFaceProjection =
            ComputeCubeFaceProjection(frameState.Camera.GetNearZ(), frameState.Camera.GetFarZ());

        // プローブ1面ぶんのキャプチャ(フォワード描画 → スクラッチのキューブ面へコピー)。
        // フルベイクと時間分割の両方から呼ぶためラムダへ切り出してある
        const auto captureProbeFace =
            [this, &constants, probeFaceProjection, skyTexture](RHI::IRHICommandList* cmd, size_t probeIndex, uint32_t face)
        {
            const Assets::ReflectionProbe& probe = m_ReflectionProbes[probeIndex];
            const DirectX::XMFLOAT3 probePosition{ probe.Position[0], probe.Position[1], probe.Position[2] };

            RHI::Viewport probeViewport;
            probeViewport.Width = static_cast<float>(kProbeCaptureSize);
            probeViewport.Height = static_cast<float>(kProbeCaptureSize);
            // 2枚目は距離(19.12節)。ProbeCapture.hlslのPSOutputと並びを一致させること
            RHI::IRHITexture* const captureTargets[] = { m_ProbeCaptureColor.get(), m_ProbeCaptureDistance.get() };

            // 太陽・カスケード・ライト数・IBL設定は共有のFrameConstantsをそのまま使い、
            // 視点に関わる2つだけをプローブのものへ差し替える(ProbeCapture.hlsl冒頭参照)。
            // Viewはカメラのまま残す(カスケード選択の深度がカメラ視錐台基準のため)
            FrameConstants captureConstants = constants;
            const DirectX::XMMATRIX faceViewProj = ComputeCubeFaceView(probePosition, face) * probeFaceProjection;
            DirectX::XMStoreFloat4x4(&captureConstants.ViewProj, DirectX::XMMatrixTranspose(faceViewProj));
            captureConstants.CameraPosition = { probePosition.x, probePosition.y, probePosition.z, 0.0f };
            // TAA関連のフィールドはカメラ視点のものが入ったままなので、プローブ視点として意味を成すよう
            // 明示的に潰しておく(前フレーム=今フレーム、ジッター無し=速度0)。ProbeCapture.hlslは
            // 現状これらを読まないが、将来読んだときに黙ってカメラの値を拾うのを防ぐため
            captureConstants.PrevViewProj = captureConstants.ViewProj;
            captureConstants.TAAParams = { 0.0f, 0.0f, 0.0f, 0.0f };
            // Hi-Zオクルージョンカリングも潰す。Hi-Zはメインカメラ視点の深度で、
            // プローブ視点から見える範囲とは何の関係も無い。上でPrevViewProjを
            // 「前フレーム」でない値へ差し替えている以上、判定の前提そのものが崩れている
            captureConstants.OcclusionCullParams = { 0.0f, 0.0f, 0.0f, 0.0f };
            // 統計も止める。プローブ視点で数えた分がメインカメラの間引き率に混ざると、
            // 「1フレームあたりの判定数」がプローブを焼いたフレームだけ跳ね上がって読めなくなる
            captureConstants.MeshletCullStatsParams = { 0.0f, 0.0f, 0.0f, 0.0f };
            cmd->UpdateBuffer(m_ProbeCaptureConstantBuffer.get(), &captureConstants, sizeof(captureConstants));

            cmd->SetRenderTargets(captureTargets, 2, m_ProbeCaptureDepth.get());
            cmd->SetViewport(probeViewport);
            // 両方のレンダーターゲットが0でクリアされる。距離側の0は「ジオメトリ無し」を意味しないが、
            // コピー側は深度が書かれたかどうかで判定するためこれで問題ない
            cmd->ClearRenderTarget({ 0.0f, 0.0f, 0.0f, 0.0f });
            // Reverse-Zのため遠平面側(NDC z=0.0)にクリアする。コピー側はこの0を
            // 「何も描かれなかった=スカイ」の判定に使う
            cmd->ClearDepth(0.0f);

            cmd->SetPipelineState(m_ProbeCapturePipelineState.get());
            cmd->SetConstantBuffer(0, m_ProbeCaptureConstantBuffer.get());
            cmd->SetSamplerSet(m_MaterialSamplers.get());

            // メッシュによらず共通のバインドはループの外で1回だけ行う。テクスチャのバインドは
            // 上書きするまで維持される(IRHICommandList::SetTexture参照)。DX12もバインド状態の
            // シャドウコピーを持ち寿命がDX11と揃っているため、ここで先にバインドしたものが
            // ループ内の各Drawへ引き継がれる
            cmd->SetTexture(4, m_ShadowCascadeArray.get());
            cmd->SetShaderResourceBuffer(8, m_LightBuffer.get());
            cmd->SetTexture(9, m_IrradianceTexture.get());
            cmd->SetTexture(10, m_PrefilteredEnvTexture.get());
            cmd->SetTexture(11, m_BRDFLUTTexture.get());
            // DDGI(22章)の多重バウンス。ProbeCapture.hlslは拡散の環境光をここから引く。
            // 参照するのは「前フレームまでに焼けているアトラス」で、同じフレームの中でも
            // 既に更新済みのプローブぶんは新しい値になる。DDGIは元々ヒステリシスで
            // 時間収束させる手法なので、この程度の混在は問題にならない
            cmd->SetTexture(12, m_DDGIIrradianceAtlas.get());
            cmd->SetTexture(13, m_DDGIDistanceAtlas.get());

            // このキューブ面の錐台で間引く。6面それぞれで判定するので、どこかの面には入る
            // モデルが全部消えることはない
            const FrustumPlanes faceFrustum = ExtractFrustumPlanes(faceViewProj);

            // インスタンシングのバッチと、まとめられなかった1体を同じ形で回す。
            // プローブは常に最も粗い段なので、シャドウと同じ組(coarsestLOD=true)を使う
            GetInstanceDrawUnits(/*coarsestLOD=*/true, m_DrawUnitScratch);
            for (const InstanceDrawUnit& unit : m_DrawUnitScratch)
            {
                const Assets::ModelInstance& instance = *unit.Instance;
                ++m_FrustumCullTested;
                if (!IsAABBVisible(faceFrustum, unit.WorldBoundsMin, unit.WorldBoundsMax))
                {
                    ++m_FrustumCullCulled;
                    continue;
                }

                // 【プローブも最も粗い段】焼き込むのは間接光で、細部は残らない
                // ストリーミング中で未読み込みなら描かない
                const Assets::Model* const coarsestModel =
                    unit.Model ? unit.Model : GetCoarsestLOD(instance);
                if (!coarsestModel) { continue; }
                for (const auto& mesh : coarsestModel->Meshes)
                {
                    // 半透明メッシュはプローブへ焼かない。ProbeCapture.hlslは不透明として描くため、
                    // ガラスを焼き込むと「向こう側が見えるはずの面」が不透明の壁としてキューブに
                    // 残り、その裏にある本来映るべき景色が欠ける。半透明を正しく焼くには
                    // キャプチャ側にも奥から手前への描画順とブレンドが要り、コストに見合わない
                    // (プローブへ半透明を含めないのは一般的な割り切り)
                    if (mesh.IsTransparent)
                    {
                        continue;
                    }

                    // メッシュ単位のカリング。錐台はキューブの1面ぶん。
                    // 【バッチでは行わない】理由はG-Bufferパスの同じ箇所を参照
                    if (!unit.IsBatch()
                        && !IsMeshVisibleWithStats(
                            m_MeshCullingEnabled, faceFrustum, instance, *coarsestModel, mesh, m_MeshCullTested, m_MeshCullCulled))
                    {
                        continue;
                    }

                    ObjectConstants objectConstants = MakeObjectConstants(instance, *coarsestModel, mesh, m_EmissiveIntensity, m_OcclusionMapEnabled, m_MeshletLODFrame);
                    objectConstants.InstanceBase = unit.InstanceBase;
                    objectConstants.InstancingEnabled = unit.IsBatch() ? 1u : 0u;
                    cmd->UpdateBuffer(m_ObjectConstantBuffer.get(), &objectConstants, sizeof(objectConstants));
                    cmd->SetConstantBuffer(1, m_ObjectConstantBuffer.get());

                    // 【毎回張り直す】頂点シェーダー用SRVはt0の1本しかない
                    if (unit.IsBatch())
                    {
                        cmd->SetVertexShaderResourceBuffer(0, m_ModelInstanceBuffer.get());
                    }

                    cmd->SetVertexBuffer(mesh.VertexBuffer.get());
                    cmd->SetIndexBuffer(mesh.IndexBuffer.get());

                    // メッシュごとに変わるマテリアルのテクスチャだけをここでバインドする
                    cmd->SetTexture(0, mesh.BaseColorTexture);
                    cmd->SetTexture(1, mesh.NormalTexture);
                    cmd->SetTexture(2, mesh.MetallicRoughnessTexture);
                    cmd->SetTexture(3, mesh.EmissiveTexture);
                    // t4はカスケードシャドウマップ配列が占めているため遮蔽マップはt5、
                    // bent normalはその次のt6(GBuffer.hlsl/ProbeCapture.hlslで共通)
                    cmd->SetTexture(5, mesh.OcclusionTexture);
                    cmd->SetTexture(6, mesh.BentNormalTexture);

                    cmd->DrawIndexed(mesh.IndexCount, 0, 0, unit.InstanceCount);
                }
            }

            // 描き終えたカラー/深度をコンピュートシェーダーからSRVとして読むため、
            // 先にレンダーターゲットのバインドを外す(D3D11は同一リソースの
            // RTV/DSVとSRVの同時バインドを許さず、SRV側がnullに落とされる)
            cmd->SetRenderTargets(nullptr, 0, nullptr);

            IBLFaceConstants faceConstants{};
            faceConstants.Face = face;
            cmd->SetComputePipelineState(m_ProbeCubeCopyPipelineState.get());
            cmd->UpdateBuffer(m_IBLPrefilterConstantBuffer.get(), &faceConstants, sizeof(faceConstants));
            cmd->SetComputeConstantBuffer(0, m_IBLPrefilterConstantBuffer.get());
            cmd->SetComputeSamplerSet(m_MaterialSamplers.get());
            // ジオメトリが描かれなかったテクセルを埋める空。手続き空が有効なフレームでは
            // そちらを使わないと、プローブにだけ古いDDSの空が焼き込まれて本編と食い違う
            // (このフレームで使う空はRender冒頭のskyTextureに確定させてある)
            cmd->SetComputeTexture(0, skyTexture);
            cmd->SetComputeTexture(1, m_ProbeCaptureColor.get());
            cmd->SetComputeTexture(2, m_ProbeCaptureDepth.get());
            cmd->SetComputeTexture(3, m_ProbeCaptureDistance.get());
            cmd->SetComputeUnorderedAccessTextureCubeFace(0, m_ProbeRadianceCube.get(), face, 0, 0);
            // 距離は畳み込まないため、スクラッチのキューブを経由せずプローブのスライスへ直接書く
            cmd->SetComputeUnorderedAccessTextureCubeFace(
                1, m_ProbeDistanceArray.get(), face, 0, static_cast<uint32_t>(probeIndex));
            cmd->Dispatch((kProbeCaptureSize + 7) / 8, (kProbeCaptureSize + 7) / 8, 1);
        };

        // 組み上がったスクラッチのキューブマップを、IBLとまったく同じ手順で畳み込んで
        // プローブのスライスへ書き込む。入力が違うだけでシェーダーはIBLBakeパスと共通
        // プローブのプリフィルタ済み鏡面の畳み込み。
        // 反射プローブは鏡面専任なので拡散イラディアンス側の畳み込みは持たない
        // (DDGIが拡散を担う。ReflectionProbe.hlsli冒頭のコメント参照)。
        // (mip, face)1組ぶんだけディスパッチする。SetComputePipelineState/SetComputeTexture/
        // SetComputeSamplerSetは呼び出し側が先に1回済ませておくこと(同じプローブの複数ステップを
        // 1パスにまとめて呼ぶ場合、毎回張り直す必要が無いため。Realtimeの時間分割参照)
        const auto convolveProbePrefilterStep =
            [this](RHI::IRHICommandList* cmd, size_t probeIndex, uint32_t mip, uint32_t face)
        {
            const uint32_t cubeIndex = static_cast<uint32_t>(probeIndex);
            const uint32_t mipSize = std::max(1u, kIBLPrefilterBaseSize >> mip);
            const float roughness = static_cast<float>(mip) / static_cast<float>(kIBLPrefilterMipLevels - 1);

            IBLFaceConstants faceConstants{};
            faceConstants.Face = face;
            faceConstants.Roughness = roughness;
            cmd->UpdateBuffer(m_IBLPrefilterConstantBuffer.get(), &faceConstants, sizeof(faceConstants));
            cmd->SetComputeConstantBuffer(0, m_IBLPrefilterConstantBuffer.get());
            cmd->SetComputeUnorderedAccessTextureCubeFace(0, m_ProbePrefilteredArray.get(), face, mip, cubeIndex);
            cmd->Dispatch((mipSize + 7) / 8, (mipSize + 7) / 8, 1);
        };

        // 6ミップ×6面ぶん全部を1回で焼く(フルベイク用。Realtimeの時間分割はconvolveProbePrefilterStepを
        // 直接、複数フレームに分けて呼ぶ。下のRealtimeブロック参照)
        const auto convolveProbePrefilter = [this, &convolveProbePrefilterStep](RHI::IRHICommandList* cmd, size_t probeIndex)
        {
            cmd->SetComputePipelineState(m_PrefilterPipelineState.get());
            cmd->SetComputeTexture(0, m_ProbeRadianceCube.get());
            cmd->SetComputeSamplerSet(m_MaterialSamplers.get());
            for (uint32_t mip = 0; mip < kIBLPrefilterMipLevels; ++mip)
            {
                for (uint32_t face = 0; face < kCubeFaceCount; ++face)
                {
                    convolveProbePrefilterStep(cmd, probeIndex, mip, face);
                }
            }
        };

        // キャプチャパスがReadsにシャドウマップとグローバルの畳み込み結果を挙げることで、
        // レンダーグラフがこれらをシャドウパス・IBLBakeパスより後ろへ順序付ける。
        // 空はm_SkyboxTextureではなくこのフレームで実際に使うskyTextureを挙げる。手続き空のときは
        // SkyGenerateパスがそれのWriterなので、これによりベイクが空の焼き直しより後ろへ順序付けられる
        const std::vector<RHI::IRHITexture*> probeCaptureReads = {
            m_ShadowCascadeArray.get(),
            skyTexture, m_IrradianceTexture.get(), m_PrefilteredEnvTexture.get(), m_BRDFLUTTexture.get(),
        };
        const size_t probeCount = m_ReflectionProbes.size();

        // OnDemandは、焼き上がりに影響する状態(時刻・太陽・ライト)が変わったフレームだけ焼き直す。
        // 一度も焼けていない間はシーン読み込み時の要求が既に立っているのでここでは何もしない
        if (m_ProbeUpdateMode == ProbeUpdateMode::OnDemand && probeCount > 0 && m_ProbeBaked &&
            ComputeProbeBakeSignature() != m_ProbeBakeSignature)
        {
            m_ProbeBakeRequested = true;
        }

        if (m_ProbeBakeRequested && probeCount > 0)
        {
            // --- フルベイク: 全プローブの6面を1フレームで焼く ---
            // プローブごとに、さらにキャプチャ/プリフィルタ畳み込みで別パスへ分けることで、
            // GPUプロファイラでそれぞれのコストを個別に読める(19.10節の実測)。
            // 各パスがm_ProbeRadianceCubeを読み書きするため、レンダーグラフのWrite-after-Write /
            // Read-after-Write依存で登録順に直列化される(スクラッチを共有しても取り違えは起きない)
            for (size_t probeIndex = 0; probeIndex < probeCount; ++probeIndex)
            {
                graph.AddPass(Core::RenderGraphPassDesc{
                    .Name = "ProbeBakeCapture" + std::to_string(probeIndex),
                    .Reads = probeCaptureReads,
                    .Writes = {
                        m_ProbeCaptureColor.get(), m_ProbeCaptureDistance.get(), m_ProbeCaptureDepth.get(),
                        m_ProbeRadianceCube.get(), m_ProbeDistanceArray.get(),
                    },
                    .Execute = [&captureProbeFace, probeIndex](RHI::IRHICommandList* cmd)
                    {
                        for (uint32_t face = 0; face < kCubeFaceCount; ++face)
                        {
                            captureProbeFace(cmd, probeIndex, face);
                        }
                    },
                });
                graph.AddPass(Core::RenderGraphPassDesc{
                    .Name = "ProbeBakeConvolvePrefilter" + std::to_string(probeIndex),
                    .Reads = { m_ProbeRadianceCube.get() },
                    .Writes = { m_ProbePrefilteredArray.get() },
                    .Execute = [&convolveProbePrefilter, probeIndex](RHI::IRHICommandList* cmd)
                    {
                        convolveProbePrefilter(cmd, probeIndex);
                    },
                });
            }

            m_ProbeBakeRequested = false;
            // このフレームの描画時点ではまだ焼き上がっていない(同じコマンドリスト内でこの後の
            // Lightingパスが読むのは問題ないが、gpuProbesは既に確定済み)。次フレームから
            // プローブが有効になるよう、ここでフラグだけ立てる
            m_ProbeBaked = true;
            m_ProbeBakeSignature = ComputeProbeBakeSignature();
            // このフレームの実効プリ露出で焼かれるので、読み出し側の換算倍率もここで更新する
            m_ProbeBakedExposureEV100 = m_EffectiveExposureEV100;
            // 全プローブが今焼けたので、時間分割は先頭から仕切り直す
            m_ProbeRealtimeProbeIndex = 0;
            m_ProbeRealtimeFace = 0;
            m_ProbeRealtimePrefilterStep = kProbePrefilterStepCount;
        }
        else if (m_ProbeUpdateMode == ProbeUpdateMode::Realtime && probeCount > 0 && m_ProbeBaked)
        {
            // --- 時間分割: キャプチャフェーズ(1フレーム1面、6フレーム)→ プリフィルタフェーズ
            //     (1フレームkProbeRealtimePrefilterStepsPerFrame個の(mip,face)、6フレーム)を
            //     交互に繰り返す。
            //
            // **プリフィルタを6面揃った瞬間に36ディスパッチまとめて発行してはいけない** ――
            // これが「6フレームに1回のスパイク」になる。1フレームあたり数ステップへ分割することで、
            // どのフレームもほぼ均等なコストになる。
            //
            // プリフィルタフェーズの間はキャプチャを止める(m_ProbeRadianceCubeがそのプローブの
            // ぶんのまま変わらないことを保証するため)。そのプローブのスライスは、旧キャプチャ→
            // 旧キューブ→新スライスの畳み込みが終わるまで前回の内容のまま表示され続ける
            // (描きかけの中間状態が映り込むことはない)
            if (m_ProbeRealtimeProbeIndex >= probeCount)
            {
                m_ProbeRealtimeProbeIndex = 0;
                m_ProbeRealtimeFace = 0;
                m_ProbeRealtimePrefilterStep = kProbePrefilterStepCount;
            }

            if (m_ProbeRealtimePrefilterStep < kProbePrefilterStepCount)
            {
                // --- プリフィルタフェーズ ---
                const size_t realtimeProbe = m_ProbeRealtimeProbeIndex;
                const uint32_t startStep = m_ProbeRealtimePrefilterStep;
                const uint32_t stepsThisFrame =
                    std::min(kProbeRealtimePrefilterStepsPerFrame, kProbePrefilterStepCount - startStep);

                graph.AddPass(Core::RenderGraphPassDesc{
                    .Name = "ProbeRealtimeConvolvePrefilterStep",
                    .Reads = { m_ProbeRadianceCube.get() },
                    .Writes = { m_ProbePrefilteredArray.get() },
                    .Execute = [this, &convolveProbePrefilterStep, realtimeProbe, startStep, stepsThisFrame](
                        RHI::IRHICommandList* cmd)
                    {
                        cmd->SetComputePipelineState(m_PrefilterPipelineState.get());
                        cmd->SetComputeTexture(0, m_ProbeRadianceCube.get());
                        cmd->SetComputeSamplerSet(m_MaterialSamplers.get());
                        for (uint32_t s = 0; s < stepsThisFrame; ++s)
                        {
                            const uint32_t step = startStep + s;
                            // 【ステップ番号→(面, ミップ)の割り当て】面を外側・ミップを内側にする。
                            // ミップの解像度は段ごとに1/4になるので、テクセル数は
                            //   ミップ0: 128² / 1:64² / 2:32² / 3:16² / 4:8² / 5:4²
                            // で、1面ぶん21,840テクセルのうちミップ0だけで16,384(75%)を占める。
                            //
                            // これを mip=step/6, face=step%6 と割り当てると「1フレーム目が
                            // ミップ0の6面をまとめて引き受ける」ことになり、畳み込み全体の75%が
                            // 1フレームへ集中する。個数は6ステップずつ均等でもコストは均等にならない
                            // (実測: この割り当てでは9.4msのスパイクが残っていた)。
                            //
                            // 面を外側にすると1フレーム = 1面ぶんの全ミップ = 21,840テクセルとなり、
                            // 6フレームすべてが厳密に同じ量になる。ピークは16,384+残り → 21,840、
                            // つまりミップ0の6面ぶんに対して約1/4.5になる。
                            // なお1フレームの下限は「ミップ0の1面」であり、これ以上細かくするには
                            // 1つの面をさらに矩形へ分割する必要がある(そこまではやっていない)
                            const uint32_t face = step / kIBLPrefilterMipLevels;
                            const uint32_t mip = step % kIBLPrefilterMipLevels;
                            convolveProbePrefilterStep(cmd, realtimeProbe, mip, face);
                        }
                    },
                });

                m_ProbeRealtimePrefilterStep = startStep + stepsThisFrame;
                if (m_ProbeRealtimePrefilterStep >= kProbePrefilterStepCount)
                {
                    // このプローブの畳み込みが完了。次のプローブのキャプチャへ進む
                    m_ProbeRealtimePrefilterStep = kProbePrefilterStepCount;
                    m_ProbeRealtimeProbeIndex = static_cast<uint32_t>((realtimeProbe + 1) % probeCount);
                    m_ProbeRealtimeFace = 0;
                }
            }
            else
            {
                // --- キャプチャフェーズ ---
                const size_t realtimeProbe = m_ProbeRealtimeProbeIndex;
                const uint32_t realtimeFace = m_ProbeRealtimeFace;

                graph.AddPass(Core::RenderGraphPassDesc{
                    .Name = "ProbeRealtimeCapture",
                    .Reads = probeCaptureReads,
                    .Writes = {
                        m_ProbeCaptureColor.get(), m_ProbeCaptureDistance.get(), m_ProbeCaptureDepth.get(),
                        m_ProbeRadianceCube.get(), m_ProbeDistanceArray.get(),
                    },
                    .Execute = [&captureProbeFace, realtimeProbe, realtimeFace](RHI::IRHICommandList* cmd)
                    {
                        captureProbeFace(cmd, realtimeProbe, realtimeFace);
                    },
                });

                m_ProbeRealtimeFace = realtimeFace + 1;
                if (m_ProbeRealtimeFace >= kCubeFaceCount)
                {
                    // 6面揃った。次フレームからこのプローブのプリフィルタフェーズへ入る
                    // (プローブ番号はプリフィルタが完了するまで進めない。上のプリフィルタフェーズ参照)
                    m_ProbeRealtimeFace = 0;
                    m_ProbeRealtimePrefilterStep = 0;
                }
            }

            // 常に焼き直しているのでOnDemandの署名も追随させておく。こうしておかないと
            // Realtimeから切り替えた直後に不要なフルベイクが1回走る
            m_ProbeBakeSignature = ComputeProbeBakeSignature();
            // 露出の換算倍率も追随させる。1ステップずつ焼くため厳密には面・ミップごとに焼いた
            // 露出が違うが、実効プリ露出の変化は毎秒2倍程度(m_EffectiveExposureAdaptSpeed)なので
            // 1周(最大12フレーム)ぶんのずれは数%にとどまり、常時焼き直している以上すぐ解消する
            m_ProbeBakedExposureEV100 = m_EffectiveExposureEV100;
        }

        // --- DDGIのプローブ更新(22章) ---
        // 反射プローブとまったく同じキャプチャ経路を使い、解像度だけkDDGICaptureSizeへ落とす。
        // 6面×16×16 = 1536テクセルがそのままDDGIの「1536本のレイ」になる。
        // フルベイクは持たず、初回も含めて常に1フレームm_DDGIProbesPerFrame個ずつ時間分割で回す
        // (理由はKurenaiEngine3D.hのm_DDGIWarmingUpのコメント参照)

        // プローブ1面ぶんのキャプチャ → スクラッチのキューブ2本(放射輝度・距離)の該当面へコピー。
        // コピーCSはIBLConvolve.hlslのCSCopyCaptureToCubeFaceをそのまま使う。u1の宣言が
        // RWTexture2DArray<float>なので、キューブ配列だけでなく単体のキューブ(=6要素の2D配列)の
        // 面へもそのまま書ける
        const auto captureDDGIProbeFace =
            [this, &constants, probeFaceProjection, skyTexture](RHI::IRHICommandList* cmd, uint32_t probeIndex, uint32_t face)
        {
            const DirectX::XMFLOAT3 probePosition = ComputeDDGIProbePosition(probeIndex);

            RHI::Viewport ddgiViewport;
            ddgiViewport.Width = static_cast<float>(kDDGICaptureSize);
            ddgiViewport.Height = static_cast<float>(kDDGICaptureSize);
            RHI::IRHITexture* const captureTargets[] = { m_DDGICaptureColor.get(), m_DDGICaptureDistance.get() };

            FrameConstants captureConstants = constants;
            const DirectX::XMMATRIX faceViewProj = ComputeCubeFaceView(probePosition, face) * probeFaceProjection;
            DirectX::XMStoreFloat4x4(&captureConstants.ViewProj, DirectX::XMMatrixTranspose(faceViewProj));
            captureConstants.CameraPosition = { probePosition.x, probePosition.y, probePosition.z, 0.0f };
            cmd->UpdateBuffer(m_ProbeCaptureConstantBuffer.get(), &captureConstants, sizeof(captureConstants));

            cmd->SetRenderTargets(captureTargets, 2, m_DDGICaptureDepth.get());
            cmd->SetViewport(ddgiViewport);
            cmd->ClearRenderTarget({ 0.0f, 0.0f, 0.0f, 0.0f });
            // Reverse-Zのため遠平面側(NDC z=0.0)。コピー側はこの0を「空」の判定に使う
            cmd->ClearDepth(0.0f);

            // PSOは反射プローブと共通(同じシェーダー・同じレンダーターゲットフォーマット)
            cmd->SetPipelineState(m_ProbeCapturePipelineState.get());
            cmd->SetConstantBuffer(0, m_ProbeCaptureConstantBuffer.get());
            cmd->SetSamplerSet(m_MaterialSamplers.get());

            cmd->SetTexture(4, m_ShadowCascadeArray.get());
            cmd->SetShaderResourceBuffer(8, m_LightBuffer.get());
            cmd->SetTexture(9, m_IrradianceTexture.get());
            cmd->SetTexture(10, m_PrefilteredEnvTexture.get());
            cmd->SetTexture(11, m_BRDFLUTTexture.get());
            // DDGI(22章)の多重バウンス。ProbeCapture.hlslは拡散の環境光をここから引く。
            // 参照するのは「前フレームまでに焼けているアトラス」で、同じフレームの中でも
            // 既に更新済みのプローブぶんは新しい値になる。DDGIは元々ヒステリシスで
            // 時間収束させる手法なので、この程度の混在は問題にならない
            cmd->SetTexture(12, m_DDGIIrradianceAtlas.get());
            cmd->SetTexture(13, m_DDGIDistanceAtlas.get());

            // 【ここにはフラスタムカリングを入れない】このループのドロー数は
            // ClampDDGIProbesPerFrameToConstantRingが「同じ条件で数えること」を前提に
            // 定数バッファリングの予算を決めている(同関数のコメント)。カリングは
            // プローブの位置ごとに結果が変わるため、予算計算と食い違う。
            // 定数バッファの予算超過は例外ではなくログ1行で続行し、描画が静かに壊れる
            // (DX12Buffer.h)ため、整合が取れるまでは入れないほうが安全
            // 【DDGIからだけ自発光を抜く】プロキシとして起こした発光は既にGPULight(型3)から
            // 入っているので、プローブが同じ面を「明るい面」として焼くと二重に数える。
            // **反射プローブでは抑止しない** ―― 同じProbeCapture.hlslを共有しているが、
            // 鏡面が光源を直接見ているのは二重計上ではなく、消すと看板が鏡に映らなくなる。
            // だから材質のフラグではなくCPUのパスごとに決めている
            const bool suppressEmissiveForDDGI = ShouldSuppressEmissiveForGI();
            uint32_t ddgiDrawnMeshes = 0;
            uint32_t ddgiEmissiveMeshes = 0;
            uint32_t ddgiSuppressedMeshes = 0;
            uint32_t ddgiLODMismatchMeshes = 0;
            for (size_t instanceIndex = 0; instanceIndex < m_Scene.Instances.size(); ++instanceIndex)
            {
                const Assets::ModelInstance& instance = m_Scene.Instances[instanceIndex];
                // 【DDGIも最も粗い段】理由は反射プローブと同じ
                // ストリーミング中で未読み込みなら描かない
                const Assets::Model* const coarsestModel = GetCoarsestLOD(instance);
                if (!coarsestModel) { continue; }
                // 【診断にだけ使う】抑止するかどうかの判定には入れないこと ――
                // レイトレ側(RaytracingMaterial::Flags)はインスタンスを見ないので、
                // ここだけ条件を増やすと2経路で判定がずれる軸が1本増える。
                // いまは「プロキシを作ったインスタンス」と「クラスタを持つメッシュ」が
                // 必ず一致するため冗長でもあるが、将来プロキシ生成に条件が入ったときに
                // 静かに乖離する形になる
                const bool instanceHasProxy =
                    instanceIndex < m_EmissiveProxyInstances.size() && m_EmissiveProxyInstances[instanceIndex];
                for (const auto& mesh : coarsestModel->Meshes)
                {
                    // 半透明メッシュを焼かない理由は反射プローブと同じ(不透明として描かれるため、
                    // ガラスが壁になって裏の景色が欠ける)
                    if (mesh.IsTransparent)
                    {
                        continue;
                    }

                    // 【シェーダーには手を入れない】倍率を0にすればEmissiveFactorごと0になる。
                    // 判定をクラスタの有無で行うのは、係数が0でないのにテクスチャの平均が
                    // 真っ黒でクラスタが出ないメッシュがあり、そちらは光源になっていないため
                    // 【レイトレ側とまったく同じ述語にする】あちらは
                    // RaytracingScene.cpp が !mesh.EmissiveClusters.empty() だけで印を付ける。
                    // 条件が1つでも違うと、環境によって二重計上の有無が変わる
                    const bool meshIsProxySource = suppressEmissiveForDDGI && !mesh.EmissiveClusters.empty();
                    const float ddgiEmissiveIntensity = meshIsProxySource ? 0.0f : m_EmissiveIntensity;
                    ++ddgiDrawnMeshes;
                    const float emissiveMax =
                        std::max({ mesh.EmissiveFactor[0], mesh.EmissiveFactor[1], mesh.EmissiveFactor[2] });
                    if (emissiveMax > 0.0f) { ++ddgiEmissiveMeshes; }
                    if (meshIsProxySource) { ++ddgiSuppressedMeshes; }
                    // 【2経路で判定がずれうる唯一の条件】レイトレ側の印は段0のクラスタで付くが、
                    // こちらが描くのは最も粗い段。粗い段でクラスタが消えている(簡略化で発光
                    // 三角形が落ちた等)と、レイトレは抑止するのにラスタは抑止せず二重に数える。
                    // **絵からは分からない**ので、条件に当たったことだけは残す
                    if (instanceHasProxy && emissiveMax > 0.0f && mesh.EmissiveClusters.empty())
                    {
                        ++ddgiLODMismatchMeshes;
                    }
                    const ObjectConstants objectConstants = MakeObjectConstants(instance, *coarsestModel, mesh, ddgiEmissiveIntensity, m_OcclusionMapEnabled, m_MeshletLODFrame);
                    cmd->UpdateBuffer(m_ObjectConstantBuffer.get(), &objectConstants, sizeof(objectConstants));
                    cmd->SetConstantBuffer(1, m_ObjectConstantBuffer.get());

                    cmd->SetVertexBuffer(mesh.VertexBuffer.get());
                    cmd->SetIndexBuffer(mesh.IndexBuffer.get());

                    cmd->SetTexture(0, mesh.BaseColorTexture);
                    cmd->SetTexture(1, mesh.NormalTexture);
                    cmd->SetTexture(2, mesh.MetallicRoughnessTexture);
                    cmd->SetTexture(3, mesh.EmissiveTexture);

                    cmd->DrawIndexed(mesh.IndexCount, 0, 0);
                }
            }

            if (!m_DDGIEmissiveSuppressLoggedRaster)
            {
                m_DDGIEmissiveSuppressLoggedRaster = true;
                Core::Logger::Info(
                    "KurenaiEngine3D",
                    std::string("DDGI(ラスタ)の自発光: 抑止 ") + (suppressEmissiveForDDGI ? "する" : "しない") +
                        " / 描いたメッシュ " + std::to_string(ddgiDrawnMeshes) + "個(うち自発光 " +
                        std::to_string(ddgiEmissiveMeshes) + "個) / 0にしたメッシュ " +
                        std::to_string(ddgiSuppressedMeshes) + "個 / 自発光の強度 " +
                        std::to_string(m_EmissiveIntensity));
                if (ddgiLODMismatchMeshes > 0)
                {
                    Core::Logger::Warning(
                        "KurenaiEngine3D",
                        "DDGI(ラスタ)で抑止できない自発光メッシュがあります: " +
                            std::to_string(ddgiLODMismatchMeshes) +
                            "個。粗いモデルLODでエミッシブのかたまりが消えているため、"
                            "レイトレ経路だけが抑止し、この経路では二重計上が残ります");
                }
            }

            // 描いたカラー/深度をコンピュートからSRVで読むため、先にRTVを外す(DX11の制約)
            cmd->SetRenderTargets(nullptr, 0, nullptr);

            IBLFaceConstants faceConstants{};
            faceConstants.Face = face;
            cmd->SetComputePipelineState(m_ProbeCubeCopyPipelineState.get());
            cmd->UpdateBuffer(m_IBLPrefilterConstantBuffer.get(), &faceConstants, sizeof(faceConstants));
            cmd->SetComputeConstantBuffer(0, m_IBLPrefilterConstantBuffer.get());
            cmd->SetComputeSamplerSet(m_MaterialSamplers.get());
            cmd->SetComputeTexture(0, skyTexture);
            cmd->SetComputeTexture(1, m_DDGICaptureColor.get());
            cmd->SetComputeTexture(2, m_DDGICaptureDepth.get());
            cmd->SetComputeTexture(3, m_DDGICaptureDistance.get());
            cmd->SetComputeUnorderedAccessTextureCubeFace(0, m_DDGICaptureRadianceCube.get(), face, 0, 0);
            cmd->SetComputeUnorderedAccessTextureCubeFace(1, m_DDGICaptureDistanceCube.get(), face, 0, 0);
            cmd->Dispatch((kDDGICaptureSize + 7) / 8, (kDDGICaptureSize + 7) / 8, 1);
        };

        // captureDDGIProbeFaceのDXR版。ラスタライズとキューブへの書き写しをまとめて置き換え、
        // 同じスクラッチキューブ2本を1ディスパッチで直接埋める。
        //
        // 【面ごとに1ディスパッチなのはRHIの制約】キューブのUAVは面ごと(要素数1のTexture2DArray)に
        // しか張れないため、6面を1回のディスパッチへ畳むにはRHIへ「キューブ全スライスを
        // RWTexture2DArrayとして張る」メソッドをDX11/DX12の両方へ足す必要がある。
        // ドローとメッシュ走査が消えるのが本題なので、そこは測ってから決める
        const auto traceDDGIProbeFace =
            [this, skyTexture](RHI::IRHICommandList* cmd, uint32_t probeIndex, uint32_t face)
        {
            const DirectX::XMFLOAT3 probePosition = ComputeDDGIProbePosition(probeIndex);

            DDGITraceConstants traceConstants{};
            traceConstants.Params0 = {
                probePosition.x, probePosition.y, probePosition.z, static_cast<float>(face)
            };
            // w = プロキシとして起こされたマテリアルの自発光倍率。0.0で抑止、1.0でそのまま。
            // ラスタ経路(ObjectConstantsの倍率を0にする)と**同じ判定**から決めること
            traceConstants.Params1 = {
                static_cast<float>(kDDGICaptureSize),
                m_EmissiveIntensity,
                m_DDGISunShadowRayEnabled ? 1.0f : 0.0f,
                ShouldSuppressEmissiveForGI() ? 0.0f : 1.0f
            };

            if (!m_DDGIEmissiveSuppressLoggedTrace)
            {
                m_DDGIEmissiveSuppressLoggedTrace = true;
                Core::Logger::Info(
                    "KurenaiEngine3D",
                    "DDGI(レイトレ)の自発光: Params1.y(強度) " + std::to_string(traceConstants.Params1.y) +
                        " / Params1.w(プロキシ材質の倍率) " + std::to_string(traceConstants.Params1.w) +
                        " / プロキシ印の付いた材質 " + std::to_string(m_RaytracingScene.GetEmissiveProxyMaterialCount()) +
                        "件 / 全メッシュ " + std::to_string(m_RaytracingScene.GetMeshCount()) + "件");
            }

            cmd->SetComputePipelineState(m_DDGIProbeTracePipelineState.get());
            // ヒット面のマテリアルテクスチャをbindlessで引くためs0にWrapが要る(RTAOと同じ理由)
            cmd->SetComputeSamplerSet(m_MaterialSamplers.get());
            cmd->UpdateBuffer(m_DDGITraceConstantBuffer.get(), &traceConstants, sizeof(traceConstants));
            // b0はこのフレームのFrameConstantsをそのまま使う。ラスタ経路と違い、
            // プローブ位置は専用の定数バッファ(b1)で渡すのでViewProjを差し替える必要が無い
            cmd->SetComputeConstantBuffer(0, m_FrameConstantBuffer.get());
            cmd->SetComputeConstantBuffer(1, m_DDGITraceConstantBuffer.get());

            cmd->SetComputeAccelerationStructure(0, m_RaytracingScene.GetTopLevelAS());
            cmd->SetComputeShaderResourceBuffer(1, m_RaytracingScene.GetVertexAttributeBuffer());
            cmd->SetComputeShaderResourceBuffer(2, m_RaytracingScene.GetIndexBuffer());
            cmd->SetComputeShaderResourceBuffer(3, m_RaytracingScene.GetMeshInfoBuffer());
            cmd->SetComputeShaderResourceBuffer(4, m_RaytracingScene.GetInstanceInfoBuffer());
            cmd->SetComputeShaderResourceBuffer(5, m_RaytracingScene.GetMaterialBuffer());
            // メッシュレット表(t6)。このシェーダー自身は引かないが、共有ヘッダーの
            // RaytracingScene.hlsliが宣言を持つためバインドしておく(RTAOと同じ扱い)。
            // メッシュレットを持つメッシュが1つも無いシーンではバッファ自体が無い
            if (RHI::IRHIBuffer* meshletBuffer = m_RaytracingScene.GetMeshletTriangleOffsetBuffer())
            {
                cmd->SetComputeShaderResourceBuffer(6, meshletBuffer);
            }
            cmd->SetComputeShaderResourceBuffer(7, m_LightBuffer.get());
            cmd->SetComputeTexture(8, m_IrradianceTexture.get());
            cmd->SetComputeTexture(9, m_PrefilteredEnvTexture.get());
            cmd->SetComputeTexture(10, m_BRDFLUTTexture.get());
            cmd->SetComputeTexture(11, skyTexture);
            // DDGIの多重バウンス。ラスタ経路がt12/t13で引いているのと同じアトラス
            cmd->SetComputeTexture(12, m_DDGIIrradianceAtlas.get());
            cmd->SetComputeTexture(13, m_DDGIDistanceAtlas.get());

            // UAVはDispatch直後に解除されるため毎回バインドし直す(IRHICommandList.h参照)
            cmd->SetComputeUnorderedAccessTextureCubeFace(0, m_DDGICaptureRadianceCube.get(), face, 0, 0);
            cmd->SetComputeUnorderedAccessTextureCubeFace(1, m_DDGICaptureDistanceCube.get(), face, 0, 0);
            cmd->Dispatch((kDDGICaptureSize + 7) / 8, (kDDGICaptureSize + 7) / 8, 1);
        };

        // 組み上がったキューブ2本から、オクタヘドラルアトラスの該当セルを焼き直す。
        // 境界の複製は本体の書き込みが全て終わってからでないと正しい値を読めないので別ディスパッチ
        const auto updateDDGIProbe = [this, effectiveExposure](RHI::IRHICommandList* cmd, uint32_t probeIndex, bool overwrite)
        {
            DDGIUpdateConstants updateConstants{};
            updateConstants.Params0 = {
                static_cast<float>(probeIndex),
                m_GIVolume.Hysteresis,
                m_GIVolume.MaxRayDistance,
                static_cast<float>(kDDGICaptureSize),
            };
            updateConstants.Params1 = {
                static_cast<float>(kDDGIIrradianceTexels),
                static_cast<float>(kDDGIDistanceTexels),
                static_cast<float>(kDDGIProbeBorder),
                overwrite ? 1.0f : 0.0f,
            };
            updateConstants.Params2 = {
                static_cast<float>(m_GIVolume.ProbeCounts[0]),
                static_cast<float>(m_GIVolume.ProbeCounts[1]),
                static_cast<float>(m_GIVolume.ProbeCounts[2]),
                effectiveExposure,
            };
            cmd->UpdateBuffer(m_DDGIUpdateConstantBuffer.get(), &updateConstants, sizeof(updateConstants));

            // 本体の書き込み。スレッドは2つの解像度の広いほうに合わせて起動し、
            // それぞれの範囲外はシェーダー側で弾く
            constexpr uint32_t kUpdateThreads = (kDDGIIrradianceTexels > kDDGIDistanceTexels)
                ? kDDGIIrradianceTexels : kDDGIDistanceTexels;
            cmd->SetComputePipelineState(m_DDGIProbeUpdatePipelineState.get());
            cmd->SetComputeConstantBuffer(0, m_DDGIUpdateConstantBuffer.get());
            cmd->SetComputeSamplerSet(m_MaterialSamplers.get());
            cmd->SetComputeTexture(0, m_DDGICaptureRadianceCube.get());
            cmd->SetComputeTexture(1, m_DDGICaptureDistanceCube.get());
            cmd->SetComputeUnorderedAccessTexture(0, m_DDGIIrradianceAtlas.get());
            cmd->SetComputeUnorderedAccessTexture(1, m_DDGIDistanceAtlas.get());
            cmd->Dispatch((kUpdateThreads + 7) / 8, (kUpdateThreads + 7) / 8, 1);

            // 境界の複製。セル全体(境界込み)を走査するので広いほうのセルサイズに合わせる
            constexpr uint32_t kBorderThreads = (kDDGIIrradianceCell > kDDGIDistanceCell)
                ? kDDGIIrradianceCell : kDDGIDistanceCell;
            cmd->SetComputePipelineState(m_DDGIBorderCopyPipelineState.get());
            cmd->SetComputeConstantBuffer(0, m_DDGIUpdateConstantBuffer.get());
            cmd->SetComputeUnorderedAccessTexture(0, m_DDGIIrradianceAtlas.get());
            cmd->SetComputeUnorderedAccessTexture(1, m_DDGIDistanceAtlas.get());
            cmd->Dispatch((kBorderThreads + 7) / 8, (kBorderThreads + 7) / 8, 1);
        };

        // 焼き上がりに影響する状態が変わったら、停止していた更新を再開する。
        // 【判定はm_DDGIEnabled等のガードの外に置く】無効な間も署名を追い続けないと、
        // 無効中に時刻を動かして再度有効にしたとき「署名は同じ」と誤判定して止まったままになる
        if (m_HasGIVolume && m_DDGIProbeCount > 0)
        {
            const uint64_t bakeSignature = ComputeProbeBakeSignature();
            if (!m_DDGIBakeSignatureValid || bakeSignature != m_DDGIBakeSignature)
            {
                m_DDGIBakeSignature = bakeSignature;
                m_DDGIBakeSignatureValid = true;
                m_DDGIStableCycles = 0;
                m_DDGIUpdateSuspended = false;
            }
        }

        if (m_DDGIEnabled && m_HasGIVolume && m_DDGIProbeCount > 0 && !m_DDGIUpdateSuspended)
        {
            // レイの取得をどちらで行うか。パスの登録とキャプチャの実行で同じ判定を使う
            const bool useRaytracedTrace = ShouldRunRaytracedDDGITrace();

            // 【どちらの経路が実際に走ったかをログに残す】切り替えたつもりで切り替わっていない、
            // という取り違えをA/B比較の前に潰すため。切り替わったときだけ出す
            if (!m_DDGIRayModeReported || m_DDGIRayModeReportedRaytraced != useRaytracedTrace)
            {
                m_DDGIRayModeReported = true;
                m_DDGIRayModeReportedRaytraced = useRaytracedTrace;
                Core::Logger::Info(
                    "KurenaiEngine3D",
                    useRaytracedTrace
                        ? std::string("DDGIのレイ取得: レイトレーシング(DXR)。太陽の影レイ: ") +
                              (m_DDGISunShadowRayEnabled ? "有効" : "無効")
                        : std::string("DDGIのレイ取得: ラスタライズ"));
            }

            uint32_t perFrame = std::min<uint32_t>(
                static_cast<uint32_t>(std::max(m_DDGIProbesPerFrame, 1)), m_DDGIProbeCount);
            if (!useRaytracedTrace)
            {
                // 1フレームの描画回数・定数書き込み回数の上限はラスタ経路だけの制約。
                // レイトレース経路はメッシュごとの描画をしないので抑える必要が無い
                perFrame = ClampDDGIProbesPerFrameToConstantRing(perFrame);
            }
            const bool warmingUp = m_DDGIWarmingUp;

            // 実効プリ露出が大きく動いたら、一巡ぶんだけ上書きへ切り替えて即座に追従させる
            // (理由はKurenaiEngine3D.hのm_DDGIOverwriteRemainingのコメント参照)。
            // 一巡目(warmingUp)は元から上書きなので何もしない
            if (!warmingUp)
            {
                if (!m_DDGILastExposureValid)
                {
                    m_DDGILastExposureEV100 = m_EffectiveExposureEV100;
                    m_DDGILastExposureValid = true;
                }
                else if (std::abs(m_EffectiveExposureEV100 - m_DDGILastExposureEV100) > kDDGIExposureRewarmEV)
                {
                    m_DDGIOverwriteRemaining = m_DDGIProbeCount;
                    m_DDGILastExposureEV100 = m_EffectiveExposureEV100;
                }
            }
            // このフレームで上書きするぶんを先に確定させる(ラムダへ値で渡すため)
            const uint32_t overwriteThisFrame = std::min(m_DDGIOverwriteRemaining, perFrame);
            m_DDGIOverwriteRemaining -= overwriteThisFrame;

            // 【止めるモードでは停止するまでの全巡回を上書きで焼く】理由はKurenaiEngine3D.hの
            // kDDGIBounceCyclesのコメント参照。露出追従のm_DDGIOverwriteRemainingとは
            // 独立に効かせたいので、残数を消費せず条件だけ合流させる
            const bool overwriteWholeCycle = !warmingUp && m_DDGIUpdateMode != DDGIUpdateMode::Always;

            // --- 格子のスクロールで未確定になったスロットを拾う ---
            //
            // カメラが動くと、各LODの原点がその段の格子へスナップし直される。スナップして
            // いるのでプローブのワールド座標そのものは動かないが、範囲から抜けた列のセルが
            // 反対側の新しい列へ回るため、そのセルは「別の場所を担当する」ようになる。
            // そこには前の場所のイラディアンスが残っているので、焼き直すまで使ってはいけない。
            m_DDGIDirtyProbeList.clear();
            if (m_DDGIProbeBakedCoord.size() == m_DDGIProbeCount)
            {
                for (uint32_t slot = 0; slot < m_DDGIProbeCount; ++slot)
                {
                    const DirectX::XMINT3 current = ComputeDDGIProbeWorldCoord(slot);
                    const DirectX::XMINT3& baked = m_DDGIProbeBakedCoord[slot];
                    if (current.x != baked.x || current.y != baked.y || current.z != baked.z)
                    {
                        m_DDGIDirtyProbeList.push_back(slot);
                    }
                }
            }

            // 【細かいLODを優先する】通し番号は LOD0 が先頭に並ぶので、番号順に詰めるだけで
            // 「細かい段から先に焼き直す」になる。見ている場所の間接光が先に確定する。
            //
            // 未確定が1フレームの予算を超えるときは、超えたぶんが次フレーム以降へ回る。
            // その間そのスロットはαの印(2.0)でサンプリングから外れているので、
            // 「別の場所の色を配る」ことは無い(遅れるだけで壊れない)
            const uint32_t dirtyThisFrame =
                std::min<uint32_t>(static_cast<uint32_t>(m_DDGIDirtyProbeList.size()), perFrame);

            // 【スナップが効いているかを数で見るためのログ】カメラが1セル未満しか動かなければ
            // どのプローブもワールド座標を変えないので、未確定は0のままでなければならない。
            // セル境界をまたぐと、その軸に垂直な面1枚ぶんが一度に未確定になる。
            // 追従が「スナップせず連続的に動く」実装になっていると毎フレーム全数が未確定になるので、
            // この数を見れば取り違えにすぐ気づける
            if (m_GIVolume.FollowCamera && !m_DDGIDirtyProbeList.empty())
            {
                // 【LOD0の基準格子座標も出す】これが同じなら格子は同じ場所にある。
                // 往復の検証で「カメラが同じセルへ戻ったか」を、絵ではなく数で確かめられる
                const DirectX::XMINT3 base0 = ComputeDDGILODBaseIndex(0);
                Core::Logger::Info(
                    "KurenaiEngine3D",
                    "DDGIの格子がスクロールしました: 未確定 " +
                        std::to_string(m_DDGIDirtyProbeList.size()) + " / " +
                        std::to_string(m_DDGIProbeCount) + " スロット(このフレームで焼き直すのは " +
                        std::to_string(dirtyThisFrame) + " 個) LOD0基準=(" +
                        std::to_string(base0.x) + "," + std::to_string(base0.y) + "," +
                        std::to_string(base0.z) + ")");
            }

            // --- 未確定のスロットを、焼き直されるまでサンプリングから外す ---
            //
            // 【焼ける数より多くてもすべて外す】このフレームで焼けるのは perFrame 個までだが、
            // 印を付けるのは全部に対して行う。付けそこねたスロットは、焼き直されるまでの間
            // 「別の場所のイラディアンス」を配り続けることになる
            if (!m_DDGIDirtyProbeList.empty() && m_DDGIInvalidateProbesPipelineState && m_DDGIDirtyProbeBuffer)
            {
                const uint32_t dirtyCount =
                    std::min<uint32_t>(static_cast<uint32_t>(m_DDGIDirtyProbeList.size()), kDDGIMaxProbes);
                graph.AddPass(Core::RenderGraphPassDesc{
                    .Name = "DDGIInvalidate",
                    .Writes = { m_DDGIIrradianceAtlas.get() },
                    .Execute = [this, dirtyCount](RHI::IRHICommandList* cmd)
                    {
                        cmd->UpdateBuffer(
                            m_DDGIDirtyProbeBuffer.get(), m_DDGIDirtyProbeList.data(),
                            dirtyCount * static_cast<uint32_t>(sizeof(uint32_t)));

                        DDGIUpdateConstants invalidateConstants{};
                        // このパスだけ Params0.x は「無効化する個数」の意味で使う
                        invalidateConstants.Params0 = {
                            static_cast<float>(dirtyCount), 0.0f, 0.0f, static_cast<float>(kDDGICaptureSize)
                        };
                        invalidateConstants.Params1 = {
                            static_cast<float>(kDDGIIrradianceTexels), static_cast<float>(kDDGIDistanceTexels),
                            static_cast<float>(kDDGIProbeBorder), 0.0f
                        };
                        invalidateConstants.Params2 = {
                            static_cast<float>(m_GIVolume.ProbeCounts[0]),
                            static_cast<float>(m_GIVolume.ProbeCounts[1]),
                            static_cast<float>(m_GIVolume.ProbeCounts[2]), 1.0f
                        };
                        cmd->UpdateBuffer(
                            m_DDGIUpdateConstantBuffer.get(), &invalidateConstants, sizeof(invalidateConstants));

                        cmd->SetComputePipelineState(m_DDGIInvalidateProbesPipelineState.get());
                        cmd->SetComputeConstantBuffer(0, m_DDGIUpdateConstantBuffer.get());
                        cmd->SetComputeShaderResourceBuffer(2, m_DDGIDirtyProbeBuffer.get());
                        // UAVはDispatch直後に解除されるため毎回バインドし直す
                        cmd->SetComputeUnorderedAccessTexture(0, m_DDGIIrradianceAtlas.get());
                        // 1グループ = 1プローブのセル
                        cmd->Dispatch(dirtyCount, 1, 1);
                    },
                });
            }

            for (uint32_t i = 0; i < perFrame; ++i)
            {
                // 未確定のスロットを先に消化し、余った枠を通常のラウンドロビンへ回す
                const bool isDirtySlot = (i < dirtyThisFrame);
                const uint32_t probeIndex = isDirtySlot
                    ? m_DDGIDirtyProbeList[i]
                    : (m_DDGIUpdateCursor + (i - dirtyThisFrame)) % m_DDGIProbeCount;

                // 一巡目はヒステリシスを使わず上書きする(混ぜる相手の「前の値」が未初期化のため)。
                // 露出が急変した直後も同じく上書きで追従させる。
                // **未確定のスロットも必ず上書き** ―― 前の値は別の場所のものなので混ぜてはいけない
                const bool overwrite =
                    warmingUp || overwriteWholeCycle || isDirtySlot || (i < overwriteThisFrame);

                // このスロットを焼いたので、担当しているワールド格子座標を記録し直す
                if (m_DDGIProbeBakedCoord.size() == m_DDGIProbeCount)
                {
                    m_DDGIProbeBakedCoord[probeIndex] = ComputeDDGIProbeWorldCoord(probeIndex);
                }

                graph.AddPass(Core::RenderGraphPassDesc{
                    .Name = "DDGIUpdate" + std::to_string(probeIndex),
                    .Reads = probeCaptureReads,
                    .Writes = {
                        m_DDGICaptureColor.get(), m_DDGICaptureDistance.get(), m_DDGICaptureDepth.get(),
                        m_DDGICaptureRadianceCube.get(), m_DDGICaptureDistanceCube.get(),
                        m_DDGIIrradianceAtlas.get(), m_DDGIDistanceAtlas.get(),
                    },
                    .Execute =
                        [&captureDDGIProbeFace, &traceDDGIProbeFace, &updateDDGIProbe, probeIndex, overwrite,
                         useRaytracedTrace](RHI::IRHICommandList* cmd)
                    {
                        // レイの取得だけを差し替える。埋めるスクラッチキューブも、
                        // そのあとの更新CSも同じものを使う(A/Bの差分をレイ取得に限定するため)
                        for (uint32_t face = 0; face < kCubeFaceCount; ++face)
                        {
                            if (useRaytracedTrace)
                            {
                                traceDDGIProbeFace(cmd, probeIndex, face);
                            }
                            else
                            {
                                captureDDGIProbeFace(cmd, probeIndex, face);
                            }
                        }
                        updateDDGIProbe(cmd, probeIndex, overwrite);
                    },
                });
            }

            // 【未確定ぶんはカーソルを進めない】未確定のスロットは番号順ではなく飛び飛びに
            // 選ばれるので、その枠までカーソルを進めると通常の巡回に穴が空く
            const uint32_t nextCursor = m_DDGIUpdateCursor + (perFrame - dirtyThisFrame);
            const bool cycleCompleted = nextCursor >= m_DDGIProbeCount;

            // 一巡ぶん焼き終えるたびに数え、モードごとの巡回数に達したら止める。
            // 【上書きが残っている間は止めない】まだ焼き切っていないため。
            // 一巡目(warmingUp)はこの後の分岐で別に扱うのでここでは数えない
            if (cycleCompleted && !warmingUp && m_DDGIUpdateMode != DDGIUpdateMode::Always)
            {
                ++m_DDGIStableCycles;
                if (m_DDGIOverwriteRemaining == 0)
                {
                    const uint32_t requiredCycles = (m_DDGIUpdateMode == DDGIUpdateMode::OverwriteThenStop)
                        ? 1u
                        : kDDGIBounceCycles;
                    if (m_DDGIStableCycles >= requiredCycles)
                    {
                        m_DDGIUpdateSuspended = true;
                        Core::Logger::Info(
                            "KurenaiEngine3D",
                            "DDGIが収束したため更新を停止しました(" + std::to_string(m_DDGIStableCycles) +
                                "巡)。焼き上がりに影響する状態が変わると再開します");
                    }
                }
            }

            if (warmingUp && cycleCompleted)
            {
                // 全プローブが一度ずつ書かれた。ここから先はヒステリシスで滑らかに追従させ、
                // 同時にサンプリング側(DDGIParams0.w)を有効にする
                m_DDGIWarmingUp = false;
                m_DDGIBaked = true;
                m_DDGILastExposureEV100 = m_EffectiveExposureEV100;
                m_DDGILastExposureValid = true;
                Core::Logger::Info(
                    "KurenaiEngine3D",
                    "DDGIの初回一巡が完了しました(" + std::to_string(m_DDGIProbeCount) + "プローブ)");
            }
            m_DDGIUpdateCursor = nextCursor % m_DDGIProbeCount;
        }

        // --- 深度プリパス(41.22節): 不透明ジオメトリの深度だけを先に埋める ---
        //
        // これを通しておくと、次のG-Bufferパスでは最前面の断片だけが深度テストを通り
        // (PSOのDepthAllowEqual)、隠れる画素のピクセルシェーダー ―― 6テクスチャの
        // サンプルと6枚のレンダーターゲットへの書き込み ―― がまるごと省ける。
        //
        // 【カットアウトのPSOが作れていないときはプリパスごと切る】カットアウトのメッシュだけを
        // プリパスから外すと、そのメッシュはG-Buffer側で深度を書くことになるが、
        // プリパスで手前に別のものが書かれていると早期Zに落とされて消える。
        // 中途半端に混ぜるより丸ごと従来経路にするほうが安全
        //
        // 【メッシュシェーダー経路とも併用できるようになった】かつてプリパスはメッシュレット経路と
        // 排他だった。プリパスが頂点シェーダーで深度を書き、G-Bufferがメッシュシェーダーで描くと、
        // 同じ頂点でも変換の丸めが一致する保証が無く、深度が1ulpずれた面がGREATER_EQUALを
        // 通らずに消えるため。プリパスにもG-Bufferとまったく同じ増幅/メッシュシェーダーを使う
        // PSOを用意したので、変換は文字どおり同一のコードになりこの問題は起きない。
        //
        // メッシュレット版のPSOが作れなかった場合は、その経路で描くモデルだけを
        // プリパスから外す(下のループ参照)。深度が埋まらないぶん早期Zが効かないだけで、
        // G-Buffer側が改めて深度を書くため絵は壊れない
        // (depthPrepassRuns / hiZFromDepthPrepass の宣言は FrameConstants を組む箇所にある)

        // --- モデル単位のGPUカリングパス(Stage 5-3) ---
        //
        // 描画候補のワールドAABBを、コンピュートシェーダーが視錐台とHi-Zで判定し、
        // 生き残ったものだけの ExecuteIndirect 引数をGPU上に作る。
        // 深度プリパスとG-Bufferは、その引数でまとめて描く。
        //
        // 【深度プリパスより前に登録すること】RenderGraphは登録順で実行する。
        // 引数を使うのはプリパスとG-Bufferの両方なので、どちらよりも前にいる必要がある。
        // **片方だけ間引くと絵が壊れる** ―― プリパスが深度を書いたものをG-Bufferが
        // 描かないと、その画素は「深度はあるのに色が無い」穴になる。
        //
        // 【Hi-Zは前フレームのもの】Hi-Zパスの登録はG-Bufferより後なので、ここが読むのは
        // 前フレームに書かれた内容になる。カメラ移動ぶんAABBを膨らませて視差を吸収する
        const bool modelCullGpuActive = m_ModelCullGpuEnabled && meshletPathActive
            && m_ModelCullPipelineState && m_ModelCullCounterBuffer && !m_Scene.Instances.empty();

        // hiZFromDepthPrepass = Hi-Zを**深度プリパスの深度から**作るか(宣言は上流にある)。
        // 作れるなら、G-Bufferの判定は今フレームのHi-Zで行える ――
        // 1フレーム遅れも保守的な膨張も要らなくなる。
        //
        // 【プリパスが走らないフレームは従来どおり】プリパスが無ければ深度が埋まっておらず、
        // そこでHi-Zを作っても中身は空になる。その場合はG-Bufferの後で作り、
        // 次フレームに前フレームのものとして読む(m_HiZValidが立つのもそのとき)。
        //
        // 【プリパス自身は今フレームのHi-Zを使えない】そのHi-Zはプリパスの出力から作る。
        // プリパスは従来どおり前フレームのHi-Zで判定する ―― 保守的なので絵は壊れないし、
        // 判定を捨てるとプリパスが描くメッシュレットが9倍近くに戻る

        // 前フレームのHi-Zで判定してよいか(深度プリパス、およびプリパスが無いときのG-Buffer)
        const bool occlusionPrevFrameEnabled = occlusionCullEnabledThisFrame;
        // 今フレームのHi-Zで判定してよいか(G-Buffer。Hi-Z構築パスが必ず先に走る)
        const bool occlusionCurrentFrameEnabled = hiZFromDepthPrepass;

        // ObjectConstants::MeshletOcclusionMode と ModelCull のオクルージョン有効フラグへ渡す値
        const uint32_t prepassOcclusionMode = occlusionPrevFrameEnabled ? 1u : 0u;
        const uint32_t gbufferOcclusionMode = occlusionCurrentFrameEnabled
            ? 2u
            : (occlusionPrevFrameEnabled ? 1u : 0u);

        // 区画(=PSO)の対応表。nullptrの区画は候補に載せない(そのぶんは従来のCPUループが描く)
        RHI::IRHIPipelineState* modelCullRegionPipelines[kModelCullRegionCount]{};
        if (modelCullGpuActive)
        {
            const bool meshletDebug = m_MeshletDebugViewEnabled && m_GBufferMeshletDebugPipelineState;
            modelCullRegionPipelines[kModelCullRegionGBuffer] = meshletDebug
                ? m_GBufferMeshletDebugPipelineState.get()
                : m_GBufferMeshletPipelineState.get();
            modelCullRegionPipelines[kModelCullRegionGBufferMirrored] = meshletDebug
                ? m_GBufferMeshletDebugPipelineStateMirrored.get()
                : m_GBufferMeshletPipelineStateMirrored.get();
            if (depthPrepassRuns)
            {
                modelCullRegionPipelines[kModelCullRegionPrepassOpaque] =
                    m_DepthPrepassMeshletPipelineState.get();
                modelCullRegionPipelines[kModelCullRegionPrepassOpaqueMirrored] =
                    m_DepthPrepassMeshletPipelineStateMirrored.get();
                modelCullRegionPipelines[kModelCullRegionPrepassCutout] =
                    m_DepthPrepassMeshletCutoutPipelineState.get();
                modelCullRegionPipelines[kModelCullRegionPrepassCutoutMirrored] =
                    m_DepthPrepassMeshletCutoutPipelineStateMirrored.get();
            }
        }

        // 候補の列挙。**プリパスとG-Bufferの元のループとまったく同じ条件で選ぶこと** ――
        // 選び方がずれると、間引かれてもいないのに描かれないドローが出る
        //
        // 【並びは「深度プリパスぶん → G-Bufferぶん」】判定を2回に分けるため、
        // 各ディスパッチが受け持つ範囲が連続していなければならない
        std::vector<ModelCullDrawCandidate> modelCullDraws;
        std::vector<ModelCullDrawCandidate> modelCullGBufferDraws;
        std::fill(std::begin(m_ModelCullRegionCandidates), std::end(m_ModelCullRegionCandidates), 0u);
        m_ModelCullCandidateCount = 0;
        m_ModelCullPrepassCandidateCount = 0;
        m_ModelCullCpuFrustumCulled = 0;
        if (modelCullGpuActive)
        {
            // 突き合わせ相手のCPU側の判定。G-Bufferのループが使うものと同じ錐台
            const FrustumPlanes cullFrustum = ExtractFrustumPlanes(viewProj);
            modelCullDraws.reserve(m_Scene.Instances.size() * 2);

            for (size_t instanceIndex = 0; instanceIndex < m_Scene.Instances.size(); ++instanceIndex)
            {
                const Assets::ModelInstance& instance = m_Scene.Instances[instanceIndex];

                // モデルLOD。フェード中は2段を重ねる。
                // 【プリパス・G-Bufferとまったく同じ組であること】UpdateModelLODがフレーム先頭で
                // 1回だけ決めた結果を全員が引くので、ここで距離を測り直さない
                LODDraw lodDraws[2]{};
                const uint32_t lodDrawCount = GetLODDraws(instanceIndex, lodDraws);
                for (uint32_t lodDrawIndex = 0; lodDrawIndex < lodDrawCount; ++lodDrawIndex)
                {
                    const Assets::Model& lodModel = *lodDraws[lodDrawIndex].Model;
                    if (!ShouldUseModelMeshletPath(instance, lodModel))
                    {
                        continue;
                    }

                    // 起動するのは「モデル全体のメッシュレット数 ÷ 増幅シェーダーのグループサイズ」。
                    // 実際にラスタライズされるのはカリングとふるい分けを生き延びたぶんに絞られる
                    constexpr uint32_t kAmplificationGroupSize = 32; // GBufferMeshlet.hlslと一致させること
                    const uint32_t groupCount =
                        (lodModel.TotalMeshletCount + kAmplificationGroupSize - 1) / kAmplificationGroupSize;
                    if (groupCount == 0)
                    {
                        continue;
                    }

                    const float lodDitherFade = lodDraws[lodDrawIndex].DitherFade;
                    const bool mirrored = instance.IsMirrored;
                    // 同じ候補をCPU側の判定なら間引くか。GPUの「視錐台で間引いた数」と突き合わせる
                    const bool cpuVisible =
                        IsAABBVisible(cullFrustum, instance.WorldBoundsMin, instance.WorldBoundsMax);

                    const auto addCandidate = [&](std::vector<ModelCullDrawCandidate>& list, uint32_t region,
                                                  uint32_t rejectMask, uint32_t requireMask, float ditherFade,
                                                  bool countCullStats, uint32_t occlusionMode)
                    {
                        if (!modelCullRegionPipelines[region])
                        {
                            return;
                        }
                        ModelCullDrawCandidate candidate{};
                        candidate.Instance = &instance;
                        candidate.Model = &lodModel;
                        candidate.Region = region;
                        candidate.GroupCount = groupCount;
                        candidate.RejectMask = rejectMask;
                        candidate.RequireMask = requireMask;
                        candidate.DitherFade = ditherFade;
                        candidate.CountCullStats = countCullStats;
                        candidate.OcclusionMode = occlusionMode;
                        list.push_back(candidate);
                        ++m_ModelCullRegionCandidates[region];
                        // 【CPU側の間引き数もG-Bufferぶんだけ数える】GPU側の統計と単位を揃えるため。
                        // 両方数えると1モデルを2回数え、モデル数の2倍という比べにくい数になる
                        if (countCullStats && !cpuVisible)
                        {
                            ++m_ModelCullCpuFrustumCulled;
                        }
                    };

                    // G-Buffer。半透明(BLEND)だけは増幅シェーダーが落とす ――
                    // G-Bufferに書かず専用のフォワードパスへ回るため
                    addCandidate(
                        modelCullGBufferDraws,
                        mirrored ? kModelCullRegionGBufferMirrored : kModelCullRegionGBuffer,
                        Assets::kGpuMaterialFlagTransparent, 0u, lodDitherFade, /*countCullStats=*/true,
                        gbufferOcclusionMode);

                    // 深度プリパス。
                    // 【クロスディザのフェード中は載せない】不透明用のPSOはピクセルシェーダーを
                    // 持たずApplyLODDitherを通せないため、捨てるはずの画素まで深度を書いて
                    // G-Bufferとの食い違いで穴が開く(元のループと同じ条件)
                    if (!depthPrepassRuns || lodDitherFade < 1.0f)
                    {
                        continue;
                    }
                    addCandidate(
                        modelCullDraws,
                        mirrored ? kModelCullRegionPrepassOpaqueMirrored : kModelCullRegionPrepassOpaque,
                        Assets::kGpuMaterialFlagTransparent | Assets::kGpuMaterialFlagCutout, 0u, 1.0f, false,
                        prepassOcclusionMode);
                    // カットアウトぶん(clipを通す)。持たないモデルではこの回は発行しない
                    if (lodModel.HasCutoutMaterial)
                    {
                        addCandidate(
                            modelCullDraws,
                            mirrored ? kModelCullRegionPrepassCutoutMirrored : kModelCullRegionPrepassCutout,
                            Assets::kGpuMaterialFlagTransparent, Assets::kGpuMaterialFlagCutout, 1.0f, false,
                            prepassOcclusionMode);
                    }
                }
            }

            // プリパスぶんを前半、G-Bufferぶんを後半に置く
            m_ModelCullPrepassCandidateCount = static_cast<uint32_t>(modelCullDraws.size());
            modelCullDraws.insert(
                modelCullDraws.end(), modelCullGBufferDraws.begin(), modelCullGBufferDraws.end());
            m_ModelCullCandidateCount = static_cast<uint32_t>(modelCullDraws.size());
            EnsureModelCullCapacity(m_ModelCullCandidateCount);
        }

        // ここまで来て初めてバッファが揃っているかが分かる(容量確保は失敗しうる)
        const bool modelCullReady = modelCullGpuActive && m_ModelCullCandidateCount > 0
            && m_ModelCullInstanceBuffer && m_ModelCullDrawArgsBuffer;
        // 実際に描画発行まで任せるか。
        // 【DX11とメッシュシェーダー非対応環境では常にfalse】従来のCPUループへ縮退する
        const bool modelCullIndirectActive =
            modelCullReady && m_ModelCullIndirectEnabled && m_Device->SupportsIndirectDispatchMesh();
        m_ModelCullIndirectActiveLastFrame = modelCullIndirectActive;
        m_HiZFromDepthPrepassLastFrame = hiZFromDepthPrepass;
        m_ModelCullDispatchCounts[0] = hiZFromDepthPrepass
            ? m_ModelCullPrepassCandidateCount
            : m_ModelCullCandidateCount;
        m_ModelCullDispatchCounts[1] = hiZFromDepthPrepass
            ? (m_ModelCullCandidateCount - m_ModelCullPrepassCandidateCount)
            : 0u;

        // 候補配列のうち [beginIndex, beginIndex + count) だけを判定するパスを1つ登録する。
        //
        // 【2回に分ける必要がある】Hi-Zを深度プリパスの深度から作ると、判定に使えるHi-Zが
        // フレームの途中で「前フレームのもの」から「今フレームのもの」へ変わる。
        // 深度プリパスぶんはプリパスより前に前フレームのHi-Zで、G-Bufferぶんは
        // Hi-Zを作り終えてから今フレームのHi-Zで判定する。
        // initializeBuffers は候補配列の書き込みとカウンタ初期化を行うか(先頭の1回だけtrue)
        const auto addModelCullPass =
            [&](std::string passName, uint32_t beginIndex, uint32_t count, bool initializeBuffers,
                bool useCurrentFrameHiZ, bool occlusionEnabled)
        {
            if (!modelCullReady || (count == 0 && !initializeBuffers))
            {
                return;
            }
            const uint32_t regionStride = m_ModelCullRegionStride;
            const uint32_t statsBeginIndex = m_ModelCullPrepassCandidateCount;
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = std::move(passName),
                // Hi-Zを読む。前フレームのものを読む側では、それより前に書き手がいないので
                // 辺は張られない(RenderGraphのReadsは登録順で解決する)
                .Reads = { m_HiZTexture.get() },
                .BufferWrites = { m_ModelCullCounterBuffer.get(), m_ModelCullDrawArgsBuffer.get() },
                .Execute = [this, beginIndex, count, initializeBuffers, useCurrentFrameHiZ, occlusionEnabled,
                            regionStride, statsBeginIndex, cameraMoveDistance, modelCullIndirectActive,
                            &viewProj, &modelCullDraws](RHI::IRHICommandList* cmd)
                {
                    if (initializeBuffers)
                    {
                        // 候補ごとのObjectConstantsを定数バッファのリングへ書き、そのスロットの
                        // GPUアドレスを候補に添える。
                        //
                        // 【描画パスではなくここで書く理由】ExecuteIndirectの引数には
                        // 「このドローが使う定数バッファのアドレス」そのものが要る。アドレスは
                        // UpdateBufferがリングを1つ進めた後にしか分からず、しかも引数を組み立てるのは
                        // このパスのコンピュートシェーダーなので、その材料より前に書いておく必要がある。
                        //
                        // 【後半(G-Bufferぶん)もここで書く】2回目のディスパッチのぶんも含めて
                        // 一度に載せる。リングのスロットは上書きされない限り生き続けるので、
                        // 使うのが後のパスでも構わない
                        m_ModelCullUploadScratch.clear();
                        m_ModelCullUploadScratch.reserve(modelCullDraws.size());
                        for (const ModelCullDrawCandidate& draw : modelCullDraws)
                        {
                            GpuModelCullInstance candidate{};
                            candidate.BoundsMin[0] = draw.Instance->WorldBoundsMin[0];
                            candidate.BoundsMin[1] = draw.Instance->WorldBoundsMin[1];
                            candidate.BoundsMin[2] = draw.Instance->WorldBoundsMin[2];
                            candidate.BoundsMax[0] = draw.Instance->WorldBoundsMax[0];
                            candidate.BoundsMax[1] = draw.Instance->WorldBoundsMax[1];
                            candidate.BoundsMax[2] = draw.Instance->WorldBoundsMax[2];
                            candidate.GroupCount = draw.GroupCount;
                            candidate.RegionIndex = draw.Region;

                            if (modelCullIndirectActive)
                            {
                                const ObjectConstants objectConstants = MakeModelObjectConstants(
                                    *draw.Instance, *draw.Model, m_EmissiveIntensity, m_OcclusionMapEnabled,
                                    draw.RejectMask, draw.RequireMask, m_MeshletLODFrame,
                                    draw.CountCullStats, draw.DitherFade, draw.OcclusionMode);
                                cmd->UpdateBuffer(
                                    m_ObjectConstantBuffer.get(), &objectConstants, sizeof(objectConstants));
                                if (!m_ObjectConstantBuffer->GetLastUpdateGpuAddress(
                                        candidate.CbvAddress[0], candidate.CbvAddress[1]))
                                {
                                    // アドレスが取れないバックエンドはそもそも間接描画へ行かない。
                                    // 万一ここへ来たら、嘘のアドレスを引数へ書くよりは
                                    // 「描くものが無い候補」にしてGPUに触らせない
                                    candidate.GroupCount = 0;
                                }
                            }

                            m_ModelCullUploadScratch.push_back(candidate);
                        }
                        cmd->UpdateBuffer(
                            m_ModelCullInstanceBuffer.get(), m_ModelCullUploadScratch.data(),
                            static_cast<uint32_t>(
                                sizeof(GpuModelCullInstance) * m_ModelCullUploadScratch.size()));

                        // 加算しかしないので毎フレーム0へ戻す。生き残りを詰める位置も
                        // このカウンタで取るため、戻さないと2フレーム目以降が範囲外へ書く
                        cmd->ClearUnorderedAccessBufferUint(m_ModelCullCounterBuffer.get(), 0);
                        // 引数バッファも同じ理由で戻す。**先頭の発行数だけでなく全体を0にする** ――
                        // 前フレームの引数が残っていると、件数が減ったときに古い引数が
                        // 範囲内に居座り、そのぶんが二重に描かれる
                        cmd->ClearUnorderedAccessBufferUint(m_ModelCullDrawArgsBuffer.get(), 0);
                    }

                    if (count == 0)
                    {
                        return;
                    }

                    ModelCullConstants cullConstants{};
                    DirectX::XMStoreFloat4x4(
                        &cullConstants.CullViewProj, DirectX::XMMatrixTranspose(viewProj));
                    if (useCurrentFrameHiZ)
                    {
                        // 今フレームの深度プリパスから作ったHi-Zを読む。投影も今フレームの行列
                        cullConstants.CullPrevViewProj = cullConstants.CullViewProj;
                    }
                    else
                    {
                        cullConstants.CullPrevViewProj =
                            m_TAAPrevViewProjValid ? m_TAAPrevViewProj : DirectX::XMFLOAT4X4{};
                    }
                    cullConstants.CullParams = {
                        count, m_HiZMipLevels, occlusionEnabled ? 1u : 0u, kModelCullArgsBaseOffset
                    };
                    cullConstants.CullRegionParams = {
                        regionStride, kModelCullRegionCount, beginIndex, statsBeginIndex
                    };
                    cullConstants.CullHiZScreenParams = {
                        static_cast<float>(m_RenderWidth), static_cast<float>(m_RenderHeight), 0.0f, 0.0f
                    };
                    // 今フレームのHi-Zなら視差のずれが無いので膨らませない
                    cullConstants.CullExpandParams = {
                        useCurrentFrameHiZ ? 0.0f : cameraMoveDistance, 0.0f, 0.0f, 0.0f
                    };
                    cmd->UpdateBuffer(m_ModelCullConstantBuffer.get(), &cullConstants, sizeof(cullConstants));

                    cmd->SetComputePipelineState(m_ModelCullPipelineState.get());
                    cmd->SetComputeConstantBuffer(0, m_ModelCullConstantBuffer.get());
                    cmd->SetComputeShaderResourceBuffer(0, m_ModelCullInstanceBuffer.get());
                    cmd->SetComputeTexture(1, m_HiZTexture.get());
                    cmd->SetComputeUnorderedAccessBuffer(0, m_ModelCullCounterBuffer.get());
                    cmd->SetComputeUnorderedAccessBuffer(1, m_ModelCullDrawArgsBuffer.get());

                    constexpr uint32_t kModelCullGroupSize = 64; // ModelCull.hlslと一致させること
                    cmd->Dispatch((count + kModelCullGroupSize - 1) / kModelCullGroupSize, 1, 1);
                },
            });
        };

        // Hi-Zミップチェーンの構築パス。**登録する場所が2つある**ため関数にしてある。
        // 深度プリパスが走るなら直後に(今フレームの深度から)、走らないならG-Bufferの後に。
        // 詳細は下の hiZPassRuns のコメント
        const auto addHiZPass = [&]()
        {
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "HiZ",
                .Reads = { m_GBufferDepth.get() },
                .Writes = { m_HiZTexture.get() },
                .Execute = [this](RHI::IRHICommandList* cmd)
                {
                    HiZConstants hizConstants{};
                    hizConstants.SrcSize = { m_RenderWidth, m_RenderHeight };
                    hizConstants.DstSize = { m_RenderWidth, m_RenderHeight };
                    cmd->UpdateBuffer(m_HiZConstantBuffer.get(), &hizConstants, sizeof(hizConstants));

                    cmd->SetComputePipelineState(m_HiZCopyPipelineState.get());
                    cmd->SetComputeConstantBuffer(0, m_HiZConstantBuffer.get());
                    cmd->SetComputeTexture(0, m_GBufferDepth.get());
                    cmd->SetComputeUnorderedAccessTexture(0, m_HiZTexture.get(), 0);
                    cmd->Dispatch((m_RenderWidth + 7) / 8, (m_RenderHeight + 7) / 8, 1);

                    cmd->SetComputePipelineState(m_HiZDownsamplePipelineState.get());
                    uint32_t hizSrcWidth = m_RenderWidth;
                    uint32_t hizSrcHeight = m_RenderHeight;
                    for (uint32_t mip = 1; mip < m_HiZMipLevels; ++mip)
                    {
                        const uint32_t hizDstWidth = std::max(1u, hizSrcWidth / 2);
                        const uint32_t hizDstHeight = std::max(1u, hizSrcHeight / 2);

                        hizConstants.SrcSize = { hizSrcWidth, hizSrcHeight };
                        hizConstants.DstSize = { hizDstWidth, hizDstHeight };
                        cmd->UpdateBuffer(m_HiZConstantBuffer.get(), &hizConstants, sizeof(hizConstants));
                        cmd->SetComputeConstantBuffer(0, m_HiZConstantBuffer.get());
                        cmd->SetComputeUnorderedAccessTexture(0, m_HiZTexture.get(), mip - 1);
                        cmd->SetComputeUnorderedAccessTexture(1, m_HiZTexture.get(), mip);
                        cmd->Dispatch((hizDstWidth + 7) / 8, (hizDstHeight + 7) / 8, 1);

                        hizSrcWidth = hizDstWidth;
                        hizSrcHeight = hizDstHeight;
                    }

                    // ここまで来たら全ミップに実データが入った。
                    // 【Executeの中で立てること】パスの登録だけでは実行されたことにならない
                    m_HiZValid = true;
                },
            });
        };

        // 深度プリパスぶんの判定。**プリパスより前**でなければ間接描画の引数が間に合わない。
        // 読めるHi-Zは前フレームのものなので、保守的に膨らませる従来の経路のまま
        addModelCullPass(
            "ModelCull", 0u,
            hiZFromDepthPrepass ? m_ModelCullPrepassCandidateCount : m_ModelCullCandidateCount,
            /*initializeBuffers=*/true, /*useCurrentFrameHiZ=*/false, occlusionPrevFrameEnabled);

        if (depthPrepassRuns)
        {
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "DepthPrepass",
                // 増幅シェーダーのHi-Zオクルージョンカリングが読む
                // (G-Bufferパスと同じ理由で循環にはならない)
                .Reads = { m_HiZTexture.get() },
                // レンダーターゲットは持たない(深度だけを書く)
                .DepthTarget = m_GBufferDepth.get(),
                // 間接描画の引数(直前のModelCullパスが書いたもの)
                .BufferReads = { m_ModelCullDrawArgsBuffer.get() },
                .Execute = [this, &gbufferViewport, &viewProj, modelCullIndirectActive,
                            occlusionCullingActive](RHI::IRHICommandList* cmd)
                {
                    cmd->SetViewport(gbufferViewport);
                    // Reverse-Zのため遠平面側(NDC z=0.0)。G-Bufferパスの代わりにここでクリアする
                    cmd->ClearDepth(0.0f);

                    // 増幅シェーダーのHi-Zオクルージョンカリング用(t8。GBufferMeshlet.hlsl)。
                    //
                    // 【プリパスでも張ること】このパスはG-Bufferとまったく同じ増幅シェーダーを
                    // 使うので、同じ判定を同じ入力で行わなければならない。
                    // **張り忘れてもエラーは出ない** ―― SRVのバインドはコマンドリスト側が
                    // シャドウで保持しており、別のパスが8番へ張ったテクスチャがそのまま残る。
                    // そうなるとプリパスとG-Bufferで間引くメッシュレットが食い違い、
                    // プリパスだけが描いた面は「深度はあるのに色が無い」穴になる
                    if (occlusionCullingActive)
                    {
                        cmd->SetTextureAllStages(8, m_HiZTexture.get());
                    }

                    RHI::IRHIPipelineState* currentPipelineState = nullptr;

                    // 1モデル1ドロー経路ぶんは、GPUが間引いた結果をそのまま発行する。
                    // 【下のCPUループより先に出す】どちらが先でも深度テストが前後関係を
                    // 決めるので絵は変わらないが、件数の多いこちらを先に流すほうが
                    // 後続のCPUループがGPUの実行と重なる
                    if (modelCullIndirectActive)
                    {
                        if (IssueModelCullIndirect(
                                cmd, kModelCullRegionPrepassOpaque, m_DepthPrepassMeshletPipelineState.get(),
                                currentPipelineState))
                        {
                            ++m_DrawCallsDepthPrepass;
                        }
                        if (IssueModelCullIndirect(
                                cmd, kModelCullRegionPrepassOpaqueMirrored,
                                m_DepthPrepassMeshletPipelineStateMirrored.get(), currentPipelineState))
                        {
                            ++m_DrawCallsDepthPrepass;
                        }
                        if (IssueModelCullIndirect(
                                cmd, kModelCullRegionPrepassCutout,
                                m_DepthPrepassMeshletCutoutPipelineState.get(), currentPipelineState))
                        {
                            ++m_DrawCallsDepthPrepass;
                        }
                        if (IssueModelCullIndirect(
                                cmd, kModelCullRegionPrepassCutoutMirrored,
                                m_DepthPrepassMeshletCutoutPipelineStateMirrored.get(), currentPipelineState))
                        {
                            ++m_DrawCallsDepthPrepass;
                        }
                    }
                    // G-Bufferと同じカメラなので、間引かれるモデルも同じになる
                    const FrustumPlanes prepassFrustum = ExtractFrustumPlanes(viewProj);
                    // インスタンシングのバッチと、まとめられなかった1体を同じ形で回す。
                    // 【G-Bufferとまったく同じ組を使うこと】まとめ方が食い違うと、
                    // 深度を書いた画素と色を書く画素がずれて穴が開く
                    GetInstanceDrawUnits(/*coarsestLOD=*/false, m_DrawUnitScratch);
                    for (const InstanceDrawUnit& unit : m_DrawUnitScratch)
                    {
                        const size_t instanceIndex = unit.InstanceIndex;
                        const Assets::ModelInstance& instance = *unit.Instance;
                        ++m_FrustumCullTested;
                        if (!IsAABBVisible(prepassFrustum, unit.WorldBoundsMin, unit.WorldBoundsMax))
                        {
                            ++m_FrustumCullCulled;
                            continue;
                        }

                        // モデルLOD。フェード中は2段を重ねる。
                        // 【G-Bufferとまったく同じ組・同じDitherFadeで描くこと】片方だけが捨てた画素は
                        // 「深度は書かれているのに色が書かれない」穴になる。
                        // バッチはフェード中でないものだけで構成されるので必ず1段(BuildInstanceBatches)
                        LODDraw lodDraws[2];
                        uint32_t lodDrawCount;
                        if (unit.IsBatch())
                        {
                            lodDraws[0] = { unit.Model, 1.0f };
                            lodDrawCount = 1;
                        }
                        else
                        {
                            lodDrawCount = GetLODDraws(instanceIndex, lodDraws);
                        }
                        for (uint32_t lodDrawIndex = 0; lodDrawIndex < lodDrawCount; ++lodDrawIndex)
                        {
                        const float lodDitherFade = lodDraws[lodDrawIndex].DitherFade;
                        const Assets::Model& lodModel = *lodDraws[lodDrawIndex].Model;

                        // G-Bufferが1ドローで描くモデルは、プリパスも同じ増幅/メッシュシェーダーで
                        // 描く。**同じ判断関数(ShouldUseModelMeshletPath)で経路を選ぶことが要点**で、
                        // 片方だけがメッシュシェーダーになると深度が一致しない。
                        //
                        // 不透明とカットアウトでピクセルシェーダーの有無が変わるため、
                        // カットアウトのマテリアルを持つモデルだけ2回に分ける。
                        // 持たないモデル(PLATEAUのタイルがそう)は1回で済む
                        if (ShouldUseModelMeshletPath(instance, lodModel))
                        {
                            // 間接描画が有効なら、この経路のドローは上でまとめて発行済み。
                            // **ここで描くと二重になる**
                            if (modelCullIndirectActive)
                            {
                                continue;
                            }

                            // 【フェード中は1ドロー経路のプリパスを外す】不透明用のPSOは
                            // ピクセルシェーダーを持たないためApplyLODDitherを通せず、
                            // 捨てるはずの画素まで深度を書いてG-Bufferとの食い違いで穴が開く。
                            // 早期Zが効かなくなるだけで、G-Buffer側が深度を書くので絵は壊れない
                            if (lodDitherFade < 1.0f)
                            {
                                continue;
                            }

                            if (!m_DepthPrepassMeshletPipelineState)
                            {
                                // メッシュレット版のPSOが無い。このモデルはプリパスから外す
                                // (早期Zが効かないだけで、G-Buffer側が深度を書くので絵は壊れない)
                                continue;
                            }

                            constexpr uint32_t kAmplificationGroupSize = 32;
                            const uint32_t groupCount =
                                (lodModel.TotalMeshletCount + kAmplificationGroupSize - 1)
                                / kAmplificationGroupSize;

                            const auto dispatchMeshletPrepass =
                                [&](RHI::IRHIPipelineState* pipelineState, uint32_t rejectMask, uint32_t requireMask)
                            {
                                if (!pipelineState)
                                {
                                    return;
                                }
                                if (pipelineState != currentPipelineState)
                                {
                                    cmd->SetPipelineState(pipelineState);
                                    cmd->SetConstantBuffer(0, m_FrameConstantBuffer.get());
                                    cmd->SetSamplerSet(m_MaterialSamplers.get());
                                    currentPipelineState = pipelineState;
                                }

                                const ObjectConstants objectConstants = MakeModelObjectConstants(
                                    instance, lodModel, m_EmissiveIntensity, m_OcclusionMapEnabled, rejectMask, requireMask,
                                    m_MeshletLODFrame);
                                cmd->UpdateBuffer(
                                    m_ObjectConstantBuffer.get(), &objectConstants, sizeof(objectConstants));
                                cmd->SetConstantBuffer(1, m_ObjectConstantBuffer.get());
                                cmd->DispatchMesh(groupCount, 1, 1);
                                ++m_DrawCallsDepthPrepass;
                            };

                            // 不透明ぶん(ピクセルシェーダー無し)。半透明とカットアウトを落とす
                            dispatchMeshletPrepass(
                                instance.IsMirrored ? m_DepthPrepassMeshletPipelineStateMirrored.get()
                                                    : m_DepthPrepassMeshletPipelineState.get(),
                                Assets::kGpuMaterialFlagTransparent | Assets::kGpuMaterialFlagCutout, 0);

                            // カットアウトぶん(clipを通す)。持たないモデルではこの回は発行しない
                            if (lodModel.HasCutoutMaterial)
                            {
                                dispatchMeshletPrepass(
                                    instance.IsMirrored ? m_DepthPrepassMeshletCutoutPipelineStateMirrored.get()
                                                        : m_DepthPrepassMeshletCutoutPipelineState.get(),
                                    Assets::kGpuMaterialFlagTransparent, Assets::kGpuMaterialFlagCutout);
                            }
                            continue;
                        }

                        for (const auto& mesh : lodModel.Meshes)
                        {
                            // BLENDマテリアルはG-Bufferに描かれないので深度も書かない
                            // (書くと後ろのものが消える)
                            if (mesh.IsTransparent)
                            {
                                continue;
                            }

                            // メッシュ単位のカリング。
                            // 【G-Bufferと同じ錐台・同じ判定にすること】ここで間引いたメッシュが
                            // G-Bufferでは描かれると、深度プリパスが埋めていない画素で早期Zが効かず
                            // 遅くなるだけで済むが、逆(プリパスで描いてG-Bufferで間引く)だと
                            // 描かれていないものの深度が残る
                            // 【バッチでは行わない】メッシュ単位のワールドAABBは
                            // 「インスタンス×メッシュ」の値で、まとめた相手のぶんが無い。
                            // G-Buffer側も同じ条件で外すので、両者の食い違いは起きない
                            if (!unit.IsBatch()
                                && !IsMeshVisibleWithStats(
                                    m_MeshCullingEnabled, prepassFrustum, instance, lodModel, mesh, m_MeshCullTested,
                                    m_MeshCullCulled))
                            {
                                continue;
                            }

                            // カットアウトは切り抜きを反映しないと深度に嘘が入る。
                            // ミラーリングは表裏判定が逆のPSOでないとカリングされる面が入れ替わり、
                            // G-Bufferと違う深度になってしまう。
                            // 【LODのフェード中もピクセルシェーダーが要る】カットアウトが無くても
                            // クロスディザで捨てる画素があるため、PS無しのPSOでは抜けない
                            const bool cutout = mesh.AlphaCutoff > 0.0f || lodDitherFade < 1.0f;
                            RHI::IRHIPipelineState* const wanted =
                                cutout ? (instance.IsMirrored ? m_DepthPrepassCutoutPipelineStateMirrored.get()
                                                              : m_DepthPrepassCutoutPipelineState.get())
                                       : (instance.IsMirrored ? m_DepthPrepassPipelineStateMirrored.get()
                                                              : m_DepthPrepassPipelineState.get());
                            if (wanted != currentPipelineState)
                            {
                                cmd->SetPipelineState(wanted);
                                cmd->SetConstantBuffer(0, m_FrameConstantBuffer.get());
                                cmd->SetSamplerSet(m_MaterialSamplers.get());
                                currentPipelineState = wanted;
                            }

                            ObjectConstants objectConstants =
                                MakeObjectConstants(instance, lodModel, mesh, m_EmissiveIntensity, m_OcclusionMapEnabled, m_MeshletLODFrame, lodDitherFade);
                            objectConstants.InstanceBase = unit.InstanceBase;
                            objectConstants.InstancingEnabled = unit.IsBatch() ? 1u : 0u;
                            cmd->UpdateBuffer(m_ObjectConstantBuffer.get(), &objectConstants, sizeof(objectConstants));
                            cmd->SetConstantBuffer(1, m_ObjectConstantBuffer.get());

                            // カットアウト以外はピクセルシェーダーを持たないためテクスチャも要らない
                            if (cutout)
                            {
                                cmd->SetTexture(0, mesh.BaseColorTexture);
                            }

                            // 【毎回張り直す】頂点シェーダー用SRVはt0の1本しかなく、
                            // ドローンショーが同じスロットを使う
                            if (unit.IsBatch())
                            {
                                cmd->SetVertexShaderResourceBuffer(0, m_ModelInstanceBuffer.get());
                            }

                            cmd->SetVertexBuffer(mesh.VertexBuffer.get());
                            cmd->SetIndexBuffer(mesh.IndexBuffer.get());
                            cmd->DrawIndexed(mesh.IndexCount, 0, 0, unit.InstanceCount);
                            ++m_DrawCallsDepthPrepass;
                        }
                        }   // モデルLODの段のループ
                    }
                },
            });
        }

        // 深度プリパスが書いた深度からHi-Zを作り、**今フレームのHi-Z**でG-Bufferぶんを判定する。
        // ここまで来ればプリパスは全不透明ジオメトリの深度を書き終えている ――
        // 1フレーム遅れも保守的な膨張も要らず、判定はそのまま正確になる。
        //
        // 【プリパスの深度は G-Buffer が描くものの部分集合】LODのフェード中のモデルなど、
        // プリパスから外れるものがある。遮蔽物が減る方向なので間引きが甘くなるだけで、
        // 見えているものを消す側へは倒れない
        if (hiZFromDepthPrepass)
        {
            addHiZPass();
            addModelCullPass(
                "ModelCullGBuffer", m_ModelCullPrepassCandidateCount,
                m_ModelCullCandidateCount - m_ModelCullPrepassCandidateCount,
                /*initializeBuffers=*/false, /*useCurrentFrameHiZ=*/true, occlusionCullingActive);
        }

        // --- ジオメトリパス: G-Bufferへ書き込む(常に指定した内部解像度) ---
        graph.AddPass(Core::RenderGraphPassDesc{
            .Name = "GBuffer",
            // 増幅シェーダーのHi-Zオクルージョンカリングが読む(Stage 5-2)。
            //
            // 【循環にならない】Hi-Zパスは「G-Buffer深度を読んでHi-Zを書く」ので、依存だけ見ると
            // 互いを参照しているように見える。しかしRenderGraphのReadsは「それより前に登録された
            // 書き手」がいるときにだけ辺を張る規則で、Hi-Zパスの登録はこのパスより後なので
            // 辺は張られない(RenderGraph::ResolveExecutionOrder)。実行順も登録順のまま、
            // 読むのは前フレームに書かれた内容になる ―― それがこの判定の前提そのもの
            .Reads = { m_HiZTexture.get() },
            // 深度プリパス(直前に登録される)を通したときは、ここへ来る時点で深度が埋まっており、
            // PSOのDepthAllowEqual(GREATER_EQUAL)によって最前面の断片だけがテストを通る。
            //
            // 6枚目のbent normalまで含め、並びはGBuffer.hlslのPSOutputおよび
            // CreatePrecisionDependentPipelineStatesのRenderTargetFormatsと一致させること
            .RenderTargets = { m_GBufferAlbedo.get(), m_GBufferNormal.get(), m_GBufferMaterial.get(),
                               m_GBufferEmissive.get(), m_GBufferVelocity.get(), m_GBufferBentNormal.get() },
            .DepthTarget = m_GBufferDepth.get(),
            // 間接描画の引数を読む(ModelCullパスが書いたもの)
            .BufferReads = { m_ModelCullDrawArgsBuffer.get() },
            .Execute = [this, &gbufferViewport, depthPrepassRuns, &viewProj, occlusionCullingActive,
                        meshletCullStatsActive, modelCullIndirectActive](RHI::IRHICommandList* cmd)
            {
                // カリング統計のカウンタを0へ戻す。増幅シェーダーは加算しかしないので、
                // 戻さないとフレームをまたいで積み上がる。
                //
                // 【このパスの中で行う理由】数えるのも読み戻すのもこのパスなので、
                // 「クリア→数える→コピー」を1か所にまとめたほうが順序を追いやすい。
                // 別パスに分けるとRenderGraphの登録順への依存が1本増える
                if (meshletCullStatsActive)
                {
                    cmd->ClearUnorderedAccessBufferUint(m_MeshletCullStatsBuffer.get(), 0);
                }

                cmd->SetViewport(gbufferViewport);
                // ClearRenderTargetはバインド済みの全レンダーターゲットを同じ色でクリアするため、
                // 速度バッファもここで0(=動いていない)になる。ジオメトリが描かれない画素
                // (空)の速度は0のまま残るが、空はカメラ回転で動くのでTAA側で別途補う(TAA.hlsl参照)
                cmd->ClearRenderTarget({ 0.0f, 0.0f, 0.0f, 0.0f });
                // Reverse-Zのため遠平面側(NDC z=0.0)にクリアする(GBuffer.hlsl参照)。
                // 【深度プリパスを通したときはクリアしない】プリパスが既に正しい深度を
                // 書いており、ここで消すとGREATER_EQUALのテストが全断片を通してしまい
                // プリパスが無意味になる(クリアはプリパス側が行う)
                if (!depthPrepassRuns)
                {
                    cmd->ClearDepth(0.0f);
                }

                cmd->SetPipelineState(m_GBufferPipelineState.get());
                cmd->SetConstantBuffer(0, m_FrameConstantBuffer.get());
                cmd->SetSamplerSet(m_MaterialSamplers.get());

                // 増幅シェーダーのHi-Zオクルージョンカリング用(t8。GBufferMeshlet.hlsl)。
                //
                // 【SetTextureではなくSetTextureAllStagesを使う】SetTextureはリソースを
                // PIXEL_SHADER_RESOURCEへしか遷移させず、増幅シェーダーからは読めない。
                //
                // 【パスの先頭で1回だけでよい】SRVのバインドはDX12CommandList側がシャドウで
                // 保持しており、SetPipelineStateで無効化されるのはルート引数だけで、
                // 次のDrawの直前にFlushPendingSrvWritesが張り直す。
                // ここでPSOを切り替えても、下のbindPipelineStateがCBVとサンプラーを
                // 張り直すのと違って、テクスチャは張り直す必要が無い。
                //
                // 【判定しないフレームではバインドもしない】不要な状態遷移を1つ減らすと同時に、
                // 「バインドされていないのに間引き率が出た」という取り違えを起こせなくする
                if (occlusionCullingActive)
                {
                    cmd->SetTextureAllStages(8, m_HiZTexture.get());
                }

                // ミラーリング(Worldの行列式が負)されたインスタンス・水面(ModelInstance::IsWater)
                // インスタンスの組み合わせ(4通り)に応じてパイプラインを切り替える。上で通常の
                // パイプラインを先にバインドしてあるため、どちらも含まないシーンでは以下のラムダは
                // 一度も切り替えを行わず、発行されるコマンド列はこの機能の追加前と完全に同一になる。
                // DX12のSetPipelineStateはルートシグネチャを張り直して既存のバインドを
                // 無効化するので、切り替えたときはパス共通のバインドもやり直す
                //
                // メッシュレット経路(useMeshlet)はさらにその上の分岐。頂点シェーダー版と
                // 同じG-Bufferへ同じ内容を書くので、切り替えても見た目は一致する
                RHI::IRHIPipelineState* currentPipelineState = m_GBufferPipelineState.get();
                const auto bindPipelineState = [&](bool mirrored, bool water, bool useMeshlet)
                {
                    RHI::IRHIPipelineState* wanted = nullptr;
                    if (useMeshlet)
                    {
                        // デバッグ表示が有効ならメッシュレットごとの色分けPSOを使う。
                        // 用意できていない場合(作成失敗)は通常のメッシュレットPSOへ落とす
                        const bool debugView = m_MeshletDebugViewEnabled && m_GBufferMeshletDebugPipelineState;
                        wanted = debugView
                            ? (mirrored ? m_GBufferMeshletDebugPipelineStateMirrored.get()
                                        : m_GBufferMeshletDebugPipelineState.get())
                            : (mirrored ? m_GBufferMeshletPipelineStateMirrored.get()
                                        : m_GBufferMeshletPipelineState.get());
                    }
                    else
                    {
                        wanted = water
                            ? (mirrored ? m_GBufferWaterPipelineStateMirrored.get() : m_GBufferWaterPipelineState.get())
                            : (mirrored ? m_GBufferPipelineStateMirrored.get() : m_GBufferPipelineState.get());
                    }
                    if (wanted == currentPipelineState)
                    {
                        return;
                    }
                    cmd->SetPipelineState(wanted);
                    cmd->SetConstantBuffer(0, m_FrameConstantBuffer.get());
                    cmd->SetSamplerSet(m_MaterialSamplers.get());
                    currentPipelineState = wanted;
                };

                // 視錐台の外にあるモデルは丸ごと飛ばし、通ったモデルの中でさらに
                // 視錐台の外にあるメッシュを飛ばす(下のメッシュのループ)。
                //
                // 【2段になっている理由】モデル単位だけだと、1モデルに多数のメッシュを持つ
                // アセット(Bistro、Emerald Square、PLATEAUのLOD2タイル)では1つも間引けない
                // ―― モデル全体のAABBが視錐台と交差する限り全メッシュを描くしかないため。
                // .kmodel v10がメッシュ単位のAABBを持つようになったので、もう一段入れてある。
                // 効くシーンが逆(モデル単位は.kmodelを多数並べるシーンで効く)なので、
                // 統計も別のカウンタで数える
                const FrustumPlanes frustum = ExtractFrustumPlanes(viewProj);

                // 1モデル1ドロー経路ぶんは、GPUが間引いた結果をそのまま発行する。
                // 【下のCPUループより先に出す】どちらが先でも深度テストが前後関係を
                // 決めるので絵は変わらないが、件数の多いこちらを先に流すほうが
                // 後続のCPUループがGPUの実行と重なる。
                // 選ぶPSOはbindPipelineState(mirrored, false, true)と同じもの
                if (modelCullIndirectActive)
                {
                    const bool meshletDebug = m_MeshletDebugViewEnabled && m_GBufferMeshletDebugPipelineState;
                    if (IssueModelCullIndirect(
                            cmd, kModelCullRegionGBuffer,
                            meshletDebug ? m_GBufferMeshletDebugPipelineState.get()
                                         : m_GBufferMeshletPipelineState.get(),
                            currentPipelineState))
                    {
                        ++m_DrawCallsGBuffer;
                    }
                    if (IssueModelCullIndirect(
                            cmd, kModelCullRegionGBufferMirrored,
                            meshletDebug ? m_GBufferMeshletDebugPipelineStateMirrored.get()
                                         : m_GBufferMeshletPipelineStateMirrored.get(),
                            currentPipelineState))
                    {
                        ++m_DrawCallsGBuffer;
                    }
                }

                // インスタンシングのバッチと、まとめられなかった1体を同じ形で回す。
                // 【深度プリパスとまったく同じ組を使うこと】まとめ方が食い違うと穴が開く
                GetInstanceDrawUnits(/*coarsestLOD=*/false, m_DrawUnitScratch);
                for (const InstanceDrawUnit& unit : m_DrawUnitScratch)
                {
                    const size_t instanceIndex = unit.InstanceIndex;
                    const Assets::ModelInstance& instance = *unit.Instance;
                    ++m_FrustumCullTested;
                    if (!IsAABBVisible(frustum, unit.WorldBoundsMin, unit.WorldBoundsMax))
                    {
                        ++m_FrustumCullCulled;
                        continue;
                    }

                    // モデルLOD。フェード中は2段を重ねる。
                    // 【深度プリパスとまったく同じ組・同じDitherFadeであること】UpdateModelLODが
                    // フレーム先頭で1回だけ決めた結果を両方が引くので、ここで距離を測り直さない。
                    // バッチはフェード中でないものだけで構成されるので必ず1段(BuildInstanceBatches)
                    LODDraw lodDraws[2];
                    uint32_t lodDrawCount;
                    if (unit.IsBatch())
                    {
                        lodDraws[0] = { unit.Model, 1.0f };
                        lodDrawCount = 1;
                    }
                    else
                    {
                        lodDrawCount = GetLODDraws(instanceIndex, lodDraws);
                    }
                    for (uint32_t lodDrawIndex = 0; lodDrawIndex < lodDrawCount; ++lodDrawIndex)
                    {
                    const float lodDitherFade = lodDraws[lodDrawIndex].DitherFade;
                    const Assets::Model& lodModel = *lodDraws[lodDrawIndex].Model;

                    // モデル全体を1回のDispatchMeshで描ける場合はメッシュのループへ入らない。
                    // マテリアルはメッシュシェーダーが出力した番号でピクセルシェーダーが引くため、
                    // メッシュごとのSetTextureも定数バッファの更新も要らない。
                    //
                    // 【BLENDだけは増幅シェーダーが落とす】半透明はG-Bufferに書かず専用の
                    // Transparentパスでフォワードシェーディングする。ドローを分けられない以上、
                    // メッシュレット単位のふるい分けでしか除外できない
                    if (ShouldUseModelMeshletPath(instance, lodModel))
                    {
                        // 間接描画が有効なら、この経路のドローはこのループの前に
                        // まとめて発行済み。**ここで描くと二重になる**
                        if (modelCullIndirectActive)
                        {
                            continue;
                        }

                        bindPipelineState(instance.IsMirrored, false, true);

                        const ObjectConstants objectConstants = MakeModelObjectConstants(
                            instance, lodModel, m_EmissiveIntensity, m_OcclusionMapEnabled,
                            Assets::kGpuMaterialFlagTransparent, 0, m_MeshletLODFrame,
                            /*countCullStats=*/true, lodDitherFade);
                        cmd->UpdateBuffer(m_ObjectConstantBuffer.get(), &objectConstants, sizeof(objectConstants));
                        cmd->SetConstantBuffer(1, m_ObjectConstantBuffer.get());

                        // 起動するのは「モデル全体のメッシュレット数 ÷ 増幅シェーダーのグループサイズ」。
                        // 実際にラスタライズされるのはカリングとふるい分けを生き延びたぶんに絞られる
                        constexpr uint32_t kAmplificationGroupSize = 32; // GBufferMeshlet.hlslと一致させること
                        const uint32_t groupCount =
                            (lodModel.TotalMeshletCount + kAmplificationGroupSize - 1) / kAmplificationGroupSize;
                        cmd->DispatchMesh(groupCount, 1, 1);
                        ++m_DrawCallsGBuffer;
                        continue;
                    }

                    for (const auto& mesh : lodModel.Meshes)
                    {
                        // BLENDマテリアル(mesh.IsTransparent)はG-Bufferに書き込まず、専用のTransparentパスで
                        // フォワードシェーディングする(G-Bufferのアルファは常に1.0で半透明合成ができないため)
                        if (mesh.IsTransparent)
                        {
                            continue;
                        }

                        // メッシュ単位のカリング。深度プリパスとまったく同じ錐台・同じ判定
                        // (片方だけで間引くと深度とG-Bufferが食い違う)
                        // 【バッチでは行わない】理由と、深度プリパスと条件を揃えることの
                        // 必要性はプリパス側の同じ箇所を参照
                        if (!unit.IsBatch()
                            && !IsMeshVisibleWithStats(
                                m_MeshCullingEnabled, frustum, instance, lodModel, mesh, m_MeshCullTested, m_MeshCullCulled))
                        {
                            continue;
                        }
                        // 【ここへ来た時点でメッシュレット経路は使わない】上のモデル単位の
                        // 判定を通らなかったインスタンス(水面、メッシュレットを持たない
                        // メッシュが混ざるモデル、メッシュレット描画が無効)なので、
                        // モデル全体を従来の頂点シェーダーで描く。
                        // **メッシュ単位でメッシュレット経路へ入れてはいけない** ――
                        // 深度プリパスは同じ判断関数(ShouldUseModelMeshletPath)で経路を選ぶため、
                        // ここで食い違うとプリパスの深度とG-Bufferの深度が一致しなくなる
                        bindPipelineState(instance.IsMirrored, instance.IsWater, false);

                        ObjectConstants objectConstants =
                            MakeObjectConstants(instance, lodModel, mesh, m_EmissiveIntensity, m_OcclusionMapEnabled, m_MeshletLODFrame, lodDitherFade);
                        objectConstants.InstanceBase = unit.InstanceBase;
                        objectConstants.InstancingEnabled = unit.IsBatch() ? 1u : 0u;
                        cmd->UpdateBuffer(m_ObjectConstantBuffer.get(), &objectConstants, sizeof(objectConstants));
                        cmd->SetConstantBuffer(1, m_ObjectConstantBuffer.get());

                        cmd->SetTexture(0, mesh.BaseColorTexture);
                        cmd->SetTexture(1, mesh.NormalTexture);
                        cmd->SetTexture(2, mesh.MetallicRoughnessTexture);
                        cmd->SetTexture(3, mesh.EmissiveTexture);
                        cmd->SetTexture(5, mesh.OcclusionTexture);
                        cmd->SetTexture(6, mesh.BentNormalTexture);
                        if (instance.IsWater)
                        {
                            // Water.hlslのPSMainだけが読むt7。通常のGBuffer PSOはt7を宣言していないため
                            // 水面以外のインスタンスではバインドしない。
                            // 【t6は使えない】t6はbent normal(34章)が使う
                            cmd->SetTexture(7, m_WaterNormalMapTexture.get());
                        }

                        // 【毎回張り直す】頂点シェーダー用SRVはt0の1本しかなく、
                        // ドローンショーが同じスロットを使う
                        if (unit.IsBatch())
                        {
                            cmd->SetVertexShaderResourceBuffer(0, m_ModelInstanceBuffer.get());
                        }

                        cmd->SetVertexBuffer(mesh.VertexBuffer.get());
                        cmd->SetIndexBuffer(mesh.IndexBuffer.get());
                        cmd->DrawIndexed(mesh.IndexCount, 0, 0, unit.InstanceCount);
                        ++m_DrawCallsGBuffer;
                    }
                    }   // モデルLODの段のループ
                }

                // 数え終わったカウンタを受け皿へ写す。読むのは数フレーム後(下のリングの説明参照)。
                //
                // 【全描画の後で行うこと】ここより前に置くと、まだ発行していない
                // DispatchMeshの寄与が入らない。コピーはUNORDERED_ACCESS→COPY_SOURCEの
                // 状態遷移を伴い、その遷移が増幅シェーダーの書き込み完了も保証する
                if (meshletCullStatsActive)
                {
                    cmd->CopyBufferToReadback(
                        m_MeshletCullStatsReadback[m_MeshletCullStatsRingIndex].get(),
                        m_MeshletCullStatsBuffer.get(),
                        static_cast<uint32_t>(sizeof(uint32_t)) * kMeshletCullStatsCount);
                }
            },
        });

        // --- モデル単位GPUカリングのカウンタを受け皿へ写すパス ---
        //
        // 【ディスパッチと同じパスに置かない・すぐ後ろにも置かない】
        // UNORDERED_ACCESS→COPY_SOURCE の遷移は直前のUAV書き込みを流し切る。
        // ディスパッチの直後に置くと、その待ちがModelCullパスのGPU時間に乗って
        // 「671スレッドの判定に1ms」というありえない値に見える。
        // G-Bufferパスより後ろに置けば、待ちは描画と重なって消える
        // (メッシュレット統計のコピーが重いG-Bufferパスの末尾にあって
        //  表面化していないのと同じ構図)。
        //
        // **別パスにしてあるので、その待ち自体も独立した数値として見られる**
        if (modelCullReady)
        {
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "ModelCullReadback",
                .BufferReads = { m_ModelCullCounterBuffer.get() },
                .Execute = [this](RHI::IRHICommandList* cmd)
                {
                    cmd->CopyBufferToReadback(
                        m_ModelCullReadback[m_ModelCullRingIndex].get(), m_ModelCullCounterBuffer.get(),
                        static_cast<uint32_t>(sizeof(uint32_t)) * kModelCullCounterCount);
                },
            });
        }

        // --- 自前ソフトウェアラスタライザパス(46章): 三角形をコンピュートシェーダーで
        //     ラスタライズし、専用のバッファへ深度・法線・フラット陰影を書く ---
        //
        // 【既存の描画には一切寄与しない】比較用の独立した経路で、出力を読むのは
        // DebugView::SoftwareRaster* だけ。GBufferパスの直後に登録しているのは、
        // 依存関係が無いパス同士の実行順が登録順で決まるため(プロファイラの並びを揃える)。
        //
        // 【なぜハードウェアと比べられるのか】GBufferパスとまったく同じjitteredProjを渡すため、
        // 深度は丸め誤差とフィルルールの差を除いて一致するはず。差が面全体に出たら
        // 座標変換の間違いで、シルエットの±1画素ならフィルルールの差(想定内)
        const bool softwareRasterPassRuns = m_SoftwareRasterEnabled && m_SoftwareRasterAvailable &&
                                            m_SoftwareRasterVisibilityBuffer && !m_Scene.Instances.empty();
        if (softwareRasterPassRuns)
        {
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "SWRaster",
                .Writes = { m_SoftwareRasterColor.get(), m_SoftwareRasterDepth.get(), m_SoftwareRasterNormal.get() },
                .BufferReads = { m_SoftwareRasterMeshInfoBuffer.get() },
                .BufferWrites = { m_SoftwareRasterVisibilityBuffer.get(),
                                  m_SoftwareRasterLargeEntriesBuffer.get(),
                                  m_SoftwareRasterIndirectArgsBuffer.get() },
                .Execute = [this, viewProj, sunLighting](RHI::IRHICommandList* cmd)
                {
                    ExecuteSoftwareRasterPass(cmd, viewProj, sunLighting.Direction);
                },
            });
        }

        // --- Hi-Zミップチェーン構築パス: G-Buffer深度から1x1までのミップチェーンをコンピュートシェーダーで
        //     構築する ---
        //
        // 【消費者がいるフレームだけ登録する】このパスは「コピー1回 + ミップ段数-1回のディスパッチ」
        // (1280x720なら計11回)で、Intel UHD 620での実測で1.19〜1.21ms、GPUフレーム時間30msの
        // 約4%を占める。以前は消費者がPresentパスのDebugView::HiZ表示しか無かったため、
        // その表示中だけに絞ってこの4%を削った経緯がある。
        //
        // Stage 5-2で2人目の消費者(増幅シェーダーのオクルージョンカリング)が付いたが、
        // **条件を丸ごと外してはいけない** ―― 外すと上の節約がそのまま戻る。
        // 「消費者がいるフレームか」へ条件を書き換えるのが正しい。メッシュシェーダー非対応の
        // 環境ではocclusionCullingActiveが常にfalseになり、従来どおり1msを払わずに済む。
        //
        // 【1フレーム遅れになる】このパスはGBufferパスより後に登録されるため、増幅シェーダーが
        // 読むのは前フレームのHi-Zになる。判定側はそれを前提に、前フレームのビュー射影行列
        // (FrameConstants::PrevViewProj)で投影し、球を保守的に膨らませて吸収している
        // (GBufferMeshlet.hlslのIsMeshletOccluded参照)
        // Hi-Zミップチェーンの構築。**このパスは条件付きで走らせる。**
        // 1280x720ならコピー1回+ミップ段数-1回のディスパッチ(計11回)で、Intel UHD 620での
        // 実測で1.19〜1.21ms、GPUフレーム時間30msの約4%を占める。消費者がいないフレームでは
        // その4%をまるごと捨てることになるので、条件を丸ごと外してはいけない。
        //
        // 【登録する場所は2つある】深度プリパスが走るなら**その直後**に登録済みで
        // (hiZFromDepthPrepass。今フレームの深度から作り、同じフレームのG-Bufferが読む)、
        // ここへは来ない。プリパスが走らないフレームだけ、従来どおりG-Bufferの後で作る ――
        // その場合に読めるのは次フレームで、増幅シェーダーは前フレームのビュー射影行列で
        // 投影し、球を保守的に膨らませて視差を吸収する(GBufferMeshlet.hlslのIsMeshletOccluded)
        const bool hiZPassRuns = (m_DebugView == DebugView::HiZ) || occlusionCullingActive;
        if (!hiZPassRuns)
        {
            // このフレームで作らないなら、次フレームのHi-Zは「何フレームか前の、別のカメラ位置で
            // 撮った深度」になる。それで遮蔽を判定すると見えているものを消す。
            // 【トグルを往復させると必ず起きる】メッシュレット描画やオクルージョンを一度OFFにして
            // ONへ戻す操作で踏むので、"構築しなかった"を必ず記録しておく
            m_HiZValid = false;
        }
        if (hiZPassRuns && !hiZFromDepthPrepass)
        {
            addHiZPass();
        }

        // --- タイルライトカリングパス: 画面を16x16のタイルに分け、タイルごとに「そのタイルに届くライト」の
        //     インデックスリストをコンピュートシェーダーで作る。直接光パスはそのリストだけをループする。
        //     BufferReads/BufferWritesを宣言しているのは、このパスと直接光パスがどちらもm_GBufferDepthを
        //     Readsするだけの「読み手同士」で、テクスチャの依存だけでは両者の間に順序が張られないため ---
        if (m_LightCullingEnabled)
        {
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "LightCull",
                .Reads = { m_GBufferDepth.get() },
                .BufferReads = { m_LightBuffer.get() },
                .BufferWrites = { m_LightTileBuffer.get() },
                .Execute = [this, &gpuLights, viewMatrix, jitteredProj](RHI::IRHICommandList* cmd)
                {
                    LightCullingConstants cullingConstants{};
                    DirectX::XMStoreFloat4x4(
                        &cullingConstants.View, DirectX::XMMatrixTranspose(viewMatrix));
                    cullingConstants.TileParams =
                    {
                        m_LightTileCountX,
                        m_LightTileCountY,
                        static_cast<uint32_t>(gpuLights.size()),
                        kLightTileCapacity,
                    };
                    cullingConstants.RenderSize = { m_RenderWidth, m_RenderHeight, 0u, 0u };

                    // タイル錐台の側面を組み立てるのに射影行列の(0,0)/(1,1)成分が要る。
                    // 深度リニアライズ定数(z/w)は直接光パスへ渡しているものと同じ。
                    // ここで読む_11/_22/_33/_43はいずれもTAAのジッター(_31/_32のみを書き換える)では
                    // 変化しないが、深度バッファを描いたときと同じ行列から導くという規約に揃えている
                    DirectX::XMFLOAT4X4 projection;
                    DirectX::XMStoreFloat4x4(&projection, jitteredProj);
                    cullingConstants.ProjParams =
                    {
                        projection._11,
                        projection._22,
                        projection._33,
                        projection._43,
                    };

                    cmd->UpdateBuffer(m_LightCullingConstantBuffer.get(), &cullingConstants, sizeof(cullingConstants));

                    cmd->SetComputePipelineState(m_LightCullingPipelineState.get());
                    cmd->SetComputeConstantBuffer(0, m_LightCullingConstantBuffer.get());
                    cmd->SetComputeShaderResourceBuffer(0, m_LightBuffer.get());
                    cmd->SetComputeTexture(1, m_GBufferDepth.get());
                    cmd->SetComputeUnorderedAccessBuffer(0, m_LightTileBuffer.get());
                    cmd->Dispatch(m_LightTileCountX, m_LightTileCountY, 1);
                },
            });
        }

        // --- MegaLightsの候補プールパス: タイルごとに「そこへ届くライト」を走査し、寄与に比例した
        //     確率でK灯を重みつきで抽出する。到達判定はタイルライトカリングと共有している
        //     (TileLightCulling.hlsli)ので、両者の「届いた灯数」は一致するはず。
        //     現段階では参照実装がこれを読まない(全灯を回す)ため、出力の消費者はまだいない ---
        if (ShouldRunMegaLights() && m_MegaLightsTilePoolBuffer && m_MegaLightsTilePoolPipelineState)
        {
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "MegaLightsPool",
                .Reads = { m_GBufferDepth.get() },
                .BufferReads = { m_LightBuffer.get() },
                .BufferWrites = { m_MegaLightsTilePoolBuffer.get() },
                .Execute = [this, &gpuLights, viewMatrix, jitteredProj](RHI::IRHICommandList* cmd)
                {
                    MegaLightsTilePoolConstants poolConstants{};
                    DirectX::XMStoreFloat4x4(&poolConstants.View, DirectX::XMMatrixTranspose(viewMatrix));
                    poolConstants.TileParams =
                    {
                        m_LightTileCountX,
                        m_LightTileCountY,
                        static_cast<uint32_t>(gpuLights.size()),
                        kMegaLightsTilePoolCapacity,
                    };
                    poolConstants.RenderSize = { m_RenderWidth, m_RenderHeight, 0u, 0u };

                    // タイル錐台の組み立てと深度のリニアライズに使う。LightCullパスと同じ行列から
                    // 同じ成分を取る(判定を共有している以上、入力もずらしてはいけない)
                    DirectX::XMFLOAT4X4 projection;
                    DirectX::XMStoreFloat4x4(&projection, jitteredProj);
                    poolConstants.ProjParams =
                    {
                        projection._11,
                        projection._22,
                        projection._33,
                        projection._43,
                    };
                    // 候補を毎フレーム引き直すための種。TAAのフレーム番号を流用する
                    // (単調増加していればよく、ジッターの位相とは無関係)
                    poolConstants.PoolParams = { m_TAAFrameIndex, 0u, 0u, 0u };

                    cmd->UpdateBuffer(m_MegaLightsTilePoolConstantBuffer.get(), &poolConstants, sizeof(poolConstants));

                    cmd->SetComputePipelineState(m_MegaLightsTilePoolPipelineState.get());
                    cmd->SetComputeConstantBuffer(0, m_MegaLightsTilePoolConstantBuffer.get());
                    cmd->SetComputeShaderResourceBuffer(0, m_LightBuffer.get());
                    cmd->SetComputeTexture(1, m_GBufferDepth.get());
                    // UAVはDispatch直後に解除されるため毎回バインドし直す
                    cmd->SetComputeUnorderedAccessBuffer(0, m_MegaLightsTilePoolBuffer.get());
                    cmd->Dispatch(m_LightTileCountX, m_LightTileCountY, 1);
                },
            });
        }

        // --- MegaLightsパス: ポイント/スポットライトの直接光を専用パスで求め、HDRで書き出す。
        //     直後の直接光パスがt7でこれを読み、自分のライトループは回さない。
        //
        //     【必ず直接光パスより前に登録すること】RenderGraphの依存解決は登録順を1回だけ舐める
        //     前方走査で、Readsは自分より前に登録された書き手しか見つけられない。後ろに置くと
        //     辺が張られず、依存なし同士は登録順で実行されるため直接光パスが先に走り、
        //     **前フレームの残骸を読む**(初回は未初期化のfp16でNaNが伝播する) ---
        // 確率的サンプリングが t7 で読む候補プール。参照実装のフレームや、まだ確保していない
        // 環境では読まれないダミーとしてライトグリッドを張る(DX12はPSO切替でルート引数が
        // 無効化されるため、シェーダが宣言しているリソースは必ず何かをバインドする必要がある)
        RHI::IRHIBuffer* const tilePoolBufferForBinding =
            m_MegaLightsTilePoolBuffer ? m_MegaLightsTilePoolBuffer.get() : m_LightTileBuffer.get();

        if (ShouldRunMegaLights() && m_MegaLightsMode == MegaLightsMode::Reference)
        {
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "MegaLights",
                .Reads =
                {
                    m_GBufferAlbedo.get(), m_GBufferNormal.get(), m_GBufferMaterial.get(), m_GBufferDepth.get(),
                    // スペキュラのエネルギー補正でEssを引く。Readsへ挙げることでBRDFLUTBakeパス
                    // (このLUTの書き手)より後ろに順序付けられる
                    m_BRDFLUTTexture.get(),
                },
                .Writes = { m_MegaLightsTexture.get() },
                // ライトリストはグラフの外(UpdateBuffer)で更新済みだが、読むものは宣言しておく
                // というこのコードベースの規約に従う。候補プールは確率的サンプリングのときだけ
                // 読むが、宣言しておくことで候補プールパスより後ろへ順序付けられる
                // (参照実装のフレームでは辺が1本余分に張られるだけで無害)
                .BufferReads = { m_LightBuffer.get(), tilePoolBufferForBinding },
                .Execute = [this, &gpuLights, tilePoolBufferForBinding](RHI::IRHICommandList* cmd)
                {
                    MegaLightsConstants megaLightsConstants{};
                    megaLightsConstants.Params0 =
                    {
                        m_RenderWidth,
                        m_RenderHeight,
                        static_cast<uint32_t>(std::max(0, m_MegaLightsShadowRayCount)),
                        static_cast<uint32_t>(gpuLights.size()),
                    };
                    // 球光源のサンプル列を毎フレーム回す種。確率的サンプリング側と同じ
                    // フレーム番号を使う(あちらは Params1.w)
                    megaLightsConstants.Params1 = { m_TAAFrameIndex, 0u, 0u, 0u };
                    cmd->UpdateBuffer(m_MegaLightsConstantBuffer.get(), &megaLightsConstants,
                                      sizeof(megaLightsConstants));

                    cmd->SetComputePipelineState(m_MegaLightsReferencePipelineState.get());
                    cmd->SetComputeConstantBuffer(0, m_FrameConstantBuffer.get());
                    cmd->SetComputeConstantBuffer(1, m_MegaLightsConstantBuffer.get());

                    // BRDF積分LUTをColorSampler(s1、Linear+Clamp)で引くためサンプラーを張る。
                    // 直前のパスのバインドが残っていることに依存しない
                    cmd->SetComputeSamplerSet(m_ScreenSpaceSamplers.get());

                    // レジスタ割り当てはMegaLightsReference.hlsl側の宣言と一致させること
                    cmd->SetComputeAccelerationStructure(0, m_RaytracingScene.GetTopLevelAS());
                    cmd->SetComputeTexture(1, m_GBufferNormal.get());
                    cmd->SetComputeTexture(2, m_GBufferDepth.get());
                    cmd->SetComputeTexture(3, m_GBufferAlbedo.get());
                    cmd->SetComputeTexture(4, m_GBufferMaterial.get());
                    cmd->SetComputeTexture(5, m_BRDFLUTTexture.get());
                    // ライトが0灯のフレームでも必ずバインドする(DX12はSetPipelineStateのたびに
                    // ルート引数が無効化されるため、シェーダが宣言しているリソースを未バインドで
                    // Dispatchすることになる)
                    cmd->SetComputeShaderResourceBuffer(6, m_LightBuffer.get());
                    // 候補プール。参照実装は宣言していないが、DX12はPSO切替でルート引数が
                    // 無効化されるため、確率的サンプリングのときは必ずバインドする必要がある。
                    // 常に張っても害は無いので分岐させない
                    cmd->SetComputeShaderResourceBuffer(7, tilePoolBufferForBinding);

                    // UAVはDispatch直後に解除されるため毎回バインドし直す(IRHICommandList.h参照)
                    cmd->SetComputeUnorderedAccessTexture(0, m_MegaLightsTexture.get());
                    cmd->Dispatch((m_RenderWidth + 7) / 8, (m_RenderHeight + 7) / 8, 1);
                },
            });
        }

        // --- 確率的サンプリングは2パスに分かれる ---
        //   MegaLightsInitial : 候補プールからRISで1灯へ絞り、**リザーバ**として書く(色は作らない)
        //   MegaLightsShade   : そのリザーバへ影レイを1本撃ち、選ばれた確率で割り戻してHDRを書く
        //
        // 【なぜ1パスにまとめないのか】時間・空間の再利用は「どの灯を選んだか」を持ち回って
        // 現フレームで評価し直す形でしか書けない。選択とシェードが混ざっていると再利用の段を
        // 差し込む場所が無い。分けておけば両者の間に挟むだけで済む
        //
        // 【クアッド共有(手法3)は Initial をそのまま共有する】違うのは後段だけで、
        // 時間・空間再利用を挟まずに Resolve が2x2の4標本を平均する。
        // Initial を共有していることが陽性対照の土台になる ―― 共有を切った手法3は、
        // 手法2から再利用を外した構成と画素単位で一致するはず
        const bool megaLightsQuadShared =
            ShouldRunMegaLights() && m_MegaLightsMode == MegaLightsMode::QuadShared;
        if (ShouldRunMegaLights() &&
            (m_MegaLightsMode == MegaLightsMode::Stochastic || megaLightsQuadShared))
        {
            // 2パスで同じ定数バッファを共有する。中身はグラフ構築のこの時点で確定しているので、
            // Initial側のExecuteで1回だけ更新すればよい
            const auto buildStochasticConstants =
                [this, &jitteredProj, megaLightsQuadShared](uint32_t spatialIteration)
            {
                MegaLightsStochasticConstants stochasticConstants{};
                stochasticConstants.Params0 =
                {
                    m_RenderWidth,
                    m_RenderHeight,
                    static_cast<uint32_t>(std::max(1, m_MegaLightsSampleCount)),
                    // 影レイ本数の意味は参照実装と揃える(0なら影を撃たない=恒等テスト側)。
                    // 確率的サンプリングは選ばれた1灯にしか撃たないので本数ではなく有無
                    (m_MegaLightsShadowRayCount > 0) ? 1u : 0u,
                };
                stochasticConstants.Params1 =
                {
                    m_LightTileCountX,
                    kLightTileSize,
                    kMegaLightsTilePoolCapacity,
                    m_TAAFrameIndex,
                };
                stochasticConstants.Params2 =
                {
                    static_cast<uint32_t>(std::max(0, m_MegaLightsSpatialNeighborCount)),
                    static_cast<uint32_t>(std::max(1, m_MegaLightsSpatialRadius)),
                    m_MegaLightsSpatialMIS ? 1u : 0u,
                    // 初期可視レイでリザーバを殺すか(Initialが読む)。殺すと影の縁に
                    // 暗い側の系統誤差が残るため、切り替えて測れるようにしてある。
                    // 【手法3では必ず撃つ】クアッド共有は「Initialが撃った1本」だけを
                    // 可視性の情報源にしている。切ると全標本が可視フラグ付きで出てきて
                    // 影が1つも出ない(絵が明るいだけで例外もログも出ない)
                    (megaLightsQuadShared || m_MegaLightsInitialVisibility) ? 1u : 0u,
                };
                // 候補プールが錐台を組み立てたのと**同じ行列**から取る。ずれると
                // 「その灯が隣のタイルへ届くか」の判定が候補プールと食い違い、定義域がずれる
                {
                    DirectX::XMFLOAT4X4 projection;
                    DirectX::XMStoreFloat4x4(&projection, jitteredProj);

                    // 【プリ露出の補正は入れない ―― TAA/DDGIから写してはいけない】
                    // あちらが補正するのは履歴の*色*で、色はプリ露出に比例するから
                    // 「今の露出 / 前の露出」を掛ける必要がある。こちらが持ち回るのは
                    // リザーバのWで、W = Σw / (M * p̂)、w = p̂ / p_source。
                    // 分子も分母も p̂ に比例し、p_source は正規化された確率なので露出に
                    // 依存しない。**露出が約分されるのでWは露出に対して不変**。
                    //
                    // 【両方向を実測して確かめた(-megalightsperturb 2)】
                    // 「今/前」を掛けると露出+2段の直後に4倍暗くなり、「前/今」を掛けると
                    // 4倍明るいまま居座る。掛けないときだけ履歴なしの経路と一致する。
                    // **静止画では絶対に気付けない誤り**だった
                    stochasticConstants.Params3 = {
                        projection._11, projection._22,
                        // z は未使用(かつて露出補正を入れていた枠。上のコメント参照)
                        0.0f,
                        static_cast<float>(std::max(1, m_MegaLightsTemporalMClamp)),
                    };
                }
                // 履歴が使えるか。解像度が変わった直後は添字の意味が変わっており、
                // バッファのクリアが無いRHIでは前の内容が別画素のものとして残っている
                // y は空間再利用の反復番号。近傍の型板の種に混ぜて、反復ごとに別の近傍を選ばせる
                // z/w はクアッド共有(手法3)。z は Resolve が、w は Initial が読む。
                //
                // 【手法3の Params4.x の意味は手法2と違う】手法2では「時間再利用が履歴
                // リザーバを読んでよいか」だが、手法3に時間再利用は無く、Initial が
                // 遮蔽の確定した灯のキャッシュを信用してよいかの判定にだけ使う。
                // **陽性対照では切る**(履歴に依存すると手法2との画素単位の一致が崩れる)
                const bool historyUsable = megaLightsQuadShared
                                               ? (m_MegaLightsHistoryValid && m_MegaLightsBlockedCacheEnabled)
                                               : m_MegaLightsHistoryValid;
                stochasticConstants.Params4 = {
                    historyUsable ? 1u : 0u,
                    spatialIteration,
                    (megaLightsQuadShared && m_MegaLightsQuadShareEnabled) ? 1u : 0u,
                    (megaLightsQuadShared && m_MegaLightsQuadStratify) ? 1u : 0u,
                };
                // 1画素あたりの標本数。**リザーババッファの確保と必ず同じ値にすること** ――
                // ずれると Initial が確保外へ書くか、Resolve が別画素の標本を読む
                // (どちらも例外にならず、絵が「それらしく」出るので気付けない)
                stochasticConstants.Params5 = {
                    static_cast<uint32_t>(MegaLightsSamplesPerPixel()), 0u, 0u, 0u
                };
                return stochasticConstants;
            };
            const auto updateStochasticConstants = [this, buildStochasticConstants](RHI::IRHICommandList* cmd)
            {
                const MegaLightsStochasticConstants stochasticConstants = buildStochasticConstants(0u);
                cmd->UpdateBuffer(
                    m_MegaLightsStochasticConstantBuffer.get(), &stochasticConstants, sizeof(stochasticConstants));
            };

            // --- 再利用の連鎖: Initial → (Temporal) → (Spatial) → Shade ---
            // どちらの再利用も任意に切れるので、シェードが読むリザーバは「最後に書いた者」になる。
            //
            // 【Temporalの出力がそのまま次フレームの履歴になる】Spatialの結果は履歴へ戻さない。
            // 戻すと、空間で混ぜたものを時間でまた混ぜることになり、近傍どうしの相関が
            // フレームをまたいで積み上がる(ノイズが塊で蠢く)。RTXDI系には戻す実装もあるが、
            // まず戻さない形で入れて、必要になったら測ってから変える
            // 【手法3は再利用の段をどちらも通さない】リザーバを持ち回らないのが手法3の要点で、
            // 追加のレイ(可視レイ・時間検証レイ・不偏化の分母のための補正レイ)が
            // ここから生まれている。1画素1レイという予算はこれを外して初めて成り立つ
            const bool temporalRuns = !megaLightsQuadShared && m_MegaLightsTemporalEnabled &&
                                      m_MegaLightsTemporalPipelineState &&
                                      m_MegaLightsReservoirHistory[0] && m_MegaLightsHistoryGuide[0];
            const bool spatialRuns = !megaLightsQuadShared && m_MegaLightsSpatialEnabled &&
                                     m_MegaLightsSpatialPipelineState &&
                                     m_MegaLightsReservoirSpatialBuffer &&
                                     m_MegaLightsReservoirSpatialBuffer2 && m_MegaLightsSpatialNeighborCount > 0;
            // 反復回数。ping-pongのバッファと定数バッファの本数で上限が決まる。
            // 【時間再利用を切っているときは1回に落とす】不偏化の分母(Z)の可視性込みの
            // 判定は「生きているリザーバはこのフレーム・この画素で可視」という不変条件に
            // 依っており、それを保っているのは時間検証レイである。時間再利用を切ると
            // 検証が無くなり、2回目の反復が未検証のサンプルを重ねて数えるため
            // **明るい側へ大きく偏る**(900枚の蓄積平均で参照実装比 +22.4%。1回なら
            // -0.07% なので反復を重ねたときにだけ出る)。時間再利用があれば不偏
            //(同じ測定で +0.0% / 誤差の中央値は 0.0379 → 0.0305 と改善)
            uint32_t spatialIterations =
                spatialRuns ? static_cast<uint32_t>(std::clamp(
                                  m_MegaLightsSpatialIterations, 1,
                                  static_cast<int32_t>(kMegaLightsMaxSpatialIterations)))
                            : 0u;
            if (!temporalRuns && spatialIterations > 1u)
            {
                spatialIterations = 1u;
            }
            // 最後の反復が書いた側をシェードが読む
            RHI::IRHIBuffer* const spatialPingPong[kMegaLightsMaxSpatialIterations] = {
                m_MegaLightsReservoirSpatialBuffer.get(), m_MegaLightsReservoirSpatialBuffer2.get()
            };

            // ping-pong。今フレームが書く側と、前フレームが書いた側
            const uint32_t historyWriteIndex = m_MegaLightsHistoryIndex;
            const uint32_t historyReadIndex = m_MegaLightsHistoryIndex ^ 1u;

            // 【履歴は時間再利用の出力に取る ―― 空間再利用の出力を履歴へ戻してはいけない】
            // 一度、計画(1-3節)どおり「時間→空間の結果を履歴にする」形へ変えたところ、
            // 発散振動した(隣接フレーム差が63階調。実測)。近傍の履歴に自分の過去の
            // サンプルが混ざる正帰還ループができ、Wが往復のたびに複利で増幅されるため。
            // 空間再利用はフレーム内で完結させ、履歴には時間再利用の出力だけを入れる
            RHI::IRHIBuffer* const temporalOutputBuffer =
                temporalRuns ? m_MegaLightsReservoirHistory[historyWriteIndex].get() : nullptr;
            // 空間再利用の入力 = 時間再利用を挟んだならその出力、挟まないならInitialの出力
            RHI::IRHIBuffer* const reuseInputBuffer =
                temporalRuns ? temporalOutputBuffer : m_MegaLightsReservoirBuffer.get();
            RHI::IRHIBuffer* const shadeReservoirBuffer =
                spatialRuns ? spatialPingPong[(spatialIterations - 1u) % kMegaLightsMaxSpatialIterations]
                            : reuseInputBuffer;

            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "MegaLightsInitial",
                .Reads =
                {
                    m_GBufferAlbedo.get(), m_GBufferNormal.get(), m_GBufferMaterial.get(), m_GBufferDepth.get(),
                    m_BRDFLUTTexture.get(),
                },
                .BufferReads = { m_LightBuffer.get(), tilePoolBufferForBinding },
                .BufferWrites = { m_MegaLightsReservoirBuffer.get(), m_MegaLightsBlockedLightBuffer.get() },
                .Execute = [this, tilePoolBufferForBinding, updateStochasticConstants](RHI::IRHICommandList* cmd)
                {
                    updateStochasticConstants(cmd);

                    cmd->SetComputePipelineState(m_MegaLightsInitialPipelineState.get());
                    cmd->SetComputeConstantBuffer(0, m_FrameConstantBuffer.get());
                    cmd->SetComputeConstantBuffer(1, m_MegaLightsStochasticConstantBuffer.get());
                    cmd->SetComputeSamplerSet(m_ScreenSpaceSamplers.get());

                    // レジスタ割り当てはMegaLightsInitialSample.hlsl側の宣言と一致させること。
                    // 初期可視レイ(選んだサンプルが遮蔽されていたら殺す)を撃つのでTLASが要る
                    cmd->SetComputeAccelerationStructure(0, m_RaytracingScene.GetTopLevelAS());
                    cmd->SetComputeTexture(1, m_GBufferNormal.get());
                    cmd->SetComputeTexture(2, m_GBufferDepth.get());
                    cmd->SetComputeTexture(3, m_GBufferAlbedo.get());
                    cmd->SetComputeTexture(4, m_GBufferMaterial.get());
                    cmd->SetComputeTexture(5, m_BRDFLUTTexture.get());
                    cmd->SetComputeShaderResourceBuffer(6, m_LightBuffer.get());
                    cmd->SetComputeShaderResourceBuffer(7, tilePoolBufferForBinding);

                    cmd->SetComputeUnorderedAccessBuffer(0, m_MegaLightsReservoirBuffer.get());
                    cmd->SetComputeUnorderedAccessBuffer(1, m_MegaLightsBlockedLightBuffer.get());
                    cmd->Dispatch((m_RenderWidth + 7) / 8, (m_RenderHeight + 7) / 8, 1);
                },
            });

            // --- 時間再利用: 前フレームの自分が選んだ灯を再投影して借りる ---
            // 実効サンプル数がフレーム方向に積み上がるので収束が速くなる。
            // レイは1本だけ増える(採用した履歴サンプルが今も見えるかを確かめる時間検証レイ)
            if (temporalRuns)
            {
                graph.AddPass(Core::RenderGraphPassDesc{
                    .Name = "MegaLightsTemporal",
                    .Reads =
                    {
                        m_GBufferAlbedo.get(), m_GBufferNormal.get(), m_GBufferMaterial.get(), m_GBufferDepth.get(),
                        m_BRDFLUTTexture.get(), m_GBufferVelocity.get(),
                    },
                    // 【読むのは前フレームが書いた側】今フレームが書くのはもう片方なので、
                    // 同じバッファへの読み書きが同一フレーム内で起きない(WARが生じない)。
                    // RenderGraphはWARの辺を張らないので、これは構造で守るしかない
                    .BufferReads = { m_LightBuffer.get(), m_MegaLightsReservoirBuffer.get(),
                                     m_MegaLightsReservoirHistory[historyReadIndex].get(),
                                     m_MegaLightsHistoryGuide[historyReadIndex].get() },
                    .BufferWrites = { m_MegaLightsReservoirHistory[historyWriteIndex].get(),
                                      m_MegaLightsHistoryGuide[historyWriteIndex].get() },
                    .Execute = [this, historyReadIndex, historyWriteIndex](RHI::IRHICommandList* cmd)
                    {
                        // 定数はInitial側で更新済み(中身はフレーム内で不変)
                        cmd->SetComputePipelineState(m_MegaLightsTemporalPipelineState.get());
                        cmd->SetComputeConstantBuffer(0, m_FrameConstantBuffer.get());
                        cmd->SetComputeConstantBuffer(1, m_MegaLightsStochasticConstantBuffer.get());
                        cmd->SetComputeSamplerSet(m_ScreenSpaceSamplers.get());

                        // レジスタ割り当てはMegaLightsTemporal.hlsl側の宣言と一致させること。
                        // 時間検証レイを撃つのでTLASが要る。
                        // 【使わないフレームでも必ずバインドする】DX12は宣言された
                        // リソースが未バインドだと壊れる
                        cmd->SetComputeAccelerationStructure(0, m_RaytracingScene.GetTopLevelAS());
                        cmd->SetComputeTexture(1, m_GBufferNormal.get());
                        cmd->SetComputeTexture(2, m_GBufferDepth.get());
                        cmd->SetComputeTexture(3, m_GBufferAlbedo.get());
                        cmd->SetComputeTexture(4, m_GBufferMaterial.get());
                        cmd->SetComputeTexture(5, m_BRDFLUTTexture.get());
                        cmd->SetComputeShaderResourceBuffer(6, m_LightBuffer.get());
                        cmd->SetComputeShaderResourceBuffer(7, m_MegaLightsReservoirBuffer.get());
                        cmd->SetComputeShaderResourceBuffer(
                            8, m_MegaLightsReservoirHistory[historyReadIndex].get());
                        cmd->SetComputeShaderResourceBuffer(9, m_MegaLightsHistoryGuide[historyReadIndex].get());
                        // 再投影はTAAとまったく同じ引き方をする(historyUv = uv - velocity)
                        cmd->SetComputeTexture(10, m_GBufferVelocity.get());

                        cmd->SetComputeUnorderedAccessBuffer(
                            0, m_MegaLightsReservoirHistory[historyWriteIndex].get());
                        cmd->SetComputeUnorderedAccessBuffer(1, m_MegaLightsHistoryGuide[historyWriteIndex].get());
                        cmd->Dispatch((m_RenderWidth + 7) / 8, (m_RenderHeight + 7) / 8, 1);
                    },
                });
            }

            // --- 空間再利用: 近傍が選んだ灯を借りて自分の面で評価し直す ---
            // 候補プールの重みが法線を見られないぶんを、「選んだあとで隣から借りる」ことで
            // 埋め合わせる。【レイは増える】目標関数に可視性を入れるために標的ごとに1本、
            // 不偏化の分母でも可視性が不明な近傍に補正レイを撃つ(MegaLightsSpatial.hlsl 冒頭)。
            // そのぶん Shade 側の影レイは省ける
            for (uint32_t spatialIteration = 0u; spatialIteration < spatialIterations; ++spatialIteration)
            {
                // 1回目の入力は再利用の連鎖の出力、2回目以降は前の反復の出力
                RHI::IRHIBuffer* const spatialInput =
                    (spatialIteration == 0u)
                        ? reuseInputBuffer
                        : spatialPingPong[(spatialIteration - 1u) % kMegaLightsMaxSpatialIterations];
                RHI::IRHIBuffer* const spatialOutput =
                    spatialPingPong[spatialIteration % kMegaLightsMaxSpatialIterations];
                RHI::IRHIBuffer* const spatialConstants =
                    m_MegaLightsSpatialConstantBuffer[spatialIteration % kMegaLightsMaxSpatialIterations].get();
                graph.AddPass(Core::RenderGraphPassDesc{
                    .Name = "MegaLightsSpatial",
                    .Reads =
                    {
                        m_GBufferAlbedo.get(), m_GBufferNormal.get(), m_GBufferMaterial.get(), m_GBufferDepth.get(),
                        m_BRDFLUTTexture.get(),
                    },
                    // 入力は「時間再利用を挟んだならその出力、挟まないならInitialの出力」。
                    // 初期リザーバ(今フレームの殺しの持ち回り)も自画素の遮蔽の確定情報として読む
                    .BufferReads = { m_LightBuffer.get(), spatialInput, m_MegaLightsTilePoolBuffer.get(),
                                     m_MegaLightsReservoirBuffer.get(), m_MegaLightsBlockedLightBuffer.get() },
                    .BufferWrites = { spatialOutput },
                    .Execute = [this, spatialInput, spatialOutput, spatialConstants, spatialIteration,
                                buildStochasticConstants](RHI::IRHICommandList* cmd)
                    {
                        // 【この定数だけは自分で更新する】反復番号が反復ごとに違うため、
                        // Initial が更新する共有分は使えない。UpdateBuffer と
                        // SetConstantBuffer の順序は厳守(逆にすると前フレームの値を読む)
                        const MegaLightsStochasticConstants iterationConstants =
                            buildStochasticConstants(spatialIteration);
                        cmd->UpdateBuffer(spatialConstants, &iterationConstants, sizeof(iterationConstants));
                        cmd->SetComputePipelineState(m_MegaLightsSpatialPipelineState.get());
                        cmd->SetComputeConstantBuffer(0, m_FrameConstantBuffer.get());
                        cmd->SetComputeConstantBuffer(1, spatialConstants);
                        cmd->SetComputeSamplerSet(m_ScreenSpaceSamplers.get());

                        // レジスタ割り当てはMegaLightsSpatial.hlsl側の宣言と一致させること。
                        // 不偏化の分母(Z)の判定にバイアス補正レイを撃つのでTLASが要る。
                        // 【使わないフレームでも必ずバインドする】DX12は宣言された
                        // リソースが未バインドだと壊れる
                        cmd->SetComputeAccelerationStructure(0, m_RaytracingScene.GetTopLevelAS());
                        cmd->SetComputeTexture(1, m_GBufferNormal.get());
                        cmd->SetComputeTexture(2, m_GBufferDepth.get());
                        cmd->SetComputeTexture(3, m_GBufferAlbedo.get());
                        cmd->SetComputeTexture(4, m_GBufferMaterial.get());
                        cmd->SetComputeTexture(5, m_BRDFLUTTexture.get());
                        cmd->SetComputeShaderResourceBuffer(6, m_LightBuffer.get());
                        cmd->SetComputeShaderResourceBuffer(7, spatialInput);
                        // MIS重みが「その灯が隣のタイルへ届くか」を判定するのに、
                        // 候補プールのヘッダ(タイルの深度スラブ)を読む
                        cmd->SetComputeShaderResourceBuffer(8, m_MegaLightsTilePoolBuffer.get());
                        // 今フレームの初期リザーバ。殺しの持ち回り(=自画素の遮蔽の確定情報)を
                        // 選択から外すのに使う
                        cmd->SetComputeShaderResourceBuffer(9, m_MegaLightsReservoirBuffer.get());
                        // 遮蔽が確定した灯のキャッシュ
                        cmd->SetComputeShaderResourceBuffer(10, m_MegaLightsBlockedLightBuffer.get());

                        cmd->SetComputeUnorderedAccessBuffer(0, spatialOutput);
                        cmd->Dispatch((m_RenderWidth + 7) / 8, (m_RenderHeight + 7) / 8, 1);
                    },
                });
            }

            if (megaLightsQuadShared)
            {
                // --- クアッド共有の解決: 2x2の4標本を自分の面で評価し直して平均する ---
                // レイを1本も撃たないのでTLASを束縛しない。可視性は Initial が撃った
                // 1本の結果を仲間から借りる(受け入れた偏りの本体。MegaLightsResolve.hlsl 冒頭)。
                //
                // 【履歴ガイドをここで書く】手法3は時間再利用パスを持たないので、
                // デノイザが「前フレームの幾何」を引くためのガイドを書く者がいなくなる。
                // 書かないと動く細い形状でデノイザの履歴が構造的に必ず棄却される
                // (docs/ImplementationDetail.md 61.7g.6)
                RHI::IRHIBuffer* const guideWriteBuffer = m_MegaLightsHistoryGuide[historyWriteIndex].get();
                graph.AddPass(Core::RenderGraphPassDesc{
                    .Name = "MegaLightsResolve",
                    .Reads =
                    {
                        m_GBufferAlbedo.get(), m_GBufferNormal.get(), m_GBufferMaterial.get(),
                        m_GBufferDepth.get(), m_BRDFLUTTexture.get(),
                    },
                    .Writes = { m_MegaLightsTexture.get() },
                    .BufferReads = { m_LightBuffer.get(), m_MegaLightsReservoirBuffer.get() },
                    .BufferWrites = { guideWriteBuffer },
                    .Execute = [this, guideWriteBuffer](RHI::IRHICommandList* cmd)
                    {
                        // 定数はInitial側で更新済み。ここでバインドし直すのは、DX12が
                        // SetPipelineStateのたびにルート引数を無効化するため
                        cmd->SetComputePipelineState(m_MegaLightsResolvePipelineState.get());
                        cmd->SetComputeConstantBuffer(0, m_FrameConstantBuffer.get());
                        cmd->SetComputeConstantBuffer(1, m_MegaLightsStochasticConstantBuffer.get());
                        cmd->SetComputeSamplerSet(m_ScreenSpaceSamplers.get());

                        // レジスタ割り当てはMegaLightsResolve.hlsl側の宣言と一致させること。
                        // **t0(TLAS)は宣言していない** ―― レイを撃たないパスなので張らない
                        cmd->SetComputeTexture(1, m_GBufferNormal.get());
                        cmd->SetComputeTexture(2, m_GBufferDepth.get());
                        cmd->SetComputeTexture(3, m_GBufferAlbedo.get());
                        cmd->SetComputeTexture(4, m_GBufferMaterial.get());
                        cmd->SetComputeTexture(5, m_BRDFLUTTexture.get());
                        cmd->SetComputeShaderResourceBuffer(6, m_LightBuffer.get());
                        cmd->SetComputeShaderResourceBuffer(7, m_MegaLightsReservoirBuffer.get());

                        cmd->SetComputeUnorderedAccessTexture(0, m_MegaLightsTexture.get());
                        cmd->SetComputeUnorderedAccessBuffer(1, guideWriteBuffer);
                        cmd->Dispatch((m_RenderWidth + 7) / 8, (m_RenderHeight + 7) / 8, 1);
                    },
                });
            }
            else
            {
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "MegaLightsShade",
                .Reads =
                {
                    m_GBufferAlbedo.get(), m_GBufferNormal.get(), m_GBufferMaterial.get(), m_GBufferDepth.get(),
                    m_BRDFLUTTexture.get(),
                },
                .Writes = { m_MegaLightsTexture.get() },
                .BufferReads = { m_LightBuffer.get(), shadeReservoirBuffer },
                .Execute = [this, shadeReservoirBuffer](RHI::IRHICommandList* cmd)
                {
                    // 定数はInitial側で更新済み。ここでバインドし直すのは、DX12が
                    // SetPipelineStateのたびにルート引数を無効化するため
                    cmd->SetComputePipelineState(m_MegaLightsShadePipelineState.get());
                    cmd->SetComputeConstantBuffer(0, m_FrameConstantBuffer.get());
                    cmd->SetComputeConstantBuffer(1, m_MegaLightsStochasticConstantBuffer.get());
                    cmd->SetComputeSamplerSet(m_ScreenSpaceSamplers.get());

                    // レジスタ割り当てはMegaLightsShade.hlsl側の宣言と一致させること
                    cmd->SetComputeAccelerationStructure(0, m_RaytracingScene.GetTopLevelAS());
                    cmd->SetComputeTexture(1, m_GBufferNormal.get());
                    cmd->SetComputeTexture(2, m_GBufferDepth.get());
                    cmd->SetComputeTexture(3, m_GBufferAlbedo.get());
                    cmd->SetComputeTexture(4, m_GBufferMaterial.get());
                    cmd->SetComputeTexture(5, m_BRDFLUTTexture.get());
                    cmd->SetComputeShaderResourceBuffer(6, m_LightBuffer.get());
                    // 空間再利用を挟んだフレームはその出力を、挟まないフレームはInitialの出力を読む
                    cmd->SetComputeShaderResourceBuffer(7, shadeReservoirBuffer);

                    cmd->SetComputeUnorderedAccessTexture(0, m_MegaLightsTexture.get());
                    cmd->Dispatch((m_RenderWidth + 7) / 8, (m_RenderHeight + 7) / 8, 1);
                },
            });
            }
        }

        // --- デノイザ(段階5): 時間累積 + エッジ停止付き a-trous ---
        // 【蓄積パス(計測)より前に置く】計測したいのはデノイズ後の絵。
        // 【TAAより前に落とす】ノイズを残したまま渡すとTAAが履歴を毎フレーム棄却し、
        // ノイズもAAも両方失う(MegaLightsDenoise.hlsl 冒頭)
        // 手法2と手法3は同じデノイザを共有する(入力は「確率的に作られた1枚の絵」で同じもの)
        const bool megaLightsDenoiseRuns = ShouldRunMegaLights() &&
                                           (m_MegaLightsMode == MegaLightsMode::Stochastic ||
                                            m_MegaLightsMode == MegaLightsMode::QuadShared) &&
                                           m_MegaLightsDenoiseEnabled && m_MegaLightsDenoiseTemporalPSO &&
                                           m_MegaLightsDenoisedTexture != nullptr;
        if (megaLightsDenoiseRuns)
        {
            const uint32_t denoiseWrite = m_MegaLightsDenoiseHistoryIndex;
            const uint32_t denoiseRead = denoiseWrite ^ 1u;
            // 履歴の妥当性判定に「前フレームの幾何」を使えるか。ガイドを毎フレーム全画素へ
            // 書いているのは、手法2では時間再利用、手法3では Resolve。
            // どちらも走っていなければ更新されないので使えない
            const bool denoiseGuideWritten =
                (m_MegaLightsMode == MegaLightsMode::QuadShared)
                    ? (m_MegaLightsResolvePipelineState != nullptr)
                    : (m_MegaLightsTemporalEnabled && m_MegaLightsTemporalPipelineState != nullptr);
            const bool denoiseGuideValid =
                denoiseGuideWritten && m_MegaLightsHistoryGuide[0] && m_MegaLightsHistoryValid;
            // 【読むのは前フレームが書いた側】今フレームの時間再利用はもう片方へ書いている
            RHI::IRHIBuffer* const denoiseGuideBuffer =
                m_MegaLightsHistoryGuide[m_MegaLightsHistoryIndex ^ 1u]
                    ? m_MegaLightsHistoryGuide[m_MegaLightsHistoryIndex ^ 1u].get()
                    : nullptr;
            const std::vector<RHI::IRHIBuffer*> denoiseGuideReads =
                denoiseGuideBuffer ? std::vector<RHI::IRHIBuffer*>{ denoiseGuideBuffer }
                                   : std::vector<RHI::IRHIBuffer*>{};
            const int atrousPasses = std::clamp(m_MegaLightsDenoiseAtrousPasses, 0, 5);

            const auto updateDenoiseConstants =
                [this, denoiseGuideValid](RHI::IRHICommandList* cmd, uint32_t pass, float stepWidth)
            {
                MegaLightsDenoiseConstants denoiseConstants{};
                denoiseConstants.Params0 = {
                    m_RenderWidth, m_RenderHeight, m_MegaLightsDenoiseHistoryValid ? 1u : 0u, pass
                };
                // 時間累積の上限は手法ごとに別の変数を持つ。手法3にはリザーバの履歴が
                // 無く、デノイザだけが時間方向の記憶なので長くしてある(EngineDefaults.h)
                const int32_t denoiseMaxFrames = (m_MegaLightsMode == MegaLightsMode::QuadShared)
                                                     ? m_MegaLightsQuadDenoiseMaxFrames
                                                     : m_MegaLightsDenoiseMaxFrames;
                denoiseConstants.Params1 = {
                    stepWidth,
                    static_cast<float>(std::max(1, denoiseMaxFrames)),
                    // 輝度のエッジ停止の強さ(σ_l)。根拠は EngineDefaults.h の宣言に書いてある
                    m_MegaLightsDenoiseSigmaLuminance,
                    // 法線のエッジ停止の指数(同128)
                    128.0f,
                };
                // 深度のエッジ停止(View空間Zに対する相対差なので無次元)と、
                // ファイアフライの近傍クランプの強さ(近傍平均 + k・標準偏差で頭打ちにする)
                denoiseConstants.Params2 = {
                    0.02f, m_MegaLightsDenoiseFireflyClamp, denoiseGuideValid ? 1.0f : 0.0f, 0.0f
                };
                cmd->UpdateBuffer(
                    m_MegaLightsDenoiseConstantBuffer.get(), &denoiseConstants, sizeof(denoiseConstants));
            };

            // G-Bufferの束縛。**DX12はSetPipelineStateのたびにルート引数を無効化する**ので
            // パスごとに張り直す
            const auto bindDenoiseCommon = [this, denoiseGuideBuffer](RHI::IRHICommandList* cmd)
            {
                cmd->SetComputeConstantBuffer(0, m_FrameConstantBuffer.get());
                // 前フレームの幾何。【使わないパスでも必ず張る】DX12はPSO切替でルート引数が
                // 無効化されるため、宣言したリソースが未バインドだと壊れる
                if (denoiseGuideBuffer)
                {
                    cmd->SetComputeShaderResourceBuffer(0, denoiseGuideBuffer);
                }
                cmd->SetComputeConstantBuffer(1, m_MegaLightsDenoiseConstantBuffer.get());
                cmd->SetComputeSamplerSet(m_ScreenSpaceSamplers.get());
                cmd->SetComputeTexture(1, m_GBufferNormal.get());
                cmd->SetComputeTexture(2, m_GBufferDepth.get());
                cmd->SetComputeTexture(3, m_GBufferAlbedo.get());
                cmd->SetComputeTexture(4, m_GBufferMaterial.get());
                cmd->SetComputeTexture(5, m_GBufferVelocity.get());
            };

            // --- 時間累積: 生出力を復調して履歴と混ぜる ---
            // 【履歴もここで書く】SVGFは1段目のa-trous出力を履歴にするが、こちらは時間累積の
            // 出力をそのまま履歴にしている。パスが1本減るぶん履歴のノイズは多いが、
            // 指数移動平均が均すので破綻はしない。差が問題になったら分ける
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "MegaLightsDenoiseTemporal",
                .Reads =
                {
                    m_MegaLightsTexture.get(), m_GBufferAlbedo.get(), m_GBufferNormal.get(),
                    m_GBufferMaterial.get(), m_GBufferDepth.get(), m_GBufferVelocity.get(),
                    m_MegaLightsDenoiseHistory[denoiseRead].get(),
                    m_MegaLightsDenoiseMoments[denoiseRead].get(),
                },
                .Writes =
                {
                    m_MegaLightsDenoisePing[0].get(), m_MegaLightsDenoiseMomentPing[0].get(),
                    m_MegaLightsDenoiseHistory[denoiseWrite].get(),
                    m_MegaLightsDenoiseMoments[denoiseWrite].get(),
                },
                // 前フレームの幾何は「前フレームが書いた側」なので今フレームに書き手はいない。
                // 辺は張れないが、ping-pongで別バッファになっているので衝突しない
                .BufferReads = denoiseGuideReads,
                .Execute =
                    [this, denoiseRead, denoiseWrite, updateDenoiseConstants,
                     bindDenoiseCommon](RHI::IRHICommandList* cmd)
                {
                    updateDenoiseConstants(cmd, 0u, 1.0f);
                    cmd->SetComputePipelineState(m_MegaLightsDenoiseTemporalPSO.get());
                    bindDenoiseCommon(cmd);
                    cmd->SetComputeTexture(6, m_MegaLightsTexture.get());
                    // 【読むのは前フレームが書いた側】今フレームはもう片方へ書くので
                    // 同一フレーム内でのWARが生じない(RenderGraphはWARの辺を張らない)
                    cmd->SetComputeTexture(7, m_MegaLightsDenoiseHistory[denoiseRead].get());
                    cmd->SetComputeTexture(8, m_MegaLightsDenoiseMoments[denoiseRead].get());
                    cmd->SetComputeUnorderedAccessTexture(0, m_MegaLightsDenoisePing[0].get());
                    cmd->SetComputeUnorderedAccessTexture(1, m_MegaLightsDenoiseMomentPing[0].get());
                    cmd->SetComputeUnorderedAccessTexture(2, m_MegaLightsDenoiseHistory[denoiseWrite].get());
                    cmd->SetComputeUnorderedAccessTexture(3, m_MegaLightsDenoiseMoments[denoiseWrite].get());
                    cmd->Dispatch((m_RenderWidth + 7) / 8, (m_RenderHeight + 7) / 8, 1);
                },
            });

            // --- a-trous: 段ごとにステップ幅を倍にしてping-pong ---
            for (int atrousPass = 0; atrousPass < atrousPasses; ++atrousPass)
            {
                const int atrousSrc = atrousPass & 1;
                const int atrousDst = atrousSrc ^ 1;
                const float atrousStep = static_cast<float>(1 << atrousPass);
                graph.AddPass(Core::RenderGraphPassDesc{
                    .Name = "MegaLightsDenoiseAtrous",
                    .Reads =
                    {
                        m_MegaLightsDenoisePing[atrousSrc].get(),
                        m_MegaLightsDenoiseMomentPing[atrousSrc].get(),
                        m_GBufferNormal.get(), m_GBufferDepth.get(), m_GBufferAlbedo.get(),
                        m_GBufferMaterial.get(), m_GBufferVelocity.get(),
                    },
                    .Writes =
                    {
                        m_MegaLightsDenoisePing[atrousDst].get(),
                        m_MegaLightsDenoiseMomentPing[atrousDst].get(),
                    },
                    .Execute =
                        [this, atrousSrc, atrousDst, atrousPass, atrousStep, updateDenoiseConstants,
                         bindDenoiseCommon](RHI::IRHICommandList* cmd)
                    {
                        updateDenoiseConstants(cmd, static_cast<uint32_t>(atrousPass + 1), atrousStep);
                        cmd->SetComputePipelineState(m_MegaLightsDenoiseAtrousPSO.get());
                        bindDenoiseCommon(cmd);
                        cmd->SetComputeTexture(6, m_MegaLightsDenoisePing[atrousSrc].get());
                        // t7は使わないが、DX12は宣言したリソースを全部束縛しないと壊れる
                        cmd->SetComputeTexture(7, m_MegaLightsDenoisePing[atrousSrc].get());
                        cmd->SetComputeTexture(8, m_MegaLightsDenoiseMomentPing[atrousSrc].get());
                        cmd->SetComputeUnorderedAccessTexture(0, m_MegaLightsDenoisePing[atrousDst].get());
                        cmd->SetComputeUnorderedAccessTexture(
                            1, m_MegaLightsDenoiseMomentPing[atrousDst].get());
                        cmd->SetComputeUnorderedAccessTexture(2, m_MegaLightsDenoisePing[atrousDst].get());
                        cmd->SetComputeUnorderedAccessTexture(
                            3, m_MegaLightsDenoiseMomentPing[atrousDst].get());
                        cmd->Dispatch((m_RenderWidth + 7) / 8, (m_RenderHeight + 7) / 8, 1);
                    },
                });
            }

            // --- 復調を戻して最終出力にする ---
            const int denoiseFinalSrc = atrousPasses & 1;
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "MegaLightsDenoiseRemodulate",
                .Reads =
                {
                    m_MegaLightsDenoisePing[denoiseFinalSrc].get(),
                    m_MegaLightsDenoiseMomentPing[denoiseFinalSrc].get(),
                    m_GBufferAlbedo.get(), m_GBufferMaterial.get(), m_GBufferDepth.get(),
                    m_GBufferNormal.get(), m_GBufferVelocity.get(),
                },
                .Writes = { m_MegaLightsDenoisedTexture.get() },
                .Execute =
                    [this, denoiseFinalSrc, updateDenoiseConstants,
                     bindDenoiseCommon](RHI::IRHICommandList* cmd)
                {
                    updateDenoiseConstants(cmd, 0u, 1.0f);
                    cmd->SetComputePipelineState(m_MegaLightsDenoiseRemodulatePSO.get());
                    bindDenoiseCommon(cmd);
                    cmd->SetComputeTexture(6, m_MegaLightsDenoisePing[denoiseFinalSrc].get());
                    cmd->SetComputeTexture(7, m_MegaLightsDenoisePing[denoiseFinalSrc].get());
                    cmd->SetComputeTexture(8, m_MegaLightsDenoiseMomentPing[denoiseFinalSrc].get());
                    cmd->SetComputeUnorderedAccessTexture(0, m_MegaLightsDenoisedTexture.get());
                    cmd->SetComputeUnorderedAccessTexture(1, m_MegaLightsDenoisedTexture.get());
                    cmd->SetComputeUnorderedAccessTexture(2, m_MegaLightsDenoisedTexture.get());
                    cmd->SetComputeUnorderedAccessTexture(3, m_MegaLightsDenoisedTexture.get());
                    cmd->Dispatch((m_RenderWidth + 7) / 8, (m_RenderHeight + 7) / 8, 1);
                },
            });
        }

        // 計測が読む先。デノイザを通したフレームはその出力になる
        RHI::IRHITexture* const megaLightsAccumSourceTexture =
            (megaLightsDenoiseRuns && m_MegaLightsDenoisedTexture) ? m_MegaLightsDenoisedTexture.get()
                                                                   : m_MegaLightsTexture.get();

        // --- MegaLightsの蓄積パス(計測専用): 出力を線形空間でフレーム方向へ足し込む。
        //     トーンマップ後の8bitをN枚平均しても、トーンマップが凹関数なので
        //     「偏りが無くてもノイズがあるだけで平均が低く出る」。線形で足す場所がここに要る ---
        // 整定を待ってから足し始める(内部解像度の切り替えとストリーミングが片付くまで)
        ++m_MegaLightsAccumWarmupFrames;
        const bool megaLightsAccumRuns = ShouldRunMegaLights() && m_MegaLightsAccumTargetFrames > 0 &&
                                         m_MegaLightsAccumPipelineState && m_MegaLightsAccumBuffer &&
                                         m_MegaLightsAccumWarmupFrames > kMegaLightsAccumWarmup &&
                                         m_MegaLightsAccumFrames < static_cast<uint32_t>(m_MegaLightsAccumTargetFrames);
        if (megaLightsAccumRuns)
        {
            // 最初の1枚は「足す」ではなく「代入する」。RHIにバッファのクリアが無いため
            const uint32_t accumReset = (m_MegaLightsAccumFrames == 0u) ? 1u : 0u;
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "MegaLightsAccum",
                // 【計測はデノイズ後の絵を測る】デノイザを通したフレームはその出力を読む。
                // ここを生出力のままにすると、デノイザのON/OFFで測定値が1ビットも動かず
                // 「効いていない」と誤診する(実際に一度そう出た)
                .Reads = { megaLightsAccumSourceTexture },
                .BufferWrites = { m_MegaLightsAccumBuffer.get() },
                .Execute = [this, accumReset, megaLightsAccumSourceTexture](RHI::IRHICommandList* cmd)
                {
                    MegaLightsAccumConstants accumConstants{};
                    accumConstants.Params0 = { m_RenderWidth, m_RenderHeight, accumReset, 0u };
                    cmd->UpdateBuffer(m_MegaLightsAccumConstantBuffer.get(), &accumConstants, sizeof(accumConstants));

                    cmd->SetComputePipelineState(m_MegaLightsAccumPipelineState.get());
                    cmd->SetComputeConstantBuffer(0, m_MegaLightsAccumConstantBuffer.get());
                    cmd->SetComputeTexture(0, megaLightsAccumSourceTexture);
                    // UAVはDispatch直後に解除されるため毎回バインドし直す
                    cmd->SetComputeUnorderedAccessBuffer(0, m_MegaLightsAccumBuffer.get());
                    cmd->Dispatch((m_RenderWidth + 7) / 8, (m_RenderHeight + 7) / 8, 1);
                },
            });
            ++m_MegaLightsAccumFrames;
        }

        // --- 蓄積し終えた平均を生データで書き出す(計測専用) ---
        // 画面キャプチャは8bit・トーンマップ後で、丸めだけでRMSEに0.29階調の下限が生まれる。
        // 「平均が真値へ 1/√N で寄るか」はその下限に隠れて読めないので、線形のまま取り出す
        if (!m_MegaLightsDumpPath.empty() && !m_MegaLightsDumpDone && m_MegaLightsAccumBuffer &&
            m_MegaLightsAccumTargetFrames > 0 &&
            m_MegaLightsAccumFrames >= static_cast<uint32_t>(m_MegaLightsAccumTargetFrames))
        {
            const uint32_t accumBytes =
                static_cast<uint32_t>(sizeof(float) * 4) * m_RenderWidth * m_RenderHeight;

            if (!m_MegaLightsDumpIssued)
            {
                if (!m_MegaLightsAccumReadback)
                {
                    try
                    {
                        RHI::BufferDesc readbackDesc;
                        readbackDesc.Usage = RHI::BufferUsage::Readback;
                        readbackDesc.SizeInBytes = accumBytes;
                        readbackDesc.StrideInBytes = static_cast<uint32_t>(sizeof(float) * 4);
                        m_MegaLightsAccumReadback = m_Device->CreateBuffer(readbackDesc);
                    }
                    catch (const std::exception& e)
                    {
                        Core::Logger::Error(
                            "KurenaiEngine3D",
                            std::string("MegaLightsの蓄積平均の読み戻しバッファを作れませんでした: ") + e.what());
                        m_MegaLightsDumpDone = true; // 何度も試さない
                    }
                }

                if (m_MegaLightsAccumReadback)
                {
                    graph.AddPass(Core::RenderGraphPassDesc{
                        .Name = "MegaLightsDump",
                        .BufferReads = { m_MegaLightsAccumBuffer.get() },
                        .Execute = [this, accumBytes](RHI::IRHICommandList* cmd)
                        {
                            cmd->CopyBufferToReadback(
                                m_MegaLightsAccumReadback.get(), m_MegaLightsAccumBuffer.get(), accumBytes);
                        },
                    });
                    m_MegaLightsDumpIssued = true;
                    m_MegaLightsDumpCopyFrame = m_TAAFrameIndex;
                }
            }
            // GPUの実行はCPUより数フレーム遅れる。積んだ直後に読むと未完了の内容を掴む
            else if (m_TAAFrameIndex - m_MegaLightsDumpCopyFrame >= 5u)
            {
                std::vector<float> host(static_cast<size_t>(m_RenderWidth) * m_RenderHeight * 4u);
                if (m_MegaLightsAccumReadback->ReadbackData(host.data(), accumBytes))
                {
                    std::ofstream file(m_MegaLightsDumpPath, std::ios::binary | std::ios::trunc);
                    if (file)
                    {
                        // 形式: 'K','M','L','A' / 幅 / 高さ / 足したフレーム数 / 予約 / float4 × 画素数
                        const char magic[4] = { 'K', 'M', 'L', 'A' };
                        const uint32_t header[4] = { m_RenderWidth, m_RenderHeight, m_MegaLightsAccumFrames, 0u };
                        file.write(magic, sizeof(magic));
                        file.write(reinterpret_cast<const char*>(header), sizeof(header));
                        file.write(reinterpret_cast<const char*>(host.data()),
                                   static_cast<std::streamsize>(accumBytes));
                        Core::Logger::Info(
                            "KurenaiEngine3D",
                            "MegaLightsの蓄積平均を書き出しました: " + Core::WideToUtf8(m_MegaLightsDumpPath) +
                                " (" + std::to_string(m_RenderWidth) + "x" + std::to_string(m_RenderHeight) +
                                ", " + std::to_string(m_MegaLightsAccumFrames) + "フレームぶん)");
                    }
                    else
                    {
                        Core::Logger::Error(
                            "KurenaiEngine3D",
                            "MegaLightsの蓄積平均を書き出せませんでした(ファイルを開けない): " +
                                Core::WideToUtf8(m_MegaLightsDumpPath));
                    }
                }
                else
                {
                    Core::Logger::Error(
                        "KurenaiEngine3D", "MegaLightsの蓄積平均の読み戻しに失敗しました");
                }
                m_MegaLightsDumpDone = true;
            }
        }

        // --- RTシャドウパス: TLASへ太陽の見かけの円盤方向へ影レイを撃ち、可視率(0〜1)を
        //     単チャンネルのテクスチャへ書く。直後の直接光パスがt6でこれを読む ---
        if (ShouldRunRaytracedShadow())
        {
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "RTShadow",
                .Reads = { m_GBufferNormal.get(), m_GBufferDepth.get() },
                .Writes = { m_RTShadowTexture.get() },
                .Execute = [this](RHI::IRHICommandList* cmd)
                {
                    RTShadowConstants rtShadowConstants{};
                    rtShadowConstants.Params0 =
                    {
                        static_cast<float>(m_RenderWidth),
                        static_cast<float>(m_RenderHeight),
                        DirectX::XMConvertToRadians(m_RTShadowSunAngularRadiusDegrees),
                        static_cast<float>(std::max(1, m_RTShadowSampleCount)),
                    };
                    cmd->UpdateBuffer(m_RTShadowConstantBuffer.get(), &rtShadowConstants, sizeof(rtShadowConstants));

                    cmd->SetComputePipelineState(m_RTShadowPipelineState.get());
                    cmd->SetComputeConstantBuffer(0, m_FrameConstantBuffer.get());
                    cmd->SetComputeConstantBuffer(1, m_RTShadowConstantBuffer.get());

                    // レジスタ割り当てはRTShadow.hlsl側の宣言と一致させること。
                    // このシェーダはLoad(整数座標)しか使わないためサンプラーはバインドしない
                    cmd->SetComputeAccelerationStructure(0, m_RaytracingScene.GetTopLevelAS());
                    cmd->SetComputeTexture(1, m_GBufferNormal.get());
                    cmd->SetComputeTexture(2, m_GBufferDepth.get());

                    // UAVはDispatch直後に解除されるため毎回バインドし直す(IRHICommandList.h参照)
                    cmd->SetComputeUnorderedAccessTexture(0, m_RTShadowTexture.get());
                    cmd->Dispatch((m_RenderWidth + 7) / 8, (m_RenderHeight + 7) / 8, 1);
                },
            });
        }

        // 直接光パスがt6へバインドする可視率テクスチャ。DirectLighting.hlslは
        // LightCount.zがRaytracedのときしか読まないが、DX12はSetPipelineStateのたびに
        // ルート引数が無効化されるため、シェーダが宣言しているリソースは必ず何かをバインドする
        // 必要がある(nullptrはSetTextureが受け付けない)。非対応環境では読まれないダミーとして
        // 深度テクスチャを張る(Presentのデバッグ用t1/t2/t4に既定値を持たせているのと同じ理由)
        RHI::IRHITexture* const rtShadowTextureForBinding =
            m_RTShadowTexture ? m_RTShadowTexture.get() : m_GBufferDepth.get();

        // 直接光パスがt7へバインドするMegaLightsの寄与。上と同じ理由で、読まれないフレームでも
        // 何かを張る必要がある(非対応環境ではそもそもテクスチャを確保していない)
        // デノイズを通したフレームはその出力を、通さないフレームは生出力を読む。
        // **DirectLighting.hlsl 側は変わらない**(同じ t7)ので、非MegaLights経路には影響しない
        RHI::IRHITexture* megaLightsTextureForBinding =
            m_MegaLightsTexture ? m_MegaLightsTexture.get() : m_GBufferDepth.get();
        if (megaLightsDenoiseRuns && m_MegaLightsDenoisedTexture)
        {
            megaLightsTextureForBinding = m_MegaLightsDenoisedTexture.get();
        }

        // --- 直接光パス: G-Buffer+シャドウマップ(またはRTシャドウの可視率)からPBRの直接光
        //     (拡散+鏡面反射、シャドウ適用済み)を計算しHDRで書き出す(常に指定した内部解像度)。
        //     DeferredLighting/SSILの両方から読まれる ---
        graph.AddPass(Core::RenderGraphPassDesc{
            .Name = "DirectLight",
            .Reads =
            {
                m_GBufferAlbedo.get(), m_GBufferNormal.get(), m_GBufferMaterial.get(), m_GBufferDepth.get(),
                m_ShadowCascadeArray.get(),
                // RTシャドウの可視率。RTシャドウパスを実行しないフレームではm_GBufferDepthと
                // 同じポインタになるが、RenderGraphは同じ書き手への多重エッジを弾くため無害
                rtShadowTextureForBinding,
                // MegaLightsの寄与。MegaLightsパスはこれより前に登録してあるので、
                // ここに挙げることでRAWの辺が張られる(実行しないフレームでは
                // m_GBufferDepthと同じポインタになるが、多重エッジは無害)
                megaLightsTextureForBinding,
                // スペキュラのエネルギー補正(14.9節)でEss=brdf.x+brdf.yを引くためBRDF積分LUTを読む。
                // Readsに挙げることでRenderGraphがBRDFLUTBakeパス(このLUTのWriter)より後に順序付ける
                m_BRDFLUTTexture.get(),
            },
            .RenderTargets = { m_DirectLightTexture.get() },
            .Execute = [this, &gbufferViewport, &gpuLights, &lightingConstants, rtShadowTextureForBinding,
                        megaLightsTextureForBinding](RHI::IRHICommandList* cmd)
            {
                cmd->SetViewport(gbufferViewport);

                cmd->SetPipelineState(m_DirectLightPipelineState.get());
                cmd->SetConstantBuffer(0, m_FrameConstantBuffer.get());

                // UpdateBufferはSetConstantBufferより前に呼ぶ必要がある。DX12の定数バッファは
                // リングバッファで、GetGPUVirtualAddress()が現在のリングスロットのアドレスを返すため
                cmd->UpdateBuffer(m_LightingConstantBuffer.get(), &lightingConstants, sizeof(lightingConstants));
                cmd->SetConstantBuffer(1, m_LightingConstantBuffer.get());

                cmd->SetSamplerSet(m_ScreenSpaceSamplers.get());
                cmd->SetTexture(0, m_GBufferAlbedo.get());
                cmd->SetTexture(1, m_GBufferNormal.get());
                cmd->SetTexture(2, m_GBufferMaterial.get());
                cmd->SetTexture(3, m_GBufferDepth.get());
                cmd->SetTexture(4, m_ShadowCascadeArray.get());
                // RTシャドウの可視率。LightCount.zがRaytracedのときだけ読まれる
                cmd->SetTexture(6, rtShadowTextureForBinding);
                // MegaLightsが求めたポイント/スポットの直接光。LightCount.wが1のときだけ読まれる
                cmd->SetTexture(7, megaLightsTextureForBinding);

                // ライトが1つも無いフレームでもSetShaderResourceBufferは必ず呼ぶ(SetPipelineStateが
                // 毎回ルート引数を無効化するため、シェーダが宣言しているリソースを未バインドのまま
                // Drawすることになってしまう)。バッファの中身の更新はグラフ構築前に1回だけ済ませてある
                cmd->SetShaderResourceBuffer(8, m_LightBuffer.get());
                // タイルライトカリングが書いたライトグリッド。カリング無効時もシェーダが宣言している
                // リソースは必ずバインドする(上と同じ理由)
                cmd->SetShaderResourceBuffer(5, m_LightTileBuffer.get());
                // スペキュラのエネルギー補正(14.9節)用のBRDF積分LUT。t8はライトリスト
                // (StructuredBuffer)が占有しているためt9に置く
                cmd->SetTexture(9, m_BRDFLUTTexture.get());

                cmd->Draw(3, 0);
            },
        });

        // --- AO/GIパス: 選択中の手法(SSAO / SSIL / RTAO)で遮蔽率(・間接拡散光)を計算し、
        //     ブラーで均す(常に指定した内部解像度)。出力フォーマットはどれもrgb=間接拡散光, a=遮蔽率で共通 ---
        if (m_AOEnabled)
        {
            RHI::IRHITexture* const aoRawTexture = GetActiveAORawTexture();
            RHI::IRHITexture* const aoBlurredTexture = GetActiveAOTexture();
            const bool useSSIL = !ShouldRunRaytracedAO() && m_AOTechnique == AOTechnique::SSILVisibilityBitmask;

            if (ShouldRunRaytracedAO())
            {
                // RTAOパス。SSAO/SSILと違いコンピュートでUAVへ書くため、レンダーターゲットではなく
                // Writesで宣言する。レジスタ割り当てはRTAO.hlsl側の宣言と一致させること
                graph.AddPass(Core::RenderGraphPassDesc{
                    .Name = "RTAO",
                    // 直接光バッファは、バウンス面が画面に映っているときの再放射の放射輝度として読む
                    // (SSILと同じ理由でDirectLightパスより後に順序付けられる。RTAO.hlsl参照)
                    .Reads = { m_GBufferNormal.get(), m_GBufferDepth.get(), m_DirectLightTexture.get() },
                    .Writes = { aoRawTexture },
                    .Execute = [this](RHI::IRHICommandList* cmd)
                    {
                        RTAOConstants rtAOConstants{};
                        rtAOConstants.Params0 = {
                            static_cast<float>(m_RenderWidth), static_cast<float>(m_RenderHeight),
                            m_RTAOMaxDistance, m_RTAOPower
                        };
                        rtAOConstants.Params1 = {
                            static_cast<float>(std::max(1, m_RTAOSampleCount)), m_RTAOIntensity,
                            m_RTAOBounceShadowRayEnabled ? 1.0f : 0.0f, 0.0f
                        };
                        cmd->UpdateBuffer(m_RTAOConstantBuffer.get(), &rtAOConstants, sizeof(rtAOConstants));

                        cmd->SetComputePipelineState(m_RTAOPipelineState.get());
                        // ヒット面のマテリアルテクスチャをbindlessで引くためs0にWrapが要る
                        // (理由はRT反射パスの同じ呼び出しのコメント参照)。
                        // このパスは以前サンプラーセットを一度もバインドしておらず、
                        // 直前のパスが残したセットに依存していた
                        cmd->SetComputeSamplerSet(m_MaterialSamplers.get());
                        cmd->SetComputeConstantBuffer(0, m_FrameConstantBuffer.get());
                        cmd->SetComputeConstantBuffer(1, m_RTAOConstantBuffer.get());

                        cmd->SetComputeAccelerationStructure(0, m_RaytracingScene.GetTopLevelAS());
                        cmd->SetComputeTexture(1, m_GBufferNormal.get());
                        cmd->SetComputeTexture(2, m_GBufferDepth.get());
                        cmd->SetComputeShaderResourceBuffer(3, m_RaytracingScene.GetVertexAttributeBuffer());
                        cmd->SetComputeShaderResourceBuffer(4, m_RaytracingScene.GetIndexBuffer());
                        cmd->SetComputeShaderResourceBuffer(5, m_RaytracingScene.GetMeshInfoBuffer());
                        cmd->SetComputeShaderResourceBuffer(6, m_RaytracingScene.GetInstanceInfoBuffer());
                        cmd->SetComputeShaderResourceBuffer(7, m_RaytracingScene.GetMaterialBuffer());
                        // メッシュレット表(t9)。RTAO.hlsl自体は引かないが、共有ヘッダーの
                        // RaytracingScene.hlsliが宣言を持つためバインドしておく。
                        // メッシュレットを持つメッシュが1つも無いシーンではバッファ自体が無いので
                        // バインドしない(未バインドのスロットは0を返す。RTMeshInfo::MeshletCountも
                        // 0になっているため、シェーダーがここを引くことはない)
                        if (RHI::IRHIBuffer* meshletBuffer = m_RaytracingScene.GetMeshletTriangleOffsetBuffer())
                        {
                            cmd->SetComputeShaderResourceBuffer(9, meshletBuffer);
                        }
                        cmd->SetComputeTexture(8, m_DirectLightTexture.get());

                        // UAVはDispatch直後に解除されるため毎回バインドし直す(IRHICommandList.h参照)
                        cmd->SetComputeUnorderedAccessTexture(0, m_RTAORawTexture.get());
                        cmd->Dispatch((m_RenderWidth + 7) / 8, (m_RenderHeight + 7) / 8, 1);
                    },
                });
            }
            else
            {
                graph.AddPass(Core::RenderGraphPassDesc{
                    .Name = "AO",
                    .Reads = useSSIL
                        ? std::vector<RHI::IRHITexture*>{ m_GBufferNormal.get(), m_GBufferDepth.get(), m_DirectLightTexture.get() }
                        : std::vector<RHI::IRHITexture*>{ m_GBufferNormal.get(), m_GBufferDepth.get() },
                    .RenderTargets = { aoRawTexture },
                    .Execute = [this, &gbufferViewport, useSSIL](RHI::IRHICommandList* cmd)
                    {
                        cmd->SetViewport(gbufferViewport);
                        cmd->SetConstantBuffer(0, m_FrameConstantBuffer.get());
                        cmd->SetSamplerSet(m_ScreenSpaceSamplers.get());

                        if (useSSIL)
                        {
                            SSILConstants ssilConstants{};
                            ssilConstants.Params0 = { m_SSILRadius, m_SSILThickness, m_SSILIntensity, m_SSILPower };
                            ssilConstants.Params1 = { m_SSILSliceCount, m_SSILStepCount, 0u, 0u };
                            cmd->UpdateBuffer(m_SSILConstantBuffer.get(), &ssilConstants, sizeof(ssilConstants));

                            cmd->SetPipelineState(m_SSILPipelineState.get());
                            cmd->SetConstantBuffer(1, m_SSILConstantBuffer.get());
                            cmd->SetTexture(0, m_GBufferNormal.get());
                            cmd->SetTexture(1, m_GBufferDepth.get());
                            cmd->SetTexture(2, m_DirectLightTexture.get());
                            cmd->Draw(3, 0);
                        }
                        else
                        {
                            // UIやプリセットで段数が変わったらカーネルを作り直す。
                            // 先頭N本を流用してはいけない理由はm_SSAOKernelSizeのコメント参照。
                            // 生成は16回のRNGだけなので毎フレーム比較しても問題にならない
                            const uint32_t kernelSize =
                                std::clamp(m_SSAOKernelSize, 1u, kSSAOKernelSizeMax);
                            if (m_SSAOKernel.size() != kernelSize)
                            {
                                m_SSAOKernel = GenerateSSAOKernel(kernelSize);
                            }

                            // 使わない残りの要素は0のまま(シェーダはsampleCountまでしか読まない)
                            SSAOConstants ssaoConstants{};
                            std::copy(m_SSAOKernel.begin(), m_SSAOKernel.end(), ssaoConstants.Samples);
                            ssaoConstants.Params = {
                                m_SSAORadius, m_SSAORadius * 0.05f, m_SSAOPower, static_cast<float>(kernelSize) };
                            cmd->UpdateBuffer(m_SSAOConstantBuffer.get(), &ssaoConstants, sizeof(ssaoConstants));

                            cmd->SetPipelineState(m_SSAOPipelineState.get());
                            cmd->SetConstantBuffer(1, m_SSAOConstantBuffer.get());
                            cmd->SetTexture(0, m_GBufferNormal.get());
                            cmd->SetTexture(1, m_GBufferDepth.get());
                            cmd->Draw(3, 0);
                        }
                    },
                });
            }

            // ブラーパス: 遮蔽率・間接拡散光のタイル状ノイズをボックスブラーで均す(SSAO/SSIL共通シェーダ)
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "AOBlur",
                .Reads = { aoRawTexture },
                .RenderTargets = { aoBlurredTexture },
                .Execute = [this, &gbufferViewport, aoRawTexture](RHI::IRHICommandList* cmd)
                {
                    cmd->SetViewport(gbufferViewport);
                    cmd->SetPipelineState(m_AOBlurPipelineState.get());
                    // ブラーはカーネルのタップが画面端で[0,1]を出るため、Wrapのサンプラーが
                    // 1つも入っていないこのセットを明示的にバインドする(直前のパスのバインドが
                    // そのまま残るのに依存してはいけない)
                    cmd->SetSamplerSet(m_ScreenSpaceSamplers.get());
                    cmd->SetTexture(0, aoRawTexture);
                    cmd->Draw(3, 0);
                },
            });
        }

        // デバッグ表示(ブラー前確認用)のため、ブラー前の生バッファへの参照も別途保持しておく。
        // 上のパスが書いた先と必ず一致させるため、どちらも同じアクセサから取る
        RHI::IRHITexture* const activeAOTexture = GetActiveAOTexture();
        RHI::IRHITexture* const activeAORawTexture = GetActiveAORawTexture();

        // --- 雲パス: 積雲と巻雲だけを1/2解像度で評価し、透過率と事前乗算済みの散乱光を書く ---
        //
        // 【なぜ分離したか】雲の評価は背景1画素あたり値ノイズを数十回踏むため極端に重く、
        // Intel UHD Graphics 620 / 1280x720 / DX11 / Release の実測ではLightingパス19.4msのうち
        // 積雲14.5ms + 巻雲1.3msを占めていた。雲は空間周波数が低いので低解像度で評価しても
        // 見た目の劣化が小さく、面積1/4で評価すればそのぶん素直に安くなる。
        // 太陽・星のような高周波成分はLighting側(SkyColorWithoutClouds)に残るためにじまない。
        // 合成が事前乗算のover合成であることを使った厳密な分離である(SkyCloud.hlsl冒頭参照)。
        //
        // 【手続き空が無効なら登録しない】.ksceneでDDSスカイボックスを使う場合、Lightingパスは
        // キューブマップをサンプルする経路(SkyParams.y <= 0.5)へ入り、この結果を一切読まない
        const bool skyCloudPassRuns = m_SkyCloudTexture && (m_SkyAnalyticBackground && usingProceduralSky);
        if (skyCloudPassRuns)
        {
            RHI::Viewport skyCloudViewport;
            skyCloudViewport.Width = static_cast<float>(m_SkyCloudWidth);
            skyCloudViewport.Height = static_cast<float>(m_SkyCloudHeight);

            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "SkyCloud",
                // SkyViewBakeより後に順序付けさせる(SkyCloudLayers自体はLUTを引かないが、
                // Sky.hlsliの宣言上バインドが必要で、パスの前後関係も揃えておく)
                .Reads = { m_SkyViewLUT.get(), m_CloudShapeNoiseTexture.get(), m_CloudDetailNoiseTexture.get() },
                .RenderTargets = { m_SkyCloudTexture.get() },
                // 空パラメータ。SkyIntegrateパスより後に順序付けさせる
                .BufferReads = { m_SkyParametersBuffer.get() },
                .Execute = [this, skyCloudViewport](RHI::IRHICommandList* cmd)
                {
                    cmd->SetViewport(skyCloudViewport);
                    cmd->SetPipelineState(m_SkyCloudPipelineState.get());
                    cmd->SetConstantBuffer(0, m_FrameConstantBuffer.get());
                    cmd->SetSamplerSet(m_ScreenSpaceSamplers.get());
                    cmd->SetTexture(0, m_SkyViewLUT.get());
                    cmd->SetTexture(1, m_CloudShapeNoiseTexture.get());
                    cmd->SetTexture(2, m_CloudDetailNoiseTexture.get());
                    cmd->SetShaderResourceBuffer(3, m_SkyParametersBuffer.get());
                    cmd->Draw(3, 0);
                },
            });
        }

        // --- DDGIの低解像度解決パス(有効なときだけ) ---
        // 拡散イラディアンスとinsideWeightを1/2解像度で求め、Lightingパスが深度を見て
        // アップサンプルする。雲と違い厳密ではない近似のため既定は無効(DDGIResolve.hlsl冒頭参照)
        const bool ddgiResolvePassRuns =
            m_DDGIHalfResolution && m_DDGIResolveTexture && m_DDGIEnabled && m_HasGIVolume && m_DDGIBaked;
        if (ddgiResolvePassRuns)
        {
            RHI::Viewport ddgiResolveViewport;
            ddgiResolveViewport.Width = static_cast<float>(m_DDGIResolveWidth);
            ddgiResolveViewport.Height = static_cast<float>(m_DDGIResolveHeight);

            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "DDGIResolve",
                // アトラスはDDGIUpdateパスが書くので、それより後に順序付けさせる。
                // 深度と法線はG-Bufferパスより後
                .Reads = {
                    m_DDGIIrradianceAtlas.get(), m_DDGIDistanceAtlas.get(),
                    m_GBufferDepth.get(), m_GBufferNormal.get(),
                },
                // 2枚目は合成側のGatherRed用の低解像度深度(41.24節)。
                // 並びはDDGIResolve.hlslのPSOutputおよびPSOのRenderTargetFormatsと一致させること
                .RenderTargets = { m_DDGIResolveTexture.get(), m_DDGIResolveDepthTexture.get() },
                .Execute = [this, ddgiResolveViewport](RHI::IRHICommandList* cmd)
                {
                    cmd->SetViewport(ddgiResolveViewport);
                    cmd->SetPipelineState(m_DDGIResolvePipelineState.get());
                    cmd->SetConstantBuffer(0, m_FrameConstantBuffer.get());
                    cmd->SetSamplerSet(m_ScreenSpaceSamplers.get());
                    cmd->SetTexture(0, m_DDGIIrradianceAtlas.get());
                    cmd->SetTexture(1, m_DDGIDistanceAtlas.get());
                    cmd->SetTexture(2, m_GBufferDepth.get());
                    cmd->SetTexture(3, m_GBufferNormal.get());
                    cmd->Draw(3, 0);
                },
            });
        }

        // --- ライティングパス: G-Bufferを読み、SceneColorへ出力(常に指定した内部解像度) ---
        graph.AddPass(Core::RenderGraphPassDesc{
            .Name = "Lighting",
            .Reads = {
                m_GBufferAlbedo.get(), m_DirectLightTexture.get(), m_GBufferMaterial.get(), m_GBufferDepth.get(),
                skyTexture, activeAOTexture, m_GBufferEmissive.get(), m_GBufferNormal.get(),
                m_IrradianceTexture.get(), m_PrefilteredEnvTexture.get(), m_BRDFLUTTexture.get(),
                m_GBufferBentNormal.get(),
                // ProbeBakeパスより後に順序付けさせるために挙げる(実際のバインドはExecute内)。
                // 反射プローブは鏡面専任なので拡散イラディアンス側の配列は無い
                m_ProbePrefilteredArray.get(), m_ProbeDistanceArray.get(),
                // 同じくDDGIUpdateパスより後に順序付けさせるために挙げる(22章)
                m_DDGIIrradianceAtlas.get(), m_DDGIDistanceAtlas.get(),
                // 大気散乱のSkyView LUT。背景の空をここから引くため、
                // SkyViewBakeパスより後に順序付けさせる
                m_SkyViewLUT.get(),
                // 低解像度で評価済みの雲。SkyCloudパスより後に順序付けさせるために挙げる
                // (パスが登録されないフレームでは書き手が居ないので依存も張られない)
                m_SkyCloudTexture.get(),
                // 同じく低解像度で評価済みのDDGI。DDGIResolveパスより後に順序付けさせる
                m_DDGIResolveTexture.get(), m_DDGIResolveDepthTexture.get(),
            },
            .RenderTargets = { m_SceneColor.get() },
            // 空パラメータ。SkyIntegrateパスより後に順序付けさせるために挙げる
            // (実際のバインドはExecute内)
            .BufferReads = { m_SkyParametersBuffer.get() },
            .Execute = [this, &gbufferViewport, activeAOTexture, skyTexture](RHI::IRHICommandList* cmd)
            {
                cmd->SetViewport(gbufferViewport);
                // 深度テストに失敗した(=何も描かれていない)ピクセル用の背景色。discardされた箇所に前フレームのデータが
                // 残らないよう、フルスクリーン三角形を描く前に明示的にクリアしておく
                cmd->ClearRenderTarget({ 0.05f, 0.05f, 0.08f, 1.0f });

                cmd->SetPipelineState(m_LightingPipelineState.get());
                cmd->SetConstantBuffer(0, m_FrameConstantBuffer.get());
                cmd->SetSamplerSet(m_ScreenSpaceSamplers.get());
                cmd->SetTexture(0, m_GBufferAlbedo.get());
                cmd->SetTexture(1, m_DirectLightTexture.get());
                cmd->SetTexture(2, m_GBufferMaterial.get());
                cmd->SetTexture(3, m_GBufferDepth.get());
                cmd->SetTexture(4, skyTexture);
                cmd->SetTexture(5, activeAOTexture);
                cmd->SetTexture(6, m_GBufferEmissive.get());
                cmd->SetTexture(7, m_GBufferNormal.get());
                cmd->SetTexture(8, m_IrradianceTexture.get());
                cmd->SetTexture(9, m_PrefilteredEnvTexture.get());
                cmd->SetTexture(10, m_BRDFLUTTexture.get());
                // 反射プローブ(19章、鏡面専任なので拡散イラディアンス側のスロットは無い)。
                // FrameConstants.ProbeParams.xが0のとき(未ベイク・無効時)はシェーダー側が
                // 選択ループを回さないため中身は参照されないが、DX12はディスクリプタテーブルに
                // 未初期化のスロットが残ると動作が未定義になるため常にバインドする
                cmd->SetTexture(12, m_ProbePrefilteredArray.get());
                cmd->SetShaderResourceBuffer(13, m_ProbeBuffer.get());
                cmd->SetTexture(14, m_ProbeDistanceArray.get());
                // DDGI(22章)。反射プローブと同じ理由で、無効時も含めて常にバインドする
                cmd->SetTexture(15, m_DDGIIrradianceAtlas.get());
                cmd->SetTexture(16, m_DDGIDistanceAtlas.get());
                // 空パラメータ。t11に置く(t17はbent normalが使う)
                cmd->SetShaderResourceBuffer(11, m_SkyParametersBuffer.get());
                // bent normal(34章)
                cmd->SetTexture(17, m_GBufferBentNormal.get());
                // 低解像度で評価済みの雲(rgb=事前乗算済みの散乱光、a=透過率)。
                // このシェーダーは雲を自前で評価しなくなったため、3Dノイズが使っていたt18を
                // そのまま流用している(DeferredLighting.hlsl冒頭のコメント参照)
                cmd->SetTexture(18, m_SkyCloudTexture.get());
                // 低解像度で評価済みのDDGI(rgb=イラディアンス、a=insideWeight)。
                // 【無効時も常にバインドする】シェーダーはDDGIParams4.yで読むかどうかを分けるが、
                // DX12のディスクリプタテーブルは21スロットぶんをまとめてコピーするため、
                // 未初期化のスロットを残せない(反射プローブ・DDGIアトラスと同じ理由)。
                // 以前はここへ雲の3Dノイズを差していたが、Texture2Dの宣言と型が食い違うため
                // このテクスチャへ置き換えた
                cmd->SetTexture(19, m_DDGIResolveTexture.get());
                // 低解像度の深度(41.24節)。UpsampleDDGIがGatherRed 1回で4テクセルぶんを取る
                cmd->SetTexture(21, m_DDGIResolveDepthTexture.get());
                // 大気散乱のSkyView LUT。日中の空の色はここから引く
                cmd->SetTexture(20, m_SkyViewLUT.get());
                cmd->Draw(3, 0);
            },
        });

        // --- 半透明フォワードパス: glTFのalphaMode=BLENDのメッシュ(mesh.IsTransparent)だけを、
        //     LightingパスのSceneColorの上にカメラから遠い順(奥から手前)でアルファブレンド合成する。
        //     深度テストはGBuffer深度に対して行うが書き込みは行わない(半透明パイプラインステートの
        //     DepthWriteEnabled=false)ため、不透明物体には隠れる一方、半透明同士は常に描画順で
        //     正しく重なる。RenderTargets/DepthTargetにSceneColor/GBuffer深度を指定しているだけで
        //     ClearRenderTarget/ClearDepthは呼ばないため、Lightingパスが書いた内容の上に描き足す形になる ---
        graph.AddPass(Core::RenderGraphPassDesc{
            .Name = "Transparent",
            // ProbeBakeパスより後に順序付けさせるために挙げる(実際のバインドはExecute内)。
            // 半透明パスもLightingパスと同じ環境ソース(反射プローブ+グローバルIBL)を使うため、
            // 焼き上がる前のプローブを読まないようにする必要がある。
            // DDGIアトラスもReadsへ挙げ、DDGIProbeUpdateパスより後ろへ順序付ける
            .Reads = {
                m_ProbePrefilteredArray.get(), m_ProbeDistanceArray.get(),
                m_DDGIIrradianceAtlas.get(), m_DDGIDistanceAtlas.get(),
            },
            .RenderTargets = { m_SceneColor.get() },
            .DepthTarget = m_GBufferDepth.get(),
            .Execute = [this, &gbufferViewport, &gpuLights, &cameraPosition, &viewProj](RHI::IRHICommandList* cmd)
            {
                // 半透明メッシュをインスタンス単位でカメラからの距離降順(奥から手前)に並べる。
                // instance.WorldはHLSL(mul(vec, World))に合わせて転置済みのため、ワールド座標の
                // 平行移動成分は行ではなく列(_14/_24/_34)に入っている
                struct TransparentDraw
                {
                    const Assets::ModelInstance* Instance;
                    // 【段も覚える】meshが属する段のメッシュレット表を指す必要がある
                    const Assets::Model* Model;
                    const Assets::Mesh* Mesh;
                    float DistanceSq;
                };
                std::vector<TransparentDraw> draws;
                // 半透明もカメラの錐台で間引く。ここは描画リストの構築なので、
                // 間引いた分はソートの対象からも外れる
                const FrustumPlanes transparentFrustum = ExtractFrustumPlanes(viewProj);
                for (size_t instanceIndex = 0; instanceIndex < m_Scene.Instances.size(); ++instanceIndex)
                {
                    const Assets::ModelInstance& instance = m_Scene.Instances[instanceIndex];
                    ++m_FrustumCullTested;
                    if (!IsAABBVisible(transparentFrustum, instance.WorldBoundsMin, instance.WorldBoundsMax))
                    {
                        ++m_FrustumCullCulled;
                        continue;
                    }

                    const float dx = instance.World._14 - cameraPosition.x;
                    const float dy = instance.World._24 - cameraPosition.y;
                    const float dz = instance.World._34 - cameraPosition.z;
                    const float distanceSq = dx * dx + dy * dy + dz * dz;
                    // クロスディザ非対応の経路なので、フェード中でも段は1つに決め打つ
                    // ストリーミング中で未読み込みなら描かない
                    const Assets::Model* const currentModel = GetCurrentLOD(instanceIndex);
                    if (!currentModel) { continue; }
                    for (const auto& mesh : currentModel->Meshes)
                    {
                        if (!mesh.IsTransparent)
                        {
                            continue;
                        }
                        // メッシュ単位のカリング。ここだけはループ内で描かず描画リストを作るので、
                        // 判定はpush_backの直前に入れる(描かないものをリストへ積まない)
                        if (!IsMeshVisibleWithStats(
                                m_MeshCullingEnabled, transparentFrustum, instance, *currentModel, mesh, m_MeshCullTested,
                                m_MeshCullCulled))
                        {
                            continue;
                        }
                        draws.push_back({ &instance, currentModel, &mesh, distanceSq });
                    }
                }
                if (draws.empty())
                {
                    return;
                }
                std::sort(
                    draws.begin(), draws.end(),
                    [](const TransparentDraw& a, const TransparentDraw& b) { return a.DistanceSq > b.DistanceSq; });

                cmd->SetViewport(gbufferViewport);
                cmd->SetPipelineState(m_TransparentPipelineState.get());
                cmd->SetConstantBuffer(0, m_FrameConstantBuffer.get());
                cmd->SetSamplerSet(m_MaterialSamplers.get());

                // ライトバッファの中身の更新はグラフ構築前に1回だけ済ませてある
                // (タイルライトカリングパスがこのパスより先に読むため、パス内で更新できない)

                // メッシュによらずパス全体で共通のテクスチャはここで一度だけバインドする。
                // テクスチャのバインドは上書きするまで維持されるため(IRHICommandList::SetTexture参照)、
                // メッシュごとのループ内で張り直す必要はない
                cmd->SetTexture(4, m_ShadowCascadeArray.get());
                cmd->SetShaderResourceBuffer(8, m_LightBuffer.get());
                // IBL(14章)。このパスにはSSRが適用されないため、半透明サーフェスの環境の
                // 映り込みはこの環境ソースだけが担う
                cmd->SetTexture(9, m_IrradianceTexture.get());
                cmd->SetTexture(10, m_PrefilteredEnvTexture.get());
                cmd->SetTexture(11, m_BRDFLUTTexture.get());
                // 反射プローブ(19章、鏡面専任)。Lightingパスと同じReflectionProbe.hlsliを
                // 共有しており、半透明サーフェスも室内なら室内の環境が映るようになる。
                // t0〜t4とt8〜t11が埋まっているため、このパスではt5・t7を割り当てている
                // (Transparent.hlsl冒頭。t6は使わない)。
                // マテリアルの遮蔽マップ(OcclusionTexture)はt5〜t7と衝突するためt13を使う
                // ProbeParams.xが0でも常にバインドするのはLightingパスと同じ理由
                cmd->SetTexture(5, m_ProbePrefilteredArray.get());
                cmd->SetShaderResourceBuffer(7, m_ProbeBuffer.get());
                cmd->SetTexture(12, m_ProbeDistanceArray.get());
                // DDGI(22章)。Lighting/ProbeCaptureパスと同じアトラスを共有する。
                // ProbeParams同様、DDGIParams0.wが0でも常にバインドする。
                // t14はメッシュごとのbent normal(34章)が使うためt15/t16へ置く
                // ——ここを14/15のままにするとメッシュのループが毎回上書きしてしまう
                cmd->SetTexture(15, m_DDGIIrradianceAtlas.get());
                cmd->SetTexture(16, m_DDGIDistanceAtlas.get());

                // 半透明は奥から手前への描画順そのものが正しさの前提なので並べ替えられない。
                // そのため必要になった時点でパイプラインを切り替える(GBufferパスと同じ方式)
                RHI::IRHIPipelineState* currentPipelineState = m_TransparentPipelineState.get();
                const auto bindPipelineState = [&](bool mirrored)
                {
                    RHI::IRHIPipelineState* const wanted =
                        mirrored ? m_TransparentPipelineStateMirrored.get() : m_TransparentPipelineState.get();
                    if (wanted == currentPipelineState)
                    {
                        return;
                    }
                    cmd->SetPipelineState(wanted);
                    cmd->SetConstantBuffer(0, m_FrameConstantBuffer.get());
                    cmd->SetSamplerSet(m_MaterialSamplers.get());
                    currentPipelineState = wanted;
                };

                for (const TransparentDraw& draw : draws)
                {
                    bindPipelineState(draw.Instance->IsMirrored);

                    const ObjectConstants objectConstants =
                        MakeObjectConstants(
                            *draw.Instance, *draw.Model, *draw.Mesh, m_EmissiveIntensity,
                            m_OcclusionMapEnabled, m_MeshletLODFrame);
                    cmd->UpdateBuffer(m_ObjectConstantBuffer.get(), &objectConstants, sizeof(objectConstants));
                    cmd->SetConstantBuffer(1, m_ObjectConstantBuffer.get());

                    cmd->SetVertexBuffer(draw.Mesh->VertexBuffer.get());
                    cmd->SetIndexBuffer(draw.Mesh->IndexBuffer.get());
                    // メッシュごとに変わるマテリアルテクスチャのみ差し替える
                    // (t4のシャドウとt8以降のライト/IBLはループ前に一度バインドしたものがそのまま残る。
                    // t13だけはマテリアルの遮蔽マップなのでメッシュごとに差し替える)
                    cmd->SetTexture(0, draw.Mesh->BaseColorTexture);
                    cmd->SetTexture(1, draw.Mesh->NormalTexture);
                    cmd->SetTexture(2, draw.Mesh->MetallicRoughnessTexture);
                    cmd->SetTexture(3, draw.Mesh->EmissiveTexture);
                    cmd->SetTexture(13, draw.Mesh->OcclusionTexture);
                    // bent normal(34章)。このパスはt0〜t13を使い切っているためt14
                    cmd->SetTexture(14, draw.Mesh->BentNormalTexture);

                    cmd->DrawIndexed(draw.Mesh->IndexCount, 0, 0);
                }
            },
        });

        // --- 平面反射パス: 水面に不透明ジオメトリの鏡像を映すフォワードパス ---
        // 水面が無いシーン・無効化時はパスを登録しない(SSR側のフラグも0になる。下のSSRパス参照)
        if (planarReflectionPassRuns)
        {
            RHI::Viewport planarReflectionViewport;
            planarReflectionViewport.Width = static_cast<float>(m_PlanarReflectionWidth);
            planarReflectionViewport.Height = static_cast<float>(m_PlanarReflectionHeight);

            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "PlanarReflection",
                // ProbeCapture/captureProbeFaceと同じ理由でシャドウ・IBL・DDGIを挙げ、
                // これらを書くパスより後ろへ順序付ける(実際のバインドはExecute内)
                .Reads = {
                    m_ShadowCascadeArray.get(), m_IrradianceTexture.get(), m_PrefilteredEnvTexture.get(),
                    m_BRDFLUTTexture.get(), m_DDGIIrradianceAtlas.get(), m_DDGIDistanceAtlas.get(),
                    m_SkyViewLUT.get(),
                },
                .RenderTargets = { m_PlanarReflectionColor.get() },
                .DepthTarget = m_PlanarReflectionDepth.get(),
                // 大気遠近。空パラメータ(m_SkyParametersBuffer)をSkyIntegrateパスの後へ
                // 順序付けさせるために挙げる(実際のバインドはExecute内。SSRパスの同じ宣言と同じ理由)
                // m_DroneBufferはこのパス末尾でドローンショーの機体を描き足すために読む
                // (実際のバインドはExecute内)
                .BufferReads = { m_LightBuffer.get(), m_SkyParametersBuffer.get(), m_DroneBuffer.get() },
                .Execute = [this, &constants, &planarReflectionViewport, reflectedViewProj, reflectMatrix,
                            waterPlaneY, viewMatrix, jitteredProj, effectiveExposure](RHI::IRHICommandList* cmd)
                {
                    // captureProbeFaceとまったく同じ作法(constants.ViewProj/CameraPosition/
                    // PrevViewProj/TAAParams/PlanarReflectionPlaneだけをこのパス用に差し替える)。
                    // Viewはカメラのビュー行列のままにする(PlanarReflection.hlsl冒頭の
                    // 【Viewをカメラのままにする理由】参照。ProbeCaptureとは異なる理由による)
                    FrameConstants reflectionConstants = constants;
                    DirectX::XMStoreFloat4x4(&reflectionConstants.ViewProj, DirectX::XMMatrixTranspose(reflectedViewProj));
                    const DirectX::XMVECTOR reflectedCameraPos =
                        DirectX::XMVector3Transform(DirectX::XMLoadFloat4(&constants.CameraPosition), reflectMatrix);
                    DirectX::XMFLOAT4 reflectedCameraPosFloat;
                    DirectX::XMStoreFloat4(&reflectedCameraPosFloat, reflectedCameraPos);
                    reflectionConstants.CameraPosition = { reflectedCameraPosFloat.x, reflectedCameraPosFloat.y, reflectedCameraPosFloat.z, 0.0f };
                    // TAA関連はカメラ視点のものが入ったままなので明示的に潰す(captureProbeFaceと同じ理由)
                    reflectionConstants.PrevViewProj = reflectionConstants.ViewProj;
                    reflectionConstants.TAAParams = { 0.0f, 0.0f, 0.0f, 0.0f };
                    // Hi-Zオクルージョンカリングも潰す(captureProbeFaceと同じ理由)。
                    // 鏡映カメラから見える範囲とメインカメラのHi-Zは無関係
                    reflectionConstants.OcclusionCullParams = { 0.0f, 0.0f, 0.0f, 0.0f };
                    // 統計も止める(captureProbeFaceと同じ理由)
                    reflectionConstants.MeshletCullStatsParams = { 0.0f, 0.0f, 0.0f, 0.0f };
                    reflectionConstants.PlanarReflectionPlane = { 0.0f, 1.0f, 0.0f, -waterPlaneY };
                    cmd->UpdateBuffer(m_PlanarReflectionConstantBuffer.get(), &reflectionConstants, sizeof(reflectionConstants));

                    // RenderTargets/DepthTargetはパス宣言(.RenderTargets/.DepthTarget)により
                    // RenderGraphが自動的にバインド済みのため、ここではビューポート設定と
                    // クリアだけでよい(GBuffer/Lightingパスと同じ流儀。captureProbeFaceは
                    // .Writesのみの宣言のため例外的に手動バインドしている)
                    cmd->SetViewport(planarReflectionViewport);
                    cmd->ClearRenderTarget({ 0.0f, 0.0f, 0.0f, 0.0f });
                    // Reverse-Zのため遠平面側(NDC z=0.0)にクリアする
                    cmd->ClearDepth(0.0f);

                    cmd->SetPipelineState(m_PlanarReflectionPipelineState.get());
                    cmd->SetConstantBuffer(0, m_PlanarReflectionConstantBuffer.get());
                    cmd->SetSamplerSet(m_MaterialSamplers.get());

                    // captureProbeFaceと同じ順・同じレジスタでバインドする(PlanarReflection.hlsl参照)
                    cmd->SetTexture(4, m_ShadowCascadeArray.get());
                    cmd->SetShaderResourceBuffer(8, m_LightBuffer.get());
                    cmd->SetTexture(9, m_IrradianceTexture.get());
                    cmd->SetTexture(10, m_PrefilteredEnvTexture.get());
                    cmd->SetTexture(11, m_BRDFLUTTexture.get());
                    cmd->SetTexture(12, m_DDGIIrradianceAtlas.get());
                    cmd->SetTexture(13, m_DDGIDistanceAtlas.get());
                    // 大気遠近のin-scatter項が読む空パラメータ(PlanarReflection.hlsl参照)
                    cmd->SetShaderResourceBuffer(14, m_SkyParametersBuffer.get());
                    // 大気散乱のSkyView LUT。in-scatter項の空の色はここから引く
                    cmd->SetTexture(15, m_SkyViewLUT.get());

                    // 鏡映カメラで描くとワインディングが全反転するため、PSOの切り替えは
                    // instance.IsMirroredの否定で行う(このファイル冒頭のPSO生成箇所のコメント参照)
                    RHI::IRHIPipelineState* currentPipelineState = m_PlanarReflectionPipelineState.get();
                    const auto bindPipelineState = [&](bool mirrored)
                    {
                        RHI::IRHIPipelineState* const wanted =
                            mirrored ? m_PlanarReflectionPipelineStateMirrored.get() : m_PlanarReflectionPipelineState.get();
                        if (wanted == currentPipelineState)
                        {
                            return;
                        }
                        cmd->SetPipelineState(wanted);
                        cmd->SetConstantBuffer(0, m_PlanarReflectionConstantBuffer.get());
                        cmd->SetSamplerSet(m_MaterialSamplers.get());
                        currentPipelineState = wanted;
                    };

                    // 鏡映カメラの錐台で間引く。カメラ本体の錐台とは別物なので、
                    // 画面には映っていないが水面には映るものが正しく残る
                    const FrustumPlanes reflectionFrustum = ExtractFrustumPlanes(reflectedViewProj);

                    // インスタンシングのバッチと、まとめられなかった1体を同じ形で回す。
                    // 深度プリパス/G-Bufferと同じ「そのフレームに選ばれた段」の組を使う
                    GetInstanceDrawUnits(/*coarsestLOD=*/false, m_DrawUnitScratch);
                    for (const InstanceDrawUnit& unit : m_DrawUnitScratch)
                    {
                        const size_t instanceIndex = unit.InstanceIndex;
                        const Assets::ModelInstance& instance = *unit.Instance;
                        ++m_FrustumCullTested;
                        if (!IsAABBVisible(reflectionFrustum, unit.WorldBoundsMin, unit.WorldBoundsMax))
                        {
                            ++m_FrustumCullCulled;
                            continue;
                        }

                        // クロスディザ非対応の経路なので、フェード中でも段は1つに決め打つ
                        // ストリーミング中で未読み込みなら描かない。
                        // バッチはフェード中でないものだけで構成されるので、
                        // unit.Model と GetCurrentLOD は同じ段を指す
                        const Assets::Model* const currentModel =
                            unit.Model ? unit.Model : GetCurrentLOD(instanceIndex);
                        if (!currentModel) { continue; }
                        for (const auto& mesh : currentModel->Meshes)
                        {
                            // 半透明メッシュは反射に含めない(ProbeCaptureと同じ割り切り。
                            // PlanarReflection.hlsl冒頭参照)
                            if (mesh.IsTransparent)
                            {
                                continue;
                            }

                            // メッシュ単位のカリング。錐台は鏡映カメラのもの。
                            // 【バッチでは行わない】理由はG-Bufferパスの同じ箇所を参照
                            if (!unit.IsBatch()
                                && !IsMeshVisibleWithStats(
                                    m_MeshCullingEnabled, reflectionFrustum, instance, *currentModel, mesh, m_MeshCullTested,
                                    m_MeshCullCulled))
                            {
                                continue;
                            }

                            // 鏡映で巻きが反転するため、ミラーリングの有無に対して逆のPSOを選ぶ。
                            // バッチ内では IsMirrored が同一(グループ化のキー)なので代表で決めてよい
                            bindPipelineState(!instance.IsMirrored);

                            ObjectConstants objectConstants =
                                MakeObjectConstants(instance, *currentModel, mesh, m_EmissiveIntensity, m_OcclusionMapEnabled, m_MeshletLODFrame);
                            objectConstants.InstanceBase = unit.InstanceBase;
                            objectConstants.InstancingEnabled = unit.IsBatch() ? 1u : 0u;
                            cmd->UpdateBuffer(m_ObjectConstantBuffer.get(), &objectConstants, sizeof(objectConstants));
                            cmd->SetConstantBuffer(1, m_ObjectConstantBuffer.get());

                            // 【毎回張り直す】このパスはモデルのあとにドローンショーを描き、
                            // そちらが同じ頂点シェーダー用SRV(t0)へ自分のバッファを張る。
                            // 張り直さないと全インスタンスがドローンの座標を行列として読む
                            if (unit.IsBatch())
                            {
                                cmd->SetVertexShaderResourceBuffer(0, m_ModelInstanceBuffer.get());
                            }

                            cmd->SetVertexBuffer(mesh.VertexBuffer.get());
                            cmd->SetIndexBuffer(mesh.IndexBuffer.get());
                            cmd->SetTexture(0, mesh.BaseColorTexture);
                            cmd->SetTexture(1, mesh.NormalTexture);
                            cmd->SetTexture(2, mesh.MetallicRoughnessTexture);
                            cmd->SetTexture(3, mesh.EmissiveTexture);
                            cmd->SetTexture(5, mesh.OcclusionTexture);
                            cmd->DrawIndexed(mesh.IndexCount, 0, 0, unit.InstanceCount);
                        }
                    }

                    // --- 水面へ映すドローンショーの機体 ---
                    // 平面反射は「カメラを鏡映しただけで世界は動かしていない」ので、
                    // 機体もそのままのワールド座標で、鏡映済みのビュー行列で描き直せばよい。
                    // これを描かないと、空には編隊が出ているのに水面には何も映らない
                    // (SSRパスがm_PlanarReflectionColorを水面へ合成する)
                    if (m_DroneShowEnabled && !m_DroneInstances.empty())
                    {
                        DirectX::XMFLOAT4X4 projection;
                        DirectX::XMStoreFloat4x4(&projection, jitteredProj);

                        DroneShowConstants droneConstants{};
                        // 鏡映×カメラのビュー行列。reflectedViewProjの分解と同じ組み合わせで、
                        // Projはメインカメラのジッター済みProjをそのまま使う
                        DirectX::XMStoreFloat4x4(
                            &droneConstants.View, DirectX::XMMatrixTranspose(reflectMatrix * viewMatrix));
                        DirectX::XMStoreFloat4x4(&droneConstants.Proj, DirectX::XMMatrixTranspose(jitteredProj));
                        droneConstants.Params0 = {
                            m_DroneShow.Data().Brightness * effectiveExposure,
                            m_DroneShowMinScreenRadius,
                            projection._11,
                            0.0f,
                        };
                        // 水面より下にいる機体は反射に映してはいけない。ジオメトリ側の
                        // SV_ClipDistance0(FrameConstants.PlanarReflectionPlane)と同じ規約・同じ平面
                        droneConstants.ClipPlane = { 0.0f, 1.0f, 0.0f, -waterPlaneY };
                        droneConstants.Params1 = { 1.0f, 0.0f, 0.0f, 0.0f };
                        cmd->UpdateBuffer(m_DroneShowConstantBuffer.get(), &droneConstants, sizeof(droneConstants));

                        // メイン描画とまったく同じPSOでよい(ビルボードの四隅はビュー空間で
                        // 足しており鏡映行列を通らないため、巻きが反転しない。
                        // 詳しい理由はPSO生成箇所のコメント)
                        cmd->SetPipelineState(m_DroneShowPipelineState.get());
                        cmd->SetConstantBuffer(1, m_DroneShowConstantBuffer.get());
                        cmd->SetVertexShaderResourceBuffer(0, m_DroneBuffer.get());
                        cmd->Draw(static_cast<uint32_t>(m_DroneInstances.size()) * 6u, 0);
                    }
                },
            });
        }

        // --- 反射パス: Lightingパスが適用した鏡面IBLを、実際に追跡した反射で差し替える(20章)。
        //     ScreenSpaceならSSR(レイマーチ)、RaytracedならRT反射(RayQuery)。
        //     Offならスキップし、後段のTonemapが直接m_SceneColorを読む ---
        if (m_ReflectionMode == ReflectionMode::ScreenSpace)
        {
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "SSR",
                // SSRはLightingパスが適用した鏡面IBLを「差し替える」ため、そのとき使ったものと
                // 同じ環境ソース(プローブ配列・グローバルのプリフィルタ済み鏡面)とBRDF LUT・AOを
                // 読む必要がある(20章)。
                // 手続き空はm_PrefilteredEnvTextureの焼き込み経由で入ってくるため、
                // 空のキューブマップをここで直接バインドする必要はない
                .Reads = {
                    m_SceneColor.get(), m_GBufferNormal.get(), m_GBufferMaterial.get(), m_GBufferDepth.get(),
                    m_GBufferAlbedo.get(), activeAOTexture, m_BRDFLUTTexture.get(), m_PrefilteredEnvTexture.get(),
                    m_ProbePrefilteredArray.get(), m_ProbeDistanceArray.get(),
                    // 平面反射。パスが登録されなかったフレームでもこのReadsは無害
                    // (今フレームのWriterが無いため単に依存辺が張られないだけ)
                    m_PlanarReflectionColor.get(),
                    // 大気散乱のSkyView LUT。水面に映る空をここから引く
                    m_SkyViewLUT.get(),
                    // bent normal(34章)。スペキュラ遮蔽をLightingパスと同じ規則で求めるために読む
                    m_GBufferBentNormal.get(),
                },
                .RenderTargets = { m_SSRTexture.get() },
                // 空パラメータ。SkyIntegrateパスより後に順序付けさせるために挙げる
                // (実際のバインドはExecute内)
                .BufferReads = { m_SkyParametersBuffer.get() },
                .Execute = [this, &gbufferViewport, activeAOTexture, usingProceduralSky,
                            planarReflectionPassRuns](RHI::IRHICommandList* cmd)
                {
                    // 水面の解析空フォールバック。手続き空が無効(.ksceneがDDSスカイボックスを
                    // 明示するシーン)なときは、m_WaterAnalyticSkyReflectionの値に関わらず必ず0にする
                    // ――DDSは任意の絵でPerezモデルとは無関係なため、SSR.hlsl側のSkyColorで
                    // 解析評価してはいけない(usingProceduralSkyはRender()前半で既に確定済み。
                    // DeferredLighting.hlsl向けのconstants.SkyParams.y代入と同じ判断)
                    const float waterAnalyticSkyFlag =
                        (m_WaterAnalyticSkyReflection && usingProceduralSky) ? 1.0f : 0.0f;
                    // 平面反射。このフレームでPlanarReflectionパスを実際に実行したときだけ
                    // 有効にする(登録されなかったフレームにm_PlanarReflectionColorの中身は
                    // 前フレーム/未定義の残骸なので、フラグをそのままSSR.hlsl側へ渡してはいけない)
                    const float planarReflectionFlag = planarReflectionPassRuns ? 1.0f : 0.0f;

                    SSRConstants ssrConstants{};
                    ssrConstants.Params0 =
                        { m_SSRMaxDistance, m_SSRThickness, m_SSRRoughnessCutoff, waterAnalyticSkyFlag };
                    ssrConstants.Params1 = { planarReflectionFlag, m_PlanarReflectionDistortion, 0.0f, 0.0f };
                    cmd->UpdateBuffer(m_SSRConstantBuffer.get(), &ssrConstants, sizeof(ssrConstants));

                    cmd->SetViewport(gbufferViewport);
                    cmd->SetPipelineState(m_SSRPipelineState.get());
                    cmd->SetConstantBuffer(0, m_FrameConstantBuffer.get());
                    cmd->SetConstantBuffer(1, m_SSRConstantBuffer.get());
                    cmd->SetSamplerSet(m_ScreenSpaceSamplers.get());
                    cmd->SetTexture(0, m_SceneColor.get());
                    cmd->SetTexture(1, m_GBufferNormal.get());
                    cmd->SetTexture(2, m_GBufferMaterial.get());
                    cmd->SetTexture(3, m_GBufferDepth.get());
                    cmd->SetTexture(4, m_GBufferAlbedo.get());
                    cmd->SetTexture(5, activeAOTexture);
                    cmd->SetTexture(6, m_BRDFLUTTexture.get());
                    cmd->SetTexture(7, m_PrefilteredEnvTexture.get());
                    cmd->SetTexture(8, m_ProbePrefilteredArray.get());
                    cmd->SetShaderResourceBuffer(9, m_ProbeBuffer.get());
                    cmd->SetTexture(10, m_ProbeDistanceArray.get());
                    // 平面反射。DX12はディスクリプタテーブルに未初期化のスロットが残ると
                    // 動作が未定義になるため、パスが無効なフレームでも常にバインドする
                    // (反射プローブ・DDGIと同じ理由)
                    cmd->SetTexture(11, m_PlanarReflectionColor.get());
                    // 空パラメータ。SSR.hlsl側はt12(t0〜t11が既に使用済み)
                    cmd->SetShaderResourceBuffer(12, m_SkyParametersBuffer.get());
                    // ボリュメトリック積雲の3Dノイズ。水面に映る雲も背景とまったく同じ
                    // 立体にならなければ「空の雲と水面の雲が別物」になるため、ここにも同じものを渡す
                    cmd->SetTexture(13, m_CloudShapeNoiseTexture.get());
                    cmd->SetTexture(14, m_CloudDetailNoiseTexture.get());
                    // 大気散乱のSkyView LUT。雲と同じ理由で、水面に映る空も
                    // 背景とまったく同じものでなければならない
                    cmd->SetTexture(15, m_SkyViewLUT.get());
                    // bent normal(34章)。Lightingパスとまったく同じものを読まないと、
                    // SSRが適用される領域とされない領域の境界に段差が出る。
                    // **t11は平面反射が使っているためt16へ移した**
                    cmd->SetTexture(16, m_GBufferBentNormal.get());
                    cmd->Draw(3, 0);
                },
            });
        }
        else if (ShouldRunRaytracedReflection())
        {
            // RT反射パス。読むものはSSRとほぼ同じ(同じ鏡面IBLを差し替えるため)で、
            // これに加えてTLASとシーンジオメトリの統合バッファを読む。
            // レジスタ割り当てはRTReflection.hlsl側の宣言と一致させること
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "RTReflection",
                .Reads = {
                    m_SceneColor.get(), m_GBufferNormal.get(), m_GBufferMaterial.get(), m_GBufferDepth.get(),
                    m_GBufferAlbedo.get(), activeAOTexture, m_BRDFLUTTexture.get(), m_PrefilteredEnvTexture.get(),
                    m_ProbePrefilteredArray.get(), m_GBufferBentNormal.get(),
                },
                .Writes = { m_RTReflectionTexture.get() },
                .Execute = [this, activeAOTexture](RHI::IRHICommandList* cmd)
                {
                    RTReflectionConstants rtConstants{};
                    rtConstants.Params0 = {
                        static_cast<float>(m_RenderWidth), static_cast<float>(m_RenderHeight),
                        m_RTReflectionMaxDistance, m_RTReflectionRoughnessCutoff
                    };
                    // yはメッシュレットのデバッグ表示。ラスタ側と同じトグルで駆動するので、
                    // 有効にすると「直接見えている面」と「反射に映る面」の両方が
                    // メッシュレット色になり、同じ塊が同じ色かを見比べられる
                    rtConstants.Params1 = {
                        m_RTReflectionShadowRayEnabled ? 1.0f : 0.0f,
                        m_MeshletDebugViewEnabled ? 1.0f : 0.0f,
                        0.0f,
                        0.0f,
                    };
                    cmd->UpdateBuffer(m_RTReflectionConstantBuffer.get(), &rtConstants, sizeof(rtConstants));

                    cmd->SetComputePipelineState(m_RTReflectionPipelineState.get());
                    // 【スクリーン空間用ではなくマテリアル用のセット】ヒット面のマテリアル
                    // テクスチャをbindlessで引くようになったため、s0にWrapのサンプラーが要る。
                    // モデルのUVはタイリング前提で[0,1]の外へ出るので、Clampで引くと
                    // 端のテクセルが引き伸ばされて模様が崩れる。
                    // s1(色バッファ)・s2(データ)はどちらのセットでも中身が同じで、
                    // s0でこのシェーダーが他に引くのはキューブマップだけ(アドレスモードは
                    // 面をまたぐフィルタに使われないため無関係)なので、切り替えの影響はここだけ
                    cmd->SetComputeSamplerSet(m_MaterialSamplers.get());
                    cmd->SetComputeConstantBuffer(0, m_FrameConstantBuffer.get());
                    cmd->SetComputeConstantBuffer(1, m_RTReflectionConstantBuffer.get());

                    cmd->SetComputeAccelerationStructure(0, m_RaytracingScene.GetTopLevelAS());
                    cmd->SetComputeTexture(1, m_SceneColor.get());
                    cmd->SetComputeTexture(2, m_GBufferNormal.get());
                    cmd->SetComputeTexture(3, m_GBufferMaterial.get());
                    cmd->SetComputeTexture(4, m_GBufferDepth.get());
                    cmd->SetComputeTexture(5, m_GBufferAlbedo.get());
                    cmd->SetComputeTexture(6, activeAOTexture);
                    cmd->SetComputeTexture(7, m_BRDFLUTTexture.get());
                    cmd->SetComputeTexture(8, m_PrefilteredEnvTexture.get());
                    cmd->SetComputeTexture(9, m_ProbePrefilteredArray.get());
                    cmd->SetComputeShaderResourceBuffer(10, m_ProbeBuffer.get());
                    cmd->SetComputeShaderResourceBuffer(11, m_RaytracingScene.GetVertexAttributeBuffer());
                    cmd->SetComputeShaderResourceBuffer(12, m_RaytracingScene.GetIndexBuffer());
                    cmd->SetComputeShaderResourceBuffer(13, m_RaytracingScene.GetMeshInfoBuffer());
                    cmd->SetComputeShaderResourceBuffer(14, m_RaytracingScene.GetInstanceInfoBuffer());
                    cmd->SetComputeShaderResourceBuffer(15, m_RaytracingScene.GetMaterialBuffer());
                    // メッシュレット表(t17)。RTReflection.hlslのKURENAI_RT_MESHLET_REGISTERと
                    // 一致させること。デバッグ表示でヒット面のメッシュレットを引くのに使う。
                    // 無いシーンでバインドしない理由はRTAO側と同じ。
                    // t8はプリフィルタ済み鏡面(上の16行目)が使っており空いていない
                    if (RHI::IRHIBuffer* meshletBuffer = m_RaytracingScene.GetMeshletTriangleOffsetBuffer())
                    {
                        cmd->SetComputeShaderResourceBuffer(17, meshletBuffer);
                    }
                    // bent normal(34章)。t0〜t15が埋まっているためt16。
                    // SSR.hlslと同じくスペキュラ遮蔽の方向依存を再現するために要る
                    cmd->SetComputeTexture(16, m_GBufferBentNormal.get());

                    // UAVはDispatch直後に解除されるため毎回バインドし直す(IRHICommandList.h参照)
                    cmd->SetComputeUnorderedAccessTexture(0, m_RTReflectionTexture.get());
                    cmd->Dispatch((m_RenderWidth + 7) / 8, (m_RenderHeight + 7) / 8, 1);
                },
            });
        }

        // --- 大気遠近パス: 反射パス(SSR/RT反射)の後、TAAパスの直前に置く。
        //     Lightingパスの中に入れない理由・TAAより前へ置く理由はShaders/3D/AerialPerspective.hlsl
        //     冒頭のコメント参照。無効時はパス自体を登録せず、reflectionOutputがそのまま
        //     TAA(またはTonemap)への入力になる ---
        RHI::IRHITexture* const reflectionOutput = GetActiveReflectionOutput();
        if (fogPassRuns)
        {
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "AerialPerspective",
                .Reads = { reflectionOutput, m_GBufferDepth.get(), m_SkyViewLUT.get() },
                .RenderTargets = { m_AerialPerspectiveTexture.get() },
                // 空パラメータ。SkyIntegrateパスの後へ順序付けさせるために挙げる
                // (実際のバインドはExecute内。SSRパスの同じ宣言と同じ理由)
                .BufferReads = { m_SkyParametersBuffer.get() },
                .Execute = [this, &gbufferViewport, reflectionOutput](RHI::IRHICommandList* cmd)
                {
                    cmd->SetViewport(gbufferViewport);
                    cmd->SetPipelineState(m_AerialPerspectivePipelineState.get());
                    cmd->SetConstantBuffer(0, m_FrameConstantBuffer.get());
                    cmd->SetSamplerSet(m_ScreenSpaceSamplers.get());
                    cmd->SetTexture(0, reflectionOutput);
                    cmd->SetTexture(1, m_GBufferDepth.get());
                    cmd->SetShaderResourceBuffer(2, m_SkyParametersBuffer.get());
                    // 大気散乱のSkyView LUT。in-scatter項に背景と同じ空の色を
                    // 使うのがこのパスの要点なので、当然同じLUTを読む
                    cmd->SetTexture(3, m_SkyViewLUT.get());
                    cmd->Draw(3, 0);
                },
            });
        }

        // --- TAAパス: 前フレームのTAA結果をモーションベクターで再投影し、今フレームの色へ蓄積する。
        //     ジッターで散らしたサンプルがここで平均され、実質的なスーパーサンプリングになる。
        //     トーンマップ前のHDRの段階で行うのは、露出・ブルームがTAAで安定した絵を入力に
        //     できるようにするため(逆順にするとブルームがフレームごとのちらつきを拾う)。
        //     入力はGetActiveReflectionOutput()(反射Off/SSR/RT反射のいずれか、または大気遠近が
        //     有効ならその出力)で、SSRだけを見ていた従来の判定ではRT反射有効時にTAAが古い
        //     SceneColorを拾ってしまうため、ここも合わせて直す ---
        RHI::IRHITexture* const taaInputColor = fogPassRuns ? m_AerialPerspectiveTexture.get() : reflectionOutput;

        // --- ドローンショーパス: 夜空の機体を発光ビルボードとして加算合成で描く ---
        //
        // 【なぜここなのか(大気遠近より後・TAAより前)】
        //  ・大気遠近より後: 機体は深度を書かないため、先に描くとAerialPerspectiveが
        //    「背後の空の距離」で霞を掛けてしまい、光点が washout する
        //  ・TAAより前: ここに置くとRenderGraphがtaaInputColorのRead-after-Write依存で
        //    自動的にTAAの前へ順序付ける。機体がTAA・自動露出・ブルーム・トーンマップを
        //    一貫して通るため、シーンの他の発光物とまったく同じ扱いになる。
        //    TAAの後(=m_TAAHistoryへ直接加算)にしてはいけない ―― 履歴を汚し、
        //    次フレーム以降に尾を引く
        //
        // 書き込み先をtaaInputColorにしているのは、反射やフォグの有無でHDRシーン色の実体が
        // 移り変わるため。「今のHDRシーン色」を指す変数へ描くことでどの組み合わせでも成立する
        if (m_DroneShowEnabled && !m_DroneInstances.empty())
        {
            const uint32_t droneCount = static_cast<uint32_t>(m_DroneInstances.size());
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "DroneShow",
                .RenderTargets = { taaInputColor },
                // 島や地形の後ろに回った機体を隠すために深度テストを行う(書き込みはしない)
                .DepthTarget = m_GBufferDepth.get(),
                .BufferReads = { m_DroneBuffer.get() },
                .Execute = [this, &gbufferViewport, viewMatrix, jitteredProj, effectiveExposure, droneCount](
                               RHI::IRHICommandList* cmd)
                {
                    DirectX::XMFLOAT4X4 projection;
                    DirectX::XMStoreFloat4x4(&projection, jitteredProj);

                    DroneShowConstants droneConstants{};
                    DirectX::XMStoreFloat4x4(&droneConstants.View, DirectX::XMMatrixTranspose(viewMatrix));
                    DirectX::XMStoreFloat4x4(&droneConstants.Proj, DirectX::XMMatrixTranspose(jitteredProj));
                    droneConstants.Params0 = {
                        // 実効プリ露出を掛ける。HDRバッファの中身はすべてプリ露出済みの値なので、
                        // ここで掛けないと機体だけが露出に追従しない浮いた明るさになる
                        m_DroneShow.Data().Brightness * effectiveExposure,
                        m_DroneShowMinScreenRadius,
                        // 射影行列の[0][0]。シェーダ側で最小画面サイズを世界半径へ逆算するのに使う
                        projection._11,
                        0.0f,
                    };
                    droneConstants.ClipPlane = { 0.0f, 1.0f, 0.0f, 0.0f };
                    // メイン描画ではクリップしない(平面反射パスだけが使う)
                    droneConstants.Params1 = { 0.0f, 0.0f, 0.0f, 0.0f };
                    cmd->UpdateBuffer(m_DroneShowConstantBuffer.get(), &droneConstants, sizeof(droneConstants));

                    cmd->SetViewport(gbufferViewport);
                    cmd->SetPipelineState(m_DroneShowPipelineState.get());
                    cmd->SetConstantBuffer(1, m_DroneShowConstantBuffer.get());
                    // 機体データは頂点シェーダーが読む。通常のSetShaderResourceBufferが使う
                    // SRVテーブルはピクセルシェーダーからしか見えないため専用の経路を使う
                    cmd->SetVertexShaderResourceBuffer(0, m_DroneBuffer.get());
                    // 1機につき2三角形。頂点バッファもインデックスバッファも要らない
                    cmd->Draw(droneCount * 6u, 0);
                },
            });
        }

        if (m_TAAEnabled)
        {
            // 今フレームの書き込み先と、前フレームの結果(履歴)。Render()の末尾で役割が入れ替わる
            const uint32_t historyWriteIndex = m_TAAHistoryIndex;
            const uint32_t historyReadIndex = 1u - historyWriteIndex;
            RHI::IRHITexture* const historyTexture = m_TAAHistory[historyReadIndex].get();

            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "TAA",
                // 履歴(historyTexture)は今フレーム誰も書かないので依存の辺は張られないが、
                // 実際にバインドするテクスチャはReadsにも宣言しておくというRenderGraphの規約に従う
                .Reads = { taaInputColor, historyTexture, m_GBufferVelocity.get(), m_GBufferDepth.get() },
                .RenderTargets = { m_TAAHistory[historyWriteIndex].get() },
                .Execute = [this, &gbufferViewport, taaInputColor, historyTexture, invViewProj, jitterUv,
                            effectiveExposure](RHI::IRHICommandList* cmd)
                {
                    TAAConstants taaConstants{};
                    DirectX::XMStoreFloat4x4(&taaConstants.InvViewProj, DirectX::XMMatrixTranspose(invViewProj));
                    taaConstants.PrevViewProj = m_TAAPrevViewProjValid ? m_TAAPrevViewProj : DirectX::XMFLOAT4X4{};
                    taaConstants.JitterUv = { jitterUv.x, jitterUv.y, m_TAAPrevJitterUv.x, m_TAAPrevJitterUv.y };
                    taaConstants.ScreenParams = {
                        static_cast<float>(m_RenderWidth),
                        static_cast<float>(m_RenderHeight),
                        1.0f / static_cast<float>(m_RenderWidth),
                        1.0f / static_cast<float>(m_RenderHeight),
                    };

                    // 履歴が無効な間は「サンプルすらするな」をシェーダへ伝える(TAA.hlsl参照)。
                    // 作りたてのfp16バッファはNaNを含みうるため、混ぜる割合を0にするだけでは足りない
                    const bool historyValid = m_TAAHistoryValid.load(std::memory_order_relaxed);

                    // プリ露出はm_EffectiveExposureEV100の時間順応で毎フレーム変わる。履歴は前フレームの
                    // 露出で焼かれた明るさのままなので、比率を掛けて今の露出へ揃える。
                    // 揃えないと露出が動いている間ずっと明るさの尾を引く
                    const float previousExposure = ComputeExposure(m_TAAPrevEffectiveExposureEV100);
                    const float exposureRescale =
                        (historyValid && previousExposure > 0.0f) ? (effectiveExposure / previousExposure) : 1.0f;

                    taaConstants.Params0 = {
                        m_TAABlendWeight,
                        m_TAAClipGamma,
                        historyValid ? 1.0f : 0.0f,
                        exposureRescale,
                    };
                    taaConstants.Params1 = {
                        static_cast<float>(m_TAAClipMode), m_TAAAntiFlicker, 0.0f, 0.0f
                    };
                    cmd->UpdateBuffer(m_TAAConstantBuffer.get(), &taaConstants, sizeof(taaConstants));

                    cmd->SetViewport(gbufferViewport);
                    cmd->SetPipelineState(m_TAAPipelineState.get());
                    cmd->SetConstantBuffer(1, m_TAAConstantBuffer.get());
                    cmd->SetSamplerSet(m_ScreenSpaceSamplers.get());
                    // t0〜t3はすべて必ずバインドすること。SRVのバインドは上書きするまで維持されるため、
                    // 省くと直前のパスが張ったテクスチャを読んでしまう
                    cmd->SetTexture(0, taaInputColor);
                    cmd->SetTexture(1, historyTexture);
                    cmd->SetTexture(2, m_GBufferVelocity.get());
                    cmd->SetTexture(3, m_GBufferDepth.get());
                    cmd->Draw(3, 0);
                },
            });
        }

        // --- Tonemapパス: HDRのSceneColor(反射パス有効時はその出力、TAA有効時はさらにTAA適用後)を
        //     LDRへ変換する。反射等のHDR演算がすべて完了した後、Present直前の独立したステージとして
        //     常に実行する ---
        // この行はTAAパスのAddPassより後に置くこと。ラムダは値キャプチャなので、先に差し替えると
        // TAAが自分の出力を入力として読む形になる(RenderGraphが循環を検出して例外を投げる)
        RHI::IRHITexture* hdrSceneColor = m_TAAEnabled ? m_TAAHistory[m_TAAHistoryIndex].get() : taaInputColor;

        // --- 自動露出パス: SceneColorの輝度ヒストグラムから目標EV100を求め、時間方向に順応させる。
        //     結果はm_ExposureTextureへ書かれ、後段のTonemapパスが読む(AutoExposure.hlsl参照) ---
        if (m_AutoExposureEnabled)
        {
            // シーン切り替え直後の1回だけ順応を飛ばす。パスを積んだ時点で消費しておくことで、
            // Executeが呼ばれる保証(グラフの枝刈り)に依存せず必ず1回で消える
            const bool resetAdaptation = m_AutoExposureResetRequested;
            m_AutoExposureResetRequested = false;

            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "AutoExposure",
                .Reads = { hdrSceneColor, m_GBufferDepth.get() },
                .Writes = { m_ExposureTexture.get() },
                .Execute = [this, hdrSceneColor, keyReferenceEV100, usingProceduralSky, resetAdaptation](
                    RHI::IRHICommandList* cmd)
                {
                    AutoExposureConstants autoExposureConstants{};
                    autoExposureConstants.InputSize = { m_RenderWidth, m_RenderHeight };
                    // Min>Maxのような不正な範囲だとヒストグラムのビン割りが破綻するため順序を保証する
                    autoExposureConstants.MinEV100 = std::min(m_AutoExposureMinEV100, m_AutoExposureMaxEV100);
                    autoExposureConstants.MaxEV100 = std::max(m_AutoExposureMinEV100, m_AutoExposureMaxEV100);
                    autoExposureConstants.PreExposureEV100 = m_EffectiveExposureEV100;
                    // 一時停止やシーン読み込み直後の巨大なdtで順応が飛ばないよう上限を設ける
                    autoExposureConstants.DeltaTime = std::clamp(m_RenderDeltaTime, 0.0f, 0.1f);
                    autoExposureConstants.AdaptationSpeedUp = m_AutoExposureSpeedUp;
                    autoExposureConstants.AdaptationSpeedDown = m_AutoExposureSpeedDown;
                    autoExposureConstants.LowPercentile = std::min(m_AutoExposureLowPercentile, m_AutoExposureHighPercentile);
                    autoExposureConstants.HighPercentile = std::max(m_AutoExposureLowPercentile, m_AutoExposureHighPercentile);
                    autoExposureConstants.ExposureCompensation = m_AutoExposureCompensation;
                    autoExposureConstants.NightRolloffEV = m_AutoExposureNightRolloffEV;
                    // 折れ点は必ずDark < Brightにする(逆転すると補正が不連続になる)
                    autoExposureConstants.NightRolloffDarkEV100 =
                        std::min(m_AutoExposureNightRolloffDarkEV100, m_AutoExposureNightRolloffBrightEV100);
                    autoExposureConstants.NightRolloffBrightEV100 =
                        std::max(m_AutoExposureNightRolloffDarkEV100, m_AutoExposureNightRolloffBrightEV100);
                    // 構図に依存しないシーンの基準EV。測光値の上限の足がかりになる。
                    //
                    // **手続き空を使っていないシーンではクランプを無効にする**。
                    // 基準EVはこのエンジンの太陽・月・空モデルが出す照度から求めているので、
                    // .ksceneが独自のスカイボックスを指定しているシーン(White Furnace Testなど)
                    // では、そのシーンを実際に照らしている光と無関係な値になってしまう。
                    // 実際、無効化前はWhite Furnace Testの一様グレーが107から208まで持ち上がり、
                    // 白飛びまで余裕が無くなっていた(一様性そのものは保たれていたが、
                    // 飽和させてしまうとエネルギー保存の検証が成立しなくなる)
                    autoExposureConstants.KeyReferenceEV100 = keyReferenceEV100;
                    autoExposureConstants.KeyCeilingEV =
                        usingProceduralSky ? m_AutoExposureKeyCeilingEV : 1.0e4f;
                    autoExposureConstants.ResetAdaptation = resetAdaptation ? 1.0f : 0.0f;
                    cmd->UpdateBuffer(m_AutoExposureConstantBuffer.get(), &autoExposureConstants, sizeof(autoExposureConstants));

                    // 1) ヒストグラムをゼロクリア
                    cmd->SetComputePipelineState(m_AutoExposureClearPipelineState.get());
                    cmd->SetComputeConstantBuffer(1, m_AutoExposureConstantBuffer.get());
                    cmd->SetComputeUnorderedAccessBuffer(0, m_ExposureHistogramBuffer.get());
                    cmd->Dispatch(1, 1, 1);

                    // 2) SceneColorから輝度ヒストグラムを構築
                    //    (UAVはDispatch直後に解除されるため毎回バインドし直す。IRHICommandList.h参照)
                    cmd->SetComputePipelineState(m_AutoExposureHistogramPipelineState.get());
                    cmd->SetComputeConstantBuffer(1, m_AutoExposureConstantBuffer.get());
                    cmd->SetComputeTexture(0, hdrSceneColor);
                    // 空(背景)を測光から外すために深度を読む(AutoExposure.hlsl参照)
                    cmd->SetComputeTexture(1, m_GBufferDepth.get());
                    cmd->SetComputeUnorderedAccessBuffer(0, m_ExposureHistogramBuffer.get());
                    cmd->Dispatch((m_RenderWidth + 15) / 16, (m_RenderHeight + 15) / 16, 1);

                    // 3) 縮約して目標EV100を求め、前フレームの値から指数的に順応させて書き戻す
                    cmd->SetComputePipelineState(m_AutoExposureResolvePipelineState.get());
                    cmd->SetComputeConstantBuffer(1, m_AutoExposureConstantBuffer.get());
                    cmd->SetComputeUnorderedAccessBuffer(0, m_ExposureHistogramBuffer.get());
                    cmd->SetComputeUnorderedAccessTexture(1, m_ExposureTexture.get());
                    cmd->Dispatch(1, 1, 1);
                },
            });
        }

        // --- ブルームパス: SceneColorから半解像度のピラミッドを作り、段階的にダウンサンプル→
        //     3x3テントでアップサンプルしながら加算する。最終段(m_BloomUpTextures[0])をTonemapが読む ---
        if (m_BloomEnabled && !m_BloomDownTextures.empty())
        {
            std::vector<RHI::IRHITexture*> bloomWrites;
            bloomWrites.reserve(m_BloomDownTextures.size() + m_BloomUpTextures.size());
            for (const auto& texture : m_BloomDownTextures)
            {
                bloomWrites.push_back(texture.get());
            }
            for (const auto& texture : m_BloomUpTextures)
            {
                bloomWrites.push_back(texture.get());
            }

            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "Bloom",
                .Reads = { hdrSceneColor, m_ExposureTexture.get() },
                .Writes = std::move(bloomWrites),
                .Execute = [this, hdrSceneColor, manualExposureScale](RHI::IRHICommandList* cmd)
                {
                    const uint32_t levelCount = static_cast<uint32_t>(m_BloomDownTextures.size());

                    BloomConstants bloomConstants{};
                    bloomConstants.Threshold = m_BloomThreshold;
                    bloomConstants.SoftKnee = m_BloomSoftKnee;
                    // しきい値を「表示上の白」基準の直感的な値のままにするため、
                    // ピラミッドの入力段で露出を反映する(Bloom.hlsl ExposureScale()参照)。
                    // Tonemapと同じ倍率でなければ、ブルームだけ露出がずれて合成比が狂う
                    bloomConstants.UseAutoExposure = m_AutoExposureEnabled ? 1.0f : 0.0f;
                    bloomConstants.PreExposureEV100 = m_EffectiveExposureEV100;
                    bloomConstants.ExposureScale = manualExposureScale;

                    // --- ダウンサンプル: SceneColor -> down[0] -> down[1] -> ... ---
                    cmd->SetComputePipelineState(m_BloomDownsamplePipelineState.get());
                    cmd->SetComputeSamplerSet(m_ScreenSpaceSamplers.get());
                    for (uint32_t level = 0; level < levelCount; ++level)
                    {
                        const bool isFirst = (level == 0);
                        RHI::IRHITexture* source = isFirst ? hdrSceneColor : m_BloomDownTextures[level - 1].get();
                        const DirectX::XMUINT2 srcSize = isFirst
                            ? DirectX::XMUINT2{ m_RenderWidth, m_RenderHeight }
                            : m_BloomLevelSizes[level - 1];
                        const DirectX::XMUINT2 dstSize = m_BloomLevelSizes[level];

                        bloomConstants.SrcSize = srcSize;
                        bloomConstants.DstSize = dstSize;
                        // 最初のダウンサンプルだけKaris平均としきい値を適用する(理由はBloom.hlsl冒頭)
                        bloomConstants.ApplyKarisAndThreshold = isFirst ? 1.0f : 0.0f;
                        cmd->UpdateBuffer(m_BloomConstantBuffer.get(), &bloomConstants, sizeof(bloomConstants));

                        cmd->SetComputeConstantBuffer(1, m_BloomConstantBuffer.get());
                        cmd->SetComputeTexture(0, source);
                        cmd->SetComputeTexture(2, m_ExposureTexture.get());
                        // UAVはDispatch直後に解除されるため毎回バインドし直す(IRHICommandList.h参照)
                        cmd->SetComputeUnorderedAccessTexture(0, m_BloomDownTextures[level].get());
                        cmd->Dispatch((dstSize.x + 7) / 8, (dstSize.y + 7) / 8, 1);
                    }

                    // --- アップサンプル: 最下段から上へ、down[level] + tent(1段下) を up[level] へ書く ---
                    cmd->SetComputePipelineState(m_BloomUpsamplePipelineState.get());
                    cmd->SetComputeSamplerSet(m_ScreenSpaceSamplers.get());
                    for (int32_t level = static_cast<int32_t>(levelCount) - 2; level >= 0; --level)
                    {
                        // 最下段の1つ上だけは、まだup[]が書かれていないのでdown[]の最下段を読む
                        const bool readsDownChain = (level == static_cast<int32_t>(levelCount) - 2);
                        RHI::IRHITexture* lower = readsDownChain
                            ? m_BloomDownTextures[level + 1].get()
                            : m_BloomUpTextures[level + 1].get();

                        const DirectX::XMUINT2 srcSize = m_BloomLevelSizes[level + 1];
                        const DirectX::XMUINT2 dstSize = m_BloomLevelSizes[level];

                        bloomConstants.SrcSize = srcSize;
                        bloomConstants.DstSize = dstSize;
                        bloomConstants.ApplyKarisAndThreshold = 0.0f;
                        cmd->UpdateBuffer(m_BloomConstantBuffer.get(), &bloomConstants, sizeof(bloomConstants));

                        cmd->SetComputeConstantBuffer(1, m_BloomConstantBuffer.get());
                        cmd->SetComputeTexture(0, m_BloomDownTextures[level].get());
                        cmd->SetComputeTexture(1, lower);
                        cmd->SetComputeUnorderedAccessTexture(0, m_BloomUpTextures[level].get());
                        cmd->Dispatch((dstSize.x + 7) / 8, (dstSize.y + 7) / 8, 1);
                    }
                },
            });
        }

        // Tonemapがブルームとして読むテクスチャ。無効時も有効なテクスチャを常にt2へバインドする
        // 必要があるため、その場合はピラミッド最上段(内容は前フレームのまま)を渡し、
        // BloomStrength=0で寄与しないようにする
        RHI::IRHITexture* bloomResultTexture =
            m_BloomUpTextures.empty() ? hdrSceneColor : m_BloomUpTextures[0].get();

        // このフレームで超解像パスを走らせるか。デバッグ表示中は内部解像度の中間バッファを
        // そのまま等倍で見たいので走らせない(拡大するとバッファの実際の解像度が分からなくなる)
        const bool upscaleActive = IsUpscaleActive() && m_DebugView == DebugView::Final;

        graph.AddPass(Core::RenderGraphPassDesc{
            .Name = "Tonemap",
            .Reads = { hdrSceneColor, m_ExposureTexture.get(), bloomResultTexture },
            .RenderTargets = { m_TonemapTexture.get() },
            .Execute = [this, &gbufferViewport, hdrSceneColor, bloomResultTexture, manualExposureScale,
                        keyReferenceEV100, upscaleActive](RHI::IRHICommandList* cmd)
            {
                TonemapConstants tonemapConstants{};
                tonemapConstants.Curve = static_cast<int32_t>(m_TonemapCurve);
                // 手動露出時: プリ露出は時刻連動で変動するので、設定EV100との差分を割り戻して
                // 「設定EV100で固定した絵」へ戻す(manualExposureScaleの算出箇所のコメント参照)
                tonemapConstants.ExposureScale = manualExposureScale;
                tonemapConstants.DitherStrength = m_DitherEnabled ? 1.0f : 0.0f;
                tonemapConstants.UseAutoExposure = m_AutoExposureEnabled ? 1.0f : 0.0f;
                tonemapConstants.PreExposureEV100 = m_EffectiveExposureEV100;
                tonemapConstants.BloomStrength =
                    (m_BloomEnabled && !m_BloomUpTextures.empty()) ? m_BloomStrength : 0.0f;
                tonemapConstants.MesopicStrength = m_MesopicStrength;
                // 目の順応は画面の構図ではなくシーンの明るさで決まるので、
                // 自動露出の測光値ではなくキー照度から求めた基準EVを使う
                tonemapConstants.MesopicAdaptationEV100 = keyReferenceEV100;
                // シャープネスはTAAの蓄積で失われた高域を戻すためのものなので、TAAが無効なら0。
                // そうしないとTAA導入前の絵と変わってしまう。
                //
                // 【超解像が有効なときも0にする】ここのシャープネスは内部レンダー解像度で効く。
                // その後EASUで拡大すると、戻した高域もオーバーシュートの縁も一緒に引き伸ばされて
                // 太い縁取りになる。超解像時のシャープ化は出力解像度で効くRCASへ一本化し、
                // ここは素直なトーンマップ出力をEASUへ渡すことに徹する
                tonemapConstants.Sharpness = (m_TAAEnabled && !upscaleActive) ? m_TAASharpness : 0.0f;
                tonemapConstants.InvRenderWidth = 1.0f / static_cast<float>(m_RenderWidth);
                tonemapConstants.InvRenderHeight = 1.0f / static_cast<float>(m_RenderHeight);
                tonemapConstants.BlackPoint = m_TonemapBlackPoint;
                cmd->UpdateBuffer(m_TonemapConstantBuffer.get(), &tonemapConstants, sizeof(tonemapConstants));

                cmd->SetViewport(gbufferViewport);
                cmd->SetPipelineState(m_TonemapPipelineState.get());
                cmd->SetConstantBuffer(1, m_TonemapConstantBuffer.get());
                cmd->SetSamplerSet(m_ScreenSpaceSamplers.get());
                cmd->SetTexture(0, hdrSceneColor);
                cmd->SetTexture(1, m_ExposureTexture.get());
                // t2は必ずバインドすること。SRVのバインドは上書きするまで維持されるため、
                // ここを省くと直前のパスが張ったテクスチャをブルームとして読んでしまう
                // (省くとG-Bufferのバッファを読んで画面全体が緑に転ぶ)
                cmd->SetTexture(2, bloomResultTexture);
                cmd->Draw(3, 0);
            },
        });

        // --- 超解像パス(41.23節): Tonemapが出したLDR画像を出力解像度へ再構成する ---
        //
        // EASU(拡大)とRCAS(シャープ化)を別パスにしているのは、RCASがEASUの結果の
        // 十字5タップを読むため。1つにまとめると同一リソースのSRV/UAV同時バインドになる。
        //
        // ここより後(Present)は出力解像度、ここより前はすべて内部レンダー解像度である。
        // ImGuiはRenderGraphの外でバックバッファへ直接描かれるため、この拡大の影響を受けない
        if (upscaleActive)
        {
            const uint32_t upscaleOutputWidth = m_UpscaleTargetWidth;
            const uint32_t upscaleOutputHeight = m_UpscaleTargetHeight;

            UpscaleConstants upscaleConstants{};
            ComputeEasuConstants(
                upscaleConstants, m_RenderWidth, m_RenderHeight, upscaleOutputWidth, upscaleOutputHeight);
            upscaleConstants.OutputSize = { upscaleOutputWidth, upscaleOutputHeight };
            upscaleConstants.RcasSharpnessScale = ComputeRcasSharpnessScale(m_UpscaleSharpness);

            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "UpscaleEASU",
                .Reads = { m_TonemapTexture.get() },
                .Writes = { m_UpscaleTexture.get() },
                .Execute = [this, upscaleConstants, upscaleOutputWidth,
                            upscaleOutputHeight](RHI::IRHICommandList* cmd)
                {
                    cmd->SetComputePipelineState(m_UpscaleEASUPipelineState.get());
                    // Gather4のアドレスモードがClampであることがEASUの前提(Upscale.hlslのコメント参照)
                    cmd->SetComputeSamplerSet(m_ScreenSpaceSamplers.get());
                    cmd->UpdateBuffer(m_UpscaleConstantBuffer.get(), &upscaleConstants, sizeof(upscaleConstants));
                    cmd->SetComputeConstantBuffer(1, m_UpscaleConstantBuffer.get());
                    cmd->SetComputeTexture(0, m_TonemapTexture.get());
                    // UAVはDispatch直後に解除されるため毎回バインドし直す
                    cmd->SetComputeUnorderedAccessTexture(0, m_UpscaleTexture.get());
                    cmd->Dispatch((upscaleOutputWidth + 7) / 8, (upscaleOutputHeight + 7) / 8, 1);
                },
            });

            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "UpscaleRCAS",
                .Reads = { m_UpscaleTexture.get() },
                .Writes = { m_UpscaleSharpTexture.get() },
                .Execute = [this, upscaleConstants, upscaleOutputWidth,
                            upscaleOutputHeight](RHI::IRHICommandList* cmd)
                {
                    cmd->SetComputePipelineState(m_UpscaleRCASPipelineState.get());
                    // RCASはLoadで整数座標を引くのでサンプラーは使わないが、シェーダーが
                    // Samplers.hlsliを取り込んで宣言している以上バインドはしておく
                    cmd->SetComputeSamplerSet(m_ScreenSpaceSamplers.get());
                    cmd->UpdateBuffer(m_UpscaleConstantBuffer.get(), &upscaleConstants, sizeof(upscaleConstants));
                    cmd->SetComputeConstantBuffer(1, m_UpscaleConstantBuffer.get());
                    cmd->SetComputeTexture(0, m_UpscaleTexture.get());
                    cmd->SetComputeUnorderedAccessTexture(0, m_UpscaleSharpTexture.get());
                    cmd->Dispatch((upscaleOutputWidth + 7) / 8, (upscaleOutputHeight + 7) / 8, 1);
                },
            });
        }

        // --- Presentパス: 選択中のレンダーターゲットを、アスペクト比を保ってバックバッファへ出力 ---
        // デバッグ表示(Render Targets UI)で選択されたバッファに応じて表示ソースを切り替える。
        // 深度バッファ(GBuffer深度・シャドウマップ)はPresent.hlsl側でグレースケール化するためMode=1を渡す
        RHI::IRHITexture* presentSourceTexture = m_TonemapTexture.get();
        // Mode 9(IBL Irradiance/Prefilterのキューブマップ表示)専用。他のModeでは使われないが、
        // t1には常に何らかの有効なTextureCubeをバインドしておく必要があるため既定値を持たせる
        RHI::IRHITexture* presentDebugCubeTexture = skyTexture;
        // Mode 10(シャドウマップのカスケード表示)専用。t1と同じ理由で、t2にも常に有効な
        // Texture2DArrayをバインドしておく必要があるためシャドウマップ配列自身を既定値にする
        RHI::IRHITexture* presentDebugArrayTexture = m_ShadowCascadeArray.get();
        // Mode 12(反射プローブのキューブマップ配列)専用。TextureCube(t1)ともTexture2DArray(t2)とも
        // 型が違うためさらに別スロット(t4)が要る。こちらも常に有効なテクスチャをバインドしておく
        // (反射プローブは鏡面専任なので、既定値はプリフィルタ済み鏡面の配列にしてある)
        RHI::IRHITexture* presentDebugCubeArrayTexture = m_ProbePrefilteredArray.get();
        // Mode 18(雲の3Dノイズ)専用。Texture3Dはここまでのどの型とも別なのでさらに
        // 別スロット(t5)が要る。他と同じく常に有効なテクスチャをバインドしておく
        RHI::IRHITexture* presentDebugVolumeTexture = m_CloudShapeNoiseTexture.get();
        int32_t presentMode = 0;
        uint32_t presentSourceWidth = m_RenderWidth;
        uint32_t presentSourceHeight = m_RenderHeight;
        switch (m_DebugView)
        {
        case DebugView::Final:
            // Tonemapパスが既にSSR有効/無効を考慮したHDRソースをLDR変換済みのため、そのまま使う。
            // 超解像が有効なときは、その先のRCASまで通した出力解像度の結果へ差し替える。
            // レターボックスの基準になるpresentSourceWidth/Heightも出力解像度にすること
            // (ここを内部解像度のままにすると、拡大済みの絵をさらに拡大してしまう)
            if (upscaleActive)
            {
                presentSourceTexture = m_UpscaleSharpTexture.get();
                presentSourceWidth = m_UpscaleTargetWidth;
                presentSourceHeight = m_UpscaleTargetHeight;
            }
            else
            {
                presentSourceTexture = m_TonemapTexture.get();
            }
            break;
        case DebugView::Albedo:
            presentSourceTexture = m_GBufferAlbedo.get();
            break;
        case DebugView::Normal:
            presentSourceTexture = m_GBufferNormal.get();
            presentMode = 7; // オクタヘドラルエンコードをデコードして[0,1]へ再マップして表示
            break;
        case DebugView::Material:
            presentSourceTexture = m_GBufferMaterial.get();
            break;
        case DebugView::Emissive:
            presentSourceTexture = m_GBufferEmissive.get();
            break;
        case DebugView::Depth:
            presentSourceTexture = m_GBufferDepth.get();
            presentMode = 2;
            break;
        case DebugView::DepthRaw:
            presentSourceTexture = m_GBufferDepth.get();
            presentMode = 5; // 生の深度値(0〜1)を加工せずそのまま表示(reverse-z等の生値確認用)
            break;
        case DebugView::DirectLight:
            presentSourceTexture = m_DirectLightTexture.get();
            presentMode = 4; // HDRのためトーンマッピング(Reinhard)+ガンマ補正して表示
            break;
        case DebugView::MegaLights:
            // MegaLightsが求めたポイント/スポットの直接光。パスが今フレーム実行されていない場合、
            // バッファの中身は前フレーム/未定義の残骸なので最終結果のまま何も切り替えない
            // (SWラスタ・PlanarReflection・RTShadowのデバッグ表示と同じ方針)。
            // ModeはDebugView::DirectLightと同じ4 ―― 並べて差分を取るのが目的なので、
            // 表示側の処理まで一致させる
            if (ShouldRunMegaLights())
            {
                presentSourceTexture = m_MegaLightsTexture.get();
                presentMode = 4;
            }
            break;
        case DebugView::AOIndirectLight:
            presentSourceTexture = activeAOTexture;
            presentMode = 0; // rgb(間接拡散光)をそのまま表示。SSAOはrgbが常に0のため常に黒になる
            break;
        case DebugView::AOIndirectLightRaw:
            presentSourceTexture = activeAORawTexture;
            presentMode = 0; // ブラー前の生値(タイル状ノイズが乗った状態)
            break;
        case DebugView::AOOcclusion:
            presentSourceTexture = activeAOTexture;
            presentMode = 3; // a(遮蔽率)をグレースケール表示
            break;
        case DebugView::AOOcclusionRaw:
            presentSourceTexture = activeAORawTexture;
            presentMode = 3; // ブラー前の生値(タイル状ノイズが乗った状態)
            break;
        case DebugView::ShadowMap:
            // Texture2DArrayはSourceTexture(t0、Texture2D)へバインドできないため、専用の
            // DebugArrayTexture(t2)を表示スライス指定付きでサンプルする(IBLキューブマップの
            // Mode 9と同じ方式。Present.hlsl参照)
            presentDebugArrayTexture = m_ShadowCascadeArray.get();
            presentMode = 10;
            presentSourceWidth = kShadowMapSize;
            presentSourceHeight = kShadowMapSize;
            break;
        case DebugView::RTShadow:
            // 可視率(0〜1のスカラー)をそのままグレースケール表示する。RTシャドウを実行していない
            // フレーム(非対応環境・手法がRaytraced以外)はテクスチャの中身が意味を持たないため、
            // 最終結果のまま何も切り替えない
            if (ShouldRunRaytracedShadow())
            {
                presentSourceTexture = m_RTShadowTexture.get();
                presentMode = 5;
            }
            break;
        case DebugView::SSR:
            // 反射がOffのときは反射パスをスキップしているため、Tonemapパスの入力もSceneColorになり
            // 結果的にFinalと同一表示になる(SSR / RT反射のどちらでも同じ扱い)
            presentSourceTexture = m_TonemapTexture.get();
            break;
        case DebugView::HiZ:
            presentSourceTexture = m_HiZTexture.get();
            presentMode = 6; // 指定ミップをSampleLevelで読みグレースケール表示
            presentSourceWidth = std::max(1u, m_RenderWidth >> m_HiZDebugMipLevel);
            presentSourceHeight = std::max(1u, m_RenderHeight >> m_HiZDebugMipLevel);
            break;
        case DebugView::IBLIrradiance:
            // 本物のTextureCubeのため、SourceTexture(t0、Texture2D)ではなくDebugCubeTexture(t1)を
            // 現在のカメラ視線方向でサンプルする(Present.hlsl Mode 9、presentDebugCubeTexture参照)
            presentDebugCubeTexture = m_IrradianceTexture.get();
            presentMode = 9;
            presentSourceWidth = m_RenderWidth;
            presentSourceHeight = m_RenderHeight;
            break;
        case DebugView::IBLPrefilter:
            presentDebugCubeTexture = m_PrefilteredEnvTexture.get();
            presentMode = 9;
            presentSourceWidth = m_RenderWidth;
            presentSourceHeight = m_RenderHeight;
            break;
        case DebugView::ProbePrefilter:
            presentDebugCubeArrayTexture = m_ProbePrefilteredArray.get();
            presentMode = 12;
            break;
        case DebugView::ProbeInfluence:
            // 塗り分けはDeferredLighting.hlsl側(FrameConstants.ProbeParams.y)で行うため、
            // Presentは通常どおり最終結果を表示するだけでよい
            presentSourceTexture = m_TonemapTexture.get();
            break;
        case DebugView::ProbeDistance:
            // 距離キューブ(19.12節)。格納値はワールド距離なので専用のMode 13でGain倍して
            // グレースケール表示する(Mode 12でそのまま出すと数メートルで白飛びする)
            presentDebugCubeArrayTexture = m_ProbeDistanceArray.get();
            presentMode = 13;
            break;
        case DebugView::IBLBRDFLUT:
            presentSourceTexture = m_BRDFLUTTexture.get();
            presentMode = 0; // (A, B, Eavg)の生値をそのままRGBとして表示(値域はおおむね[0,1])
            presentSourceWidth = kIBLBRDFLUTSize;
            presentSourceHeight = kIBLBRDFLUTSize;
            break;
        case DebugView::Bloom:
            // ピラミッド最上段(半解像度、HDR)。Mode 4でトーンマッピングしてから表示する
            if (!m_BloomUpTextures.empty())
            {
                presentSourceTexture = m_BloomUpTextures[0].get();
                presentMode = 4;
                presentSourceWidth = m_BloomLevelSizes[0].x;
                presentSourceHeight = m_BloomLevelSizes[0].y;
            }
            break;
        case DebugView::LightTiles:
            // ライトグリッドは構造化バッファなのでSourceTexture(t0)では受け取れず、専用のt3から読む
            // (Present.hlsl Mode 11)。t0には何かをバインドしておく必要があるため、
            // 解像度だけ合わせてm_TonemapTextureをそのまま渡す(Mode 11では読まれない)
            presentSourceTexture = m_TonemapTexture.get();
            presentMode = 11;
            break;
        case DebugView::MegaLightsAverage:
            // 蓄積した平均。1フレームも足していないうちは中身が未定義なので切り替えない
            if (m_MegaLightsAccumFrames > 0u && m_MegaLightsAccumBuffer)
            {
                presentSourceTexture = m_TonemapTexture.get();
                presentMode = 22;
            }
            break;
        case DebugView::MegaLightsTilePool:
            // 候補プールも構造化バッファなのでt3から読む(Present.hlsl Mode 21)。t0の扱いは
            // Mode 11と同じ。パスが走っていないフレームは中身が前フレーム/未定義の残骸なので、
            // 最終結果のまま何も切り替えない(他のMegaLights系の表示と同じ方針)
            if (ShouldRunMegaLights() && m_MegaLightsTilePoolBuffer)
            {
                presentSourceTexture = m_TonemapTexture.get();
                presentMode = 21;
            }
            break;
        case DebugView::BentNormal:
            // bent normal(34章)。正規化しないベクトルなので、法線表示(Mode 7)のような
            // オクタヘドラルのデコードは通さず専用のModeで扱う。
            // 【15ではなく19】15はDDGIのイラディアンスアトラスが使っている。Present.hlslの
            // PSMainではそちらの分岐が先にreturnするため、15を割り当てるとbent normalの
            // 表示へ到達できない(Present.hlsl冒頭のMode一覧を参照)
            presentSourceTexture = m_GBufferBentNormal.get();
            presentMode = 19;
            break;
        case DebugView::MotionVector:
            // 速度バッファ。格納値はUV単位(1画素ぶんの移動で1/解像度、1920幅なら約0.0005)と
            // 極端に小さく、そのまま色として出しても真っ黒にしか見えない。専用のMode 14で
            // ピクセル単位へ換算してから中間灰色を原点に色付けする
            presentSourceTexture = m_GBufferVelocity.get();
            presentMode = 14;
            break;
        case DebugView::SceneColorRaw:
            // トーンマップもガンマも通さないリニア値をそのまま出す。スペキュラのエネルギー補正の
            // 各方式を数値で突き合わせるための測定用(14.9.9節)。
            // バックバッファが8bit UNormのため1.0を超える値はクリップする ―― 測定時は
            // EV100を上げてピークが1.0未満に収まるようにしてから読むこと。
            // TAAが有効な場合、hdrSceneColorはTAAの蓄積結果(23章)を指す。静止して収束させれば
            // ジッターの平均が取れたぶん単フレームより安定した値が読めるが、カメラを動かした
            // 直後の数フレームは履歴が混ざっているため、値を読むのは静止させてから
            presentSourceTexture = hdrSceneColor;
            presentMode = 0;
            break;
        case DebugView::DDGIIrradiance:
        case DebugView::DDGIDistance:
        case DebugView::DDGIProbeBackface:
        {
            // アトラスはただのTexture2Dなのでt0でそのまま受け取れる(22章)。
            // 反射プローブのキューブと違い専用スロットは要らない。
            // アトラスは横長(列=Cx*Cy、行=Cz)なので、レターボックスがその比率に合うよう
            // 実寸を渡す。渡さないと画面いっぱいへ引き伸ばされ、セルが正方形に見えなくなる
            // 裏面率はイラディアンスアトラスのαなので、資源も寸法もイラディアンスと同じ
            const bool isIrradiance =
                (m_DebugView == DebugView::DDGIIrradiance || m_DebugView == DebugView::DDGIProbeBackface);
            const uint32_t cell = isIrradiance ? kDDGIIrradianceCell : kDDGIDistanceCell;
            const uint32_t columns = m_GIVolume.ProbeCounts[0] * m_GIVolume.ProbeCounts[1];
            const uint32_t rows = m_GIVolume.ProbeCounts[2];

            presentSourceTexture = isIrradiance ? m_DDGIIrradianceAtlas.get() : m_DDGIDistanceAtlas.get();
            // Present.hlslのMode 14はモーションベクター(TAA、23章)が既に使っているため、
            // DDGIのイラディアンス/距離モーメントはMode 15/16にずらしてある
            presentMode = (m_DebugView == DebugView::DDGIProbeBackface) ? 20 : (isIrradiance ? 15 : 16);
            presentSourceWidth = m_HasGIVolume ? columns * cell : cell;
            presentSourceHeight = m_HasGIVolume ? rows * cell : cell;
            break;
        }
        case DebugView::WaterMask:
            // G-BufferのMaterial.a(水面のマテリアルID)をそのままグレースケール表示する。
            // 0/1の二値なのでMode 3(Gain倍する遮蔽率表示)ではなく専用のMode 17を使う
            presentSourceTexture = m_GBufferMaterial.get();
            presentMode = 17;
            break;
        case DebugView::PlanarReflection:
            // 平面反射パスの出力。パスが今フレーム実行されていない(無効化・水面なし)場合、
            // m_PlanarReflectionColorの中身は前フレーム/未定義の残骸なので最終結果のまま何も
            // 切り替えない(RTShadowデバッグ表示と同じ方針)
            if (planarReflectionPassRuns)
            {
                // HDRのためMode 4でReinhardトーンマッピング+ガンマ補正して表示する
                // (DirectLight/Bloomと同じ扱い)。専用のMode追加は不要でPresent.hlslは無変更のまま使える
                presentSourceTexture = m_PlanarReflectionColor.get();
                presentMode = 4;
                presentSourceWidth = m_PlanarReflectionWidth;
                presentSourceHeight = m_PlanarReflectionHeight;
            }
            break;
        case DebugView::AtmosphereLUT:
            // 大気散乱のLUT。HDRなのでMode 4(Reinhard+ガンマ)で表示する。
            // Transmittanceは0〜1なのでそのままでも読めるが、MultiScatteringは値が小さいので
            // 表示輝度の倍率と併用する
            if (m_AtmosphereLUTDebugIndex == 1)
            {
                presentSourceTexture = m_MultiScatteringLUT.get();
                presentSourceWidth = kMultiScatteringLUTSize;
                presentSourceHeight = kMultiScatteringLUTSize;
            }
            else if (m_AtmosphereLUTDebugIndex == 2)
            {
                presentSourceTexture = m_SkyViewLUT.get();
                presentSourceWidth = kSkyViewLUTWidth;
                presentSourceHeight = kSkyViewLUTHeight;
            }
            else
            {
                presentSourceTexture = m_TransmittanceLUT.get();
                presentSourceWidth = kTransmittanceLUTWidth;
                presentSourceHeight = kTransmittanceLUTHeight;
            }
            presentMode = 4;
            break;
        case DebugView::SoftwareRaster:
        case DebugView::SoftwareRasterDepth:
        case DebugView::SoftwareRasterNormal:
            // 自前ソフトウェアラスタライザ(46章)の出力。パスが今フレーム実行されていない場合、
            // バッファの中身は前フレーム/未定義の残骸なので最終結果のまま何も切り替えない
            // (PlanarReflection・RTShadowのデバッグ表示と同じ方針)。
            //
            // 【Modeはハードウェア側と同じものを使う】深度はDebugView::DepthRawと同じMode 5、
            // 法線はDebugView::Normalと同じMode 7。並べて差分を取るのが目的なので、
            // 表示側の処理まで完全に一致させる。Present.hlslは無変更のまま使える
            if (softwareRasterPassRuns)
            {
                if (m_DebugView == DebugView::SoftwareRasterDepth)
                {
                    presentSourceTexture = m_SoftwareRasterDepth.get();
                    presentMode = 5;
                }
                else if (m_DebugView == DebugView::SoftwareRasterNormal)
                {
                    presentSourceTexture = m_SoftwareRasterNormal.get();
                    presentMode = 7;
                }
                else
                {
                    // フラット陰影はHDRのためMode 4(Reinhard+ガンマ)
                    presentSourceTexture = m_SoftwareRasterColor.get();
                    presentMode = 4;
                }
            }
            break;
        case DebugView::CloudNoiseSlice:
        {
            // 雲の3Dノイズ。形状(128^3)とディテール(32^3)を切り替えて任意のスライスを見る。
            // 正方形のテクスチャなので表示も正方形にする(レターボックスの計算に渡す)
            const bool showDetail = m_CloudNoiseDebugShowDetail;
            presentDebugVolumeTexture =
                showDetail ? m_CloudDetailNoiseTexture.get() : m_CloudShapeNoiseTexture.get();
            presentMode = 18;
            const uint32_t size = showDetail ? kCloudDetailNoiseSize : kCloudShapeNoiseSize;
            presentSourceWidth = size;
            presentSourceHeight = size;
            break;
        }
        }

        // Mode 11(ライトグリッド)とMode 21(MegaLightsの候補プール)はどちらもt3の構造化バッファを
        // 読むが、1タイルぶんの要素数が違う。バッファと容量は必ず対で切り替えること
        // (片方だけ切り替えると、正しいバッファを別のストライドで読んで無関係な値をヒートマップにする)
        const bool presentUsesTilePool = (presentMode == 21) && m_MegaLightsTilePoolBuffer != nullptr;
        RHI::IRHIBuffer* const presentTileBuffer =
            presentUsesTilePool ? m_MegaLightsTilePoolBuffer.get() : m_LightTileBuffer.get();
        const uint32_t presentTileCapacity =
            presentUsesTilePool ? kMegaLightsTilePoolCapacity : kLightTileCapacity;

        PresentConstants presentConstants{};
        presentConstants.Mode = presentMode;
        presentConstants.TileParams =
        {
            static_cast<float>(m_LightTileCountX),
            static_cast<float>(kLightTileSize),
            static_cast<float>(presentTileCapacity),
            // ヒートマップで赤に振り切る基準のライト数。容量そのものを基準にすると
            // 実データ(数灯)ではほぼ真っ青で差が読めないため、別のつまみにしてある
            static_cast<float>(std::max(1, m_LightTileHeatmapMax)),
        };
        presentConstants.TileRenderSize =
        {
            static_cast<float>(m_RenderWidth),
            static_cast<float>(m_RenderHeight),
            0.0f,
            0.0f,
        };
        // Mode 22(蓄積平均)が割る数。0で割らないよう下限1
        presentConstants.AccumParams =
        {
            static_cast<float>(std::max(1u, m_MegaLightsAccumFrames)),
            0.0f,
            0.0f,
            0.0f,
        };
        if (m_DebugView == DebugView::IBLPrefilter)
        {
            presentConstants.MipLevel = static_cast<float>(m_IBLPrefilterDebugMipLevel);
        }
        else if (m_DebugView == DebugView::IBLIrradiance)
        {
            presentConstants.MipLevel = 0.0f; // イラディアンスマップは常に1ミップのみ
        }
        else if (m_DebugView == DebugView::ProbePrefilter)
        {
            presentConstants.MipLevel = static_cast<float>(m_ProbePrefilterDebugMipLevel);
        }
        else
        {
            presentConstants.MipLevel = static_cast<float>(m_HiZDebugMipLevel);
        }
        // ArraySliceはMode 10ではカスケード番号、Mode 12ではプローブ番号として使う。
        // プローブが1つも無い場合でも配列の範囲外を引かないようクランプする
        if (m_DebugView == DebugView::ProbePrefilter || m_DebugView == DebugView::ProbeDistance)
        {
            presentConstants.ArraySlice = static_cast<float>(
                std::clamp(m_ProbeDebugIndex, 0, std::max(0, static_cast<int32_t>(m_ReflectionProbes.size()) - 1)));
        }
        else if (m_DebugView == DebugView::CloudNoiseSlice)
        {
            // Mode 18ではW座標(0〜1)として使う。3Dテクスチャなので配列番号ではなく連続値
            presentConstants.ArraySlice = std::clamp(m_CloudNoiseDebugSlice, 0.0f, 1.0f);
        }
        else
        {
            presentConstants.ArraySlice =
                static_cast<float>(std::clamp(m_ShadowDebugCascade, 0, static_cast<int32_t>(kCascadeCount) - 1));
        }
        // Finalの見た目は倍率の影響を受けてはならないため、デバッグ表示のときだけ倍率を掛ける
        // (Gainはゼロ初期化のままだと0倍=真っ黒になるので、必ず明示的に設定すること)
        if (m_DebugView == DebugView::ProbeDistance || m_DebugView == DebugView::DDGIDistance)
        {
            // 距離は色ではなくワールド距離なので、Debug View Gain(1倍以上)ではなく
            // 「白になる距離」の逆数を渡す。Present.hlsl Mode 13/15の式は他と同じ「値×Gain」のまま。
            // DDGI側は距離がMaxRayDistanceでクランプされているので、そこを白にすると
            // 「クランプに当たっている方向」が一目で分かる
            const float whiteAt = (m_DebugView == DebugView::DDGIDistance)
                ? m_GIVolume.MaxRayDistance
                : m_ProbeDistanceDebugRange;
            presentConstants.Gain = 1.0f / std::max(whiteAt, 0.01f);
        }
        else if (m_DebugView == DebugView::DDGIIrradiance)
        {
            // アトラスは露出非依存の物理量で持っている(FrameConstants::DDGIParams4 参照)ため、
            // そのまま出すと昼は数万倍の値になって白飛びする。表示だけ実効プリ露出を掛けて
            // 他のバッファと同じ表示レンジへ揃える。こうしておくと
            // 「IBL - イラディアンス」の表示と直接見比べられる(22.9.1節の検証がこれに依存している)
            presentConstants.Gain = m_DebugViewGain * effectiveExposure;
        }
        else
        {
            presentConstants.Gain = (m_DebugView == DebugView::Final) ? 1.0f : m_DebugViewGain;
        }
        commandList->UpdateBuffer(m_PresentConstantBuffer.get(), &presentConstants, sizeof(presentConstants));

        // レターボックス/ピラーボックスの余白もクリア色のまま残るよう、絞ったビューポートで描画する
        const RHI::Viewport letterboxViewport = ComputeLetterboxViewport(
            m_Window->GetWidth(), m_Window->GetHeight(), presentSourceWidth, presentSourceHeight);

        graph.AddPass(Core::RenderGraphPassDesc{
            .Name = "Present",
            .Reads = { presentSourceTexture, presentDebugCubeTexture, presentDebugArrayTexture,
                       presentDebugCubeArrayTexture, presentDebugVolumeTexture },
            // DebugView::LightTilesでライトグリッドを、DebugView::MegaLightsTilePoolで候補プールを
            // 読むため、それぞれの書き手より後に順序付ける(表示していないフレームでも
            // 同じポインタになるだけで無害)
            .BufferReads = { m_LightTileBuffer.get(), presentTileBuffer, m_MegaLightsAccumBuffer.get() },
            .SwapChainTarget = m_SwapChain.get(),
            .Execute = [this, &letterboxViewport, presentSourceTexture, presentDebugCubeTexture,
                        presentDebugArrayTexture, presentDebugCubeArrayTexture,
                        presentDebugVolumeTexture, presentTileBuffer](RHI::IRHICommandList* cmd)
            {
                cmd->ClearRenderTarget({ 0.05f, 0.05f, 0.08f, 1.0f });
                cmd->ClearDepth(1.0f);
                cmd->SetViewport(letterboxViewport);

                cmd->SetPipelineState(m_PresentPipelineState.get());
                cmd->SetConstantBuffer(0, m_FrameConstantBuffer.get());
                cmd->SetConstantBuffer(1, m_PresentConstantBuffer.get());
                cmd->SetSamplerSet(m_ScreenSpaceSamplers.get());
                cmd->SetTexture(0, presentSourceTexture);
                cmd->SetTexture(1, presentDebugCubeTexture);
                cmd->SetTexture(2, presentDebugArrayTexture);
                // Mode 11(ライトグリッド)/ Mode 21(候補プール)以外でも、シェーダが宣言している
                // リソースは必ずバインドする(SetPipelineStateが毎回ルート引数を無効化するため)
                cmd->SetShaderResourceBuffer(3, presentTileBuffer);
                // Mode 22(蓄積平均)専用。読まれないModeでも必ずバインドする(上と同じ理由)
                cmd->SetShaderResourceBuffer(6, m_MegaLightsAccumBuffer.get());
                cmd->SetTexture(4, presentDebugCubeArrayTexture);
                cmd->SetTexture(5, presentDebugVolumeTexture);
                cmd->Draw(3, 0);
            },
        });

        graph.Execute();

        // --- メッシュレットカリングの統計を読み戻す(Stage 5-2) ---
        //
        // 【GPUを待たない】直前に積んだコピーはまだ実行されていないので、リングの中で
        // **最も古いもの**(kMeshletCullStatsRingSize-1 = 2フレーム前に書いたもの)を読む。
        // DX12はkFrameCount(=2)フレームぶんCPUが先行するため、2フレーム前のGPU実行は
        // 完了している。待ちを入れるとフレームが直列化し、計測のために計測対象を壊す。
        //
        // 【読めなかったフレームは足さない】DX11のMap(DO_NOT_WAIT)はまだ実行中ならfalseを返す。
        // 0として集計に足すと間引き率が実際より低く出るので、そのフレームは丸ごと飛ばす
        if (meshletCullStatsActive)
        {
            const uint32_t oldestIndex = (m_MeshletCullStatsRingIndex + 1) % kMeshletCullStatsRingSize;
            uint32_t counters[kMeshletCullStatsCount] = {};
            if (m_MeshletCullStatsReadback[oldestIndex] &&
                m_MeshletCullStatsReadback[oldestIndex]->ReadbackData(counters, sizeof(counters)))
            {
                m_MeshletCullTested = counters[0];
                m_MeshletCullFrustumCulled = counters[1];
                m_MeshletCullOcclusionCulled = counters[2];

                m_FrameStatsMeshletTestedSum += m_MeshletCullTested;
                m_FrameStatsMeshletFrustumCulledSum += m_MeshletCullFrustumCulled;
                m_FrameStatsMeshletOcclusionCulledSum += m_MeshletCullOcclusionCulled;
                ++m_FrameStatsMeshletSampleCount;
            }
            m_MeshletCullStatsRingIndex = (m_MeshletCullStatsRingIndex + 1) % kMeshletCullStatsRingSize;
        }
        else
        {
            // 統計を切っている間に古い値が残っていると、UIやログが「今もこの数だけ間引いている」
            // ように見える。切った時点で0へ戻す
            m_MeshletCullTested = 0;
            m_MeshletCullFrustumCulled = 0;
            m_MeshletCullOcclusionCulled = 0;
        }

        // --- モデル単位のGPUカリングの結果を読み戻す(Stage 5-3) ---
        // リングの理由も「読めなかったフレームは足さない」もメッシュレット統計と同じ
        if (modelCullReady)
        {
            // 今フレームのCPU側の結果を、GPUのコピーとまったく同じ位置へ積む。
            // 読むときに同じ位置から取れば、比べるのは同じフレームのもの同士になる
            m_ModelCullCpuFrustumHistory[m_ModelCullRingIndex] = m_ModelCullCpuFrustumCulled;
            // 【比べる相手はG-Bufferぶんの候補数】GPU側の「判定」もそこだけを数えている
            m_ModelCullCandidateHistory[m_ModelCullRingIndex] =
                m_ModelCullCandidateCount - m_ModelCullPrepassCandidateCount;

            const uint32_t oldest = (m_ModelCullRingIndex + 1) % kMeshletCullStatsRingSize;
            uint32_t counters[kModelCullCounterCount] = {};
            if (m_ModelCullReadback[oldest] &&
                m_ModelCullReadback[oldest]->ReadbackData(counters, sizeof(counters)))
            {
                m_ModelCullTested = counters[0];
                m_ModelCullFrustumCulled = counters[1];
                m_ModelCullOcclusionCulled = counters[2];
                m_ModelCullSurvived = counters[3];
                for (uint32_t region = 0; region < kModelCullRegionCount; ++region)
                {
                    m_ModelCullRegionIssued[region] = counters[4 + region];
                }
                m_ModelCullComparedCpuFrustumCulled = m_ModelCullCpuFrustumHistory[oldest];
                m_ModelCullComparedCandidateCount = m_ModelCullCandidateHistory[oldest];
            }
            m_ModelCullRingIndex = (m_ModelCullRingIndex + 1) % kMeshletCullStatsRingSize;
        }
        else
        {
            m_ModelCullTested = 0;
            m_ModelCullFrustumCulled = 0;
            m_ModelCullOcclusionCulled = 0;
            m_ModelCullSurvived = 0;
            std::fill(std::begin(m_ModelCullRegionIssued), std::end(m_ModelCullRegionIssued), 0u);
        }

        // ImGuiはPresentパスでバインドされたバックバッファにそのまま重ねて描画する。
        // GPU側は計測していない(このスコープ専用の描画パイプラインを持たないため)が、
        // CPU側のコマンド記録コストはDX11/DX12で差が出やすいのでここも計測しておく
        m_CPUProfiler.BeginScope("ImGui");
        m_ImGuiBackend->Render();
        m_CPUProfiler.EndScope(); // ImGui

        // Present呼び出しでコマンドリストが実行投入される(DX12)ため、それより前にEndFrame()で
        // フレーム終端のタイムスタンプ書き込み・結果リードバックのコマンドを記録しておく必要がある
        m_GPUProfiler->EndFrame();

        // ExecuteCommandLists・実際のPresent・(DX12のみ)フェンス待ちを含む区間。
        // Present呼び出し自体のCPUコストはここで計測しないと、各パスのコマンド記録時間の
        // 合計とCPU Frame Time全体の差分がどこにあるのか分からなくなるため計測しておく
        m_CPUProfiler.BeginScope("PresentSubmit");
        m_SwapChain->Present(m_VSyncEnabled);
        m_CPUProfiler.EndScope(); // PresentSubmit

        // GPUの完了待ち(DX12のフレームパイプライン化に伴うフェンス待ち)は実際のCPU負荷ではなく
        // GPU側の処理時間の反映なので、PresentSubmitの計測値からは除外しておく
        m_CPUProfiler.SubtractFromScope("PresentSubmit", m_Device->GetLastFrameGPUWaitTimeMs());

        // --- 次フレームがこのフレームを「前フレーム」として参照するための状態を確定させる ---
        // 早期returnより後のここで行うことで、描画を行わなかったフレームでは前フレームの状態が
        // そのまま保たれ、履歴テクスチャの中身と行列の対応が1フレームずれない
        m_TAAPrevViewProj = constants.ViewProj;
        m_TAAPrevJitterUv = jitterUv;
        // Hi-Zオクルージョンカリングが「1フレームぶんの視差ずれ」を見積もるのに使う。
        // m_TAAPrevViewProjと同じ場所・同じタイミングで書くので有効性の管理も同じで済む
        m_PrevCameraPosition = { constants.CameraPosition.x, constants.CameraPosition.y, constants.CameraPosition.z };
        m_TAAPrevViewProjValid = true;
        // --- GPU計測の書き出し(計測専用) ---
        // 【毎フレーム走る場所へ置くこと】Perfログを出す関数は1秒に1回しか先へ進まない
        // (集計期間に達するまで早期returnする)。そこへ置くと収集が毎秒になり、
        // 300フレーム集めるのに5分かかって測定が終わらない
        // 【Perfログとは別に集める】あちらは0.05ms未満を落とし1フレームの代表値しか出さない。
        // ここでは**閾値なしで全パスを、指定枚数ぶん平均**する
        if (!m_PerfDumpPath.empty() && !m_PerfDumpDone && m_GPUProfiler)
        {
            ++m_PerfDumpWarmupFrames;
            // 整定を待つ。内部解像度の切り替えとストリーミングが片付くまで
            if (m_PerfDumpWarmupFrames > static_cast<int32_t>(kMegaLightsAccumWarmup))
            {
                for (const RHI::GPUTimingResult& pass : m_GPUProfiler->GetResults())
                {
                    m_PerfDumpTotals[pass.Name] += static_cast<double>(pass.TimeMs);
                }
                ++m_PerfDumpCollected;

                if (m_PerfDumpCollected >= m_PerfDumpTargetFrames)
                {
                    m_PerfDumpDone = true;
                    std::ofstream file(m_PerfDumpPath, std::ios::trunc);
                    if (file)
                    {
                        file << "pass,avg_ms\n";
                        for (const auto& entry : m_PerfDumpTotals)
                        {
                            file << entry.first << ','
                                 << (entry.second / static_cast<double>(m_PerfDumpCollected)) << '\n';
                        }
                        file << "__frames," << m_PerfDumpCollected << '\n';
                        Core::Logger::Info(
                            "KurenaiEngine3D",
                            "GPU計測を書き出しました: " + Core::WideToUtf8(m_PerfDumpPath) + " (" +
                                std::to_string(m_PerfDumpCollected) + "フレームの平均)");
                    }
                    else
                    {
                        Core::Logger::Error(
                            "KurenaiEngine3D",
                            "GPU計測を書き出せませんでした(ファイルを開けない): " +
                                Core::WideToUtf8(m_PerfDumpPath));
                    }
                }
            }
        }

        m_TAAPrevEffectiveExposureEV100 = m_EffectiveExposureEV100;

        // MegaLightsの時間再利用も同じ場所でping-pongを反転する。
        // 今フレームの書き込み先が、次フレームでは履歴(読み込み元)になる
        {
            const bool temporalRan = ShouldRunMegaLights() && m_MegaLightsMode == MegaLightsMode::Stochastic &&
                                     m_MegaLightsTemporalEnabled && m_MegaLightsTemporalPipelineState &&
                                     m_MegaLightsReservoirHistory[0] && m_MegaLightsHistoryGuide[0];
            // 【手法3もガイドを書くので同じ反転が要る】あちらは時間再利用を持たないが、
            // デノイザが読む「前フレームの幾何」を Resolve が書いている。反転しないと
            // 同じフレームで書いた側を読むことになり、比べたい「別のフレームの同じ点」に
            // ならない(そのうえ RenderGraph は WAR の辺を張らないので競合する)
            const bool quadGuideRan = ShouldRunMegaLights() &&
                                      m_MegaLightsMode == MegaLightsMode::QuadShared &&
                                      m_MegaLightsResolvePipelineState && m_MegaLightsHistoryGuide[0];
            if (temporalRan || quadGuideRan)
            {
                m_MegaLightsHistoryIndex ^= 1u;
                // 【1フレーム走ってから有効にする】書いた側を次フレームが読むので、
                // 反転したあとに立てる。立てるのが早いと未初期化の内容を履歴として読む
                m_MegaLightsHistoryValid = true;
            }
            else
            {
                // 走らなかったフレームを挟むと履歴が途切れる(中身が古い or 未初期化)
                m_MegaLightsHistoryValid = false;
            }
            // 露出はパスの有無に関わらず記録する(次に走ったときの比較の基準になる)
            m_MegaLightsPrevEffectiveExposureEV100 = m_EffectiveExposureEV100;

            // デノイザの履歴も同じ場所で反転する。今フレームの書き込み先が次フレームの履歴になる
            // 【時間再利用の有無には依存しない】デノイザは「出た色」をならすもので、
            // リザーバを混ぜる時間再利用とは独立に効く。条件を混ぜると、片方を切ったときに
            // もう片方の履歴まで無効になって原因が分からなくなる
            const bool denoiseRan = ShouldRunMegaLights() &&
                                    (m_MegaLightsMode == MegaLightsMode::Stochastic ||
                                     m_MegaLightsMode == MegaLightsMode::QuadShared) &&
                                    m_MegaLightsDenoiseEnabled && m_MegaLightsDenoiseTemporalPSO &&
                                    m_MegaLightsDenoisedTexture != nullptr;
            if (denoiseRan)
            {
                m_MegaLightsDenoiseHistoryIndex ^= 1u;
                m_MegaLightsDenoiseHistoryValid = true;
            }
            else
            {
                // 走らなかったフレームを挟むと履歴が途切れる(中身が古い)
                m_MegaLightsDenoiseHistoryValid = false;
            }
        }

        if (m_TAAEnabled)
        {
            // 今フレームの書き込み先が、次フレームでは履歴(読み込み元)になる
            m_TAAHistoryIndex ^= 1u;
            m_TAAHistoryValid.store(true, std::memory_order_relaxed);
        }
        else
        {
            // 無効の間は履歴を更新していないので、再度有効化されたときに古い絵が混ざらないよう落としておく
            m_TAAHistoryValid.store(false, std::memory_order_relaxed);
        }
    }
}
