#pragma once

#include <DirectXMath.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Model.h"
#include "ModelLoader.h"
#include "ShowLoader.h"

namespace Kurenai::Assets
{
    // 1つのModelInstanceが持てるLODの段数の上限(.ksceneのPathを含む)。
    // .kmodelのMeshEntryが持つメッシュレットLODの上限(kMaxMeshletLODCount)と、
    // DDGIボリュームのLODCountの上限に揃えてある。
    // これ以上粗くしたいものは、段を増やすのではなく別のシーンへ分ける粒度になる
    inline constexpr size_t kMaxModelLODCount = 4;
    // モデルインスタンスの常駐状態(ストリーミング)。
    //
    // 値が動くのは[Scene]StreamingDistanceを指定したシーンだけで、
    // 指定が無ければ読み込み時のLoadedのまま変化しない(従来どおりの全常駐)。
    //
    // 【なぜ表示を先に用意するか】破棄が早すぎる/範囲内なのに読み込まれない、といった破綻は
    // 画面を見ても分からない(そこに何も無いのが正しいのか間違いなのか区別できない)。
    // 常駐状態を位置ごとに見られるようにしておかないと、入れた後で原因を切れない
    enum class ResidencyState : uint8_t
    {
        Unloaded = 0,   // 範囲外。ジオメトリ/テクスチャをGPUに載せていない
        Loading  = 1,   // 読み込み中
        Loaded   = 2,   // 常駐。描画できる
    };

    // メッシュ1つぶんのワールド空間AABB。ModelInstanceが自分のModelのメッシュ数だけ持つ。
    //
    // 【なぜ配列でなく構造体にするか】IsAABBVisible(KurenaiEngine3D.cpp)が
    // const float(&)[3] を2本取るため、min/maxが同じ要素の中に隣り合っている必要がある
    struct MeshWorldBounds
    {
        float Min[3] = { 0.0f, 0.0f, 0.0f };
        float Max[3] = { 0.0f, 0.0f, 0.0f };
    };

    // シーン内に配置された1つのモデルインスタンス。Modelのジオメトリ自体は
    // ワールド空間原点に焼き込み済み(ModelLoader.cpp参照)のままなので、
    // 実際の配置はWorld/NormalMatrixで頂点シェーダー側にて適用する(KurenaiEngine3D::
    // MakeObjectConstants参照)
    struct ModelInstance
    {
        // 【値ではなく共有ポインタで持つ理由】同じ.kmodelを複数のインスタンスが指せるようにするため。
        // Modelは頂点/インデックス/テクスチャのGPUリソースを丸ごと抱えるので、同じパスを2回読むと
        // VRAMに二重に載る(MultiModelTest.ksceneは同じ.kmodelを3回配置しており、実際に3重だった)。
        // 実体はScene::ModelCacheが所有し、ここはその共有参照になる。
        //
        // constなのは、読み込み後にモデルの中身を書き換える経路を作らないため。1つのModelを
        // 複数のインスタンスが共有するので、片方から書き換えるともう片方に波及する。
        // 唯一の例外だったRaytracingSceneのCPUコピー解放はScene::ModelCache側を回す形へ移した
        std::shared_ptr<const Model> Model;

        // モデルLODの2段目以降(.ksceneの[Model]LODPath / LODDistance)。粗くなっていく順。
        // LODModels[i] は「カメラからの距離が LODDistances[i] 以上」のときに使う段で、
        // どれにも当てはまらない(=最も近い)ときは上のModelを使う。
        // LODを持たないインスタンスでは両方とも空になり、従来どおりModelだけが描かれる。
        //
        // 【距離はAABBの最近接点まで】中心距離だと1.1km四方のPLATEAUタイルで破綻する
        // (タイルの端に立っていても中心までは500m以上あるため、近景なのに粗い段が選ばれる)
        //
        // 【型を Assets:: で修飾する理由】直前のメンバ名 Model が型名 Model を隠すため、
        // ここから素の Model と書くと「型ではない」とコンパイルエラーになる(C2327)
        std::vector<std::shared_ptr<const Assets::Model>> LODModels;
        std::vector<float> LODDistances;

        // --- ストリーミング([Scene]StreamingDistance 指定時) --------------------------------
        //
        // 実体を読み込むのに必要な .kmodel のフルパス。Model / LODModels と同じ並びで、
        // 先頭が Model、以降が LODModels に対応する。
        //
        // 【ストリーミングしないシーンでも埋める】どちらの経路でも同じデータが揃っているほうが、
        // 「読み込み済みかどうか」の判定を Model が空かどうかの1点に寄せられる
        std::vector<std::wstring> ModelPaths;

        // 各段が読み込み済みか。ストリーミングしないシーンでは全段が読み込み済みで始まる。
        // 実体そのものは Model / LODModels の shared_ptr が空かどうかで判る
        bool IsLODLoaded(size_t level) const
        {
            return (level == 0) ? static_cast<bool>(Model) : static_cast<bool>(LODModels[level - 1]);
        }

        DirectX::XMFLOAT4X4 World;          // Scale * Rotation * Translation(転置済み、HLSLへそのまま渡せる形)
        DirectX::XMFLOAT4X4 NormalMatrix;   // Worldの3x3部分の逆転置(4x4に格納、転置済み)
        float TangentSignFlip = 1.0f;       // Worldの行列式が負(ミラーリング)なら-1

