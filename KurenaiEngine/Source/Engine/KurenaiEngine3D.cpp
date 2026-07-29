#include "KurenaiEngine3D.h"

#include <imgui.h>

#include <objbase.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <random>

#include "Assets/SceneLoader.h"
#include "Core/Logger.h"
#include "Core/RenderGraph.h"
#include "Core/StringUtil.h"

namespace Kurenai
{
    namespace
    {
        using Core::GetModuleDirectory;
        using Core::WideToUtf8;

        struct alignas(16) FrameConstants
        {
            DirectX::XMFLOAT4X4 ViewProj;
            DirectX::XMFLOAT4X4 InvViewProj;
            // カスケードシャドウマップ(CSM)用、カスケードごとのライト視点ビュー・プロジェクション行列。
            // 以前は単一行列(LightViewProj)だったが、M2でカスケード化した際にここを直接配列化した
            // (このフィールドより後ろにCameraPosition等が続くため、この配列サイズを変える場合は
            // 全シェーダのFrameConstants宣言を合わせて更新する必要がある)
            DirectX::XMFLOAT4X4 CascadeViewProj[KurenaiEngine3D::kCascadeCount];
            DirectX::XMFLOAT4 CameraPosition;
            DirectX::XMFLOAT4 LightDirection;
            DirectX::XMFLOAT4 LightColor;
            // SSAOパスがView空間でのサンプリングに使う(末尾に追加し、既存シェーダのオフセットは変えない)
            DirectX::XMFLOAT4X4 View;
            DirectX::XMFLOAT4X4 Proj;
            // 昼夜サイクル用(末尾に追加し、既存シェーダのオフセットは変えない)。rgb=環境光の色
            // (m_AmbientScale乗算済み、Render()側のconstants.AmbientColor代入部を参照)、
            // a=昼度(0=夜,1=昼。m_AmbientScaleは掛けない)
            DirectX::XMFLOAT4 AmbientColor;
            // M2: カスケード選択・PCSS用(末尾に追加)。xyzw = 各カスケードのView空間far距離
            DirectX::XMFLOAT4 CascadeSplits;
            // x: PCSSのライトサイズ(m_ShadowLightSize)。y: IBLプリフィルタ済み鏡面マップの
            // 最大ミップレベル(kIBLPrefilterMipLevels-1、DeferredLighting.hlslがラフネス→ミップの
            // 変換に使う)。z: IBL強度倍率(m_IBLEnabled=falseの場合は0.0fを渡し、シェーダ側で
            // EvaluateIBLの代わりに定数色アンビエント(AmbientColor.rgb)へフォールバックする)。
            // w: スペキュラのマルチスキャッタリング・エネルギー補正の有効フラグ
            // (m_SpecularEnergyCompensationEnabled、1.0f=有効/0.0f=無効。共有ヘッダー
            // SpecularEnergy.hlsliのSpecularEnergyCompensationが読む。14.9節)
            DirectX::XMFLOAT4 ShadowParams;
            // 半透明パス(Transparent.hlsl)専用。x=t8のライトリストの有効数。DirectLighting.hlslは
            // 専用のLightingConstants(b1)で受け取るためこのフィールドを使わない(末尾に追加のため
            // 既存シェーダのオフセットは変わらない)
            DirectX::XMFLOAT4 ActiveLightCount;
            // 反射プローブ用(末尾に追加)。x=有効プローブ数(0ならプローブを使わずグローバルIBLのみ)、
            // y=影響範囲のデバッグ表示フラグ(1以上でプローブ番号ごとの色分け表示)、zw=未使用。
            // DeferredLighting.hlslのみが読む
            DirectX::XMFLOAT4 ProbeParams;
        };

        // シャドウパスの各カスケード描画専用の定数バッファ(FrameConstantsとは別バッファ)
        struct alignas(16) CascadeConstants
        {
            DirectX::XMFLOAT4X4 ViewProj;
        };

        // IBLConvolve.hlsl(CSIrradiance/CSPrefilter)へ、処理対象の面(キューブマップは面ごとに
        // 個別ディスパッチが必要)とCSPrefilterのみが使うラフネス値を渡す専用の定数バッファ
        struct alignas(16) IBLFaceConstants
        {
            uint32_t Face = 0;
            float Roughness = 0.0f;
            DirectX::XMFLOAT2 Padding{};
        };

        // DeferredLighting.hlsl側のstruct GPUReflectionProbeと並び・ストライド(48バイト)を
        // 一致させる必要がある
        struct alignas(16) GPUReflectionProbe
        {
            DirectX::XMFLOAT4 PositionRadius; // xyz=ワールド座標(Box形状では箱の中心), w=Sphere形状の影響半径
            DirectX::XMFLOAT4 BoxExtents;     // xyz=Box形状の各軸の半径(ハーフエクステント), w=ブレンド距離
            DirectX::XMFLOAT4 ShapeParams;    // x=形状(0=Sphere,1=Box), y=sin(Yaw), z=cos(Yaw), w=未使用
        };

        // キューブマップの1面を撮るためのビュー行列(左手系)。前方向・上方向の組は
        // IBLConvolve.hlslのCubeFaceDirectionが定める面→方向の対応と一致していなければならない
        // (ずれると焼いた面が回転・反転する)。D3Dのキューブマップ標準順(+X,-X,+Y,-Y,+Z,-Z)
        DirectX::XMMATRIX ComputeCubeFaceView(const DirectX::XMFLOAT3& position, uint32_t face)
        {
            using namespace DirectX;

            static const XMFLOAT3 kForward[6] =
            {
                {  1.0f,  0.0f,  0.0f }, // +X
                { -1.0f,  0.0f,  0.0f }, // -X
                {  0.0f,  1.0f,  0.0f }, // +Y
                {  0.0f, -1.0f,  0.0f }, // -Y
                {  0.0f,  0.0f,  1.0f }, // +Z
                {  0.0f,  0.0f, -1.0f }, // -Z
            };
            static const XMFLOAT3 kUp[6] =
            {
                { 0.0f, 1.0f,  0.0f },
                { 0.0f, 1.0f,  0.0f },
                { 0.0f, 0.0f, -1.0f },
                { 0.0f, 0.0f,  1.0f },
                { 0.0f, 1.0f,  0.0f },
                { 0.0f, 1.0f,  0.0f },
            };

            return XMMatrixLookToLH(XMLoadFloat3(&position), XMLoadFloat3(&kForward[face]), XMLoadFloat3(&kUp[face]));
        }

        // プローブのキャプチャ用プロジェクション(画角90度・アスペクト1)。Core::Cameraの
        // 遠近投影と同じReverse-Z(近平面=NDC z=1.0、遠平面=NDC z=0.0)で作る必要がある
        // (深度クリア値・PipelineStateDesc::ReverseZが同じ前提で組まれているため)
        DirectX::XMMATRIX ComputeCubeFaceProjection(float nearZ, float farZ)
        {
            // 画角90度なのでtan(45度)=1、すなわちw=h=1になる
            const float a = nearZ / (nearZ - farZ);
            const float b = -a * farZ;

            return DirectX::XMMatrixSet(
                1.0f, 0.0f, 0.0f, 0.0f,
                0.0f, 1.0f, 0.0f, 0.0f,
                0.0f, 0.0f, a, 1.0f,
                0.0f, 0.0f, b, 0.0f);
        }

        // 太陽光の向き・色・環境光を時刻(0〜24時)から計算する
        struct SunLighting
        {
            DirectX::XMFLOAT3 Direction; // 光が進む向き(サーフェスに当たる方向)
            DirectX::XMFLOAT4 Color;
            DirectX::XMFLOAT4 Ambient; // rgb=環境光の色, a=昼度(0=夜,1=昼)
        };

        // edge0とedge1の間をなめらかに0→1で補間する(edge0以下は0、edge1以上は1)
        float Smoothstep(float edge0, float edge1, float x)
        {
            const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        }

        // 実在の写真露出値(EV100)から露出係数を求める。絞り値・シャッター速度・ISO感度から一意に
        // 定まる実在の量で、Lagarde & de Rousiers, "Moving Frostbite to Physically Based Rendering"
        // (SIGGRAPH 2014 course notes)やGoogle FilamentのPhysically Based Cameraドキュメントが
        // 採る標準式。カンデラ/ルクスの測光量に直接掛けることで表示レンジへ変換する
        // (放射量(W)への変換は行わない。本エンジンには放射量ベースの大気モデルが無く、
        // 変換段を増やす意味が無いため)
        float ComputeExposure(float ev100)
        {
            return 1.0f / (1.2f * std::pow(2.0f, ev100));
        }

        SunLighting ComputeSunLighting(float timeOfDayHours, float sunAzimuthDegrees, float exposureEV100)
        {
            using namespace DirectX;

            // 日の出(東)側の水平方向。太陽はこの方向と天頂(真上)を通る鉛直面内で、
            // 東→天頂(正午)→西→天底(真夜中)と一日一周する半円軌道を描く。
            // 方位角(sunAzimuthDegrees)はX軸を0度、Z軸(+方向)を90度としてImGuiで調整する
            const float azimuthRadians = XMConvertToRadians(sunAzimuthDegrees);
            const XMFLOAT3 kSunriseHorizontal{ std::cos(azimuthRadians), 0.0f, std::sin(azimuthRadians) };

            // 6時=0度(日の出/東)、12時=90度(天頂)、18時=180度(日の入り/西)、24時=270度(天底/真夜中)
            const float hourAngle = (timeOfDayHours / 24.0f) * XM_2PI - XM_PIDIV2;
            const float sinHour = std::sin(hourAngle);
            const float cosHour = std::cos(hourAngle);

            // 太陽の方向(地面から見て太陽がある向き)。kSunriseHorizontalとY軸(天頂)を結ぶ円軌道上の点
            const XMFLOAT3 sunDirection{ kSunriseHorizontal.x * cosHour, sinHour, kSunriseHorizontal.z * cosHour };

            SunLighting result{};
            result.Direction = { -sunDirection.x, -sunDirection.y, -sunDirection.z };

            // 6時〜7時でなめらかに夜→昼、17時〜18時でなめらかに昼→夜へ切り替える。
            // この遷移カーブ自体は物理的な大気散乱シミュレーションではなく既存のアート的な遷移のまま
            const float dayFactor = Smoothstep(6.0f, 7.0f, timeOfDayHours) * (1.0f - Smoothstep(17.0f, 18.0f, timeOfDayHours));

            // 太陽の色味(ティント)。ピーク照度はkSunIlluminanceLuxが持つので、ここは相対比のみ
            const XMFLOAT3 kSunColorTint{ 1.0f, 0.967f, 0.9f };
            // 直射日光(正午・快晴)の照度[lx]。Lagarde & de Rousiers 2014の照度参照テーブルに
            // 掲載される代表値
            constexpr float kSunIlluminanceLux = 100000.0f;
            // 空光(直射日光を除いた間接照度)の照度[lx]。同テーブルの曇天相当値を、直射日光に対する
            // 空光の比率(おおむね1〜2割)としても妥当な範囲であることの根拠として採用する
            constexpr float kSkylightIlluminanceLux = 20000.0f;
            // 夜間の環境光は天文学的な実測値(星明かり~0.001lx、満月~0.1〜0.3lx)をそのまま使うと
            // ほぼ完全な黒になり視認性が失われるため、視認性確保のためのアート的な下限値のまま残す
            // (物理値ではないことを明記した上での意図的な妥協)
            const XMFLOAT3 kNightAmbientArt{ 0.006f, 0.008f, 0.015f };

            const float exposure = ComputeExposure(exposureEV100);

            const float sunPeak = kSunIlluminanceLux * dayFactor * exposure;
            result.Color = { kSunColorTint.x * sunPeak, kSunColorTint.y * sunPeak, kSunColorTint.z * sunPeak, 0.0f };

            const float skyPeak = kSkylightIlluminanceLux * exposure;
            const XMFLOAT3 dayAmbient{ kSunColorTint.x * skyPeak, kSunColorTint.y * skyPeak, kSunColorTint.z * skyPeak };
            result.Ambient =
            {
                kNightAmbientArt.x + (dayAmbient.x - kNightAmbientArt.x) * dayFactor,
                kNightAmbientArt.y + (dayAmbient.y - kNightAmbientArt.y) * dayFactor,
                kNightAmbientArt.z + (dayAmbient.z - kNightAmbientArt.z) * dayFactor,
                dayFactor,
            };

            return result;
        }

        // Shaders/GBuffer.hlsl・Shaders/Shadow.hlslのObjectConstants(register b1)と
        // レイアウトを一致させる必要がある。DX12のルートシグネチャがCBVをb0/b1の2枠しか
        // 持たないため、モデル行列もマテリアル係数(Emissive/AlphaCutoff含む)と同居させている
        // (Architecture.html参照)。float3(EmissiveFactor)以降が16バイト境界をまたがないよう、
        // 直前のMetallicFactor/RoughnessFactor/TangentSignFlip/AlphaCutoffで先に16バイトを
        // 埋めてからEmissiveFactor+Paddingで次の16バイトを埋める配置にしている
        struct alignas(16) ObjectConstants
        {
            DirectX::XMFLOAT4X4 World;
            DirectX::XMFLOAT4X4 NormalMatrix;
            float MetallicFactor;
            float RoughnessFactor;
            float TangentSignFlip;
            // 0以下ならアルファカットアウト無効
            float AlphaCutoff;
            float EmissiveFactor[3];
            float Padding;
            // glTFのbaseColorFactor(既定[1,1,1,1])。GBuffer.hlsl/Shadow.hlslはこのフィールドを
            // 宣言していない(=読まない)ため、末尾に追加してもオフセットへの影響は無い。
            // Transparent.hlsl(半透明フォワードパス)のみがBaseColorTextureと乗算して使う(14章参照)
            float BaseColorFactor[4];
        };

        // instance.World/NormalMatrix/TangentSignFlipはAssets::LoadScene(SceneLoader.cpp)が
        // TRS(平行移動・回転・スケール)から計算済み(HLSL側のmul(vec, matrix)規約に合わせて
        // 転置済み)なので、ここでは単純にコピーするだけでよい
        ObjectConstants MakeObjectConstants(const Assets::ModelInstance& instance, const Assets::Mesh& mesh)
        {
            ObjectConstants constants{};
            constants.World = instance.World;
            constants.NormalMatrix = instance.NormalMatrix;
            constants.MetallicFactor = mesh.MetallicFactor;
            constants.RoughnessFactor = mesh.RoughnessFactor;
            constants.TangentSignFlip = instance.TangentSignFlip;
            constants.AlphaCutoff = mesh.AlphaCutoff;
            constants.EmissiveFactor[0] = mesh.EmissiveFactor[0];
            constants.EmissiveFactor[1] = mesh.EmissiveFactor[1];
            constants.EmissiveFactor[2] = mesh.EmissiveFactor[2];
            constants.BaseColorFactor[0] = mesh.BaseColorFactor[0];
            constants.BaseColorFactor[1] = mesh.BaseColorFactor[1];
            constants.BaseColorFactor[2] = mesh.BaseColorFactor[2];
            constants.BaseColorFactor[3] = mesh.BaseColorFactor[3];
            return constants;
        }

        // Present.hlsl側のModeと一致させる必要がある
        struct alignas(16) PresentConstants
        {
            int32_t Mode;
            float MipLevel;   // Mode==6(Hi-Z)でSampleLevelに渡すミップレベル
            float ArrayIndex; // Mode==10(反射プローブのキューブマップ配列)で表示するプローブ番号
            float Padding;
        };

        // HiZ.hlsl側のcbuffer HiZConstantsと一致させる必要がある
        struct alignas(16) HiZConstants
        {
            DirectX::XMUINT2 SrcSize;
            DirectX::XMUINT2 DstSize;
        };

        // widthとheightのうち大きい方が1になるまでのミップ数(width/heightそのものを含む)を返す。
        // 例: 1280x720 -> max=1280 -> 1280,640,320,160,80,40,20,10,5,2,1 の11ミップ
        uint32_t ComputeMipLevelCount(uint32_t width, uint32_t height)
        {
            uint32_t levels = 1;
            uint32_t size = std::max(width, height);
            while (size > 1)
            {
                size /= 2;
                ++levels;
            }
            return levels;
        }

        // SSAO.hlsl側のkSSAOKernelSizeと一致させる必要がある
        constexpr uint32_t kSSAOKernelSize = 16;

        struct alignas(16) SSAOConstants
        {
            DirectX::XMFLOAT4 Samples[kSSAOKernelSize]; // タンジェント空間の半球カーネル
            DirectX::XMFLOAT4 Params;                   // x: 半径, y: バイアス, z: 強さ(べき乗), w: 未使用
        };

        // SSIL_VisibilityBitmask.hlsl側のcbuffer SSILConstantsと一致させる必要がある
        struct alignas(16) SSILConstants
        {
            DirectX::XMFLOAT4 Params0; // x: 半径, y: 厚み(Thickness Heuristic), z: 間接光の強さ, w: AOのべき乗
            DirectX::XMUINT4 Params1;  // x: スライス数, y: スライスあたりのステップ数, z/w: 未使用
        };

        // SSR.hlsl側のcbuffer SSRConstantsと一致させる必要がある
        struct alignas(16) SSRConstants
        {
            DirectX::XMFLOAT4 Params0; // x: 最大レイ距離, y: ヒット判定の厚み, z: ラフネスカットオフ, w: 未使用
        };

        // DirectLighting.hlsl側のstruct GPULightと並び・ストライド(64バイト)を一致させる必要がある
        struct alignas(16) GPULight
        {
            DirectX::XMFLOAT4 PositionType;   // xyz=ワールド座標, w=LightType
            DirectX::XMFLOAT4 ColorRange;     // rgb=露出済み放射輝度, w=Range
            DirectX::XMFLOAT4 DirectionAngle; // xyz=向き(正規化済み), w=spotAngleScale
            DirectX::XMFLOAT4 Params;         // x=spotAngleOffset, yzw=未使用(エリアライト用に予約)
        };
        static_assert(sizeof(GPULight) == 64, "GPULightはDirectLighting.hlsl側と64バイトで一致させる必要がある");

        // t5の構造化バッファに詰めるライトの最大数。実データ(BistroInterior.fbxで4灯)に対しては
        // 十分すぎる余裕を持たせてあるが、構造化バッファなのでこの容量自体がGPU時間へ影響することはない
        // (シェーダはLightCount.xまでしかループしないため)
        constexpr uint32_t kMaxLights = 1024;

        // DirectLighting.hlsl側のcbuffer LightingConstantsと一致させる必要がある
        struct alignas(16) LightingConstants
        {
            DirectX::XMUINT4 LightCount; // x=有効ライト数, yzw=未使用
        };

        // Assets::LightをGPU側のGPULightへ変換する。カンデラ/ルクスの測光量にEV100露出を直接掛けて
        // 表示レンジへ変換する(設計判断は「強度の単位」節を参照)。Frostbiteのスポット角度減衰用
        // lightAngleScale/lightAngleOffsetもここでCPU事前計算する
        GPULight MakeGPULight(const Assets::Light& light, float exposureEV100)
        {
            const float exposure = ComputeExposure(exposureEV100);
            const float radiance = light.Intensity * exposure;

            GPULight gpuLight{};
            gpuLight.PositionType = { light.Position[0], light.Position[1], light.Position[2], static_cast<float>(light.Type) };
            gpuLight.ColorRange = { light.Color[0] * radiance, light.Color[1] * radiance, light.Color[2] * radiance, light.Range };

            float angleScale = 0.0f;
            float angleOffset = 0.0f;
            if (light.Type == Assets::LightType::Spot)
            {
                // Frostbiteのスポット減衰式: t = saturate(dot(spotDir,-L)*scale + offset), atten = t*t
                const float cosOuter = std::cos(light.SpotOuterConeAngle);
                const float cosInner = std::cos(light.SpotInnerConeAngle);
                angleScale = 1.0f / std::max(0.001f, cosInner - cosOuter);
                angleOffset = -cosOuter * angleScale;
            }
            gpuLight.DirectionAngle = { light.Direction[0], light.Direction[1], light.Direction[2], angleScale };
            gpuLight.Params = { angleOffset, 0.0f, 0.0f, 0.0f };
            return gpuLight;
        }

        // ImGuiでのライト方向編集用。正規化済み方向ベクトルをYaw/Pitch(度)に変換する。
        // DragFloat3で直接編集すると正規化のたびに値が跳ねて操作しづらいため、角度で編集する。
        // Yawは水平面内の角度(X軸を0度、Z軸を90度)、Pitchは水平面からの仰角(下向きが負)
        void DirectionToYawPitch(const float direction[3], float& outYawDegrees, float& outPitchDegrees)
        {
            outYawDegrees = DirectX::XMConvertToDegrees(std::atan2(direction[2], direction[0]));
            outPitchDegrees = DirectX::XMConvertToDegrees(std::asin(std::clamp(direction[1], -1.0f, 1.0f)));
        }

        void YawPitchToDirection(float yawDegrees, float pitchDegrees, float outDirection[3])
        {
            const float yaw = DirectX::XMConvertToRadians(yawDegrees);
            const float pitch = DirectX::XMConvertToRadians(pitchDegrees);
            outDirection[0] = std::cos(pitch) * std::cos(yaw);
            outDirection[1] = std::sin(pitch);
            outDirection[2] = std::cos(pitch) * std::sin(yaw);
        }

