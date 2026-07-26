#pragma once

#include <memory>
#include <string>
#include <vector>

#include "RHI/IRHIBuffer.h"
#include "RHI/IRHITexture.h"

namespace Kurenai::Assets
{
    struct Mesh
    {
        std::unique_ptr<RHI::IRHIBuffer> VertexBuffer;
        std::unique_ptr<RHI::IRHIBuffer> IndexBuffer;
        uint32_t IndexCount = 0;
        RHI::IRHITexture* BaseColorTexture = nullptr;
        RHI::IRHITexture* NormalTexture = nullptr;
        RHI::IRHITexture* MetallicRoughnessTexture = nullptr;
        float MetallicFactor = 0.0f;
        float RoughnessFactor = 0.7f;
    };

    enum class LightType : uint32_t
    {
        Directional = 0,
        Point       = 1,
        Spot        = 2,
        // 3以降はエリアライト(球/チューブ/矩形)用に予約。今回は未実装
    };

    struct Light
    {
        LightType Type = LightType::Point;
        float Position[3]{ 0.0f, 0.0f, 0.0f };
        float Direction[3]{ 0.0f, -1.0f, 0.0f };  // 光が進む向き(正規化済み)。Spot/Directional で使用
        float Color[3]{ 1.0f, 1.0f, 1.0f };       // 線形色。最大成分が1になるよう正規化して保持する
        // 測光量。Point/Spot はカンデラ(cd = lm/sr)、Directional はルクス(lx = lm/m²)。
        // glTF は KHR_lights_punctual の値をそのまま格納する(物理単位として正確)。
        // FBX は物理単位を持たないため、DCC側のIntensity/100をカンデラ相当として近似する(ModelLoader参照)
        float Intensity = 1.0f;
        float Range = 10.0f;                      // 影響半径。Directional では未使用
        float SpotInnerConeAngle = 0.4f;          // ラジアン(軸からの半角)。Spot のみ
        float SpotOuterConeAngle = 0.6f;
        bool Enabled = true;
        std::string Name;                         // aiLight::mName 由来。ImGui 一覧の表示に使う
    };

    struct Model
    {
        std::vector<Mesh> Meshes;
        std::vector<std::unique_ptr<RHI::IRHITexture>> Textures;
        std::vector<Light> Lights;
        float BoundsMin[3] = { 0.0f, 0.0f, 0.0f };
        float BoundsMax[3] = { 0.0f, 0.0f, 0.0f };
    };
}
