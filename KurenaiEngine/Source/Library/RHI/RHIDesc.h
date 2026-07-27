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

    struct PipelineStateDesc
    {
        std::vector<InputElementDesc> InputLayout;
        IRHIShader* VertexShader = nullptr;
        IRHIShader* PixelShader = nullptr;
        PrimitiveTopology Topology = PrimitiveTopology::TriangleList;

        // DX12のパイプラインステートオブジェクト作成時にレンダーターゲット/深度のフォーマットを
        // 事前に確定させる必要があるため保持する。DX11実装では参照しない
        std::vector<Format> RenderTargetFormats;
        bool HasDepthStencil = false;

        // 深度への書き込みを行うか。既定はtrue(通常の不透明描画)。半透明描画では、既存の不透明物体の
        // 深度に対してテストはしたいが(裏側の物体に隠れさせるため)、書き込みは行いたくない
        // (奥から手前に描く複数の半透明物体同士が互いの深度で隠し合わないようにするため)。
        // そのためHasDepthStencil=true(テスト有効)のまま、こちらだけfalseにする
        bool DepthWriteEnabled = true;

        // Reverse-Z(深度比較をGREATERにし、近平面=1.0/遠平面=0.0にマッピングする)を使うか。
        // 浮動小数点深度バッファと組み合わせて遠方のZファイティングを抑えるための設定で、
        // 透視投影のメインカメラパスにのみ使う(正射影のシャドウマップは元々Zが線形分布のため対象外)
        bool ReverseZ = false;

        // アルファブレンド設定。既定は不透明(Opaque)。半透明の2Dスプライトなどを描画する場合はAlphaBlendを、
        // 炎・光などの発光エフェクトはAdditiveを、減光表現はMultiplyを、事前乗算済みテクスチャはPremultipliedAlphaを指定する
        BlendMode BlendMode = BlendMode::Opaque;
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