        // タンジェント空間(Z軸=法線方向)の半球状にランダムなカーネルサンプルを生成する。
        // John Chapmanのチュートリアルにならい、原点付近にサンプルが偏るようスケーリングして
        // 近距離のディテールを優先的に拾う
        std::vector<DirectX::XMFLOAT4> GenerateSSAOKernel(uint32_t kernelSize)
        {
            std::mt19937 rng(12345);
            std::uniform_real_distribution<float> dist(0.0f, 1.0f);

            std::vector<DirectX::XMFLOAT4> kernel;
            kernel.reserve(kernelSize);
            for (uint32_t i = 0; i < kernelSize; ++i)
            {
                DirectX::XMVECTOR sample = DirectX::XMVectorSet(
                    dist(rng) * 2.0f - 1.0f,
                    dist(rng) * 2.0f - 1.0f,
                    dist(rng),
                    0.0f);
                sample = DirectX::XMVector3Normalize(sample);
                sample = DirectX::XMVectorScale(sample, dist(rng));

                float scale = static_cast<float>(i) / static_cast<float>(kernelSize);
                scale = 0.1f + 0.9f * scale * scale;
                sample = DirectX::XMVectorScale(sample, scale);

                DirectX::XMFLOAT4 sampleF;
                DirectX::XMStoreFloat4(&sampleF, sample);
                sampleF.w = 0.0f;
                kernel.push_back(sampleF);
            }
            return kernel;
        }

        // レンダー解像度(renderWidth x renderHeight)のアスペクト比を保ったまま、
        // windowWidth x windowHeight の中央に収まるビューポート(レターボックス/ピラーボックス)を求める
        RHI::Viewport ComputeLetterboxViewport(uint32_t windowWidth, uint32_t windowHeight, uint32_t renderWidth, uint32_t renderHeight)
        {
            const float windowAspect = static_cast<float>(windowWidth) / static_cast<float>(windowHeight);
            const float renderAspect = static_cast<float>(renderWidth) / static_cast<float>(renderHeight);

            float viewportWidth;
            float viewportHeight;
            if (windowAspect > renderAspect)
            {
                // ウィンドウの方が横長 -> 高さいっぱいに合わせ、左右に余白(ピラーボックス)
                viewportHeight = static_cast<float>(windowHeight);
                viewportWidth = viewportHeight * renderAspect;
            }
            else
            {
                // ウィンドウの方が縦長 -> 幅いっぱいに合わせ、上下に余白(レターボックス)
                viewportWidth = static_cast<float>(windowWidth);
                viewportHeight = viewportWidth / renderAspect;
            }

            RHI::Viewport viewport;
            viewport.TopLeftX = (static_cast<float>(windowWidth) - viewportWidth) * 0.5f;
            viewport.TopLeftY = (static_cast<float>(windowHeight) - viewportHeight) * 0.5f;
            viewport.Width = viewportWidth;
            viewport.Height = viewportHeight;
            return viewport;
        }
    }

    KurenaiEngine3D::KurenaiEngine3D(GraphicsAPI api, uint32_t renderWidth, uint32_t renderHeight)
        : KurenaiEngineBase(L"Kurenai Engine", 1280, 720, api)
        , m_GraphicsAPI(api)
        , m_RenderWidth(renderWidth)
        , m_RenderHeight(renderHeight)
    {
        m_ImGuiBackend = m_Device->CreateImGuiBackend(m_Window->GetHandle());
        m_GPUProfiler = m_Device->CreateGPUProfiler();

        // imgui.iniの保存先を起動時の作業ディレクトリに依存させず、KurenaiEngine.dllと同じフォルダに固定する。
        // ImGuiはIniFilenameのポインタを保持するだけでコピーしないため、m_ImGuiIniPathで寿命を維持する
        m_ImGuiIniPath = WideToUtf8(GetModuleDirectory() + L"imgui.ini");
        ImGui::GetIO().IniFilename = m_ImGuiIniPath.c_str();

        m_Camera.SetAspectRatio(static_cast<float>(m_RenderWidth) / static_cast<float>(m_RenderHeight));

        CreateSceneResources();

        m_LastFrameTime = std::chrono::steady_clock::now();
    }

    KurenaiEngine3D::~KurenaiEngine3D() = default;

    void KurenaiEngine3D::CreateSceneResources()
    {
        // Shaders/AssetsはビルドでKurenaiEngine.dllと同じフォルダにコピーされる
        const std::wstring dataRoot = GetModuleDirectory();
        const std::wstring shaderDirectory = dataRoot + L"Shaders\\";

        const std::vector<RHI::InputElementDesc> modelInputLayout =
        {
            { "POSITION", 0, RHI::Format::R32G32B32_Float, 0 },
            { "NORMAL", 0, RHI::Format::R32G32B32_Float, 12 },
            { "TEXCOORD", 0, RHI::Format::R32G32_Float, 24 },
            { "TANGENT", 0, RHI::Format::R32G32B32A32_Float, 32 },
        };

        // ジオメトリパス(G-Buffer書き込み)
        RHI::ShaderDesc gbufferVsDesc;
        gbufferVsDesc.Stage = RHI::ShaderStage::Vertex;
        gbufferVsDesc.FilePath = shaderDirectory + L"GBuffer.hlsl";
        gbufferVsDesc.EntryPoint = "VSMain";
        m_GBufferVertexShader = m_Device->CreateShader(gbufferVsDesc);

        RHI::ShaderDesc gbufferPsDesc;
        gbufferPsDesc.Stage = RHI::ShaderStage::Pixel;
        gbufferPsDesc.FilePath = shaderDirectory + L"GBuffer.hlsl";
        gbufferPsDesc.EntryPoint = "PSMain";
        m_GBufferPixelShader = m_Device->CreateShader(gbufferPsDesc);

        RHI::PipelineStateDesc gbufferPipelineDesc;
        gbufferPipelineDesc.InputLayout = modelInputLayout;
        gbufferPipelineDesc.VertexShader = m_GBufferVertexShader.get();
        gbufferPipelineDesc.PixelShader = m_GBufferPixelShader.get();
        gbufferPipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
        gbufferPipelineDesc.RenderTargetFormats =
        {
            RHI::Format::R8G8B8A8_UNorm, // Albedo
            RHI::Format::R16G16_Float,   // Normal(オクタヘドラルエンコード)
            RHI::Format::R8G8B8A8_UNorm, // Material(R=Metallic, G=Roughness)
            RHI::Format::R8G8B8A8_UNorm, // Emissive
        };
        gbufferPipelineDesc.HasDepthStencil = true;
        gbufferPipelineDesc.ReverseZ = true;
        m_GBufferPipelineState = m_Device->CreatePipelineState(gbufferPipelineDesc);

        // 直接光パス(頂点バッファなしのフルスクリーン三角形。G-Buffer+シャドウマップからPBRの
        // 直接光を計算しHDRで書き出す)
        RHI::ShaderDesc directLightVsDesc;
        directLightVsDesc.Stage = RHI::ShaderStage::Vertex;
        directLightVsDesc.FilePath = shaderDirectory + L"DirectLighting.hlsl";
        directLightVsDesc.EntryPoint = "VSMain";
        m_DirectLightVertexShader = m_Device->CreateShader(directLightVsDesc);

        RHI::ShaderDesc directLightPsDesc;
        directLightPsDesc.Stage = RHI::ShaderStage::Pixel;
        directLightPsDesc.FilePath = shaderDirectory + L"DirectLighting.hlsl";
        directLightPsDesc.EntryPoint = "PSMain";
        m_DirectLightPixelShader = m_Device->CreateShader(directLightPsDesc);

        RHI::PipelineStateDesc directLightPipelineDesc;
        directLightPipelineDesc.VertexShader = m_DirectLightVertexShader.get();
        directLightPipelineDesc.PixelShader = m_DirectLightPixelShader.get();
        directLightPipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
        directLightPipelineDesc.RenderTargetFormats = { RHI::Format::R32G32B32A32_Float };
        m_DirectLightPipelineState = m_Device->CreatePipelineState(directLightPipelineDesc);

        // AO/GI共通の頂点シェーダ(頂点バッファなしのフルスクリーン三角形)。SSAO/SSIL/共通ブラーの
        // 3つのピクセルシェーダで使い回す
        RHI::ShaderDesc aoVsDesc;
        aoVsDesc.Stage = RHI::ShaderStage::Vertex;
        aoVsDesc.FilePath = shaderDirectory + L"SSAO.hlsl";
        aoVsDesc.EntryPoint = "VSMain";
        m_AOVertexShader = m_Device->CreateShader(aoVsDesc);

        // SSAOパス
        RHI::ShaderDesc ssaoPsDesc;
        ssaoPsDesc.Stage = RHI::ShaderStage::Pixel;
        ssaoPsDesc.FilePath = shaderDirectory + L"SSAO.hlsl";
        ssaoPsDesc.EntryPoint = "PSMain";
        m_SSAOPixelShader = m_Device->CreateShader(ssaoPsDesc);

        RHI::PipelineStateDesc ssaoPipelineDesc;
        ssaoPipelineDesc.VertexShader = m_AOVertexShader.get();
        ssaoPipelineDesc.PixelShader = m_SSAOPixelShader.get();
        ssaoPipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
        ssaoPipelineDesc.RenderTargetFormats = { RHI::Format::R8G8B8A8_UNorm };
        m_SSAOPipelineState = m_Device->CreatePipelineState(ssaoPipelineDesc);

        m_SSAOKernel = GenerateSSAOKernel(kSSAOKernelSize);

        RHI::BufferDesc ssaoConstantBufferDesc;
        ssaoConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        ssaoConstantBufferDesc.SizeInBytes = sizeof(SSAOConstants);
        m_SSAOConstantBuffer = m_Device->CreateBuffer(ssaoConstantBufferDesc);

        // SSILパス(Visibility Bitmask)
        RHI::ShaderDesc ssilPsDesc;
        ssilPsDesc.Stage = RHI::ShaderStage::Pixel;
        ssilPsDesc.FilePath = shaderDirectory + L"SSIL_VisibilityBitmask.hlsl";
        ssilPsDesc.EntryPoint = "PSMain";
        m_SSILPixelShader = m_Device->CreateShader(ssilPsDesc);

        RHI::PipelineStateDesc ssilPipelineDesc;
        ssilPipelineDesc.VertexShader = m_AOVertexShader.get();
        ssilPipelineDesc.PixelShader = m_SSILPixelShader.get();
        ssilPipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
        ssilPipelineDesc.RenderTargetFormats = { RHI::Format::R8G8B8A8_UNorm };
        m_SSILPipelineState = m_Device->CreatePipelineState(ssilPipelineDesc);

        RHI::BufferDesc ssilConstantBufferDesc;
        ssilConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        ssilConstantBufferDesc.SizeInBytes = sizeof(SSILConstants);
        m_SSILConstantBuffer = m_Device->CreateBuffer(ssilConstantBufferDesc);

        // AO/GI共通のブラーパス(SSAO.hlslのPSMainBlurを、rgbaフォーマットが同じSSAO/SSIL両方で使い回す)
        RHI::ShaderDesc aoBlurPsDesc;
        aoBlurPsDesc.Stage = RHI::ShaderStage::Pixel;
        aoBlurPsDesc.FilePath = shaderDirectory + L"SSAO.hlsl";
        aoBlurPsDesc.EntryPoint = "PSMainBlur";
        m_AOBlurPixelShader = m_Device->CreateShader(aoBlurPsDesc);

        RHI::PipelineStateDesc aoBlurPipelineDesc;
        aoBlurPipelineDesc.VertexShader = m_AOVertexShader.get();
        aoBlurPipelineDesc.PixelShader = m_AOBlurPixelShader.get();
        aoBlurPipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
        aoBlurPipelineDesc.RenderTargetFormats = { RHI::Format::R8G8B8A8_UNorm };
        m_AOBlurPipelineState = m_Device->CreatePipelineState(aoBlurPipelineDesc);

        // AO/GI無効時はこの常に黒・不透明(遮蔽なし=a:1、間接光なし=rgb:0)のテクスチャをライティングパスに渡す
        m_AODisabledTexture = m_Device->CreateSolidColorTexture(0, 0, 0, 255);

        // ライティングパス(頂点バッファなしのフルスクリーン三角形)
        RHI::ShaderDesc lightingVsDesc;
        lightingVsDesc.Stage = RHI::ShaderStage::Vertex;
        lightingVsDesc.FilePath = shaderDirectory + L"DeferredLighting.hlsl";
        lightingVsDesc.EntryPoint = "VSMain";
        m_LightingVertexShader = m_Device->CreateShader(lightingVsDesc);

        RHI::ShaderDesc lightingPsDesc;
        lightingPsDesc.Stage = RHI::ShaderStage::Pixel;
        lightingPsDesc.FilePath = shaderDirectory + L"DeferredLighting.hlsl";
        lightingPsDesc.EntryPoint = "PSMain";
        m_LightingPixelShader = m_Device->CreateShader(lightingPsDesc);

        RHI::PipelineStateDesc lightingPipelineDesc;
        lightingPipelineDesc.VertexShader = m_LightingVertexShader.get();
        lightingPipelineDesc.PixelShader = m_LightingPixelShader.get();
        lightingPipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
        lightingPipelineDesc.RenderTargetFormats = { RHI::Format::R16G16B16A16_Float };
        m_LightingPipelineState = m_Device->CreatePipelineState(lightingPipelineDesc);

        // 半透明フォワードパス(Transparent.hlsl)。頂点入力・トポロジはGBufferパスと共通で、
        // 出力先はLightingパスと同じSceneColor(R16G16B16A16_Float)
        RHI::ShaderDesc transparentVsDesc;
        transparentVsDesc.Stage = RHI::ShaderStage::Vertex;
        transparentVsDesc.FilePath = shaderDirectory + L"Transparent.hlsl";
        transparentVsDesc.EntryPoint = "VSMain";
        m_TransparentVertexShader = m_Device->CreateShader(transparentVsDesc);

        RHI::ShaderDesc transparentPsDesc;
        transparentPsDesc.Stage = RHI::ShaderStage::Pixel;
        transparentPsDesc.FilePath = shaderDirectory + L"Transparent.hlsl";
        transparentPsDesc.EntryPoint = "PSMain";
        m_TransparentPixelShader = m_Device->CreateShader(transparentPsDesc);

        RHI::PipelineStateDesc transparentPipelineDesc;
        transparentPipelineDesc.InputLayout = modelInputLayout;
        transparentPipelineDesc.VertexShader = m_TransparentVertexShader.get();
        transparentPipelineDesc.PixelShader = m_TransparentPixelShader.get();
        transparentPipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
        transparentPipelineDesc.RenderTargetFormats = { RHI::Format::R16G16B16A16_Float };
        transparentPipelineDesc.HasDepthStencil = true;
        // 既存の不透明物体には隠れさせたいが(テストは有効)、奥から手前に描く半透明同士が互いの深度で
        // 隠し合わないよう書き込みは行わない
        transparentPipelineDesc.DepthWriteEnabled = false;
        transparentPipelineDesc.ReverseZ = true;
        // 事前乗算済みアルファ(src.rgb + dst.rgb * (1 - src.a))。標準アルファブレンドではなく
        // こちらを使うのは、ガラスの鏡面反射(スペキュラ)を不透明度で減衰させないため。
        // 標準アルファブレンドはシェーダーの出力色全体にsrc.aを掛けるので、Bistroの酒瓶のように
        // 不透明度が0.04しかないマテリアルではハイライトまで1/25に潰れ、ガラスが「透明」ではなく
        // 「何も無い」ように見えてしまう。Transparent.hlsl側で拡散光にのみ不透明度を乗じ、
        // 鏡面反射は減衰させずに加算した色を出力する(詳細はdocs/Architecture.htmlの半透明描画の章を参照)
        transparentPipelineDesc.BlendMode = RHI::BlendMode::PremultipliedAlpha;
        m_TransparentPipelineState = m_Device->CreatePipelineState(transparentPipelineDesc);

        // Hi-Zミップチェーン構築パス(コンピュートシェーダー)。CSCopyでG-Buffer深度をミップ0へコピーし、
        // CSDownsampleをミップ数-1回ディスパッチして1x1まで縮小する
        RHI::ShaderDesc hizCopyCsDesc;
        hizCopyCsDesc.Stage = RHI::ShaderStage::Compute;
        hizCopyCsDesc.FilePath = shaderDirectory + L"HiZ.hlsl";
        hizCopyCsDesc.EntryPoint = "CSCopy";
        m_HiZCopyComputeShader = m_Device->CreateShader(hizCopyCsDesc);
        m_HiZCopyPipelineState = m_Device->CreateComputePipelineState({ m_HiZCopyComputeShader.get() });

        RHI::ShaderDesc hizDownsampleCsDesc;
        hizDownsampleCsDesc.Stage = RHI::ShaderStage::Compute;
        hizDownsampleCsDesc.FilePath = shaderDirectory + L"HiZ.hlsl";
        hizDownsampleCsDesc.EntryPoint = "CSDownsample";
        m_HiZDownsampleComputeShader = m_Device->CreateShader(hizDownsampleCsDesc);
        m_HiZDownsamplePipelineState = m_Device->CreateComputePipelineState({ m_HiZDownsampleComputeShader.get() });

        RHI::BufferDesc hizConstantBufferDesc;
        hizConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        hizConstantBufferDesc.SizeInBytes = sizeof(HiZConstants);
        m_HiZConstantBuffer = m_Device->CreateBuffer(hizConstantBufferDesc);

        // SSRパス(頂点バッファなしのフルスクリーン三角形。SceneColorとG-Bufferから鏡面反射を計算し加算する)
        RHI::ShaderDesc ssrVsDesc;
        ssrVsDesc.Stage = RHI::ShaderStage::Vertex;
        ssrVsDesc.FilePath = shaderDirectory + L"SSR.hlsl";
        ssrVsDesc.EntryPoint = "VSMain";
        m_SSRVertexShader = m_Device->CreateShader(ssrVsDesc);

        RHI::ShaderDesc ssrPsDesc;
        ssrPsDesc.Stage = RHI::ShaderStage::Pixel;
        ssrPsDesc.FilePath = shaderDirectory + L"SSR.hlsl";
        ssrPsDesc.EntryPoint = "PSMain";
        m_SSRPixelShader = m_Device->CreateShader(ssrPsDesc);

        RHI::PipelineStateDesc ssrPipelineDesc;
        ssrPipelineDesc.VertexShader = m_SSRVertexShader.get();
        ssrPipelineDesc.PixelShader = m_SSRPixelShader.get();
        ssrPipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
        ssrPipelineDesc.RenderTargetFormats = { RHI::Format::R16G16B16A16_Float };
        m_SSRPipelineState = m_Device->CreatePipelineState(ssrPipelineDesc);

        RHI::BufferDesc ssrConstantBufferDesc;
        ssrConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        ssrConstantBufferDesc.SizeInBytes = sizeof(SSRConstants);
        m_SSRConstantBuffer = m_Device->CreateBuffer(ssrConstantBufferDesc);

        // Tonemapパス(頂点バッファなしのフルスクリーン三角形。HDRのSceneColorをLDRへ変換する)
        RHI::ShaderDesc tonemapVsDesc;
        tonemapVsDesc.Stage = RHI::ShaderStage::Vertex;
        tonemapVsDesc.FilePath = shaderDirectory + L"Tonemap.hlsl";
        tonemapVsDesc.EntryPoint = "VSMain";
        m_TonemapVertexShader = m_Device->CreateShader(tonemapVsDesc);

        RHI::ShaderDesc tonemapPsDesc;
        tonemapPsDesc.Stage = RHI::ShaderStage::Pixel;
        tonemapPsDesc.FilePath = shaderDirectory + L"Tonemap.hlsl";
        tonemapPsDesc.EntryPoint = "PSMain";
        m_TonemapPixelShader = m_Device->CreateShader(tonemapPsDesc);

        RHI::PipelineStateDesc tonemapPipelineDesc;
        tonemapPipelineDesc.VertexShader = m_TonemapVertexShader.get();
        tonemapPipelineDesc.PixelShader = m_TonemapPixelShader.get();
        tonemapPipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
        tonemapPipelineDesc.RenderTargetFormats = { RHI::Format::R8G8B8A8_UNorm };
        m_TonemapPipelineState = m_Device->CreatePipelineState(tonemapPipelineDesc);

        // Presentパス(頂点バッファなしのフルスクリーン三角形。SceneColorをバックバッファへ拡大縮小表示)
        RHI::ShaderDesc presentVsDesc;
        presentVsDesc.Stage = RHI::ShaderStage::Vertex;
        presentVsDesc.FilePath = shaderDirectory + L"Present.hlsl";
        presentVsDesc.EntryPoint = "VSMain";
        m_PresentVertexShader = m_Device->CreateShader(presentVsDesc);

        RHI::ShaderDesc presentPsDesc;
        presentPsDesc.Stage = RHI::ShaderStage::Pixel;
        presentPsDesc.FilePath = shaderDirectory + L"Present.hlsl";
        presentPsDesc.EntryPoint = "PSMain";
        m_PresentPixelShader = m_Device->CreateShader(presentPsDesc);

        RHI::PipelineStateDesc presentPipelineDesc;
        presentPipelineDesc.VertexShader = m_PresentVertexShader.get();
        presentPipelineDesc.PixelShader = m_PresentPixelShader.get();
        presentPipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
        presentPipelineDesc.RenderTargetFormats = { RHI::Format::R8G8B8A8_UNorm };
        m_PresentPipelineState = m_Device->CreatePipelineState(presentPipelineDesc);

        // シャドウパス(ライト視点への深度のみの描画。頂点入力はPOSITIONのみ使用)
        RHI::ShaderDesc shadowVsDesc;
        shadowVsDesc.Stage = RHI::ShaderStage::Vertex;
        shadowVsDesc.FilePath = shaderDirectory + L"Shadow.hlsl";
        shadowVsDesc.EntryPoint = "VSMain";
        m_ShadowVertexShader = m_Device->CreateShader(shadowVsDesc);

        RHI::ShaderDesc shadowPsDesc;
        shadowPsDesc.Stage = RHI::ShaderStage::Pixel;
        shadowPsDesc.FilePath = shaderDirectory + L"Shadow.hlsl";
        shadowPsDesc.EntryPoint = "PSMain";
        m_ShadowPixelShader = m_Device->CreateShader(shadowPsDesc);

        const std::vector<RHI::InputElementDesc> shadowInputLayout =
        {
            { "POSITION", 0, RHI::Format::R32G32B32_Float, 0 },
        };

        RHI::PipelineStateDesc shadowPipelineDesc;
        shadowPipelineDesc.InputLayout = shadowInputLayout;
        shadowPipelineDesc.VertexShader = m_ShadowVertexShader.get();
        shadowPipelineDesc.PixelShader = m_ShadowPixelShader.get();
        shadowPipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
        shadowPipelineDesc.HasDepthStencil = true;
        m_ShadowPipelineState = m_Device->CreatePipelineState(shadowPipelineDesc);

        // シャドウマップはG-Bufferと異なりウィンドウ/レンダー解像度に依存しないため固定サイズで一度だけ作成する。
        // カスケードごとに独立したテクスチャを持つ(RHIがテクスチャ配列/DSVスライスを持たないため、
        // 既存のHi-Zミップループ等と同様に単純な配列で代替する)
        for (uint32_t cascade = 0; cascade < kCascadeCount; ++cascade)
        {
            m_ShadowCascades[cascade] = m_Device->CreateDepthTexture(kShadowMapSize, kShadowMapSize);
        }

        // 既定のスカイボックス。.ksceneの[Scene]Skyboxで差し替えられる(LoadScene参照)ため、
        // 現在読み込んでいるパスを覚えておき、同じパスなら読み直さない
        m_DefaultSkyboxPath = dataRoot + L"Assets\\Skybox\\Sky.dds";
        m_CurrentSkyboxPath = m_DefaultSkyboxPath;
        m_SkyboxTexture = m_Device->CreateTextureFromFile(m_CurrentSkyboxPath, false);

        CreateSamplerSets();

        // IBL(Image Based Lighting)の3つの畳み込み結果を保持するテクスチャと、それを生成する
        // コンピュートシェーダー一式。実際の畳み込み(スカイボックスのサンプリング)はRender()の
        // 最初のフレームで一度だけ行う(m_IBLBaked参照)。ここではリソースの作成のみ行う
        m_IrradianceTexture = m_Device->CreateUAVTextureCube(kIBLIrradianceSize, RHI::Format::R16G16B16A16_Float);
        m_PrefilteredEnvTexture = m_Device->CreateMippedUAVTextureCube(
            kIBLPrefilterBaseSize, RHI::Format::R16G16B16A16_Float, kIBLPrefilterMipLevels);
        m_BRDFLUTTexture = m_Device->CreateUAVTexture(kIBLBRDFLUTSize, kIBLBRDFLUTSize, RHI::Format::R16G16_Float);

        RHI::ShaderDesc brdfLutCsDesc;
        brdfLutCsDesc.Stage = RHI::ShaderStage::Compute;
        brdfLutCsDesc.FilePath = shaderDirectory + L"BRDFLUT.hlsl";
        brdfLutCsDesc.EntryPoint = "CSMain";
        m_BRDFLUTComputeShader = m_Device->CreateShader(brdfLutCsDesc);
        m_BRDFLUTPipelineState = m_Device->CreateComputePipelineState({ m_BRDFLUTComputeShader.get() });

        RHI::ShaderDesc irradianceCsDesc;
        irradianceCsDesc.Stage = RHI::ShaderStage::Compute;
        irradianceCsDesc.FilePath = shaderDirectory + L"IBLConvolve.hlsl";
        irradianceCsDesc.EntryPoint = "CSIrradiance";
        m_IrradianceComputeShader = m_Device->CreateShader(irradianceCsDesc);
        m_IrradiancePipelineState = m_Device->CreateComputePipelineState({ m_IrradianceComputeShader.get() });

        RHI::ShaderDesc prefilterCsDesc;
        prefilterCsDesc.Stage = RHI::ShaderStage::Compute;
        prefilterCsDesc.FilePath = shaderDirectory + L"IBLConvolve.hlsl";
        prefilterCsDesc.EntryPoint = "CSPrefilter";
        m_PrefilterComputeShader = m_Device->CreateShader(prefilterCsDesc);
        m_PrefilterPipelineState = m_Device->CreateComputePipelineState({ m_PrefilterComputeShader.get() });

        RHI::BufferDesc iblPrefilterConstantBufferDesc;
        iblPrefilterConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        iblPrefilterConstantBufferDesc.SizeInBytes = sizeof(IBLFaceConstants);
        m_IBLPrefilterConstantBuffer = m_Device->CreateBuffer(iblPrefilterConstantBufferDesc);

        // --- 反射プローブ(15章) ---
        // キャプチャ先(1面ぶんを6面で使い回す)。キューブへ写す前のHDR値を保つためFloatにする
        m_ProbeCaptureColor = m_Device->CreateRenderTexture(kProbeCaptureSize, kProbeCaptureSize, RHI::Format::R16G16B16A16_Float);
        // Reverse-Zのため遠平面側(0.0)でクリアする(G-Buffer深度と同じ)
        m_ProbeCaptureDepth = m_Device->CreateDepthTexture(kProbeCaptureSize, kProbeCaptureSize, 0.0f);
        // 畳み込みの入力になるスクラッチのキューブマップ(TextureCubeとして読めること
        // = 配列ではないことが必須。理由はヘッダのm_ProbeRadianceCubeのコメント参照)
        m_ProbeRadianceCube = m_Device->CreateUAVTextureCube(kProbeCaptureSize, RHI::Format::R16G16B16A16_Float);
        // 畳み込み結果はプローブごとに保持するためキューブマップ配列で確保する
        m_ProbeIrradianceArray = m_Device->CreateMippedUAVTextureCubeArray(
            kIBLIrradianceSize, RHI::Format::R16G16B16A16_Float, 1, kMaxReflectionProbes);
        m_ProbePrefilteredArray = m_Device->CreateMippedUAVTextureCubeArray(
            kIBLPrefilterBaseSize, RHI::Format::R16G16B16A16_Float, kIBLPrefilterMipLevels, kMaxReflectionProbes);

        RHI::ShaderDesc probeCaptureVsDesc;
        probeCaptureVsDesc.Stage = RHI::ShaderStage::Vertex;
        probeCaptureVsDesc.FilePath = shaderDirectory + L"ProbeCapture.hlsl";
        probeCaptureVsDesc.EntryPoint = "VSMain";
        m_ProbeCaptureVertexShader = m_Device->CreateShader(probeCaptureVsDesc);

        RHI::ShaderDesc probeCapturePsDesc;
        probeCapturePsDesc.Stage = RHI::ShaderStage::Pixel;
        probeCapturePsDesc.FilePath = shaderDirectory + L"ProbeCapture.hlsl";
        probeCapturePsDesc.EntryPoint = "PSMain";
        m_ProbeCapturePixelShader = m_Device->CreateShader(probeCapturePsDesc);

        RHI::PipelineStateDesc probeCapturePipelineDesc;
        probeCapturePipelineDesc.InputLayout = modelInputLayout;
        probeCapturePipelineDesc.VertexShader = m_ProbeCaptureVertexShader.get();
        probeCapturePipelineDesc.PixelShader = m_ProbeCapturePixelShader.get();
        probeCapturePipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
        probeCapturePipelineDesc.RenderTargetFormats = { RHI::Format::R16G16B16A16_Float };
        probeCapturePipelineDesc.HasDepthStencil = true;
        probeCapturePipelineDesc.ReverseZ = true;
        m_ProbeCapturePipelineState = m_Device->CreatePipelineState(probeCapturePipelineDesc);

        RHI::ShaderDesc probeCubeCopyCsDesc;
        probeCubeCopyCsDesc.Stage = RHI::ShaderStage::Compute;
        probeCubeCopyCsDesc.FilePath = shaderDirectory + L"IBLConvolve.hlsl";
        probeCubeCopyCsDesc.EntryPoint = "CSCopyCaptureToCubeFace";
        m_ProbeCubeCopyComputeShader = m_Device->CreateShader(probeCubeCopyCsDesc);
        m_ProbeCubeCopyPipelineState = m_Device->CreateComputePipelineState({ m_ProbeCubeCopyComputeShader.get() });

        // プローブの影響範囲(位置・半径)を渡すStructuredBuffer(t13)。ライトリストと同じく
        // ピクセルシェーダからは読み取り専用でよい
        RHI::BufferDesc probeBufferDesc;
        probeBufferDesc.Usage = RHI::BufferUsage::StructuredReadOnly;
        probeBufferDesc.SizeInBytes = sizeof(GPUReflectionProbe) * kMaxReflectionProbes;
        probeBufferDesc.StrideInBytes = sizeof(GPUReflectionProbe);
        m_ProbeBuffer = m_Device->CreateBuffer(probeBufferDesc);

        // キャプチャの面ごとに更新するFrameConstants(共有のm_FrameConstantBufferとは別インスタンス)
        RHI::BufferDesc probeCaptureConstantBufferDesc;
        probeCaptureConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        probeCaptureConstantBufferDesc.SizeInBytes = sizeof(FrameConstants);
        m_ProbeCaptureConstantBuffer = m_Device->CreateBuffer(probeCaptureConstantBufferDesc);

        RHI::BufferDesc constantBufferDesc;
        constantBufferDesc.Usage = RHI::BufferUsage::Constant;
        constantBufferDesc.SizeInBytes = sizeof(FrameConstants);
        m_FrameConstantBuffer = m_Device->CreateBuffer(constantBufferDesc);

        // シャドウパスはカスケードごとに異なるビュー・プロジェクション行列で同じメッシュ群を描き直すため、
        // 共有のFrameConstantsとは別に、この1個の行列だけを持つ専用バッファを使い回す
        RHI::BufferDesc cascadeConstantBufferDesc;
        cascadeConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        cascadeConstantBufferDesc.SizeInBytes = sizeof(CascadeConstants);
        m_ShadowCascadeConstantBuffer = m_Device->CreateBuffer(cascadeConstantBufferDesc);

        RHI::BufferDesc objectConstantBufferDesc;
        objectConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        objectConstantBufferDesc.SizeInBytes = sizeof(ObjectConstants);
        m_ObjectConstantBuffer = m_Device->CreateBuffer(objectConstantBufferDesc);

        // ポイント/スポットライトのリスト(t8)。CPUから毎フレーム更新するが、ピクセルシェーダから
        // 読み取り専用でよいためStructuredReadOnly(RWStructuredBufferではなくStructuredBuffer)で作成する
        RHI::BufferDesc lightBufferDesc;
        lightBufferDesc.Usage = RHI::BufferUsage::StructuredReadOnly;
        lightBufferDesc.SizeInBytes = sizeof(GPULight) * kMaxLights;
        lightBufferDesc.StrideInBytes = sizeof(GPULight);
        m_LightBuffer = m_Device->CreateBuffer(lightBufferDesc);

        RHI::BufferDesc lightingConstantBufferDesc;
        lightingConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        lightingConstantBufferDesc.SizeInBytes = sizeof(LightingConstants);
        m_LightingConstantBuffer = m_Device->CreateBuffer(lightingConstantBufferDesc);

        RHI::BufferDesc presentConstantBufferDesc;
        presentConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        presentConstantBufferDesc.SizeInBytes = sizeof(PresentConstants);
        m_PresentConstantBuffer = m_Device->CreateBuffer(presentConstantBufferDesc);

        CreateRenderTargets(m_RenderWidth, m_RenderHeight);

        DiscoverScenes();
        LoadScene(0);
    }

