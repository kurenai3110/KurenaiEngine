#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "KurenaiTypes.h"

#include "RHI/IRHIAccelerationStructure.h"
#include "RHI/IRHIBuffer.h"
#include "RHI/IRHIDevice.h"

#include "Scene.h"

#pragma warning(push)
#pragma warning(disable: 4251)

namespace Kurenai::Assets
{
    // レイトレーシングのシェーダーが「どの三角形に当たったか」から陰影計算に必要な情報へ
    // たどり着くための索引。
    //
    // RayQueryがヒット時に返すのはInstanceID / GeometryIndex / PrimitiveIndex / 重心座標だけで、
    // そこから法線やマテリアルを引くにはシーン全体のジオメトリをシェーダーから読める形に
    // しておく必要がある。このエンジンではメッシュごとに個別の頂点/インデックスバッファを
    // 持っており、HLSLはリソースを動的に添字選択できない(bindlessが要る)ため、
    // シーン全体を1本ずつの構造化バッファへ連結し、下のオフセット表で引く方式を採る。
    //
    // 引き方は次の通り:
    //   グローバルメッシュ番号 = InstanceInfo[InstanceID].MeshInfoOffset + GeometryIndex
    //   MeshInfo               = MeshInfoBuffer[グローバルメッシュ番号]
    //   三角形のi番目の頂点     = IndexBuffer[MeshInfo.IndexOffset + PrimitiveIndex * 3 + i]
    //   その頂点属性            = AttributeBuffer[MeshInfo.AttributeOffset + 上のインデックス]
    //   マテリアル              = MaterialBuffer[MeshInfo.MaterialIndex]
    //   所属メッシュレット       = MeshletTriangleOffsetBufferをPrimitiveIndexで二分探索
    //                             (Shaders/3D/RaytracingScene.hlsliのRTFindMeshlet)
    //
    // 【守るべき不変条件: ジオメトリの正は.kgeomただ1つ】ここに載る頂点属性・インデックスは
    // すべてModelLoaderが.kgeomから読んだものをそのまま連結したもので、ラスタライズ側の
    // 頂点/インデックスバッファとまったく同じ並びである。メッシュレットも同じ.kgeomに
    // 焼かれた分割情報でしかない。
    // つまりラスタライズとレイトレーシングは「1つのジオメトリを2通りに読んでいる」だけで、
    // 片方だけを並べ替える・間引くといった加工を挟んではならない。挟んだ瞬間、
    // 反射に映る形と直接見える形が食い違い、しかもどちらも一見もっともらしいまま壊れる。
    // メッシュレット単位のLODを入れる場合も、BLASを焼き直して両者を揃えること

    // HLSL側の対応: struct RTMeshInfo { uint AttributeOffset; uint IndexOffset; uint MaterialIndex;
    //                                   uint MeshletOffset; uint MeshletCount; uint Padding; };
    struct RaytracingMeshInfo
    {
        uint32_t AttributeOffset = 0;
        uint32_t IndexOffset = 0;
        uint32_t MaterialIndex = 0;
        // このメッシュのメッシュレットが、シーン全体を連結したメッシュレット表
        // (RaytracingScene::GetMeshletTriangleOffsetBuffer)の何番目から始まるか。
        //
        // 【何に使うのか】.kgeom v3からインデックスバッファの三角形はメッシュレット順に
        // 並んでいるため、RayQueryが返すPrimitiveIndex(=メッシュ内の三角形番号)から
        // 所属メッシュレットを二分探索だけで引ける。ラスタ側とRT側が同じ塊分けを
        // 見ていることの確認(デバッグ表示)と、将来のクラスタLODの足場になる。
        // メッシュレットを持たない.kmodelではMeshletCountが0になり、引けない
        uint32_t MeshletOffset = 0;
        uint32_t MeshletCount = 0;
        uint32_t Padding = 0;
    };
    static_assert(sizeof(RaytracingMeshInfo) == 24, "HLSL側のRTMeshInfoと一致させるため24バイト固定");

