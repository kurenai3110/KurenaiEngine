#pragma once

#include "../KurenaiEngine3D.h"

// メッシュ1つ分の描画に渡す ObjectConstants と、その組み立て(段階6)。
//
// 【なぜ共有ヘッダーなのか】ジオメトリを描くパスはシャドウ / 反射プローブ / DDGI /
// 深度プリパス / G-Buffer / 半透明 / 平面反射 と広く散っており、そのすべてが
// これを組み立てる。KurenaiEngine3D.cpp の無名名前空間に置いたままでは
// パス群から見えない。
//
// 【1フレームに数千回通る】メッシュごとに呼ばれるため、翻訳単位をまたぐ呼び出しに
// ならないよう inline にしてヘッダーへ置いている(Rendering/GeometryDrawLoop.h が
// テンプレートである理由と同じ)。
//
// 【本来の置き場所】Shaders/3D/ObjectConstants.hlsli と対になる写しなので、
// 段階4で作った ShaderInterop/ の規約に沿う。そちらへ寄せるのは公開面の整理と一緒に行う。
namespace Kurenai
{
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
        // 【HLSL側の宣言とレイアウトを揃えたまま保つための固定】cbuffer(と構造化バッファ)は
        // 宣言順でオフセットが決まるので、ここで並べ替え・挿入・型変更が起きると、
        // HLSL側を直さないかぎり黙って別の値を読むことになる。
        // **通すために期待値を書き換えないこと**(FrameConstants.h と同じ規約)。
        //
        // 【これが守るのはC++側だけ】HLSLの宣言と突き合わせているわけではない。
        // ここが落ちたら「HLSL側も同じだけ動かせ」という合図として使う
        static_assert(offsetof(ObjectConstants, World) == 0, "World のレイアウトが変わっている");
        static_assert(offsetof(ObjectConstants, NormalMatrix) == 64, "NormalMatrix のレイアウトが変わっている");
        static_assert(offsetof(ObjectConstants, MetallicFactor) == 128, "MetallicFactor のレイアウトが変わっている");
        static_assert(offsetof(ObjectConstants, RoughnessFactor) == 132, "RoughnessFactor のレイアウトが変わっている");
        static_assert(offsetof(ObjectConstants, TangentSignFlip) == 136, "TangentSignFlip のレイアウトが変わっている");
        static_assert(offsetof(ObjectConstants, AlphaCutoff) == 140, "AlphaCutoff のレイアウトが変わっている");
        static_assert(offsetof(ObjectConstants, EmissiveFactor) == 144, "EmissiveFactor のレイアウトが変わっている");
        static_assert(offsetof(ObjectConstants, OcclusionStrength) == 156, "OcclusionStrength のレイアウトが変わっている");
        static_assert(offsetof(ObjectConstants, BaseColorFactor) == 160, "BaseColorFactor のレイアウトが変わっている");
        static_assert(offsetof(ObjectConstants, MaterialID) == 176, "MaterialID のレイアウトが変わっている");
        static_assert(offsetof(ObjectConstants, MeshletOffset) == 180, "MeshletOffset のレイアウトが変わっている");
        static_assert(offsetof(ObjectConstants, MeshletBufferIndex) == 184, "MeshletBufferIndex のレイアウトが変わっている");
        static_assert(offsetof(ObjectConstants, MeshletVertexBufferIndex) == 188, "MeshletVertexBufferIndex のレイアウトが変わっている");
        static_assert(offsetof(ObjectConstants, MeshletTriangleBufferIndex) == 192, "MeshletTriangleBufferIndex のレイアウトが変わっている");
        static_assert(offsetof(ObjectConstants, MeshletCount) == 196, "MeshletCount のレイアウトが変わっている");
        static_assert(offsetof(ObjectConstants, Translucency) == 200, "Translucency のレイアウトが変わっている");
        static_assert(offsetof(ObjectConstants, DitherFade) == 204, "DitherFade のレイアウトが変わっている");
        static_assert(offsetof(ObjectConstants, MaterialTableIndex) == 208, "MaterialTableIndex のレイアウトが変わっている");
        static_assert(offsetof(ObjectConstants, MeshletFilterReject) == 212, "MeshletFilterReject のレイアウトが変わっている");
        static_assert(offsetof(ObjectConstants, MeshletFilterRequire) == 216, "MeshletFilterRequire のレイアウトが変わっている");
        static_assert(offsetof(ObjectConstants, EmissiveIntensity) == 220, "EmissiveIntensity のレイアウトが変わっている");
        static_assert(offsetof(ObjectConstants, OcclusionMapScale) == 224, "OcclusionMapScale のレイアウトが変わっている");
        static_assert(offsetof(ObjectConstants, MeshletStatsEnabled) == 228, "MeshletStatsEnabled のレイアウトが変わっている");
        static_assert(offsetof(ObjectConstants, ModelBoundsCenter) == 232, "ModelBoundsCenter のレイアウトが変わっている");
        static_assert(offsetof(ObjectConstants, ModelBoundsRadius) == 244, "ModelBoundsRadius のレイアウトが変わっている");
        static_assert(offsetof(ObjectConstants, MeshletLODCameraPos) == 248, "MeshletLODCameraPos のレイアウトが変わっている");
        static_assert(offsetof(ObjectConstants, MeshletLODPixelScale) == 260, "MeshletLODPixelScale のレイアウトが変わっている");
        static_assert(offsetof(ObjectConstants, MeshletLODScreenSize) == 264, "MeshletLODScreenSize のレイアウトが変わっている");
        static_assert(offsetof(ObjectConstants, MeshletLODForced) == 268, "MeshletLODForced のレイアウトが変わっている");
        static_assert(offsetof(ObjectConstants, MeshletDebugColorByLOD) == 272, "MeshletDebugColorByLOD のレイアウトが変わっている");
        static_assert(offsetof(ObjectConstants, MeshletLODLevelCap) == 276, "MeshletLODLevelCap のレイアウトが変わっている");
        static_assert(offsetof(ObjectConstants, InstanceBase) == 280, "InstanceBase のレイアウトが変わっている");
        static_assert(offsetof(ObjectConstants, InstancingEnabled) == 284, "InstancingEnabled のレイアウトが変わっている");
        static_assert(offsetof(ObjectConstants, MeshletOcclusionMode) == 288, "MeshletOcclusionMode のレイアウトが変わっている");
        static_assert(sizeof(ObjectConstants) == 304, "ObjectConstants の総サイズが変わっている");