        // Worldの行列式が負(ミラーリング)か。TangentSignFlipと同じ条件から求まるが、用途が異なる。
        // TangentSignFlipは接線の向きを補正するためにシェーダーへ渡す係数で、こちらは三角形の
        // ワインディングが反転することへの対処(表裏判定を入れ替えたパイプラインで描く)に使う
        bool IsMirrored = false;

        // .ksceneの[Model]Waterで指定される。trueの場合、KurenaiEngine3DはこのインスタンスをG-Bufferパスの
        // 通常PSOではなく水面専用PSO(Water.hlsl)で描画し、G-BufferのMaterial.aへ水面のマテリアルID
        // (kMaterialIDWater、Shaders/3D/GBufferCommon.hlsli)を書き込む(水面マテリアル基盤)
        bool IsWater = false;

        // Model::BoundsMin/MaxをWorldで変換した、ワールド空間の軸並行バウンディングボックス。
        // フラスタムカリングの判定に使う。
        //
        // 【読み込み時に一度だけ求める】Worldは読み込み後に変化しない(書き込みはSceneLoaderの
        // 1箇所のみ)ため、毎フレーム8頂点を変換し直す必要がない。
        // 回転が入ると軸並行でなくなるので、min/maxだけを変換するのではなく必ず8頂点を変換して
        // その包絡を取る(シーン全体のAABBを合成しているのと同じループで求めている)
        float WorldBoundsMin[3] = { 0.0f, 0.0f, 0.0f };
        float WorldBoundsMax[3] = { 0.0f, 0.0f, 0.0f };

        // Model::MeshesのメッシュごとのワールドAABB(要素数はModel.Meshes.size()と同じ)。
        // 上のWorldBoundsMin/Maxと同じくSceneLoaderが読み込み時に一度だけ求める。
        //
        // 【なぜインスタンス側に持つか】Meshのローカル空間AABBはModelが持つが、
        // ワールド空間の値はインスタンスのWorldに依存する。将来同じModelを複数のインスタンスで
        // 共有するようになっても壊れないよう、変換後の値はこちらへ置く。
        //
        // 【空になることがある】.kmodelがv10より前のメッシュ単位AABBを持たない世代…という
        // 分岐は無い(v10未満は読み込み自体が拒否される)。空になるのはメッシュ0個のモデルだけ。
        // 描画側は「要素数がMeshesと一致していること」を前提にしてよいが、
        // 念のため添字の範囲は確かめること
        std::vector<MeshWorldBounds> MeshWorldBoundsList;

        // ストリーミングの常駐状態と、いま使っているモデルLODの段(0が最も詳細)。
        //
        // 【書き込むのはエンジン側の毎フレーム更新1箇所ずつ】Residencyは
        // KurenaiEngine3D::UpdateModelStreaming、LODLevelはUpdateModelLODが書く。
        // SceneLoaderが入れるLoaded/0は「ストリーミングもLODも使わないシーン」の値。
        // 詳細はResidencyStateのコメント
        ResidencyState Residency = ResidencyState::Loaded;
        uint32_t LODLevel = 0;
    };

    // 反射プローブ(リフレクションプローブ)。この位置から周囲をキューブマップへキャプチャし、
    // 畳み込んだものを影響範囲内のピクセルのIBL環境ソースとして使う。シーン全体で1つしかない
    // スカイボックス由来のIBLと違い、位置ごとに異なる環境(屋内なら屋内の壁・天井)を反映できる
    // 反射プローブの影響範囲の形状
    enum class ReflectionProbeShape
    {
        // 中心からの距離だけで判定する球。設定項目がRadiusひとつで済む反面、部屋の形に
        // 沿わせられず、視差補正(下記)も行えない
        Sphere,
        // プローブ位置を中心とする、Y軸まわりに回転できる直方体(OBB)。部屋の壁・床・天井に
        // 合わせて置くことで、影響範囲が部屋の外へはみ出さなくなるうえ、反射ベクトルを
        // この箱と交差させる視差補正が使えるようになる
        Box,
    };

    struct ReflectionProbe
    {
        // ワールド空間のキャプチャ位置(この点から6方向を撮る)。Box形状の場合は箱の中心でもある
        float Position[3] = { 0.0f, 0.0f, 0.0f };
        // 影響範囲の半径(ワールド単位)。この球の内側のピクセルがこのプローブの環境を受け、
        // 外側はスカイボックス由来のグローバルIBLへフォールバックする(Sphere形状のときのみ使用)
        float Radius = 10.0f;

        ReflectionProbeShape Shape = ReflectionProbeShape::Sphere;
        // Box形状の各軸の半径(ハーフエクステント)。プローブのローカル空間(Yaw回転後)での値
        float BoxExtents[3] = { 10.0f, 10.0f, 10.0f };
        // Box形状のY軸まわりの回転(度)。壁が軸に平行でない部屋へ合わせるためのもの。
        // 傾いた床・天井を持つ空間は想定していないため、ピッチ・ロールは持たない
        float YawDegrees = 0.0f;

        // 影響範囲の境界から内側へ何ワールド単位かけて重みを1まで立ち上げるか。
        // 0だと境界でプローブが突然切り替わり継ぎ目が出る(Phase 1の挙動)。
        // 重なり合うプローブ同士・プローブとグローバルIBLの間の滑らかな移行に使う
        float BlendDistance = 2.0f;