    // HLSL側の対応: struct RTInstanceInfo { float4x4 NormalMatrix; uint MeshInfoOffset; uint3 Padding; };
    struct RaytracingInstanceInfo
    {
        // モデルのローカル空間の法線をワールド空間へ変換する行列(ModelInstance::NormalMatrixと
        // 同じく転置済みでHLSLへそのまま渡せる形)。BLASの頂点はモデルのローカル空間のまま
        // 登録してあり、ワールドへの配置はTLASのインスタンス変換が行うため、
        // AttributeBufferから読んだ法線もこの行列でワールドへ移す必要がある
        float NormalMatrix[16] = {};
        // このインスタンスの0番目のメッシュが、MeshInfoBufferの何番目から始まるか
        uint32_t MeshInfoOffset = 0;
        uint32_t Padding[3] = {};
    };
    static_assert(sizeof(RaytracingInstanceInfo) == 80, "HLSL側のRTInstanceInfoと一致させるため80バイト固定");

    // HLSL側の対応:
    //   struct RTMaterial { float4 BaseColorFactor; float3 EmissiveFactor; float MetallicFactor;
    //                       float RoughnessFactor; float AlphaCutoff; uint Flags; uint Padding; };
    //
    // 【テクスチャはbindless番号で持つ】かつてこの構造体は定数の係数しか持たず、
    // 反射・GIに映る色がマテリアルの定数値のみで決まってやや平板だった。
    // HLSLはリソースを実行時の番号で選べないため、ヒット面のテクスチャを引く手段が
    // 無かったのがその理由(bindless = SM 6.6のResourceDescriptorHeapが要る)。
    // 下の4つはIRHIDevice::RegisterBindlessが払い出した番号で、シェーダーは
    // Shaders/3D/Bindless.hlsliのBindlessSampleLevelでこれを引く。
    //
    // 【bindless非対応環境ではkInvalidBindlessIndexが入る】RegisterBindlessは非対応なら
    // 必ず無効値を返し、シェーダー側は無効値を「テクスチャ無し」として白1x1/フラット法線の
    // プレースホルダーへ落とす。そのため従来とまったく同じ見た目へ静かに縮退する
    struct RaytracingMaterial
    {
        float BaseColorFactor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        float EmissiveFactor[3] = { 0.0f, 0.0f, 0.0f };
        float MetallicFactor = 0.0f;
        float RoughnessFactor = 1.0f;
        // 0以下ならアルファカットアウト無効(Assets::Mesh::AlphaCutoffと同じ意味)
        float AlphaCutoff = 0.0f;
        // bit0: 半透明(glTFのalphaMode=BLEND)
        uint32_t Flags = 0;
        // 以下はbindlessディスクリプタ番号。既定はRHI::kInvalidBindlessIndex(=テクスチャ無し)
        uint32_t BaseColorTextureIndex = RHI::kInvalidBindlessIndex;
        uint32_t NormalTextureIndex = RHI::kInvalidBindlessIndex;
        uint32_t MetallicRoughnessTextureIndex = RHI::kInvalidBindlessIndex;
        uint32_t EmissiveTextureIndex = RHI::kInvalidBindlessIndex;
        uint32_t Padding = 0;
    };
    static_assert(sizeof(RaytracingMaterial) == 64, "HLSL側のRTMaterialと一致させるため64バイト固定");

    // RaytracingMaterial::Flagsのビット定義
    inline constexpr uint32_t kRaytracingMaterialFlagTransparent = 1u << 0;

    // シーン1つ分のレイトレーシング資源(BLAS/TLASと、シェーダーから引くための統合バッファ群)。
    // KurenaiEngine3D::LoadSceneがシーンを読み終えた直後に一度だけBuildし、
    // 次のシーンへ切り替えるときにReset(またはBuildの再呼び出し)で丸ごと作り直す。
    //
    // シーンは読み込み後に変形しない前提のため、TLASも構築し直さない(BLAS/TLAS双方を
    // PREFER_FAST_TRACEで焼き、更新は行わない)
    class KURENAI_LIB_API RaytracingScene
    {
    public:
        RaytracingScene() = default;
        ~RaytracingScene() = default;

        RaytracingScene(const RaytracingScene&) = delete;
        RaytracingScene& operator=(const RaytracingScene&) = delete;
        RaytracingScene(RaytracingScene&&) = default;
        RaytracingScene& operator=(RaytracingScene&&) = default;

        // シーンからBLAS/TLASと統合バッファを構築する。
        // 成功したらtrue、デバイスが非対応・シーンが空・構築失敗ならログを出してfalseを返し、
        // その場合このオブジェクトは何も保持しない状態(IsValid()==false)になる。
        //
        // sceneが非constなのは、構築後に不要となる各ModelのCPU側レイトレーシング配列
        // (Model::RaytracingAttributes / RaytracingIndices)を解放するため。
        // 数百万頂点のシーンでは100MB規模になるため、GPUへ送った後は抱え続けない
        bool Build(RHI::IRHIDevice& device, Scene& scene);

