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

    struct Scene
    {
        std::wstring Name;
        std::vector<ModelInstance> Instances;

        // 各ModelInstanceが持つModel::Lights(モデルファイル埋め込みのライト。glTFのKHR_lights_punctual
        // やFBXのライトノード由来)をInstance::Worldでワールド空間へ変換したものと、.kscene自身の
        // [Light]セクションで直接指定されたライト(元からワールド空間)を合成した、シーン全体の
        // ライト一覧。KurenaiEngine3Dはこれをそのまま読んでGPUのライトバッファを構築する
        std::vector<Light> Lights;

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