        std::string Name;
    };

    // DDGI(Dynamic Diffuse Global Illumination、22章)のプローブ格子を張る直方体ボリューム。
    //
    // 反射プローブ(上)が「少数を手で置き、主に鏡面の映り込みを担う」のに対し、こちらは
    // 「格子状に多数を自動配置し、拡散の間接光だけを担う」。両者は目的が違うため併用する。
    //
    // 各プローブはオクタヘドラル投影の2Dアトラスへ、方向ごとのイラディアンスと
    // 「その方向の面までの距離」の2つのモーメントを持つ。後者があることで
    // 「このプローブからこのピクセルは見えているか」を統計的に判定でき、
    // 仕切りの向こう側の明るさが漏れてくるのを抑えられる(これがDDGIの要)
    struct GIVolume
    {
        // ボリュームの最小コーナー(ワールド空間)。プローブiは Origin + i * ProbeSpacing に置かれる。
        // 中心指定ではなく最小コーナー指定なのは、格子の位置を間隔と個数から一意に決めるため
        float Origin[3] = { 0.0f, 0.0f, 0.0f };
        // 各軸のプローブ間隔(ワールド単位)。狭いほど間接光の空間解像度が上がるがプローブ数が増える
        float ProbeSpacing[3] = { 2.0f, 2.0f, 2.0f };
        // 各軸のプローブ数。トライリニア補間は周囲8個を使うため、各軸2以上でなければならない
        uint32_t ProbeCounts[3] = { 8u, 4u, 8u };

        // 遮蔽判定の照会点を面の法線方向へ浮かせる量(ワールド単位)。
        // これが無いと「面が、自分を直接照らしているプローブから見えていない」と誤判定し、
        // 画面全体が一様に暗くなる(22章の自己遮蔽)
        float NormalBias = 0.25f;
        // 同じく視線方向へ寄せる量。深度の量子化が効く浅い角度の面で法線方向だけでは足りないため
        float ViewBias = 0.10f;

        // 履歴とのブレンド率。1に近いほど滑らかに追従する代わりに、光が変わってからの
        // 収束が遅くなる。0なら毎回上書き(時間分割と噛み合わず、更新されたプローブだけが
        // 突然変わってちらつく)
        float Hysteresis = 0.97f;

        // 距離モーメントを記録する際の上限(ワールド単位)。
        //
        // 【必須の値であり、大きくしてはいけない】ジオメトリに当たらなかった方向には
        // 十分大きな値が入っている(IBLConvolve.hlslのkProbeSkyDistance = 1e6)。これを
        // そのまま平均すると2つの意味で壊れる:
        //   1. 分散 σ² = 平均二乗距離 - 平均距離² が桁落ちで潰れる。1e6の二乗は1e12で、
        //      fp32の有効桁(約7桁)ではこの引き算から意味のある分散が残らない
        //   2. 空と壁が同じテクセルに混ざったとき、平均が空側に完全に引っ張られ、
        //      チェビシェフ判定が「どこも遠い(=何にも遮蔽されない)」に倒れる
        // チェビシェフ判定に必要なのは「近いか遠いか」の区別だけなので、遠方を潰しても
        // 判定の意味は変わらない。プローブ間隔の数倍を目安にする
        float MaxRayDistance = 8.0f;

        // クリップマップLODの段数。1なら従来どおりの単一格子。
        //
        // LOD k は間隔が ProbeSpacing * 2^k、プローブ数は全LOD共通なので、
        // 覆う範囲はLODが1つ上がるごとに2倍になる。近くは密に、遠くは粗くしたい
        // 広いシーン(島全体など)のためのもの。
        // プローブの総数は ProbeCounts の積 × LODCount になる
        uint32_t LODCount = 1u;

        // 各LODの原点をカメラへ追従させるか。falseなら従来どおりOriginに固定する。
        //
        // 【追従させるときは必ずそのLOD自身の格子へスナップする】スナップすれば、
        // カメラが動いてもプローブのワールド座標は動かない(動くのは「どのプローブが
        // 範囲に入っているか」だけ)ので、範囲に残ったプローブの焼き上がりを使い回せる。
        // スナップしないと毎フレーム全プローブが焼き直しになる
        bool FollowCamera = false;

        std::string Name;
    };

