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
        // このバッファを、本来の用途に加えてStructuredBuffer<T>としてもシェーダーから
        // 読めるようにするか(SRVを追加で作る)。BufferUsage::VertexとBufferUsage::Indexに
        // のみ意味がある。
        //
        // メッシュシェーダーには入力アセンブラが無く、頂点は自分でバッファから読むしかない。
        // コンピュートシェーダーによる自前ラスタライザ(SoftwareRaster.hlsl)はさらに
        // インデックスも自分で引く。かといって別に同じ内容の構造化バッファを作ると
        // VRAMを二重に食うため、同一リソースへ本来のビューとSRVの両方を張れるようにする。
        // StrideInBytesがそのままStructuredBufferの要素サイズになる
        // (頂点ならsizeof(Vertex)、インデックスなら4 = StructuredBuffer<uint>)。
        //
        // DX11実装は参照しない(メッシュシェーダーも自前ラスタライザも存在せず、用途が無いため)
        bool ShaderReadable = false;

        // BufferUsage::StructuredReadOnlyのバッファを1フレームに何回UpdateBufferするかの上限。
        // DX12はこの値からステージングリングの段数(値 × kFrameCount + 1)を決める。
        // CPUはkFrameCountフレームぶん先行して記録するため、これを超えて更新すると
        // GPUが読み取り中のスロットを上書きして描画結果が静かに壊れる
        // (DX12Buffer::AdvanceUploadRingAndGetWritePtrがログで検出する)。
        // 段数ぶんのUPLOADヒープを常時確保するので、大きなバッファに大きな値を与えないこと。
        //
        // DX11実装は参照しない(D3D11_USAGE_DYNAMIC + Map(WRITE_DISCARD)はドライバが毎回
        // リネームするため、1フレームあたりの更新回数に上限が無い)
        uint32_t MaxUpdatesPerFrame = 4;

        // BufferUsage::Constantのバッファを1フレームに何回UpdateBufferするかの上限。
        // 0なら実装既定(DX12は8192スロット = 1フレームあたり4096回)を使う。
        //
        // 定数バッファも上のStructuredReadOnlyと同じ理由でリングになっている。
        // メッシュごと・パスごとに書き換えるバッファ(ObjectConstants)は、シーンの
        // メッシュ数とパス数の積だけ書かれるため、既定では足りないことがある
        // ―― BistroInteriorLit(不透明59メッシュ)はプローブのキャプチャだけで
        // 1フレーム6000回を超え、実際にリングを一周して描画が壊れていた。
        //
        // DX12はこの値 × kFrameCount ぶんのスロットをUPLOADヒープに常時確保するので、
        // (1スロットは256バイト境界へ切り上げられる)大きな値は必要なバッファにだけ与えること。
        // DX11実装は参照しない(MaxUpdatesPerFrameと同じ理由)
        uint32_t MaxConstantUpdatesPerFrame = 0;
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

        // 深度が等しい断片もテストに通すか(比較をGREATER/LESSからGREATER_EQUAL/LESS_EQUALへ緩める)。
        // 深度プリパスと組み合わせて使う ―― プリパスが書いた深度と同じ値になる最前面の断片だけを
        // 通し、それより奥の断片を早期Zで落とすことで、隠れる画素のピクセルシェーダーを省く。
        //
        // 【プリパスが無い状態で有効にしても絵は実質変わらない】GREATERとGREATER_EQUALの差が出るのは
        // 深度がビット単位で等しい面が複数ある場合(同一平面の重なり)だけで、そのとき「先に描いた方が
        // 残る」が「後に描いた方が残る」に変わる。不透明G-Bufferではどちらも同じ値を書くため、
        // PSOを2組に増やさずプリパスの有無を切り替えられるよう常にこちらを使う
        bool DepthAllowEqual = false;

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

    // 増幅シェーダー(任意)+ メッシュシェーダー + ピクセルシェーダーによる描画のパイプラインステート。
    //
    // PipelineStateDescとの違いはInputLayout / VertexShaderを持たないことだけで、
    // ラスタライザ・深度・ブレンド・レンダーターゲットフォーマットの扱いはすべて同じ。
    // 同じG-Bufferへ書くパスを頂点シェーダー版とメッシュシェーダー版で切り替えられるよう、
    // 対応するフィールドは名前も既定値もPipelineStateDescと揃えてある
    struct MeshPipelineStateDesc
    {
        // nullptrでもよい(その場合カリングを行わず、DispatchMeshで指定した数だけ
        // メッシュシェーダーが直接起動される)
        IRHIShader* AmplificationShader = nullptr;
        IRHIShader* MeshShader = nullptr;
        IRHIShader* PixelShader = nullptr;

        std::vector<Format> RenderTargetFormats;
        bool HasDepthStencil = false;
        bool DepthTargetAttached = false;
        bool DepthWriteEnabled = true;
        bool ReverseZ = false;
        bool DepthAllowEqual = false;
        BlendMode BlendMode = BlendMode::Opaque;
        bool FrontCounterClockwise = false;
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
