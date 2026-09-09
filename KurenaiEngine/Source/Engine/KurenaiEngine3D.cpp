#include "KurenaiEngine3D.h"

#include <imgui.h>

#include <objbase.h>

#include <algorithm>
#include <cstddef>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <limits>
#include <random>

#include "Assets/SceneLoader.h"
#include "Core/Logger.h"
#include "Core/RenderGraph.h"
#include "Core/StringUtil.h"
#include "Passes/EnvironmentPasses.h"
#include "Passes/GeometryConstants.h"
#include "Passes/GeometryPasses.h"
#include "Passes/DDGIPasses.h"
#include "Passes/LightingConstants.h"
#include "Passes/LightingPasses.h"
#include "Passes/MegaLightsConstants.h"
#include "Passes/MegaLightsPasses.h"
#include "Passes/PostProcessPasses.h"
#include "Passes/ReflectionPasses.h"
#include "Passes/ReflectionProbePasses.h"
#include "Passes/ShadowPasses.h"
#include "Passes/PresentPass.h"
#include "Rendering/ExposureMath.h"
#include "Rendering/CubeFaceMath.h"
#include "Rendering/CloudTransmittance.h"
#include "Rendering/GPULight.h"
#include "Rendering/GPULightBuild.h"
#include "Rendering/GPUReflectionProbe.h"
#include "Rendering/SampleSequence.h"
#include "Rendering/GeometryDrawLoop.h"
#include "Rendering/ObjectConstants.h"
#include "Rendering/SunLighting.h"
#include "Rendering/RenderBlackboard.h"
#include "Rendering/RenderFrameContext.h"
#include "ShaderInterop/CascadeConstants.h"
#include "ShaderInterop/FrameConstants.h"
#include "ShaderInterop/GroupSizes.h"
#include "ShaderInterop/MegaLightsStochasticConstants.h"
#include "UI/UIManager.h"
#include "UI/UITheme.h"

namespace Kurenai
{
    namespace
    {
        using Core::GetModuleDirectory;
        using Core::WideToUtf8;

        // 視錐台カリングの一式は Rendering/GeometryDrawLoop.h へ移した。
        // 描画パスの共通ループ(ForEachGeometryDraw)と同じ場所にある必要がある
        using Rendering::FrustumPlanes;
        using Rendering::ComputeCloudAverageTransmittance;
        using Rendering::kMaxDrones;
        using Rendering::kMaxLights;
        using Rendering::kTAAJitterSampleCount;
        using Rendering::MakeGPULight;
        using Rendering::RadicalInverse;
        using Rendering::ExtractFrustumPlanes;
        using Rendering::IsAABBVisible;
        using Rendering::IsMeshVisibleWithStats;

        // モデル描画(G-Bufferパス)の頂点入力レイアウト。PSOの作り直し
        // (CreatePrecisionDependentPipelineStates)からも使うため関数にしてある
        std::vector<RHI::InputElementDesc> GetModelInputLayout()
        {
            return
            {
                { "POSITION", 0, RHI::Format::R32G32B32_Float, 0 },
                { "NORMAL", 0, RHI::Format::R32G32B32_Float, 12 },
                { "TEXCOORD", 0, RHI::Format::R32G32_Float, 24 },
                { "TANGENT", 0, RHI::Format::R32G32B32A32_Float, 32 },
                // ライトマップUV(遮蔽マップ専用)。Assets::Vertex::UV1
                { "TEXCOORD", 1, RHI::Format::R32G32_Float, 48 },
            };
        }

        // FrameConstantsは全描画パスがregister(b0)で共有し、24本のHLSLがこのレイアウトに
        // 依存する。宣言はShaderInterop/FrameConstants.hに1本だけ置き、HLSL側の
        // ShaderInterop/FrameConstants.hlsliと1対1で対応させている
        // (食い違いはFrameConstants.hのoffsetofのstatic_assertが止める)。
        // ここでは無名名前空間の中でだけ短い名前で使えるようにする
        using ShaderInterop::FrameConstants;
        // カスケード数はShaderInterop側が独立に持っている(あちらはKurenaiEngine3Dに
        // 依存しない)。食い違うとCascadeViewProj以降のオフセットが全部ずれるので、
        // ここで突き合わせる(kDDGILODCountの同じ検査はkDDGIMaxLODCountがprivateのため
        // メンバ関数の側にある)
        static_assert(
            ShaderInterop::kCascadeCount == KurenaiEngine3D::kCascadeCount,
            "FrameConstantsのカスケード数がKurenaiEngine3D::kCascadeCountと食い違っている");

        // シャドウパスの各カスケード描画専用(FrameConstantsとは別バッファ)。
        // 宣言は ShaderInterop/CascadeConstants.h に1本だけ置いている
        using ShaderInterop::CascadeConstants;

        // 直射日光の照度 kSunIlluminanceLux は Rendering/SunLighting.h へ移した。
        // 可変プリ露出を決める UpdateEffectiveExposure が別の翻訳単位にあり、そこからも引くため
        // 空光(直射日光を除いた間接照度)の照度[lx]。同テーブルの曇天相当値を、直射日光に対する
        // 空光の比率(おおむね1〜2割)としても妥当な範囲であることの根拠として採用する。
        // 手続き空の天頂輝度の正規化目標にもなるためRender()からも参照する
        constexpr float kSkylightIlluminanceLux = 20000.0f;
        // 満月が地表へ与える照度[lx]。太陽(10万lx)の約40万分の1という実測値。
        // 満ち欠けは未実装(常に満月)。位置は時刻に連動せず、ImGuiで手動指定する
        constexpr float kMoonIlluminanceLux = 0.25f;
        // 満月時に夜空全体が散乱で持つ照度[lx]。地表照度0.25lxのうち空由来の寄与にあたる概算値。
        //
        // 【月と夜空の比が夜の影の見え方を決める】影の濃さは「平行光(月) : 環境光(夜空)」の比で
        // 決まる。物理値の0.25:0.05は5:1で、影は十分な濃さを持つ。この比を保ったまま
        // 表示上の明るさだけを調整したい場合は、照度ではなく自動露出の
        // m_PostProcessSettings.AutoExposureNightRolloffEV(夜の露出切り詰め量)を動かすこと
        constexpr float kMoonSkyIlluminanceLux = 0.05f;
        // 星明かりだけの夜空の照度[lx]。月が地平線下にあるときの下限になる。
        // 月の位置は手動指定なので「月の出ていない夜」もスライダー一つで作れる。
        // そこで夜空の目標照度が厳密に0になると空が真っ黒になり、
        // 自動露出が持ち上げようのない画になる。星明かりは実在する量(約0.001lx)なので、
        // アート的な下駄ではなく物理値としてここに置く
        constexpr float kStarlightIlluminanceLux = 0.001f;

        // edge0とedge1の間をなめらかに0→1で補間する(edge0以下は0、edge1以上は1)
        float Smoothstep(float edge0, float edge1, float x)
        {
            const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        }

        // --- 手続き空(SkyGenerate.hlsl)の色味・照度正規化はGPU側(SkyIntegrate.hlsl)に
        //     一本化してある。**ここへCPUミラーを置いてはいけない**(Sky.hlsliの同じ式と
        //     二重実装になり、「片方を直したら必ずもう片方も直す」規約でしか整合が保てない)。
        //     GPUSkyParameters/m_SkyResources.ParametersBufferの定義とコメントは
        //     このファイル内の該当箇所(GPU用構造体の宣言、Render()のbakeSkyThisFrameブロック)を
        //     参照。式の実体はShaders/3D/Sky.hlsliのComputeSkyTintSet/PerezRelativeLuminance/
        //     SkyTintFromSetと、それを呼ぶShaders/3D/SkyIntegrate.hlslにある ---

        // Sky.hlsliのkCloudNoisePeriodと同じ値であること。CPU側(RenderThreadMainの
        // m_CloudScrollOffset更新)がこの値でstd::fmodして風のスクロール位相を巻き戻しており、
        // ずれるとCPU側で巻き戻した位置とシェーダー側の周期境界が食い違い、風が吹くたびに
        // 雲がジャンプする
        constexpr float kCloudNoisePeriod = 256.0f;

        // 環境の照度[lx]から「そのシーンの基準EV100」を求める。
        //
        // 自動露出のヒストグラムと違い、これは**画面に何が写っているかに一切依存しない**。
        // 測光値が構図で振れる(空が画面に占める割合で2〜3.5段動く)のを抑えるための
        // 足がかりとして使う(AutoExposure.hlsl の KeyReferenceEV100 参照)。
        //
        // 導出: 反射率ρのLambertian面が照度Eを受けたときの輝度は L = E·ρ/π。
        // EV100と輝度の関係は L = 2^EV100 · K/S(反射光式露出計の標準、K=12.5・S=100)
        // すなわち EV100 = log2(8L)。ρには中庸なグレーの18%を使う。
        // 検算: E=100,000lx(直射日光) → EV100=15.5、E=0.3lx(満月の夜) → EV100=-2.9。
        // どちらも実写の露出値と一致する
        float ComputeReferenceEV100(float illuminanceLux)
        {
            constexpr float kMiddleGreyReflectance = 0.18f;
            const float luminance =
                std::max(illuminanceLux, 1e-6f) * kMiddleGreyReflectance / DirectX::XM_PI;
            return std::log2(8.0f * luminance);
        }

        // 太陽・月・空の状態を時刻から求める。
        // **露出は一切掛けない**(すべて絶対的な測光量[lx]のまま返す)。露出はこの結果から
        // 決まる実効EV100を使ってRender()側で掛ける(可変プリ露出。KurenaiEngine3D.h参照)
        SunLighting ComputeSunLighting(
            float timeOfDayHours, float sunAzimuthDegrees, float moonAzimuthDegrees, float moonElevationDegrees)
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

            // === 昼夜の遷移係数を「時刻」ではなく「太陽の仰角」で決める ===
            // sinHour がそのまま太陽仰角のサインになる(軌道が単位円のため)。
            //
            // 【なぜ時刻ベースではいけないか】Smoothstep(6,7) * (1 - Smoothstep(17,18)) という
            // 時刻の窓は仰角0度〜15度にちょうど一致するため成立するが、遷移を長くしようと
            // 窓を5-7時/17-19時へ広げると
            // 5.5時(仰角-7.5度)で dayFactor≈0.156 となり、**地平線下の太陽が15,600 lx で照らす**
            // ことになる。LightDirection.y > 0 となってカスケードシャドウが地面の下から
            // 影を焼き、物体の裏側が照らされる。
            //
            // そこで遷移を2本に分ける:
            //   SunFactor      … 直接光と影。仰角[0°,15°]。地平線下では厳密に0
            //   TwilightFactor … 空の輝度と環境光。仰角[-15°,+15°] = 時刻でちょうど5-7時/17-19時
            // 「2時間かけて遷移する」という見た目の要求は TwilightFactor が満たし、
            // 直接光は物理的に成立する範囲(地平線より上)に留まる。
            // 実際の市民薄明(太陽が地平線下0〜-6度)もこの構造になっている。
            const float sunElevationSin = sinHour;
            const float kSin15Deg = std::sin(XMConvertToRadians(15.0f));
            const float sunFactor = Smoothstep(0.0f, kSin15Deg, sunElevationSin);
            const float twilightFactor = Smoothstep(-kSin15Deg, kSin15Deg, sunElevationSin);

            // === 月は時刻に連動せず、方位角と仰角で手動指定する ===
            // 実際の月は太陽とは独立した周期(朔望月)で動くため、反太陽方向に固定するのは
            // 「常に満月かつ常に真夜中に南中する」という二重の簡略化になる。
            // 位置を手動指定にすることで、任意の月齢・任意の時刻の見え方を作れるようにする。
            // 方位角の規約は太陽と同じ(X軸が0度、Z軸(+方向)が90度)
            const float moonAzimuthRadians = XMConvertToRadians(moonAzimuthDegrees);
            const float moonElevationRadians = XMConvertToRadians(moonElevationDegrees);
            const float moonCosElevation = std::cos(moonElevationRadians);
            const XMFLOAT3 moonDirection{
                moonCosElevation * std::cos(moonAzimuthRadians),
                std::sin(moonElevationRadians),
                moonCosElevation * std::sin(moonAzimuthRadians),
            };

            // 月が地平線より上にあるかどうか(太陽と同じく仰角[0°,15°]で立ち上げる)
            const float moonElevationFactor = Smoothstep(0.0f, kSin15Deg, moonDirection.y);
            // 【なぜ太陽の高度でも月を絞るのか】平行光源の枠は1つしかないので、
            // 太陽と月は「支配的な方」を選んで切り替える。月を反太陽方向に固定するなら、
            // 切替点(太陽の仰角0度)で月の係数もちょうど0になり、向きが反転しても
            // 何も見えないためポップは原理的に起きない。
            // 月の位置が独立だとこの保証が無く、太陽が沈む瞬間に月が高く昇っていると
            // 0.25lxの直接光が向きだけ突然入れ替わる(夜の影が見える明るさなので実際に目に付く)。
            // そこで月の立ち上がりを太陽の仰角0°→-5°に遅らせ、
            // **切替点では太陽も月も厳密に0**という性質を保つ
            const float kSin5Deg = std::sin(XMConvertToRadians(5.0f));
            const float moonNightGate = Smoothstep(0.0f, kSin5Deg, -sunElevationSin);
            const float moonFactor = moonElevationFactor * moonNightGate;

            // 太陽の色味(ティント)。ピーク照度はkSunIlluminanceLuxが持つので、ここは相対比のみ。
            // 仰角が低いほど暖色へ寄せる(大気の光路長が伸びて短波長が散乱で失われる現象の
            // アート的な近似。朝焼け・夕焼けの赤みはこれで出る)
            const XMFLOAT3 kSunColorTintHigh{ 1.0f, 0.967f, 0.9f };
            const XMFLOAT3 kSunColorTintHorizon{ 1.0f, 0.55f, 0.30f };
            const float warmth = 1.0f - sunFactor; // 仰角15度以上で0、地平線で1
            const XMFLOAT3 kSunColorTint{
                kSunColorTintHigh.x + (kSunColorTintHorizon.x - kSunColorTintHigh.x) * warmth,
                kSunColorTintHigh.y + (kSunColorTintHorizon.y - kSunColorTintHigh.y) * warmth,
                kSunColorTintHigh.z + (kSunColorTintHorizon.z - kSunColorTintHigh.z) * warmth,
            };
            // 満月の照度[lx]。太陽(10万lx)の40万分の1という実測値。
            // 月光は分光的には太陽光とほぼ同じだが、暗所視で青く感じられる(プルキンエ現象)ため
            // 慣例に従って寒色のティントを当てる(物理ではなくアート的な選択)
            const XMFLOAT3 kMoonColorTint{ 0.75f, 0.85f, 1.0f };
            // 夜間の環境光は天文学的な実測値(星明かり~0.001lx、満月~0.1〜0.3lx)をそのまま使うと
            // ほぼ完全な黒になり視認性が失われるため、視認性確保のためのアート的な下限値のまま残す
            // (物理値ではないことを明記した上での意図的な妥協)
            const XMFLOAT3 kNightAmbientArt{ 0.006f, 0.008f, 0.015f };

            // === 平行光源1枠を太陽と月で共有し、支配的な方を選ぶ ===
            // 太陽10万lx と満月0.25lx は40万倍違うので、両者の照度が入れ替わるのは
            // 実質的に太陽の仰角0度ちょうどの一点だけ。そこでは SunFactor も MoonFactor も
            // 厳密に0(=どちらの色もゼロ)になるよう moonNightGate で仕込んであるので、
            // 光源の向きが突然変わっても直接光も影も一切見えず、ポップは原理的に発生しない。
            // このためヒステリシスのような追加の対策は要らない
            const float sunIlluminance = kSunIlluminanceLux * sunFactor;
            const float moonIlluminance = kMoonIlluminanceLux * moonFactor;
            result.DominantIsSun = (sunIlluminance >= moonIlluminance);

            const float dominantPeak = result.DominantIsSun ? sunIlluminance : moonIlluminance;
            const XMFLOAT3& dominantTint = result.DominantIsSun ? kSunColorTint : kMoonColorTint;
            result.Color = {
                dominantTint.x * dominantPeak, dominantTint.y * dominantPeak, dominantTint.z * dominantPeak, 0.0f
            };
            // 支配ライトの向き(光が進む向き)。天体が「ある」向きの符号を反転したもの。
            // **カスケードシャドウの行列もこの向きから作ること**(LightColorだけ切り替えると
            // 月夜に太陽方向の影が残ってしまう)
            result.Direction = result.DominantIsSun
                ? XMFLOAT3{ -sunDirection.x, -sunDirection.y, -sunDirection.z }
                : XMFLOAT3{ -moonDirection.x, -moonDirection.y, -moonDirection.z };

            // 非IBLフォールバック用の定数色アンビエント(Enable IBL 無効時のみ使われる)。
            // 昼度は薄明係数をそのまま使う
            const float dayFactor = twilightFactor;
            const float skyPeak = kSkylightIlluminanceLux;
            const XMFLOAT3 dayAmbient{ kSunColorTint.x * skyPeak, kSunColorTint.y * skyPeak, kSunColorTint.z * skyPeak };
            // 夜間の下限値もここでは絶対値のまま持つ(露出はRender()側で掛ける)
            const float kNightAmbientScale = kMoonSkyIlluminanceLux;
            result.Ambient =
            {
                kNightAmbientArt.x * kNightAmbientScale + (dayAmbient.x - kNightAmbientArt.x * kNightAmbientScale) * dayFactor,
                kNightAmbientArt.y * kNightAmbientScale + (dayAmbient.y - kNightAmbientArt.y * kNightAmbientScale) * dayFactor,
                kNightAmbientArt.z * kNightAmbientScale + (dayAmbient.z - kNightAmbientArt.z * kNightAmbientScale) * dayFactor,
                dayFactor,
            };

            // === 手続き空(SkyGenerate.hlsl)へ渡す値 ===
            // 空が届ける照度は薄明係数で変調する。GPU側の照度正規化積分(SkyIntegrate.hlsl)
            // が「目標照度ちょうど」を保証するので、時刻による空の明るさは
            // ここの係数だけで素直に制御できる。
            // 夜側は月明かりで散乱する空の照度を足す(満月時の夜空はおよそ0.05lx相当)。
            // 月が地平線下でも星明かりぶんは残る(月の位置は手動指定で
            // 「月の出ていない夜」も作れるため、そこで0にしない)
            const float nightFactor = 1.0f - twilightFactor;
            result.SkyIlluminanceLux = kSkylightIlluminanceLux * twilightFactor +
                                       kMoonSkyIlluminanceLux * moonFactor +
                                       kStarlightIlluminanceLux * nightFactor;
            result.TwilightFactor = twilightFactor;
            // 空生成が使うのは**常に太陽の位置**(月が支配的でもPerez分布の基準は太陽のまま)。
            // result.Direction は支配ライトの向きなので、そこから逆算してはいけない
            result.SunPosition = sunDirection;

            // このフレームの「キーとなる照度」。可変プリ露出の基準にする(Render()参照)。
            // 支配ライトと空の両方を足すのは、太陽が沈んだ直後のように
            // 直接光がほぼ0でも空がまだ明るい時間帯を正しく拾うため
            result.KeyIlluminanceLux = std::max(sunIlluminance, moonIlluminance) + result.SkyIlluminanceLux;

            return result;
        }

        // SkyGenerate.hlsl側のcbuffer SkyBakeConstantsと一致させる必要がある
        // SkyIntegrate.hlsl が書き、SkyGenerate.hlsl / DeferredLighting.hlsl / SSR.hlsl が読む
        // 構造化バッファ(要素数1)の1要素。Sky.hlsliのGPUSkyParametersと完全に一致させること
        struct alignas(16) GPUSkyParameters
        {
            DirectX::XMFLOAT4 ZenithTint;    // xyz
            DirectX::XMFLOAT4 HorizonTint;   // xyz
            DirectX::XMFLOAT4 GroundTint;    // xyz
            DirectX::XMFLOAT4 SunGlowTint;   // xyz=色、w=強さ
            DirectX::XMFLOAT4 Luminance;     // x=天頂輝度(実効プリ露出込み、雲の減光は含まない)
                                              // y=余弦重み積分の値(ログ・検証用)、zw=予備
            // Preetham xyYモデル用のパラメータ。x=タービディティ、y=Preethamの重み
            // (0=従来ティントのみ、1=Preethamのみ)、zw=予備
            DirectX::XMFLOAT4 ModelParams;
            // xyz=雲による空の明かりの変化(P18)。「雲込みの空の照度 ÷ 晴天の空の照度」。
            // 被覆率0で厳密に(1,1,1)。w=予備。大気遠近のin-scatterへ掛ける
            // (意味と、なぜ天頂輝度に混ぜないのかはSky.hlsliのCloudSkyLightのコメント参照)
            DirectX::XMFLOAT4 CloudSkyLight;
        };
        // 【HLSL側の宣言とレイアウトを揃えたまま保つための固定】cbuffer(と構造化バッファ)は
        // 宣言順でオフセットが決まるので、ここで並べ替え・挿入・型変更が起きると、
        // HLSL側を直さないかぎり黙って別の値を読むことになる。
        // **通すために期待値を書き換えないこと**(FrameConstants.h と同じ規約)。
        //
        // 【これが守るのはC++側だけ】HLSLの宣言と突き合わせているわけではない。
        // ここが落ちたら「HLSL側も同じだけ動かせ」という合図として使う
        static_assert(offsetof(GPUSkyParameters, ZenithTint) == 0, "ZenithTint のレイアウトが変わっている");
        static_assert(offsetof(GPUSkyParameters, HorizonTint) == 16, "HorizonTint のレイアウトが変わっている");
        static_assert(offsetof(GPUSkyParameters, GroundTint) == 32, "GroundTint のレイアウトが変わっている");
        static_assert(offsetof(GPUSkyParameters, SunGlowTint) == 48, "SunGlowTint のレイアウトが変わっている");
        static_assert(offsetof(GPUSkyParameters, Luminance) == 64, "Luminance のレイアウトが変わっている");
        static_assert(offsetof(GPUSkyParameters, ModelParams) == 80, "ModelParams のレイアウトが変わっている");
        static_assert(offsetof(GPUSkyParameters, CloudSkyLight) == 96, "CloudSkyLight のレイアウトが変わっている");
        static_assert(sizeof(GPUSkyParameters) == 112, "GPUSkyParameters の総サイズが変わっている");

        // 間接引数の刻みが8の倍数であること。区画の定義と kModelCullArgsBaseOffset は
        // Passes/GeometryConstants.h が持つ(ここに在った写しは消した)。
        static_assert(
            (RHI::IRHICommandList::kDispatchMeshIndirectArgStride % 8) == 0,
            "引数の刻みが8の倍数でないと、2件目以降のGPU仮想アドレスが境界を割る");
        // 間接引数の刻みは RHI がインターフェースとして持ち、ShaderInterop 側は
        // パッカーが HLSL へ渡すための写しを持つ。両者が離れないようここで止める
        static_assert(
            ShaderInterop::kDispatchMeshIndirectArgStride
                == RHI::IRHICommandList::kDispatchMeshIndirectArgStride,
            "ShaderInterop::kDispatchMeshIndirectArgStride が RHI 側と食い違っている");


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

        // 確率的サンプリング経路の5本のHLSLが共有する。宣言は
        // ShaderInterop/MegaLightsStochasticConstants.h に1本だけ置き、HLSL側の
        // 同名の .hlsli と1対1で対応させている(食い違いはあちらのstatic_assertが止める)
        using ShaderInterop::MegaLightsStochasticConstants;

        // kLightTileSize / kLightTileCapacity / kLightTileStride はKurenaiEngine3Dのstatic constexprへ
        // 移した(DebugViewPanelがヒートマップの上限として参照するため)。定義はKurenaiEngine3D.h