        // モデルのAABBから外接球を作り、段の選択に要る値を定数へ書き込む。
        //
        // 【AABBの外接球を使う】メッシュ単位ではなくモデル単位にするのは、
        // 1つのモデルの中で段を混ぜないため。段が混ざると、簡略化で頂点が動いた側と
        // 動いていない側で辺が一致せず、境目に穴が開く
        inline void ApplyMeshletLODConstants(
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
        // emissiveIntensity: シーン全体の自発光の強度倍率(m_EmissiveLightSettings.Intensity)。glTFの
        // emissiveFactorは通常1.0以下に収まるため、これを掛けないとG-Bufferのエミッシブを
        // HDR化しても照明器具の輝度が1.0を超えず、ブルームが効かない
        // occlusionMapEnabled: マテリアルの遮蔽マップを使うか(m_AmbientOcclusionSettings.OcclusionMapEnabled)。
        // 各パスは lerp(1, occlusionSample, OcclusionStrength) で遮蔽率を求めるため、
        // ここで0を渡せばシェーダー側に手を入れずに遮蔽マップの寄与だけを消せる
        // ditherFade: モデルLODの切り替え中だけ1.0以外を渡す(既定の1.0は「全画素を描く」)。
        // 呼び出し箇所7つのうち、2段を重ねるのはG-Bufferと深度プリパスだけなので既定値を持たせている。
        // シャドウ・プローブ・DDGIは常に最も粗い段を1つだけ描くためフェードそのものが起きない
        // 【モデルは引数で受け取る】meshが属する段のメッシュレット表を指す必要がある。
        // instance.Modelは最も詳細な段でしかなく、シャドウや粗い段を描くときは食い違う
        inline ObjectConstants MakeObjectConstants(
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
        inline ObjectConstants MakeModelObjectConstants(
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
}
