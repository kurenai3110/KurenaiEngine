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

        // Worldの行列式が負(ミラーリング)か。TangentSignFlipと同じ条件から求まるが、用途が異なる。
        // TangentSignFlipは接線の向きを補正するためにシェーダーへ渡す係数で、こちらは三角形の
        // ワインディングが反転することへの対処(表裏判定を入れ替えたパイプラインで描く)に使う
        bool IsMirrored = false;
    };

    // 反射プローブ(リフレクションプローブ)。この位置から周囲をキューブマップへキャプチャし、
    // 畳み込んだものを影響範囲内のピクセルのIBL環境ソースとして使う。シーン全体で1つしかない
    // スカイボックス由来のIBLと違い、位置ごとに異なる環境(屋内なら屋内の壁・天井)を反映できる
    // 反射プローブの影響範囲の形状
    enum class ReflectionProbeShape
    {
        // 中心からの距離だけで判定する球。設定項目がRadiusひとつで済む反面、部屋の形に
        // 沿わせられず、視差補正(下記)も行えない
        Sphere,
        // プローブ位置を中心とする、Y軸まわりに回転できる直方体(OBB)。部屋の壁・床・天井に
        // 合わせて置くことで、影響範囲が部屋の外へはみ出さなくなるうえ、反射ベクトルを
        // この箱と交差させる視差補正が使えるようになる
        Box,
    };

    struct ReflectionProbe
    {
        // ワールド空間のキャプチャ位置(この点から6方向を撮る)。Box形状の場合は箱の中心でもある
        float Position[3] = { 0.0f, 0.0f, 0.0f };
        // 影響範囲の半径(ワールド単位)。この球の内側のピクセルがこのプローブの環境を受け、
        // 外側はスカイボックス由来のグローバルIBLへフォールバックする(Sphere形状のときのみ使用)
        float Radius = 10.0f;

        ReflectionProbeShape Shape = ReflectionProbeShape::Sphere;
        // Box形状の各軸の半径(ハーフエクステント)。プローブのローカル空間(Yaw回転後)での値
        float BoxExtents[3] = { 10.0f, 10.0f, 10.0f };
        // Box形状のY軸まわりの回転(度)。壁が軸に平行でない部屋へ合わせるためのもの。
        // 傾いた床・天井を持つ空間は想定していないため、ピッチ・ロールは持たない
        float YawDegrees = 0.0f;

        // 影響範囲の境界から内側へ何ワールド単位かけて重みを1まで立ち上げるか。
        // 0だと境界でプローブが突然切り替わり継ぎ目が出る(Phase 1の挙動)。
        // 重なり合うプローブ同士・プローブとグローバルIBLの間の滑らかな移行に使う
        float BlendDistance = 2.0f;

        std::string Name;
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

        // .ksceneの[ReflectionProbe]セクションで配置された反射プローブの一覧(ワールド空間)。
        // ライトと違いモデルファイルへ埋め込む概念が無いため、.ksceneに書かれたものが全て
        std::vector<ReflectionProbe> ReflectionProbes;

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