    // エミッシブなメッシュから起こした光源プロキシ1つ分(**ワールド空間**)。
    // Mesh::EmissiveClusters(ローカル空間)を ModelInstance の変換で移したもので、
    // SceneLoader が読み込み時に一度だけ作る(シーンは読み込み後に変形しない)。
    struct EmissiveProxy
    {
        // ワールド空間の位置。クラスタの重心そのもの。
        // 【法線方向へずらさない】面上に置いたままでよい ―― 減衰の分母が d^2 ではなく
        // d^2 + SourceRadius^2 になっており、面の上では cos も 0 に落ちるため、
        // 自分自身を照らす寄与はここで既に潰れている(docs/ImplementationDetail.md 62章)
        float Position[3] = { 0.0f, 0.0f, 0.0f };
        // 放射の向き(ワールド空間、正規化済み)。
        // 【NormalMatrix(逆転置)で移すこと】これは光の進行方向ではなく**面の法線**である。
        // すぐ下にあるモデル埋め込みライトの Direction は World でそのまま回してよいが
        // (方向ベクトルなので)、こちらを同じように扱うと非一様スケールで黙ってずれる
        float Direction[3] = { 0.0f, 1.0f, 0.0f };
        // 面が出している放射輝度。EmissiveFactor × エミッシブテクスチャの平均色。
        //
        // 【シーン全体の倍率(m_EmissiveIntensity)も露出も掛けない】倍率は毎フレームの
        // ライトリスト構築で掛ける ―― そうしないとImGuiのスライダーが効かなくなる。
        // 露出はそもそも掛けてはいけない(G-Bufferのエミッシブが露出を通らないため。62章)
        float RadianceBase[3] = { 0.0f, 0.0f, 0.0f };
        // ワールド空間の総面積[m^2]
        float Area = 0.0f;
        // 指向性 κ(Assets::EmissiveCluster::Directionality と同じ量)
        float Directionality = 0.0f;
        // ワールド空間の面積等価円板半径[m]
        float SourceRadius = 0.0f;
        // どのインスタンスのどのメッシュのどのクラスタか。
        // 【採用順を決めるときのタイブレークに使う】スコアが同値のときにこの3つ組の辞書順で
        // 決めると、フレーム間で順序が揺れない(揺れるとライトが出入りしてちらつく)
        uint32_t InstanceIndex = 0;
        uint32_t MeshIndex = 0;
        uint32_t ClusterIndex = 0;
    };

    struct Scene
    {
        std::wstring Name;

        // 全モデルで共有する1x1のフォールバックテクスチャ(白/フラット法線/黒/マゼンタ)。
        //
        // 【ModelCache/Instancesより後ろに置いてはいけない】メンバはここでの宣言順に構築され、
        // 逆順に破棄される。Modelが持つMeshはここのテクスチャを生ポインタで指しているため、
        // 先に破棄されると解放済みを指す。Modelを持つ2つより前に宣言してこの順序を保証する。
        //
        // 【以前はここがInstancesより後ろにあった】コメントは「Instancesより前に宣言する」と
        // 書いてあるのに、実際の宣言はInstancesの後ろにあった(=破棄はInstancesより先)。
        // Modelのデストラクタがこの生ポインタを読まないため実害は出ていなかったが、
        // ModelCacheがModelの実体を所有するようになって順序の重みが増したので、
        // コメントが元から要求していた並びへ直した
        SharedTexturePool SharedTextures;

        // .kmodelのパス(assetRootDirectoryを含む絶対パス)から読み込み済みModelを引くキャッシュ。
        // Modelの実体を所有するのはここで、ModelInstance::Modelはこれへの共有参照になる。
        //
        // 【SharedTexturesより後ろ・Instancesより前】上のSharedTexturesのコメントの理由で
        // SharedTexturesより後ろに、そしてInstancesが指す先を先に消さないためInstancesより前に置く
        // (破棄は Instances -> ModelCache -> SharedTextures の順になる)。
        //
        // 【スレッド】読み書きするのはLoaderスレッドのLoadSceneだけで、Renderスレッドは
        // ApplyLoadedSceneで受け取った後に読むだけ。したがってロックを持たない
        std::unordered_map<std::wstring, std::shared_ptr<Model>> ModelCache;

        std::vector<ModelInstance> Instances;

        // 各ModelInstanceが持つModel::Lights(モデルファイル埋め込みのライト。glTFのKHR_lights_punctual
        // やFBXのライトノード由来)をInstance::Worldでワールド空間へ変換したものと、.kscene自身の
        // [Light]セクションで直接指定されたライト(元からワールド空間)を合成した、シーン全体の
        // ライト一覧。KurenaiEngine3Dはこれをそのまま読んでGPUのライトバッファを構築する
        std::vector<Light> Lights;

        // エミッシブなメッシュから起こした光源プロキシの一覧(ワールド空間)。
        // 各ModelInstanceが持つMesh::EmissiveClusters(ローカル空間)をInstance::Worldで
        // 変換したもの。**Lightsには混ぜない** ―― 作者が置いたライトと自動生成の光源を
        // 同じ配列にすると、ImGuiのライト一覧から消せてしまい元のメッシュと食い違う。
        // 上限を超えたときに手置きのライトが押し出されないよう、詰める順序も分ける必要がある
        std::vector<EmissiveProxy> EmissiveProxies;

        // .ksceneの[ReflectionProbe]セクションで配置された反射プローブの一覧(ワールド空間)。
        // ライトと違いモデルファイルへ埋め込む概念が無いため、.ksceneに書かれたものが全て
        std::vector<ReflectionProbe> ReflectionProbes;

        // .ksceneの[GIVolume]セクションで配置されたDDGIボリュームの一覧(ワールド空間)。
        // 現状KurenaiEngine3Dが使うのは先頭の1つだけで、2つ目以降は警告を出して切り捨てる
        // (複数ボリュームの重なりや優先順位を決める仕組みがまだ無いため)。
        // ここをvectorにしてあるのは、対応した時点で読み込み側を変えずに済ませるため
        std::vector<GIVolume> GIVolumes;

        // [Camera]セクションが無い場合はfalseのままで、呼び出し側は
        // KurenaiEngine3D::ComputeInitialCamera相当の自動配置ヒューリスティックを使う
        bool HasCameraOverride = false;
        float CameraPosition[3] = { 0.0f, 0.0f, 0.0f };
        float CameraYaw = 0.0f;
        float CameraPitch = 0.0f;

