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

        // AO/間接光(SSAO・SSIL)を有効にするか。Furnace Testでは球の縁がAOで暗くなると
        // 「エネルギー損失による暗さ」と区別がつかなくなるため無効にする
        bool AOEnabled = true;

        // SSR(スクリーンスペースリフレクション)を有効にするか。SSRは既にエネルギー保存している
        // 鏡面IBLへ反射色を「加算」する設計のため、Furnace Testでは球が背景より明るくなってしまう
        // (同じ反射を二重に計上している。docs/Architecture.html 14.9.5節)。そのため無効にする
        bool SSREnabled = true;

        // 各ModelInstanceのAABB(Modelのローカル空間Bounds)をWorldで変換し合成した、
        // シーン全体のワールド空間AABB。FrameCameraToModel/ComputeLightViewProjが使う
        float BoundsMin[3] = { 0.0f, 0.0f, 0.0f };
        float BoundsMax[3] = { 0.0f, 0.0f, 0.0f };
    };
}
