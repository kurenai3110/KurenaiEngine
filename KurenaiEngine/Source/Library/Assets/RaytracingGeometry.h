#pragma once

#include <cmath>
#include <cstdint>

#include <DirectXPackedVector.h>

namespace Kurenai::Assets
{
    // レイトレーシングでヒットした三角形の陰影計算に必要な頂点属性。
    //
    // 【位置を持たない理由】ヒット点のワールド座標はRayQueryの
    // WorldRayOrigin() + CommittedRayT() * WorldRayDirection() で復元できるため、
    // 描画用のAssets::Vertex(48バイト)をそのまま複製する必要がない。
    // 実際に読む必要があるのは法線とUVだけなので、1頂点16バイトに抑えている
    // (Bistro級のシーンでは頂点数が数百万に達するため、この差がそのままVRAM消費の差になる)。
    //
    // 【HLSL側との対応】シェーダー側では次の宣言と1対1で対応する。
    // レイアウトを変更する場合は必ず両方を同時に直すこと:
    //   struct RTVertexAttribute { float2 UV; uint PackedNormal; uint Padding; };
    struct RaytracingVertexAttribute
    {
        float UV[2] = { 0.0f, 0.0f };
        // 法線をオクタヘドラル図法で2成分へ落とし、さらにhalf2(2×16bit浮動小数点)へ詰めたもの。
        // エンコード方式はShaders/3D/NormalEncoding.hlsliのOctEncode/OctDecodeと同一で、
        // G-Bufferの法線格納(R16G16_Float)と同じ精度になる
        uint32_t PackedNormal = 0;
        // 16バイト境界へ揃えるための予約領域。将来ノーマルマップをレイトレーシング側でも
        // 扱うようになったら、ここへ同じくオクタヘドラル+half2に詰めた接線を入れる
        uint32_t Padding = 0;
    };

    static_assert(sizeof(RaytracingVertexAttribute) == 16, "HLSL側のRTVertexAttributeと一致させるため16バイト固定");

    // Shaders/3D/NormalEncoding.hlsliのOctEncodeと同一の処理をCPU側で行う。
    // シェーダー側のOctDecodeでそのまま復元できるようにするため、式を変えないこと
    inline void OctEncodeNormal(const float normal[3], float& outX, float& outY)
    {
        float n[3] = { normal[0], normal[1], normal[2] };
        const float l1 = std::fabs(n[0]) + std::fabs(n[1]) + std::fabs(n[2]);
        if (l1 > 0.0f)
        {
            n[0] /= l1;
            n[1] /= l1;
            n[2] /= l1;
        }
        else
        {
            // 長さ0の法線(データ不正)はエンコードできないため、+Yを向いた既定値として扱う。
            // ここで弾かずに進めるとNaNがそのままGPUへ流れ、原因の分かりにくい黒画面になる
            n[0] = 0.0f;
            n[1] = 1.0f;
            n[2] = 0.0f;
        }

        if (n[2] < 0.0f)
        {
            const float signX = n[0] >= 0.0f ? 1.0f : -1.0f;
            const float signY = n[1] >= 0.0f ? 1.0f : -1.0f;
            const float x = (1.0f - std::fabs(n[1])) * signX;
            const float y = (1.0f - std::fabs(n[0])) * signY;
            outX = x;
            outY = y;
            return;
        }

        outX = n[0];
        outY = n[1];
    }

    // 法線・UVからRaytracingVertexAttributeを作る
    inline RaytracingVertexAttribute PackRaytracingVertexAttribute(const float normal[3], const float uv[2])
    {
        float encodedX = 0.0f;
        float encodedY = 0.0f;
        OctEncodeNormal(normal, encodedX, encodedY);

        RaytracingVertexAttribute attribute;
        attribute.UV[0] = uv[0];
        attribute.UV[1] = uv[1];
        // half2としてx=下位16bit、y=上位16bitへ詰める(HLSLのf16tof32(packed & 0xFFFF)/
        // f16tof32(packed >> 16)で取り出せる並び)
        const uint32_t halfX = DirectX::PackedVector::XMConvertFloatToHalf(encodedX);
        const uint32_t halfY = DirectX::PackedVector::XMConvertFloatToHalf(encodedY);
        attribute.PackedNormal = halfX | (halfY << 16);
        return attribute;
    }
}