        // [Sun]セクションが無い場合は既定値(従来のKurenaiEngine3Dの初期値と同じ)のまま
        float SunTimeOfDay = 12.0f;
        float SunAzimuthDegrees = 126.87f;
        bool ShadowEnabled = true;
        // 太陽(平行光)そのものを無効にするか。TimeOfDayを夜にすると昼度(AmbientColor.a)も
        // 一緒に0になり環境光ごと消えてしまうため、「昼のまま太陽だけ消す」にはこれが必要になる。
        // White Furnace Test(一様な環境光だけで照らし、エネルギー保存を検証するシーン)のように
        // 環境光以外の寄与を完全に排除したい場合に使う
        bool SunEnabled = true;

        // スカイボックス(キューブマップDDS)のAssetsルートからの相対パス。空なら既定の
        // Assets/Skybox/Sky.ddsを使う。IBLの拡散イラディアンス・プリフィルタ済み鏡面は
        // このスカイボックスから焼かれるため、差し替えるとシーンの環境光そのものが変わる
        std::wstring SkyboxPath;

        // IBLの強度倍率。指定が無ければ呼び出し側の現在値を維持する
        // (White Furnace Testは背景のスカイボックスと球の明るさが厳密に一致する必要があるため
        //  1.0でなければならない。背景は強度倍率を掛けずにそのまま描かれるので、
        //  既定の0.5のままだと球だけが半分の明るさになり検証が成立しない)
        bool HasIBLIntensityOverride = false;
        float IBLIntensity = 1.0f;

        // カスケードシャドウを打ち切る距離[m]。未指定(HasShadowDistance == false)なら
        // 従来どおりカメラの遠クリップ面までを4カスケードで分割する。
        //
        // 【なぜ必要か】遠クリップ面はシーンAABBの対角から自動決定される
        // (farZ = max(100, 対角×4)、KurenaiEngine3D::ComputeInitialCamera)。数十km規模のシーンでは
        // farZが100km級になり、カスケードの分割範囲がそのまま伸びるため、第1カスケードが
        // 数kmを2048x2048の1枚で覆うことになって近景の影が事実上消える。
        // シャドウだけを手前で打ち切れば、遠景の描画距離を保ったまま近景の影の密度を戻せる。
        //
        // 【既定値を持たせない理由】「指定しなければ従来の挙動」を保証するためにフラグで分ける。
        // 何らかの既定値を入れると、これまで正しく影が出ていたシーンの見え方が黙って変わる
        bool HasShadowDistance = false;
        float ShadowDistance = 0.0f;

        // モデルのストリーミングを行う距離[m]。未指定(HasStreamingDistance == false)なら
        // **従来どおり全モデルを読み込み時に常駐させる**。
        //
        // 指定するとLoadSceneは.kmodelの実体を読まず、ヘッダのAABBだけでインスタンスを配置する。
        // 実体はカメラがこの距離まで近づいたときにLoaderスレッドが読み込む。
        //
        // 【この距離はAABBの最近接点まで】モデルLODの切り替え距離(ModelInstance::LODDistances)と
        // 同じ測り方。中心距離だと巨大なタイルで足元のものが未読み込みのまま残る
        bool HasStreamingDistance = false;
        float StreamingDistance = 0.0f;

        // WASD/E/Qでカメラを動かす速度[m/s]。未指定(HasCameraSpeed == false)なら
        // シーンAABBの対角から自動で決める(KurenaiEngine3D::ResetSceneDependentParams)。
        //
        // 【なぜ必要か】従来は moveSpeed = Shift ? 20 : 5 の即値だった。市街地規模のシーン
        // (Project PLATEAU 東京23区は対角約45km)ではこの速度で端から端まで38分かかり、
        // シーンを見て回ること自体ができない。逆に速度を上げただけにするとSponza(対角37m)のような
        // 小さいシーンが操作不能になるため、シーンの規模から決めたうえで個別に上書きできる形にする。
        //
        // 【既定値を持たせない理由】ShadowDistanceと同じ。何らかの既定値を入れると、
        // これまで問題なく操作できていたシーンの挙動が黙って変わる
        bool HasCameraSpeed = false;
        float CameraSpeed = 0.0f;

        // テクスチャの常駐ミップ制御(テクスチャストリーミング)を有効にするか。
        //
        // 【既定はoff】未指定なら従来どおり全ミップを常駐させる。既存アセットの見え方も
        // VRAM使用量も1ビットも変えないため。PLATEAU LOD2のようにテクスチャが支配的な
        // シーンだけがonにする。仕組みはAssets::TextureStreamingManager参照
        bool TextureStreamingEnabled = false;
        // 必要ミップの推定に足すバイアス[段]。負なら安全側(より詳細なミップを常駐させる)。
        //
        // 【既定 -2 は実測で決めた】Bistro Exteriorで、常駐ミップ制御をoff/onした画を
        // 同一起動・同一カメラで撮り、32pxタイルごとにラプラシアン分散を比べた結果:
        //
        //   UV密度の代表値  バイアス  常駐率   比<0.80のタイル   最悪タイル
        //   (ノイズ下限)       ―        ―          0枚           0.918
        //   p90              -2      13.8%       14枚           0.175
        //   中央値            -3      28.6%        0枚           0.852
        //   p10              -1      12.4%        3枚           0.745
        //   p10              -2      21.1%        1枚           0.768  ← 既定
        //
        // 残る1枚は暗部のアルファテスト葉で、並べても区別が付かない(絶対値が小さいため
        // 相対指標だけが大きく動く)。根拠と経緯は docs/ImplementationDetail.md 48章
        float TextureStreamingBias = -2.0f;