    void KurenaiEngine3D::DiscoverScenes()
    {
        const std::wstring sceneDirectory = GetModuleDirectory() + L"Assets\\Scenes\\";

        std::vector<std::wstring> fileNames;
        WIN32_FIND_DATAW findData{};
        HANDLE findHandle = FindFirstFileW((sceneDirectory + L"*.kscene").c_str(), &findData);
        if (findHandle != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                {
                    fileNames.push_back(findData.cFileName);
                }
            } while (FindNextFileW(findHandle, &findData));
            FindClose(findHandle);
        }

        // ImGuiのシーン一覧・LoadSceneのインデックスをビルドのたびに変わらないようにする
        std::sort(fileNames.begin(), fileNames.end(), [](const std::wstring& a, const std::wstring& b)
        {
            return _wcsicmp(a.c_str(), b.c_str()) < 0;
        });

        m_SceneFilePaths.clear();
        m_SceneDisplayNames.clear();
        for (const std::wstring& fileName : fileNames)
        {
            const std::wstring fullPath = sceneDirectory + fileName;
            try
            {
                m_SceneDisplayNames.push_back(Assets::ReadSceneName(fullPath));
                m_SceneFilePaths.push_back(fullPath);
            }
            catch (const std::exception& e)
            {
                // 1ファイルの不備でアプリ全体が起動できなくなるのを避け、そのファイルだけ除外して続行する
                Core::Logger::Error("KurenaiEngine3D", "シーンファイルの読み込みに失敗したため一覧から除外します (" + WideToUtf8(fullPath) + "): " + e.what());
            }
        }