        // SWRasterConstants / SWRasterMeshInfo は Passes/GeometryConstants.h へ移した

    }

    KurenaiEngine3D::KurenaiEngine3D(
        GraphicsAPI api, uint32_t renderWidth, uint32_t renderHeight, size_t initialSceneIndex)
        : KurenaiEngineBase(L"Kurenai Engine", 1280, 720, api)
        , m_GraphicsAPI(api)
        , m_InitialSceneIndex(initialSceneIndex)
        , m_RenderWidth(std::max(1u, renderWidth))
        , m_RenderHeight(std::max(1u, renderHeight))
    {
        // 超解像の出力解像度は、無効なうちは内部レンダー解像度と同じ意味を持つ。
        // ここを揃えておかないと、UIで初めて超解像を有効にした瞬間に
        // 出力解像度が既定値(1920x1080)へ飛んでしまう。
        // 【初期化子リストではなくここで代入する】メンバ変数の中のフィールドは
        // 初期化子リストへ書けない(m_PostProcessSettings 自体の既定値が先に入る)
        m_PostProcessSettings.UpscaleOutputWidth = std::max(1u, renderWidth);
        m_PostProcessSettings.UpscaleOutputHeight = std::max(1u, renderHeight);

        m_ImGuiBackend = m_Device->CreateImGuiBackend(m_Window->GetHandle());
        m_GPUProfiler = m_Device->CreateGPUProfiler();

        // imgui.iniの保存先を起動時の作業ディレクトリに依存させず、KurenaiEngine.dllと同じフォルダに固定する。
        // ImGuiはIniFilenameのポインタを保持するだけでコピーしないため、m_ImGuiIniPathで寿命を維持する
        m_ImGuiIniPath = WideToUtf8(GetModuleDirectory() + L"imgui.ini");
        ImGui::GetIO().IniFilename = m_ImGuiIniPath.c_str();

        // UIパネル群はImGuiコンテキストの生成後に作る(パネルの構築自体はImGuiを呼ばないが、
        // 以降の段階でスタイル・フォント設定をここへ足す前提で順序を固定しておく)
        m_UIManager = std::make_unique<UI::UIManager>(*this);

        // Render()から切り出したパス群(段階6)。CreateSceneResources()より前に作ってよい
        // ―― この時点では空で、シェーダーやPSOは後段の生成呼び出しで受け取る。
        // 【エンジンへの参照を渡している群と渡していない群がある】段階6.5で所有権を
        // 移し終えた結果、エンジンの公開APIを1つも使わなくなった群は参照を持たない
        m_EnvironmentPasses = std::make_unique<Passes::EnvironmentPasses>();
        m_PostProcessPasses = std::make_unique<Passes::PostProcessPasses>(*this);
        m_DDGIPasses = std::make_unique<Passes::DDGIPasses>(*this);
        m_GeometryPasses = std::make_unique<Passes::GeometryPasses>(*this);
        m_LightingPasses = std::make_unique<Passes::LightingPasses>(*this);
        m_MegaLightsPasses = std::make_unique<Passes::MegaLightsPasses>(*this);
        m_ReflectionPasses = std::make_unique<Passes::ReflectionPasses>(*this);
        m_ReflectionProbePasses = std::make_unique<Passes::ReflectionProbePasses>(*this);
        m_ShadowPasses = std::make_unique<Passes::ShadowPasses>(*this);
        m_PresentPass = std::make_unique<Passes::PresentPass>(*this);

        // アスペクト比はm_RenderAspectを唯一の出所にする(解像度は実行時に変わるため)。
        // ここではまだUpdateスレッドが動いていないのでm_Cameraへ直接書いてよい
        m_RenderAspect.store(
            static_cast<float>(m_RenderWidth) / static_cast<float>(m_RenderHeight), std::memory_order_relaxed);
        m_Camera.SetAspectRatio(m_RenderAspect.load(std::memory_order_relaxed));

        CreateSceneResources();

        m_LastFrameTime = std::chrono::steady_clock::now();
    }

    KurenaiEngine3D::~KurenaiEngine3D()
    {
        // このクラスが持つGPUリソース(レンダーターゲット・G-Buffer・各種バッファ・
        // シーンのテクスチャ)を1つも壊す前に、GPUの実行完了を待つ。
        // 基底のKurenaiEngineBaseも待つが、そちらが走るのはこのクラスのメンバが
        // すべて破棄された後なので間に合わない(WaitForGPUIdleの宣言側コメント参照)。
        //
        // ここへ来る時点でRun()がRender/Loaderの両スレッドをjoin済みのため、
        // 待った後に新しいコマンドが積まれることはない
        WaitForGPUIdle();
    }

    void KurenaiEngine3D::CreateSceneResources()
    {
        // Shaders/AssetsはビルドでKurenaiEngine.dllと同じフォルダにコピーされる
        const std::wstring dataRoot = GetModuleDirectory();
        const std::wstring shaderDirectory = dataRoot + L"Shaders\\";

        const std::vector<RHI::InputElementDesc> modelInputLayout = GetModelInputLayout();

        // 【元の行位置のまま呼ぶ】DX12はディスクリプタ枠を生成順に割り当てるため、
        // 所有権をGeometryPassesへ移しても生成の順序はここから動かさない
        m_GeometryPasses->CreateGeometryShaders(*m_Device, shaderDirectory, m_Device->SupportsMeshShader());

        if (m_Device->SupportsMeshShader())
        {
            m_ShadowPasses->CreateMeshletShaders(*m_Device, shaderDirectory);
        }

        // G-BufferのPSOはEmissiveのフォーマットがバッファ精度に依存するため、
        // この関数の末尾でCreatePrecisionDependentPipelineStates()がまとめて作る

        // 【元の行位置のまま呼ぶ】DX12はディスクリプタ枠を生成順に割り当てるため、
        // 所有権をLightingPassesへ移しても生成の順序はここから動かさない
        m_LightingPasses->CreateDirectLightPipelineState(*m_Device, shaderDirectory);

        // 【元の行位置のまま呼ぶ】上と同じ理由。SSAO/SSIL/AOブラーのPSOだけは出力先の
        // フォーマットがバッファ精度に依存するため、この関数の末尾で
        // CreatePrecisionDependentPipelineStates()がまとめて作る
        m_LightingPasses->CreateAOResources(
            *m_Device, shaderDirectory, m_AmbientOcclusionSettings.SSAOKernelSize);

        // AO/GI無効時はこの常に黒・不透明(遮蔽なし=a:1、間接光なし=rgb:0)のテクスチャをライティングパスに渡す
        m_AODisabledTexture = m_Device->CreateSolidColorTexture(0, 0, 0, 255);

        m_LightingPasses->CreateLightingPipelineState(*m_Device, shaderDirectory);

        m_LightingPasses->CreateTransparentPipelineStates(*m_Device, shaderDirectory, modelInputLayout);

        // ドローンショーパス(DroneShow.hlsl)。頂点バッファを持たず、Draw(6*機体数, 0)と
        // SV_VertexIDでビルボードのクアッドを展開する(InputLayoutは空のまま)
        RHI::ShaderDesc droneShowVsDesc;
        droneShowVsDesc.Stage = RHI::ShaderStage::Vertex;
        droneShowVsDesc.FilePath = shaderDirectory + L"DroneShow.kshader";
        droneShowVsDesc.EntryPoint = "VSMain";
        m_DroneShowVertexShader = m_Device->CreateShader(droneShowVsDesc);

        RHI::ShaderDesc droneShowPsDesc;
        droneShowPsDesc.Stage = RHI::ShaderStage::Pixel;
        droneShowPsDesc.FilePath = shaderDirectory + L"DroneShow.kshader";
        droneShowPsDesc.EntryPoint = "PSMain";
        m_DroneShowPixelShader = m_Device->CreateShader(droneShowPsDesc);

        RHI::PipelineStateDesc droneShowPipelineDesc;
        droneShowPipelineDesc.VertexShader = m_DroneShowVertexShader.get();
        droneShowPipelineDesc.PixelShader = m_DroneShowPixelShader.get();
        droneShowPipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
        // SceneColorと平面反射(m_RenderTargets.PlanarReflectionColor)はどちらもR16G16B16A16_Floatなので、
        // 同じPSOを両方のパスで使える
        droneShowPipelineDesc.RenderTargetFormats = { RHI::Format::R16G16B16A16_Float };
        // 島や地形の後ろに回った機体を隠すため深度テストは行うが、
        // 機体同士は隠し合わせない(加算合成は順序に依存しないのでソートも不要)
        droneShowPipelineDesc.HasDepthStencil = true;
        droneShowPipelineDesc.DepthWriteEnabled = false;
        droneShowPipelineDesc.ReverseZ = true;
        // 【Additiveではなくこちらを使う理由 ― アルファ(カバレッジ)を書かないため】
        // Additiveは SrcBlendAlpha=ONE / DestBlendAlpha=ONE なので、機体を描くたびに
        // レンダーターゲットのアルファへ1.0が積まれる。SceneColorではアルファを誰も読まないので
        // 実害が無いが、平面反射(m_RenderTargets.PlanarReflectionColor)ではアルファが
        // 「そのテクセルにジオメトリが描かれたか」のカバレッジとして使われており
        // (SSR.hlslのApplyPlanarReflection)、機体のクアッド全域でカバレッジが1になってしまう。
        // すると水面はクアッドの円の内側で解析空の映り込みを失い、裾(glowがほぼ0の外周)が
        // 黒い円として抜ける。機体が重なるとアルファは1を超え、解析空の係数(1-a)が負へ振れる。
        // PremultipliedAlphaは SrcBlend=ONE / DestBlend=INV_SRC_ALPHA なので、
        // PSMainがアルファ0を返せば rgb=src+dst(加算合成のまま)・alpha=dst(据え置き)になり、
        // 「光は足すが遮蔽はしない」という発光点の正しい意味になる
        droneShowPipelineDesc.BlendMode = RHI::BlendMode::PremultipliedAlpha;
        // 【平面反射用にワインディングを反転したPSOは要らない】
        // メッシュの描画(LightingPasses::m_TransparentPipelineStateMirrored等)では鏡映ビュー行列が頂点そのものを
        // 変換するため画面上の巻きが反転するが、このパスのビルボードは
        // 「ワールド座標をViewで変換した"後"に、ビュー空間で四隅のオフセットを足す」
        // という作り方をしている(DroneShow.hlslのVSMain)。四隅のオフセットは鏡映行列を
        // 一度も通らないので、Viewが鏡映を含んでいてもクアッド自身の巻きは変わらない。
        // 反転したPSOで描くと1機残らず裏面として捨てられ、水面に何も映らなくなる
        m_DroneShowResources.PipelineState = m_Device->CreatePipelineState(droneShowPipelineDesc);

        RHI::BufferDesc droneShowConstantBufferDesc;
        droneShowConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        droneShowConstantBufferDesc.SizeInBytes = sizeof(Passes::DroneShowConstants);
        m_DroneShowResources.ConstantBuffer = m_Device->CreateBuffer(droneShowConstantBufferDesc);

        RHI::BufferDesc droneBufferDesc;
        droneBufferDesc.Usage = RHI::BufferUsage::StructuredReadOnly;
        droneBufferDesc.SizeInBytes = sizeof(GPUDrone) * kMaxDrones;
        droneBufferDesc.StrideInBytes = sizeof(GPUDrone);
        m_DroneShowResources.Buffer = m_Device->CreateBuffer(droneBufferDesc);

        m_GeometryPasses->CreateHiZResources(*m_Device, shaderDirectory);

        // 【元の行位置のまま呼ぶ】DX12はディスクリプタ枠を生成順に割り当てるため、
        // 所有権をMegaLightsPassesへ移しても生成の順序はここから動かさない
        m_MegaLightsPasses->CreateLightCullingPipelineState(*m_Device, shaderDirectory);

        // 自前ソフトウェアラスタライザ(46章)。比較用の独立した経路で、既存の描画には寄与しない。
        // 解像度に依存するリソース(visibility buffer・出力テクスチャ)はCreateRenderTargetsで作る。
        //
        // 【失敗しても他を巻き込まない】シェーダーはSM 6.6の64bitアトミックとbindlessを使うため、
        // デバイス判定を通っていてもコンパイル環境によっては落ち得る。ここで捕まえて
        // 機能だけ無効化する(レイトレーシングと同じ扱い)
        if (m_Device->SupportsSoftwareRaster())
        {
            try
            {
                // 【元の行位置のまま呼ぶ】DX12はディスクリプタ枠を生成順に割り当てるため、
                // 所有権をGeometryPassesへ移しても生成の順序はここから動かさない
                m_GeometryPasses->CreateSoftwareRasterResources(*m_Device, shaderDirectory);

                m_RenderCapabilities.SoftwareRasterAvailable = true;
            }
            catch (const std::exception& e)
            {
                Core::Logger::Warning(
                    "KurenaiEngine3D",
                    std::string("ソフトウェアラスタライザの初期化に失敗したため無効にします: ") + e.what());
                m_RenderCapabilities.SoftwareRasterAvailable = false;
                m_GeometryPasses->ResetSoftwareRasterResources();
            }
        }
        else
        {
            Core::Logger::Info(
                "KurenaiEngine3D",
                "ソフトウェアラスタライザは利用できません(DX12・シェーダーモデル6.6・"
                "64bit整数アトミック・bindlessのすべてが必要です)");
        }

        // 【元の行位置のまま呼ぶ】DX12はディスクリプタ枠を生成順に割り当てるため、
        // 所有権をReflectionPassesへ移しても生成の順序はここから動かさない
        m_ReflectionPasses->CreateSSRPipelineState(*m_Device, shaderDirectory);

        // 大気遠近パス(頂点バッファなしのフルスクリーン三角形。反射パスの出力とG-Buffer深度から
        // フォグを合成する)。専用のb1定数バッファは持たない(パラメータはFrameConstants末尾の
        // FogParams0/1に入れているため。AerialPerspective.hlsl冒頭参照)
        // 【元の行位置のまま呼ぶ】DX12はディスクリプタ枠を生成順に割り当てるため、
        // 所有権をPostProcessPassesへ移しても生成の順序はここから動かさない
        m_PostProcessPasses->CreateAerialPerspectivePipelineState(*m_Device, shaderDirectory);

        m_LightingPasses->CreateSkyCloudPipelineState(*m_Device, shaderDirectory);

        // 【元の行位置のまま呼ぶ】DX12はディスクリプタ枠を生成順に割り当てるため、
        // 所有権をDDGIPassesへ移しても生成の順序はここから動かさない
        m_DDGIPasses->CreateResolvePipelineState(*m_Device, shaderDirectory);

        // RT反射パス(コンピュートシェーダー。TLASへ鏡面レイを撃ち反射色を求める)。
        // RTReflection.hlslはRayQueryを含むためシェーダーモデル6.5でしかコンパイルできない。
        // 非対応環境ではシェーダー自体を作らず、UIからもRaytracedを選べないようにする
        m_RenderCapabilities.RaytracingAvailable = m_Device->SupportsRaytracing();
        // メッシュシェーダーの可否もここで控える(UIパネルが参照する)
        m_RenderCapabilities.MeshShaderAvailable = m_Device->SupportsMeshShader();
        // 間接ディスパッチの可否も同じ理由でここへ控える(パス群が毎フレーム問い合わせない)
        m_RenderCapabilities.IndirectDispatchMeshAvailable = m_Device->SupportsIndirectDispatchMesh();
        // bindless区画の容量も同じ理由でここへ控える(使用数はフレームごとに更新する)
        m_RenderStats.BindlessCapacity = m_Device->GetBindlessCapacity();

        // メッシュレットカリングの統計(Stage 5-2)。増幅シェーダーがカウンタへ数え上げ、
        // それを数フレーム遅れでCPUへ読み戻してPerfログへ出す。
        // 増幅シェーダーが走らない環境では一切使わないので、そもそも作らない
        if (m_RenderCapabilities.MeshShaderAvailable)
        {
            try
            {
                // 【元の行位置のまま呼ぶ】DX12はディスクリプタ枠を生成順に割り当てるため、
                // 所有権をGeometryPassesへ移しても生成の順序はここから動かさない
                m_GeometryPasses->CreateMeshletCullStatsBuffer(*m_Device);

                // 【SRVではなくUAVを登録する】増幅シェーダーは読むのではなく書く。
                // RegisterBindless(SRV)の番号を渡すと読み取り専用のビューへ書き込むことになる
                m_MeshletCullStatsBindlessIndex =
                    m_Device->RegisterBindlessUAV(m_GeometryPasses->GetMeshletCullStatsBuffer());
                if (m_MeshletCullStatsBindlessIndex == RHI::kInvalidBindlessIndex)
                {
                    Core::Logger::Warning(
                        "KurenaiEngine3D",
                        "メッシュレットカリングの統計バッファをbindlessへ登録できませんでした(統計を無効にします)");
                    m_GeometryPasses->ResetMeshletCullStatsBuffer();
                }
                else
                {
                    for (uint32_t i = 0; i < kMeshletCullStatsRingSize; ++i)
                    {
                        RHI::BufferDesc readbackDesc;
                        readbackDesc.Usage = RHI::BufferUsage::Readback;
                        readbackDesc.SizeInBytes =
                            static_cast<uint32_t>(sizeof(uint32_t)) * kMeshletCullStatsCount;
                        readbackDesc.StrideInBytes = static_cast<uint32_t>(sizeof(uint32_t));
                        m_MeshletCullStatsReadback[i] = m_Device->CreateBuffer(readbackDesc);
                    }
                }
            }
            catch (const std::exception& e)
            {
                // 統計が作れないだけで描画は成立する。カリング本体は止めない
                Core::Logger::Warning(
                    "KurenaiEngine3D",
                    std::string("メッシュレットカリングの統計の初期化に失敗したため無効にします: ") + e.what());
                m_GeometryPasses->ResetMeshletCullStatsBuffer();
                for (auto& readback : m_MeshletCullStatsReadback)
                {
                    readback.reset();
                }
                m_MeshletCullStatsBindlessIndex = RHI::kInvalidBindlessIndex;
            }
        }

        // モデル単位のGPUカリング(Stage 5-3)。判定はコンピュートシェーダーで行い、
        // 生き残りの DispatchMesh 引数と統計をGPU上に作る。
        //
        // 【メッシュレット経路が使えるときだけ作る】判定結果の行き先(ExecuteIndirect)も、
        // 判定に使うHi-Zも、メッシュシェーダー経路の話でしか意味を持たない
        if (m_RenderCapabilities.MeshShaderAvailable)
        {
            try
            {
                // 【元の行位置のまま呼ぶ】DX12はディスクリプタ枠を生成順に割り当てるため、
                // 所有権をGeometryPassesへ移しても生成の順序はここから動かさない
                m_GeometryPasses->CreateModelCullResources(*m_Device, shaderDirectory);

                // カウンタの読み戻し。大きさは群が作るカウンタバッファと同じにする
                for (uint32_t i = 0; i < kMeshletCullStatsRingSize; ++i)
                {
                    RHI::BufferDesc readbackDesc;
                    readbackDesc.Usage = RHI::BufferUsage::Readback;
                    readbackDesc.SizeInBytes =
                        static_cast<uint32_t>(sizeof(uint32_t)) * kModelCullCounterCount;
                    readbackDesc.StrideInBytes = static_cast<uint32_t>(sizeof(uint32_t));
                    m_ModelCullReadback[i] = m_Device->CreateBuffer(readbackDesc);
                }
            }
            catch (const std::exception& e)
            {
                // カリングが作れないだけで描画は成立する(CPU側のループがそのまま描く)
                Core::Logger::Warning(
                    "KurenaiEngine3D",
                    std::string("モデル単位のGPUカリングの初期化に失敗したため無効にします: ") + e.what());
                m_GeometryPasses->ResetModelCullResources();
                for (auto& readback : m_ModelCullReadback)
                {
                    readback.reset();
                }
            }
        }

        if (m_RenderCapabilities.RaytracingAvailable)
        {
            // 【元の行位置のまま呼ぶ】上のCreateSSRPipelineStateと同じ理由
            m_ReflectionPasses->CreateRaytracedResources(*m_Device, shaderDirectory);

            m_ShadowPasses->CreateRaytracedResources(*m_Device, shaderDirectory);

            // 【元の行位置のまま呼ぶ】上と同じ理由。DXR対応環境でだけ作る点も変えない
            m_MegaLightsPasses->CreateStochasticPipelineStates(*m_Device, shaderDirectory);

            m_LightingPasses->CreateRaytracedAOResources(*m_Device, shaderDirectory);

            // DDGIのプローブ取得(コンピュートシェーダー。プローブから6面ぶんのレイを撃ち、
            // ラスタ経路と同じ形のスクラッチキューブを直接埋める)
            //
            // 【失敗しても他のRTパスを巻き込まない】このシェーダーはコンピュートシェーダーの中で
            // テクスチャを微分付きにサンプルするため、DXILの検証がSM 6.6を要求する
            // (Derivatives in CS/MS/AS is SM 6.6+)。RayQuery自体はSM 6.5で足りるので、
            // 「DXR Tier 1.1に対応していて、かつSM 6.5のバリアントで動いている」環境
            // (ビルドマシンのWindows SDKが古くbindlessバリアントを焼けなかった場合など)では、
            // 上のRT反射/RTシャドウ/RTAOは作れるのにこれだけ作れない。
            // ここで捕まえてDDGIのレイ取得だけをラスタ経路へ戻す(自前ラスタライザと同じ扱い)
            try
            {
                m_DDGIPasses->CreateRaytracedTraceResources(*m_Device, shaderDirectory);

                m_RenderCapabilities.DDGIRaytracedTraceAvailable = true;
            }
            catch (const std::exception& e)
            {
                m_DDGIPasses->ResetRaytracedTraceResources();
                m_RenderCapabilities.DDGIRaytracedTraceAvailable = false;
                Core::Logger::Error(
                    "KurenaiEngine3D",
                    std::string("DDGIのレイ取得(DXR)を用意できませんでした。ラスタライズ経路で動作します"
                                "(DDGIProbeTrace.hlslはシェーダーモデル6.6を要求します): ") + e.what());
            }

            // レイトレーシングが使える環境ではDDGIのレイ取得も既定でDXRにする。
            // 更新コストが下がり、カメラから遠いプローブにも影が落ちるようになるため
            m_DDGISettings.RayMode = DDGISettings::DDGIRayModeForCapability(m_RenderCapabilities.DDGIRaytracedTraceAvailable);

            Core::Logger::Info(
                "KurenaiEngine3D",
                m_RenderCapabilities.DDGIRaytracedTraceAvailable
                    ? "レイトレーシングを利用できます(反射・シャドウ・AO/GI・DDGIでRaytracedを選択可能)"
                    : "レイトレーシングを利用できます(反射・シャドウ・AOでRaytracedを選択可能。DDGIのレイ取得は"
                      "ラスタライズのみ)");
        }
        else
        {
            Core::Logger::Info(
                "KurenaiEngine3D",
                "レイトレーシングは利用できません(反射・シャドウ・AO/GIはいずれもスクリーンスペース手法のみ)");
        }

        m_PostProcessPasses->CreateTAAPipelineState(*m_Device, shaderDirectory);

        m_PostProcessPasses->CreateTonemapPipelineState(*m_Device, shaderDirectory);

        m_PostProcessPasses->CreateUpscalePipelineStates(*m_Device, shaderDirectory);

        m_PostProcessPasses->CreateAutoExposureResources(*m_Device, shaderDirectory);

        // 露出の保存先。フレームをまたいで順応の履歴を保持するため、ウィンドウリサイズで
        // 作り直されるCreateRenderTargetsではなくここで一度だけ作る。
        // 生成直後はゼロクリアされており、texel(1,0)=0が「未初期化」を意味する
        // (CSResolveがこれを見て初回だけ順応を飛ばして即座に目標値へ合わせる)
        m_RenderTargets.ExposureTexture = m_Device->CreateUAVTexture(2, 1, RHI::Format::R32_Float);

        m_PostProcessPasses->CreateBloomPipelineStates(*m_Device, shaderDirectory);

        // 【元の行位置のまま呼ぶ】DX12はディスクリプタ枠を生成順に割り当てるため、
        // 所有権をPresentPassへ移しても生成の順序はここから動かさない
        m_PresentPass->CreatePipelineState(*m_Device, shaderDirectory);

        m_ShadowPasses->CreateCascadePipelineStates(*m_Device, shaderDirectory);

        m_RenderTargets.CreateShadowCascadeArray(*m_Device, kShadowMapSize, kCascadeCount);

        // 既定のスカイボックス。.ksceneの[Scene]Skyboxで差し替えられる(LoadScene参照)ため、
        // 現在読み込んでいるパスを覚えておき、同じパスなら読み直さない
        m_DefaultSkyboxPath = dataRoot + L"Assets\\Skybox\\Sky.dds";
        m_CurrentSkyboxPath = m_DefaultSkyboxPath;
        m_SkyboxTexture = m_Device->CreateTextureFromFile(m_CurrentSkyboxPath, false);

        // 水面法線マップの既定。.ksceneに[Water]NormalMapが無いシーンではこのフラット法線
        // (128,128,255,255=接線空間で真上を向く法線)がWater.hlslのt6へバインドされ続ける。
        // ModelLoader.cppが法線マップ未指定のマテリアルに使うプレースホルダーと同じ値
        m_CurrentWaterNormalMapPath.clear();
        m_WaterNormalMapTexture = m_Device->CreateSolidColorTexture(128, 128, 255, 255);

        CreateSamplerSets();

        // IBL(Image Based Lighting)の3つの畳み込み結果を保持するテクスチャと、それを生成する
        // コンピュートシェーダー一式。実際の畳み込み(スカイボックスのサンプリング)はRender()の
        // 最初のフレームで一度だけ行う(EnvironmentPassesのm_IBLBaked参照)。ここではリソースの作成のみ行う
        m_IBLResources.CreateEnvironmentMaps(
            *m_Device, kIBLIrradianceSize, kIBLPrefilterBaseSize, kIBLPrefilterMipLevels);
        // BRDF積分LUTは2パスで焼く。パス1(CSMain)が(A, B)をスクラッチへ書き、
        // パス2(CSCombineEavg)がそれを読んでEavgを足した float4(A, B, Eavg, 0) を最終LUTへ書く。
        // 同一リソースをSRVとUAVへ同時バインドできないためスクラッチが要る(BRDFLUT.hlsl参照)
        // 【元の行位置のまま呼ぶ】DX12はディスクリプタ枠を生成順に割り当てるため、
        // 所有権をEnvironmentPassesへ移しても生成の順序はここから動かさない
        m_EnvironmentPasses->CreateBRDFLUTScratch(*m_Device, kIBLBRDFLUTSize);
        m_IBLResources.CreateBRDFLUT(*m_Device, kIBLBRDFLUTSize);
        if (!m_EnvironmentPasses->HasBRDFLUTScratch() || !m_IBLResources.BRDFLUTTexture)
        {
            Core::Logger::Error("KurenaiEngine3D",
                "BRDF積分LUTのテクスチャ作成に失敗しました(スペキュラのエネルギー補正が正しく動作しません)");
        }

        m_EnvironmentPasses->CreateBRDFLUTPipelineStates(*m_Device, shaderDirectory);

        // ボリュメトリック雲の3Dノイズ。カメラにも太陽にも空の状態にも依存しない
        // 純粋な手続き生成なので、BRDF積分LUTと同じく起動後に一度だけ焼く(EnvironmentPassesのm_CloudNoiseBaked)。
        // ここではリソースとパイプラインの作成だけを行う
        m_SkyResources.CreateCloudNoise(
            *m_Device, kCloudShapeNoiseSize, kCloudDetailNoiseSize, kCloudWeatherNoiseSize);
        if (!m_SkyResources.CloudShapeNoiseTexture || !m_SkyResources.CloudDetailNoiseTexture ||
            !m_SkyResources.CloudWeatherNoiseTexture)
        {
            Core::Logger::Error("KurenaiEngine3D",
                "雲のノイズテクスチャの作成に失敗しました(ボリュメトリック雲が正しく描画されません)");
        }

        m_EnvironmentPasses->CreateCloudNoisePipelineStates(*m_Device, shaderDirectory);

        // 大気散乱のLUT(P14a: Hillaire 2020)。TransmittanceとMultiScatteringはカメラにも太陽にも
        // 依存せず、大気パラメータ(濁りを含む)だけの関数なので、濁りが変わらない限り焼き直さない
        // (EnvironmentPassesのm_AtmosphereLUTBakedTurbidity)。SkyViewは太陽の位置と濁りで変わるため、
        // そのどちらかが動いたときに焼き直す(同 m_SkyViewBakedSunPosition)。
        m_SkyResources.CreateAtmosphereLUTs(
            *m_Device, kTransmittanceLUTWidth, kTransmittanceLUTHeight, kMultiScatteringLUTSize,
            kSkyViewLUTWidth, kSkyViewLUTHeight);
        if (!m_SkyResources.TransmittanceLUT || !m_SkyResources.MultiScatteringLUT ||
            !m_SkyResources.SkyViewLUT)
        {
            Core::Logger::Error("KurenaiEngine3D",
                "大気散乱のLUTテクスチャの作成に失敗しました(日中の空が黒くなります)");
        }
        m_EnvironmentPasses->CreateAtmosphereResources(*m_Device, shaderDirectory);

        m_EnvironmentPasses->CreateIrradiancePipelineState(*m_Device, shaderDirectory);

        RHI::ShaderDesc prefilterCsDesc;
        prefilterCsDesc.Stage = RHI::ShaderStage::Compute;
        prefilterCsDesc.FilePath = shaderDirectory + L"IBLConvolve.kshader";
        prefilterCsDesc.EntryPoint = "CSPrefilter";
        m_PrefilterComputeShader = m_Device->CreateShader(prefilterCsDesc);
        m_IBLResources.PrefilterPipelineState = m_Device->CreateComputePipelineState({ m_PrefilterComputeShader.get() });

        m_EnvironmentPasses->CreateSHResources(*m_Device, shaderDirectory);

        // 手続き空(SkyGenerate.hlsl)。太陽が動くたびに焼き直すため、IBLのプリフィルタと同じく
        // 面ごとに1回ずつディスパッチする。プリフィルタの入力にしかならないので解像度は
        // オフラインDDS(512)より小さい256で足りる(生成コストが1/4になる)
        m_SkyResources.ProceduralSkyTexture =
            m_Device->CreateUAVTextureCube(kProceduralSkySize, RHI::Format::R16G16B16A16_Float);

        m_EnvironmentPasses->CreateSkyGenerateResources(*m_Device, shaderDirectory);

        m_EnvironmentPasses->CreateSkyIntegrateResources(*m_Device, shaderDirectory);

        // SkyIntegrate.hlslが書き、SkyGenerate.hlsl/DeferredLighting.hlsl/SSR.hlslが読む
        // 要素数1のStructuredRWバッファ(m_RenderTargets.LightTileBufferと同じ作法)。
        //
        // 【CPU側からのゼロ初期化はできない】UpdateBuffer(CPU→GPU書き込み)でゼロ埋めする案を
        // 最初に採ったが、DX12のStructuredRWバッファはUAV/SRVでのGPUアクセス専用にDEFAULTヒープへ
        // 作成しており(DX12Device::CreateBuffer参照)、CPUから書き込むためのマップ済みポインタ・
        // ステージングリングを一切持たない。DX12CommandList::UpdateBufferの非対応分岐
        // (StructuredReadOnly/StructuredImmutable以外の既定経路)はAdvanceRingAndGetWritePtrで
        // nullptrへ書き込もうとしてクラッシュする。そのため未初期化対策はCPUからのゼロ埋めではなく、
        // 「SkyIntegrateパスをまだ一度も実行していないフレームでは、手続き空が無効でも1回だけ
        // 実行する」という形でGPU側から埋める(Render()のskyIntegrateThisFrame・
        // EnvironmentPasses::IsSkyParametersBufferInitialized参照)
        m_SkyResources.CreateParametersBuffer(*m_Device, sizeof(GPUSkyParameters));

        m_IBLResources.CreatePrefilterConstantBuffer(*m_Device, sizeof(Passes::IBLFaceConstants));

        // --- 反射プローブ(19章) ---
        // キャプチャ先(1面ぶんを6面で使い回す)。キューブへ写す前のHDR値を保つためFloatにする
        m_GIResources.ProbeCaptureColor = m_Device->CreateRenderTexture(kProbeCaptureSize, kProbeCaptureSize, RHI::Format::R16G16B16A16_Float);
        // 同じキャプチャの2枚目(SV_TARGET1)。プローブからのワールド距離をそのまま入れるため、
        // [0,1]に収まらず精度も必要になる。R32_Floatなら室内スケールでも十分な絶対精度がある
        m_GIResources.ProbeCaptureDistance = m_Device->CreateRenderTexture(kProbeCaptureSize, kProbeCaptureSize, RHI::Format::R32_Float);
        // Reverse-Zのため遠平面側(0.0)でクリアする(G-Buffer深度と同じ)
        m_GIResources.ProbeCaptureDepth = m_Device->CreateDepthTexture(kProbeCaptureSize, kProbeCaptureSize, 0.0f);
        // 畳み込みの入力になるスクラッチのキューブマップ(TextureCubeとして読めること
        // = 配列ではないことが必須。理由はGIResources::ProbeRadianceCubeのコメント参照)
        m_GIResources.ProbeRadianceCube = m_Device->CreateUAVTextureCube(kProbeCaptureSize, RHI::Format::R16G16B16A16_Float);
        // 畳み込み結果はプローブごとに保持するためキューブマップ配列で確保する。
        // 反射プローブは鏡面専任なので拡散イラディアンス側の配列は持たない
        // (拡散はDDGIへ一本化。ReflectionProbe.hlsli冒頭のコメント参照)
        m_GIResources.ProbePrefilteredArray = m_Device->CreateMippedUAVTextureCubeArray(
            kIBLPrefilterBaseSize, RHI::Format::R16G16B16A16_Float, kIBLPrefilterMipLevels, kMaxReflectionProbes);
        // 距離キューブ(19.12節)。畳み込まないためミップは1段だけでよく、スクラッチのキューブも要らない
        // (キャプチャからこの配列のスライスへ直接書き込む)。
        // 128²×6面×8枚×4バイト = 3.1MB
        m_GIResources.ProbeDistanceArray = m_Device->CreateMippedUAVTextureCubeArray(
            kProbeCaptureSize, RHI::Format::R32_Float, 1, kMaxReflectionProbes);

        RHI::ShaderDesc probeCaptureVsDesc;
        probeCaptureVsDesc.Stage = RHI::ShaderStage::Vertex;
        probeCaptureVsDesc.FilePath = shaderDirectory + L"ProbeCapture.kshader";
        probeCaptureVsDesc.EntryPoint = "VSMain";
        m_ProbeCaptureVertexShader = m_Device->CreateShader(probeCaptureVsDesc);

        RHI::ShaderDesc probeCapturePsDesc;
        probeCapturePsDesc.Stage = RHI::ShaderStage::Pixel;
        probeCapturePsDesc.FilePath = shaderDirectory + L"ProbeCapture.kshader";
        probeCapturePsDesc.EntryPoint = "PSMain";
        m_ProbeCapturePixelShader = m_Device->CreateShader(probeCapturePsDesc);

        RHI::PipelineStateDesc probeCapturePipelineDesc;
        probeCapturePipelineDesc.InputLayout = modelInputLayout;
        probeCapturePipelineDesc.VertexShader = m_ProbeCaptureVertexShader.get();
        probeCapturePipelineDesc.PixelShader = m_ProbeCapturePixelShader.get();
        probeCapturePipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
        // レンダーターゲットは2枚(放射輝度と距離)。ProbeCapture.hlslのPSOutputと並びを一致させること
        probeCapturePipelineDesc.RenderTargetFormats = { RHI::Format::R16G16B16A16_Float, RHI::Format::R32_Float };
        probeCapturePipelineDesc.HasDepthStencil = true;
        probeCapturePipelineDesc.ReverseZ = true;
        m_GIResources.ProbeCapturePipelineState = m_Device->CreatePipelineState(probeCapturePipelineDesc);

        RHI::ShaderDesc probeCubeCopyCsDesc;
        probeCubeCopyCsDesc.Stage = RHI::ShaderStage::Compute;
        probeCubeCopyCsDesc.FilePath = shaderDirectory + L"IBLConvolve.kshader";
        probeCubeCopyCsDesc.EntryPoint = "CSCopyCaptureToCubeFace";
        m_ProbeCubeCopyComputeShader = m_Device->CreateShader(probeCubeCopyCsDesc);
        m_GIResources.ProbeCubeCopyPipelineState = m_Device->CreateComputePipelineState({ m_ProbeCubeCopyComputeShader.get() });

        // プローブの影響範囲(位置・半径)を渡すStructuredBuffer(t13)。ライトリストと同じく
        // ピクセルシェーダからは読み取り専用でよい
        RHI::BufferDesc probeBufferDesc;
        probeBufferDesc.Usage = RHI::BufferUsage::StructuredReadOnly;
        probeBufferDesc.SizeInBytes = sizeof(GPUReflectionProbe) * kMaxReflectionProbes;
        probeBufferDesc.StrideInBytes = sizeof(GPUReflectionProbe);
        m_GIResources.ProbeBuffer = m_Device->CreateBuffer(probeBufferDesc);

        // キャプチャの面ごとに更新するFrameConstants(共有のm_FrameConstantBufferとは別インスタンス)
        RHI::BufferDesc probeCaptureConstantBufferDesc;
        probeCaptureConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        probeCaptureConstantBufferDesc.SizeInBytes = sizeof(FrameConstants);
        m_GIResources.ProbeCaptureConstantBuffer = m_Device->CreateBuffer(probeCaptureConstantBufferDesc);

        // --- 平面反射 ---
        // 【元の行位置のまま呼ぶ】上のCreateSSRPipelineStateと同じ理由
        m_ReflectionPasses->CreatePlanarPipelineStates(*m_Device, shaderDirectory, modelInputLayout);

        // --- DDGI(22章) ---
        // キャプチャ経路は反射プローブとまったく同じ(ProbeCapture.hlslとm_GIResources.ProbeCapturePipelineStateを
        // そのまま使う)で、解像度だけkDDGICaptureSizeへ落とす。レンダーターゲットのフォーマットは
        // PSOと一致していなければならないため、反射プローブ側と同じ組み合わせにする
        m_DDGIPasses->CreateCaptureResources(*m_Device);

        m_DDGIPasses->CreateProbeUpdateResources(*m_Device, shaderDirectory);

        // ここまでで全シェーダーの生成が終わっている。読み込んだ.kshaderはもう誰も読まないので、
        // バイトコードをプロセスの寿命ぶん抱え続けないよう明示的に捨てる
        // (このあとCreateShaderを呼ぶことがあれば、必要なパッケージが読み直されるだけ)
        m_Device->ReleaseShaderPackages();

        // シーン読み込み前でもSRVをバインドできるよう、この時点で1プローブぶんのダミーを確保しておく
        RecreateDDGIAtlases();

        RHI::BufferDesc constantBufferDesc;
        constantBufferDesc.Usage = RHI::BufferUsage::Constant;
        constantBufferDesc.SizeInBytes = sizeof(FrameConstants);
        m_FrameConstantBuffer = m_Device->CreateBuffer(constantBufferDesc);

        m_ShadowPasses->CreateCascadeConstantBuffer(*m_Device);

        RHI::BufferDesc objectConstantBufferDesc;
        objectConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        objectConstantBufferDesc.SizeInBytes = sizeof(ObjectConstants);
        // このバッファだけは「メッシュごと・パスごと」に書かれるため、既定の段数では足りない。
        // 1フレームの最悪ケースは、本編のパス(深度プリパス・G-Buffer・シャドウ4枚・半透明ほか)に
        // 加えて、プローブのキャプチャが「プローブ数 × 6面 × 不透明メッシュ数」を積む。
        // BistroInteriorLit(不透明59メッシュ)で既定の16プローブ/フレームだと
        // 59 × 6 × 16 = 5664 回に達し、既定の4096回では一周して描画が壊れていた。
        // 16384にしておけば同シーンで3倍近い余裕がある(1スロット256Bなので約8MB)
        objectConstantBufferDesc.MaxConstantUpdatesPerFrame = kObjectConstantUpdatesPerFrame;
        m_ObjectConstantBuffer = m_Device->CreateBuffer(objectConstantBufferDesc);

        // ポイント/スポットライトのリスト(t8)。CPUから毎フレーム更新するが、ピクセルシェーダから
        // 読み取り専用でよいためStructuredReadOnly(RWStructuredBufferではなくStructuredBuffer)で作成する
        RHI::BufferDesc lightBufferDesc;
        lightBufferDesc.Usage = RHI::BufferUsage::StructuredReadOnly;
        lightBufferDesc.SizeInBytes = sizeof(GPULight) * kMaxLights;
        lightBufferDesc.StrideInBytes = sizeof(GPULight);
        m_SceneGPUResources.LightBuffer = m_Device->CreateBuffer(lightBufferDesc);

        m_LightingPasses->CreateLightingConstantBuffer(*m_Device);

        // 【元の行位置のまま呼ぶ】上のCreatePipelineStateと同じ理由
        m_PresentPass->CreateConstantBuffer(*m_Device);

        // レンダーターゲットを先に作る。CreateRenderTargetsはHDRフォーマットの作成に失敗した場合に
        // m_SystemSettings.PrecisionをLegacy8bitへ落とすフォールバックを持つため、PSOはその結果が
        // 確定した後に作らなければフォーマットがずれる
        CreateRenderTargets(m_RenderWidth, m_RenderHeight);
        // 平面反射専用のレンダーターゲットも、レンダー解像度が確定したこのタイミングで作る
        // (呼び出し箇所はCreateRenderTargetsと同じ2か所。もう1か所はRender()の解像度変更ハンドリング)
        CreatePlanarReflectionTargets();
        CreatePrecisionDependentPipelineStates();

        DiscoverScenes();

        // 起動時の1シーン目だけは同期的に読み込む。この時点ではRender/Loaderのどちらのスレッドも
        // まだ動いていないため、通常のハンドオフを経由せず直接読み込んで反映してよい
        // (初回フレームより前にシーンが揃う従来の挙動を保つ)。
        // m_LoaderSkyboxPathはCreateSceneResourcesが読み込んだ既定スカイボックスに合わせておく
        m_LoaderSkyboxPath = m_CurrentSkyboxPath;
        // m_LoaderWaterNormalMapPathも同様。CreateSceneResourcesはフラット法線フォールバック
        // (m_CurrentWaterNormalMapPath = 空文字列)から始めるため、ここも空文字列で揃える
        m_LoaderWaterNormalMapPath = m_CurrentWaterNormalMapPath;
        // 通常は0(ファイル名昇順の先頭)。グラフィックスAPIの切り替えで作り直された場合だけ、
        // 呼び出し側が切り替え前のシーン番号を渡してくる。範囲外なら先頭へ落とす
        // (シーン一覧はDiscoverScenesが空でないことを保証済み)
        if (m_InitialSceneIndex >= m_SceneFilePaths.size())
        {
            Core::Logger::Warning(
                "KurenaiEngine3D",
                "指定された起動シーン番号" + std::to_string(m_InitialSceneIndex) + "が範囲外(シーン数: " +
                    std::to_string(m_SceneFilePaths.size()) + ")のため先頭のシーンを読み込みます");
            m_InitialSceneIndex = 0;
        }
        if (std::unique_ptr<LoadedScene> initialScene = LoadSceneOnLoaderThread(m_InitialSceneIndex))
        {
            ApplyLoadedScene(*initialScene);
            // ApplyLoadedSceneはUpdateスレッドへの引き渡しとして公開するだけなので、
            // まだUpdateスレッドが回っていないここでは自分で取り込む
            UpdateAppliedSceneHandoff();
        }
    }

    RHI::Format KurenaiEngine3D::GetEmissiveFormat() const
    {
        // Emissive: 1.0でクリップされると照明器具がHDRな輝度を持てず、ブルームが成立しない。
        // アルファを使わないためR11G11B10_Floatで足りる(帯域はR16G16B16A16_Floatの半分)
        return m_SystemSettings.Precision == BufferPrecision::Legacy8bit ? RHI::Format::R8G8B8A8_UNorm
                                                                : RHI::Format::R11G11B10_Float;
    }

    RHI::Format KurenaiEngine3D::GetAOFormat() const
    {
        // AO/GIバッファ: rgb=間接拡散光(HDR)、a=遮蔽率。間接光は暗い室内では0.02〜0.1に収まり、
        // UNorm8ではコード5〜26の約20階調しか使えずポスタリゼーションする。
        // aに遮蔽率を持つためアルファ付きのR16G16B16A16_Floatを使う
        return m_SystemSettings.Precision == BufferPrecision::Legacy8bit ? RHI::Format::R8G8B8A8_UNorm
                                                                : RHI::Format::R16G16B16A16_Float;
    }

    // 反射プローブの焼き上がりの状態は Passes::ReflectionProbePasses が持つ。
    // ここはImGuiのパネルとシーン読み込みのために委譲するだけで、状態そのものは持たない。
    // **ヘッダではPasses::*を前方宣言しかしていないため、定義はここに置く**
    // DDGIの進行状態は Passes::DDGIPasses が持つ。ここはImGuiと品質設定のために委譲するだけ
    bool& KurenaiEngine3D::GetDDGIEmissiveSuppressLoggedRaster() { return m_DDGIPasses->GetEmissiveSuppressLoggedRaster(); }
    bool& KurenaiEngine3D::GetDDGIEmissiveSuppressLoggedTrace() { return m_DDGIPasses->GetEmissiveSuppressLoggedTrace(); }
    bool& KurenaiEngine3D::GetDDGIUpdateSuspended() { return m_DDGIPasses->GetUpdateSuspended(); }
    uint32_t& KurenaiEngine3D::GetDDGIStableCycles() { return m_DDGIPasses->GetStableCycles(); }
    bool KurenaiEngine3D::GetDDGIWarmingUp() const { return m_DDGIPasses->IsWarmingUp(); }
    // IBLの焼き上がりの状態は Passes::EnvironmentPasses が持つ。ここは委譲するだけ
    bool& KurenaiEngine3D::GetIBLBaked() { return m_EnvironmentPasses->GetIBLBaked(); }
    bool& KurenaiEngine3D::GetIBLIrradianceBaked() { return m_EnvironmentPasses->GetIBLIrradianceBaked(); }
    bool& KurenaiEngine3D::GetProbeBaked() { return m_ReflectionProbePasses->GetProbeBaked(); }
    bool& KurenaiEngine3D::GetProbeBakeRequested() { return m_ReflectionProbePasses->GetProbeBakeRequested(); }
    uint32_t KurenaiEngine3D::GetProbeRealtimeProbeIndex() const { return m_ReflectionProbePasses->GetProbeRealtimeProbeIndex(); }
    uint32_t KurenaiEngine3D::GetProbeRealtimeFace() const { return m_ReflectionProbePasses->GetProbeRealtimeFace(); }

    bool KurenaiEngine3D::ShouldRunRaytracedReflection() const
    {
        return m_ReflectionSettings.Mode == ReflectionMode::Raytraced && m_SceneGPUResources.RaytracingScene.IsValid() &&
               m_ReflectionPasses->HasRaytracedPipelineState() && m_RenderTargets.RTReflectionTexture != nullptr;
    }

    bool KurenaiEngine3D::ShouldRunRaytracedShadow() const
    {
        return m_ShadowSettings.Mode == ShadowMode::Raytraced && m_SceneGPUResources.RaytracingScene.IsValid() &&
               m_ShadowPasses->HasRaytracedPipelineState() && m_RenderTargets.RTShadowTexture != nullptr;
    }

    bool KurenaiEngine3D::ShouldRunMegaLights() const
    {
        if (m_MegaLightsSettings.Mode == MegaLightsMode::Off || !m_SceneGPUResources.RaytracingScene.IsValid() || m_RenderTargets.MegaLightsTexture == nullptr)
        {
            return false;
        }
        // 手法ごとに必要なパイプラインが違う。確率的サンプリングもクアッド共有も
        // 候補プールと初期RIS(リザーバ)を共有し、そのあとの段だけが違う
        if (m_MegaLightsSettings.Mode == MegaLightsMode::Stochastic)
        {
            return m_MegaLightsPasses->HasCommonPipelineStates() && m_MegaLightsPasses->HasShadePipelineState() &&
                   m_RenderTargets.MegaLightsTilePoolBuffer != nullptr &&
                   m_RenderTargets.MegaLightsReservoirBuffer != nullptr;
        }
        if (m_MegaLightsSettings.Mode == MegaLightsMode::QuadShared)
        {
            // Shade ではなく Resolve が色を書く。時間・空間再利用は使わないので、
            // 履歴バッファや空間再利用のping-pongが無くても走れる
            return m_MegaLightsPasses->HasCommonPipelineStates() && m_MegaLightsPasses->HasResolvePipelineState() &&
                   m_RenderTargets.MegaLightsTilePoolBuffer != nullptr &&
                   m_RenderTargets.MegaLightsReservoirBuffer != nullptr &&
                   m_RenderTargets.MegaLightsHistoryGuide[0] != nullptr;
        }
        return m_MegaLightsPasses->HasReferencePipelineState();
    }

    bool KurenaiEngine3D::ShouldRunLightCulling() const
    {
        if (!m_GeometrySettings.LightCullingEnabled)
        {
            return false;
        }
        // ライトグリッドのデバッグ表示は、MegaLightsが走っていてもグリッドそのものを見せるものなので
        // 実行が要る。**ここを落とすと表示が前フレームの残骸か未初期化の中身になる**
        if (m_DebugViewSettings.View == DebugView::LightTiles)
        {
            return true;
        }
        // MegaLightsが走るフレームはポイント/スポットの寄与をあちらが出しており、
        // DirectLighting.hlslのローカルライトのループはLightCount.wで止まっている。
        // それでもグリッドだけは毎フレーム作り続けていた ―― 誰も読まない出力で、
        // 灯数に比例して増える(1026灯で0.198ms。docs 61.7e.3)
        return !ShouldRunMegaLights();
    }

    bool KurenaiEngine3D::ShouldRunRaytracedAO() const
    {
        return m_AmbientOcclusionSettings.Technique == AOTechnique::Raytraced && m_SceneGPUResources.RaytracingScene.IsValid() &&
               m_LightingPasses->HasRaytracedPipelineState() && m_RenderTargets.RTAORawTexture != nullptr &&
               m_RenderTargets.RTAOTexture != nullptr;
    }

    void KurenaiEngine3D::SetDebugViewIndex(int index)
    {
        // 総数はenumのすぐ隣で定義してある(Settings/DebugViewSettings.h)
        if (index < 0 || index >= kDebugViewCount)
        {
            Core::Logger::Warning(
                "KurenaiEngine3D",
                "デバッグ表示の番号が範囲外のため無視します: " + std::to_string(index) + " (0〜" +
                    std::to_string(kDebugViewCount - 1) + ")");
            return;
        }

        m_DebugViewSettings.View = static_cast<DebugView>(index);
        Core::Logger::Info("KurenaiEngine3D", "デバッグ表示を番号で選択しました: " + std::to_string(index));
    }

    bool KurenaiEngine3D::ShouldSuppressEmissiveForGI() const
    {
        // 【プロキシが1つも無いなら抑止しない】発光面を光源にしていないのに
        // DDGIから自発光だけ抜くと、その面の照明が丸ごと落ちる
        return m_EmissiveLightSettings.LightsEnabled && !m_EmissiveLightSettings.LightsDoubleCountGI && !m_EmissiveProxies.empty();
    }

    void KurenaiEngine3D::SetEmissiveLights(int enabled, float cutoffIrradiance, int maxCount, int doubleCountGI)
    {
        // 負は「既定のまま」。しきい値だけ差し替えたいときに状態を巻き添えで倒さないため
        if (enabled >= 0)
        {
            m_EmissiveLightSettings.LightsEnabled = (enabled > 0);
        }
        // 0以下は「既定のまま」。OverrideMegaLightsの負値と同じ約束にしてある
        bool cutoffChanged = false;
        if (cutoffIrradiance > 0.0f)
        {
            cutoffChanged = (m_EmissiveLightSettings.LightsCutoffIrradiance != cutoffIrradiance);
            m_EmissiveLightSettings.LightsCutoffIrradiance = cutoffIrradiance;
        }
        if (maxCount > 0)
        {
            m_EmissiveLightSettings.LightsMaxCount = maxCount;
        }
        if (doubleCountGI >= 0)
        {
            m_EmissiveLightSettings.LightsDoubleCountGI = (doubleCountGI > 0);
        }
        // 【三角形テーブルを焼き直す】メッシュライトの影響半径は読み込み時に焼くので、
        // τを変えても焼き直さないと**つまみが静かに効かない**。シーンの読み込みは
        // 設定の適用より先に走るため、ここで焼き直さないと起動引数すら届かない
        // (実際に踏んだ。τを100分の1にしてもダンプがバイト完全一致した)
        if (cutoffChanged && m_Device && !m_Scene.Instances.empty())
        {
            m_MeshLightScene.Build(*m_Device, m_Scene, m_EmissiveLightSettings.LightsCutoffIrradiance);
        }

        // 上限の警告は設定を変えたら出し直す(τを上げてRangeを縮めた結果を見たいため)
        m_EmissiveLightsCapLogged = false;
        Core::Logger::Info(
            "KurenaiEngine3D",
            std::string("エミッシブ光源: ") + (m_EmissiveLightSettings.LightsEnabled ? "有効" : "無効") +
                " / 打ち切り照度 " + std::to_string(m_EmissiveLightSettings.LightsCutoffIrradiance) + " / 上限 " +
                std::to_string(m_EmissiveLightSettings.LightsMaxCount) + "個 / プロキシ " +
                std::to_string(m_EmissiveProxies.size()) + "個 / DDGIの自発光 " +
                (ShouldSuppressEmissiveForGI() ? "抑止" : "そのまま(二重計上)"));
    }

    void KurenaiEngine3D::SetMeshLights(int enabled)
    {
        // 負は「既定のまま」(SetEmissiveLights と同じ約束)
        if (enabled < 0)
        {
            return;
        }
        m_MeshLightsEnabled = (enabled > 0);

        // 【効かない組み合わせを黙って受け付けない】有効にしたのに何も起きない状態は、
        // 「実装が壊れている」と「前提が揃っていない」の区別がつかない。
        // 実際にどちらへ落ちるかをここで言い切る
        std::string note;
        if (m_MeshLightsEnabled)
        {
            if (!m_EmissiveLightSettings.LightsEnabled)
            {
                note = " ※エミッシブ光源が無効なので三角形は出ない";
            }
            else if (!m_MeshLightScene.IsValid())
            {
                note = " ※三角形テーブルが空(発光メッシュが無いか読み込み前)";
            }
            else if (!ShouldRunMegaLights())
            {
                note = " ※MegaLightsが走らない構成(DX11/非DXR/無効)なので段階1のプロキシが光る";
            }
        }
        Core::Logger::Info(
            "KurenaiEngine3D",
            std::string("メッシュライト: ") + (m_MeshLightsEnabled ? "有効" : "無効") + " / 三角形 " +
                std::to_string(m_MeshLightScene.GetTriangleCount()) + "枚" + note);
    }

    void KurenaiEngine3D::SetEmissiveIntensity(float intensity)
    {
        if (intensity <= 0.0f)
        {
            return;
        }
        m_EmissiveLightSettings.Intensity = intensity;
        // 倍率を変えるとRangeも変わる(強さから解いているため)。上限の警告を出し直す
        m_EmissiveLightsCapLogged = false;
        m_EmissiveLightsValuesLogged = false;
        Core::Logger::Info(
            "KurenaiEngine3D", "自発光の強度: " + std::to_string(m_EmissiveLightSettings.Intensity) + "倍");
    }

    void KurenaiEngine3D::OverrideMegaLights(int mode, int shadowRayCount, int sampleCount)
    {
        // 手法の総数はenumの末尾で決まる。値はUIのコンボの並びとも一致している
        constexpr int kMegaLightsModeCount = static_cast<int>(MegaLightsMode::QuadShared) + 1;
        if (mode >= kMegaLightsModeCount)
        {
            Core::Logger::Warning(
                "KurenaiEngine3D",
                "MegaLightsの手法の番号が範囲外のため無視します: " + std::to_string(mode) + " (0〜" +
                    std::to_string(kMegaLightsModeCount - 1) + ")");
        }
        // 負の値は「既定のまま」。本数側と同じ約束にしてある
        else if (mode >= 0)
        {
            m_MegaLightsSettings.Mode = static_cast<MegaLightsMode>(mode);
            Core::Logger::Info(
                "KurenaiEngine3D", "MegaLightsの手法を番号で選択しました: " + std::to_string(mode));

            if (m_MegaLightsSettings.Mode != MegaLightsMode::Off && !m_RenderCapabilities.RaytracingAvailable)
            {
                // 黙って何も起きないと「効かないバグ」に見えるので、必ず理由を残す
                Core::Logger::Warning(
                    "KurenaiEngine3D",
                    "この環境ではレイトレーシングが使えないため、MegaLightsのパスは実行されません"
                    "(DX12かつDXR Tier 1.1が必要)");
            }
        }

        // 負の値は「既定のまま」。0は恒等テストとして意味のある値なので弾かない
        if (shadowRayCount >= 0)
        {
            m_MegaLightsSettings.ShadowRayCount = shadowRayCount;
            Core::Logger::Info(
                "KurenaiEngine3D",
                "MegaLightsの1灯あたりの影レイ本数を設定しました: " + std::to_string(shadowRayCount));
        }

        // RISのM。0以下は意味を成さないので1以上に丸める
        if (sampleCount > 0)
        {
            m_MegaLightsSettings.SampleCount = sampleCount;
            Core::Logger::Info(
                "KurenaiEngine3D",
                "MegaLightsの初期候補数Mを設定しました: " + std::to_string(sampleCount));
        }
    }

    void KurenaiEngine3D::SetMegaLightsAccumFrames(int frames)
    {
        if (frames < 0)
        {
            Core::Logger::Warning(
                "KurenaiEngine3D",
                "MegaLightsの蓄積フレーム数が負のため無視します: " + std::to_string(frames));
            return;
        }

        m_MegaLightsSettings.AccumTargetFrames = frames;
        // 枚数を変えたら取り直す。途中まで足した状態に継ぎ足すと、
        // 「何サンプルの平均か」が分からなくなる
        m_MegaLightsPasses->ResetAccumFrames();
        Core::Logger::Info(
            "KurenaiEngine3D", "MegaLightsの蓄積フレーム数を設定しました: " + std::to_string(frames));
    }

    void KurenaiEngine3D::SetMegaLightsSpatial(int enabled, int neighborCount, int radius, int useMIS)
    {
        if (enabled >= 0)
        {
            m_MegaLightsSettings.SpatialEnabled = (enabled != 0);
            Core::Logger::Info(
                "KurenaiEngine3D",
                std::string("MegaLightsの空間再利用を") + (m_MegaLightsSettings.SpatialEnabled ? "有効" : "無効") +
                    "にしました");
        }
        // 0は「近傍を借りない」= 実質無効として意味があるので弾かない
        if (neighborCount >= 0)
        {
            m_MegaLightsSettings.SpatialNeighborCount = neighborCount;
            Core::Logger::Info(
                "KurenaiEngine3D",
                "MegaLightsの空間再利用で借りる近傍の数を設定しました: " + std::to_string(neighborCount));
        }
        if (radius > 0)
        {
            m_MegaLightsSettings.SpatialRadius = radius;
            Core::Logger::Info(
                "KurenaiEngine3D",
                "MegaLightsの空間再利用の半径を設定しました: " + std::to_string(radius));
        }
        if (useMIS >= 0)
        {
            m_MegaLightsSettings.SpatialMIS = (useMIS != 0);
            Core::Logger::Info(
                "KurenaiEngine3D",
                std::string("MegaLightsの空間再利用の結合を") +
                    (m_MegaLightsSettings.SpatialMIS ? "生成化バランスヒューリスティック" : "confidence重み") +
                    "にしました");
        }
    }

    void KurenaiEngine3D::SetAutoExposureEnabled(bool enabled)
    {
        m_PostProcessSettings.AutoExposureEnabled = enabled;
        Core::Logger::Info(
            "KurenaiEngine3D",
            std::string("自動露出を起動オプションで設定しました: ") + (enabled ? "有効" : "無効"));
    }

    void KurenaiEngine3D::SetOcclusionCullingEnabled(bool enabled)
    {
        m_GeometrySettings.OcclusionCullingEnabled = enabled;
        Core::Logger::Info(
            "KurenaiEngine3D",
            std::string("Hi-Zオクルージョンカリングを起動オプションで設定しました: ")
                + (enabled ? "有効" : "無効"));
    }

    void KurenaiEngine3D::SetMeshletRenderingEnabled(bool enabled)
    {
        m_GeometrySettings.MeshletRenderingEnabled = enabled;
        Core::Logger::Info(
            "KurenaiEngine3D",
            std::string("メッシュレット描画を起動オプションで設定しました: ")
                + (enabled ? "有効" : "無効"));
    }

    void KurenaiEngine3D::SetTAAEnabled(bool enabled)
    {
        m_PostProcessSettings.TAAEnabled = enabled;
        Core::Logger::Info(
            "KurenaiEngine3D",
            std::string("TAAを起動オプションで設定しました: ") + (enabled ? "有効" : "無効"));
    }

    void KurenaiEngine3D::SetAOTechnique(int technique)
    {
        if (technique < static_cast<int>(AOTechnique::SSAO) || technique > static_cast<int>(AOTechnique::Raytraced))
        {
            Core::Logger::Error("KurenaiEngine3D", "SetAOTechnique: 不正な値です: " + std::to_string(technique));
            return;
        }
        m_AmbientOcclusionSettings.Technique = static_cast<AOTechnique>(technique);
        Core::Logger::Info("KurenaiEngine3D", "AO手法を設定しました: " + std::to_string(technique));
    }

    void KurenaiEngine3D::SetSoftwareRasterEnabled(bool enabled)
    {
        m_GeometrySettings.SoftwareRasterEnabled = enabled;
        Core::Logger::Info("KurenaiEngine3D", std::string("ソフトウェアラスタライザを設定しました: ") + (enabled ? "有効" : "無効"));
    }

    void KurenaiEngine3D::SetDDGIHalfResolutionEnabled(bool enabled)
    {
        m_DDGISettings.HalfResolution = enabled;
        Core::Logger::Info("KurenaiEngine3D", std::string("DDGI半解像度を設定しました: ") + (enabled ? "有効" : "無効"));
    }

    void KurenaiEngine3D::SetProbeUpdateMode(int mode)
    {
        if (mode < static_cast<int>(ProbeUpdateMode::Baked) || mode > static_cast<int>(ProbeUpdateMode::Realtime))
        {
            Core::Logger::Error("KurenaiEngine3D", "SetProbeUpdateMode: 不正な値です: " + std::to_string(mode));
            return;
        }
        m_ReflectionProbeSettings.UpdateMode = static_cast<ProbeUpdateMode>(mode);
        Core::Logger::Info("KurenaiEngine3D", "反射プローブ更新モードを設定しました: " + std::to_string(mode));
    }

    void KurenaiEngine3D::SetUpscaleEnabled(bool enabled)
    {
        // UI と同じく、現在の品質モードと出力解像度を保ったまま有効状態だけを変える。
        RequestUpscaleSettings(enabled, m_PostProcessSettings.UpscaleQuality, m_PostProcessSettings.UpscaleOutputWidth, m_PostProcessSettings.UpscaleOutputHeight);
        Core::Logger::Info("KurenaiEngine3D", std::string("超解像を設定しました: ") + (enabled ? "有効" : "無効"));
    }

    void KurenaiEngine3D::SetFixedTimeStep(float seconds)
    {
        if (!std::isfinite(seconds) || seconds <= 0.0f)
        {
            Core::Logger::Error("KurenaiEngine3D", "SetFixedTimeStep: 0以下の値は設定できません: " + std::to_string(seconds));
            return;
        }
        m_FixedTimeStep = seconds;
        Core::Logger::Info("KurenaiEngine3D", "固定タイムステップを設定しました: " + std::to_string(seconds) + " 秒");
    }

    void KurenaiEngine3D::SetPerfDump(const wchar_t* path, int frames)
    {
        if (path == nullptr || path[0] == L'\0' || frames <= 0)
        {
            m_PerfDumpPath.clear();
            m_PerfDumpTargetFrames = 0;
            return;
        }
        m_PerfDumpPath = path;
        m_PerfDumpTargetFrames = frames;
        m_PerfDumpWarmupFrames = 0;
        m_PerfDumpCollected = 0;
        m_PerfDumpDone = false;
        m_PerfDumpTotals.clear();
        Core::Logger::Info(
            "KurenaiEngine3D",
            "GPU計測の書き出しを設定しました(計測用): " + Core::WideToUtf8(m_PerfDumpPath) + " / " +
                std::to_string(frames) + "フレーム");
    }

    void KurenaiEngine3D::SetPassManifest(const wchar_t* path, int frames)
    {
        if (path == nullptr || path[0] == L'\0' || frames < 1)
        {
            m_PassManifestPath.clear();
            m_PassManifestTargetFrames = 1;
            m_PassManifestIssuedFrames = 0;
            m_PassManifestIssued = false;
            Core::Logger::Error("KurenaiEngine3D", "パスマニフェストの出力設定が不正です");
            return;
        }

        m_PassManifestPath = path;
        m_PassManifestTargetFrames = static_cast<uint32_t>(frames);
        m_PassManifestIssuedFrames = 0;
        m_PassManifestIssued = false;
        Core::Logger::Info(
            "KurenaiEngine3D", "パスマニフェストの出力を設定しました: " + Core::WideToUtf8(path) +
                " (frames=" + std::to_string(frames) + ")");
    }

    void KurenaiEngine3D::SetMegaLightsSpatialIterations(int iterations)
    {
        if (iterations < 1)
        {
            Core::Logger::Warning(
                "KurenaiEngine3D",
                "MegaLightsの空間再利用の反復回数に1未満が指定されたため、既定のままにします: " +
                    std::to_string(iterations));
            return;
        }
        const int clamped = std::min(iterations, static_cast<int>(kMegaLightsMaxSpatialIterations));
        if (clamped != iterations)
        {
            Core::Logger::Warning(
                "KurenaiEngine3D",
                "MegaLightsの空間再利用の反復回数が上限を超えたため頭打ちにしました: " +
                    std::to_string(iterations) + " -> " + std::to_string(clamped));
        }
        m_MegaLightsSettings.SpatialIterations = clamped;
        Core::Logger::Info(
            "KurenaiEngine3D",
            "MegaLightsの空間再利用の反復回数を設定しました: " + std::to_string(clamped));
    }

    void KurenaiEngine3D::SetMegaLightsDenoiseFireflyClamp(float k)
    {
        if (k < 0.0f)
        {
            Core::Logger::Warning(
                "KurenaiEngine3D",
                "MegaLightsのファイアフライのクランプに負の値が指定されたため、既定のままにします: " +
                    std::to_string(k));
            return;
        }
        m_MegaLightsSettings.DenoiseFireflyClamp = k;
        Core::Logger::Info(
            "KurenaiEngine3D",
            "MegaLightsのファイアフライのクランプを設定しました: " + std::to_string(k));
    }

    void KurenaiEngine3D::SetMegaLightsDenoiseSigmaLuminance(float sigma)
    {
        if (!(sigma > 0.0f))
        {
            Core::Logger::Warning(
                "KurenaiEngine3D",
                "MegaLightsのデノイザのσ(輝度)に正でない値が指定されたため、既定のままにします: " +
                    std::to_string(sigma));
            return;
        }
        m_MegaLightsSettings.DenoiseSigmaLuminance = sigma;
        Core::Logger::Info(
            "KurenaiEngine3D",
            "MegaLightsのデノイザのσ(輝度)を設定しました: " + std::to_string(sigma));
    }

    void KurenaiEngine3D::SetMegaLightsDenoise(int enabled, int atrousPasses, int maxFrames)
    {
        // 負の値・0は「既定のまま」。他のMegaLightsオプションと同じ約束
        if (enabled >= 0)
        {
            m_MegaLightsSettings.DenoiseEnabled = (enabled != 0);
            // 切り替えた瞬間の履歴は今の設定で作られたものではないので捨てる
            m_MegaLightsPasses->InvalidateDenoiseHistory();
            Core::Logger::Info(
                "KurenaiEngine3D",
                std::string("MegaLightsのデノイザを") + (m_MegaLightsSettings.DenoiseEnabled ? "有効" : "無効") +
                    "にしました");
        }
        // 0段は「時間累積だけ」で意味があるので弾かない
        if (atrousPasses >= 0)
        {
            m_MegaLightsSettings.DenoiseAtrousPasses = atrousPasses;
            Core::Logger::Info(
                "KurenaiEngine3D",
                "MegaLightsのデノイザのa-trousの段数を設定しました: " + std::to_string(atrousPasses));
        }
        if (maxFrames > 0)
        {
            // 【両方の手法へ入れる】計測用のつまみなので、指定したのに走っている手法の
            // ほうが読まれない、という取りこぼしを作らない
            m_MegaLightsSettings.DenoiseMaxFrames = maxFrames;
            m_MegaLightsSettings.QuadDenoiseMaxFrames = maxFrames;
            Core::Logger::Info(
                "KurenaiEngine3D",
                "MegaLightsのデノイザの時間累積の上限を設定しました: " + std::to_string(maxFrames));
        }
    }

    void KurenaiEngine3D::SetMegaLightsPerturb(int mode)
    {
        if (mode < 0 || mode > 2)
        {
            Core::Logger::Warning(
                "KurenaiEngine3D",
                "MegaLightsの摂動モードが範囲外のため無視します: " + std::to_string(mode) + " (0〜2)");
            return;
        }
        m_MegaLightsSettings.PerturbMode = mode;
        m_MegaLightsPerturbApplied = false;
        Core::Logger::Info(
            "KurenaiEngine3D", "MegaLightsの摂動モードを設定しました(検証用): " + std::to_string(mode));
    }

    void KurenaiEngine3D::SetMegaLightsTemporal(int enabled, int mClamp)
    {
        // 負の値は「既定のまま」。他のMegaLightsオプションと同じ約束
        if (enabled >= 0)
        {
            m_MegaLightsSettings.TemporalEnabled = (enabled != 0);
            // 切り替えた瞬間の履歴は今の設定で作られたものではないので捨てる
            m_MegaLightsPasses->InvalidateHistory();
            Core::Logger::Info(
                "KurenaiEngine3D",
                std::string("MegaLightsの時間再利用を") + (m_MegaLightsSettings.TemporalEnabled ? "有効" : "無効") +
                    "にしました");
        }
        if (mClamp > 0)
        {
            m_MegaLightsSettings.TemporalMClamp = mClamp;
            Core::Logger::Info(
                "KurenaiEngine3D",
                "MegaLightsの時間再利用のMの上限を設定しました: " + std::to_string(mClamp));
        }
    }

    void KurenaiEngine3D::SetMegaLightsInitialVisibility(int enabled)
    {
        // 負の値は「既定のまま」。他のMegaLightsオプションと同じ約束
        if (enabled >= 0)
        {
            m_MegaLightsSettings.InitialVisibility = (enabled != 0);
            Core::Logger::Info(
                "KurenaiEngine3D",
                std::string("MegaLightsの初期可視レイを") + (m_MegaLightsSettings.InitialVisibility ? "有効" : "無効") +
                    "にしました");
        }
    }

    void KurenaiEngine3D::SetMegaLightsQuadShare(int share, int stratify, int blockedCache)
    {
        // 負の値は「既定のまま」。他のMegaLightsオプションと同じ約束
        if (share >= 0)
        {
            m_MegaLightsSettings.QuadShareEnabled = (share != 0);
            Core::Logger::Info(
                "KurenaiEngine3D",
                std::string("MegaLightsのクアッド共有を") + (m_MegaLightsSettings.QuadShareEnabled ? "有効" : "無効") +
                    "にしました");
        }
        if (stratify >= 0)
        {
            m_MegaLightsSettings.QuadStratify = (stratify != 0);
            Core::Logger::Info(
                "KurenaiEngine3D",
                std::string("MegaLightsのクアッド層化を") + (m_MegaLightsSettings.QuadStratify ? "有効" : "無効") +
                    "にしました");
        }
        if (blockedCache >= 0)
        {
            m_MegaLightsSettings.BlockedCacheEnabled = (blockedCache != 0);
            Core::Logger::Info(
                "KurenaiEngine3D",
                std::string("MegaLightsの遮蔽キャッシュを") + (m_MegaLightsSettings.BlockedCacheEnabled ? "有効" : "無効") +
                    "にしました");
        }
    }

    void KurenaiEngine3D::SetMegaLightsQuadSamples(int samples)
    {
        // 負の値は「既定のまま」。他のMegaLightsオプションと同じ約束
        if (samples < 0)
        {
            return;
        }
        if (samples < 1 || samples > kMegaLightsMaxSamplesPerPixel)
        {
            Core::Logger::Warning(
                "KurenaiEngine3D",
                "MegaLightsのクアッド標本数が範囲外のため無視します: " + std::to_string(samples) +
                    " (1〜" + std::to_string(kMegaLightsMaxSamplesPerPixel) + ")");
            return;
        }
        if (samples == m_MegaLightsSettings.QuadSamplesPerPixel)
        {
            return;
        }
        m_MegaLightsSettings.QuadSamplesPerPixel = samples;
        // リザーババッファの大きさが変わる。GPUが参照していない状態で作り直す必要があるので、
        // 解像度変更と同じ「フレームの先頭でまとめて作り直す」経路に乗せる
        m_MegaLightsReservoirDirty = true;
        Core::Logger::Info(
            "KurenaiEngine3D",
            "MegaLightsのクアッド標本数を " + std::to_string(m_MegaLightsSettings.QuadSamplesPerPixel) +
                " にしました(影レイの本数も同じ数になります)");
    }

    void KurenaiEngine3D::SetMegaLightsTilePoolCapacity(int capacity)
    {
        // 負の値は「既定のまま」。他のMegaLightsオプションと同じ約束
        if (capacity < 0)
        {
            return;
        }
        if (capacity < kMegaLightsTilePoolMinCapacity ||
            capacity > static_cast<int>(kMegaLightsTilePoolCapacity))
        {
            Core::Logger::Warning(
                "KurenaiEngine3D",
                "MegaLightsの候補プールの容量が範囲外のため無視します: " + std::to_string(capacity) +
                    " (" + std::to_string(kMegaLightsTilePoolMinCapacity) + "〜" +
                    std::to_string(kMegaLightsTilePoolCapacity) + ")");
            return;
        }
        if (capacity == m_MegaLightsSettings.TilePoolCapacity)
        {
            return;
        }
        m_MegaLightsSettings.TilePoolCapacity = capacity;
        Core::Logger::Info(
            "KurenaiEngine3D",
            "MegaLightsの候補プールの容量を " + std::to_string(m_MegaLightsSettings.TilePoolCapacity) +
                " にしました");
    }

    void KurenaiEngine3D::SetMegaLightsTileJitter(int mode)
    {
        // 負の値は「既定のまま」。未指定時も現在値を起動ログへ残すためreturnしない
        if (mode >= 0)
        {
            if (mode > 2)
            {
                Core::Logger::Warning(
                    "KurenaiEngine3D",
                    "MegaLightsのタイル格子ジッターのモードが範囲外のため無視します: " +
                        std::to_string(mode) + " (0〜2)");
            }
            else
            {
                m_MegaLightsSettings.TileJitterMode = mode;
            }
        }

        if (m_MegaLightsSettings.TileJitterMode == 1)
        {
            Core::Logger::Info(
                "KurenaiEngine3D",
                "MegaLightsのタイル格子ジッター: 有効 (m_TAAFrameIndexのHalton(2,3)を16段階へ量子化)");
        }
        else if (m_MegaLightsSettings.TileJitterMode == 2)
        {
            Core::Logger::Info(
                "KurenaiEngine3D", "MegaLightsのタイル格子ジッター: 有効 (検証用オフセット(0,0)固定)");
        }
        else
        {
            Core::Logger::Info("KurenaiEngine3D", "MegaLightsのタイル格子ジッター: 無効");
        }
    }

    int32_t KurenaiEngine3D::MegaLightsSamplesPerPixel() const
    {
        // 【手法3以外は必ず1】手法2の時間・空間再利用は「1画素1リザーバ」を前提に
        // 添字を組み立てているので、ここを1より大きくすると別画素の標本を読む
        if (m_MegaLightsSettings.Mode != MegaLightsMode::QuadShared)
        {
            return 1;
        }
        return std::clamp(m_MegaLightsSettings.QuadSamplesPerPixel, 1, kMegaLightsMaxSamplesPerPixel);
    }

    void KurenaiEngine3D::SetMegaLightsDumpPath(const wchar_t* path)
    {
        if (path == nullptr || path[0] == L'\0')
        {
            m_MegaLightsPasses->ClearDumpPath();
            return;
        }

        m_MegaLightsPasses->SetDumpPath(path);
        Core::Logger::Info(
            "KurenaiEngine3D", "MegaLightsの蓄積平均の書き出し先を設定しました: " + Core::WideToUtf8(path));
    }

    void KurenaiEngine3D::ForceDDGIRayModeRaster()
    {
        m_DDGISettings.RayMode = DDGIRayMode::Raster;
        Core::Logger::Info("KurenaiEngine3D", "DDGIのレイ取得をラスタライズへ固定しました(起動オプション)");
    }

    void KurenaiEngine3D::SetDDGIBackfaceThreshold(float threshold)
    {
        if (threshold <= 0.0f)
        {
            m_DDGISettings.ProbeClassificationEnabled = false;
            Core::Logger::Info("KurenaiEngine3D", "DDGIのプローブ分類を無効にしました(起動オプション)");
            return;
        }

        m_DDGISettings.ProbeClassificationEnabled = true;
        m_DDGISettings.BackfaceThreshold = threshold;
        Core::Logger::Info(
            "KurenaiEngine3D",
            "DDGIのプローブ分類のしきい値を設定しました(起動オプション): " + std::to_string(threshold));
    }

    void KurenaiEngine3D::OverrideDDGILOD(uint32_t lodCount, bool followCamera)
    {
        if (!m_GIResources.HasGIVolume)
        {
            Core::Logger::Warning("KurenaiEngine3D", "[GIVolume]が無いためLODの上書きは効きません");
            return;
        }

        // 0は「.ksceneの指定のまま」を意味する(追従だけを切り替えたいとき)
        const uint32_t requested = (lodCount == 0u) ? m_GIResources.GIVolume.LODCount : lodCount;
        const uint32_t clamped = std::clamp(requested, 1u, kDDGIMaxLODCount);
        const uint64_t probeCount =
            static_cast<uint64_t>(m_GIResources.GIVolume.ProbeCounts[0]) *
            static_cast<uint64_t>(m_GIResources.GIVolume.ProbeCounts[1]) *
            static_cast<uint64_t>(m_GIResources.GIVolume.ProbeCounts[2]) *
            static_cast<uint64_t>(clamped);
        if (probeCount > kDDGIMaxProbes)
        {
            Core::Logger::Error(
                "KurenaiEngine3D",
                "LODの上書きでプローブ数が上限(" + std::to_string(kDDGIMaxProbes) + ")を超えるため無視します: " +
                    std::to_string(probeCount) + "個");
            return;
        }

        m_GIResources.GIVolume.LODCount = clamped;
        m_GIResources.GIVolume.FollowCamera = followCamera;
        // 段数が変わるとアトラスの行数が変わるので確保し直す(中身も作り直しになる)
        RecreateDDGIAtlases();
        Core::Logger::Info(
            "KurenaiEngine3D",
            "DDGIのLODを上書きしました(起動オプション): 段数 " + std::to_string(clamped) +
                " / カメラ追従 " + (followCamera ? "有効" : "無効"));
    }

    bool KurenaiEngine3D::ShouldRunRaytracedDDGITrace() const
    {
        return m_DDGISettings.RayMode == DDGIRayMode::Raytraced && m_SceneGPUResources.RaytracingScene.IsValid() &&
               m_DDGIPasses->HasRaytracedTraceResources();
    }

    bool KurenaiEngine3D::ShouldUseMeshletPath(
        const Assets::Model& model, const Assets::Mesh& mesh, bool isWater) const
    {
        // 【水面はメッシュレット経路に載せない】水面のピクセルシェーダーはWater.hlslの
        // PSMainで、G-Buffer本体のPSMainとは別物。メッシュシェーダー版を用意するには
        // PSOをもう2本(通常/ミラー)増やすことになるが、水面は.ksceneが置く平面1枚で
        // 三角形数が少なく、メッシュレットカリングの利得がほとんど無い
        if (isWater)
        {
            return false;
        }

        // メッシュレットが焼かれていない(--no-meshletsでパックされた.kmodel)、
        // またはデバイスが非対応でGPUバッファを作っていない場合はnullptrになる。
        // 【MeshletCountで判定しないこと】あちらはアセットが持つ数そのもので、
        // メッシュシェーダー非対応の環境でも(レイトレーシングが使うため)0にはならない。
        // 表はモデル単位なので、このメッシュ自身が塊を持っているかも併せて見る
        // (モデル内に塊を持たないメッシュが混ざりうる)
        if (!model.MeshletBuffer || !model.MaterialTableBuffer || mesh.MeshletCount == 0)
        {
            return false;
        }

        return m_GeometrySettings.MeshletRenderingEnabled && m_GeometryPasses->HasMeshletPipelineState();
    }

    uint32_t KurenaiEngine3D::GetHiZMipLevels() const
    {
        return m_GeometryPasses->GetHiZMipLevels();
    }
    bool KurenaiEngine3D::IsMeshVisibleCounted(
        const Rendering::FrustumPlanes& frustum, const Assets::ModelInstance& instance,
        const Assets::Model& model, const Assets::Mesh& mesh)
    {
        return Rendering::IsMeshVisibleWithStats(
            m_GeometrySettings.MeshCullingEnabled, frustum, instance, model, mesh,
            m_MeshCullTested, m_MeshCullCulled);
    }

    bool KurenaiEngine3D::ShouldUseModelMeshletPath(
        const Assets::ModelInstance& instance, const Assets::Model& model) const
    {
        // モデル内の1メッシュでも従来経路へ落ちる条件があるなら、モデル全体を従来経路にする。
        // 混ぜると「1ドローで描いたぶん」と「メッシュ単位で描いたぶん」が同じフレームに
        // 同居し、食い違いが出たときにどちらのせいか切り分けられなくなる
        if (!model.AllMeshesHaveMeshlets)
        {
            return false;
        }
        if (model.Meshes.empty())
        {
            return false;
        }

        // 代表として先頭のメッシュで判定する。AllMeshesHaveMeshletsが真なら
        // メッシュ間で結果は変わらない(残りの条件はすべてモデル単位/インスタンス単位)
        return ShouldUseMeshletPath(model, model.Meshes.front(), instance.IsWater);
    }

    RHI::IRHITexture* KurenaiEngine3D::GetActiveAOTexture() const
    {
        if (!m_AmbientOcclusionSettings.Enabled)
        {
            return m_AODisabledTexture.get();
        }
        if (ShouldRunRaytracedAO())
        {
            return m_RenderTargets.RTAOTexture.get();
        }
        if (m_AmbientOcclusionSettings.Technique == AOTechnique::SSILVisibilityBitmask)
        {
            return m_RenderTargets.SSILTexture.get();
        }
        // SSAO、およびRaytracedを選んでいても実行できないフレーム(高速化構造が無い等)
        return m_RenderTargets.SSAOTexture.get();
    }

    RHI::IRHITexture* KurenaiEngine3D::GetActiveAORawTexture() const
    {
        if (!m_AmbientOcclusionSettings.Enabled)
        {
            return m_AODisabledTexture.get();
        }
        if (ShouldRunRaytracedAO())
        {
            return m_RenderTargets.RTAORawTexture.get();
        }
        if (m_AmbientOcclusionSettings.Technique == AOTechnique::SSILVisibilityBitmask)
        {
            return m_RenderTargets.SSILRawTexture.get();
        }
        return m_RenderTargets.SSAORawTexture.get();
    }

    RHI::IRHITexture* KurenaiEngine3D::GetActiveReflectionOutput() const
    {
        if (m_ReflectionSettings.Mode == ReflectionMode::ScreenSpace)
        {
            return m_RenderTargets.SSRTexture.get();
        }
        if (ShouldRunRaytracedReflection())
        {
            return m_RenderTargets.RTReflectionTexture.get();
        }
        // 反射なし、またはRT反射を実行しなかった場合はLightingパスの結果をそのまま後段へ渡す
        return m_RenderTargets.SceneColor.get();
    }

    void KurenaiEngine3D::CreatePrecisionDependentPipelineStates()
    {
        const RHI::Format emissiveFormat = GetEmissiveFormat();
        const RHI::Format aoFormat = GetAOFormat();

        try
        {
            // 【元の行位置のまま呼ぶ】DX12はディスクリプタ枠を生成順に割り当てるため、
            // 所有権をGeometryPassesへ移しても生成の順序はここから動かさない
            m_GeometryPasses->CreatePrecisionDependentPipelineStates(
                *m_Device, emissiveFormat, GetModelInputLayout());

            m_LightingPasses->CreatePrecisionDependentPipelineStates(*m_Device, aoFormat);
        }
        catch (const std::exception& e)
        {
            // ここで失敗するとG-Buffer/AOパスが描けず復旧手段が無いため、ログを残して投げ直す
            Core::Logger::Error(
                "KurenaiEngine3D",
                std::string("バッファ精度に依存するパイプラインステートの作成に失敗しました (バッファ精度=") +
                    (m_SystemSettings.Precision == BufferPrecision::Legacy8bit ? "Legacy8bit" : "HDR") + "): " + e.what());
            throw;
        }
    }

    void KurenaiEngine3D::CreateSamplerSets()
    {
        // スロットの並びはShaders/3D/Samplers.hlsliの役割定義と一致させること
        // (s0 = MaterialSampler、s1 = ColorSampler、s2 = DataSampler、s3 = VolumeSampler)。

        // 色バッファ・LUT用。UVの端が定義域の端なのでClamp、拡縮でブロック状にならないようLinear。
        // BRDF積分LUTをWrapで引くと何が起きるかはdocs/Architecture.html 14.2.1節
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

        // ボリュームテクスチャ(3Dノイズ)用。ワールド空間で無限にタイリングして引くためWrapが必須で、
        // Clampだと周期の境界でトライリニア補間のタップが端のテクセルに張り付き継ぎ目が出る
        // (シェーダー側でfrac()しても補間がテクスチャの端を跨げないため消せない)。
        // レイマーチで等方的に刻んで引くので異方性フィルタは意味を持たずLinearでよい
        RHI::SamplerDesc volumeSampler{};
        volumeSampler.Filter = RHI::SamplerFilter::Linear;
        volumeSampler.AddressMode = RHI::SamplerAddressMode::Wrap;

        const RHI::SamplerDesc materialSet[] = { materialSampler, colorSampler, dataSampler, volumeSampler };
        m_MaterialSamplers = m_Device->CreateSamplerSet(materialSet, static_cast<uint32_t>(std::size(materialSet)));

        // スクリーン空間パスは画面内の中間バッファしか読まないため、s0にもWrapを置かない。
        // 万一シェーダ側で役割を選び違えても、画面端でUVが反対側へ回り込む不具合が起きないようにする。
        // 【s3のVolumeSamplerだけはこの原則の例外】引くのは画面UVではなくワールド空間の3D座標から
        // 作ったUVWなので、回り込む先の「反対側の画面端」がそもそも存在しない。詳細はSamplers.hlsliの
        // VolumeSamplerの宣言に書いてある
        const RHI::SamplerDesc screenSpaceSet[] = { colorSampler, colorSampler, dataSampler, volumeSampler };
        m_ScreenSpaceSamplers = m_Device->CreateSamplerSet(screenSpaceSet, static_cast<uint32_t>(std::size(screenSpaceSet)));
    }

    RHI::IRHITexture* KurenaiEngine3D::ActiveSkyTexture() const
    {
        // .ksceneが[Scene]Skyboxを明示しているシーンは、そのDDSでなければ意味を成さない
        // (White Furnace Testの一様放射輝度キューブマップが該当する)。手続き空で
        // 上書きしてしまうと検証そのものが壊れるため、明示指定があるときは必ずDDSを使う
        const bool useProcedural = m_SkySettings.ProceduralEnabled && m_Scene.SkyboxPath.empty();
        return useProcedural ? m_SkyResources.ProceduralSkyTexture.get() : m_SkyboxTexture.get();
    }

    KurenaiEngine3D::CloudBakeSignature KurenaiEngine3D::MakeCloudBakeSignature() const
    {
        // 【FrameConstants/SkyIntegrateConstantsと同じ潰し方をすること】無効化のときに
        // 何が0になるかが揃っていないと、「無効にしたのに焼き直しが走らない」取りこぼしが出る。
        // 対応するのはRender()のconstants.CloudParams0〜3・FogParams0の組み立て箇所
        CloudBakeSignature s;
        s.CumulusCoverage = m_CloudSettings.Enabled ? m_CloudSettings.Coverage : 0.0f;
        s.CumulusAltitude = m_CloudSettings.Altitude;
        s.CumulusUvScale = m_CloudSettings.UvScale;
        s.CumulusDensity = m_CloudSettings.Density;
        s.CumulusForwardG = m_CloudSettings.ForwardG;
        s.CumulusThickness = m_CloudSettings.Volumetric ? m_CloudSettings.Thickness : 0.0f;
        s.CloudTypeBias = m_CloudSettings.TypeBias;
        s.CirrusCoverage = m_CloudSettings.CirrusEnabled ? m_CloudSettings.CirrusCoverage : 0.0f;
        s.CirrusAltitude = m_CloudSettings.CirrusAltitude;
        s.CirrusUvScale = m_CloudSettings.CirrusUvScale;
        s.CirrusDensity = m_CloudSettings.CirrusDensity;
        s.CirrusAnisotropy = m_CloudSettings.CirrusAnisotropy;
        s.FogSigma0 = m_FogSettings.Density;
        s.FogScaleHeight = m_FogSettings.ScaleHeight;
        s.FogRefHeight = m_FogSettings.RefHeight;
        // 【usingProceduralSkyを掛けない】FrameConstants側のfogEnabledFlagはそれも見るが、
        // この判定自体が手続き空のときにしか走らない(呼び出し元のifを参照)ので同じ値になる
        s.FogEnabled = (m_FogSettings.Enabled && m_FogSettings.Density > 0.0f) ? 1.0f : 0.0f;
        return s;
    }

    void KurenaiEngine3D::CreateRenderTargets(uint32_t width, uint32_t height)
    {
        if (width == 0 || height == 0)
        {
            return;
        }

        // 中間バッファのフォーマットはm_SystemSettings.Precisionで切り替える(A/B比較用。BufferPrecision参照)。
        // Legacy8bitは「中間バッファをすべてR8G8B8A8_UNorm」にする構成
        const bool legacyPrecision = (m_SystemSettings.Precision == BufferPrecision::Legacy8bit);

        // Albedoは両構成ともリニアのR8G8B8A8_UNormのままにする。
        // sRGB格納(R8G8B8A8_UNorm_SRGB)にすれば符号点が暗部へ寄り、暗いマテリアルの量子化は
        // 細かくなる(リニア反射率L=0.02で約4.3倍)。しかし実測すると最終画像への寄与は
        // 平均0.03/255と測定限界以下である。アルベドの量子化は面ごとの一定オフセットとして出るため、
        // 狙っていた暗部のバンディング(=照明の滑らかな変化が最終8bitで潰れる現象)には
        // そもそも効かない。加えてL>0.244では逆に粗くなり、金属はアルベドバッファの値を
        // F0として使う(DeferredLighting.hlsl)ぶん確実にその領域へ入るため、
        // 利点が確認できないまま欠点だけを抱えることになる。詳細はArchitecture.html 17.4節
        // フォーマットの決定はGetEmissiveFormat/GetAOFormatに一本化している。ここへ直接書くと
        // 同じ値を宣言するPSO側(CreatePrecisionDependentPipelineStates)とずれ、
        // D3D12では仕様違反になる
        const RHI::Format emissiveFormat = GetEmissiveFormat();
        const RHI::Format aoFormat = GetAOFormat();

        try
        {
            m_RenderTargets.CreateGBufferCore(*m_Device, width, height, emissiveFormat);
            m_RenderTargets.CreateLightingChain(*m_Device, width, height, aoFormat);
            // 大気遠近パスの出力。m_RenderTargets.SSRTextureと同じ作法(HDR、R16G16B16A16_Float)で永続確保する
            m_RenderTargets.AerialPerspectiveTexture = m_Device->CreateRenderTexture(width, height, RHI::Format::R16G16B16A16_Float);
            // 雲パスの出力(rgb=事前乗算済みの散乱光、a=透過率)。内部レンダー解像度の1/2で持つ。
            // 【R16G16B16A16_Float固定にする理由】平面反射(CreatePlanarReflectionTargets)と同じで、
            // 散乱光はHDRの輝度をそのまま持つためLegacy8bitでは飽和して雲が白く潰れる。
            // また透過率は乗算に使うので8bitの量子化がそのままバンディングになる
            m_RenderTargets.SkyCloudWidth = std::max(1u, width / 2);
            m_RenderTargets.SkyCloudHeight = std::max(1u, height / 2);
            m_RenderTargets.SkyCloudTexture =
                m_Device->CreateRenderTexture(m_RenderTargets.SkyCloudWidth, m_RenderTargets.SkyCloudHeight, RHI::Format::R16G16B16A16_Float);
            // 上のパスが同時に書く fogInFront(雲に最初に当たった位置の霞の透過率、P18b)。
            // 合成側(DeferredLighting.hlsl)が clearColor * (CloudSkyLight - 1) * (1 - fogInFront) を
            // フル解像度で掛けるためだけに要る。1チャンネルなのでDDGIResolveの低解像度深度と
            // 同じR32_Floatにする。
            // 【t19/t21と同じ理由で常に確保する】雲パスが登録されないフレーム(DDSスカイボックス)でも
            // t22を空のままにできない(DX12のディスクリプタテーブルを埋め切るため)
            m_RenderTargets.SkyCloudFogTexture =
                m_Device->CreateRenderTexture(m_RenderTargets.SkyCloudWidth, m_RenderTargets.SkyCloudHeight, RHI::Format::R32_Float);
            // DDGIの低解像度解決パスの出力(rgb=イラディアンス、a=insideWeight)。雲と同じく1/2解像度。
            // 【常に確保する】m_DDGISettings.HalfResolutionが無効でもシェーダーのt19には何かを
            // バインドしておく必要がある(DX12のディスクリプタテーブルを埋め切るため)。
            // フォーマットを雲と揃えているのも同じ理由 ―― イラディアンスはHDRの物理量で、
            // 8bitでは飽和と量子化がそのまま間接光のバンディングになる
            m_GIResources.DDGIResolveWidth = std::max(1u, width / 2);
            m_GIResources.DDGIResolveHeight = std::max(1u, height / 2);
            m_GIResources.DDGIResolveTexture = m_Device->CreateRenderTexture(
                m_GIResources.DDGIResolveWidth, m_GIResources.DDGIResolveHeight, RHI::Format::R16G16B16A16_Float);
            // 上のパスが同時に書く「そのテクセルが代表している全解像度の深度」(41.24節)。
            // 合成側(DeferredLighting.hlsl)がGatherRed 1回で4テクセルぶんを取るためのもので、
            // t19と同じ理由で常に確保する(t21を空のままにできない)
            m_GIResources.DDGIResolveDepthTexture = m_Device->CreateRenderTexture(
                m_GIResources.DDGIResolveWidth, m_GIResources.DDGIResolveHeight, RHI::Format::R32_Float);
            // RT反射はコンピュートシェーダーがUAVで書くため、レンダーターゲットではなくUAVテクスチャを作る。
            // 非対応環境ではパス自体が実行されないので確保しない
            if (m_RenderCapabilities.RaytracingAvailable)
            {
                m_RenderTargets.CreateRTReflection(*m_Device, width, height);
                m_RenderTargets.CreateRTShadow(*m_Device, width, height);
                // RTAOの生バッファはコンピュートがUAVで書くためUAVテクスチャ、ブラー後は
                // 従来どおりピクセルシェーダーが書くレンダーターゲット。
                // フォーマットはSSAO/SSILと同じaoFormat(バッファ精度の設定に追従する)
                m_RenderTargets.RTAORawTexture = m_Device->CreateUAVTexture(width, height, aoFormat);
                m_RenderTargets.RTAOTexture = m_Device->CreateRenderTexture(width, height, aoFormat);
                // MegaLightsが書くポイント/スポットライトの直接光(HDR)。DirectLighting.hlslが
                // t7で読んで加算する。
                //
                // 【fp16ではなくfp32にしてある】RT反射やSceneColorと同じR16G16B16A16_Floatで
                // 十分に見えるが、このテクスチャは参照実装の出力 ―― 以降の段階すべての
                // 「真値」になる物差しでもある。fp16に落とすと、恒等テスト
                // (影レイ0本で従来のライトループと一致するか)で**片側だけに寄った差**が出た。
                // 実測: 3840x2088のManyLightsTestで、fp16は11661画素が1/255だけ暗い側へずれ、
                // 逆向きは0画素。fp32では差のある画素が13まで減り、符号も両側(9/4)に散った。
                // 物差し自体が系統的に暗い側へ寄っていると、確率的サンプリングの
                // バイアス検査(N枚平均が真値へ寄るか)がそのぶん汚染される。
                // 帯域が問題になったら、参照実装とは別の出力先を用意して測ってから決めること
                m_RenderTargets.CreateMegaLightsOutput(*m_Device, width, height);
            }
            m_RenderTargets.CreateTonemap(*m_Device, width, height);

            m_RenderTargets.CreateGBufferVelocity(*m_Device, width, height);

            m_RenderTargets.CreateGBufferBentNormal(*m_Device, width, height);

            // m_TAAHistoryIndexが今フレームの書き込み先。
            m_RenderTargets.CreateTAAHistory(*m_Device, width, height);

            const uint32_t hiZMipLevels = ComputeMipLevelCount(width, height);
            m_GeometryPasses->SetHiZMipLevels(hiZMipLevels);
            m_RenderTargets.CreateHiZ(*m_Device, width, height, hiZMipLevels);
            m_DebugViewSettings.HiZDebugMipLevel = 0;
            // 作り直した直後の中身は未定義。Hi-Zパスが1回走るまでオクルージョン判定を止める
            m_GeometryPasses->InvalidateHiZ();

            // タイルライトカリングのライトグリッド。タイル数は解像度に依存するためここで作り直す。
            // 端のタイルは部分的にしか埋まらないので切り上げる
            m_RenderTargets.CreateLightTiles(*m_Device, width, height, kLightTileSize, kLightTileStride);

            // MegaLightsの候補プール。タイルの切り方はライトグリッドと同じで、1タイルあたりの
            // 要素数だけが違う。非対応環境ではパス自体が走らないので確保しない
            if (m_RenderCapabilities.RaytracingAvailable)
            {
                m_RenderTargets.CreateMegaLightsTilePool(*m_Device, kMegaLightsTilePoolStride);

                // 1画素につきN本のリザーバ(1本16バイト)。MegaLightsCommon.hlsli の
                // MegaLightsReservoir と**ストライドを一致させること**。
                //
                // 【手法に関わらずクアッドの標本数で確保する】ここで手法を見て 1 と N を
                // 切り替えると、手法を切り替えるたびに確保し直しが要る。常に大きい側で
                // 取っておけば、手法2は先頭の 幅x高さ 本だけを使う形になり無駄なだけで安全。
                // 定数バッファへ渡す値(MegaLightsSamplesPerPixel())は手法3以外で1になるので、
                // **確保 >= 実際に使う本数** が常に成り立つ
                m_MegaLightsAllocatedSamplesPerPixel =
                    std::clamp(m_MegaLightsSettings.QuadSamplesPerPixel, 1, kMegaLightsMaxSamplesPerPixel);
                m_RenderTargets.CreateMegaLightsReservoirs(
                    *m_Device, width, height, static_cast<uint32_t>(m_MegaLightsAllocatedSamplesPerPixel));

                // 履歴の幾何(前フレームの法線・線形深度・材質)。
                // 【なぜ専用に持つのか】G-Bufferは毎フレーム上書きされ、前フレームの写しは
                // どこにも残らない。再投影先が「同じ面か」を判定するには前フレームの幾何が要る。
                // 1画素12バイト(法線oct 4 + View空間Z 4 + 材質 4)。
                // MegaLightsCommon.hlsli の MegaLightsHistoryGuide とストライドを一致させること
                m_RenderTargets.CreateMegaLightsHistoryGuide(*m_Device, width, height);
                // 【履歴を無効にする】解像度が変わると添字の意味が変わり、前フレームの内容は
                // 別の画素のものになる。RHIにバッファのクリアが無いので、初回は
                // シェーダ側で「履歴を使わない」と判断させる
                m_MegaLightsPasses->InvalidateHistory();
                // デノイザの作業用テクスチャ。整数フォーマットが無いRHIなのですべてfloat。
                // 【履歴もping-pongにする】RenderGraphはWARの辺を張らないので、
                // 読む側と書く側が同じだと条件分岐でパスが消えた瞬間に静かに壊れる
                m_RenderTargets.CreateMegaLightsDenoiseWork(*m_Device, width, height);
                m_RenderTargets.CreateMegaLightsDenoised(*m_Device, width, height);
                // 解像度が変わると履歴の添字の意味が変わる。バッファのクリアが無いRHIなので、
                // シェーダ側へ「履歴を読むな」と伝える
                m_MegaLightsPasses->InvalidateDenoiseHistory();
            }

            // MegaLightsの蓄積バッファ(計測専用)。1画素につきfloat4。
            // 非対応環境でも、Presentがt6へ張るための1要素のダミーとして必ず作る
            // (DX12はSetPipelineStateのたびにルート引数が無効化されるため、シェーダが
            // 宣言しているリソースを未バインドのままDrawできない)
            {
                const uint32_t accumElements = m_RenderCapabilities.RaytracingAvailable ? (width * height) : 1u;
                m_RenderTargets.CreateMegaLightsAccum(*m_Device, accumElements);
            }
            // 解像度が変わると添字の意味が変わるので、蓄積も書き出しも必ず取り直す。
            // 【書き出し済みフラグも戻すこと】起動直後は既定解像度から実際のウィンドウサイズへ
            // 切り替わる。戻さないと、切り替わる前の低解像度のまま1回書き出して終わってしまう
            m_MegaLightsPasses->ResetAccumulation();
            m_LightTileOverflowLogged = false;

            // ブルームのピラミッド。第0段が半解像度で、以降1段ごとに半分になる。
            // 1x1まで落とさず段数を固定しているのは、これ以上小さくしても裾の広がりが
            // 見た目に寄与しないため(解像度が低いと逆にアップサンプル時のちらつき源になる)。
            // レベルごとに独立したテクスチャにしている理由はBloom.hlsl冒頭を参照
            m_RenderTargets.CreateBloomPyramid(*m_Device, width, height, kBloomLevelCount);

            // 自前ソフトウェアラスタライザ(46章)の解像度依存リソース。
            //
            // 【内側で捕まえる】ここが落ちてもエンジン全体を止める理由が無い比較用の機能なので、
            // 外側のLegacy8bitフォールバックへ持ち出さず、この機能だけ無効化して続行する
            // (フォールバックしたところでVRAM不足は解決しない。m_RenderTargets.PlanarReflectionColorと同じ判断)
            if (m_RenderCapabilities.SoftwareRasterAvailable)
            {
                try
                {
                    m_GeometryPasses->CreateSoftwareRasterVisibilityBuffer(*m_Device, width, height);

                    m_RenderTargets.CreateSoftwareRasterOutputs(*m_Device, width, height);
                }
                catch (const std::exception& e)
                {
                    Core::Logger::Error(
                        "KurenaiEngine3D",
                        std::string("ソフトウェアラスタライザのリソース作成に失敗したため無効にします (") +
                            std::to_string(width) + "x" + std::to_string(height) + "): " + e.what());
                    m_RenderCapabilities.SoftwareRasterAvailable = false;
                    m_GeometryPasses->ResetSoftwareRasterVisibilityBuffer();
                    m_RenderTargets.ResetSoftwareRasterOutputs();
                }
            }
        }
        catch (const std::exception& e)
        {
            // Legacy8bit構成でも失敗する場合は、このエンジンが前提とする最低限の
            // フォーマット(R8G8B8A8_UNorm等)すら作れていないため復旧手段が無い
            if (legacyPrecision)
            {
                Core::Logger::Error(
                    "KurenaiEngine3D",
                    std::string("レンダーターゲットの作成に失敗しました (") + std::to_string(width) + "x" +
                        std::to_string(height) + ", バッファ精度=Legacy8bit): " + e.what());
                throw;
            }

            // R11G11B10_Float / R16G16B16A16_Float のいずれかが
            // このデバイスでレンダーターゲットとして使えない場合の保険。8bit構成へ落として続行する
            // (画質は落ちるが起動できなくなるよりはよい)
            Core::Logger::Error(
                "KurenaiEngine3D",
                std::string("HDR精度のレンダーターゲット作成に失敗したためLegacy8bit構成へフォールバックします (") +
                    std::to_string(width) + "x" + std::to_string(height) + "): " + e.what());
            m_SystemSettings.Precision = BufferPrecision::Legacy8bit;
            CreateRenderTargets(width, height);
            return;
        }

        // 履歴バッファを作り直した直後は中身が未定義なので、TAAへ「今フレームは履歴を使うな」と伝える。
        // fp16の未初期化領域はNaNのことがあり、lerp(NaN, x, 1.0)もNaNになるため、
        // ブレンド率を0にするだけでは足りず「サンプルそのものを行わない」必要がある(TAA.hlsl参照)
        m_TAAHistoryValid = false;
        m_TAAHistoryIndex = 0;

        // ポインタが作り直されたので、グラフィックスデバッガ向けの名前を焼き直す
        m_DebugNamesDirty = true;

        // A/B比較の記録用。どちらの構成で描かれたスクリーンショットなのかをログから追えるようにする
        Core::Logger::Info(
            "KurenaiEngine3D",
            std::string("レンダーターゲットを作成しました (") + std::to_string(width) + "x" + std::to_string(height) +
                ", バッファ精度=" + (legacyPrecision ? "Legacy8bit" : "HDR") + ")");
    }

    void KurenaiEngine3D::CreatePlanarReflectionTargets()
    {
        if (m_RenderWidth == 0 || m_RenderHeight == 0)
        {
            return;
        }

        // 反射解像度 = レンダー解像度 × 倍率。最低でも1x1は確保する
        // (倍率が非常に小さい・レンダー解像度が非常に小さい場合でもテクスチャ作成自体は失敗させない)
        const uint32_t width = std::max(
            1u, static_cast<uint32_t>(static_cast<float>(m_RenderWidth) * m_ReflectionSettings.PlanarResolutionScale));
        const uint32_t height = std::max(
            1u, static_cast<uint32_t>(static_cast<float>(m_RenderHeight) * m_ReflectionSettings.PlanarResolutionScale));

        try
        {
            m_RenderTargets.CreatePlanarReflection(*m_Device, width, height);
        }
        catch (const std::exception& e)
        {
            Core::Logger::Error(
                "KurenaiEngine3D",
                std::string("平面反射のレンダーターゲット作成に失敗しました (") + std::to_string(width) + "x" +
                    std::to_string(height) + "): " + e.what());
            throw;
        }

        // ポインタが作り直されたので、グラフィックスデバッガ向けの名前を焼き直す
        m_DebugNamesDirty = true;

        Core::Logger::Info(
            "KurenaiEngine3D",
            std::string("平面反射のレンダーターゲットを作成しました (") + std::to_string(width) + "x" +
                std::to_string(height) + ")");
    }

    void KurenaiEngine3D::RequestRenderResolution(uint32_t width, uint32_t height)
    {
        // 上限はHi-Zのミップ構築・ライトタイル・ブルームピラミッドがいずれも
        // D3Dのテクスチャ上限(16384)以内で完結することを保証するための保険。
        // 実際にはそのはるか手前でVRAMが尽きるが、その場合はRender()側が元の解像度へ戻す
        constexpr uint32_t kMaxRenderSize = 16384;
        if (width == 0 || height == 0 || width > kMaxRenderSize || height > kMaxRenderSize)
        {
            Core::Logger::Error(
                "KurenaiEngine3D",
                "RequestRenderResolution: 解像度" + std::to_string(width) + "x" + std::to_string(height) +
                    "が範囲外です(1〜" + std::to_string(kMaxRenderSize) + ")。要求を無視します");
            return;
        }

        if (width == m_RenderWidth && height == m_RenderHeight)
        {
            // 同じ解像度への要求はレンダーターゲットの作り直し(とTAA履歴の破棄)を伴うだけで
            // 何も変わらないため無視する
            return;
        }

        m_PendingRenderWidth = width;
        m_PendingRenderHeight = height;
        m_RenderResolutionDirty = true;
    }

    float KurenaiEngine3D::GetUpscaleRatio(UpscaleQualityMode mode)
    {
        // FSR1が定義している4段。倍率は「出力の一辺 ÷ 入力の一辺」
        switch (mode)
        {
        case UpscaleQualityMode::UltraQuality: return 1.3f;
        case UpscaleQualityMode::Quality:      return 1.5f;
        case UpscaleQualityMode::Balanced:     return 1.7f;
        case UpscaleQualityMode::Performance:  return 2.0f;
        default:
            Core::Logger::Error(
                "KurenaiEngine3D",
                "GetUpscaleRatio: 未知の品質モード(" + std::to_string(static_cast<int>(mode)) +
                    ")です。Quality(1.5倍)として扱います");
            return 1.5f;
        }
    }

    void KurenaiEngine3D::ComputeUpscaleRenderResolution(
        uint32_t outputWidth, uint32_t outputHeight, UpscaleQualityMode mode,
        uint32_t& outRenderWidth, uint32_t& outRenderHeight)
    {
        const float ratio = GetUpscaleRatio(mode);

        // 8の倍数へ切り捨てる。LightCullのタイル・Hi-Zのミップ連鎖・Bloomのピラミッド・
        // SkyCloud/DDGIResolveの1/2解像度がいずれも2の冪で割っていくため、
        // 半端な解像度にすると端の1〜2画素の扱いがパスごとにずれる。
        // 丸めた結果アスペクト比が出力とわずかにずれる(1920x1080の1.7倍で1128x632、
        // 1.7848対1.7778で0.4%)が、EASUは入力矩形を出力矩形へ写すだけなのでこの差は
        // 微小な引き伸ばしとして吸収され、視認できない
        constexpr uint32_t kMinRenderSize = 320;
        constexpr uint32_t kMinRenderHeight = 180;
        const uint32_t rawWidth = static_cast<uint32_t>(static_cast<float>(outputWidth) / ratio);
        const uint32_t rawHeight = static_cast<uint32_t>(static_cast<float>(outputHeight) / ratio);
        outRenderWidth = std::max(kMinRenderSize, rawWidth & ~7u);
        outRenderHeight = std::max(kMinRenderHeight, rawHeight & ~7u);
    }

    float KurenaiEngine3D::ComputeRcasSharpnessScale(float sharpness)
    {
        // FSR1のsharpnessは「シャープさを何ストップ(=半分に)落とすか」で、0が最大・大きいほど弱い。
        // UI側は「0で無効、1で最強」のほうが直感的なので、ここで向きと尺度を変換する。
        // 2ストップ(=1/4)を弱い側の端にしているのは、それ以上落とすと見た目の変化が無くなるため
        const float clamped = std::clamp(sharpness, 0.0f, 1.0f);
        if (clamped <= 0.0f)
        {
            // 完全に0のときはlobeごと0になるようにする(exp2(-2)=0.25では弱いシャープが残る)
            return 0.0f;
        }
        return std::exp2(-2.0f * (1.0f - clamped));
    }

    void KurenaiEngine3D::RequestUpscaleSettings(
        bool enabled, UpscaleQualityMode mode, uint32_t outputWidth, uint32_t outputHeight)
    {
        if (outputWidth == 0 || outputHeight == 0)
        {
            Core::Logger::Error(
                "KurenaiEngine3D",
                "RequestUpscaleSettings: 出力解像度" + std::to_string(outputWidth) + "x" +
                    std::to_string(outputHeight) + "が不正です。要求を無視します");
            return;
        }

        m_PostProcessSettings.UpscaleEnabled = enabled;
        m_PostProcessSettings.UpscaleQuality = mode;
        m_PostProcessSettings.UpscaleOutputWidth = outputWidth;
        m_PostProcessSettings.UpscaleOutputHeight = outputHeight;

        if (enabled)
        {
            uint32_t renderWidth = 0;
            uint32_t renderHeight = 0;
            ComputeUpscaleRenderResolution(outputWidth, outputHeight, mode, renderWidth, renderHeight);
            RequestRenderResolution(renderWidth, renderHeight);
            // 出力解像度用のテクスチャがまだ無い、またはサイズが変わったときだけ作り直す
            if (m_RenderTargets.UpscaleTargetWidth != outputWidth || m_RenderTargets.UpscaleTargetHeight != outputHeight)
            {
                m_UpscaleTargetsDirty = true;
            }
        }
        else
        {
            // 無効化したときは内部解像度を出力解像度と同じに戻す。こうしないと
            // 「超解像を切ったのに低解像度のまま」という状態が残る
            RequestRenderResolution(outputWidth, outputHeight);
            // 使わなくなったテクスチャは解放する(1080pで約8MBが2枚)
            if (m_RenderTargets.UpscaleTargetWidth != 0 || m_RenderTargets.UpscaleTargetHeight != 0)
            {
                m_UpscaleTargetsDirty = true;
            }
        }
    }

    void KurenaiEngine3D::CreateUpscaleTargets(uint32_t width, uint32_t height)
    {
        // 無効化された場合は解放だけして戻る
        if (!m_PostProcessSettings.UpscaleEnabled)
        {
            m_RenderTargets.ResetUpscale();
            return;
        }

        m_RenderTargets.CreateUpscale(*m_Device, width, height);
    }

    bool KurenaiEngine3D::IsUpscaleActive() const
    {
        // テクスチャの確保に失敗している場合にパスを登録すると、バインドするリソースが無いまま
        // Dispatchすることになるため、確保済みであることまで条件に入れる
        return m_PostProcessSettings.UpscaleEnabled && m_RenderTargets.UpscaleTexture && m_RenderTargets.UpscaleSharpTexture &&
               m_RenderTargets.UpscaleTargetWidth > 0 && m_RenderTargets.UpscaleTargetHeight > 0;
    }

    void KurenaiEngine3D::RequestPlanarReflectionResolutionScale(float scale)
    {
        // 0以下はテクスチャが確保できない。上限を1.0(等倍)にしているのは、水面はラフネスが
        // 低くても波の法線で歪むため等倍を超える解像度に意味が無いため(EngineDefaults.h参照)
        if (scale <= 0.0f || scale > 1.0f)
        {
            Core::Logger::Error(
                "KurenaiEngine3D",
                "RequestPlanarReflectionResolutionScale: 倍率" + std::to_string(scale) +
                    "が範囲外です(0より大きく1.0以下)。要求を無視します");
            return;
        }

        if (scale == m_ReflectionSettings.PlanarResolutionScale)
        {
            return;
        }

        // レンダーターゲットの作り直しはGPUがまだ参照しているかもしれない状態では行えないため、
        // RequestRenderResolutionと同じく要求を記録するだけにしてRender()の先頭でまとめて反映する
        m_PendingPlanarReflectionResolutionScale = scale;
        m_PlanarReflectionResolutionDirty = true;
    }

    void KurenaiEngine3D::ResetSceneDependentParams()
    {
        const float sizeY = m_Scene.BoundsMax[1] - m_Scene.BoundsMin[1];
        const float dx = m_Scene.BoundsMax[0] - m_Scene.BoundsMin[0];
        const float dz = m_Scene.BoundsMax[2] - m_Scene.BoundsMin[2];
        const float diagonal = std::sqrt(dx * dx + sizeY * sizeY + dz * dz);

        // SSAO/SSILのサンプリング半径はシーンの規模に応じて変わるべきなので、対角線に比例させる
        // (小さすぎる/大きすぎるシーンでも遮蔽表現が破綻しないよう妥当な範囲にクランプする)
        m_AmbientOcclusionSettings.SSAORadius = std::clamp(diagonal * 0.01f, 0.05f, 2.0f);
        m_AmbientOcclusionSettings.SSILRadius = m_AmbientOcclusionSettings.SSAORadius;
        m_AmbientOcclusionSettings.SSILThickness = m_AmbientOcclusionSettings.SSILRadius * 0.2f;

        // SSRの最大レイ距離もシーンの規模に応じて変わるべきなので、対角線に比例させる。
        // ヒット判定の厚みはSSAO/SSILと同様、遮蔽・接触判定として妥当な小さい値にする
        m_ReflectionSettings.SSRMaxDistance = std::clamp(diagonal * 0.5f, 1.0f, 100.0f);
        m_ReflectionSettings.SSRThickness = m_AmbientOcclusionSettings.SSAORadius * 0.2f;

        // RT反射のレイ距離はSSRより長く取る。SSRは「画面外へ出たら打ち切り」で早々に確信度0へ
        // 落ちるためシーン対角の半分でも足りるが、RTは画面外も追えるので短く切ると
        // 本来映るはずの建物を通り越して空が映ってしまう。シーン対角そのものを上限にする
        m_ReflectionSettings.RTReflectionMaxDistance = std::clamp(diagonal, 1.0f, 500.0f);

        // RTAOのレイ距離はSSAO/SSILの半径より長く取る。スクリーンスペース手法は
        // 半径を伸ばすほど画面上のサンプル間隔が粗くなって破綻するが、RTには
        // その制約が無く、部屋の広さ程度まで伸ばしたほうがバウンス光が正しく回る
        m_AmbientOcclusionSettings.RTAOMaxDistance = std::clamp(diagonal * 0.03f, 0.1f, 10.0f);

        // カメラの移動速度。.ksceneが[Scene]CameraSpeedを持っていればそれを使い、
        // 無ければシーン対角から決める。
        //
        // 【比例と下限の2段】基準はEmeraldSquare(対角344.6m)で従来どおりの5 m/sになる比例式。
        // それより小さいシーンは従来の5 m/sで既に使いやすいので下限で据え置く
        // (比例だけだとSponza(対角37.1m)が0.54 m/sになり、逆に遅くなる)。
        // 根拠と実測はEngineDefaults.hのCameraSpeed一式のコメントに置いてある
        m_SystemSettings.CameraSpeed = m_Scene.HasCameraSpeed
            ? m_Scene.CameraSpeed
            : (std::max)(
                  Defaults::CameraSpeedMin,
                  diagonal / Defaults::CameraSpeedReferenceDiagonal * Defaults::CameraSpeed);

        // 【必ずログに出す】速度は絵に写らないため、「効いていない」と「効いているが
        // 想定と違う値になっている」を見た目では区別できない。シーンごとの実効値を残しておく
        char cameraSpeedText[192];
        std::snprintf(
            cameraSpeedText, sizeof(cameraSpeedText),
            "カメラ移動速度: %.2f m/s (Shift時 %.2f m/s) [シーン対角 %.1f m / %s]",
            m_SystemSettings.CameraSpeed, m_SystemSettings.CameraSpeed * Defaults::CameraSpeedShiftMultiplier, diagonal,
            m_Scene.HasCameraSpeed ? "[Scene]CameraSpeedの指定" : "対角からの自動決定");
        Core::Logger::Info("KurenaiEngine3D", cameraSpeedText);
    }

    // 歩き回る視点のカメラの近平面を求める。シーン対角に比例させつつ、上限で頭打ちにする。
    //
    // 【比例させるだけでは足元が丸ごと消える】diagonal * 0.0005 は「near:far比を一定に保って
    // 深度精度を確保する」という経験則で、深度をNDCへほぼ1/zで写す従来のZバッファを前提にしている。
    // このエンジンはReverse-Z + D32_FLOATで、1/zが近平面側へ寄せる分布と浮動小数点の指数が
    // 0付近で細かくなる性質がちょうど噛み合うため、近平面を小さくしても遠方の精度がほとんど落ちない
    // (Reverse-Zを採る目的がまさにこれ)。一方で近平面が大きいままだと、その距離より手前の
    // ジオメトリはラスタライズ前に丸ごと捨てられる。
    //
    // 実測: 6000m四方の干潟のシーン(対角約8487m)ではこの式が near = 4.24m を返し、水面の
    // 1.45m上に置いたカメラを俯角19.9度より下へ向けると水面が画面から丸ごと消えた
    // (G-Bufferのアルベドも水面マスクも0、つまり「暗く描かれている」のではなく「何も描かれて
    // いない」状態になり、背景として空モデルの下半球の色が見えていた)。
    // 上限は視点の高さ(人の目線で1.6m前後)に対して十分小さい値として0.1mを採る。
    // 対角200m以下のシーンでは元の式が0.1mを下回るため、この上限は効かない(挙動が変わらない)。
    float ComputeWalkableNearZ(float diagonal)
    {
        return std::clamp(diagonal * 0.0005f, 0.01f, 0.1f);
    }

    Core::Camera KurenaiEngine3D::ComputeInitialCamera(const Assets::Scene& scene)
    {
        Core::Camera camera;
        const float sizeY = scene.BoundsMax[1] - scene.BoundsMin[1];
        const float dx = scene.BoundsMax[0] - scene.BoundsMin[0];
        const float dz = scene.BoundsMax[2] - scene.BoundsMin[2];
        const float diagonal = std::sqrt(dx * dx + sizeY * sizeY + dz * dz);

        if (scene.HasCameraOverride)
        {
            camera.SetPosition({ scene.CameraPosition[0], scene.CameraPosition[1], scene.CameraPosition[2] });
            camera.SetYawPitch(scene.CameraYaw, scene.CameraPitch);
            camera.SetLens(DirectX::XM_PIDIV4, ComputeWalkableNearZ(diagonal), std::max(100.0f, diagonal * 4.0f));
            return camera;
        }

        const float centerX = (scene.BoundsMin[0] + scene.BoundsMax[0]) * 0.5f;
        const float centerY = (scene.BoundsMin[1] + scene.BoundsMax[1]) * 0.5f;
        const float centerZ = (scene.BoundsMin[2] + scene.BoundsMax[2]) * 0.5f;
        const float eyeHeight = scene.BoundsMin[1] + sizeY * 0.15f;

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
            posX = scene.BoundsMin[0] + dx * 0.2f;
            posY = eyeHeight;
            posZ = centerZ;
            yaw = DirectX::XM_PIDIV2;
            nearZ = ComputeWalkableNearZ(diagonal);
        }
        else
        {
            posX = centerX;
            posY = eyeHeight;
            posZ = scene.BoundsMin[2] + dz * 0.2f;
            yaw = 0.0f;
            nearZ = ComputeWalkableNearZ(diagonal);
        }

        camera.SetPosition({ posX, posY, posZ });
        camera.SetYawPitch(yaw, 0.0f);
        camera.SetLens(DirectX::XM_PIDIV4, nearZ, farZ);
        return camera;
    }

    // カメラ視錐台をkCascadeCount個の深度範囲に分割する境界(View空間でのカメラからの距離)を求める。
    // 対数分割(遠くのカスケードほど急激に広がる。人間の目の距離知覚・遠近感に合う)と均等分割
    // (どのカスケードも同じ奥行きを持つ)を按分するPractical Split Scheme(GPU Gems 3, Dimitrov 2007)を使う。
    // 対数分割のみだと手前のカスケードが極端に狭くなり、均等分割のみだと遠方のテクセル密度が
    // 不足するため、両者を混ぜることで手前の精度と遠方のカバレッジを両立する
    void KurenaiEngine3D::ComputeCascadeSplits(const Core::Camera& camera, float (&outSplits)[kCascadeCount]) const
    {
        const float nearZ = camera.GetNearZ();
        // [Scene]ShadowDistanceが指定されていれば、そこでカスケードの分割範囲を打ち切る。
        //
        // 【なぜ必要か】遠クリップ面はシーンAABBの対角から自動で決まる(farZ = max(100, 対角×4))。
        // 数十km規模のシーンではfarZが100km級になり、分割範囲がそのまま伸びるため
        // 第1カスケードが数kmを2048x2048の1枚で覆うことになって近景の影が消える。
        // 【未指定なら従来どおり】書かなかったシーンの見え方は1ピクセルも変えない
        const float farZ = m_Scene.HasShadowDistance
            ? (std::min)(camera.GetFarZ(), m_Scene.ShadowDistance)
            : camera.GetFarZ();
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

    float KurenaiEngine3D::GetLastFrameGPUWaitTimeMs() const
    {
        return m_Device->GetLastFrameGPUWaitTimeMs();
    }

    float KurenaiEngine3D::GetMonitorDpiScale() const
    {
        return m_Window->GetDpiScale();
    }

    void KurenaiEngine3D::Run()
    {
        // シーン読み込み専用スレッドを起動する。ファイルI/O・デコード・アセット由来のGPUリソースの
        // 作成と破棄をこのスレッドが担い、読み込み中もRenderスレッドがフレームを進められるようにする
        m_LoaderThread = std::thread(&KurenaiEngine3D::LoaderThreadMain, this);

        // 描画専用スレッドを起動する。以後このスレッドがRender()の呼び出しとPresentを担当し、
        // 呼び出し元スレッド(以下Updateスレッド)はPumpMessages/Updateに専念する
        m_RenderThread = std::thread(&KurenaiEngine3D::RenderThreadMain, this);

        // 注意: ウィンドウのドラッグ中(移動・リサイズ)はWindowsが自前のモーダルループを回すため、
        // このループのPumpMessages()は戻ってこない。その間は1フレームも進まず画面が固まる
        // (ドラッグ中は描画不要という方針のためこのままにしている)。
        // その結果、モニタをまたいだときのUI拡大率の変化はマウスを離した時点でまとめて反映される。
        //
        // HasPendingGraphicsAPIChange()でも抜ける。この場合ウィンドウは閉じられておらず、
        // 呼び出し側がこのオブジェクトを破棄して別のAPIで作り直す(ヘッダのコメント参照)
        while (!m_Window->ShouldClose() && !HasPendingGraphicsAPIChange())
        {
            m_Window->PumpMessages();
            if (m_Window->ShouldClose())
            {
                break;
            }

            TickFrame();
        }

        {
            std::lock_guard<std::mutex> lock(m_FrameStateMutex);
            m_StopRenderThread = true;
        }
        m_FrameStateCV.notify_one();
        m_RenderThread.join();

        // Renderスレッドが止まった後にLoaderスレッドを止める。この順序により、Loaderの停止後に
        // 新しい破棄依頼が積まれることはない。Loaderは終了前に残った破棄依頼を片付けるため、
        // アセット用ディスクリプタヒープを触るのはこのスレッドだけ、という不変条件が保たれる
        {
            std::lock_guard<std::mutex> lock(m_LoadRequestMutex);
            m_StopLoaderThread = true;
        }
        m_LoadRequestCV.notify_one();
        m_LoaderThread.join();

        // Loaderが作り終えていたが取り込まれなかったシーンをここで解放する。
        // この時点で動いているのはこのスレッドだけなので、どのヒープを触っても競合しない
        {
            std::lock_guard<std::mutex> lock(m_LoadedSceneMutex);
            m_LoadedScene.reset();
        }
    }

    void KurenaiEngine3D::RequestGraphicsAPIChange(GraphicsAPI api)
    {
        if (api == m_GraphicsAPI)
        {
            return;
        }

        Core::Logger::Info(
            "KurenaiEngine3D",
            std::string("グラフィックスAPIの切り替えが要求されました: ") +
                (m_GraphicsAPI == GraphicsAPI::DX12 ? "DX12" : "DX11") + " -> " +
                (api == GraphicsAPI::DX12 ? "DX12" : "DX11"));

        m_RequestedGraphicsAPI.store(static_cast<int>(api), std::memory_order_relaxed);
    }

    bool KurenaiEngine3D::HasPendingGraphicsAPIChange() const
    {
        return m_RequestedGraphicsAPI.load(std::memory_order_relaxed) >= 0;
    }

    GraphicsAPI KurenaiEngine3D::GetPendingGraphicsAPI() const
    {
        const int requested = m_RequestedGraphicsAPI.load(std::memory_order_relaxed);
        // 要求が無いときは現在のAPIを返す(呼び出し側がHasPendingGraphicsAPIChangeを
        // 見ずに呼んでも、少なくとも同じAPIで作り直すだけで済むようにする)
        return requested < 0 ? m_GraphicsAPI : static_cast<GraphicsAPI>(requested);
    }

    void KurenaiEngine3D::SetExtraImGuiCallback(std::function<void()> callback)
    {
        // 【Run()の前に呼ぶこと】Renderスレッドが走り出した後にここを書き換えると、
        // 描画中のstd::functionを差し替えることになる。エディタはエンジンを構築した直後、
        // Run()を呼ぶ前に一度だけ登録する
        m_ExtraImGuiCallback = std::move(callback);
    }

    void KurenaiEngine3D::ApplyDroneShowData(const Assets::ShowData& data)
    {
        // 呼び出しスレッドの前提はヘッダー側のコメントを参照(SetExtraImGuiCallbackで
        // 登録したコールバックの中から呼ぶこと)。
        // 時刻は戻さない ―― プレビュー中に点をいじるたびにショーが先頭へ飛ぶと、
        // 「いま見ている瞬間の形」を直せなくなるため
        m_DroneShow.SetData(data);
    }

    void KurenaiEngine3D::TickFrame()
    {
        const auto now = std::chrono::steady_clock::now();
        const float realDeltaTime = std::chrono::duration<float>(now - m_LastFrameTime).count();
        m_LastFrameTime = now;

        // 同じフレーム番号でも実時間が異なると、アニメーションが進んで描画結果を比較できない。
        const float deltaTime = m_FixedTimeStep > 0.0f ? m_FixedTimeStep : realDeltaTime;
        Update(deltaTime);

        // m_CameraはUpdateスレッド(UpdateMouseLook/UpdateMovement/UpdateAppliedSceneHandoff)
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
            const float realRenderDeltaTime = std::chrono::duration<float>(now - m_LastRenderFrameTime).count();
            m_LastRenderFrameTime = now;
            const float renderDeltaTime = m_FixedTimeStep > 0.0f ? m_FixedTimeStep : realRenderDeltaTime;
            // 自動露出の時間方向の順応で使う(次フレームのRender()が読む)
            m_RenderDeltaTime = renderDeltaTime;

            // 昼夜サイクルの自動進行はUpdateスレッドではなくこちら(Renderスレッド)で行う。
            // m_SkySettings.TimeOfDay/m_SkySettings.TimeAutoAdvance/m_SkySettings.TimeAdvanceSpeedはImGuiパネル(RenderLightingUI、
            // Renderスレッドから描画)でも書き換えられるため、両方をRenderスレッド専有にすることで
            // 追加の排他制御なしに済ませられる
            if (m_SkySettings.TimeAutoAdvance)
            {
                m_SkySettings.TimeOfDay = std::fmod(m_SkySettings.TimeOfDay + m_SkySettings.TimeAdvanceSpeed * renderDeltaTime, 24.0f);
                if (m_SkySettings.TimeOfDay < 0.0f)
                {
                    m_SkySettings.TimeOfDay += 24.0f;
                }
            }

            // 水面のスクロール位相。太陽の自動進行とまったく同じ場所・同じ理由
            // (m_SkySettings.TimeAutoAdvance/m_WaterSettings.TimeFrozenがRenderingパネル(Renderスレッドから描画)でも
            // 書き換えられるため、両方をRenderスレッド専有にすることで追加の排他制御なしに済ませる)
            if (!m_WaterSettings.TimeFrozen)
            {
                m_WaterScrollOffset = std::fmod(m_WaterScrollOffset + renderDeltaTime * m_WaterSettings.WaveSpeed, 1.0f);
            }

            // 雲のスクロール位相。水面とまったく同じ場所・同じ理由でRenderスレッド専有のまま進める。
            // 【風速の単位について】m_CloudSettings.WindSpeedは実世界の速度[m/s]として持つ(UIで直感的に
            // 扱えるようにするため)。Sky.hlsliのノイズ空間はワールド距離にCloudUvScaleを掛けた
            // ものなので、ノイズ空間上の移動量へ換算するにはここでCloudUvScaleを掛ける必要がある。
            // 【なぜベイクをdirtyにしないのか】風のスクロールはIBLキューブの明るさに一切影響しない
            // (判断A: キューブには雲を焼かない)。ここでm_SkyBakeDirtyを立てると、風が吹くたびに
            // 毎フレーム空生成6回+プリフィルタ36回のディスパッチが走ってしまい、判断Aの利点が
            // 丸ごと消える。被覆率のような「キューブの明るさに効く」パラメータだけがdirtyを立てる
            // (RenderingPanel::DrawCloudSection参照)
            if (!m_CloudSettings.TimeFrozen)
            {
                const float windRadians = DirectX::XMConvertToRadians(m_CloudSettings.WindDirectionDegrees);
                const float windDirX = std::cos(windRadians);
                const float windDirZ = std::sin(windRadians);
                const float advanceNoiseSpace = m_CloudSettings.WindSpeed * m_CloudSettings.UvScale * renderDeltaTime;
                // Sky.hlsliのkCloudNoisePeriodと同じ値でwrapする(このファイル冒頭近くの
                // kCloudNoisePeriod定数のコメント参照)
                m_CloudScrollOffset.x =
                    std::fmod(m_CloudScrollOffset.x + windDirX * advanceNoiseSpace, kCloudNoisePeriod);
                m_CloudScrollOffset.y =
                    std::fmod(m_CloudScrollOffset.y + windDirZ * advanceNoiseSpace, kCloudNoisePeriod);

                // 巻雲。積雲とまったく同じ形(kCloudNoisePeriodでstd::fmod)で進める。
                // 風向はm_CloudSettings.WindDirectionDegreesを積雲と共有し、速度・UVスケールだけ
                // 巻雲側の値(m_CloudSettings.CirrusWindSpeed/m_CloudSettings.CirrusUvScale)を使う。凍結トグル
                // (m_CloudSettings.TimeFrozen)も積雲と共有する——片方にしか効かないとA/B比較で
                // スクロールが揺れる側だけ残ってしまい対照が取れなくなるため
                const float cirrusAdvanceNoiseSpace = m_CloudSettings.CirrusWindSpeed * m_CloudSettings.CirrusUvScale * renderDeltaTime;
                m_CirrusScrollOffset.x =
                    std::fmod(m_CirrusScrollOffset.x + windDirX * cirrusAdvanceNoiseSpace, kCloudNoisePeriod);
                m_CirrusScrollOffset.y =
                    std::fmod(m_CirrusScrollOffset.y + windDirZ * cirrusAdvanceNoiseSpace, kCloudNoisePeriod);
            }

            // ドローンショーの進行時刻。水面・雲のスクロール位相とまったく同じ場所・同じ理由で
            // Renderスレッド専有のまま進める。
            //
            // 【1巡ぶんで必ず折り返すこと】DroneShow::Evaluate自身も1巡の周期でstd::fmodするので
            // 絵の上は折り返さなくても正しく出る。折り返しが要るのは**floatの精度**のためである。
            // 仮数は24bitなので、1日(86,400秒)積むとULPが約0.010秒になり、60fpsのdt(0.0167秒)が
            // まともに積めなくなってショーが止まる。以前はUIの「ショー時刻」スライダーで
            // 手動で戻せることを逃げ道にしていたが、そのUIごと無くなったのでここで閉じる
            m_DroneShowTime += renderDeltaTime * m_DroneShow.Data().Speed;
            const float showLoopDuration = m_DroneShow.LoopDuration();
            if (showLoopDuration > 0.0f)
            {
                // 未初期化(LoopDuration()==0)のときに割るとNaNになるのでガードする
                m_DroneShowTime = std::fmod(m_DroneShowTime, showLoopDuration);
            }

            // m_Scene・ポストプロセスのパラメータ・UIの状態はすべてこのRenderスレッド専有に
            // なっているため、ミューテックスによる保護は要らない
            // (経緯はdocs/ImplementationHistory.md 23章)
            const auto cpuStart = std::chrono::steady_clock::now();
            try
            {
                Render(frameState);
            }
            catch (const std::exception& e)
            {
                // この時点でRenderスレッドを終えると、次のTickFrameがフレーム受け渡し待ちのまま
                // 停止する。例外は記録して次フレームを試み、Run側の通常終了処理で停止させる。
                Core::Logger::Error("KurenaiEngine3D", std::string("Render中に例外が発生しました: ") + e.what());
            }
            catch (...)
            {
                // 例外の型が不明でもスレッド関数から抜けるとstd::terminateになるため、必ず記録して継続する。
                Core::Logger::Error("KurenaiEngine3D", "Render中に不明な例外が発生しました");
            }
            const auto cpuEnd = std::chrono::steady_clock::now();
            // GPUの完了待ち(DX12のフレームパイプライン化に伴うフェンス待ち)は実際のCPU負荷ではなく
            // GPU側の処理時間の反映なので差し引く(DX11は常に0が返るため影響しない)
            const float rawCPUTimeMs = std::chrono::duration<float, std::milli>(cpuEnd - cpuStart).count();
            m_RenderStats.CPUFrameTimeMs = std::max(0.0f, rawCPUTimeMs - m_Device->GetLastFrameGPUWaitTimeMs());

            // 固定FPSモード: このフレームの処理(Time of Day更新+Render+Present)が目標フレーム時間
            // より短く終わった場合、余った時間だけ待機して間隔を揃える。CPU/GPU計測(上記)の後に
            // 行うことで、この待機時間自体がプロファイラの計測値に混ざらないようにしている
            if (m_SystemSettings.FixedFPSEnabled && m_SystemSettings.TargetFPS > 0.0f)
            {
                const auto targetFrameDuration = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(1.0 / m_SystemSettings.TargetFPS));
                const auto frameDeadline = now + targetFrameDuration;
                if (std::chrono::steady_clock::now() < frameDeadline)
                {
                    std::this_thread::sleep_until(frameDeadline);
                }
            }

            // FPSは指数移動平均で平滑化する(生の1/deltaTimeだとフレームごとの揺れが大きく読み取りにくいため)
            if (realRenderDeltaTime > 0.0f)
            {
                const float instantFPS = 1.0f / realRenderDeltaTime;
                m_RenderStats.FPS = (m_RenderStats.FPS == 0.0f) ? instantFPS : (m_RenderStats.FPS * 0.9f + instantFPS * 0.1f);
            }

            LogFrameStatsIfDue(realRenderDeltaTime);
        }

        if (SUCCEEDED(comResult))
        {
            CoUninitialize();
        }
    }

    void KurenaiEngine3D::UpdateMouseLook(bool imguiWantsMouse)
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
                // ImGuiパネルの上で右ボタンを押し始めた場合は視点回転を開始しない
                // (ウィジェットの右クリックメニューと衝突するため)。
                // 一度キャプチャに入った後はカーソルが画面中央へ固定され続けてImGui側の判定が
                // 変わりうるため、この判定は開始時にだけ行う
                if (imguiWantsMouse)
                {
                    return;
                }

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
        //
        // 【速度は即値ではなくシーンから決まる】m_SystemSettings.CameraSpeedは.ksceneの[Scene]CameraSpeed、
        // 無ければシーン対角から自動で決まる(ResetSceneDependentParams)。Shiftの倍率は
        // 従来の 20/5 = 4倍をそのまま保つ
        const float moveSpeed =
            m_SystemSettings.CameraSpeed * (IsKeyDown(VK_SHIFT) ? Defaults::CameraSpeedShiftMultiplier : 1.0f);
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

    void KurenaiEngine3D::UpdateAppliedSceneHandoff()
    {
        // ロックを取る前にatomicで有無を判定する(publishされるのはシーン切り替え時だけなので、
        // ほとんどのフレームはここで抜ける)
        if (!m_AppliedScenePending.load(std::memory_order_acquire))
        {
            return;
        }

        Core::Camera camera;
        std::wstring title;
        bool applyCamera = true;
        {
            std::lock_guard<std::mutex> lock(m_AppliedSceneMutex);
            camera = m_AppliedSceneCamera;
            title = m_AppliedSceneTitle;
            applyCamera = m_AppliedSceneApplyCamera;
        }
        m_AppliedScenePending.store(false, std::memory_order_relaxed);

        // m_Cameraの書き込み手はこのUpdateスレッド1つに保つ(Renderスレッドは触らない)。
        // ホットリロードで「現在のカメラを保持する」が入っているときはここを飛ばす。
        // このとき位置・向きだけでなくnear/far(ComputeInitialCameraがシーンのAABBから決める)も
        // 前のまま残る。同じ.ksceneを読み直す用途では[Model]が変わらない限りAABBも変わらないので
        // 実害は無いが、モデルを差し替えたときはこのトグルを外して読み直すこと
        if (applyCamera)
        {
            m_Camera = camera;
        }
        // ウィンドウタイトルの変更もウィンドウを所有するこのスレッドから行う
        m_Window->SetTitle(title);
    }

    void KurenaiEngine3D::Update(float deltaTime)
    {
        // ImGui(Renderスレッド)が入力を掴んでいるかを読む。Renderは1フレーム遅れて描くため
        // この値も1フレーム遅れるが、WantCaptureKeyboardはInputTextがアクティブな間ずっと
        // trueであり続けるため、実用上ずれるのは押し始めの1フレームだけ
        const bool imguiWantsKeyboard = m_ImGuiWantCaptureKeyboard.load(std::memory_order_relaxed);
        const bool imguiWantsMouse = m_ImGuiWantCaptureMouse.load(std::memory_order_relaxed);

        // 内部レンダー解像度が変わったときのアスペクト比の反映。m_CameraはこのUpdateスレッドしか
        // 書けないため、解像度を変えるRenderスレッドはm_RenderAspectへ置くだけにしてある
        // (m_RenderAspectの宣言のコメント参照)。同じ値なら再設定しても副作用は無いので毎フレーム呼ぶ
        m_Camera.SetAspectRatio(m_RenderAspect.load(std::memory_order_relaxed));

        UpdateMouseLook(imguiWantsMouse);

        // ライト名のInputTextを編集中にWASDがカメラ移動として解釈されるのを防ぐ
        if (!imguiWantsKeyboard)
        {
            UpdateMovement(deltaTime);
        }


        // F1(ImGuiパネルの表示/非表示)はWantCaptureKeyboardに関係なく常に効かせる。
        // ここも抑止すると、テキスト入力中にパネルを畳んで戻す手段が無くなり、入力欄から
        // フォーカスを外す方法(Esc / 別の場所をクリック)を知らないと詰むため。
        // ImGuiのInputTextはF1を消費しないので、通しても入力内容には影響しない
        UpdateImGuiToggle();
        // 新しいシーンが反映されていれば、その初期カメラとウィンドウタイトルをここで取り込む
        UpdateAppliedSceneHandoff();
        // 昼夜サイクルの自動進行(m_SkySettings.TimeOfDay)はRenderThreadMain側で行う(RenderThreadMain参照)
    }

    void KurenaiEngine3D::Render(const FrameState& frameState)
    {
        // --- フレームの先頭: 作り直しと、グラフを組む前の更新(段階6.6のAブロック) ---
        // 【この並びを動かさないこと】どれも「このフレームのGPUコマンドをまだ1つも
        // 積んでいない」ことを前提にしており、作り直しの契機はこの先頭に集めてある
        ResetFrameCounters();
        ApplyPendingRecreations();

        if (m_Window->GetWidth() == 0 || m_Window->GetHeight() == 0)
        {
            return;
        }

        BeginImGuiFrame(frameState);
        RecreateDirtyRenderTargets();

        // このフレームのGPUコマンドをまだ1つも積んでおらず、UpdateSceneStreamingとバッファ精度・
        // 解像度の作り直しの両方より後なので、そこで生じた破棄をすべて拾える。どのパスもまだ
        // バインドしていないため、上書きまで維持する前提のバインドをフレーム途中で失わせない。
        m_Device->ApplyPendingResourceInvalidation();
        auto* commandList = m_Device->GetImmediateCommandList();
        m_GPUProfiler->BeginFrame();
        m_CPUProfiler.BeginFrame();

        // 太陽・月・空の状態を求める(すべて絶対的な測光量[lx]。露出はまだ掛かっていない)
        const SunLighting sunLighting = ComputeSunLighting(
            m_SkySettings.TimeOfDay, m_SkySettings.SunAzimuthDegrees, m_SkySettings.MoonAzimuthDegrees, m_SkySettings.MoonElevationDegrees);

        UpdateEffectiveExposure(sunLighting.KeyIlluminanceLux);
        ApplyMegaLightsPerturbationIfDue();

        const float effectiveExposure = ComputeExposure(m_EffectiveExposureEV100);

        // 手動露出時にTonemap/Bloomが掛ける倍率。
        // HDRバッファには「実効EV100」でプリ露出された値が入っているが、ユーザーが見たいのは
        // 「設定EV100で撮った絵」なので、その差分を割り戻す。
        // 実効EV100は夜に最大18段下がる(=バッファ上の値が26万倍明るくなる)ため、
        // ここを1.0に固定していると夜が昼と同じ明るさで出てしまい、
        // 自動露出をオフにしても露出が時刻に追従し続ける状態になる
        const float manualExposureScale = std::exp2(m_EffectiveExposureEV100 - m_PostProcessSettings.SceneExposureEV100);

        // 自動露出の測光値を上側で止めるための、構図に依存しない基準EV。
        // キー照度は画面に何が写っていようと変わらないので、
        // 「空が画面のどれだけを占めるか」で露出が振れるのを抑えられる
        const float keyReferenceEV100 = ComputeReferenceEV100(sunLighting.KeyIlluminanceLux);

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

        UpdateSceneForFrame(commandList, cameraPosition, frameState.Camera);

        // --- フレームの値の組み立て(段階6.6のB+Cブロック) ---
        // 【graph.Execute()が終わるまで生かすこと】パスのExecuteラムダはこれらより長生きする。
        // BuildFrameContextのローカルにすると、graph.Execute()の時点で解放済みのメモリを指す。
        // 解放直後なら中身が残っていて同じ絵が出るため、10構成の採取を何回回しても捕まらない
        Rendering::RenderFrameContext frameContext{};
        std::vector<GPULight> gpuLights;
        FrameConstants constants;
        Passes::LightingConstants lightingConstants{};
        // ライトのうちベイク済み(手動+発光プロキシ)の数。Dブロックが登録前に読む
        size_t bakedLightCount = 0;
        BuildFrameContext(
            frameState, commandList, sunLighting, effectiveExposure, manualExposureScale, keyReferenceEV100,
            cameraPosition, cascadeSplits, cascadeViewProj, frameContext, gpuLights, constants,
            lightingConstants, bakedLightCount);

        Rendering::RenderBlackboard blackboard{};

        Core::RenderGraph graph(commandList, m_GPUProfiler.get(), &m_CPUProfiler);

        // 【frameContext.ProbeCaptureReads が指す先はここに置く】RegisterPasses の
        // ローカルにすると graph.Execute() の時点で解放済みのメモリを指す。
        // 直後なら中身が残っていて同じ絵が出るため、採取では捕まらない
        std::vector<RHI::IRHITexture*> probeCaptureReads;

        // 13回の Register を1つにまとめた。**呼び出し順は実行順の一部**なので、
        // 中の並びを1つも入れ替えないこと(RenderGraph は依存が同点のとき最小登録番号を選ぶ)
        RegisterPasses(
            graph, frameContext, blackboard, commandList, probeCaptureReads, bakedLightCount,
            frameContext.SkyTexture, cascadeViewProj, frameState.Camera);

        WritePassManifestIfDue(graph);

        graph.Execute();

        // --- グラフの実行より後: 読み戻しとフレームの締め(段階6.6のEブロック) ---
        // 【この6つは呼ぶ順が実行順の一部】読み戻し(Presentより前でなければ2フレーム前の値が
        // 読めない) → ImGui/Present → 前フレーム状態の確定 → 計測の書き出し →
        // ダンプの回収 → 履歴の反転。切り出す前の並びをそのまま保っている
        ResolveFrameCullStats(blackboard, frameContext.MeshletCullStatsActive);
        SubmitAndPresentFrame();
        AdvanceFramePrevViewState(constants, frameContext.JitterUv);
        AccumulatePerfDump();

        // --- 中間レンダーターゲットの生値ダンプ(検証専用) ---
        // 【perfdumpと同じく毎フレーム走る場所へ置く】積んだコピーを数フレーム後に読む仕組みなので、
        // ここが毎フレーム呼ばれないと待ちフレームがいつまでも進まない
        ResolveTextureDumps();

        AdvanceFrameHistory();
    }

    // 【依存していた型と定数は Rendering/ へ出した】以前ここには「無名名前空間の
    // kMaxLights / kMaxDrones / kTAAJitterSampleCount / RadicalInverse / MakeGPULight /
    // ComputeCloudAverageTransmittance / GPUReflectionProbe に依存しているので
    // この翻訳単位から移せない」と書いてあった。段階7.5でそれらを
    // SampleSequence.h / GPULightBuild.h / GPUReflectionProbe.h / CloudTransmittance.h /
    // DroneShowResources.h へ移したので、この関数を割って外へ出せるようになっている
    void KurenaiEngine3D::BuildFrameContext(
        const KurenaiEngine3D::FrameState& frameState, RHI::IRHICommandList* commandList,
        const SunLighting& sunLighting, float effectiveExposure, float manualExposureScale,
        float keyReferenceEV100, const DirectX::XMFLOAT3& cameraPosition,
        const float (&cascadeSplits)[kCascadeCount],
        const DirectX::XMMATRIX (&cascadeViewProj)[kCascadeCount],
        Rendering::RenderFrameContext& frameContext, std::vector<GPULight>& gpuLights,
        ShaderInterop::FrameConstants& constants, Passes::LightingConstants& lightingConstants,
        size_t& bakedLightCount)
    {
        DecideFrameJitterAndCamera(frameState, frameContext);

        UpdateMeshletLODFrame(frameState);
        EvaluateDroneShowFrame();

        BuildGpuLightList(gpuLights, cameraPosition, bakedLightCount);

        // タイルライトカリングの1タイルあたりの容量超過の可能性を早めに知らせる。
        // 実際に超過したかどうかはGPU側にしか無く(バッファのリードバック経路がRHIに無い)、
        // ここで分かるのは「シーンのライト数が容量を超えているので、1つのタイルに集中すれば
        // 超過し得る」という条件までである。実際の超過はデバッグ表示(DebugView::LightTiles)の
        // マゼンタで確認する
        // 【条件はパスを積む述語と揃える】グリッドを作っていないフレームで警告すると、
        // 実際には起きない欠落を知らせることになる(MegaLightsが走っていればローカルライトは
        // グリッドを経由しない)
        if (ShouldRunLightCulling() && gpuLights.size() > kLightTileCapacity && !m_LightTileOverflowLogged)
        {
            Core::Logger::Warning(
                "KurenaiEngine3D",
                "有効ライト数(" + std::to_string(gpuLights.size()) + ")がタイルの容量(" +
                    std::to_string(kLightTileCapacity) +
                    ")を超えています。1タイルへ集中した場合そのタイルではライトが欠落します"
                    "(Render TargetsのLight Tiles表示でマゼンタのタイルとして確認できます)");
            m_LightTileOverflowLogged = true;
        }

        // このフレームで空として使うキューブマップ。手続き空(SkyGenerate)か.ksceneのDDSかが
        // ここで確定する。**RenderGraphのReads宣言と実際のバインドの両方でこのローカルを使うこと**
        // (ActiveSkyTexture()を都度呼ぶと両者が食い違って依存解決が壊れる)。
        // 【ここで確定させる理由】この下のFrameConstants(constants.SkyParams.y)が
        // usingProceduralSkyを必要とするため、FrameConstantsを埋めるより前に確定させる
        RHI::IRHITexture* const skyTexture = ActiveSkyTexture();
        const bool usingProceduralSky = (skyTexture == m_SkyResources.ProceduralSkyTexture.get());

        // 太陽が閾値以上動いていたら手続き空を焼き直す。毎フレーム焼くと
        // 空生成6回+プリフィルタ36回のディスパッチが常時走って無駄になる。
        // 空はプリ露出済みの値で焼かれるため、実効プリ露出が動いたときも焼き直す必要がある
        // (焼き直さないと空だけ古い露出のまま取り残される)
        if (usingProceduralSky && !m_SkyBakeDirty)
        {
            const DirectX::XMVECTOR current = DirectX::XMLoadFloat3(&sunLighting.SunPosition);
            const DirectX::XMVECTOR baked = DirectX::XMLoadFloat3(&m_LastBakedSunPosition);
            const float cosAngle = DirectX::XMVectorGetX(DirectX::XMVector3Dot(current, baked));
            const bool sunMoved =
                cosAngle < std::cos(DirectX::XMConvertToRadians(m_SkySettings.BakeAngleThresholdDegrees));
            // 露出が0.05段(約3.5%)以上動いたら焼き直す。時刻変化に伴う露出の追従でも
            // 動くため、太陽の角度閾値とあわせて実質的に連続した更新になる
            const bool exposureMoved =
                std::abs(m_EffectiveExposureEV100 - m_LastBakedExposureEV100) > 0.05f;
            // タービディティが動いたら焼き直す。PreethamのxyYモデルの形自体が変わるため、
            // exposureMovedと同じ形の判定をここへ追加する
            const bool turbidityMoved = std::abs(m_SkySettings.Turbidity - m_LastBakedTurbidity) > 0.01f;
            // 空の彩度(アート指定)もPreethamの色度を動かすため、タービディティと同じ扱いで焼き直す
            const bool saturationMoved = std::abs(m_SkySettings.Saturation - m_LastBakedSkySaturation) > 0.005f;
            // 雲のパラメータが動いたら焼き直す(P18)。
            //
            // 【なぜ要るか】ここまでの4つは晴天の空の形を決める値だけで、雲は「晴天の空を
            // 変えない」ため入っていなかった。P18でSkyIntegrateが雲込みの空の照度を積むように
            // なったので、被覆率を動かしても焼き直しが走らないと**古い被覆率で積んだ
            // CloudSkyLightが残り続ける**。実際これで被覆率0でも比が1にならず、雲を持たない
            // シーンの遠景が動いた(切り分け: SkyIntegrateへ1を直書きした絵と、消費側で1へ
            // 潰した絵は画素まで一致した=経路は正しく、値だけが古かった)。
            //
            // 【風のスクロールを入れない】スクロール量は毎フレーム動くので、入れると毎フレーム
            // 焼き直しになる。求めているのは半球平均なので、雲の場が平行移動しても値はほとんど
            // 変わらない。同じ理由でカメラ位置も入れない。
            //
            // 【IBLの雲減光もこれで直る】m_ActiveCloudTransmittance(キューブへ焼く平均透過率)も
            // このブロックの中でしか更新されないため、被覆率を動かしても環境光が追従しない
            // という同じ形の取りこぼしがあった
            const CloudBakeSignature cloudSignature = MakeCloudBakeSignature();
            const bool cloudChanged = !m_HasBakedCloudSignature
                                      || cloudSignature != m_LastBakedCloudSignature;
            if (sunMoved || exposureMoved || turbidityMoved || saturationMoved || cloudChanged)
            {
                m_SkyBakeDirty = true;
            }
        }

        // このフレームで手続き空を焼くかどうか。下のSkyGenerateパス登録とキャッシュ更新の
        // 両方をこのフラグで判定する
        const bool bakeSkyThisFrame = usingProceduralSky && m_SkyBakeDirty;

        // このフレームでSkyIntegrateパス(m_SkyResources.ParametersBufferへ書く)を実行するかどうか。
        // 通常はbakeSkyThisFrameと同じタイミングだが、m_SkyResources.ParametersBufferが一度も書かれていない
        // 場合はusingProceduralSkyがfalse(.ksceneのDDSスカイボックス使用時)でも1回だけ実行する。
        //
        // 【なぜCPU側からのゼロ初期化ではなくこの形にしたのか】DX12のStructuredRWバッファは
        // UAV/SRVでのGPUアクセス専用にDEFAULTヒープへ作成されており、CPUから書き込むための
        // マップ済みポインタ・ステージングリングを一切持たない(m_SkyResources.ParametersBuffer作成箇所の
        // コメント参照)。そのためUpdateBufferでのゼロ埋めはDX12でクラッシュする。GPU側のパスを
        // 1回だけ走らせれば、DX11/DX12のどちらでも安全に(積分結果自体は使われないが)未初期化状態を
        // 解消できる。太陽方向・目標照度はusingProceduralSkyに関わらず既に計算済みのsunLightingを
        // そのまま使えるため、余分な分岐を増やさずに済む
        const bool skyIntegrateThisFrame = bakeSkyThisFrame || !m_EnvironmentPasses->IsSkyParametersBufferInitialized();

        // --- 空パラメータ(tintと天頂輝度)の確定はGPU側(SkyIntegrate.hlsl)で行う ---
        // 【なぜベイクと同じタイミングか】背景の解析評価(DeferredLighting.hlsl)は、下のFrameConstants
        // (SkySunDirection)とm_SkyResources.ParametersBuffer(SkyIntegrate.hlslの出力)を組み合わせて使う。
        // ベイクと同じタイミングでSkyIntegrateパスを実行することで、背景とキューブマップ
        // (IBL・反射)が常に同一の空パラメータを見る。毎フレーム走らせると、太陽の角度閾値で
        // ベイクを間引いている間だけ背景とIBLの空がずれてしまう。実際のディスパッチとcbuffer更新は
        // 下のSkyIntegrateパス登録側で行うため、ここではフラグ更新のみ済ませる
        if (bakeSkyThisFrame)
        {
            // 雲(判断B)による平均透過率をベイクと同じタイミングで確定させ、メンバへキャッシュする。
            // **この値はm_SkyResources.ParametersBuffer側の天頂輝度には掛けない**——キューブへ焼く
            // Passes::SkyBakeConstants::CloudTransmittance(下のSkyGenerateパス参照)にだけ掛ける。
            // SkyParametersBufferの天頂輝度を減光すると、雲の隙間から見える青空まで暗くなり、
            // Sky.hlsli側のSkyColorがそこへさらに雲を重ねることで二重に暗くなってしまう
            m_ActiveCloudTransmittance = ComputeCloudAverageTransmittance(
                m_CloudSettings.Enabled, m_CloudSettings.Coverage, m_CloudSettings.CirrusEnabled, m_CloudSettings.CirrusCoverage);

            // P18: この焼き直しがどの雲パラメータで行われたかを覚えておく。
            // 上の焼き直し判定(cloudChanged)がこれと比べる
            m_LastBakedCloudSignature = MakeCloudBakeSignature();
            m_HasBakedCloudSignature = true;

            // 空が変わったのでプリフィルタ済み鏡面も焼き直す必要がある。
            // 焼き直し要否のフラグ更新はここ(キャッシュを書いた場所)に一本化し、
            // 下のSkyGenerateパス登録側では行わない(二重更新・更新漏れを避けるため)
            m_SkyBakeDirty = false;
            m_LastBakedSunPosition = sunLighting.SunPosition;
            m_LastBakedExposureEV100 = m_EffectiveExposureEV100;
            m_LastBakedTurbidity = m_SkySettings.Turbidity;
            m_LastBakedSkySaturation = m_SkySettings.Saturation;
            m_EnvironmentPasses->GetIBLBaked() = false;
            m_EnvironmentPasses->GetIBLIrradianceBaked() = false;
        }

        // 平面反射: 水面インスタンスを探し、その高さ(ワールドY)を水面の平面とする。
        // 水面メッシュはローカルY=0の水平な板(Tools/generate_water_plane.py参照)なので、
        // ワールド変換の平行移動Y(instance.World._24。転置済みのため列に入っている。
        // Transparentパスの距離ソートと同じ規約)がそのまま水面の高さになる。
        // 複数の水面インスタンスが異なる高さで見つかった場合は最初のものだけを使い、警告を1度だけ出す
        // (「水面は単一の水平な平面である」という前提を明示する)
        bool hasWaterInstance = false;
        float waterPlaneY = 0.0f;
        for (const auto& instance : m_Scene.Instances)
        {
            if (!instance.IsWater)
            {
                continue;
            }
            const float instanceWaterY = instance.World._24;
            if (!hasWaterInstance)
            {
                hasWaterInstance = true;
                waterPlaneY = instanceWaterY;
            }
            else if (std::abs(instanceWaterY - waterPlaneY) > 0.01f && !m_PlanarReflectionMultipleWaterLogged)
            {
                Core::Logger::Warning(
                    "KurenaiEngine3D",
                    "複数の水面インスタンスが異なる高さ(Y=" + std::to_string(waterPlaneY) + "とY=" +
                        std::to_string(instanceWaterY) +
                        ")で見つかりました。平面反射は最初の水面のみを使用します"
                        "(水面は単一の水平な平面である前提のため)");
                m_PlanarReflectionMultipleWaterLogged = true;
            }
        }
        // このフレームで平面反射パスを実行するか。
        // 【反射の手法がSSRのときだけ実行する】このパスの出力(m_RenderTargets.PlanarReflectionColor)を読むのは
        // SSR.hlslだけである。手法がRaytracedやOffのときに走らせても、不透明メッシュ全体を
        // もう1回フォワードで描いた結果を誰も読まないまま捨てることになる
        // (DXR対応環境ではDefaultReflectionModeがRaytracedを返すため、この条件が無いと
        //  DX12では常に丸ごと無駄になる。実測でもDX12起動時に水面へ映っていたのはRT反射の結果で、
        //  平面反射パスの出力ではなかった)
        const bool planarReflectionPassRuns =
            m_ReflectionSettings.PlanarEnabled && hasWaterInstance && m_ReflectionSettings.Mode == ReflectionMode::ScreenSpace;

        // 大気遠近パスを実行するか。UIで無効化されているか、密度が0以下(効果が無い)なら
        // パス自体を登録しない(GetActiveReflectionOutput()の結果がそのままTAA/Tonemapへ渡る)。
        // 手続き空が無効なシーンかどうかの判断(FogParams0.w)はパスの実行有無とは別に、
        // 下のconstants.FogParams0組み立て時にusingProceduralSkyを見て決める
        // (SSRパスのwaterAnalyticSkyFlagと同じ、パスの実行可否とシェーダー内の有効フラグを分ける設計)
        const bool fogPassRuns = m_FogSettings.Enabled && m_FogSettings.Density > 0.0f;

        // メッシュレット(増幅シェーダー + メッシュシェーダー)経路でG-Bufferを描くか。
        // メッシュシェーダー非対応のデバイスではPSOが作られないためnullptrになる。
        //
        // 【他の「PassRuns」と並べてここに置く理由】この値はG-Bufferパスの登録時だけでなく、
        // その手前で書き上げるFrameConstantsも見る(オクルージョンカリングの有効フラグ)。
        // 定数バッファの更新はパス登録より前に一度だけ行うため、判断もそこより前で確定させる
        const bool meshletPathActive =
            m_GeometrySettings.MeshletRenderingEnabled && m_GeometryPasses->HasMeshletPipelineState();

        // 増幅シェーダーのHi-Zオクルージョンカリング(Stage 5-2)をこのフレームで行うか。
        // 判定を書いてあるのは増幅シェーダーだけなので、メッシュレット経路に乗らないフレームでは
        // 1つも間引けず、Hi-Zを構築する意味も無い(下のHi-Zパスの登録条件がこれを見る)
        const bool occlusionCullingActive = m_GeometrySettings.OcclusionCullingEnabled && meshletPathActive;

        // メッシュレットカリングの統計をこのフレームで数えるか。
        // 増幅シェーダーが走らなければ数える相手がいない
        const bool meshletCullStatsActive =
            m_GeometrySettings.MeshletCullStatsEnabled && meshletPathActive
            && m_GeometryPasses->HasMeshletCullStatsBuffer();

        // 【実体はRender()にある】frameContext.Constantsがこれを指す。元と同じく未初期化のまま
        // 受け取り、以降の代入で全フィールドを埋める
        const DirectX::XMMATRIX viewProj = frameContext.ViewMatrix * frameContext.JitteredProj;
        DirectX::XMStoreFloat4x4(&constants.ViewProj, DirectX::XMMatrixTranspose(viewProj));

        // 平面反射用の鏡映カメラ。水面平面 y=waterPlaneY に対する反射行列を、通常のView×Projへ
        // 左から掛ける(PlanarReflection.hlsl冒頭参照)。XMMatrixReflectが受け取る平面の規約は
        // 「点PがAx+By+Cz+D=0を満たす」形(ドキュメント準拠)で、これは
        // FrameConstants.PlanarReflectionPlaneのSV_ClipDistance計算(dot(worldPos, xyz) + w)と
        // 完全に同じ規約なので、同じベクトル(0,1,0,-waterPlaneY)がどちらにもそのまま使える
        // (水面より上のworldPosでdot結果が正になることも、この式から導ける)。
        // 水面が無いシーンでもwaterPlaneY=0で計算はできるが、パスを登録しないため使われない
        const DirectX::XMMATRIX reflectMatrix =
            DirectX::XMMatrixReflect(DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, -waterPlaneY));
        // メインカメラと同じジッター済みProjを使う(PlanarReflection.hlsl冒頭参照。ジッターが
        // 異なると反射がメインの画面UVとサブピクセル単位でずれてしまう)
        const DirectX::XMMATRIX reflectedViewProj = reflectMatrix * frameContext.ViewMatrix * frameContext.JitteredProj;
        DirectX::XMVECTOR determinant;
        const DirectX::XMMATRIX invViewProj = DirectX::XMMatrixInverse(&determinant, viewProj);
        DirectX::XMStoreFloat4x4(&constants.InvViewProj, DirectX::XMMatrixTranspose(invViewProj));
        for (uint32_t cascade = 0; cascade < kCascadeCount; ++cascade)
        {
            DirectX::XMStoreFloat4x4(&constants.CascadeViewProj[cascade], DirectX::XMMatrixTranspose(cascadeViewProj[cascade]));
        }
        // 【DDGIのクリップマップの追従中心をここで固定する】このあと組み立てるFrameConstantsの
        // 各LODの原点も、後段のプローブのキャプチャ位置も、すべてこの値を基準に決まる。
        // 1フレームの途中で動かすと「シェーダーが見ている格子」と「実際に焼いた位置」が
        // 食い違い、間接光が別の場所のものになる
        m_DDGIFollowCenter = DirectX::XMFLOAT3{ cameraPosition.x, cameraPosition.y, cameraPosition.z };

        constants.CameraPosition = { cameraPosition.x, cameraPosition.y, cameraPosition.z, 0.0f };
        constants.LightDirection = { sunLighting.Direction.x, sunLighting.Direction.y, sunLighting.Direction.z, 0.0f };
        // 太陽を無効にする場合は色をゼロにするだけでよい(シェーダー側は太陽の寄与に
        // LightColor.rgbを乗算するため、これで完全に消える)。TimeOfDayを夜にする方法と違い
        // 昼度(AmbientColor.a)は下がらないので、環境光だけで照らす状態を作れる
        // sunLighting.Color は絶対的な測光量[lx]なので、ここで実効プリ露出を掛けて表示レンジへ移す
        constants.LightColor = m_SkySettings.SunEnabled
            ? DirectX::XMFLOAT4{
                  sunLighting.Color.x * effectiveExposure,
                  sunLighting.Color.y * effectiveExposure,
                  sunLighting.Color.z * effectiveExposure,
                  0.0f }
            : DirectX::XMFLOAT4{ 0.0f, 0.0f, 0.0f, 0.0f };
        DirectX::XMStoreFloat4x4(&constants.View, DirectX::XMMatrixTranspose(frameContext.ViewMatrix));
        // ジッター済みの射影行列を渡す。SSAO/SSILはこの行列でView空間の点を画面へ投影して
        // 深度バッファと突き合わせるため、深度を描いたときと同じ行列でなければサブピクセルぶんずれる
        DirectX::XMStoreFloat4x4(&constants.Proj, DirectX::XMMatrixTranspose(frameContext.JitteredProj));
        // rgb(環境光の色)にm_IBLSettings.AmbientScaleを乗算する。Enable IBL無効時のフォールバックアンビエント
        // (DeferredLighting.hlsl)の強度調整用で、alpha(dayFactor、IBLの夜間減光・背景スカイの
        // 昼夜ブレンドに使う)には掛けない
        constants.AmbientColor =
        {
            sunLighting.Ambient.x * m_IBLSettings.AmbientScale * effectiveExposure,
            sunLighting.Ambient.y * m_IBLSettings.AmbientScale * effectiveExposure,
            sunLighting.Ambient.z * m_IBLSettings.AmbientScale * effectiveExposure,
            sunLighting.Ambient.w,
        };
        constants.CascadeSplits = { cascadeSplits[0], cascadeSplits[1], cascadeSplits[2], cascadeSplits[3] };
        const float iblIntensity = m_IBLSettings.Enabled ? m_IBLSettings.Intensity : 0.0f;
        const float specularEnergyCompensation = static_cast<float>(m_ReflectionSettings.SpecularCompensation);
        constants.ShadowParams = {
            m_ShadowSettings.LightSize,
            static_cast<float>(kIBLPrefilterMipLevels - 1),
            iblIntensity,
            specularEnergyCompensation,
        };
        constants.ActiveLightCount = { static_cast<float>(gpuLights.size()), 0.0f, 0.0f, 0.0f };
        constants.IBLParams = {
            m_IBLSettings.UseDedicatedIrradiance ? 1.0f : 0.0f,
            m_IBLSettings.AmbientDiffuseScale,
            m_IBLSettings.AmbientSpecularScale,
            0.0f,
        };
        constants.OcclusionParams = {
            m_AmbientOcclusionSettings.BentNormalAOSource ? 1.0f : 0.0f,
            static_cast<float>(m_AmbientOcclusionSettings.SpecularOcclusion),
            m_AmbientOcclusionSettings.MultiBounceAOEnabled ? 1.0f : 0.0f,
            0.0f };

        // 空の解析評価用。DeferredLighting.hlslが背景画素でSky.hlsliのSkyColorを画面解像度で
        // 評価するために使う。ティントと天頂輝度はm_SkyResources.ParametersBuffer(直近の手続き空ベイクで
        // SkyIntegrate.hlslが書いた値。上のbakeSkyThisFrameブロック参照)にあり、DeferredLighting.hlsl/
        // SSR.hlslがStructuredBufferとして直接読むため、ここでFrameConstantsへは詰めない。
        // SunDirectionはここで毎フレーム最新のsunLightingから渡す
        // (太陽は角度閾値以下でも連続的に動くため。天頂輝度・色味と違い積分を伴わず、
        // 毎フレーム渡してもコストが無い)。
        // 正規化はSkyGenerate.hlsl側の慣習(呼び出し側=シェーダのSkyParameters組み立て時に
        // normalizeする)に合わせ、C++側では正規化しない(DeferredLighting.hlsl側で行う)
        constants.SkySunDirection = {
            sunLighting.SunPosition.x, sunLighting.SunPosition.y, sunLighting.SunPosition.z, 0.0f
        };
        // 太陽照度と空照度の比。Sky.hlsliのEvaluateCloudLayerが雲の明るさの基準を
        // 「空の天頂輝度」から「太陽の照度」へ切り替えるために使う(雲を照らしているのは
        // 空ではなく太陽であるため。詳細はSky.hlsli側のkCumulusSingleScatterScale等のコメント参照)。
        // SkyIlluminanceLuxが0近傍(理論上は起こらないが)のときのゼロ除算を避けてある
        const float sunToSkyIlluminanceRatio =
            (sunLighting.SkyIlluminanceLux > 1e-6f)
                ? (sunLighting.KeyIlluminanceLux / sunLighting.SkyIlluminanceLux)
                : 0.0f;
        constants.SkyParams = {
            // x=未使用(天頂輝度はSkyParametersBufferにある)
            0.0f,
            // 手続き空が無効(.ksceneのDDSスカイボックス使用時)は、この設定に関わらず
            // 常にキューブマップを使う。DDSは任意の絵でPerezモデルとは無関係なため、
            // 解析評価してはいけない
            (m_SkySettings.AnalyticBackground && usingProceduralSky) ? 1.0f : 0.0f,
            // z=太陽照度/空照度比(SunToSkyIlluminanceRatio、雲の明るさの基準に使う)
            sunToSkyIlluminanceRatio,
            0.0f,
        };

        // === 実効プリ露出が大きく動いたら、更新モードに関わらずプローブを焼き直す(19.14節) ===
        // 下のProbeParams2.wは「焼いた時点の露出→現在の露出」の換算倍率で、これだけでも
        // プローブの値の解釈は常に正しくなる。ただし換算はあくまで**焼いた時点の環境**を
        // 正しい明るさで見せるだけなので、昼に焼いたプローブを夜の場面へ持ち込めば
        // 「夜の部屋に昼の環境が正しい明るさで映り込む」ことになり、換算前より派手に破綻する
        // (実測: ProbeTestを夜にしたときの平均輝度が213.6→253.9、白飽和78%)。
        //
        // 実効プリ露出が大きく動くのは時刻が大きく動いたときなので、そのときは環境そのものが
        // 古くなっている。Bakedモードが凍結すると宣言しているのはライトやマテリアルの編集に
        // 対してであって、場面全体の明るさが2倍以上変わってもなお昼の映り込みを保持することでは
        // ない。手続き空が同じ理由で焼き直しているのと揃える(閾値は空の0.05段よりずっと粗く
        // 取ってある。フルベイクはプローブ数×6面の描画になるため)。
        // Realtimeは毎フレーム焼き直しているので対象外
        if (m_ReflectionProbeSettings.UpdateMode != ProbeUpdateMode::Realtime && m_ReflectionProbePasses->GetProbeBaked() &&
            !m_GIResources.ReflectionProbes.empty() &&
            std::abs(m_EffectiveExposureEV100 - m_ReflectionProbePasses->GetProbeBakedExposureEV100()) > kProbeRebakeExposureEV)
        {
            m_ReflectionProbePasses->GetProbeBakeRequested() = true;
            // このフレームの後半で今の露出で焼かれるため、換算倍率もここで合わせておく。
            // ここで合わせないと、焼き直したフレームだけ1フレーム古い倍率が掛かって明滅する
            m_ReflectionProbePasses->GetProbeBakedExposureEV100() = m_EffectiveExposureEV100;
        }

        // 反射プローブの影響範囲をt13のStructuredBufferへ渡す。まだ一度も焼けていない場合
        // (まだ焼けていない)や機能を無効にしている場合はプローブ数を0にして、シェーダー側の
        // 選択ループ自体を回さない=中身が未定義のキューブマップを引かせないようにする
        std::vector<GPUReflectionProbe> gpuProbes;
        if (m_ReflectionProbeSettings.Enabled && m_ReflectionProbePasses->GetProbeBaked())
        {
            gpuProbes.reserve(m_GIResources.ReflectionProbes.size());
            for (const Assets::ReflectionProbe& probe : m_GIResources.ReflectionProbes)
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
            commandList->UpdateBuffer(m_GIResources.ProbeBuffer.get(), gpuProbes.data(), sizeof(GPUReflectionProbe) * gpuProbes.size());
        }

        const float probeInfluenceDebug = (m_DebugViewSettings.View == DebugView::ProbeInfluence) ? 1.0f : 0.0f;
        constants.ProbeParams = {
            static_cast<float>(gpuProbes.size()),
            probeInfluenceDebug,
            m_ReflectionProbeSettings.ParallaxCorrectionEnabled ? 1.0f : 0.0f,
            m_ReflectionProbeSettings.BlendingEnabled ? 1.0f : 0.0f,
        };
        constants.ProbeParams2 = {
            m_ReflectionProbeSettings.DepthParallaxEnabled ? 1.0f : 0.0f,
            m_ReflectionProbeSettings.OcclusionEnabled ? 1.0f : 0.0f,
            static_cast<float>(kProbeCaptureSize),
            // 焼いた時点の実効プリ露出から現在の実効プリ露出への換算倍率
            // (ReflectionProbePasses::m_ProbeBakedExposureEV100のコメント参照)。ComputeExposure(ev)=1/(1.2*2^ev)
            // なので、比は 2^(焼いたEV - 現在のEV) になる。
            // フルベイクが走るフレームだけは1フレームぶん古い倍率になるが、それが問題になるのは
            // 「焼き直しと大きな露出変化が同じフレームで起きる」ときだけで、シーン読み込み時は
            // 上のm_EffectiveExposureInitialized=falseで露出が既に確定しているため起きない
            std::exp2(m_ReflectionProbePasses->GetProbeBakedExposureEV100() - m_EffectiveExposureEV100),
        };

        // モーションベクター用の前フレーム情報。初回フレームは前フレームの行列が未定義なので、
        // 今フレームと同じものを入れて速度を0にしておく。そうしないとゴミの速度が速度バッファへ
        // 焼き込まれ、画面全体が一度だけゴーストする
        if (m_TAAPrevViewProjValid)
        {
            constants.PrevViewProj = m_TAAPrevViewProj;
            constants.TAAParams = { frameContext.JitterUv.x, frameContext.JitterUv.y, m_TAAPrevJitterUv.x, m_TAAPrevJitterUv.y };
        }
        else
        {
            constants.PrevViewProj = constants.ViewProj;
            constants.TAAParams = { frameContext.JitterUv.x, frameContext.JitterUv.y, frameContext.JitterUv.x, frameContext.JitterUv.y };
        }

        // DDGI(22章)。一度も焼けていない間はアトラスの中身が未定義なので無効にしておく
        // (反射プローブの「一度でも焼けたか」と同じ方針)
        const bool ddgiActive = m_DDGISettings.Enabled && m_GIResources.HasGIVolume && m_DDGIPasses->IsBaked();
        constants.DDGIParams0 = {
            m_GIResources.GIVolume.Origin[0], m_GIResources.GIVolume.Origin[1], m_GIResources.GIVolume.Origin[2],
            ddgiActive ? 1.0f : 0.0f,
        };
        constants.DDGIParams1 = {
            m_GIResources.GIVolume.ProbeSpacing[0], m_GIResources.GIVolume.ProbeSpacing[1], m_GIResources.GIVolume.ProbeSpacing[2],
            m_GIResources.GIVolume.NormalBias,
        };
        constants.DDGIParams2 = {
            static_cast<float>(m_GIResources.GIVolume.ProbeCounts[0]),
            static_cast<float>(m_GIResources.GIVolume.ProbeCounts[1]),
            static_cast<float>(m_GIResources.GIVolume.ProbeCounts[2]),
            m_GIResources.GIVolume.ViewBias,
        };
        constants.DDGIParams3 = {
            static_cast<float>(kDDGIIrradianceTexels),
            static_cast<float>(kDDGIDistanceTexels),
            m_DDGISettings.Intensity,
            static_cast<float>(kDDGIProbeBorder),
        };
        // y = DeferredLightingがDDGIを低解像度パス(DDGIResolve)から引くか。
        // 【パスが実際に走る条件と一致させること】走らないのに1を渡すと、前フレームの
        // (あるいは未初期化の)低解像度バッファを読んで間接光が固まる/壊れる。
        // 条件はDDGIResolveパスの登録側(ddgiResolvePassRuns)と同じものを並べている
        const bool ddgiHalfResolutionActive =
            m_DDGISettings.HalfResolution && m_GIResources.DDGIResolveTexture && m_DDGISettings.Enabled && m_GIResources.HasGIVolume && m_DDGIPasses->IsBaked();
        // プローブ分類のしきい値。裏面の情報を持てるのはレイトレース経路だけなので、
        // ラスタ経路では分類そのものを無効(0)にして従来どおりの挙動に保つ
        // (ラスタ経路のαは常に0なのでどのしきい値でも有効側に倒れるが、
        //  「分類は掛かっていない」ことを値として明示しておく)
        const float ddgiBackfaceThreshold =
            (m_DDGISettings.ProbeClassificationEnabled && ShouldRunRaytracedDDGITrace()) ? m_DDGISettings.BackfaceThreshold : 0.0f;
        constants.DDGIParams4 = {
            effectiveExposure, ddgiHalfResolutionActive ? 1.0f : 0.0f,
            static_cast<float>(m_DDGILODCount), ddgiBackfaceThreshold
        };

        // クリップマップLODの各段の原点と、トロイダルaddressingの基準になる格子座標。
        // 使わない段も0で埋めておく(未初期化のまま渡すと、段数を増やした瞬間に
        // ゴミを読んで見当違いの場所からプローブを引く)
        static_assert(
            ShaderInterop::kDDGILODCount == kDDGIMaxLODCount,
            "FrameConstantsのDDGILOD配列の要素数とkDDGIMaxLODCountを一致させること"
            "(ずれるとcbufferのレイアウトが静かに食い違う)");
        for (uint32_t lod = 0; lod < kDDGIMaxLODCount; ++lod)
        {
            if (m_GIResources.HasGIVolume && lod < m_DDGILODCount)
            {
                const DirectX::XMFLOAT3 lodOrigin = ComputeDDGILODOrigin(lod);
                const DirectX::XMINT3 lodBase = ComputeDDGILODBaseIndex(lod);
                constants.DDGILODOrigin[lod] = { lodOrigin.x, lodOrigin.y, lodOrigin.z, 0.0f };
                constants.DDGILODBase[lod] = {
                    static_cast<float>(lodBase.x), static_cast<float>(lodBase.y),
                    static_cast<float>(lodBase.z), 0.0f
                };
            }
            else
            {
                constants.DDGILODOrigin[lod] = { 0.0f, 0.0f, 0.0f, 0.0f };
                constants.DDGILODBase[lod] = { 0.0f, 0.0f, 0.0f, 0.0f };
            }
        }
        // 水面。スクロール位相はRenderThreadMainがm_WaterSettings.TimeFrozen/m_WaterSettings.WaveSpeedに
        // 応じて毎フレーム進める(m_SkySettings.TimeOfDayの自動進行と同じ場所・同じ方式)。
        // y=波のスケール倍率(m_WaterSettings.WaveScale)、z=波の強さ(m_WaterSettings.WaveStrength、0〜1)を
        // Water.hlslへ渡す(UIのスライダーが見た目へ反映されるようにするため)
        constants.TimeParams = { m_WaterScrollOffset, m_WaterSettings.WaveScale, m_WaterSettings.WaveStrength, 0.0f };

        // 雲。DeferredLighting.hlsl(背景)とSSR.hlsl(水面反射)の両方が同じ値を読むため、
        // ここで一度だけ組み立てる。m_CloudSettings.Enabled=falseのときはCloudParams0.xへ0を渡し、
        // Sky.hlsli側のSkyColorが早期脱出する経路(判断C)を通す
        constants.CloudParams0 = {
            m_CloudSettings.Enabled ? m_CloudSettings.Coverage : 0.0f,
            m_CloudSettings.Altitude,
            m_CloudSettings.UvScale,
            m_CloudSettings.Density,
        };
        // wには積雲の厚み[m]を詰めてある(FrameConstantsを増やさずに済ませるため)。
        // 0ならシェーダー側はレイマーチせず平面として扱う
        constants.CloudParams1 = {
            m_CloudScrollOffset.x, m_CloudScrollOffset.y, m_CloudSettings.ForwardG,
            m_CloudSettings.Volumetric ? m_CloudSettings.Thickness : 0.0f,
        };
        // 巻雲。積雲と同じ理由でここで一度だけ組み立てる。m_CloudSettings.CirrusEnabled=falseのときは
        // CloudParams2.xへ0を渡し、Sky.hlsli側のSkyColorが早期脱出する経路(判断C)を通す
        constants.CloudParams2 = {
            m_CloudSettings.CirrusEnabled ? m_CloudSettings.CirrusCoverage : 0.0f,
            m_CloudSettings.CirrusAltitude,
            m_CloudSettings.CirrusUvScale,
            m_CloudSettings.CirrusDensity,
        };
        constants.CloudParams3 = { m_CirrusScrollOffset.x, m_CirrusScrollOffset.y, m_CloudSettings.CirrusAnisotropy, m_CloudSettings.TypeBias };
        // 平面反射(P6)。このフィールドを参照するのはPlanarReflection.hlslだけで、そちらは
        // 専用のm_PlanarReflectionConstantBufferで明示的に上書きした値を使う
        // (Passes/ReflectionPassesが持つ)。共有のm_FrameConstantBufferにも一貫した値を入れておく
        constants.PlanarReflectionPlane = { 0.0f, 1.0f, 0.0f, hasWaterInstance ? -waterPlaneY : 0.0f };

        // 大気遠近。AerialPerspective.hlsl/PlanarReflection.hlslの両方が読む。
        // 手続き空が無効(.ksceneのDDSスカイボックス使用時)は、m_FogSettings.Enabledの値に関わらず
        // 常に無効化する――DDSは任意の絵でPerezモデルとは無関係なため、in-scatter項の
        // 解析評価(SkyColor)をしてはいけない(SSRパスのwaterAnalyticSkyFlagと同じ判断)
        const float fogEnabledFlag = (m_FogSettings.Enabled && m_FogSettings.Density > 0.0f && usingProceduralSky) ? 1.0f : 0.0f;
        constants.FogParams0 = { m_FogSettings.Density, m_FogSettings.ScaleHeight, m_FogSettings.RefHeight, fogEnabledFlag };
        constants.FogParams1 = { m_FogSettings.MaxOpacity, 0.0f, 0.0f, 0.0f };
        // 水中項。Water.hlslのPSMainが読む
        constants.WaterBodyColor = { m_WaterSettings.BodyColor.x, m_WaterSettings.BodyColor.y, m_WaterSettings.BodyColor.z, 0.0f };

        // 星空。
        // 【昼は強度0にしてしまう】星は太陽が地平線下にあるときしか見えない。ここで0に
        // 落としておけば、Sky.hlsli側は最初のif文で抜けるので昼のシーンの絵は1画素も動かない
        // (m_StarsSettings.Enabledを切ったときとまったく同じ経路を通る)。
        // sunLighting.SunPositionは太陽が「ある」向きなので、yが負なら地平線下。
        // 仰角0度から-8度にかけて滑らかに立ち上げ、市民薄明のあいだに星が出そろう形にする
        const float sunElevationSin = sunLighting.SunPosition.y;
        const float starsNightFactor = std::clamp((-sunElevationSin - 0.005f) * 8.0f, 0.0f, 1.0f);
        // 手続き空を使わないシーン(DDSスカイボックス指定)ではSkyColorの解析評価自体を
        // 通らないため、フォグの有効フラグと同じ判断で0にしておく
        const float starsIntensity =
            (m_StarsSettings.Enabled && usingProceduralSky) ? (m_StarsSettings.Brightness * starsNightFactor) : 0.0f;
        // 1画素が張る角度[rad]。射影行列の_22 = 1/tan(fovY/2) から
        // 画面の高さ全体が 2*tan(fovY/2) なので、1画素あたりはそれを縦解像度で割ればよい。
        // 解像度やFOVを変えても星の見かけの下限が追従する
        DirectX::XMFLOAT4X4 projForPixelAngle;
        DirectX::XMStoreFloat4x4(&projForPixelAngle, frameContext.JitteredProj);
        const float pixelAngle =
            (projForPixelAngle._22 > 0.0f && m_RenderHeight > 0)
                ? (2.0f / (projForPixelAngle._22 * static_cast<float>(m_RenderHeight)))
                : 0.001f;
        constants.StarsParams = { starsIntensity, m_StarsSettings.Density, m_StarsSettings.Twinkle, pixelAngle };

        // 積雲のボリュームレイマーチの段数。シェーダー側でも上限へ丸めるが、
        // 0以下を渡すと「コンパイル時の既定を使う」の意味になってしまうため下限はここで効かせる
        constants.CloudQualityParams = {
            static_cast<float>(std::clamp(m_CloudSettings.RaymarchSteps, 1u, kCloudRaymarchStepsMax)),
            0.0f, 0.0f, 0.0f
        };

        // --- Hi-Zオクルージョンカリング(Stage 5-2)の判定パラメータ ---
        //
        // 判定に使うHi-Zは前フレームのもの(構築パスがG-Bufferパスより後に登録されるため)。
        // したがって「前フレームのHi-Zが実際に作られている」ことと「前フレームのビュー射影行列が
        // 本物である」ことの両方が要る。どちらかが欠けたフレームでは判定を丸ごと止める ――
        // 初回フレームや解像度変更の直後にここを通すと、未定義の深度で視界内をまとめて消す
        const bool occlusionCullEnabledThisFrame =
            occlusionCullingActive && m_GeometryPasses->IsHiZValid() && m_TAAPrevViewProjValid;

        // 深度プリパスが走るなら、その深度からHi-Zを作れる。**そのフレームのG-Bufferは
        // 前フレームのHi-Zを待たなくてよい** ―― 上の2条件はどちらも要らなくなる。
        // 条件の意味と、プリパス自身が今フレームのHi-Zを使えない理由は、
        // 下の hiZFromDepthPrepass を定義している箇所のコメントにある
        const bool depthPrepassRuns = m_GeometrySettings.DepthPrepassEnabled
            && m_GeometryPasses->CanRunDepthPrepass();
        const bool hiZFromDepthPrepass =
            m_GeometrySettings.HiZFromDepthPrepassEnabled && occlusionCullingActive && depthPrepassRuns;

        // 前フレームからのカメラ移動距離。シーンが静的である以上、1フレームぶんの視差ずれの
        // 原因はカメラの移動だけなので、その距離をバウンディング球の半径へ足せば
        // 保守側(間引きすぎない側)へ倒せる。前フレームが無いフレームでは0でよい
        // (そのフレームは上のフラグで判定自体が止まっている)
        float cameraMoveDistance = 0.0f;
        if (m_TAAPrevViewProjValid)
        {
            const float dx = cameraPosition.x - m_PrevCameraPosition.x;
            const float dy = cameraPosition.y - m_PrevCameraPosition.y;
            const float dz = cameraPosition.z - m_PrevCameraPosition.z;
            cameraMoveDistance = std::sqrt(dx * dx + dy * dy + dz * dz);
        }

        // 【フレーム全体の「判定するか」はここ、「どのHi-Zで判定するか」はドローごと】
        // 深度プリパスから作る経路では、前フレームのHi-Zが無いフレームでも
        // G-Bufferは今フレームのHi-Zで判定できる。どちらの経路も無いときだけ全体を止める
        // (ドローごとの選択は ObjectConstants::MeshletOcclusionMode)
        constants.OcclusionCullParams = {
            (occlusionCullEnabledThisFrame || hiZFromDepthPrepass) ? 1.0f : 0.0f,
            m_GeometrySettings.OcclusionCullRadiusScale,
            cameraMoveDistance,
            static_cast<float>(m_GeometryPasses->GetHiZMipLevels()),
        };
        // Hi-Zのミップ0はG-Buffer深度と同じ解像度で作られる(CreateRenderTargets)
        constants.HiZScreenParams = {
            static_cast<float>(m_RenderWidth),
            static_cast<float>(m_RenderHeight),
            1.0f / static_cast<float>(std::max(1u, m_RenderWidth)),
            1.0f / static_cast<float>(std::max(1u, m_RenderHeight)),
        };

        // bindless番号をfloatで運ぶ。番号はkBindlessDescriptorCapacity(8192)未満で、
        // float32が誤差なく表せる整数の範囲(2^24)に十分収まる
        constants.MeshletCullStatsParams = {
            meshletCullStatsActive ? 1.0f : 0.0f,
            meshletCullStatsActive ? static_cast<float>(m_MeshletCullStatsBindlessIndex) : 0.0f,
            0.0f,
            0.0f,
        };

        commandList->UpdateBuffer(m_FrameConstantBuffer.get(), &constants, sizeof(constants));

        // スクリーンスペースシャドウ(ScreenSpaceShadow.hlsli)が深度値からView空間Zを1除算で
        // 復元するための定数。Camera::GetProjectionMatrixの射影行列(行ベクトル規約)は
        // clip.z = viewZ * a + b、clip.w = viewZ なので depth = a + b / viewZ となり、
        // 逆に解いて viewZ = b / (depth - a)。近平面・遠平面から直接組み立てず射影行列の要素を
        // 読むのは、Reverse-Zの組み方が変わっても自動的に追従させるため
        // (XMFLOAT4X4の_rcは1始まりの行・列なので、_33が行2列2=a、_43が行3列2=b)。
        // ジッター済みの行列から読むが、TAAのジッターが書き換えるのは_31/_32だけなので
        // _33/_43の値そのものはジッターの有無で変わらない。それでもジッター済みを使うのは、
        // 「深度バッファに関わる計算はすべて深度を描いたときと同じ行列から導く」という
        // 不変条件を1箇所も破らないため(将来ジッターの入れ方を変えたときに黙ってずれない)
        DirectX::XMFLOAT4X4 projectionForDepthLinearize;
        DirectX::XMStoreFloat4x4(&projectionForDepthLinearize, frameContext.JitteredProj);
        const float depthLinearizeA = projectionForDepthLinearize._33;
        const float depthLinearizeB = projectionForDepthLinearize._43;

        // 直接光パスのb1へ渡すスクリーンスペースシャドウのパラメータ。パスのラムダから
        // 値キャプチャできるようここで組み立てておく
        // 太陽の影の手法。RTシャドウを選んでいてもパスを実行できない状況(高速化構造が無い等)では
        // カスケードシャドウマップへ落とす。シャドウマップは手法によらず描いてあるため、
        // 落ちても影が消えることはない
        const ShadowMode effectiveShadowMode =
            (m_ShadowSettings.Mode == ShadowMode::Raytraced && !ShouldRunRaytracedShadow())
                ? ShadowMode::CascadedShadowMap
                : m_ShadowSettings.Mode;

        // 【実体はRender()にある】frameContext.Lightingがこれを指す
        lightingConstants.LightCount =
        {
            static_cast<uint32_t>(gpuLights.size()),
            static_cast<uint32_t>(std::max(0, m_ShadowSettings.ScreenSpaceMaxLightsPerPixel)),
            static_cast<uint32_t>(effectiveShadowMode),
            // MegaLightsが走るフレームは、ポイント/スポットの寄与をあちらが計算済みなので
            // 直接光パス側のライトループを止める。**「パスを積むか」と同じ述語で決めること** ――
            // ずれると二重加算(2倍明るい)か、ローカルライトが全部消えるかのどちらかになる
            ShouldRunMegaLights() ? 1u : 0u,
        };
        lightingConstants.SSSParams0 =
        {
            static_cast<float>(m_ShadowSettings.ScreenSpaceStepCount),
            m_ShadowSettings.ScreenSpaceMaxRayLength,
            m_ShadowSettings.ScreenSpaceThickness,
            m_ShadowSettings.ScreenSpaceEnabled ? 1.0f : 0.0f,
        };
        lightingConstants.SSSParams1 =
        {
            depthLinearizeA,
            depthLinearizeB,
            m_ShadowSettings.ScreenSpaceNormalBias,
            m_ShadowSettings.ScreenSpaceEdgeFade,
        };
        lightingConstants.TileParams =
        {
            m_RenderTargets.LightTileCountX,
            kLightTileSize,
            kLightTileCapacity,
            // 「このフレームのライトグリッドは有効か」。**パスを積む述語と同じものを使う** ――
            // トグルの状態(m_GeometrySettings.LightCullingEnabled)ではなく実際に書いたかどうかで決める。
            // なおMegaLightsが走るフレームはLightCount.wが先に効くのでこの枝には入らない
            ShouldRunLightCulling() ? 1u : 0u,
        };

        // ライトリストの中身の更新。**直接光パスや半透明パスの中で呼んではいけない** ――
        // タイルライトカリングパスが両者より先にこのバッファを読むため、
        // グラフを組み立てる前に1箇所でまとめて済ませる(更新回数も1回で済む)。
        // 0灯のフレームでは更新自体を省略してよい(シェーダはライト数までしかループしないため)
        if (!gpuLights.empty())
        {
            commandList->UpdateBuffer(m_SceneGPUResources.LightBuffer.get(), gpuLights.data(), gpuLights.size() * sizeof(GPULight));
        }

        UploadDroneInstances(commandList);

        // 各パスをリソースの読み書き依存関係から自動的に順序付けて実行するレンダーグラフ。
        // トランジェントリソースの確保は行わず、既存の永続確保済みテクスチャ(G-Buffer・SceneColor等)を
        // そのまま読み書きする(詳細はRenderGraph.h参照)
        // --- パス群へ配るフレームのスナップショット(段階6) ---
        // 【実体はRender()にある】この下で埋めるフィールドのうち、Lights / Lighting / Constants /
        // Sun は生ポインタで、指す先もRender()のローカル
        // パス群が読む設定を、この1箇所でまとめて写す。**UIパネルの描画
        // (m_UIManager->Draw)はこの行より前で終わっている**ので、写しても値は変わらない
        frameContext.Settings.AmbientOcclusion = m_AmbientOcclusionSettings;
        frameContext.Settings.Cloud = m_CloudSettings;
        frameContext.Settings.DDGI = m_DDGISettings;
        frameContext.Settings.DebugView = m_DebugViewSettings;
        frameContext.Settings.EmissiveLight = m_EmissiveLightSettings;
        frameContext.Settings.Geometry = m_GeometrySettings;
        frameContext.Settings.IBL = m_IBLSettings;
        frameContext.Settings.MegaLights = m_MegaLightsSettings;
        frameContext.Settings.PostProcess = m_PostProcessSettings;
        frameContext.Settings.ReflectionProbe = m_ReflectionProbeSettings;
        frameContext.Settings.Reflection = m_ReflectionSettings;
        frameContext.Settings.Shadow = m_ShadowSettings;
        frameContext.Settings.Sky = m_SkySettings;
        frameContext.Settings.Water = m_WaterSettings;
        // どの経路が走るかを、ここで1回だけ判定して配る。**述語の実装は増やさない**
        // (Should* が唯一の定義。パス群はその結果だけを見る)
        frameContext.MegaLightsRuns = ShouldRunMegaLights();
        frameContext.LightCullingRuns = ShouldRunLightCulling();
        frameContext.RaytracedShadowRuns = ShouldRunRaytracedShadow();
        frameContext.RaytracedAORuns = ShouldRunRaytracedAO();
        frameContext.RaytracedReflectionRuns = ShouldRunRaytracedReflection();
        frameContext.RaytracedDDGITraceRuns = ShouldRunRaytracedDDGITrace();
        frameContext.SuppressEmissiveForGI = ShouldSuppressEmissiveForGI();
        frameContext.MegaLightsSamplesPerPixel = MegaLightsSamplesPerPixel();
        frameContext.MeshletLOD = m_MeshletLODFrame;
        frameContext.EffectiveExposureEV100 = m_EffectiveExposureEV100;
        // 【ここで有効性を解決する】無効なフレームに何を配るかを1箇所で決めておく。
        // 群ごとに判定を書くと、片方だけ条件を変えたときに静かに食い違う
        frameContext.TAAPrevViewProj =
            m_TAAPrevViewProjValid ? m_TAAPrevViewProj : DirectX::XMFLOAT4X4{};
        frameContext.TAAPrevJitterUv = m_TAAPrevJitterUv;
        frameContext.TAAPrevEffectiveExposureEV100 = m_TAAPrevEffectiveExposureEV100;
        frameContext.TAAHistoryIndex = m_TAAHistoryIndex;
        frameContext.DeltaTime = m_RenderDeltaTime;
        frameContext.ActiveCloudTransmittance = m_ActiveCloudTransmittance;
        frameContext.RenderWidth = m_RenderWidth;
        frameContext.RenderHeight = m_RenderHeight;
        frameContext.WindowWidth = m_Window->GetWidth();
        frameContext.WindowHeight = m_Window->GetHeight();
        frameContext.Capabilities = m_RenderCapabilities;
        frameContext.ActiveAOTexture = GetActiveAOTexture();
        frameContext.ActiveAORawTexture = GetActiveAORawTexture();
        frameContext.ActiveReflectionOutput = GetActiveReflectionOutput();
        frameContext.UpscaleAvailable = IsUpscaleActive();
        frameContext.SwapChain = m_SwapChain.get();
        // 【遅延生成のためだけに渡す】使ってよいのはMegaLightsの読み戻しバッファだけ
        frameContext.Device = m_Device.get();
        frameContext.FrameConstantBuffer = m_FrameConstantBuffer.get();
        frameContext.ObjectConstantBuffer = m_ObjectConstantBuffer.get();
        frameContext.MaterialSamplers = m_MaterialSamplers.get();
        frameContext.ScreenSpaceSamplers = m_ScreenSpaceSamplers.get();
        frameContext.EffectiveExposure = effectiveExposure;
        frameContext.PlanarReflectionPassRuns = planarReflectionPassRuns;
        frameContext.ManualExposureScale = manualExposureScale;
        frameContext.KeyReferenceEV100 = keyReferenceEV100;
        frameContext.InvViewProj = invViewProj;
        frameContext.UsingProceduralSky = usingProceduralSky;
        frameContext.FogPassRuns = fogPassRuns;
        frameContext.DepthPrepassRuns = depthPrepassRuns;
        frameContext.HiZFromDepthPrepass = hiZFromDepthPrepass;
        frameContext.OcclusionCullEnabledThisFrame = occlusionCullEnabledThisFrame;
        frameContext.OcclusionCullingActive = occlusionCullingActive;
        frameContext.MeshletPathActive = meshletPathActive;
        frameContext.MeshletCullStatsActive = meshletCullStatsActive;
        frameContext.CameraMoveDistance = cameraMoveDistance;
        frameContext.ReflectMatrix = reflectMatrix;
        frameContext.ReflectedViewProj = reflectedViewProj;
        frameContext.WaterPlaneY = waterPlaneY;
        frameContext.CameraPosition = cameraPosition;
        frameContext.ViewProj = viewProj;
        frameContext.Lights = &gpuLights;
        frameContext.Lighting = &lightingConstants;
        frameContext.BakeSkyThisFrame = bakeSkyThisFrame;
        frameContext.SkyIntegrateThisFrame = skyIntegrateThisFrame;
        frameContext.Sun = &sunLighting;
        frameContext.Constants = &constants;
        frameContext.SkyTexture = skyTexture;
        frameContext.IBL = &m_IBLResources;
        frameContext.Sky = &m_SkyResources;
        frameContext.Scene = &m_SceneGPUResources;
        frameContext.Targets = &m_RenderTargets;
        frameContext.GI = &m_GIResources;
        frameContext.ProbeBakeSignature = ComputeProbeBakeSignature();
        frameContext.DroneShow = &m_DroneShowResources;
        // 【述語をここで1回だけ決める】本描画と平面反射の2群が同じ判定を見る必要がある
        frameContext.DroneShowRuns = m_DroneShowEnabled && !m_DroneInstances.empty();
        frameContext.DroneCount = static_cast<uint32_t>(m_DroneInstances.size());
        frameContext.DroneShowBrightness = m_DroneShow.Data().Brightness;
        frameContext.DroneShowMinScreenRadius = m_DroneShowMinScreenRadius;
    }
}