        // AO/間接光(SSAO・SSIL)を有効にするか。Furnace Testでは球の縁がAOで暗くなると
        // 「エネルギー損失による暗さ」と区別がつかなくなるため無効にする
        bool AOEnabled = true;

        // 鏡面反射を有効にするか(キー名はSSRしか無かった頃の名残)。手法(SSR / レイトレーシング)は
        // シーンでは選べず、エンジンが環境から決める。
        //
        // 【HasSSREnabledOverrideが要る理由】このキーを書いていないシーンと
        // 「= true」と書いたシーンは区別しなければならない。エンジンの既定は
        // 「レイトレーシングが使えない環境では反射なし」(EngineDefaults.h の SSREnabled。
        // SSRは画面端で反射が途切れる破綻が目立つため)で、書いていないシーンはそれに従う。
        // 一方このキーを明示したシーンは、その既定を上書きする意思表示なので指定どおりにする。
        // 【この区別を落としてはいけない】どちらも同じ扱いにして「= true」でもエンジンの既定へ
        // 問い合わせ直すと、DX11では ReflectionMode::Off になりシーンの指定が握り潰される
        // (水面に何も映らない/White Furnace TestのSSR回帰テストが動かない、という形で出る)。
        // DX12はDXRが使えてレイトレーシング反射が選ばれるため露見しない
        bool HasSSREnabledOverride = false;
        bool SSREnabled = true;

        // TAA(時間的アンチエイリアス)を有効にするか。SSREnabledと同じく「書いたこと」に
        // 意味があるので Has〜Override で区別する(エンジンの既定は EngineDefaults.h の
        // TAAEnabled = false。書いていないシーンはそれに従う)
        bool HasTAAOverride = false;
        bool TAAEnabled = false;

        // G-Buffer以降の内部レンダー解像度。ウィンドウの大きさとは独立で、表示時に
        // アスペクト比を保って拡大縮小される(KurenaiEngine3D::RequestRenderResolution)。
        //
        // 【なぜシーンが持つのか】参考写真と実機を数値で突き合わせる検証では、実効解像度が
        // 測定値そのものを動かす。既定の1280x720では島の幅が約516画素で、参考写真の946画素の
        // 半分しか無く、局所コントラストの比較が「拡大のぼけ」を測ってしまう。どの解像度で
        // 測ったかはシーンの一部として残さないと再現できない
        bool HasRenderResolutionOverride = false;
        uint32_t RenderWidth = 0;
        uint32_t RenderHeight = 0;

        // トーンマップのカーブ。Source/LibraryはSource/Engineに依存できないため、
        // KurenaiEngine3D::TonemapCurveと同じ並びの独立した列挙をここに持つ
        // (KurenaiEngine3D::ApplyLoadedSceneが1対1で対応付ける。並びを変えたら両方直すこと)。
        // 既定のAgXはハイライトが色相を保ったまま白へ脱色するので赤い内観に強い一方、
        // 空のような広い面では彩度を落とす(実測: 空の最も青い画素でB/R 1.53→1.34)。
        // 屋外の風景ではACESのほうが空の青が残るため、シーン単位で選べるようにしてある
        enum class TonemapCurveSetting
        {
            Reinhard,
            ACES,
            AgX,
        };
        TonemapCurveSetting Tonemap = TonemapCurveSetting::AgX;

        // 黒の締め(ブラックポイント)。トーンマップ後の表示リニア値からこの値を引き、
        // 残りを[0,1]へ伸ばし直す。実カメラのトーンカーブが持つ処理に相当する。
        // 0(既定)で恒等なので、指定しないシーンの見た目は一切変わらない。
        // 屋外の遠景では大気遠近が最暗部へ空の輝度を一定量だけ加算するため、画面上の
        // 最暗値が頭打ちになる。霞を非物理的な値まで薄めずに黒を締めるための逃げ道
        float TonemapBlackPoint = 0.0f;

        // 空の彩度(アート指定)。既定1.0は物理モデルの色度そのまま。色度図上で白色点から
        // 遠ざける倍率で、色相は変えずに鮮やかさだけを変える。物理量ではないので
        // 「写真に寄せたい」シーンだけが明示的に上げる
        float SkySaturation = 1.0f;

        // 大気の濁り具合(Preethamのタービディティ)。大きいほど地平線が白く霞み、天頂の青が薄くなる。
        // **指定されたときだけ**エンジンの設定を上書きする(SkySaturationのような無条件の反映に
        // しないのは、動かすと大気LUTの焼き直しが走るため)。定義域はおおむね1.7〜10
        bool HasSkyTurbidity = false;
        float SkyTurbidity = 2.5f;   // EngineDefaults.h の SkyTurbidity と同じ値にすること