        if (m_SceneFilePaths.empty())
        {
            const std::string message = "有効なシーンファイル(.kscene)が見つかりませんでした: " + WideToUtf8(sceneDirectory);
            Core::Logger::Error("KurenaiEngine3D", message);
            throw std::runtime_error(message);
        }
    }

    void KurenaiEngine3D::CreateSamplerSets()
    {
        // スロットの並びはShaders/3D/Samplers.hlsliの役割定義と一致させること
        // (s0 = MaterialSampler、s1 = ColorSampler、s2 = DataSampler)。

        // 色バッファ・LUT用。UVの端が定義域の端なのでClamp、拡縮でブロック状にならないようLinear。
        // BRDF積分LUTをWrapで引いて実際に不具合を出した経緯はdocs/Architecture.html 14.2.1節
        RHI::SamplerDesc colorSampler{};
        colorSampler.Filter = RHI::SamplerFilter::Linear;
        colorSampler.AddressMode = RHI::SamplerAddressMode::Clamp;

        // 深度・エンコード法線・metallic/roughness・シャドウマップ用。
        // 補間するとシルエット跨ぎで実在しない値になるためPoint、
        // カーネルのタップが[0,1]を出たときに反対側の端を読まないためClamp
        RHI::SamplerDesc dataSampler{};
        dataSampler.Filter = RHI::SamplerFilter::Point;
        dataSampler.AddressMode = RHI::SamplerAddressMode::Clamp;

        // マテリアル用。タイリング前提のWrapと、浅い角度で見る床・路面のボケを抑える異方性16x
        RHI::SamplerDesc materialSampler{};
        materialSampler.Filter = RHI::SamplerFilter::Anisotropic;
        materialSampler.AddressMode = RHI::SamplerAddressMode::Wrap;

        const RHI::SamplerDesc materialSet[] = { materialSampler, colorSampler, dataSampler };
        m_MaterialSamplers = m_Device->CreateSamplerSet(materialSet, static_cast<uint32_t>(std::size(materialSet)));

        // スクリーン空間パスは画面内の中間バッファしか読まないため、s0にもWrapを置かない。
        // 万一シェーダ側で役割を選び違えても、画面端でUVが反対側へ回り込む不具合が起きないようにする
        const RHI::SamplerDesc screenSpaceSet[] = { colorSampler, colorSampler, dataSampler };
        m_ScreenSpaceSamplers = m_Device->CreateSamplerSet(screenSpaceSet, static_cast<uint32_t>(std::size(screenSpaceSet)));
    }

    void KurenaiEngine3D::CreateRenderTargets(uint32_t width, uint32_t height)
    {
        if (width == 0 || height == 0)
        {
            return;
        }

        m_GBufferAlbedo = m_Device->CreateRenderTexture(width, height, RHI::Format::R8G8B8A8_UNorm);
        m_GBufferNormal = m_Device->CreateRenderTexture(width, height, RHI::Format::R16G16_Float);
        m_GBufferMaterial = m_Device->CreateRenderTexture(width, height, RHI::Format::R8G8B8A8_UNorm);
        m_GBufferEmissive = m_Device->CreateRenderTexture(width, height, RHI::Format::R8G8B8A8_UNorm);
        // Reverse-Zのため近平面側(NDC z=1.0)ではなく遠平面側(NDC z=0.0)にクリアする
        m_GBufferDepth = m_Device->CreateDepthTexture(width, height, 0.0f);
        m_DirectLightTexture = m_Device->CreateRenderTexture(width, height, RHI::Format::R32G32B32A32_Float);
        m_SSAORawTexture = m_Device->CreateRenderTexture(width, height, RHI::Format::R8G8B8A8_UNorm);
        m_SSAOTexture = m_Device->CreateRenderTexture(width, height, RHI::Format::R8G8B8A8_UNorm);
        m_SSILRawTexture = m_Device->CreateRenderTexture(width, height, RHI::Format::R8G8B8A8_UNorm);
        m_SSILTexture = m_Device->CreateRenderTexture(width, height, RHI::Format::R8G8B8A8_UNorm);
        m_SceneColor = m_Device->CreateRenderTexture(width, height, RHI::Format::R16G16B16A16_Float);
        m_SSRTexture = m_Device->CreateRenderTexture(width, height, RHI::Format::R16G16B16A16_Float);
        m_TonemapTexture = m_Device->CreateRenderTexture(width, height, RHI::Format::R8G8B8A8_UNorm);

        m_HiZMipLevels = ComputeMipLevelCount(width, height);
        m_HiZTexture = m_Device->CreateHiZTexture(width, height, m_HiZMipLevels);
        m_HiZDebugMipLevel = 0;
    }

    void KurenaiEngine3D::LoadScene(size_t sceneIndex)
    {
        if (sceneIndex >= m_SceneFilePaths.size())
        {
            return;
        }

        // m_Scene/m_Camera/Post ProcessingパラメータはRender()(Renderスレッド)も読み書きするため、
        // この関数全体をm_SceneMutexで保護する(詳細はm_SceneMutexのコメント参照)。UpdateSceneSwitch
        // (Updateスレッド)からのみ呼ばれる前提のため、Renderスレッドとの競合はこれで排他できる
        std::lock_guard<std::mutex> sceneLock(m_SceneMutex);

        // [Model]Pathの基準ディレクトリ(Assetsルート)。.kmodel自身の内部パス(.kmodelがある
        // ディレクトリからの相対)とは基準が異なる点に注意(SceneLoader.h参照)
        const std::wstring assetRootDirectory = GetModuleDirectory() + L"Assets\\";

        // 旧シーン(m_Scene)のバッファ/テクスチャを破棄する前に、GPUが旧シーンを参照する
        // コマンド(直前まで提出されていた描画コマンド)の実行を終えるまで待つ。特にDX12は
        // CPUがGPU完了を待たずに次フレームの記録を始める多重バッファリング設計のため、
        // これを省くとGPUがまだ読んでいるバッファ/テクスチャを解放してしまい、
        // ヒープ破損によるクラッシュを引き起こす(詳細はIRHIDevice::WaitForGPUIdleのコメント参照)
        m_Device->WaitForGPUIdle();

        // Assets::LoadSceneの戻り値(新シーンの全テクスチャ/バッファ)を作り終えてから代入すると、
        // 代入演算子が旧m_Sceneを破棄するまでの間、新旧シーンのGPUリソース(特にDX12の
        // 非シェーダー可視SRVディスクリプタ)が同時に確保された状態になり、大規模シーンでは
        // ディスクリプタヒープを圧迫する。先に空のSceneで置き換えて旧シーンを解放しておく
        // (直前のWaitForGPUIdleによりGPUはもう旧シーンを参照していないため安全)
        m_Scene = Assets::Scene{};
        m_Scene = Assets::LoadScene(*m_Device, m_SceneFilePaths[sceneIndex], assetRootDirectory);
        m_CurrentSceneIndex = sceneIndex;

        // [Sun]/[Camera]セクションが無いシーンでは、Sceneの側でこのメンバの既定値
        // (従来のKurenaiEngine3Dの初期値と同じ)が使われるため、常にそのまま反映してよい
        m_TimeOfDay = m_Scene.SunTimeOfDay;
        m_SunAzimuthDegrees = m_Scene.SunAzimuthDegrees;
        m_ShadowEnabled = m_Scene.ShadowEnabled;
        m_SunEnabled = m_Scene.SunEnabled;
        m_AOEnabled = m_Scene.AOEnabled;
        m_SSREnabled = m_Scene.SSREnabled;
        if (m_Scene.HasIBLIntensityOverride)
        {
            m_IBLIntensity = m_Scene.IBLIntensity;
        }

        // [Scene]Skyboxでスカイボックスを差し替える(指定が無ければ既定へ戻す)。
        // IBLの拡散イラディアンス・プリフィルタ済み鏡面はスカイボックスから焼かれるため、
        // 差し替えたらm_IBLBakedを倒して次フレームで焼き直させる必要がある。
        // 直前にWaitForGPUIdle済みなので旧テクスチャを解放しても安全
        const std::wstring desiredSkyboxPath = m_Scene.SkyboxPath.empty() ? m_DefaultSkyboxPath : m_Scene.SkyboxPath;
        if (desiredSkyboxPath != m_CurrentSkyboxPath)
        {
            try
            {
                m_SkyboxTexture = m_Device->CreateTextureFromFile(desiredSkyboxPath, false);
                m_CurrentSkyboxPath = desiredSkyboxPath;
                m_IBLBaked = false;
                Core::Logger::Info("KurenaiEngine3D", "スカイボックスを差し替えました: " + WideToUtf8(desiredSkyboxPath));
            }
            catch (const std::exception& e)
            {
                // 読み込みに失敗しても現在のスカイボックスのまま描画を続ける(シーン切り替え自体は成立させる)
                Core::Logger::Error(
                    "KurenaiEngine3D",
                    "スカイボックスの読み込みに失敗しました。現在のスカイボックスを維持します: " +
                        WideToUtf8(desiredSkyboxPath) + " : " + e.what());
            }
        }

        // アセット由来のライトをユーザー編集用のコピーへ複製する(m_Scene.Lightsは直接編集しない。
        // シーンを再読み込みすればアセット既定値に戻るようにするため)。m_Scene.Lightsは
        // SceneLoaderが各ModelInstanceのModel::Lightsをワールド空間へ変換し、.kscene自身の
        // [Light]セクションのライトと合成済みのシーン全体のライト一覧(Scene.h参照)
        m_Lights = m_Scene.Lights;
        m_SelectedLightIndex = m_Lights.empty() ? -1 : 0;
        m_LightOverflowLogged = false;

        // 反射プローブもライトと同じ方針でユーザー編集用のコピーへ複製する。
        // プローブの中身(キューブマップ)はシーンのジオメトリ・ライトに依存するため、
        // シーンを読み込んだら必ず焼き直す必要がある
        m_ReflectionProbes = m_Scene.ReflectionProbes;
        if (m_ReflectionProbes.size() > kMaxReflectionProbes)
        {
            Core::Logger::Warning(
                "KurenaiEngine3D",
                "反射プローブ数が上限(" + std::to_string(kMaxReflectionProbes) + ")を超えたため、先頭から" +
                    std::to_string(kMaxReflectionProbes) + "個のみ使用します: " + std::to_string(m_ReflectionProbes.size()) + "個");
            m_ReflectionProbes.resize(kMaxReflectionProbes);
        }
        m_SelectedProbeIndex = m_ReflectionProbes.empty() ? -1 : 0;
        m_ProbeDebugIndex = 0;
        m_ProbeBaked = false;
        m_ProbeBakeRequested = !m_ReflectionProbes.empty();

        FrameCameraToModel();

        const wchar_t* apiName = (m_GraphicsAPI == GraphicsAPI::DX12) ? L"DX12" : L"DX11";
        m_Window->SetTitle(std::wstring(L"Kurenai Engine [") + apiName + L"] - " + m_Scene.Name);
    }

    void KurenaiEngine3D::FrameCameraToModel()
    {
        const float sizeY = m_Scene.BoundsMax[1] - m_Scene.BoundsMin[1];
        const float dx = m_Scene.BoundsMax[0] - m_Scene.BoundsMin[0];
        const float dz = m_Scene.BoundsMax[2] - m_Scene.BoundsMin[2];
        const float diagonal = std::sqrt(dx * dx + sizeY * sizeY + dz * dz);

        // SSAO/SSILのサンプリング半径はシーンの規模に応じて変わるべきなので、対角線に比例させる
        // (小さすぎる/大きすぎるシーンでも遮蔽表現が破綻しないよう妥当な範囲にクランプする)
        m_SSAORadius = std::clamp(diagonal * 0.01f, 0.05f, 2.0f);
        m_SSILRadius = m_SSAORadius;
        m_SSILThickness = m_SSILRadius * 0.2f;

        // SSRの最大レイ距離もシーンの規模に応じて変わるべきなので、対角線に比例させる。
        // ヒット判定の厚みはSSAO/SSILと同様、遮蔽・接触判定として妥当な小さい値にする
        m_SSRMaxDistance = std::clamp(diagonal * 0.5f, 1.0f, 100.0f);
        m_SSRThickness = m_SSAORadius * 0.2f;

        if (m_Scene.HasCameraOverride)
        {
            m_Camera.SetPosition({ m_Scene.CameraPosition[0], m_Scene.CameraPosition[1], m_Scene.CameraPosition[2] });
            m_Camera.SetYawPitch(m_Scene.CameraYaw, m_Scene.CameraPitch);
            m_Camera.SetLens(DirectX::XM_PIDIV4, std::max(0.01f, diagonal * 0.0005f), std::max(100.0f, diagonal * 4.0f));
            return;
        }

        const float centerX = (m_Scene.BoundsMin[0] + m_Scene.BoundsMax[0]) * 0.5f;
        const float centerY = (m_Scene.BoundsMin[1] + m_Scene.BoundsMax[1]) * 0.5f;
        const float centerZ = (m_Scene.BoundsMin[2] + m_Scene.BoundsMax[2]) * 0.5f;
        const float eyeHeight = m_Scene.BoundsMin[1] + sizeY * 0.15f;

        const float longAxis = std::max(dx, dz);
        const float shortAxis = std::min(dx, dz);
        // 短辺が長辺に対して極端に短い場合は、歩いて回れる建物内部ではなく横に並んだ物体と判断し、
        // 内部に入り込む配置ではなく外側から全体を見渡す配置にする
        const bool isThinProp = shortAxis < longAxis * 0.15f;

        float posX;
        float posY;
        float posZ;
        float yaw;
        float nearZ;
        const float farZ = std::max(100.0f, diagonal * 4.0f);

        if (isThinProp)
        {
            // 縦FOVの半角のtanを使い、アスペクト比に依らず長辺全体が収まる距離を保守的に求める
            const float halfFovTan = std::tan(DirectX::XM_PIDIV4 * 0.5f);
            const float requiredDistance = (longAxis * 0.5f) / halfFovTan * 1.25f;

            posX = centerX;
            posY = centerY;
            posZ = centerZ + requiredDistance;
            yaw = DirectX::XM_PI;

            // カメラは物体から離れた位置にあるため、near平面をdiagonal基準の極小値のままにすると
            // 深度バッファの精度が視距離全体で失われてしまう(near:distance比が極端になるため)。
            // 実際の視距離に応じたスケールにして深度精度を確保する
            nearZ = std::max(0.05f, requiredDistance * 0.02f);
        }
        else if (dx >= dz)
        {
            // ホールの長辺方向の端寄りから中心を見る位置を初期視点にする(中央の装飾物や壁に埋まらないように)
            posX = m_Scene.BoundsMin[0] + dx * 0.2f;
            posY = eyeHeight;
            posZ = centerZ;
            yaw = DirectX::XM_PIDIV2;
            nearZ = std::max(0.01f, diagonal * 0.0005f);
        }
        else
        {
            posX = centerX;
            posY = eyeHeight;
            posZ = m_Scene.BoundsMin[2] + dz * 0.2f;
            yaw = 0.0f;
            nearZ = std::max(0.01f, diagonal * 0.0005f);
        }

        m_Camera.SetPosition({ posX, posY, posZ });
        m_Camera.SetYawPitch(yaw, 0.0f);
        m_Camera.SetLens(DirectX::XM_PIDIV4, nearZ, farZ);
    }

    // カメラ視錐台をkCascadeCount個の深度範囲に分割する境界(View空間でのカメラからの距離)を求める。
    // 対数分割(遠くのカスケードほど急激に広がる。人間の目の距離知覚・遠近感に合う)と均等分割
    // (どのカスケードも同じ奥行きを持つ)を按分するPractical Split Scheme(GPU Gems 3, Dimitrov 2007)を使う。
    // 対数分割のみだと手前のカスケードが極端に狭くなり、均等分割のみだと遠方のテクセル密度が
    // 不足するため、両者を混ぜることで手前の精度と遠方のカバレッジを両立する
    void KurenaiEngine3D::ComputeCascadeSplits(const Core::Camera& camera, float (&outSplits)[kCascadeCount]) const
    {
        const float nearZ = camera.GetNearZ();
        const float farZ = camera.GetFarZ();
        const float lambda = 0.75f;

        for (uint32_t i = 0; i < kCascadeCount; ++i)
        {
            const float p = static_cast<float>(i + 1) / static_cast<float>(kCascadeCount);
            const float logSplit = nearZ * std::pow(farZ / nearZ, p);
            const float uniformSplit = nearZ + (farZ - nearZ) * p;
            outSplits[i] = lambda * logSplit + (1.0f - lambda) * uniformSplit;
        }
    }

    // 平行光のライト視点から、カメラ視錐台のうち[splitNear, splitFar]の範囲(View空間距離)だけを
    // 覆う正射影のビュー・プロジェクション行列を求める(カスケードシャドウマップの1カスケード分)。
    // その深度範囲の視錐台スライスの8頂点を求め、外接球を基準にライト視点を配置する
    DirectX::XMMATRIX KurenaiEngine3D::ComputeCascadeLightViewProj(
        const DirectX::XMFLOAT3& lightDirection, const Core::Camera& camera, float splitNear, float splitFar) const
    {
        using namespace DirectX;

        const XMFLOAT3 positionF = camera.GetPosition();
        const XMFLOAT3 forwardF = camera.GetForward();
        const XMFLOAT3 rightF = camera.GetRight();
        const XMVECTOR position = XMLoadFloat3(&positionF);
        const XMVECTOR forward = XMLoadFloat3(&forwardF);
        const XMVECTOR right = XMLoadFloat3(&rightF);
        const XMVECTOR camUp = XMVector3Normalize(XMVector3Cross(right, forward));

        const float tanHalfFovY = std::tan(camera.GetFovY() * 0.5f);
        const float aspect = camera.GetAspectRatio();

        // splitNear/splitFarそれぞれの断面の4隅(ワールド座標)を求め、視錐台スライスの8頂点とする
        XMVECTOR corners[8];
        int cornerIndex = 0;
        for (const float dist : { splitNear, splitFar })
        {
            const float halfHeight = dist * tanHalfFovY;
            const float halfWidth = halfHeight * aspect;
            const XMVECTOR centerAtDist = XMVectorAdd(position, XMVectorScale(forward, dist));
            for (const float sy : { -1.0f, 1.0f })
            {
                for (const float sx : { -1.0f, 1.0f })
                {
                    corners[cornerIndex++] = XMVectorAdd(
                        centerAtDist,
                        XMVectorAdd(XMVectorScale(right, halfWidth * sx), XMVectorScale(camUp, halfHeight * sy)));
                }
            }
        }

        // 8頂点の外接球を使う(タイトなAABBだとカメラの向きによって毎フレーム形が変わり、
        // シャドウマップの見かけのサイズが揺れてちらつく。半径ベースにすることで回転に対して安定する)
        XMVECTOR centerSum = XMVectorZero();
        for (const XMVECTOR& corner : corners)
        {
            centerSum = XMVectorAdd(centerSum, corner);
        }
        const XMVECTOR sphereCenter = XMVectorScale(centerSum, 1.0f / 8.0f);

        float sphereRadius = 0.01f;
        for (const XMVECTOR& corner : corners)
        {
            sphereRadius = std::max(sphereRadius, XMVectorGetX(XMVector3Length(XMVectorSubtract(corner, sphereCenter))));
        }

        const XMVECTOR lightDirVec = XMVector3Normalize(XMLoadFloat3(&lightDirection));

        // ライト方向がほぼ真上/真下(upベクトルと平行)だとLookAt行列が縮退するため、そのときだけ別軸を使う
        XMVECTOR lightUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        if (std::abs(XMVectorGetY(lightDirVec)) > 0.99f)
        {
            lightUp = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
        }

        // シャドウマップのテクセル単位に中心位置をスナップし、カメラが動いた際にシャドウの縁が
        // 1テクセル未満の単位でちらつく(シャドウシマー)のを抑える。ライトの「向き」だけを持つ
        // (平行移動のない)基準行列でワールド座標をライト空間へ変換してからテクセル単位に丸め、
        // 再度ワールド空間へ戻す。フレームごとに視点位置から作り直す行列を直接使うと、常に
        // 中心が原点近辺の値になってしまい意味がないため、この向きだけの基準行列を使う
        const XMMATRIX lightRotation = XMMatrixLookAtLH(XMVectorZero(), lightDirVec, lightUp);
        const float orthoSize = sphereRadius * 2.0f;
        const float texelSize = orthoSize / static_cast<float>(kShadowMapSize);

        XMFLOAT3 centerLightSpace;
        XMStoreFloat3(&centerLightSpace, XMVector3TransformCoord(sphereCenter, lightRotation));
        centerLightSpace.x = std::floor(centerLightSpace.x / texelSize) * texelSize;
        centerLightSpace.y = std::floor(centerLightSpace.y / texelSize) * texelSize;

        const XMMATRIX lightRotationInv = XMMatrixInverse(nullptr, lightRotation);
        const XMVECTOR snappedCenter = XMVector3TransformCoord(XMLoadFloat3(&centerLightSpace), lightRotationInv);

        // ライトが進む方向と逆側に球の半径分だけ余裕を持って離れた位置に仮想的なライトカメラを置く
        const float margin = 1.5f;
        const XMVECTOR eye = XMVectorSubtract(snappedCenter, XMVectorScale(lightDirVec, sphereRadius * margin));
        const XMMATRIX lightView = XMMatrixLookAtLH(eye, snappedCenter, lightUp);

        const float nearZ = 0.1f;
        const float farZ = sphereRadius * margin * 2.0f + sphereRadius;
        const XMMATRIX lightProj = XMMatrixOrthographicLH(orthoSize, orthoSize, nearZ, farZ);

        return lightView * lightProj;
    }

    void KurenaiEngine3D::RenderSceneSwitchUI()
    {
        ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(260.0f, 0.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("Scenes");

        ImGui::TextUnformatted(m_GraphicsAPI == GraphicsAPI::DX12 ? "Graphics API: DX12" : "Graphics API: DX11");
        ImGui::Separator();

        for (size_t i = 0; i < m_SceneDisplayNames.size(); ++i)
        {
            const bool isCurrent = (i == m_CurrentSceneIndex);
            if (isCurrent)
            {
                ImGui::BeginDisabled();
            }

            const std::string label = WideToUtf8(m_SceneDisplayNames[i]);
            if (ImGui::Button(label.c_str(), ImVec2(-FLT_MIN, 0.0f)))
            {
                // LoadScene自体はUpdateスレッドから呼ぶ必要があるため、ここでは要求を書き込むだけにする
                // (UpdateSceneSwitch参照)
                m_PendingSceneIndex.store(static_cast<int>(i));
            }

            if (isCurrent)
            {
                ImGui::EndDisabled();
            }
        }

        ImGui::End();
    }

    void KurenaiEngine3D::RenderPostProcessUI()
    {
        ImGui::SetNextWindowPos(ImVec2(10.0f, 280.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(260.0f, 0.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("Post Processing");

        ImGui::Checkbox("Enable AO / Indirect Light", &m_AOEnabled);
        if (m_AOEnabled)
        {
            static const char* kAOTechniqueNames[] = { "SSAO", "SSIL (Visibility Bitmask)" };
            int techniqueIndex = static_cast<int>(m_AOTechnique);
            if (ImGui::Combo("Technique", &techniqueIndex, kAOTechniqueNames, IM_ARRAYSIZE(kAOTechniqueNames)))
            {
                m_AOTechnique = static_cast<AOTechnique>(techniqueIndex);
            }

            if (m_AOTechnique == AOTechnique::SSAO)
            {
                ImGui::SliderFloat("SSAO Radius", &m_SSAORadius, 0.01f, 5.0f);
                ImGui::SliderFloat("SSAO Power", &m_SSAOPower, 0.1f, 4.0f);
            }
            else
            {
                ImGui::SliderFloat("SSIL Radius", &m_SSILRadius, 0.01f, 5.0f);
                ImGui::SliderFloat("SSIL Thickness", &m_SSILThickness, 0.01f, 2.0f);
                ImGui::SliderFloat("SSIL Intensity", &m_SSILIntensity, 0.0f, 8.0f);
                ImGui::SliderFloat("SSIL AO Power", &m_SSILPower, 0.1f, 4.0f);

                int sliceCount = static_cast<int>(m_SSILSliceCount);
                if (ImGui::SliderInt("SSIL Slices", &sliceCount, 1, 8))
                {
                    m_SSILSliceCount = static_cast<uint32_t>(sliceCount);
                }

                int stepCount = static_cast<int>(m_SSILStepCount);
                if (ImGui::SliderInt("SSIL Steps", &stepCount, 1, 16))
                {
                    m_SSILStepCount = static_cast<uint32_t>(stepCount);
                }
            }
        }

        ImGui::Checkbox("Enable Shadow", &m_ShadowEnabled);
        if (m_ShadowEnabled)
        {
            ImGui::SliderFloat("Shadow Light Size", &m_ShadowLightSize, 0.001f, 0.05f, "%.4f");
        }

        ImGui::Checkbox("Enable IBL", &m_IBLEnabled);
        if (m_IBLEnabled)
        {
            ImGui::SliderFloat("IBL Intensity", &m_IBLIntensity, 0.0f, 2.0f);
        }
        else
        {
            ImGui::SliderFloat("Ambient Scale", &m_AmbientScale, 0.0f, 3.0f);
        }

        // スペキュラのマルチスキャッタリング・エネルギー補正(14.9節)。IBL鏡面・直接光鏡面の
        // 両方に効くため、Enable IBLのブロックの内側ではなく独立したチェックボックスにする
        ImGui::Checkbox("Specular Energy Compensation", &m_SpecularEnergyCompensationEnabled);

        ImGui::Checkbox("Enable VSync", &m_VSyncEnabled);

        ImGui::Checkbox("Fixed FPS", &m_FixedFPSEnabled);
        if (m_FixedFPSEnabled)
        {
            static const char* kTargetFPSNames[] = { "30", "60", "120" };
            static const float kTargetFPSValues[] = { 30.0f, 60.0f, 120.0f };
            int targetFPSIndex = 1; // 見つからない場合は60fps相当の位置にしておく
            for (int i = 0; i < IM_ARRAYSIZE(kTargetFPSValues); ++i)
            {
                if (kTargetFPSValues[i] == m_TargetFPS)
                {
                    targetFPSIndex = i;
                    break;
                }
            }
            if (ImGui::Combo("Target FPS", &targetFPSIndex, kTargetFPSNames, IM_ARRAYSIZE(kTargetFPSNames)))
            {
                m_TargetFPS = kTargetFPSValues[targetFPSIndex];
            }
        }

        ImGui::Checkbox("Enable SSR", &m_SSREnabled);
        if (m_SSREnabled)
        {
            ImGui::SliderFloat("SSR Max Distance", &m_SSRMaxDistance, 0.1f, 100.0f);
            ImGui::SliderFloat("SSR Thickness", &m_SSRThickness, 0.01f, 2.0f);
            ImGui::SliderFloat("SSR Roughness Cutoff", &m_SSRRoughnessCutoff, 0.05f, 1.0f);
        }

        ImGui::End();
    }

    void KurenaiEngine3D::RenderDebugViewUI()
    {
        // Post Processingパネル(Shadow Light Size追加分含む)が伸びた際に重ならないよう、
        // 十分な余白を空けた位置を既定にする
        ImGui::SetNextWindowPos(ImVec2(10.0f, 620.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(260.0f, 0.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("Render Targets");

        static const char* kDebugViewNames[] =
        {
            "Final (Lit)",
            "Albedo",
            "Normal",
            "Material (R=Metallic, G=Roughness)",
            "Emissive",
            "Depth",
            "Depth (Raw)",
            "Direct Light",
            "AO/GI - Indirect Light (RGB)",
            "AO/GI - Indirect Light (RGB, Before Blur)",
            "AO/GI - Occlusion (Alpha)",
            "AO/GI - Occlusion (Alpha, Before Blur)",
            "Shadow Map",
            "SSR (Final + Reflections)",
            "Hi-Z (Depth Mip Chain)",
            "IBL - Irradiance (Cubemap, Look Around)",
            "IBL - Prefiltered Specular (Cubemap Mip Chain, Look Around)",
            "IBL - BRDF LUT (X=NdotV, Y=Roughness)",
            "Probe - Irradiance (Cubemap Array, Look Around)",
            "Probe - Prefiltered Specular (Mip 0 = Raw Capture)",
            "Probe - Influence (Color per Probe)",
        };

        int currentIndex = static_cast<int>(m_DebugView);
        if (ImGui::Combo("View", &currentIndex, kDebugViewNames, IM_ARRAYSIZE(kDebugViewNames)))
        {
            m_DebugView = static_cast<DebugView>(currentIndex);
        }

        if (m_DebugView == DebugView::HiZ)
        {
            ImGui::SliderInt("Hi-Z Mip Level", &m_HiZDebugMipLevel, 0, static_cast<int>(m_HiZMipLevels) - 1);
        }

        if (m_DebugView == DebugView::ShadowMap)
        {
            ImGui::SliderInt("Shadow Cascade", &m_ShadowDebugCascade, 0, static_cast<int>(kCascadeCount) - 1);
        }

        if (m_DebugView == DebugView::IBLPrefilter)
        {
            ImGui::SliderInt("Prefilter Mip Level", &m_IBLPrefilterDebugMipLevel, 0, static_cast<int>(kIBLPrefilterMipLevels) - 1);
        }

        if (m_DebugView == DebugView::ProbeIrradiance || m_DebugView == DebugView::ProbePrefilter)
        {
            // プローブが1つも無いシーンでもスライダーの範囲が壊れないよう下限を0に保つ
            const int maxProbeIndex = m_ReflectionProbes.empty() ? 0 : static_cast<int>(m_ReflectionProbes.size()) - 1;
            ImGui::SliderInt("Probe Index", &m_ProbeDebugIndex, 0, maxProbeIndex);
            if (m_DebugView == DebugView::ProbePrefilter)
            {
                ImGui::SliderInt(
                    "Probe Prefilter Mip", &m_ProbePrefilterDebugMipLevel, 0, static_cast<int>(kIBLPrefilterMipLevels) - 1);
            }
        }

        ImGui::End();
    }

    void KurenaiEngine3D::RenderLightingUI(const FrameState& frameState)
    {
        // ライトを選択して全項目を開くと縦に長くなるため、Profiler(280,490に移動済み)と
        // 重ならないよう十分な高さを確保しておく
        ImGui::SetNextWindowPos(ImVec2(280.0f, 10.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(300.0f, 470.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("Lighting");

        if (ImGui::CollapsingHeader("Sun", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::SliderFloat("Time of Day", &m_TimeOfDay, 0.0f, 24.0f, "%.2f h");
            ImGui::Checkbox("Auto Advance", &m_TimeAutoAdvance);
            if (m_TimeAutoAdvance)
            {
                ImGui::SliderFloat("Speed", &m_TimeAdvanceSpeed, 0.1f, 10.0f, "%.1f h/s");
            }
            ImGui::SliderFloat("Sun Azimuth", &m_SunAzimuthDegrees, 0.0f, 360.0f, "%.1f deg");
            // 太陽だけを消して環境光のみで照らす状態を作る(White Furnace Testが使う)。
            // TimeOfDayを夜にする方法と違い、昼度(環境光の明るさ)は下がらない
            ImGui::Checkbox("Enable Sun", &m_SunEnabled);
            // 太陽・環境光・下記ポイント/スポットライトすべてに一様にかかるシーン全体の露出
            // (実在の写真露出値EV100)。屋内シーンでは実カメラと同様に下げて調整する運用になる
            ImGui::SliderFloat("EV100", &m_SceneExposureEV100, -8.0f, 20.0f, "%.2f");
        }

        if (ImGui::CollapsingHeader("Lights", ImGuiTreeNodeFlags_DefaultOpen))
        {
            uint32_t activeCount = 0;
            for (const Assets::Light& light : m_Lights)
            {
                if (light.Enabled)
                {
                    ++activeCount;
                }
            }
            ImGui::Text("Active: %u / %zu", activeCount, m_Lights.size());

            ImGui::BeginChild("LightList", ImVec2(0.0f, 90.0f), true);
            for (size_t i = 0; i < m_Lights.size(); ++i)
            {
                ImGui::PushID(static_cast<int>(i));
                ImGui::Checkbox("##enabled", &m_Lights[i].Enabled);
                ImGui::SameLine();
                const char* typeLabel = m_Lights[i].Type == Assets::LightType::Directional ? "Directional"
                                       : m_Lights[i].Type == Assets::LightType::Spot ? "Spot" : "Point";
                char label[192];
                std::snprintf(
                    label, sizeof(label), "[%s] %s", typeLabel, m_Lights[i].Name.empty() ? "(no name)" : m_Lights[i].Name.c_str());
                if (ImGui::Selectable(label, m_SelectedLightIndex == static_cast<int>(i)))
                {
                    m_SelectedLightIndex = static_cast<int>(i);
                }
                ImGui::PopID();
            }
            ImGui::EndChild();

            if (ImGui::Button("Add"))
            {
                Assets::Light newLight;
                const DirectX::XMFLOAT3 cameraPosition = frameState.Camera.GetPosition();
                newLight.Position[0] = cameraPosition.x;
                newLight.Position[1] = cameraPosition.y;
                newLight.Position[2] = cameraPosition.z;
                newLight.Name = "New Light";
                m_Lights.push_back(newLight);
                m_SelectedLightIndex = static_cast<int>(m_Lights.size()) - 1;
            }

            const bool hasSelection = m_SelectedLightIndex >= 0 && m_SelectedLightIndex < static_cast<int>(m_Lights.size());

            ImGui::SameLine();
            ImGui::BeginDisabled(!hasSelection);
            if (ImGui::Button("Duplicate") && hasSelection)
            {
                Assets::Light duplicated = m_Lights[static_cast<size_t>(m_SelectedLightIndex)];
                duplicated.Name += " (Copy)";
                m_Lights.push_back(duplicated);
                m_SelectedLightIndex = static_cast<int>(m_Lights.size()) - 1;
            }
            ImGui::SameLine();
            if (ImGui::Button("Remove") && hasSelection)
            {
                m_Lights.erase(m_Lights.begin() + m_SelectedLightIndex);
                m_SelectedLightIndex =
                    m_Lights.empty() ? -1 : std::min(m_SelectedLightIndex, static_cast<int>(m_Lights.size()) - 1);
            }
            ImGui::EndDisabled();

            if (hasSelection)
            {
                Assets::Light& light = m_Lights[static_cast<size_t>(m_SelectedLightIndex)];

                char nameBuffer[128];
                std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", light.Name.c_str());
                if (ImGui::InputText("Name", nameBuffer, sizeof(nameBuffer)))
                {
                    light.Name = nameBuffer;
                }

                int typeIndex = static_cast<int>(light.Type);
                const char* typeItems[] = { "Directional", "Point", "Spot" };
                if (ImGui::Combo("Type", &typeIndex, typeItems, IM_ARRAYSIZE(typeItems)))
                {
                    light.Type = static_cast<Assets::LightType>(typeIndex);
                }

                ImGui::ColorEdit3("Color", light.Color);
                ImGui::SliderFloat(
                    "Intensity (cd/lx)", &light.Intensity, 0.01f, 1000000.0f, "%.2f", ImGuiSliderFlags_Logarithmic);

                if (light.Type != Assets::LightType::Directional)
                {
                    ImGui::DragFloat3("Position", light.Position, 0.1f);
                    ImGui::SliderFloat("Range", &light.Range, 0.1f, 500.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
                }

                if (light.Type != Assets::LightType::Point)
                {
                    float yawDegrees = 0.0f;
                    float pitchDegrees = 0.0f;
                    DirectionToYawPitch(light.Direction, yawDegrees, pitchDegrees);
                    bool directionChanged = false;
                    directionChanged |= ImGui::SliderFloat("Direction Yaw", &yawDegrees, -180.0f, 180.0f, "%.1f deg");
                    directionChanged |= ImGui::SliderFloat("Direction Pitch", &pitchDegrees, -89.0f, 89.0f, "%.1f deg");
                    if (directionChanged)
                    {
                        YawPitchToDirection(yawDegrees, pitchDegrees, light.Direction);
                    }
                }

                if (light.Type == Assets::LightType::Spot)
                {
                    float innerDegrees = DirectX::XMConvertToDegrees(light.SpotInnerConeAngle);
                    float outerDegrees = DirectX::XMConvertToDegrees(light.SpotOuterConeAngle);
                    ImGui::SliderFloat("Inner Cone", &innerDegrees, 0.0f, 89.0f, "%.1f deg");
                    ImGui::SliderFloat("Outer Cone", &outerDegrees, 0.0f, 90.0f, "%.1f deg");
                    if (innerDegrees > outerDegrees)
                    {
                        innerDegrees = outerDegrees;
                    }
                    light.SpotInnerConeAngle = DirectX::XMConvertToRadians(innerDegrees);
                    light.SpotOuterConeAngle = DirectX::XMConvertToRadians(outerDegrees);
                }
            }
        }

        ImGui::End();
    }

    void KurenaiEngine3D::RenderReflectionProbeUI()
    {
        ImGui::SetNextWindowPos(ImVec2(590.0f, 10.0f), ImGuiCond_FirstUseEver);
        // Box形状を選ぶとBox Extents/Yawが増えるため、それでもスクロール無しで収まる高さにしておく
        ImGui::SetNextWindowSize(ImVec2(300.0f, 430.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("Reflection Probes");

        ImGui::Checkbox("Enable Reflection Probes", &m_ReflectionProbeEnabled);
        // 以下2つはPhase 1(球形・単一選択・視差補正なし)との見比べ用。どちらも焼き直し不要で、
        // 環境ソースの引き方だけが変わる
        ImGui::Checkbox("Parallax Correction", &m_ProbeParallaxCorrectionEnabled);
        if (ImGui::IsItemHovered())
        {
            // ImGuiの既定フォント(ProggyClean)はASCIIしか持たないため、UI文言は他と同様に英語で書く
            ImGui::SetTooltip("Box shape only. Intersects the reflection vector with the box\nso reflections line up away from the probe center.");
        }
        ImGui::Checkbox("Probe Blending", &m_ProbeBlendingEnabled);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Fades weights over Blend Distance inward from the volume border.\nOff: uses only the nearest probe, leaving a seam at the border.");
        }
        ImGui::Text("Probes: %zu / %u", m_ReflectionProbes.size(), kMaxReflectionProbes);
        if (!m_ProbeBaked && !m_ReflectionProbes.empty())
        {
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "Not baked yet");
        }

        ImGui::BeginChild("ProbeList", ImVec2(0.0f, 90.0f), true);
        for (size_t i = 0; i < m_ReflectionProbes.size(); ++i)
        {
            ImGui::PushID(static_cast<int>(i));
            char label[192];
            std::snprintf(
                label, sizeof(label), "[%zu] %s", i,
                m_ReflectionProbes[i].Name.empty() ? "(no name)" : m_ReflectionProbes[i].Name.c_str());
            if (ImGui::Selectable(label, m_SelectedProbeIndex == static_cast<int>(i)))
            {
                m_SelectedProbeIndex = static_cast<int>(i);
            }
            ImGui::PopID();
        }
        ImGui::EndChild();

        // キューブマップ配列は固定容量のため、上限に達したら追加できない
        ImGui::BeginDisabled(m_ReflectionProbes.size() >= kMaxReflectionProbes);
        if (ImGui::Button("Add"))
        {
            // 追加位置はプローブ一覧の中心ではなくシーンAABBの中心にする(カメラ位置だと
            // 壁や地面へめり込んだ位置に置かれやすく、そのまま焼くと真っ暗なプローブになるため)
            Assets::ReflectionProbe newProbe;
            newProbe.Position[0] = (m_Scene.BoundsMin[0] + m_Scene.BoundsMax[0]) * 0.5f;
            newProbe.Position[1] = (m_Scene.BoundsMin[1] + m_Scene.BoundsMax[1]) * 0.5f;
            newProbe.Position[2] = (m_Scene.BoundsMin[2] + m_Scene.BoundsMax[2]) * 0.5f;
            newProbe.Name = "New Probe";
            m_ReflectionProbes.push_back(newProbe);
            m_SelectedProbeIndex = static_cast<int>(m_ReflectionProbes.size()) - 1;
            m_ProbeBakeRequested = true;
        }
        ImGui::EndDisabled();

        const bool hasSelection =
            m_SelectedProbeIndex >= 0 && m_SelectedProbeIndex < static_cast<int>(m_ReflectionProbes.size());

        ImGui::SameLine();
        ImGui::BeginDisabled(!hasSelection);
        if (ImGui::Button("Remove") && hasSelection)
        {
            m_ReflectionProbes.erase(m_ReflectionProbes.begin() + m_SelectedProbeIndex);
            m_SelectedProbeIndex = m_ReflectionProbes.empty()
                ? -1
                : std::min(m_SelectedProbeIndex, static_cast<int>(m_ReflectionProbes.size()) - 1);
            // 番号がずれるため残り全部を焼き直す
            m_ProbeBakeRequested = !m_ReflectionProbes.empty();
            m_ProbeBaked = m_ProbeBaked && !m_ReflectionProbes.empty();
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::BeginDisabled(m_ReflectionProbes.empty());
        if (ImGui::Button("Bake"))
        {
            m_ProbeBakeRequested = true;
        }
        ImGui::EndDisabled();

        if (hasSelection)
        {
            Assets::ReflectionProbe& probe = m_ReflectionProbes[static_cast<size_t>(m_SelectedProbeIndex)];

            char nameBuffer[128];
            std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", probe.Name.c_str());
            if (ImGui::InputText("Name", nameBuffer, sizeof(nameBuffer)))
            {
                probe.Name = nameBuffer;
            }

            // 位置・半径はキャプチャ内容そのものを変えるため、動かしたら焼き直す必要がある
            if (ImGui::DragFloat3("Position", probe.Position, 0.1f))
            {
                m_ProbeBakeRequested = true;
            }
            // 以下の影響範囲パラメータはどれもキャプチャ内容には影響しない(どこから撮るかは
            // Positionだけで決まる)ため、変更しても焼き直しは不要
            int shapeIndex = (probe.Shape == Assets::ReflectionProbeShape::Box) ? 1 : 0;
            const char* const shapeNames[] = { "Sphere", "Box" };
            if (ImGui::Combo("Shape", &shapeIndex, shapeNames, IM_ARRAYSIZE(shapeNames)))
            {
                probe.Shape = (shapeIndex == 1)
                    ? Assets::ReflectionProbeShape::Box
                    : Assets::ReflectionProbeShape::Sphere;
            }

            if (probe.Shape == Assets::ReflectionProbeShape::Box)
            {
                // 各軸の半径。0以下だと箱が潰れて交差計算が成り立たないため下限を与える
                if (ImGui::DragFloat3("Box Extents", probe.BoxExtents, 0.1f, 0.1f, 1000.0f, "%.2f"))
                {
                    for (float& extent : probe.BoxExtents)
                    {
                        extent = std::max(extent, 0.1f);
                    }
                }
                ImGui::DragFloat("Yaw", &probe.YawDegrees, 0.5f, -180.0f, 180.0f, "%.1f deg");
            }
            else
            {
                ImGui::DragFloat("Radius", &probe.Radius, 0.1f, 0.1f, 1000.0f, "%.2f");
            }

            ImGui::DragFloat("Blend Distance", &probe.BlendDistance, 0.05f, 0.0f, 100.0f, "%.2f");
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Distance inward from the volume border over which this probe's\nweight ramps up to 1. Zero makes the border a hard cut.");
            }
        }

        ImGui::End();
    }

    void KurenaiEngine3D::RenderProfilerUI()
    {
        // Lightingパネル(280,10)がライト編集項目を全部開くと縦に480px近く必要になるため、
        // それより下(490)に配置して重ならないようにする
        ImGui::SetNextWindowPos(ImVec2(280.0f, 490.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(280.0f, 460.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("Profiler");

        ImGui::Text("FPS: %.1f", m_FPS);
        ImGui::Text("CPU Frame Time: %.3f ms", m_CPUFrameTimeMs);
        ImGui::Text("GPU Frame Time: %.3f ms", m_GPUProfiler->GetTotalFrameTimeMs());
        // GPUの完了待ち(DX12のフレームパイプライン化に伴うフェンス待ち)。CPU Frame Timeや
        // PresentSubmitの計測値からは既に除外済みなので、参考情報として別枠で表示する
        ImGui::Text("GPU Wait: %.3f ms", m_Device->GetLastFrameGPUWaitTimeMs());
        ImGui::Separator();
        ImGui::TextUnformatted("CPU Pass Breakdown:");
        for (const auto& result : m_CPUProfiler.GetResults())
        {
            ImGui::Text("  %s: %.3f ms", result.Name.c_str(), result.TimeMs);
        }
        ImGui::Separator();
        ImGui::TextUnformatted("GPU Pass Breakdown:");
        for (const auto& result : m_GPUProfiler->GetResults())
        {
            ImGui::Text("  %s: %.3f ms", result.Name.c_str(), result.TimeMs);
        }

        ImGui::End();
    }

    void KurenaiEngine3D::Run()
    {
        // 描画専用スレッドを起動する。以後このスレッドがRender()の呼び出しとPresentを担当し、
        // 呼び出し元スレッド(以下Updateスレッド)はPumpMessages/Updateに専念する
        m_RenderThread = std::thread(&KurenaiEngine3D::RenderThreadMain, this);

        while (!m_Window->ShouldClose())
        {
            m_Window->PumpMessages();
            if (m_Window->ShouldClose())
            {
                break;
            }

            const auto now = std::chrono::steady_clock::now();
            const float deltaTime = std::chrono::duration<float>(now - m_LastFrameTime).count();
            m_LastFrameTime = now;

            Update(deltaTime);

            // m_CameraはUpdateスレッド(UpdateMouseLook/UpdateMovement/LoadScene経由のFrameCameraToModel)
            // のみが書き込み、Render()はframeStateのスナップショット経由でしか読まないため、
            // ここでの読み取りに追加のロックは不要
            FrameState newFrameState;
            newFrameState.Camera = m_Camera;
            newFrameState.ImGuiVisible = m_ImGuiVisible;

            // Renderスレッドが直前フレーム分を取り込み終えるまで待つ(キュー深度1)。
            // 取り込み自体はスナップショットのコピーだけなので即座に完了し、その後の重いGPU発行は
            // このUpdateスレッドの次フレーム処理と並行して進む
            {
                std::unique_lock<std::mutex> lock(m_FrameStateMutex);
                m_FrameStateCV.wait(lock, [this] { return m_FrameStateTaken; });
                m_FrameState = newFrameState;
                m_FrameStateReady = true;
                m_FrameStateTaken = false;
            }
            m_FrameStateCV.notify_one();
        }

        {
            std::lock_guard<std::mutex> lock(m_FrameStateMutex);
            m_StopRenderThread = true;
        }
        m_FrameStateCV.notify_one();
        m_RenderThread.join();
    }

    void KurenaiEngine3D::RenderThreadMain()
    {
        // LoadScene(RenderSceneSwitchUI経由でこのスレッドから呼ばれる)がWICテクスチャ読み込みで
        // COMを使用する。COMはスレッドごとに初期化が必要(wWinMainでのCoInitializeExはUpdate
        // スレッド=呼び出し元スレッドにしか適用されない)なため、この描画スレッドでも初期化しておく。
        // 未初期化のままだとWIC呼び出しがハングする(Main.cppと同じAPARTMENTTHREADEDに揃える)
        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

        m_LastRenderFrameTime = std::chrono::steady_clock::now();

        for (;;)
        {
            FrameState frameState;
            {
                std::unique_lock<std::mutex> lock(m_FrameStateMutex);
                m_FrameStateCV.wait(lock, [this] { return m_FrameStateReady || m_StopRenderThread; });
                if (m_StopRenderThread && !m_FrameStateReady)
                {
                    break;
                }
                frameState = m_FrameState;
                m_FrameStateReady = false;
                m_FrameStateTaken = true;
            }
            m_FrameStateCV.notify_one();

            const auto now = std::chrono::steady_clock::now();
            const float renderDeltaTime = std::chrono::duration<float>(now - m_LastRenderFrameTime).count();
            m_LastRenderFrameTime = now;

            // 昼夜サイクルの自動進行はUpdateスレッドではなくこちら(Renderスレッド)で行う。
            // m_TimeOfDay/m_TimeAutoAdvance/m_TimeAdvanceSpeedはImGuiパネル(RenderLightingUI、
            // Renderスレッドから描画)でも書き換えられるため、両方をRenderスレッド専有にすることで
            // 追加の排他制御なしに済ませられる
            if (m_TimeAutoAdvance)
            {
                m_TimeOfDay = std::fmod(m_TimeOfDay + m_TimeAdvanceSpeed * renderDeltaTime, 24.0f);
                if (m_TimeOfDay < 0.0f)
                {
                    m_TimeOfDay += 24.0f;
                }
            }

            const auto cpuStart = std::chrono::steady_clock::now();
            {
                // WM_SIZEによるスワップチェーンのリサイズ、およびLoadScene(Updateスレッド、
                // UpdateSceneSwitch経由)によるm_Scene/m_Camera/Post Processingパラメータの書き換えと
                // 同時に走らないよう、Render()全体をこれらのミューテックスで保護する。この2つの
                // ミューテックスをこの組み合わせ・この順序でロックするのはここだけなので、
                // std::scoped_lockでなくてもデッドロックの心配はないが、明示的にまとめて扱っておく
                std::scoped_lock renderLock(m_SwapChainMutex, m_SceneMutex);
                Render(frameState);
            }
            const auto cpuEnd = std::chrono::steady_clock::now();
            // GPUの完了待ち(DX12のフレームパイプライン化に伴うフェンス待ち)は実際のCPU負荷ではなく
            // GPU側の処理時間の反映なので差し引く(DX11は常に0が返るため影響しない)
            const float rawCPUTimeMs = std::chrono::duration<float, std::milli>(cpuEnd - cpuStart).count();
            m_CPUFrameTimeMs = std::max(0.0f, rawCPUTimeMs - m_Device->GetLastFrameGPUWaitTimeMs());

            // 固定FPSモード: このフレームの処理(Time of Day更新+Render+Present)が目標フレーム時間
            // より短く終わった場合、余った時間だけ待機して間隔を揃える。CPU/GPU計測(上記)の後に
            // 行うことで、この待機時間自体がプロファイラの計測値に混ざらないようにしている
            if (m_FixedFPSEnabled && m_TargetFPS > 0.0f)
            {
                const auto targetFrameDuration = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(1.0 / m_TargetFPS));
                const auto frameDeadline = now + targetFrameDuration;
                if (std::chrono::steady_clock::now() < frameDeadline)
                {
                    std::this_thread::sleep_until(frameDeadline);
                }
            }

            // FPSは指数移動平均で平滑化する(生の1/deltaTimeだとフレームごとの揺れが大きく読み取りにくいため)
            if (renderDeltaTime > 0.0f)
            {
                const float instantFPS = 1.0f / renderDeltaTime;
                m_FPS = (m_FPS == 0.0f) ? instantFPS : (m_FPS * 0.9f + instantFPS * 0.1f);
            }
        }

        if (SUCCEEDED(comResult))
        {
            CoUninitialize();
        }
    }

    void KurenaiEngine3D::UpdateMouseLook()
    {
        // このメソッドだけは意図的にGetAsyncKeyState/GetCursorPos/SetCursorPosを使い続けている。
        // カーソルを画面中央へ強制的に固定し続ける(SetCursorPos)ことで無限ドラッグを実現しており、
        // これは実カーソルを動かす・隠す操作そのものであるため、メッセージベース化(PostMessageで
        // WM_RBUTTONDOWN/WM_MOUSEMOVEを送るだけで発火する形)にしてしまうと、動作確認用の
        // PostMessage送信が実デスクトップのカーソルを意図せず動かし・隠してしまう経路になる。
        // GetAsyncKeyState(VK_RBUTTON)はPostMessageでは変化しない実ハードウェアの状態のため、
        // このままにしておくことでPostMessageによる動作確認が誤ってカーソル操作を引き起こさない
        // (=実カーソル・他ウィンドウに影響を与えない)ことを構造的に保証している
        //
        // GetAsyncKeyStateはウィンドウフォーカスに関係なくグローバルなキー状態を返すため、
        // フォアグラウンドウィンドウチェックがないとデスクトップ上の右クリックでも
        // カーソルがウィンドウ中央へ強制移動してしまう
        const bool isForeground = GetForegroundWindow() == m_Window->GetHandle();
        if (isForeground && (GetAsyncKeyState(VK_RBUTTON) & 0x8000))
        {
            if (!m_MouseCaptured)
            {
                m_MouseCaptured = true;
                ShowCursor(FALSE);

                RECT clientRect;
                GetClientRect(m_Window->GetHandle(), &clientRect);
                POINT center{ (clientRect.right - clientRect.left) / 2, (clientRect.bottom - clientRect.top) / 2 };
                ClientToScreen(m_Window->GetHandle(), &center);
                m_MouseCaptureCenter = center;
                SetCursorPos(center.x, center.y);
            }
            else
            {
                POINT currentPos;
                GetCursorPos(&currentPos);
                const float deltaX = static_cast<float>(currentPos.x - m_MouseCaptureCenter.x);
                const float deltaY = static_cast<float>(currentPos.y - m_MouseCaptureCenter.y);

                const float mouseSensitivity = 0.0025f;
                m_Camera.Rotate(deltaX * mouseSensitivity, -deltaY * mouseSensitivity);

                SetCursorPos(m_MouseCaptureCenter.x, m_MouseCaptureCenter.y);
            }
        }
        else if (m_MouseCaptured)
        {
            m_MouseCaptured = false;
            ShowCursor(TRUE);
        }
    }

    void KurenaiEngine3D::UpdateMovement(float deltaTime)
    {
        // メッセージベースの入力API(IsKeyDown)を使う。GetAsyncKeyStateと異なりウィンドウが
        // フォーカスを失っている間は反応せず、PostMessageによるテスト自動化とも整合する
        const float moveSpeed = IsKeyDown(VK_SHIFT) ? 20.0f : 5.0f;
        const float moveAmount = moveSpeed * deltaTime;

        const DirectX::XMFLOAT3 forward = m_Camera.GetForward();
        const DirectX::XMFLOAT3 right = m_Camera.GetRight();

        DirectX::XMFLOAT3 move{ 0.0f, 0.0f, 0.0f };
        auto add = [&move](const DirectX::XMFLOAT3& v, float sign)
        {
            move.x += v.x * sign;
            move.y += v.y * sign;
            move.z += v.z * sign;
        };

        if (IsKeyDown('W')) add(forward, 1.0f);
        if (IsKeyDown('S')) add(forward, -1.0f);
        if (IsKeyDown('D')) add(right, 1.0f);
        if (IsKeyDown('A')) add(right, -1.0f);
        if (IsKeyDown('E')) move.y += 1.0f;
        if (IsKeyDown('Q')) move.y -= 1.0f;

        DirectX::XMVECTOR moveVec = DirectX::XMLoadFloat3(&move);
        if (DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(moveVec)) > 0.0001f)
        {
            moveVec = DirectX::XMVectorScale(DirectX::XMVector3Normalize(moveVec), moveAmount);
            DirectX::XMFLOAT3 delta;
            DirectX::XMStoreFloat3(&delta, moveVec);
            m_Camera.Move(delta);
        }
    }

    void KurenaiEngine3D::UpdateImGuiToggle()
    {
        // WasKeyPressedはウィンドウメッセージ由来のエッジ検出を内蔵しているため、
        // 前フレームの押下状態を自前で保持する必要がない
        if (WasKeyPressed(VK_F1))
        {
            m_ImGuiVisible = !m_ImGuiVisible;
        }
    }

    void KurenaiEngine3D::UpdateSceneSwitch()
    {
        // -1は「切り替え要求なし」を表す番兵値。exchangeで読み取りと同時に-1へ戻すことで、
        // 同じ要求を二重に処理しない
        const int pendingIndex = m_PendingSceneIndex.exchange(-1);
        if (pendingIndex >= 0)
        {
            LoadScene(static_cast<size_t>(pendingIndex));
        }
    }

    void KurenaiEngine3D::Update(float deltaTime)
    {
        UpdateMouseLook();
        UpdateMovement(deltaTime);
        UpdateImGuiToggle();
        UpdateSceneSwitch();
        // 昼夜サイクルの自動進行(m_TimeOfDay)はRenderThreadMain側で行う(RenderThreadMain参照)
    }

    void KurenaiEngine3D::Render(const FrameState& frameState)
    {
        if (m_Window->GetWidth() == 0 || m_Window->GetHeight() == 0)
        {
            return;
        }

        // WndProc(Updateスレッド)でキューイングされたメッセージを、ImGuiの状態を実際に読み書きする
        // このRenderスレッド自身からImGui_ImplWin32_WndProcHandlerへ転送する。ImGui::NewFrame()より前に
        // 行うことで、このフレームのNewFrame()が最新のマウス/キーボード状態を反映できる
        m_Window->ForwardQueuedMessagesToImGui();

        m_ImGuiBackend->NewFrame();
        if (frameState.ImGuiVisible)
        {
            RenderSceneSwitchUI();
            RenderPostProcessUI();
            RenderDebugViewUI();
            RenderLightingUI(frameState);
            RenderReflectionProbeUI();
            RenderProfilerUI();
        }

        auto* commandList = m_Device->GetImmediateCommandList();
        m_GPUProfiler->BeginFrame();
        m_CPUProfiler.BeginFrame();

        const SunLighting sunLighting = ComputeSunLighting(m_TimeOfDay, m_SunAzimuthDegrees, m_SceneExposureEV100);

        // カスケードシャドウマップ: カメラ視錐台をkCascadeCount個の深度範囲に分割し、
        // それぞれ専用のライト正射影ビュー・プロジェクション行列を求める
        float cascadeSplits[kCascadeCount];
        ComputeCascadeSplits(frameState.Camera, cascadeSplits);
        DirectX::XMMATRIX cascadeViewProj[kCascadeCount];
        float cascadeNear = frameState.Camera.GetNearZ();
        for (uint32_t cascade = 0; cascade < kCascadeCount; ++cascade)
        {
            cascadeViewProj[cascade] =
                ComputeCascadeLightViewProj(sunLighting.Direction, frameState.Camera, cascadeNear, cascadeSplits[cascade]);
            cascadeNear = cascadeSplits[cascade];
        }

        const DirectX::XMFLOAT3 cameraPosition = frameState.Camera.GetPosition();

        // 有効なライトだけを詰めてt8のライトリストへ渡す。シェーダはLightCount(・ActiveLightCount)の
        // 数までしかループしないため、無効なライトはそもそもGPUへ送らない。DirectLight/Transparentの
        // 両パスがこの1つのリストを共有する(FrameConstants.ActiveLightCountに人数を書き込むため、
        // 各パスのExecute内ではなくFrameConstants確定より前にここで組み立てる必要がある)
        std::vector<GPULight> gpuLights;
        gpuLights.reserve(m_Lights.size());
        for (const Assets::Light& light : m_Lights)
        {
            if (!light.Enabled)
            {
                continue;
            }
            gpuLights.push_back(MakeGPULight(light, m_SceneExposureEV100));
        }

        // 容量(kMaxLights)を超える場合は、カメラに近い順に先頭kMaxLights灯のみ採用する。
        // 全画面ディファードなのでフラスタムカリングは効果が薄く、これは容量超過時の
        // 安全弁としてのみ機能する(実データの上限はBistroInteriorの4灯)
        if (gpuLights.size() > kMaxLights)
        {
            std::sort(
                gpuLights.begin(), gpuLights.end(),
                [&cameraPosition](const GPULight& a, const GPULight& b)
                {
                    const float dxA = a.PositionType.x - cameraPosition.x;
                    const float dyA = a.PositionType.y - cameraPosition.y;
                    const float dzA = a.PositionType.z - cameraPosition.z;
                    const float dxB = b.PositionType.x - cameraPosition.x;
                    const float dyB = b.PositionType.y - cameraPosition.y;
                    const float dzB = b.PositionType.z - cameraPosition.z;
                    return (dxA * dxA + dyA * dyA + dzA * dzA) < (dxB * dxB + dyB * dyB + dzB * dzB);
                });
            gpuLights.resize(kMaxLights);

            if (!m_LightOverflowLogged)
            {
                Core::Logger::Warning(
                    "KurenaiEngine3D",
                    "ライト数が上限(" + std::to_string(kMaxLights) + ")を超えたため、カメラに近い順に描画します");
                m_LightOverflowLogged = true;
            }
        }

        FrameConstants constants;
        const DirectX::XMMATRIX viewProj = frameState.Camera.GetViewMatrix() * frameState.Camera.GetProjectionMatrix();
        DirectX::XMStoreFloat4x4(&constants.ViewProj, DirectX::XMMatrixTranspose(viewProj));
        DirectX::XMVECTOR determinant;
        const DirectX::XMMATRIX invViewProj = DirectX::XMMatrixInverse(&determinant, viewProj);
        DirectX::XMStoreFloat4x4(&constants.InvViewProj, DirectX::XMMatrixTranspose(invViewProj));
        for (uint32_t cascade = 0; cascade < kCascadeCount; ++cascade)
        {
            DirectX::XMStoreFloat4x4(&constants.CascadeViewProj[cascade], DirectX::XMMatrixTranspose(cascadeViewProj[cascade]));
        }
        constants.CameraPosition = { cameraPosition.x, cameraPosition.y, cameraPosition.z, 0.0f };
        constants.LightDirection = { sunLighting.Direction.x, sunLighting.Direction.y, sunLighting.Direction.z, 0.0f };
        // 太陽を無効にする場合は色をゼロにするだけでよい(シェーダー側は太陽の寄与に
        // LightColor.rgbを乗算するため、これで完全に消える)。TimeOfDayを夜にする方法と違い
        // 昼度(AmbientColor.a)は下がらないので、環境光だけで照らす状態を作れる
        constants.LightColor = m_SunEnabled ? sunLighting.Color : DirectX::XMFLOAT4{ 0.0f, 0.0f, 0.0f, 0.0f };
        DirectX::XMStoreFloat4x4(&constants.View, DirectX::XMMatrixTranspose(frameState.Camera.GetViewMatrix()));
        DirectX::XMStoreFloat4x4(&constants.Proj, DirectX::XMMatrixTranspose(frameState.Camera.GetProjectionMatrix()));
        // rgb(環境光の色)にm_AmbientScaleを乗算する。Enable IBL無効時のフォールバックアンビエント
        // (DeferredLighting.hlsl)の強度調整用で、alpha(dayFactor、IBLの夜間減光・背景スカイの
        // 昼夜ブレンドに使う)には掛けない
        constants.AmbientColor =
        {
            sunLighting.Ambient.x * m_AmbientScale,
            sunLighting.Ambient.y * m_AmbientScale,
            sunLighting.Ambient.z * m_AmbientScale,
            sunLighting.Ambient.w,
        };
        constants.CascadeSplits = { cascadeSplits[0], cascadeSplits[1], cascadeSplits[2], cascadeSplits[3] };
        const float iblIntensity = m_IBLEnabled ? m_IBLIntensity : 0.0f;
        const float specularEnergyCompensation = m_SpecularEnergyCompensationEnabled ? 1.0f : 0.0f;
        constants.ShadowParams = {
            m_ShadowLightSize,
            static_cast<float>(kIBLPrefilterMipLevels - 1),
            iblIntensity,
            specularEnergyCompensation,
        };
        constants.ActiveLightCount = { static_cast<float>(gpuLights.size()), 0.0f, 0.0f, 0.0f };

        // 反射プローブの影響範囲をt13のStructuredBufferへ渡す。まだ一度も焼けていない場合
        // (m_ProbeBaked=false)や機能を無効にしている場合はプローブ数を0にして、シェーダー側の
        // 選択ループ自体を回さない=中身が未定義のキューブマップを引かせないようにする
        std::vector<GPUReflectionProbe> gpuProbes;
        if (m_ReflectionProbeEnabled && m_ProbeBaked)
        {
            gpuProbes.reserve(m_ReflectionProbes.size());
            for (const Assets::ReflectionProbe& probe : m_ReflectionProbes)
            {
                // Yawはシェーダー側で毎ピクセル三角関数を回さずに済むよう、ここでsin/cosへ展開しておく
                const float yawRadians = DirectX::XMConvertToRadians(probe.YawDegrees);
                const bool isBox = probe.Shape == Assets::ReflectionProbeShape::Box;

                GPUReflectionProbe gpuProbe{};
                gpuProbe.PositionRadius = { probe.Position[0], probe.Position[1], probe.Position[2], probe.Radius };
                gpuProbe.BoxExtents = {
                    probe.BoxExtents[0], probe.BoxExtents[1], probe.BoxExtents[2], probe.BlendDistance
                };
                gpuProbe.ShapeParams = {
                    isBox ? 1.0f : 0.0f, std::sin(yawRadians), std::cos(yawRadians), 0.0f
                };
                gpuProbes.push_back(gpuProbe);
            }
        }
        if (!gpuProbes.empty())
        {
            commandList->UpdateBuffer(m_ProbeBuffer.get(), gpuProbes.data(), sizeof(GPUReflectionProbe) * gpuProbes.size());
        }

        const float probeInfluenceDebug = (m_DebugView == DebugView::ProbeInfluence) ? 1.0f : 0.0f;
        constants.ProbeParams = {
            static_cast<float>(gpuProbes.size()),
            probeInfluenceDebug,
            m_ProbeParallaxCorrectionEnabled ? 1.0f : 0.0f,
            m_ProbeBlendingEnabled ? 1.0f : 0.0f,
        };
        commandList->UpdateBuffer(m_FrameConstantBuffer.get(), &constants, sizeof(constants));

        // 各パスをリソースの読み書き依存関係から自動的に順序付けて実行するレンダーグラフ。
        // トランジェントリソースの確保は行わず、既存の永続確保済みテクスチャ(G-Buffer・SceneColor等)を
        // そのまま読み書きする(詳細はRenderGraph.h参照)
        Core::RenderGraph graph(commandList, m_GPUProfiler.get(), &m_CPUProfiler);

        // --- IBL畳み込みパス: スカイボックスは実行時に変化しない静的アセットのため、
        //     起動後最初のフレームでのみ実行し、以降は焼き直さない(m_IBLBaked) ---
        if (!m_IBLBaked)
        {
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "IBLBake",
                .Reads = { m_SkyboxTexture.get() },
                .Writes = { m_IrradianceTexture.get(), m_PrefilteredEnvTexture.get(), m_BRDFLUTTexture.get() },
                .Execute = [this](RHI::IRHICommandList* cmd)
                {
                    // BRDF積分LUT(スカイボックスに依存しない、NdotV×ラフネスの128x128グリッド)
                    cmd->SetComputePipelineState(m_BRDFLUTPipelineState.get());
                    cmd->SetComputeUnorderedAccessTexture(0, m_BRDFLUTTexture.get(), 0);
                    cmd->Dispatch((kIBLBRDFLUTSize + 7) / 8, (kIBLBRDFLUTSize + 7) / 8, 1);

                    // 拡散イラディアンス(本物のTextureCube、32x32x6面)。HLSLはリソースを動的に
                    // スライス選択できないため、面ごとに1回ずつディスパッチする
                    cmd->SetComputePipelineState(m_IrradiancePipelineState.get());
                    cmd->SetComputeTexture(0, m_SkyboxTexture.get());
                    cmd->SetComputeSamplerSet(m_MaterialSamplers.get());
                    for (uint32_t face = 0; face < kCubeFaceCount; ++face)
                    {
                        IBLFaceConstants faceConstants{};
                        faceConstants.Face = face;
                        cmd->UpdateBuffer(m_IBLPrefilterConstantBuffer.get(), &faceConstants, sizeof(faceConstants));
                        cmd->SetComputeConstantBuffer(0, m_IBLPrefilterConstantBuffer.get());
                        cmd->SetComputeUnorderedAccessTextureCubeFace(0, m_IrradianceTexture.get(), face, 0);
                        cmd->Dispatch((kIBLIrradianceSize + 7) / 8, (kIBLIrradianceSize + 7) / 8, 1);
                    }

                    // プリフィルタ済み鏡面(本物のTextureCube、ミップごとに異なるラフネスで畳み込む)。
                    // 面×ミップの組み合わせごとに1回ずつディスパッチする
                    cmd->SetComputePipelineState(m_PrefilterPipelineState.get());
                    cmd->SetComputeTexture(0, m_SkyboxTexture.get());
                    cmd->SetComputeSamplerSet(m_MaterialSamplers.get());
                    for (uint32_t mip = 0; mip < kIBLPrefilterMipLevels; ++mip)
                    {
                        const uint32_t mipSize = std::max(1u, kIBLPrefilterBaseSize >> mip);
                        const float roughness = static_cast<float>(mip) / static_cast<float>(kIBLPrefilterMipLevels - 1);
                        for (uint32_t face = 0; face < kCubeFaceCount; ++face)
                        {
                            IBLFaceConstants faceConstants{};
                            faceConstants.Face = face;
                            faceConstants.Roughness = roughness;
                            cmd->UpdateBuffer(m_IBLPrefilterConstantBuffer.get(), &faceConstants, sizeof(faceConstants));
                            cmd->SetComputeConstantBuffer(0, m_IBLPrefilterConstantBuffer.get());
                            cmd->SetComputeUnorderedAccessTextureCubeFace(0, m_PrefilteredEnvTexture.get(), face, mip);
                            cmd->Dispatch((mipSize + 7) / 8, (mipSize + 7) / 8, 1);
                        }
                    }
                },
            });
            m_IBLBaked = true;
        }

        RHI::Viewport shadowViewport;
        shadowViewport.Width = static_cast<float>(kShadowMapSize);
        shadowViewport.Height = static_cast<float>(kShadowMapSize);

        RHI::Viewport gbufferViewport;
        gbufferViewport.Width = static_cast<float>(m_RenderWidth);
        gbufferViewport.Height = static_cast<float>(m_RenderHeight);

        // --- シャドウパス: ライト視点から深度のみを描画する(常に固定のシャドウマップ解像度)。
        //     カスケードごとに1回ずつ、同じメッシュ群を異なるライト正射影で描き直す ---
        for (uint32_t cascade = 0; cascade < kCascadeCount; ++cascade)
        {
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "Shadow" + std::to_string(cascade),
                .DepthTarget = m_ShadowCascades[cascade].get(),
                .Execute = [this, &shadowViewport, cascade, &cascadeViewProj](RHI::IRHICommandList* cmd)
                {
                    cmd->SetViewport(shadowViewport);
                    // 深度1.0(最遠)にクリアしておく。無効時はこの後の描画をスキップするため、
                    // シェーダー側は深度比較で常に「影なし」と判定する(ComputeShadowFactor参照)
                    cmd->ClearDepth(1.0f);

                    if (m_ShadowEnabled)
                    {
                        CascadeConstants cascadeConstants{};
                        DirectX::XMStoreFloat4x4(&cascadeConstants.ViewProj, DirectX::XMMatrixTranspose(cascadeViewProj[cascade]));
                        cmd->UpdateBuffer(m_ShadowCascadeConstantBuffer.get(), &cascadeConstants, sizeof(cascadeConstants));

                        cmd->SetPipelineState(m_ShadowPipelineState.get());
                        cmd->SetConstantBuffer(0, m_ShadowCascadeConstantBuffer.get());

                        for (const auto& instance : m_Scene.Instances)
                        {
                            for (const auto& mesh : instance.Model.Meshes)
                            {
                                // シャドウパスはWorld以外を使わないが、GBufferパスと同じルートシグネチャ/
                                // 定数バッファ(b1)を共有しているため必ずバインドする必要がある
                                const ObjectConstants objectConstants = MakeObjectConstants(instance, mesh);
                                cmd->UpdateBuffer(m_ObjectConstantBuffer.get(), &objectConstants, sizeof(objectConstants));
                                cmd->SetConstantBuffer(1, m_ObjectConstantBuffer.get());

                                cmd->SetVertexBuffer(mesh.VertexBuffer.get());
                                cmd->SetIndexBuffer(mesh.IndexBuffer.get());
                                cmd->DrawIndexed(mesh.IndexCount, 0, 0);
                            }
                        }
                    }
                },
            });
        }

        // --- 反射プローブのベイクパス(15章) ---
        // シーン読み込み時とImGuiのBakeボタンで要求されたときだけ実行する。プローブの中身は
        // シーンのジオメトリ・ライト・時刻に依存するため、スカイボックス由来のIBLのような
        // 「起動時に一度だけ」では足りない。
        // Readsにシャドウマップとグローバルの畳み込み結果を挙げることで、レンダーグラフが
        // このパスをシャドウパス・IBLBakeパスより後ろへ順序付ける
        if (m_ProbeBakeRequested && !m_ReflectionProbes.empty())
        {
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "ProbeBake",
                .Reads = {
                    m_ShadowCascades[0].get(), m_ShadowCascades[1].get(), m_ShadowCascades[2].get(), m_ShadowCascades[3].get(),
                    m_SkyboxTexture.get(), m_IrradianceTexture.get(), m_PrefilteredEnvTexture.get(), m_BRDFLUTTexture.get(),
                },
                .Writes = {
                    m_ProbeCaptureColor.get(), m_ProbeCaptureDepth.get(), m_ProbeRadianceCube.get(),
                    m_ProbeIrradianceArray.get(), m_ProbePrefilteredArray.get(),
                },
                .Execute = [this, &frameState, &constants](RHI::IRHICommandList* cmd)
                {
                    RHI::Viewport probeViewport;
                    probeViewport.Width = static_cast<float>(kProbeCaptureSize);
                    probeViewport.Height = static_cast<float>(kProbeCaptureSize);

                    const DirectX::XMMATRIX faceProjection =
                        ComputeCubeFaceProjection(frameState.Camera.GetNearZ(), frameState.Camera.GetFarZ());
                    RHI::IRHITexture* const captureTargets[] = { m_ProbeCaptureColor.get() };

                    for (size_t probeIndex = 0; probeIndex < m_ReflectionProbes.size(); ++probeIndex)
                    {
                        const Assets::ReflectionProbe& probe = m_ReflectionProbes[probeIndex];
                        const DirectX::XMFLOAT3 probePosition{ probe.Position[0], probe.Position[1], probe.Position[2] };
                        const uint32_t cubeIndex = static_cast<uint32_t>(probeIndex);

                        // --- 6面をキャプチャし、スクラッチのキューブマップへ組み上げる ---
                        for (uint32_t face = 0; face < kCubeFaceCount; ++face)
                        {
                            // 太陽・カスケード・ライト数・IBL設定は共有のFrameConstantsをそのまま使い、
                            // 視点に関わる2つだけをプローブのものへ差し替える(ProbeCapture.hlsl冒頭参照)。
                            // Viewはカメラのまま残す(カスケード選択の深度がカメラ視錐台基準のため)
                            FrameConstants captureConstants = constants;
                            const DirectX::XMMATRIX faceViewProj = ComputeCubeFaceView(probePosition, face) * faceProjection;
                            DirectX::XMStoreFloat4x4(&captureConstants.ViewProj, DirectX::XMMatrixTranspose(faceViewProj));
                            captureConstants.CameraPosition = { probePosition.x, probePosition.y, probePosition.z, 0.0f };
                            cmd->UpdateBuffer(m_ProbeCaptureConstantBuffer.get(), &captureConstants, sizeof(captureConstants));

                            cmd->SetRenderTargets(captureTargets, 1, m_ProbeCaptureDepth.get());
                            cmd->SetViewport(probeViewport);
                            cmd->ClearRenderTarget({ 0.0f, 0.0f, 0.0f, 0.0f });
                            // Reverse-Zのため遠平面側(NDC z=0.0)にクリアする。コピー側はこの0を
                            // 「何も描かれなかった=スカイ」の判定に使う
                            cmd->ClearDepth(0.0f);

                            cmd->SetPipelineState(m_ProbeCapturePipelineState.get());
                            cmd->SetConstantBuffer(0, m_ProbeCaptureConstantBuffer.get());
                            cmd->SetSamplerSet(m_MaterialSamplers.get());

                            for (const auto& instance : m_Scene.Instances)
                            {
                                for (const auto& mesh : instance.Model.Meshes)
                                {
                                    const ObjectConstants objectConstants = MakeObjectConstants(instance, mesh);
                                    cmd->UpdateBuffer(m_ObjectConstantBuffer.get(), &objectConstants, sizeof(objectConstants));
                                    cmd->SetConstantBuffer(1, m_ObjectConstantBuffer.get());

                                    cmd->SetVertexBuffer(mesh.VertexBuffer.get());
                                    cmd->SetIndexBuffer(mesh.IndexBuffer.get());

                                    // テクスチャは必ずスロット0から順に、1回の描画ぶんをまとめてバインドする。
                                    // DX12はSetTexture(0, ...)を「新しい描画の開始」とみなしてSRVディスクリプタ
                                    // テーブルのブロックを割り当て直すため、ループの外で4番以降だけを先に
                                    // バインドしても次のブロックへは引き継がれない(=シャドウマップ・IBLが
                                    // 未初期化のディスクリプタのままになり、DX12だけプローブが真っ黒に焼ける)。
                                    // 半透明フォワードパスが同じ理由で同じ順序にしている
                                    cmd->SetTexture(0, mesh.BaseColorTexture);
                                    cmd->SetTexture(1, mesh.NormalTexture);
                                    cmd->SetTexture(2, mesh.MetallicRoughnessTexture);
                                    cmd->SetTexture(3, mesh.EmissiveTexture);
                                    for (uint32_t cascade = 0; cascade < kCascadeCount; ++cascade)
                                    {
                                        cmd->SetTexture(4 + cascade, m_ShadowCascades[cascade].get());
                                    }
                                    cmd->SetShaderResourceBuffer(8, m_LightBuffer.get());
                                    cmd->SetTexture(9, m_IrradianceTexture.get());
                                    cmd->SetTexture(10, m_PrefilteredEnvTexture.get());
                                    cmd->SetTexture(11, m_BRDFLUTTexture.get());

                                    cmd->DrawIndexed(mesh.IndexCount, 0, 0);
                                }
                            }

                            // 描き終えたカラー/深度をコンピュートシェーダーからSRVとして読むため、
                            // 先にレンダーターゲットのバインドを外す(D3D11は同一リソースの
                            // RTV/DSVとSRVの同時バインドを許さず、SRV側がnullに落とされる)
                            cmd->SetRenderTargets(nullptr, 0, nullptr);

                            IBLFaceConstants faceConstants{};
                            faceConstants.Face = face;
                            cmd->SetComputePipelineState(m_ProbeCubeCopyPipelineState.get());
                            cmd->UpdateBuffer(m_IBLPrefilterConstantBuffer.get(), &faceConstants, sizeof(faceConstants));
                            cmd->SetComputeConstantBuffer(0, m_IBLPrefilterConstantBuffer.get());
                            cmd->SetComputeSamplerSet(m_MaterialSamplers.get());
                            cmd->SetComputeTexture(0, m_SkyboxTexture.get());
                            cmd->SetComputeTexture(1, m_ProbeCaptureColor.get());
                            cmd->SetComputeTexture(2, m_ProbeCaptureDepth.get());
                            cmd->SetComputeUnorderedAccessTextureCubeFace(0, m_ProbeRadianceCube.get(), face, 0, 0);
                            cmd->Dispatch((kProbeCaptureSize + 7) / 8, (kProbeCaptureSize + 7) / 8, 1);
                        }

                        // --- 組み上がったキューブマップをIBLとまったく同じ手順で畳み込む ---
                        // 入力(スクラッチのキューブマップ)が違うだけで、シェーダーはIBLBakeパスと共通
                        cmd->SetComputePipelineState(m_IrradiancePipelineState.get());
                        cmd->SetComputeTexture(0, m_ProbeRadianceCube.get());
                        cmd->SetComputeSamplerSet(m_MaterialSamplers.get());
                        for (uint32_t face = 0; face < kCubeFaceCount; ++face)
                        {
                            IBLFaceConstants faceConstants{};
                            faceConstants.Face = face;
                            cmd->UpdateBuffer(m_IBLPrefilterConstantBuffer.get(), &faceConstants, sizeof(faceConstants));
                            cmd->SetComputeConstantBuffer(0, m_IBLPrefilterConstantBuffer.get());
                            cmd->SetComputeUnorderedAccessTextureCubeFace(0, m_ProbeIrradianceArray.get(), face, 0, cubeIndex);
                            cmd->Dispatch((kIBLIrradianceSize + 7) / 8, (kIBLIrradianceSize + 7) / 8, 1);
                        }

                        cmd->SetComputePipelineState(m_PrefilterPipelineState.get());
                        cmd->SetComputeTexture(0, m_ProbeRadianceCube.get());
                        cmd->SetComputeSamplerSet(m_MaterialSamplers.get());
                        for (uint32_t mip = 0; mip < kIBLPrefilterMipLevels; ++mip)
                        {
                            const uint32_t mipSize = std::max(1u, kIBLPrefilterBaseSize >> mip);
                            const float roughness = static_cast<float>(mip) / static_cast<float>(kIBLPrefilterMipLevels - 1);
                            for (uint32_t face = 0; face < kCubeFaceCount; ++face)
                            {
                                IBLFaceConstants faceConstants{};
                                faceConstants.Face = face;
                                faceConstants.Roughness = roughness;
                                cmd->UpdateBuffer(m_IBLPrefilterConstantBuffer.get(), &faceConstants, sizeof(faceConstants));
                                cmd->SetComputeConstantBuffer(0, m_IBLPrefilterConstantBuffer.get());
                                cmd->SetComputeUnorderedAccessTextureCubeFace(0, m_ProbePrefilteredArray.get(), face, mip, cubeIndex);
                                cmd->Dispatch((mipSize + 7) / 8, (mipSize + 7) / 8, 1);
                            }
                        }
                    }
                },
            });

            m_ProbeBakeRequested = false;
            // このフレームの描画時点ではまだ焼き上がっていない(同じコマンドリスト内でこの後の
            // Lightingパスが読むのは問題ないが、gpuProbesは既に確定済み)。次フレームから
            // プローブが有効になるよう、ここでフラグだけ立てる
            m_ProbeBaked = true;
        }

        // --- ジオメトリパス: G-Bufferへ書き込む(常に指定した内部解像度) ---
        graph.AddPass(Core::RenderGraphPassDesc{
            .Name = "GBuffer",
            .RenderTargets = { m_GBufferAlbedo.get(), m_GBufferNormal.get(), m_GBufferMaterial.get(), m_GBufferEmissive.get() },
            .DepthTarget = m_GBufferDepth.get(),
            .Execute = [this, &gbufferViewport](RHI::IRHICommandList* cmd)
            {
                cmd->SetViewport(gbufferViewport);
                cmd->ClearRenderTarget({ 0.0f, 0.0f, 0.0f, 0.0f });
                // Reverse-Zのため遠平面側(NDC z=0.0)にクリアする(GBuffer.hlsl参照)
                cmd->ClearDepth(0.0f);

                cmd->SetPipelineState(m_GBufferPipelineState.get());
                cmd->SetConstantBuffer(0, m_FrameConstantBuffer.get());
                cmd->SetSamplerSet(m_MaterialSamplers.get());

                for (const auto& instance : m_Scene.Instances)
                {
                    for (const auto& mesh : instance.Model.Meshes)
                    {
                        // BLENDマテリアル(mesh.IsTransparent)はG-Bufferに書き込まず、専用のTransparentパスで
                        // フォワードシェーディングする(G-Bufferのアルファは常に1.0で半透明合成ができないため)
                        if (mesh.IsTransparent)
                        {
                            continue;
                        }

                        const ObjectConstants objectConstants = MakeObjectConstants(instance, mesh);
                        cmd->UpdateBuffer(m_ObjectConstantBuffer.get(), &objectConstants, sizeof(objectConstants));
                        cmd->SetConstantBuffer(1, m_ObjectConstantBuffer.get());

                        cmd->SetVertexBuffer(mesh.VertexBuffer.get());
                        cmd->SetIndexBuffer(mesh.IndexBuffer.get());
                        cmd->SetTexture(0, mesh.BaseColorTexture);
                        cmd->SetTexture(1, mesh.NormalTexture);
                        cmd->SetTexture(2, mesh.MetallicRoughnessTexture);
                        cmd->SetTexture(3, mesh.EmissiveTexture);
                        cmd->DrawIndexed(mesh.IndexCount, 0, 0);
                    }
                }
            },
        });

        // --- Hi-Zミップチェーン構築パス: G-Buffer深度から1x1までのミップチェーンをコンピュートシェーダーで
        //     構築する(現時点では利用箇所は無く、デバッグ表示専用) ---
        graph.AddPass(Core::RenderGraphPassDesc{
            .Name = "HiZ",
            .Reads = { m_GBufferDepth.get() },
            .Writes = { m_HiZTexture.get() },
            .Execute = [this](RHI::IRHICommandList* cmd)
            {
                HiZConstants hizConstants{};
                hizConstants.SrcSize = { m_RenderWidth, m_RenderHeight };
                hizConstants.DstSize = { m_RenderWidth, m_RenderHeight };
                cmd->UpdateBuffer(m_HiZConstantBuffer.get(), &hizConstants, sizeof(hizConstants));

                cmd->SetComputePipelineState(m_HiZCopyPipelineState.get());
                cmd->SetComputeConstantBuffer(0, m_HiZConstantBuffer.get());
                cmd->SetComputeTexture(0, m_GBufferDepth.get());
                cmd->SetComputeUnorderedAccessTexture(0, m_HiZTexture.get(), 0);
                cmd->Dispatch((m_RenderWidth + 7) / 8, (m_RenderHeight + 7) / 8, 1);

                cmd->SetComputePipelineState(m_HiZDownsamplePipelineState.get());
                uint32_t hizSrcWidth = m_RenderWidth;
                uint32_t hizSrcHeight = m_RenderHeight;
                for (uint32_t mip = 1; mip < m_HiZMipLevels; ++mip)
                {
                    const uint32_t hizDstWidth = std::max(1u, hizSrcWidth / 2);
                    const uint32_t hizDstHeight = std::max(1u, hizSrcHeight / 2);

                    hizConstants.SrcSize = { hizSrcWidth, hizSrcHeight };
                    hizConstants.DstSize = { hizDstWidth, hizDstHeight };
                    cmd->UpdateBuffer(m_HiZConstantBuffer.get(), &hizConstants, sizeof(hizConstants));
                    cmd->SetComputeConstantBuffer(0, m_HiZConstantBuffer.get());
                    cmd->SetComputeUnorderedAccessTexture(0, m_HiZTexture.get(), mip - 1);
                    cmd->SetComputeUnorderedAccessTexture(1, m_HiZTexture.get(), mip);
                    cmd->Dispatch((hizDstWidth + 7) / 8, (hizDstHeight + 7) / 8, 1);

                    hizSrcWidth = hizDstWidth;
                    hizSrcHeight = hizDstHeight;
                }
            },
        });

        // --- 直接光パス: G-Buffer+シャドウマップからPBRの直接光(拡散+鏡面反射、シャドウ適用済み)を
        //     計算しHDRで書き出す(常に指定した内部解像度)。DeferredLighting/SSILの両方から読まれる ---
        graph.AddPass(Core::RenderGraphPassDesc{
            .Name = "DirectLight",
            .Reads =
            {
                m_GBufferAlbedo.get(), m_GBufferNormal.get(), m_GBufferMaterial.get(), m_GBufferDepth.get(),
                m_ShadowCascades[0].get(), m_ShadowCascades[1].get(), m_ShadowCascades[2].get(), m_ShadowCascades[3].get(),
                // スペキュラのエネルギー補正(14.9節)でEss=brdf.x+brdf.yを引くためBRDF積分LUTを読む。
                // Readsに挙げることでRenderGraphがIBLBakeパス(このLUTのWriter)より後に順序付ける
                m_BRDFLUTTexture.get(),
            },
            .RenderTargets = { m_DirectLightTexture.get() },
            .Execute = [this, &gbufferViewport, &gpuLights](RHI::IRHICommandList* cmd)
            {
                cmd->SetViewport(gbufferViewport);

                cmd->SetPipelineState(m_DirectLightPipelineState.get());
                cmd->SetConstantBuffer(0, m_FrameConstantBuffer.get());

                LightingConstants lightingConstants{};
                lightingConstants.LightCount = { static_cast<uint32_t>(gpuLights.size()), 0u, 0u, 0u };
                // UpdateBufferはSetConstantBufferより前に呼ぶ必要がある。DX12の定数バッファは
                // リングバッファで、GetGPUVirtualAddress()が現在のリングスロットのアドレスを返すため
                cmd->UpdateBuffer(m_LightingConstantBuffer.get(), &lightingConstants, sizeof(lightingConstants));
                cmd->SetConstantBuffer(1, m_LightingConstantBuffer.get());

                cmd->SetSamplerSet(m_ScreenSpaceSamplers.get());
                cmd->SetTexture(0, m_GBufferAlbedo.get());
                cmd->SetTexture(1, m_GBufferNormal.get());
                cmd->SetTexture(2, m_GBufferMaterial.get());
                cmd->SetTexture(3, m_GBufferDepth.get());
                for (uint32_t cascade = 0; cascade < kCascadeCount; ++cascade)
                {
                    cmd->SetTexture(4 + cascade, m_ShadowCascades[cascade].get());
                }

                // ライトが1つも無いフレームでもSetShaderResourceBufferは必ず呼ぶ(SetPipelineStateが
                // 毎回ルート引数を無効化するため、シェーダが宣言しているリソースを未バインドのまま
                // Drawすることになってしまう)。UpdateBuffer自体は0灯なら省略してよい
                if (!gpuLights.empty())
                {
                    cmd->UpdateBuffer(m_LightBuffer.get(), gpuLights.data(), gpuLights.size() * sizeof(GPULight));
                }
                cmd->SetShaderResourceBuffer(8, m_LightBuffer.get());
                // スペキュラのエネルギー補正(14.9節)用のBRDF積分LUT。t8はライトリスト
                // (StructuredBuffer)が占有しているためt9に置く
                cmd->SetTexture(9, m_BRDFLUTTexture.get());

                cmd->Draw(3, 0);
            },
        });

        // --- AO/GIパス: 選択中の手法(SSAO or SSIL)でG-Bufferから遮蔽率(・間接拡散光)を計算し、
        //     ブラーで均す(常に指定した内部解像度)。出力フォーマットはどちらもrgb=間接拡散光, a=遮蔽率で共通 ---
        if (m_AOEnabled)
        {
            RHI::IRHITexture* aoRawTexture = (m_AOTechnique == AOTechnique::SSAO) ? m_SSAORawTexture.get() : m_SSILRawTexture.get();
            RHI::IRHITexture* aoBlurredTexture = (m_AOTechnique == AOTechnique::SSAO) ? m_SSAOTexture.get() : m_SSILTexture.get();

            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "AO",
                .Reads = (m_AOTechnique == AOTechnique::SSAO)
                    ? std::vector<RHI::IRHITexture*>{ m_GBufferNormal.get(), m_GBufferDepth.get() }
                    : std::vector<RHI::IRHITexture*>{ m_GBufferNormal.get(), m_GBufferDepth.get(), m_DirectLightTexture.get() },
                .RenderTargets = { aoRawTexture },
                .Execute = [this, &gbufferViewport](RHI::IRHICommandList* cmd)
                {
                    cmd->SetViewport(gbufferViewport);
                    cmd->SetConstantBuffer(0, m_FrameConstantBuffer.get());
                    cmd->SetSamplerSet(m_ScreenSpaceSamplers.get());

                    if (m_AOTechnique == AOTechnique::SSAO)
                    {
                        SSAOConstants ssaoConstants{};
                        std::copy(m_SSAOKernel.begin(), m_SSAOKernel.end(), ssaoConstants.Samples);
                        ssaoConstants.Params = { m_SSAORadius, m_SSAORadius * 0.05f, m_SSAOPower, 0.0f };
                        cmd->UpdateBuffer(m_SSAOConstantBuffer.get(), &ssaoConstants, sizeof(ssaoConstants));

                        cmd->SetPipelineState(m_SSAOPipelineState.get());
                        cmd->SetConstantBuffer(1, m_SSAOConstantBuffer.get());
                        cmd->SetTexture(0, m_GBufferNormal.get());
                        cmd->SetTexture(1, m_GBufferDepth.get());
                        cmd->Draw(3, 0);
                    }
                    else
                    {
                        SSILConstants ssilConstants{};
                        ssilConstants.Params0 = { m_SSILRadius, m_SSILThickness, m_SSILIntensity, m_SSILPower };
                        ssilConstants.Params1 = { m_SSILSliceCount, m_SSILStepCount, 0u, 0u };
                        cmd->UpdateBuffer(m_SSILConstantBuffer.get(), &ssilConstants, sizeof(ssilConstants));

                        cmd->SetPipelineState(m_SSILPipelineState.get());
                        cmd->SetConstantBuffer(1, m_SSILConstantBuffer.get());
                        cmd->SetTexture(0, m_GBufferNormal.get());
                        cmd->SetTexture(1, m_GBufferDepth.get());
                        cmd->SetTexture(2, m_DirectLightTexture.get());
                        cmd->Draw(3, 0);
                    }
                },
            });

            // ブラーパス: 遮蔽率・間接拡散光のタイル状ノイズをボックスブラーで均す(SSAO/SSIL共通シェーダ)
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "AOBlur",
                .Reads = { aoRawTexture },
                .RenderTargets = { aoBlurredTexture },
                .Execute = [this, &gbufferViewport, aoRawTexture](RHI::IRHICommandList* cmd)
                {
                    cmd->SetViewport(gbufferViewport);
                    cmd->SetPipelineState(m_AOBlurPipelineState.get());
                    // ブラーはカーネルのタップが画面端で[0,1]を出るため、Wrapのサンプラーが
                    // 1つも入っていないこのセットを明示的にバインドする(以前は直前のパスの
                    // バインドがそのまま残るのに依存していた)
                    cmd->SetSamplerSet(m_ScreenSpaceSamplers.get());
                    cmd->SetTexture(0, aoRawTexture);
                    cmd->Draw(3, 0);
                },
            });
        }

        // デバッグ表示(ブラー前確認用)のため、ブラー前の生バッファへの参照も別途保持しておく
        RHI::IRHITexture* activeAOTexture = m_AODisabledTexture.get();
        RHI::IRHITexture* activeAORawTexture = m_AODisabledTexture.get();
        if (m_AOEnabled)
        {
            activeAOTexture = (m_AOTechnique == AOTechnique::SSAO) ? m_SSAOTexture.get() : m_SSILTexture.get();
            activeAORawTexture = (m_AOTechnique == AOTechnique::SSAO) ? m_SSAORawTexture.get() : m_SSILRawTexture.get();
        }

        // --- ライティングパス: G-Bufferを読み、SceneColorへ出力(常に指定した内部解像度) ---
        graph.AddPass(Core::RenderGraphPassDesc{
            .Name = "Lighting",
            .Reads = {
                m_GBufferAlbedo.get(), m_DirectLightTexture.get(), m_GBufferMaterial.get(), m_GBufferDepth.get(),
                m_SkyboxTexture.get(), activeAOTexture, m_GBufferEmissive.get(), m_GBufferNormal.get(),
                m_IrradianceTexture.get(), m_PrefilteredEnvTexture.get(), m_BRDFLUTTexture.get(),
                // ProbeBakeパスより後に順序付けさせるために挙げる(実際のバインドはExecute内)
                m_ProbeIrradianceArray.get(), m_ProbePrefilteredArray.get(),
            },
            .RenderTargets = { m_SceneColor.get() },
            .Execute = [this, &gbufferViewport, activeAOTexture](RHI::IRHICommandList* cmd)
            {
                cmd->SetViewport(gbufferViewport);
                // 深度テストに失敗した(=何も描かれていない)ピクセル用の背景色。discardされた箇所に前フレームのデータが
                // 残らないよう、フルスクリーン三角形を描く前に明示的にクリアしておく
                cmd->ClearRenderTarget({ 0.05f, 0.05f, 0.08f, 1.0f });

                cmd->SetPipelineState(m_LightingPipelineState.get());
                cmd->SetConstantBuffer(0, m_FrameConstantBuffer.get());
                cmd->SetSamplerSet(m_ScreenSpaceSamplers.get());
                cmd->SetTexture(0, m_GBufferAlbedo.get());
                cmd->SetTexture(1, m_DirectLightTexture.get());
                cmd->SetTexture(2, m_GBufferMaterial.get());
                cmd->SetTexture(3, m_GBufferDepth.get());
                cmd->SetTexture(4, m_SkyboxTexture.get());
                cmd->SetTexture(5, activeAOTexture);
                cmd->SetTexture(6, m_GBufferEmissive.get());
                cmd->SetTexture(7, m_GBufferNormal.get());
                cmd->SetTexture(8, m_IrradianceTexture.get());
                cmd->SetTexture(9, m_PrefilteredEnvTexture.get());
                cmd->SetTexture(10, m_BRDFLUTTexture.get());
                // 反射プローブ(15章)。FrameConstants.ProbeParams.xが0のとき(未ベイク・無効時)は
                // シェーダー側が選択ループを回さないため中身は参照されないが、DX12は
                // ディスクリプタテーブルに未初期化のスロットが残ると動作が未定義になるため常にバインドする
                cmd->SetTexture(11, m_ProbeIrradianceArray.get());
                cmd->SetTexture(12, m_ProbePrefilteredArray.get());
                cmd->SetShaderResourceBuffer(13, m_ProbeBuffer.get());
                cmd->Draw(3, 0);
            },
        });

        // --- 半透明フォワードパス: glTFのalphaMode=BLENDのメッシュ(mesh.IsTransparent)だけを、
        //     LightingパスのSceneColorの上にカメラから遠い順(奥から手前)でアルファブレンド合成する。
        //     深度テストはGBuffer深度に対して行うが書き込みは行わない(半透明パイプラインステートの
        //     DepthWriteEnabled=false)ため、不透明物体には隠れる一方、半透明同士は常に描画順で
        //     正しく重なる。RenderTargets/DepthTargetにSceneColor/GBuffer深度を指定しているだけで
        //     ClearRenderTarget/ClearDepthは呼ばないため、Lightingパスが書いた内容の上に描き足す形になる ---
        graph.AddPass(Core::RenderGraphPassDesc{
            .Name = "Transparent",
            .RenderTargets = { m_SceneColor.get() },
            .DepthTarget = m_GBufferDepth.get(),
            .Execute = [this, &gbufferViewport, &gpuLights, &cameraPosition](RHI::IRHICommandList* cmd)
            {
                // 半透明メッシュをインスタンス単位でカメラからの距離降順(奥から手前)に並べる。
                // instance.WorldはHLSL(mul(vec, World))に合わせて転置済みのため、ワールド座標の
                // 平行移動成分は行ではなく列(_14/_24/_34)に入っている
                struct TransparentDraw
                {
                    const Assets::ModelInstance* Instance;
                    const Assets::Mesh* Mesh;
                    float DistanceSq;
                };
                std::vector<TransparentDraw> draws;
                for (const auto& instance : m_Scene.Instances)
                {
                    const float dx = instance.World._14 - cameraPosition.x;
                    const float dy = instance.World._24 - cameraPosition.y;
                    const float dz = instance.World._34 - cameraPosition.z;
                    const float distanceSq = dx * dx + dy * dy + dz * dz;
                    for (const auto& mesh : instance.Model.Meshes)
                    {
                        if (!mesh.IsTransparent)
                        {
                            continue;
                        }
                        draws.push_back({ &instance, &mesh, distanceSq });
                    }
                }
                if (draws.empty())
                {
                    return;
                }
                std::sort(
                    draws.begin(), draws.end(),
                    [](const TransparentDraw& a, const TransparentDraw& b) { return a.DistanceSq > b.DistanceSq; });

                cmd->SetViewport(gbufferViewport);
                cmd->SetPipelineState(m_TransparentPipelineState.get());
                cmd->SetConstantBuffer(0, m_FrameConstantBuffer.get());
                cmd->SetSamplerSet(m_MaterialSamplers.get());

                if (!gpuLights.empty())
                {
                    cmd->UpdateBuffer(m_LightBuffer.get(), gpuLights.data(), gpuLights.size() * sizeof(GPULight));
                }

                for (const TransparentDraw& draw : draws)
                {
                    const ObjectConstants objectConstants = MakeObjectConstants(*draw.Instance, *draw.Mesh);
                    cmd->UpdateBuffer(m_ObjectConstantBuffer.get(), &objectConstants, sizeof(objectConstants));
                    cmd->SetConstantBuffer(1, m_ObjectConstantBuffer.get());

                    cmd->SetVertexBuffer(draw.Mesh->VertexBuffer.get());
                    cmd->SetIndexBuffer(draw.Mesh->IndexBuffer.get());
                    // t0〜t11の12スロット全てをDrawIndexedごとに再設定する。DX12はslot 0のSetTextureで
                    // 新しい描画のSRVテーブルを確定させるため(DX12CommandList::FlushPendingSrvWrites参照)、
                    // shadow/light/IBLのように値が変わらないスロットも含め毎回セットし直す必要がある
                    cmd->SetTexture(0, draw.Mesh->BaseColorTexture);
                    cmd->SetTexture(1, draw.Mesh->NormalTexture);
                    cmd->SetTexture(2, draw.Mesh->MetallicRoughnessTexture);
                    cmd->SetTexture(3, draw.Mesh->EmissiveTexture);
                    for (uint32_t cascade = 0; cascade < kCascadeCount; ++cascade)
                    {
                        cmd->SetTexture(4 + cascade, m_ShadowCascades[cascade].get());
                    }
                    cmd->SetShaderResourceBuffer(8, m_LightBuffer.get());
                    // IBL(14章)。このパスにはSSRが適用されないため、半透明サーフェスの環境の
                    // 映り込みはこの3枚だけが担う
                    cmd->SetTexture(9, m_IrradianceTexture.get());
                    cmd->SetTexture(10, m_PrefilteredEnvTexture.get());
                    cmd->SetTexture(11, m_BRDFLUTTexture.get());

                    cmd->DrawIndexed(draw.Mesh->IndexCount, 0, 0);
                }
            },
        });

        // --- SSRパス: LightingパスのSceneColorとG-Bufferから鏡面反射を計算し加算する。
        //     無効時はスキップし、Presentが直接m_SceneColorを参照する ---
        if (m_SSREnabled)
        {
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "SSR",
                .Reads = { m_SceneColor.get(), m_GBufferNormal.get(), m_GBufferMaterial.get(), m_GBufferDepth.get(), m_SkyboxTexture.get(), m_GBufferAlbedo.get() },
                .RenderTargets = { m_SSRTexture.get() },
                .Execute = [this, &gbufferViewport](RHI::IRHICommandList* cmd)
                {
                    SSRConstants ssrConstants{};
                    ssrConstants.Params0 = { m_SSRMaxDistance, m_SSRThickness, m_SSRRoughnessCutoff, 0.0f };
                    cmd->UpdateBuffer(m_SSRConstantBuffer.get(), &ssrConstants, sizeof(ssrConstants));

                    cmd->SetViewport(gbufferViewport);
                    cmd->SetPipelineState(m_SSRPipelineState.get());
                    cmd->SetConstantBuffer(0, m_FrameConstantBuffer.get());
                    cmd->SetConstantBuffer(1, m_SSRConstantBuffer.get());
                    cmd->SetSamplerSet(m_ScreenSpaceSamplers.get());
                    cmd->SetTexture(0, m_SceneColor.get());
                    cmd->SetTexture(1, m_GBufferNormal.get());
                    cmd->SetTexture(2, m_GBufferMaterial.get());
                    cmd->SetTexture(3, m_GBufferDepth.get());
                    cmd->SetTexture(4, m_SkyboxTexture.get());
                    cmd->SetTexture(5, m_GBufferAlbedo.get());
                    cmd->Draw(3, 0);
                },
            });
        }

        // --- Tonemapパス: HDRのSceneColor(SSR有効時はSSR適用後)をLDRへ変換する。
        //     SSR等のHDR演算がすべて完了した後、Present直前の独立したステージとして常に実行する ---
        RHI::IRHITexture* hdrSceneColor = m_SSREnabled ? m_SSRTexture.get() : m_SceneColor.get();
        graph.AddPass(Core::RenderGraphPassDesc{
            .Name = "Tonemap",
            .Reads = { hdrSceneColor },
            .RenderTargets = { m_TonemapTexture.get() },
            .Execute = [this, &gbufferViewport, hdrSceneColor](RHI::IRHICommandList* cmd)
            {
                cmd->SetViewport(gbufferViewport);
                cmd->SetPipelineState(m_TonemapPipelineState.get());
                cmd->SetSamplerSet(m_ScreenSpaceSamplers.get());
                cmd->SetTexture(0, hdrSceneColor);
                cmd->Draw(3, 0);
            },
        });

        // --- Presentパス: 選択中のレンダーターゲットを、アスペクト比を保ってバックバッファへ出力 ---
        // デバッグ表示(Render Targets UI)で選択されたバッファに応じて表示ソースを切り替える。
        // 深度バッファ(GBuffer深度・シャドウマップ)はPresent.hlsl側でグレースケール化するためMode=1を渡す
        RHI::IRHITexture* presentSourceTexture = m_TonemapTexture.get();
        // Mode 9(IBL Irradiance/Prefilterのキューブマップ表示)専用。他のModeでは使われないが、
        // t1には常に何らかの有効なTextureCubeをバインドしておく必要があるため既定値を持たせる
        RHI::IRHITexture* presentDebugCubeTexture = m_SkyboxTexture.get();
        // Mode 10(反射プローブのキューブマップ配列)専用。Mode 9のTextureCubeとは型が違うため
        // 別スロット(t2)が要る。こちらも常に有効なテクスチャをバインドしておく必要がある
        RHI::IRHITexture* presentDebugCubeArrayTexture = m_ProbeIrradianceArray.get();
        int32_t presentMode = 0;
        uint32_t presentSourceWidth = m_RenderWidth;
        uint32_t presentSourceHeight = m_RenderHeight;
        switch (m_DebugView)
        {
        case DebugView::Final:
            // Tonemapパスが既にSSR有効/無効を考慮したHDRソースをLDR変換済みのため、そのまま使う
            presentSourceTexture = m_TonemapTexture.get();
            break;
        case DebugView::Albedo:
            presentSourceTexture = m_GBufferAlbedo.get();
            break;
        case DebugView::Normal:
            presentSourceTexture = m_GBufferNormal.get();
            presentMode = 7; // オクタヘドラルエンコードをデコードして[0,1]へ再マップして表示
            break;
        case DebugView::Material:
            presentSourceTexture = m_GBufferMaterial.get();
            break;
        case DebugView::Emissive:
            presentSourceTexture = m_GBufferEmissive.get();
            break;
        case DebugView::Depth:
            presentSourceTexture = m_GBufferDepth.get();
            presentMode = 2;
            break;
        case DebugView::DepthRaw:
            presentSourceTexture = m_GBufferDepth.get();
            presentMode = 5; // 生の深度値(0〜1)を加工せずそのまま表示(reverse-z等の生値確認用)
            break;
        case DebugView::DirectLight:
            presentSourceTexture = m_DirectLightTexture.get();
            presentMode = 4; // HDRのためトーンマッピング(Reinhard)+ガンマ補正して表示
            break;
        case DebugView::AOIndirectLight:
            presentSourceTexture = activeAOTexture;
            presentMode = 0; // rgb(間接拡散光)をそのまま表示。SSAOはrgbが常に0のため常に黒になる
            break;
        case DebugView::AOIndirectLightRaw:
            presentSourceTexture = activeAORawTexture;
            presentMode = 0; // ブラー前の生値(タイル状ノイズが乗った状態)
            break;
        case DebugView::AOOcclusion:
            presentSourceTexture = activeAOTexture;
            presentMode = 3; // a(遮蔽率)をグレースケール表示
            break;
        case DebugView::AOOcclusionRaw:
            presentSourceTexture = activeAORawTexture;
            presentMode = 3; // ブラー前の生値(タイル状ノイズが乗った状態)
            break;
        case DebugView::ShadowMap:
            presentSourceTexture = m_ShadowCascades[std::clamp(m_ShadowDebugCascade, 0, static_cast<int32_t>(kCascadeCount) - 1)].get();
            presentMode = 1;
            presentSourceWidth = kShadowMapSize;
            presentSourceHeight = kShadowMapSize;
            break;
        case DebugView::SSR:
            // SSR無効時はSSRパスをスキップしているため、Tonemapパスの入力もSceneColorになり
            // 結果的にFinalと同一表示になる
            presentSourceTexture = m_TonemapTexture.get();
            break;
        case DebugView::HiZ:
            presentSourceTexture = m_HiZTexture.get();
            presentMode = 6; // 指定ミップをSampleLevelで読みグレースケール表示
            presentSourceWidth = std::max(1u, m_RenderWidth >> m_HiZDebugMipLevel);
            presentSourceHeight = std::max(1u, m_RenderHeight >> m_HiZDebugMipLevel);
            break;
        case DebugView::IBLIrradiance:
            // 本物のTextureCubeのため、SourceTexture(t0、Texture2D)ではなくDebugCubeTexture(t1)を
            // 現在のカメラ視線方向でサンプルする(Present.hlsl Mode 9、presentDebugCubeTexture参照)
            presentDebugCubeTexture = m_IrradianceTexture.get();
            presentMode = 9;
            presentSourceWidth = m_RenderWidth;
            presentSourceHeight = m_RenderHeight;
            break;
        case DebugView::IBLPrefilter:
            presentDebugCubeTexture = m_PrefilteredEnvTexture.get();
            presentMode = 9;
            presentSourceWidth = m_RenderWidth;
            presentSourceHeight = m_RenderHeight;
            break;
        case DebugView::ProbeIrradiance:
            // キューブマップ配列のためMode 9(TextureCube)ではなくMode 10(TextureCubeArray)を使う
            presentDebugCubeArrayTexture = m_ProbeIrradianceArray.get();
            presentMode = 10;
            break;
        case DebugView::ProbePrefilter:
            presentDebugCubeArrayTexture = m_ProbePrefilteredArray.get();
            presentMode = 10;
            break;
        case DebugView::ProbeInfluence:
            // 塗り分けはDeferredLighting.hlsl側(FrameConstants.ProbeParams.y)で行うため、
            // Presentは通常どおり最終結果を表示するだけでよい
            presentSourceTexture = m_TonemapTexture.get();
            break;
        case DebugView::IBLBRDFLUT:
            presentSourceTexture = m_BRDFLUTTexture.get();
            presentMode = 0; // (scale, bias)の生値をそのままRGとして表示(値域はおおむね[0,1])
            presentSourceWidth = kIBLBRDFLUTSize;
            presentSourceHeight = kIBLBRDFLUTSize;
            break;
        }

        PresentConstants presentConstants{};
        presentConstants.Mode = presentMode;
        if (m_DebugView == DebugView::IBLPrefilter)
        {
            presentConstants.MipLevel = static_cast<float>(m_IBLPrefilterDebugMipLevel);
        }
        else if (m_DebugView == DebugView::IBLIrradiance || m_DebugView == DebugView::ProbeIrradiance)
        {
            presentConstants.MipLevel = 0.0f; // イラディアンスマップは常に1ミップのみ
        }
        else if (m_DebugView == DebugView::ProbePrefilter)
        {
            presentConstants.MipLevel = static_cast<float>(m_ProbePrefilterDebugMipLevel);
        }
        else
        {
            presentConstants.MipLevel = static_cast<float>(m_HiZDebugMipLevel);
        }
        // プローブが1つも無い場合でも配列の範囲外を引かないようクランプする
        presentConstants.ArrayIndex = static_cast<float>(
            std::clamp(m_ProbeDebugIndex, 0, std::max(0, static_cast<int32_t>(m_ReflectionProbes.size()) - 1)));
        commandList->UpdateBuffer(m_PresentConstantBuffer.get(), &presentConstants, sizeof(presentConstants));

        // レターボックス/ピラーボックスの余白もクリア色のまま残るよう、絞ったビューポートで描画する
        const RHI::Viewport letterboxViewport = ComputeLetterboxViewport(
            m_Window->GetWidth(), m_Window->GetHeight(), presentSourceWidth, presentSourceHeight);

        graph.AddPass(Core::RenderGraphPassDesc{
            .Name = "Present",
            .Reads = { presentSourceTexture, presentDebugCubeTexture, presentDebugCubeArrayTexture },
            .SwapChainTarget = m_SwapChain.get(),
            .Execute =
                [this, &letterboxViewport, presentSourceTexture, presentDebugCubeTexture, presentDebugCubeArrayTexture](
                    RHI::IRHICommandList* cmd)
            {
                cmd->ClearRenderTarget({ 0.05f, 0.05f, 0.08f, 1.0f });
                cmd->ClearDepth(1.0f);
                cmd->SetViewport(letterboxViewport);

                cmd->SetPipelineState(m_PresentPipelineState.get());
                cmd->SetConstantBuffer(0, m_FrameConstantBuffer.get());
                cmd->SetConstantBuffer(1, m_PresentConstantBuffer.get());
                cmd->SetSamplerSet(m_ScreenSpaceSamplers.get());
                cmd->SetTexture(0, presentSourceTexture);
                cmd->SetTexture(1, presentDebugCubeTexture);
                cmd->SetTexture(2, presentDebugCubeArrayTexture);
                cmd->Draw(3, 0);
            },
        });

        graph.Execute();

        // ImGuiはPresentパスでバインドされたバックバッファにそのまま重ねて描画する。
        // GPU側は計測していない(このスコープ専用の描画パイプラインを持たないため)が、
        // CPU側のコマンド記録コストはDX11/DX12で差が出やすいのでここも計測しておく
        m_CPUProfiler.BeginScope("ImGui");
        m_ImGuiBackend->Render();
        m_CPUProfiler.EndScope(); // ImGui

        // Present呼び出しでコマンドリストが実行投入される(DX12)ため、それより前にEndFrame()で
        // フレーム終端のタイムスタンプ書き込み・結果リードバックのコマンドを記録しておく必要がある
        m_GPUProfiler->EndFrame();

        // ExecuteCommandLists・実際のPresent・(DX12のみ)フェンス待ちを含む区間。
        // Present呼び出し自体のCPUコストはここで計測しないと、各パスのコマンド記録時間の
        // 合計とCPU Frame Time全体の差分がどこにあるのか分からなくなるため計測しておく
        m_CPUProfiler.BeginScope("PresentSubmit");
        m_SwapChain->Present(m_VSyncEnabled);
        m_CPUProfiler.EndScope(); // PresentSubmit

        // GPUの完了待ち(DX12のフレームパイプライン化に伴うフェンス待ち)は実際のCPU負荷ではなく
        // GPU側の処理時間の反映なので、PresentSubmitの計測値からは除外しておく
        m_CPUProfiler.SubtractFromScope("PresentSubmit", m_Device->GetLastFrameGPUWaitTimeMs());
    }
}
