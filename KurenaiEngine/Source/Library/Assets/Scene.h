#pragma once

#include <DirectXMath.h>

#include <cstdint>
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

        // .ksceneの[Model]Waterで指定される。trueの場合、KurenaiEngine3DはこのインスタンスをG-Bufferパスの
        // 通常PSOではなく水面専用PSO(Water.hlsl)で描画し、G-BufferのMaterial.aへ水面のマテリアルID
        // (kMaterialIDWater、Shaders/3D/GBufferCommon.hlsli)を書き込む(P2: 水面マテリアル基盤)
        bool IsWater = false;
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

    // DDGI(Dynamic Diffuse Global Illumination、22章)のプローブ格子を張る直方体ボリューム。
    //
    // 反射プローブ(上)が「少数を手で置き、主に鏡面の映り込みを担う」のに対し、こちらは
    // 「格子状に多数を自動配置し、拡散の間接光だけを担う」。両者は目的が違うため併用する。
    //
    // 各プローブはオクタヘドラル投影の2Dアトラスへ、方向ごとのイラディアンスと
    // 「その方向の面までの距離」の2つのモーメントを持つ。後者があることで
    // 「このプローブからこのピクセルは見えているか」を統計的に判定でき、
    // 仕切りの向こう側の明るさが漏れてくるのを抑えられる(これがDDGIの要)
    struct GIVolume
    {
        // ボリュームの最小コーナー(ワールド空間)。プローブiは Origin + i * ProbeSpacing に置かれる。
        // 中心指定ではなく最小コーナー指定なのは、格子の位置を間隔と個数から一意に決めるため
        float Origin[3] = { 0.0f, 0.0f, 0.0f };
        // 各軸のプローブ間隔(ワールド単位)。狭いほど間接光の空間解像度が上がるがプローブ数が増える
        float ProbeSpacing[3] = { 2.0f, 2.0f, 2.0f };
        // 各軸のプローブ数。トライリニア補間は周囲8個を使うため、各軸2以上でなければならない
        uint32_t ProbeCounts[3] = { 8u, 4u, 8u };

        // 遮蔽判定の照会点を面の法線方向へ浮かせる量(ワールド単位)。
        // これが無いと「面が、自分を直接照らしているプローブから見えていない」と誤判定し、
        // 画面全体が一様に暗くなる(22章の自己遮蔽)
        float NormalBias = 0.25f;
        // 同じく視線方向へ寄せる量。深度の量子化が効く浅い角度の面で法線方向だけでは足りないため
        float ViewBias = 0.10f;

        // 履歴とのブレンド率。1に近いほど滑らかに追従する代わりに、光が変わってからの
        // 収束が遅くなる。0なら毎回上書き(時間分割と噛み合わず、更新されたプローブだけが
        // 突然変わってちらつく)
        float Hysteresis = 0.97f;

        // 距離モーメントを記録する際の上限(ワールド単位)。
        //
        // 【必須の値であり、大きくしてはいけない】ジオメトリに当たらなかった方向には
        // 十分大きな値が入っている(IBLConvolve.hlslのkProbeSkyDistance = 1e6)。これを
        // そのまま平均すると2つの意味で壊れる:
        //   1. 分散 σ² = 平均二乗距離 - 平均距離² が桁落ちで潰れる。1e6の二乗は1e12で、
        //      fp32の有効桁(約7桁)ではこの引き算から意味のある分散が残らない
        //   2. 空と壁が同じテクセルに混ざったとき、平均が空側に完全に引っ張られ、
        //      チェビシェフ判定が「どこも遠い(=何にも遮蔽されない)」に倒れる
        // チェビシェフ判定に必要なのは「近いか遠いか」の区別だけなので、遠方を潰しても
        // 判定の意味は変わらない。プローブ間隔の数倍を目安にする
        float MaxRayDistance = 8.0f;

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

        // .ksceneの[GIVolume]セクションで配置されたDDGIボリュームの一覧(ワールド空間)。
        // 現状KurenaiEngine3Dが使うのは先頭の1つだけで、2つ目以降は警告を出して切り捨てる
        // (複数ボリュームの重なりや優先順位を決める仕組みがまだ無いため)。
        // ここをvectorにしてあるのは、対応した時点で読み込み側を変えずに済ませるため
        std::vector<GIVolume> GIVolumes;

        // [Camera]セクションが無い場合はfalseのままで、呼び出し側は
        // KurenaiEngine3D::ComputeInitialCamera相当の自動配置ヒューリスティックを使う
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

        // トーンマップのカーブ。Source/LibraryはSource/Engineに依存できないため、
        // KurenaiEngine3D::TonemapCurveと同じ並びの独立した列挙をここに持つ
        // (KurenaiEngine3D::ApplyLoadedSceneが1対1で対応付ける。並びを変えたら両方直すこと)。
        // 既定のAgXはハイライトが色相を保ったまま白へ脱色するので赤い内観に強い一方、
        // 空のような広い面では彩度を落とす(実測: 空の最も青い画素でB/R 1.53→1.34)。
        // 屋外の風景ではACESのほうが空の青が残るため、シーン単位で選べるようにしてある
        enum class TonemapCurveSetting
        {
            Reinhard,
            ACES,
            AgX,
        };
        TonemapCurveSetting Tonemap = TonemapCurveSetting::AgX;

        // 空の彩度(アート指定)。既定1.0は物理モデルの色度そのまま。色度図上で白色点から
        // 遠ざける倍率で、色相は変えずに鮮やかさだけを変える。物理量ではないので
        // 「写真に寄せたい」シーンだけが明示的に上げる
        float SkySaturation = 1.0f;

        // シーン全体の露出(EV100)。**指定されたときだけ**エンジンの設定を上書きする
        // (IBLIntensityと同じ扱い)。Tonemap/SkySaturationのような無条件の反映にしないのは、
        // 露出はUIでも頻繁に触る値で、Exposureを持たないシーンを読み直すたびに
        // ユーザーの調整を既定値へ戻してしまうため。
        //
        // 【なぜシーンが持つのか】屋外の風景と屋内では被写体の輝度が桁で違う。エンジンの
        // 既定値(EngineDefaults.h の SceneExposureEV100 = 15)は屋内基準で決まっており、
        // 物理的に正しい空(地平線際が天頂の8倍明るい)を入れると屋外ではトーンカーブの肩に
        // 乗って彩度が落ちる。既定値を動かすと他のシーンを巻き込むので、シーン側に持たせる
        bool HasExposureOverride = false;
        float ExposureEV100 = 15.0f;   // EngineDefaults.h の SceneExposureEV100 と同じ値にすること

        // --- [Cloud]セクション(P10)。天候はシーンが持つべき性質なので、[Water]と同じく
        // シーンごとに指定できるようにする。**指定されたキーだけ**エンジンの設定を上書きする
        // (Exposure/IBLIntensityと同じ扱い)ので、書かなかったキーはエンジンの既定値のまま。
        // 既定値はEngineDefaults.hの複製で、両方を同時に直すこと ---
        bool HasCloudCoverage = false;
        float CloudCoverage = 0.40f;
        bool HasCloudAltitude = false;
        float CloudAltitude = 1500.0f;      // 雲底の高度[m]
        bool HasCloudThickness = false;
        float CloudThickness = 400.0f;      // 雲底から雲頂までの厚み[m]
        bool HasCloudDensity = false;
        float CloudDensity = 8.0f;          // 光学的な濃さ。上げるほど不透明で白い塊になる
        bool HasCloudCellSize = false;
        float CloudCellSize = 1000.0f;      // 雲の塊1つぶんのワールド上の大きさ[m]。
                                             // エンジン側はこの逆数(UvScale)を持つ
        bool HasCirrusCoverage = false;
        float CirrusCoverage = 0.5f;        // 高層の巻雲。0で消える

        // 各ModelInstanceのAABB(Modelのローカル空間Bounds)をWorldで変換し合成した、
        // シーン全体のワールド空間AABB。ComputeInitialCamera/ComputeLightViewProjが使う
        float BoundsMin[3] = { 0.0f, 0.0f, 0.0f };
        float BoundsMax[3] = { 0.0f, 0.0f, 0.0f };

        // .ksceneの[Water]セクション(P2: 水面マテリアル基盤)。水面(ModelInstance::IsWater)専用の
        // シェーディングパラメータで、[Water]が無いシーンでは既定値のまま(水面インスタンス自体も
        // 存在しないため未使用)。NormalMapはAssetsルートからの相対パスで、[Scene]Skyboxと同じ
        // ルート外チェックを通したうえで絶対パスへ解決してからここへ入る。空文字列なら
        // 「法線マップ無しのフラット水面」を意味し、エラーではない(C++側は1x1のフラット法線
        // テクスチャへフォールバックする)
        std::wstring WaterNormalMapPath;
        // 波の見た目に関する3つの既定値。SunTimeOfDay等と同じ方針で、EngineDefaults.h
        // ([--- 水面 ---]セクション)の値をリテラルとして複製している(Source/Libraryは
        // Source/Engineに依存できないため、Defaults::を直接参照できない)。
        // シーン読み込み時にKurenaiEngine3D::m_WaterWaveScale等へコピーされ、以降はUIで
        // 実行時上書きできる(m_ReflectionModeがScene.SSREnabledから初期化されるのと同じ設計)
        float WaterWaveScale = 12.0f;
        float WaterWaveSpeed = 0.03f;
        float WaterWaveStrength = 0.25f;
    };
}