        // シーン全体の露出(EV100)。**指定されたときだけ**エンジンの設定を上書きする
        // (IBLIntensityと同じ扱い)。Tonemap/SkySaturationのような無条件の反映にしないのは、
        // 露出はUIでも頻繁に触る値で、Exposureを持たないシーンを読み直すたびに
        // ユーザーの調整を既定値へ戻してしまうため。
        //
        // 【なぜシーンが持つのか】屋外の風景と屋内では被写体の輝度が桁で違う。エンジンの
        // 既定値(EngineDefaults.h の SceneExposureEV100 = 15)は屋内基準で決まっており、
        // 物理的に正しい空(地平線際が天頂の8倍明るい)を入れると屋外ではトーンカーブの肩に
        // 乗って彩度が落ちる。既定値を動かすと他のシーンを巻き込むので、シーン側に持たせる
        bool HasExposureOverride = false;
        float ExposureEV100 = 15.0f;   // EngineDefaults.h の SceneExposureEV100 と同じ値にすること

        // --- [Cloud]セクション。天候はシーンが持つべき性質なので、[Water]と同じく
        // シーンごとに指定できるようにする。**指定されたキーだけ**エンジンの設定を上書きする
        // (Exposure/IBLIntensityと同じ扱い)ので、書かなかったキーはエンジンの既定値のまま。
        // 既定値はEngineDefaults.hの複製で、両方を同時に直すこと ---
        bool HasCloudCoverage = false;
        float CloudCoverage = 0.40f;
        bool HasCloudAltitude = false;
        float CloudAltitude = 1500.0f;      // 雲底の高度[m]
        bool HasCloudThickness = false;
        float CloudThickness = 400.0f;      // 雲底から雲頂までの厚み[m]
        bool HasCloudDensity = false;
        float CloudDensity = 8.0f;          // 光学的な濃さ。上げるほど不透明で白い塊になる
        bool HasCloudTypeBias = false;
        float CloudTypeBias = 0.5f;         // 雲の種類の偏り(C4)。0=層雲寄り / 0.5=中立 / 1=雄大積雲寄り
        bool HasCloudCellSize = false;
        float CloudCellSize = 1000.0f;      // 雲の塊1つぶんのワールド上の大きさ[m]。
                                             // エンジン側はこの逆数(UvScale)を持つ
        // 高層の巻雲(P11)。積雲と同じく**指定されたキーだけ**上書きする。
        // 【被覆率以外も持たせた理由】巻雲は「白い筋」ではなく灰色の薄膜として出ており、
        // 明るさが未較正のまま出荷設定に0.5で入っていた。詰めるには濃さ・高度・筋の強さを
        // シーンから振れる必要がある(シェーダ定数と違いホットリロードで振れる)
        bool HasCirrusCoverage = false;
        float CirrusCoverage = 0.5f;        // 0で消える(このとき巻雲の計算経路自体を通らない)
        bool HasCirrusAltitude = false;
        float CirrusAltitude = 8000.0f;     // 雲底の高度[m]
        bool HasCirrusCellSize = false;
        float CirrusCellSize = 2000.0f;     // 筋1本ぶんのワールド上の大きさ[m]。
                                             // 積雲のCellSizeと同じくエンジン側は逆数を持つ
        bool HasCirrusDensity = false;
        float CirrusDensity = 2.0f;         // 消散係数。巻雲は光学的に薄いので積雲より1桁小さい
        bool HasCirrusAnisotropy = false;
        float CirrusAnisotropy = 3.0f;      // fBmのUVをU方向へ伸ばす倍率。1で積雲と同じ等方な塊
        bool HasCirrusWindSpeed = false;
        float CirrusWindSpeed = 15.0f;      // 風速[m/s]。風向きは積雲と共有する

        // --- [Fog]セクション。大気の澄み具合はシーンが持つべき性質なので、[Cloud]と同じく
        // 指定されたキーだけエンジンの設定を上書きする。
        //
        // 【なぜシーンごとに要るのか】消散係数は遠景の霞の濃さだけでなく、**雲がどれだけ空から
        // 浮き上がって見えるか**を一手に決める。雲底1,500mの層は仰角20度の方向で4.4km先にあり、
        // エンジンの既定値0.0004(視程9.8km相当)ではそこまでの透過率が0.40しかない——
        // 雲のコントラストの6割が目に届く前に空の色へ溶ける。実測でも、雲の受光を削っている
        // 要因はこれがほぼ単独で、自己影(+1.2)や縦方向の勾配(+3.6)に対してこの項だけが
        // +24.6(雲の90%点と空の中央値の差、255段階)を占めていた。
        // 既定値はEngineDefaults.hの複製で、両方を同時に直すこと ---
        bool HasFogEnabled = false;
        bool FogEnabled = true;
        bool HasFogDensity = false;
        // 基準高度での消散係数[1/m]。気象学的視程Vとは Koschmieder の V = 3.912 / 消散係数
        // で結び付く(0.0004 で V ≒ 9.8km、0.0002 で V ≒ 19.6km)
        float FogDensity = 0.0004f;
        bool HasFogScaleHeight = false;
        float FogScaleHeight = 1000.0f;     // 霞の層の厚み[m]。大きいほど高い高度まで及ぶ
        bool HasFogRefHeight = false;
        float FogRefHeight = 0.0f;          // 消散係数を定義する高さ(ワールドY)

