#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "RHIEnums.h"

namespace Kurenai::RHI
{
    struct BufferDesc
    {
        BufferUsage Usage = BufferUsage::Vertex;
        uint32_t SizeInBytes = 0;
        uint32_t StrideInBytes = 0;
        const void* InitialData = nullptr;
    };

    struct ShaderDesc
    {
        ShaderStage Stage = ShaderStage::Vertex;
        std::wstring FilePath;
        std::string EntryPoint;
    };

    struct InputElementDesc
    {
        std::string SemanticName;
        uint32_t SemanticIndex = 0;
        Format Format = Format::R32G32B32_Float;
        uint32_t AlignedByteOffset = 0;
    };

    class IRHIShader;
    class IRHIBuffer;
    class IRHIAccelerationStructure;

    // --- レイトレーシングの高速化構造(Acceleration Structure) --------------------------------

    // BLAS(Bottom Level AS)を構成する三角形ジオメトリ1つ分。Assets::Meshが持つ頂点/インデックス
    // バッファをそのまま渡せるようにしてある(RT専用に複製する必要はない)
    struct ASGeometryDesc
    {
        IRHIBuffer* VertexBuffer = nullptr;
        uint32_t VertexCount = 0;
        uint32_t VertexStrideInBytes = 0;
        // 頂点構造体の先頭から位置(float3)までのバイトオフセット。Assets::Vertexは位置が
        // 先頭にあるため0でよい
        uint32_t VertexPositionOffsetInBytes = 0;
        // インデックスは32bit(uint32_t)固定。このエンジンのインデックスバッファは
        // Assets::GeometryHeader::IndexStrideの通りすべてuint32_tのため
        IRHIBuffer* IndexBuffer = nullptr;
        uint32_t IndexCount = 0;
        // 不透明(アルファテスト・半透明を持たない)か。falseのジオメトリは、レイ側が
        // RAY_FLAG_CULL_NON_OPAQUEを指定した場合にトレース対象から外れる。
        // アルファカットアウトのマテリアルを正しく抜くにはAnyHit相当の処理が要るが、
        // インラインレイトレーシングではRayQuery::Proceed()のループで呼び出し側が判定する
        bool IsOpaque = true;
    };

    struct BottomLevelASDesc
    {
        // 1つのモデル(Assets::Model)に含まれる全メッシュをまとめて1つのBLASにする想定。
        // シェーダー側はRayQuery::CommittedGeometryIndex()でこの配列の何番目に当たったかを受け取る
        std::vector<ASGeometryDesc> Geometries;
    };

    // TLAS(Top Level AS)に登録する1インスタンス
    struct ASInstanceDesc
    {
        IRHIAccelerationStructure* BottomLevel = nullptr;
        // 行優先3x4のワールド変換(Transform[行][列]、平行移動はTransform[*][3])。
        // Assets::ModelInstance::WorldはHLSLへそのまま渡せるよう転置済みで保持されているため、
        // その先頭3行をそのままコピーすればこの並びになる
        float Transform[3][4] = { { 1.0f, 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f, 0.0f } };
        // シェーダーがRayQuery::CommittedInstanceID()で受け取る値。D3D12の仕様上24bitまで
        uint32_t InstanceID = 0;
    };

    struct TopLevelASDesc
    {
        std::vector<ASInstanceDesc> Instances;
    };

    struct PipelineStateDesc
    {
        std::vector<InputElementDesc> InputLayout;
        IRHIShader* VertexShader = nullptr;
        IRHIShader* PixelShader = nullptr;
        PrimitiveTopology Topology = PrimitiveTopology::TriangleList;

        // DX12のパイプラインステートオブジェクト作成時にレンダーターゲット/深度のフォーマットを
        // 事前に確定させる必要があるため保持する。DX11実装では参照しない
        std::vector<Format> RenderTargetFormats;
        // 深度テストを行うか(DX12ではこれがtrueのときDSVフォーマットも申告する)
        bool HasDepthStencil = false;

        // 深度テストはしないが、描画時に深度ステンシルビューがバインドされた状態になるか。
        // スワップチェインへ描くパス(Present / 2D / ImGui)が該当する。DX12は「PSOが申告した
        // DSVフォーマット」と「実際にバインドされているDSV」が一致していないと仕様違反になるため、
        // 深度テストを使わなくてもDSVが張られるならフォーマットの申告だけは必要になる
        // (HasDepthStencilで兼ねると深度テストまで有効になってしまうため別のフラグにしている)。
        // DX11はパイプラインステートと実際のDSVが独立しているため参照しない
        bool DepthTargetAttached = false;

        // 深度への書き込みを行うか。既定はtrue(通常の不透明描画)。半透明描画では、既存の不透明物体の
        // 深度に対してテストはしたいが(裏側の物体に隠れさせるため)、書き込みは行いたくない
        // (奥から手前に描く複数の半透明物体同士が互いの深度で隠し合わないようにするため)。
        // そのためHasDepthStencil=true(テスト有効)のまま、こちらだけfalseにする
        bool DepthWriteEnabled = true;

        // Reverse-Z(深度比較をGREATERにし、近平面=1.0/遠平面=0.0にマッピングする)を使うか。
        // 浮動小数点深度バッファと組み合わせて遠方のZファイティングを抑えるための設定で、
        // 透視投影のメインカメラパスにのみ使う(正射影のシャドウマップはZが線形分布のため対象外)
        bool ReverseZ = false;

        // アルファブレンド設定。既定は不透明(Opaque)。半透明の2Dスプライトなどを描画する場合はAlphaBlendを、
        // 炎・光などの発光エフェクトはAdditiveを、減光表現はMultiplyを、事前乗算済みテクスチャはPremultipliedAlphaを指定する
        BlendMode BlendMode = BlendMode::Opaque;

        // 三角形の表面(front face)を反時計回り(CCW)とみなすか。既定のfalseは「時計回りが表」で、
        // D3D11/D3D12双方のラスタライザ既定値と一致する(裏面はカリングされる)。
        //
        // ワールド行列に負のスケール(ミラーリング)が含まれるとインデックスの巡回順は変わらないまま
        // スクリーン上での三角形の向きだけが反転するため、既定のままでは表と裏の判定が入れ替わり、
        // 本来見えるはずの面がカリングされて物体の内側が描画されてしまう。そうしたインスタンスは
        // このフラグをtrueにしたパイプラインで描くことで、カリングを効かせたまま正しい面を残す
        // (docs/Architecture.html 10.2節)
        bool FrontCounterClockwise = false;
    };

    struct ComputePipelineStateDesc
    {
        IRHIShader* ComputeShader = nullptr;
    };

    // サンプラーの記述子。既定値は異方性16x + Wrapで、これまでの固定MIN_MAG_MIP_LINEARより
    // 浅い角度で見る面のボケを抑える。AddressU/V/Wは3軸まとめてAddressModeで指定する
    // (このエンジンでは軸ごとに別のアドレッシングを使う用途が無いため)
    struct SamplerDesc
    {
        SamplerFilter Filter = SamplerFilter::Anisotropic;
        SamplerAddressMode AddressMode = SamplerAddressMode::Wrap;
        uint32_t MaxAnisotropy = 16;
    };
}