        // 保持しているすべてのGPUリソースを解放する。
        // 【重要】呼ぶ前にIRHIDevice::WaitForGPUIdle()でGPUの実行完了を待つこと
        // (まだGPUが参照しているASを解放するとヒープ破損になる)
        void Reset();

        bool IsValid() const { return m_TopLevelAS != nullptr; }

        // シェーダーへバインドするためのアクセサ。IsValid()がfalseの場合はすべてnullptrを返す
        RHI::IRHIAccelerationStructure* GetTopLevelAS() const { return m_TopLevelAS.get(); }
        RHI::IRHIBuffer* GetVertexAttributeBuffer() const { return m_VertexAttributeBuffer.get(); }
        RHI::IRHIBuffer* GetIndexBuffer() const { return m_IndexBuffer.get(); }
        RHI::IRHIBuffer* GetMeshInfoBuffer() const { return m_MeshInfoBuffer.get(); }
        RHI::IRHIBuffer* GetInstanceInfoBuffer() const { return m_InstanceInfoBuffer.get(); }
        RHI::IRHIBuffer* GetMaterialBuffer() const { return m_MaterialBuffer.get(); }
        // メッシュレットごとの「メッシュ内での開始三角形番号」を、シーン全体で連結した表。
        // RTMeshInfo::MeshletOffset/MeshletCountが各メッシュの範囲を指す。
        //
        // 【MeshletEntryをそのまま送らない理由】RT側が必要とするのは三角形番号から
        // メッシュレットを引くための開始位置だけで、バウンディング球や法線コーンは使わない
        // (カリングはTLAS/BLASの仕事)。48バイトのMeshletEntryをそのまま送ると
        // Bistro級(約5万メッシュレット)で2.4MBになるところ、uint1本なら200KBで済む。
        // クラスタLODのようにRT側でも境界が要るようになったら、そのとき足せばよい
        RHI::IRHIBuffer* GetMeshletTriangleOffsetBuffer() const { return m_MeshletTriangleOffsetBuffer.get(); }

        // 統計(ImGuiの表示・ログ用)
        uint32_t GetInstanceCount() const { return m_InstanceCount; }
        uint32_t GetMeshCount() const { return m_MeshCount; }
        uint32_t GetTriangleCount() const { return m_TriangleCount; }
        // 統合バッファ(頂点属性+インデックス+索引)が占めるGPUメモリの合計バイト数。
        // BLAS/TLAS本体のサイズはドライバが決めるため含まない
        uint64_t GetGeometryBufferBytes() const { return m_GeometryBufferBytes; }

    private:
        // モデルインスタンスごとに1つ。各BLASは自分のインスタンスの全メッシュを
        // ジオメトリとしてまとめて持つ(GeometryIndexがメッシュ番号に対応する)。
        // TLASより長く生存させる必要があるため、TLASと同じこのオブジェクトで保持する
        std::vector<std::unique_ptr<RHI::IRHIAccelerationStructure>> m_BottomLevelAS;
        std::unique_ptr<RHI::IRHIAccelerationStructure> m_TopLevelAS;

        std::unique_ptr<RHI::IRHIBuffer> m_VertexAttributeBuffer;
        std::unique_ptr<RHI::IRHIBuffer> m_IndexBuffer;
        std::unique_ptr<RHI::IRHIBuffer> m_MeshInfoBuffer;
        std::unique_ptr<RHI::IRHIBuffer> m_InstanceInfoBuffer;
        std::unique_ptr<RHI::IRHIBuffer> m_MaterialBuffer;
        // メッシュレットを持つメッシュが1つも無いシーンではnullptrのまま
        // (要素数0の構造化バッファはD3D11/D3D12とも作れないため)。
        // シェーダー側はRTMeshInfo::MeshletCountが0かどうかで判断するので、
        // バインドされていなくても破綻しない
        std::unique_ptr<RHI::IRHIBuffer> m_MeshletTriangleOffsetBuffer;

        uint32_t m_InstanceCount = 0;
        uint32_t m_MeshCount = 0;
        uint32_t m_TriangleCount = 0;
        uint64_t m_GeometryBufferBytes = 0;
    };
}

#pragma warning(pop)