        // --- [Bloom]セクション。ブルームはエンジンの既定では**無効**(素の輝度分布を確認したい
        // ときに邪魔になるため)だが、夜景で強い光源が主役になるシーンでは有効でないと成立しない。
        // ドローンショーの機体は加算合成でHDRへ書くだけで光芒を作らず、滲みはブルームに任せている。
        // 他のセクションと同じく指定されたキーだけ上書きする。
        // 既定値はEngineDefaults.hの複製で、両方を同時に直すこと ---
        bool HasBloomEnabled = false;
        bool BloomEnabled = false;
        bool HasBloomStrength = false;
        float BloomStrength = 0.06f;
        bool HasBloomThreshold = false;
        float BloomThreshold = 1.0f;

        // --- [Stars]セクション。星空も天候と同じくシーンが持つべき性質なので、[Cloud]/[Fog]と
        // 同じく**指定されたキーだけ**エンジンの設定を上書きする。
        // 既定値はEngineDefaults.hの複製で、両方を同時に直すこと ---
        bool HasStarsEnabled = false;
        bool StarsEnabled = true;
        bool HasStarsDensity = false;
        float StarsDensity = 48.0f;         // 空を分割するセルの細かさ。大きいほど星が増える
        bool HasStarsBrightness = false;
        float StarsBrightness = 1.0f;
        bool HasStarsTwinkle = false;
        float StarsTwinkle = 0.0f;          // またたきの強さ。既定0(TAAと相性が悪いため)

        // --- [DroneShow]セクション。夜空を編隊飛行する発光ドローンの群れ。
        // 他のセクションと同じく指定されたキーだけエンジンの設定を上書きする。
        // 既定値はEngineDefaults.hの複製で、両方を同時に直すこと ---
        //
        // 【シーンが決めるのは「出すか」と「どこにどの大きさで置くか」だけ】機体数・保持/変形秒・
        // 明るさ・ビルボード半径・揺れ・再生速度・種はショー側(.kshow)が持つ。
        // ここへ足してはいけない ―― ショーの中身をシーンが上書きできると、同じショーが
        // シーンごとに別物として鳴り、どちらが「そのショー」なのかが決まらなくなる
        bool HasDroneShowEnabled = false;
        bool DroneShowEnabled = false;
        // [DroneShow]Pathで指定された.kshowを読み込んだもの。Formationsが空なら
        // 「指定なし」または「読み込み失敗」で、どちらもドローンは描かれない
        // (読み込み失敗はLoadScene側でエラーログに出る)
        ShowData DroneShowData;
        bool HasDroneShowCenter = false;
        float DroneShowCenter[3] = { 0.0f, 220.0f, 260.0f };  // 編隊の中心(ワールド座標)
        bool HasDroneShowScale = false;
        float DroneShowScale = 130.0f;      // 編隊の代表半径[m]
        // 機体を光源としても送るか。灯の明るさはショー(.kshow)のBrightnessとRadiusから
        // 導かれるので、ここには明るさのつまみを置かない(置くと「同じショーがシーンごとに
        // 別の明るさで照らす」ことになり、上の分担が崩れる)
        bool HasDroneShowCastLight = false;
        bool DroneShowCastLight = false;
        // 灯の明るさの倍率。1.0がスプライトから導いた物理的な値。
        // 【これはショーの中身ではない】倍率が変えるのは「この舞台でどれだけ照らして見せるか」で、
        // ショーそのもの(機体の見た目の明るさ)は.kshowのBrightnessが持ったままである。
        // 同じショーを別のシーンへ置いたときに、見た目は同じで照らし方だけ変わってよい
        bool HasDroneShowCastLightScale = false;
        float DroneShowCastLightScale = 1.0f;

        // 各ModelInstanceのAABB(Modelのローカル空間Bounds)をWorldで変換し合成した、
        // シーン全体のワールド空間AABB。ComputeInitialCamera/ComputeLightViewProjが使う
        float BoundsMin[3] = { 0.0f, 0.0f, 0.0f };
        float BoundsMax[3] = { 0.0f, 0.0f, 0.0f };

        // .ksceneの[Water]セクション(水面マテリアル基盤)。水面(ModelInstance::IsWater)専用の
        // シェーディングパラメータで、[Water]が無いシーンでは既定値のまま(水面インスタンス自体も
        // 存在しないため未使用)。NormalMapはAssetsルートからの相対パスで、[Scene]Skyboxと同じ
        // ルート外チェックを通したうえで絶対パスへ解決してからここへ入る。空文字列なら
        // 「法線マップ無しのフラット水面」を意味し、エラーではない(C++側は1x1のフラット法線
        // テクスチャへフォールバックする)
        std::wstring WaterNormalMapPath;
        // 波の見た目に関する3つの既定値。SunTimeOfDay等と同じ方針で、EngineDefaults.h
        // ([--- 水面 ---]セクション)の値をリテラルとして複製している(Source/Libraryは
        // Source/Engineに依存できないため、Defaults::を直接参照できない)。
        // シーン読み込み時にKurenaiEngine3D::m_WaterWaveScale等へコピーされ、以降はUIで
        // 実行時上書きできる(m_ReflectionModeがScene.SSREnabledから初期化されるのと同じ設計)
        float WaterWaveScale = 12.0f;
        float WaterWaveSpeed = 0.03f;
        float WaterWaveStrength = 0.25f;
    };
}
