#pragma once

#include <DirectXMath.h>

#include <string>
#include <vector>

#include "Model.h"

namespace Kurenai::Assets
{
    // シーン内に配置された1つのモデルインスタンス。Modelのジオメトリ自体は
    // ワールド空間原点に焼き込み済み(ModelLoader.cpp参照)のままなので、
    // 実際の配置はWorld/NormalMatrixで頂点シェーダー側にて適用する(KurenaiEngine3D::
    // MakeObjectConstants参照)
    struct ModelInstance
    {
        Model Model;
        DirectX::XMFLOAT4X4 World;          // Scale * Rotation * Translation(転置済み、HLSLへそのまま渡せる形)
        DirectX::XMFLOAT4X4 NormalMatrix;   // Worldの3x3部分の逆転置(4x4に格納、転置済み)
        float TangentSignFlip = 1.0f;       // Worldの行列式が負(ミラーリング)なら-1
    };

    enum class LightType
    {
        Point,
        Spot,
    };

    // .ksceneの[Light]セクションから読み取った内容を保持するだけで、現時点では描画に反映しない
    // (太陽光のみが実際の照明に使われる。将来の点光源/スポットライト実装用の受け皿)
    struct SceneLight
    {
        LightType Type = LightType::Point;
        float Position[3] = { 0.0f, 0.0f, 0.0f };
        float Direction[3] = { 0.0f, -1.0f, 0.0f }; // Spotのみ意味を持つ
        float Color[3] = { 1.0f, 1.0f, 1.0f };
        float Intensity = 1.0f;
        float Range = 10.0f;
        float ConeAngleDegrees = 45.0f;             // Spotのみ意味を持つ
    };

    struct Scene
    {
        std::wstring Name;
        std::vector<ModelInstance> Instances;
        std::vector<SceneLight> Lights;

        // [Camera]セクションが無い場合はfalseのままで、呼び出し側はFrameCameraToModel相当の
        // 自動配置ヒューリスティックを使う
        bool HasCameraOverride = false;
        float CameraPosition[3] = { 0.0f, 0.0f, 0.0f };
        float CameraYaw = 0.0f;
        float CameraPitch = 0.0f;

        // [Sun]セクションが無い場合は既定値(従来のKurenaiEngine3Dの初期値と同じ)のまま
        float SunTimeOfDay = 12.0f;
        float SunAzimuthDegrees = 126.87f;
        bool ShadowEnabled = true;

        // 各ModelInstanceのAABB(Modelのローカル空間Bounds)をWorldで変換し合成した、
        // シーン全体のワールド空間AABB。FrameCameraToModel/ComputeLightViewProjが使う
        float BoundsMin[3] = { 0.0f, 0.0f, 0.0f };
        float BoundsMax[3] = { 0.0f, 0.0f, 0.0f };
    };
}
