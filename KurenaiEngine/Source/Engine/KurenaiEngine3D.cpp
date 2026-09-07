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
#include <fstream>
#include <functional>
#include <limits>
#include <random>

#include "Assets/SceneLoader.h"
#include "Core/Logger.h"
#include "Core/RenderGraph.h"
#include "Core/StringUtil.h"
#include "Diagnostics/RenderDumpService.h"
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
#include "Rendering/GPULight.h"
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
        using Rendering::ExtractFrustumPlanes;
        using Rendering::IsAABBVisible;
        using Rendering::IsMeshVisibleWithStats;

        // TAAのジッターに使う低食い違い量列(Halton列)。基数baseのradical inverse、
        // すなわちindexを基数base表記にして小数点の左右を反転した値を返す([0,1)に収まる)。
        // 乱数と違い、少ない点数でも区間内へ均等に散らばるのが要点で、8フレームぶん取れば
        // ピクセル内に8点が偏りなく配置される
        float RadicalInverse(uint32_t index, uint32_t base)
        {
            float result = 0.0f;
            float fraction = 1.0f / static_cast<float>(base);
            while (index > 0)
            {
                result += static_cast<float>(index % base) * fraction;
                index /= base;
                fraction /= static_cast<float>(base);
            }
            return result;
        }

        // TAAのジッター周期(フレーム数)。長いほど多くのサンプル位置を踏めるが、
        // その分だけ収束に時間がかかり、カメラが動いている間の見た目が不安定になる。
        // 8はUnreal Engine等でも使われる実用的な妥協点
        constexpr uint32_t kTAAJitterSampleCount = 8;

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

        // DeferredLighting.hlsl側のstruct GPUReflectionProbeと並び・ストライド(48バイト)を
        // 一致させる必要がある
        struct alignas(16) GPUReflectionProbe
        {
            DirectX::XMFLOAT4 PositionRadius; // xyz=ワールド座標(Box形状では箱の中心), w=Sphere形状の影響半径
            DirectX::XMFLOAT4 BoxExtents;     // xyz=Box形状の各軸の半径(ハーフエクステント), w=ブレンド距離
            DirectX::XMFLOAT4 ShapeParams;    // x=形状(0=Sphere,1=Box), y=sin(Yaw), z=cos(Yaw), w=未使用
        };
        // 【HLSL側の宣言とレイアウトを揃えたまま保つための固定】cbuffer(と構造化バッファ)は
        // 宣言順でオフセットが決まるので、ここで並べ替え・挿入・型変更が起きると、
        // HLSL側を直さないかぎり黙って別の値を読むことになる。
        // **通すために期待値を書き換えないこと**(FrameConstants.h と同じ規約)。
        //
        // 【これが守るのはC++側だけ】HLSLの宣言と突き合わせているわけではない。
        // ここが落ちたら「HLSL側も同じだけ動かせ」という合図として使う
        static_assert(offsetof(GPUReflectionProbe, PositionRadius) == 0, "PositionRadius のレイアウトが変わっている");
        static_assert(offsetof(GPUReflectionProbe, BoxExtents) == 16, "BoxExtents のレイアウトが変わっている");
        static_assert(offsetof(GPUReflectionProbe, ShapeParams) == 32, "ShapeParams のレイアウトが変わっている");
        static_assert(sizeof(GPUReflectionProbe) == 48, "GPUReflectionProbe の総サイズが変わっている");

        // 直射日光(正午・快晴)の照度[lx]。Lagarde & de Rousiers 2014の照度参照テーブルに
        // 掲載される代表値
        constexpr float kSunIlluminanceLux = 100000.0f;
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

        // 被覆率から求める全天の平均透過率(判断B)。IBL用キューブマップには雲を焼き込まない
        // (Sky.hlsliの雲セクション、判断Aのコメント参照)ため、被覆率が上がってもキューブの
        // 明るさが晴天のまま据え置かれてしまう。これを補うため、キューブへ焼く天頂輝度にだけ
        // この平均透過率を掛けて全体を暗くする。
        // 【物理的な導出ではない】実際の曇天は多重散乱・雲の厚みで複雑に減光するが、ここでは
        // 「被覆率0で1.0(無変化)、被覆率1でkCloudOvercastTransmittanceまで直線的に落ちる」という
        // 単純な線形補間で済ませている。目的はIBLの明るさが被覆率に応じて定性的に下がることであり、
        // 精密な値は求めていない(実測で調整可能)
        constexpr float kCloudOvercastTransmittance = 0.35f;

        // 巻雲側の「全天が巻雲のときの透過率」。積雲のkCloudOvercastTransmittance(0.35)より
        // 1に近い値にしてある。巻雲は光学的に薄く(CirrusDensityが積雲の1桁下)、全天を覆っても
        // 積雲ほど大きくは減光しないという定性的な近似であり、精密な値は求めていない
        // (実測で調整可能)
        constexpr float kCirrusOvercastTransmittance = 0.75f;

        // 1層ぶんの「被覆率→平均透過率」の線形補間。ComputeCloudAverageTransmittanceが
        // 積雲・巻雲の両方でこの1つの式を共有する
        float ComputeCloudLayerTransmittance(bool layerEnabled, float coverage, float overcastTransmittance)
        {
            if (!layerEnabled)
            {
                return 1.0f;
            }
            const float clampedCoverage = std::clamp(coverage, 0.0f, 1.0f);
            // lerp(1.0f, overcastTransmittance, clampedCoverage)と同じ
            return 1.0f + (overcastTransmittance - 1.0f) * clampedCoverage;
        }

        // 被覆率から求める全天の平均透過率(判断B)。巻雲(2層目)も加味し、
        // T = T_cumulus(積雲の被覆率) * T_cirrus(巻雲の被覆率) という2層の積で求める。
        // 巻雲を無効化・被覆率0にした場合はT_cirrus=1.0になり、積雲だけの値になる
        float ComputeCloudAverageTransmittance(
            bool cloudEnabled, float coverage, bool cirrusEnabled, float cirrusCoverage)
        {
            const float cumulusTransmittance =
                ComputeCloudLayerTransmittance(cloudEnabled, coverage, kCloudOvercastTransmittance);
            const float cirrusTransmittance =
                ComputeCloudLayerTransmittance(cirrusEnabled, cirrusCoverage, kCirrusOvercastTransmittance);
            return cumulusTransmittance * cirrusTransmittance;
        }

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


        // 区画1つぶんのバイト数。区画の境目も8バイト境界に載せたいので256へ切り上げる
        uint32_t ComputeModelCullRegionStride(uint32_t capacity)
        {
            const uint32_t bytes = RHI::IRHICommandList::kDispatchMeshIndirectArgStride * capacity;
            return (bytes + 255u) & ~255u;
        }

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

        // t5の構造化バッファに詰めるライトの最大数。実データ(BistroInterior.fbxで4灯)に対しては
        // 十分すぎる余裕を持たせてあるが、構造化バッファなのでこの容量自体がGPU時間へ影響することはない
        // (シェーダはLightCount.xまでしかループしないため)
        constexpr uint32_t kMaxLights = 1024;

        // MegaLightsTilePool.hlsl の kMegaLightsMaxLights と同じ値。あちらはライトごとの重みを
        // groupshared配列に置くためコンパイル時定数である必要があり、C++からの受け渡しでは代用できない。
        //
        // 【なぜ静的検査で縛るのか】候補プールは走査するライト数をこの値で頭打ちにするが、
        // タイルライトカリング(LightCulling.hlsl)は頭打ちしない。kMaxLightsをこれより大きくすると、
        // **判定を共有しているのに定義域だけが黙ってずれる**(あぶれた灯はカリングには入るが
        // 候補プールには入らない)。到達判定の共有では防げない食い違いなので、ここで止める
        constexpr uint32_t kMegaLightsTilePoolMaxLights = 1024;
        static_assert(
            kMaxLights <= kMegaLightsTilePoolMaxLights,
            "kMaxLightsを増やすなら MegaLightsTilePool.hlsl の kMegaLightsMaxLights も同じ値へ上げること"
            "(候補プールが走査するライト数の上限。超えるとタイルライトカリングと定義域がずれる)");

        // ドローンショーの機体数の上限。構造化バッファをこの容量で固定確保する
        // (32バイト×4096 = 128KB。DEFAULTヒープ本体とステージングリングを足しても
        //  1.3MB程度で、機体数を増減しても作り直さずに済む)
        constexpr uint32_t kMaxDrones = 4096;

        // kLightTileSize / kLightTileCapacity / kLightTileStride はKurenaiEngine3Dのstatic constexprへ
        // 移した(DebugViewPanelがヒートマップの上限として参照するため)。定義はKurenaiEngine3D.h

        // 自前ソフトウェアラスタライザ用。Shaders/3D/SoftwareRasterCommon.hlsliの
        // cbuffer SWRasterConstants(b1)と並び・サイズを一致させること
        struct alignas(16) SWRasterConstants
        {
            DirectX::XMFLOAT4X4 ViewProj;
            // xy=レンダー解像度(画素)、zw=その逆数
            DirectX::XMFLOAT4 RenderSize;
            // xyz=太陽光が進む向き(正規化済み)、w=未使用
            DirectX::XMFLOAT4 SunDirection;
            // x=CSRasterのX方向グループ数(2D分解の復元用)、y=シーン全体の三角形数、
            // z=メッシュレコード数、w=巨大三角形とみなすbbox画素面積のしきい値
            DirectX::XMUINT4 DispatchParams;
            // x=巨大三角形リストの容量、yzw=未使用
            DirectX::XMUINT4 LargeParams;
        };
        // 【HLSL側の宣言とレイアウトを揃えたまま保つための固定】cbuffer(と構造化バッファ)は
        // 宣言順でオフセットが決まるので、ここで並べ替え・挿入・型変更が起きると、
        // HLSL側を直さないかぎり黙って別の値を読むことになる。
        // **通すために期待値を書き換えないこと**(FrameConstants.h と同じ規約)。
        //
        // 【これが守るのはC++側だけ】HLSLの宣言と突き合わせているわけではない。
        // ここが落ちたら「HLSL側も同じだけ動かせ」という合図として使う
        static_assert(offsetof(SWRasterConstants, ViewProj) == 0, "ViewProj のレイアウトが変わっている");
        static_assert(offsetof(SWRasterConstants, RenderSize) == 64, "RenderSize のレイアウトが変わっている");
        static_assert(offsetof(SWRasterConstants, SunDirection) == 80, "SunDirection のレイアウトが変わっている");
        static_assert(offsetof(SWRasterConstants, DispatchParams) == 96, "DispatchParams のレイアウトが変わっている");
        static_assert(offsetof(SWRasterConstants, LargeParams) == 112, "LargeParams のレイアウトが変わっている");
        static_assert(sizeof(SWRasterConstants) == 128, "SWRasterConstants の総サイズが変わっている");

        // 自前ソフトウェアラスタライザが読むメッシュ1件ぶんの情報。
        // Shaders/3D/SoftwareRasterCommon.hlsliのSWRasterMeshInfoと並び・サイズを一致させること。
        //
        // 【構造化バッファは詰めて並ぶ】定数バッファと違いHLSLのStructuredBuffer<T>は
        // C++と同じ詰め方になるため、このままのレイアウトで一致する
        struct SWRasterMeshInfo
        {
            DirectX::XMFLOAT4X4 World;
            DirectX::XMFLOAT4X4 NormalMatrix;
            // 頂点/インデックスバッファのbindless番号(IRHIBuffer::GetBindlessIndex)
            uint32_t VertexBufferIndex;
            uint32_t IndexBufferIndex;
            // シーン全体の通し三角形番号における、このメッシュの先頭。シェーダー側の二分探索のキー
            uint32_t FirstTriangle;
            uint32_t TriangleCount;
            // ミラーリングされたインスタンス(ModelInstance::IsMirrored)なら-1。
            // 表裏判定の符号を反転させる
            float FrontFaceSign;
            // bit0 = アルファカットアウト(フェーズ2で使う。現在は常に0)
            uint32_t Flags;
            uint32_t Padding[2];
        };

        static_assert(sizeof(SWRasterMeshInfo) == 160, "HLSL側のSWRasterMeshInfoと一致させるため160バイト固定");

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
            // Params.y = このライトが影を落とすか。ライトごとに切れるようにしてあるのは、
            // ピクセルあたりのシャドウレイ数に上限(Passes::LightingConstants.LightCount.y)があり、
            // 「影を出したいライト」に予算を回せるようにするため
            // Params.z = 光源そのものの半径[m]。0なら点光源。予約枠だった zw のうち z を使う。
            // 【平行光には入れない】太陽は MegaLights の対象外で、円盤サンプリングは
            // RTShadow.hlsl が別に持っている
            const float sourceRadius =
                (light.Type == Assets::LightType::Directional) ? 0.0f : std::max(0.0f, light.SourceRadius);
            // Params.y は影のフラグ。**bit0 = スクリーンスペースシャドウ / bit1 = レイトレース影レイ**
            // (Shaders/3D/LightAttenuation.hlsli と一致させること)。作者が置いたライトは
            // 両方を立てる ―― 1つの真偽値だった頃と挙動が変わらない。
            // 【リテラルで 3.0f と書かない】ビットの定義を変えたときに追随しない
            const float shadowFlags = light.CastShadow
                                          ? static_cast<float>(kLightShadowScreenSpace | kLightShadowRaytraced)
                                          : 0.0f;
            gpuLight.Params = { angleOffset, shadowFlags, sourceRadius, 0.0f };
            return gpuLight;
        }

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
        // ―― 群はまだリソースを持たず、登録時にエンジン側を参照するだけである
        m_EnvironmentPasses = std::make_unique<Passes::EnvironmentPasses>(*this);
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

        // ジオメトリパス(G-Buffer書き込み)
        RHI::ShaderDesc gbufferVsDesc;
        gbufferVsDesc.Stage = RHI::ShaderStage::Vertex;
        gbufferVsDesc.FilePath = shaderDirectory + L"GBuffer.kshader";
        gbufferVsDesc.EntryPoint = "VSMain";
        m_GBufferVertexShader = m_Device->CreateShader(gbufferVsDesc);

        RHI::ShaderDesc gbufferPsDesc;
        gbufferPsDesc.Stage = RHI::ShaderStage::Pixel;
        gbufferPsDesc.FilePath = shaderDirectory + L"GBuffer.kshader";
        gbufferPsDesc.EntryPoint = "PSMain";
        m_GBufferPixelShader = m_Device->CreateShader(gbufferPsDesc);

        // 水面(ModelInstance::IsWater)専用のピクセルシェーダー(水面マテリアル基盤)。
        // 頂点シェーダーはWater.hlslもGBufferCommon.hlsli由来の同じVSMainを使うため、
        // m_GBufferVertexShaderをそのまま共有する(専用のVSは作らない)
        RHI::ShaderDesc gbufferWaterPsDesc;
        gbufferWaterPsDesc.Stage = RHI::ShaderStage::Pixel;
        gbufferWaterPsDesc.FilePath = shaderDirectory + L"Water.kshader";
        gbufferWaterPsDesc.EntryPoint = "PSMain";
        m_GBufferWaterPixelShader = m_Device->CreateShader(gbufferWaterPsDesc);

        // 深度プリパス(41.22節)のアルファカットアウト用。頂点シェーダーはG-Bufferと共有する
        // (プリパスとG-Bufferで深度が1ulpでもずれると面が消えるため。PSO作成側のコメント参照)
        try
        {
            RHI::ShaderDesc depthPrepassCutoutPsDesc;
            depthPrepassCutoutPsDesc.Stage = RHI::ShaderStage::Pixel;
            depthPrepassCutoutPsDesc.FilePath = shaderDirectory + L"DepthPrepass.kshader";
            depthPrepassCutoutPsDesc.EntryPoint = "PSMainCutout";
            m_DepthPrepassCutoutPixelShader = m_Device->CreateShader(depthPrepassCutoutPsDesc);
        }
        catch (const std::exception& e)
        {
            // 作れなくてもプリパス自体は成立する(カットアウトのメッシュをプリパスから
            // 除外して従来どおりG-Bufferだけで描く)ため、致命的とはしない
            m_DepthPrepassCutoutPixelShader.reset();
            Core::Logger::Error(
                "KurenaiEngine3D",
                std::string("深度プリパスのアルファカットアウト用ピクセルシェーダーの作成に失敗しました。"
                            "カットアウトのメッシュはプリパスから除外します: ") + e.what());
        }

        // メッシュシェーダー版のG-Bufferパス(GBufferMeshlet.hlsl)。
        // 対応環境でのみ作る ―― 非対応環境ではas/msプロファイルのコンパイル自体ができず、
        // 毎回エラーログが出てしまうため。ピクセルシェーダーはGBuffer.hlslのものを共有する
        // (メッシュレットのON/OFFで見た目が変わらないことがこのパスの前提)
        if (m_Device->SupportsMeshShader())
        {
            RHI::ShaderDesc gbufferAsDesc;
            gbufferAsDesc.Stage = RHI::ShaderStage::Amplification;
            gbufferAsDesc.FilePath = shaderDirectory + L"GBufferMeshlet.kshader";
            gbufferAsDesc.EntryPoint = "ASMain";
            m_GBufferAmplificationShader = m_Device->CreateShader(gbufferAsDesc);

            RHI::ShaderDesc gbufferMsDesc;
            gbufferMsDesc.Stage = RHI::ShaderStage::Mesh;
            gbufferMsDesc.FilePath = shaderDirectory + L"GBufferMeshlet.kshader";
            gbufferMsDesc.EntryPoint = "MSMain";
            m_GBufferMeshShader = m_Device->CreateShader(gbufferMsDesc);

            // メッシュレットごとに色分けするデバッグ表示用
            RHI::ShaderDesc gbufferMeshletDebugPsDesc;
            gbufferMeshletDebugPsDesc.Stage = RHI::ShaderStage::Pixel;
            // 実体はGBuffer.hlsl側(PSMainをそのまま呼んでアルベドだけ差し替えるため)
            gbufferMeshletDebugPsDesc.FilePath = shaderDirectory + L"GBuffer.kshader";
            gbufferMeshletDebugPsDesc.EntryPoint = "PSMainMeshletDebug";
            m_GBufferMeshletDebugPixelShader = m_Device->CreateShader(gbufferMeshletDebugPsDesc);

            m_ShadowPasses->CreateMeshletShaders(*m_Device, shaderDirectory);
        }

        // G-BufferのPSOはEmissiveのフォーマットがバッファ精度に依存するため、
        // この関数の末尾でCreatePrecisionDependentPipelineStates()がまとめて作る

        // 直接光パス(頂点バッファなしのフルスクリーン三角形。G-Buffer+シャドウマップからPBRの
        // 直接光を計算しHDRで書き出す)
        RHI::ShaderDesc directLightVsDesc;
        directLightVsDesc.Stage = RHI::ShaderStage::Vertex;
        directLightVsDesc.FilePath = shaderDirectory + L"DirectLighting.kshader";
        directLightVsDesc.EntryPoint = "VSMain";
        m_DirectLightVertexShader = m_Device->CreateShader(directLightVsDesc);

        RHI::ShaderDesc directLightPsDesc;
        directLightPsDesc.Stage = RHI::ShaderStage::Pixel;
        directLightPsDesc.FilePath = shaderDirectory + L"DirectLighting.kshader";
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
        aoVsDesc.FilePath = shaderDirectory + L"SSAO.kshader";
        aoVsDesc.EntryPoint = "VSMain";
        m_AOVertexShader = m_Device->CreateShader(aoVsDesc);

        // SSAOパス
        RHI::ShaderDesc ssaoPsDesc;
        ssaoPsDesc.Stage = RHI::ShaderStage::Pixel;
        ssaoPsDesc.FilePath = shaderDirectory + L"SSAO.kshader";
        ssaoPsDesc.EntryPoint = "PSMain";
        m_SSAOPixelShader = m_Device->CreateShader(ssaoPsDesc);

        // SSAO/SSIL/AOブラーのPSOは出力先(AO/GIバッファ)のフォーマットがバッファ精度に依存するため、
        // この関数の末尾でCreatePrecisionDependentPipelineStates()がまとめて作る

        m_SSAOKernel = Passes::GenerateSSAOKernel(m_AmbientOcclusionSettings.SSAOKernelSize);

        RHI::BufferDesc ssaoConstantBufferDesc;
        ssaoConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        ssaoConstantBufferDesc.SizeInBytes = sizeof(Passes::SSAOConstants);
        m_SSAOConstantBuffer = m_Device->CreateBuffer(ssaoConstantBufferDesc);

        // SSILパス(Visibility Bitmask)
        RHI::ShaderDesc ssilPsDesc;
        ssilPsDesc.Stage = RHI::ShaderStage::Pixel;
        ssilPsDesc.FilePath = shaderDirectory + L"SSIL_VisibilityBitmask.kshader";
        ssilPsDesc.EntryPoint = "PSMain";
        m_SSILPixelShader = m_Device->CreateShader(ssilPsDesc);

        RHI::BufferDesc ssilConstantBufferDesc;
        ssilConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        ssilConstantBufferDesc.SizeInBytes = sizeof(Passes::SSILConstants);
        m_SSILConstantBuffer = m_Device->CreateBuffer(ssilConstantBufferDesc);

        // AO/GI共通のブラーパス(SSAO.hlslのPSMainBlurを、rgbaフォーマットが同じSSAO/SSIL両方で使い回す)
        RHI::ShaderDesc aoBlurPsDesc;
        aoBlurPsDesc.Stage = RHI::ShaderStage::Pixel;
        aoBlurPsDesc.FilePath = shaderDirectory + L"SSAO.kshader";
        aoBlurPsDesc.EntryPoint = "PSMainBlur";
        m_AOBlurPixelShader = m_Device->CreateShader(aoBlurPsDesc);

        // AO/GI無効時はこの常に黒・不透明(遮蔽なし=a:1、間接光なし=rgb:0)のテクスチャをライティングパスに渡す
        m_AODisabledTexture = m_Device->CreateSolidColorTexture(0, 0, 0, 255);

        // ライティングパス(頂点バッファなしのフルスクリーン三角形)
        RHI::ShaderDesc lightingVsDesc;
        lightingVsDesc.Stage = RHI::ShaderStage::Vertex;
        lightingVsDesc.FilePath = shaderDirectory + L"DeferredLighting.kshader";
        lightingVsDesc.EntryPoint = "VSMain";
        m_LightingVertexShader = m_Device->CreateShader(lightingVsDesc);

        RHI::ShaderDesc lightingPsDesc;
        lightingPsDesc.Stage = RHI::ShaderStage::Pixel;
        lightingPsDesc.FilePath = shaderDirectory + L"DeferredLighting.kshader";
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
        transparentVsDesc.FilePath = shaderDirectory + L"Transparent.kshader";
        transparentVsDesc.EntryPoint = "VSMain";
        m_TransparentVertexShader = m_Device->CreateShader(transparentVsDesc);

        RHI::ShaderDesc transparentPsDesc;
        transparentPsDesc.Stage = RHI::ShaderStage::Pixel;
        transparentPsDesc.FilePath = shaderDirectory + L"Transparent.kshader";
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
        transparentPipelineDesc.FrontCounterClockwise = true;
        m_TransparentPipelineStateMirrored = m_Device->CreatePipelineState(transparentPipelineDesc);

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
        // メッシュの描画(m_TransparentPipelineStateMirrored等)では鏡映ビュー行列が頂点そのものを
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

        // Hi-Zミップチェーン構築パス(コンピュートシェーダー)。CSCopyでG-Buffer深度をミップ0へコピーし、
        // CSDownsampleをミップ数-1回ディスパッチして1x1まで縮小する
        RHI::ShaderDesc hizCopyCsDesc;
        hizCopyCsDesc.Stage = RHI::ShaderStage::Compute;
        hizCopyCsDesc.FilePath = shaderDirectory + L"HiZ.kshader";
        hizCopyCsDesc.EntryPoint = "CSCopy";
        m_HiZCopyComputeShader = m_Device->CreateShader(hizCopyCsDesc);
        m_HiZCopyPipelineState = m_Device->CreateComputePipelineState({ m_HiZCopyComputeShader.get() });

        RHI::ShaderDesc hizDownsampleCsDesc;
        hizDownsampleCsDesc.Stage = RHI::ShaderStage::Compute;
        hizDownsampleCsDesc.FilePath = shaderDirectory + L"HiZ.kshader";
        hizDownsampleCsDesc.EntryPoint = "CSDownsample";
        m_HiZDownsampleComputeShader = m_Device->CreateShader(hizDownsampleCsDesc);
        m_HiZDownsamplePipelineState = m_Device->CreateComputePipelineState({ m_HiZDownsampleComputeShader.get() });

        RHI::BufferDesc hizConstantBufferDesc;
        hizConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        hizConstantBufferDesc.SizeInBytes = sizeof(Passes::HiZConstants);
        m_HiZConstantBuffer = m_Device->CreateBuffer(hizConstantBufferDesc);

        // タイルライトカリングパス(コンピュートシェーダー)。タイルごとに届くライトのインデックスリストを作る。
        // ライトグリッド本体(m_RenderTargets.LightTileBuffer)は解像度に依存するためCreateRenderTargetsで作る
        RHI::ShaderDesc lightCullingCsDesc;
        lightCullingCsDesc.Stage = RHI::ShaderStage::Compute;
        lightCullingCsDesc.FilePath = shaderDirectory + L"LightCulling.kshader";
        lightCullingCsDesc.EntryPoint = "CSMain";
        m_LightCullingComputeShader = m_Device->CreateShader(lightCullingCsDesc);
        m_LightCullingPipelineState = m_Device->CreateComputePipelineState({ m_LightCullingComputeShader.get() });

        RHI::BufferDesc lightCullingConstantBufferDesc;
        lightCullingConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        lightCullingConstantBufferDesc.SizeInBytes = sizeof(Passes::LightCullingConstants);
        m_LightCullingConstantBuffer = m_Device->CreateBuffer(lightCullingConstantBufferDesc);

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
                RHI::ShaderDesc swRasterCsDesc;
                swRasterCsDesc.Stage = RHI::ShaderStage::Compute;
                swRasterCsDesc.FilePath = shaderDirectory + L"SoftwareRaster.kshader";
                swRasterCsDesc.EntryPoint = "CSRaster";
                m_SoftwareRasterComputeShader = m_Device->CreateShader(swRasterCsDesc);

                RHI::ShaderDesc swRasterLargeCsDesc;
                swRasterLargeCsDesc.Stage = RHI::ShaderStage::Compute;
                swRasterLargeCsDesc.FilePath = shaderDirectory + L"SoftwareRaster.kshader";
                swRasterLargeCsDesc.EntryPoint = "CSRasterLarge";
                m_SoftwareRasterLargeComputeShader = m_Device->CreateShader(swRasterLargeCsDesc);

                RHI::ShaderDesc swRasterResolveCsDesc;
                swRasterResolveCsDesc.Stage = RHI::ShaderStage::Compute;
                swRasterResolveCsDesc.FilePath = shaderDirectory + L"SoftwareRasterResolve.kshader";
                swRasterResolveCsDesc.EntryPoint = "CSResolve";
                m_SoftwareRasterResolveComputeShader = m_Device->CreateShader(swRasterResolveCsDesc);

                m_SoftwareRasterPipelineState =
                    m_Device->CreateComputePipelineState({ m_SoftwareRasterComputeShader.get() });
                m_SoftwareRasterLargePipelineState =
                    m_Device->CreateComputePipelineState({ m_SoftwareRasterLargeComputeShader.get() });
                m_SoftwareRasterResolvePipelineState =
                    m_Device->CreateComputePipelineState({ m_SoftwareRasterResolveComputeShader.get() });

                RHI::BufferDesc swRasterConstantBufferDesc;
                swRasterConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
                swRasterConstantBufferDesc.SizeInBytes = sizeof(SWRasterConstants);
                m_SoftwareRasterConstantBuffer = m_Device->CreateBuffer(swRasterConstantBufferDesc);

                // メッシュレコード。毎フレームCPUから書き直すためStructuredReadOnly
                RHI::BufferDesc swRasterMeshInfoDesc;
                swRasterMeshInfoDesc.Usage = RHI::BufferUsage::StructuredReadOnly;
                swRasterMeshInfoDesc.SizeInBytes =
                    static_cast<uint32_t>(sizeof(SWRasterMeshInfo)) * kSWRasterMaxMeshes;
                swRasterMeshInfoDesc.StrideInBytes = static_cast<uint32_t>(sizeof(SWRasterMeshInfo));
                m_SoftwareRasterMeshInfoBuffer = m_Device->CreateBuffer(swRasterMeshInfoDesc);

                // 巨大三角形リスト。CSRasterがUAVで書き、CSRasterLargeがSRVで読むためStructuredRW
                RHI::BufferDesc swRasterLargeEntriesDesc;
                swRasterLargeEntriesDesc.Usage = RHI::BufferUsage::StructuredRW;
                swRasterLargeEntriesDesc.SizeInBytes =
                    static_cast<uint32_t>(sizeof(uint32_t)) * kSWRasterLargeListCapacity;
                swRasterLargeEntriesDesc.StrideInBytes = static_cast<uint32_t>(sizeof(uint32_t));
                m_SoftwareRasterLargeEntriesBuffer = m_Device->CreateBuffer(swRasterLargeEntriesDesc);

                // 間接ディスパッチ引数(uint3)。16バイトにしているのは4の倍数の要件と
                // アライメントを揃えるためで、実際に使うのは先頭12バイト
                RHI::BufferDesc swRasterIndirectArgsDesc;
                swRasterIndirectArgsDesc.Usage = RHI::BufferUsage::IndirectArgs;
                swRasterIndirectArgsDesc.SizeInBytes = 16;
                swRasterIndirectArgsDesc.StrideInBytes = static_cast<uint32_t>(sizeof(uint32_t));
                m_SoftwareRasterIndirectArgsBuffer = m_Device->CreateBuffer(swRasterIndirectArgsDesc);

                m_RenderCapabilities.SoftwareRasterAvailable = true;
            }
            catch (const std::exception& e)
            {
                Core::Logger::Warning(
                    "KurenaiEngine3D",
                    std::string("ソフトウェアラスタライザの初期化に失敗したため無効にします: ") + e.what());
                m_RenderCapabilities.SoftwareRasterAvailable = false;
                m_SoftwareRasterComputeShader.reset();
                m_SoftwareRasterLargeComputeShader.reset();
                m_SoftwareRasterResolveComputeShader.reset();
                m_SoftwareRasterPipelineState.reset();
                m_SoftwareRasterLargePipelineState.reset();
                m_SoftwareRasterResolvePipelineState.reset();
                m_SoftwareRasterConstantBuffer.reset();
                m_SoftwareRasterMeshInfoBuffer.reset();
                m_SoftwareRasterLargeEntriesBuffer.reset();
                m_SoftwareRasterIndirectArgsBuffer.reset();
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
        RHI::ShaderDesc aerialPerspectiveVsDesc;
        aerialPerspectiveVsDesc.Stage = RHI::ShaderStage::Vertex;
        aerialPerspectiveVsDesc.FilePath = shaderDirectory + L"AerialPerspective.kshader";
        aerialPerspectiveVsDesc.EntryPoint = "VSMain";
        m_AerialPerspectiveVertexShader = m_Device->CreateShader(aerialPerspectiveVsDesc);

        RHI::ShaderDesc aerialPerspectivePsDesc;
        aerialPerspectivePsDesc.Stage = RHI::ShaderStage::Pixel;
        aerialPerspectivePsDesc.FilePath = shaderDirectory + L"AerialPerspective.kshader";
        aerialPerspectivePsDesc.EntryPoint = "PSMain";
        m_AerialPerspectivePixelShader = m_Device->CreateShader(aerialPerspectivePsDesc);

        RHI::PipelineStateDesc aerialPerspectivePipelineDesc;
        aerialPerspectivePipelineDesc.VertexShader = m_AerialPerspectiveVertexShader.get();
        aerialPerspectivePipelineDesc.PixelShader = m_AerialPerspectivePixelShader.get();
        aerialPerspectivePipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
        aerialPerspectivePipelineDesc.RenderTargetFormats = { RHI::Format::R16G16B16A16_Float };
        m_AerialPerspectivePipelineState = m_Device->CreatePipelineState(aerialPerspectivePipelineDesc);

        // 雲パス(頂点バッファなしのフルスクリーン三角形。積雲と巻雲だけを1/2解像度で評価し、
        // 透過率と事前乗算済みの散乱光を書く)。専用のb1定数バッファは持たない
        // (パラメータはFrameConstants末尾のCloudParams0-3等に入っているため)
        RHI::ShaderDesc skyCloudVsDesc;
        skyCloudVsDesc.Stage = RHI::ShaderStage::Vertex;
        skyCloudVsDesc.FilePath = shaderDirectory + L"SkyCloud.kshader";
        skyCloudVsDesc.EntryPoint = "VSMain";
        m_SkyCloudVertexShader = m_Device->CreateShader(skyCloudVsDesc);

        RHI::ShaderDesc skyCloudPsDesc;
        skyCloudPsDesc.Stage = RHI::ShaderStage::Pixel;
        skyCloudPsDesc.FilePath = shaderDirectory + L"SkyCloud.kshader";
        skyCloudPsDesc.EntryPoint = "PSMain";
        m_SkyCloudPixelShader = m_Device->CreateShader(skyCloudPsDesc);

        RHI::PipelineStateDesc skyCloudPipelineDesc;
        skyCloudPipelineDesc.VertexShader = m_SkyCloudVertexShader.get();
        skyCloudPipelineDesc.PixelShader = m_SkyCloudPixelShader.get();
        skyCloudPipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
        // 2枚目は fogInFront(雲に最初に当たった位置の霞の透過率、P18b)。並びはSkyCloud.hlslの
        // PSOutputおよびSkyCloudパスのRenderTargetsと一致させること。
        // 1チャンネルなのでDDGIResolveの低解像度深度と同じR32_Floatにする
        skyCloudPipelineDesc.RenderTargetFormats = {
            RHI::Format::R16G16B16A16_Float,
            RHI::Format::R32_Float,
        };
        m_SkyCloudPipelineState = m_Device->CreatePipelineState(skyCloudPipelineDesc);

        // DDGIの低解像度解決パス(雲パスと同じ作り。拡散イラディアンスとinsideWeightを書く)
        RHI::ShaderDesc ddgiResolveVsDesc;
        ddgiResolveVsDesc.Stage = RHI::ShaderStage::Vertex;
        ddgiResolveVsDesc.FilePath = shaderDirectory + L"DDGIResolve.kshader";
        ddgiResolveVsDesc.EntryPoint = "VSMain";
        m_DDGIResolveVertexShader = m_Device->CreateShader(ddgiResolveVsDesc);

        RHI::ShaderDesc ddgiResolvePsDesc;
        ddgiResolvePsDesc.Stage = RHI::ShaderStage::Pixel;
        ddgiResolvePsDesc.FilePath = shaderDirectory + L"DDGIResolve.kshader";
        ddgiResolvePsDesc.EntryPoint = "PSMain";
        m_DDGIResolvePixelShader = m_Device->CreateShader(ddgiResolvePsDesc);

        RHI::PipelineStateDesc ddgiResolvePipelineDesc;
        ddgiResolvePipelineDesc.VertexShader = m_DDGIResolveVertexShader.get();
        ddgiResolvePipelineDesc.PixelShader = m_DDGIResolvePixelShader.get();
        ddgiResolvePipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
        // 2枚目はこのテクセルが代表している全解像度の深度(41.24節)。並びはDDGIResolve.hlslの
        // PSOutputおよびDDGIResolveパスのRenderTargetsと一致させること。
        // Reverse-Zの生値をそのまま持つのでR32_Float(合成側の相対差の判定に十分な精度が要る)
        ddgiResolvePipelineDesc.RenderTargetFormats = {
            RHI::Format::R16G16B16A16_Float,
            RHI::Format::R32_Float,
        };
        m_DDGIResolvePipelineState = m_Device->CreatePipelineState(ddgiResolvePipelineDesc);

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
                RHI::BufferDesc cullStatsDesc;
                cullStatsDesc.Usage = RHI::BufferUsage::Structured;
                cullStatsDesc.SizeInBytes = static_cast<uint32_t>(sizeof(uint32_t)) * kMeshletCullStatsCount;
                cullStatsDesc.StrideInBytes = static_cast<uint32_t>(sizeof(uint32_t));
                m_MeshletCullStatsBuffer = m_Device->CreateBuffer(cullStatsDesc);

                // 【SRVではなくUAVを登録する】増幅シェーダーは読むのではなく書く。
                // RegisterBindless(SRV)の番号を渡すと読み取り専用のビューへ書き込むことになる
                m_MeshletCullStatsBindlessIndex = m_Device->RegisterBindlessUAV(m_MeshletCullStatsBuffer.get());
                if (m_MeshletCullStatsBindlessIndex == RHI::kInvalidBindlessIndex)
                {
                    Core::Logger::Warning(
                        "KurenaiEngine3D",
                        "メッシュレットカリングの統計バッファをbindlessへ登録できませんでした(統計を無効にします)");
                    m_MeshletCullStatsBuffer.reset();
                }
                else
                {
                    for (uint32_t i = 0; i < kMeshletCullStatsRingSize; ++i)
                    {
                        RHI::BufferDesc readbackDesc;
                        readbackDesc.Usage = RHI::BufferUsage::Readback;
                        readbackDesc.SizeInBytes = cullStatsDesc.SizeInBytes;
                        readbackDesc.StrideInBytes = cullStatsDesc.StrideInBytes;
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
                m_MeshletCullStatsBuffer.reset();
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
                RHI::ShaderDesc modelCullCsDesc;
                modelCullCsDesc.Stage = RHI::ShaderStage::Compute;
                modelCullCsDesc.FilePath = shaderDirectory + L"ModelCull.kshader";
                modelCullCsDesc.EntryPoint = "CSMain";
                m_ModelCullComputeShader = m_Device->CreateShader(modelCullCsDesc);
                m_ModelCullPipelineState =
                    m_Device->CreateComputePipelineState({ m_ModelCullComputeShader.get() });

                RHI::BufferDesc modelCullConstantDesc;
                modelCullConstantDesc.Usage = RHI::BufferUsage::Constant;
                modelCullConstantDesc.SizeInBytes = sizeof(Passes::ModelCullConstants);
                m_ModelCullConstantBuffer = m_Device->CreateBuffer(modelCullConstantDesc);

                RHI::BufferDesc modelCullCounterDesc;
                modelCullCounterDesc.Usage = RHI::BufferUsage::Structured;
                modelCullCounterDesc.SizeInBytes =
                    static_cast<uint32_t>(sizeof(uint32_t)) * kModelCullCounterCount;
                modelCullCounterDesc.StrideInBytes = static_cast<uint32_t>(sizeof(uint32_t));
                m_ModelCullCounterBuffer = m_Device->CreateBuffer(modelCullCounterDesc);

                for (uint32_t i = 0; i < kMeshletCullStatsRingSize; ++i)
                {
                    RHI::BufferDesc readbackDesc;
                    readbackDesc.Usage = RHI::BufferUsage::Readback;
                    readbackDesc.SizeInBytes = modelCullCounterDesc.SizeInBytes;
                    readbackDesc.StrideInBytes = modelCullCounterDesc.StrideInBytes;
                    m_ModelCullReadback[i] = m_Device->CreateBuffer(readbackDesc);
                }
            }
            catch (const std::exception& e)
            {
                // カリングが作れないだけで描画は成立する(CPU側のループがそのまま描く)
                Core::Logger::Warning(
                    "KurenaiEngine3D",
                    std::string("モデル単位のGPUカリングの初期化に失敗したため無効にします: ") + e.what());
                m_ModelCullComputeShader.reset();
                m_ModelCullPipelineState.reset();
                m_ModelCullConstantBuffer.reset();
                m_ModelCullCounterBuffer.reset();
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

            // MegaLightsの参照実装(コンピュートシェーダー。ポイント/スポットライトを全灯
            // 総当たりし、届いた1灯ごとに光源までの影レイを撃つ)。以降の確率的サンプリングを
            // 評価するときの真値を作るためのパスで、RayQueryを含むためシェーダーモデル6.5が要る
            RHI::ShaderDesc megaLightsRefCsDesc;
            megaLightsRefCsDesc.Stage = RHI::ShaderStage::Compute;
            megaLightsRefCsDesc.FilePath = shaderDirectory + L"MegaLightsReference.kshader";
            megaLightsRefCsDesc.EntryPoint = "CSMain";
            m_MegaLightsReferenceComputeShader = m_Device->CreateShader(megaLightsRefCsDesc);
            m_MegaLightsReferencePipelineState =
                m_Device->CreateComputePipelineState({ m_MegaLightsReferenceComputeShader.get() });

            RHI::BufferDesc megaLightsConstantBufferDesc;
            megaLightsConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
            megaLightsConstantBufferDesc.SizeInBytes = sizeof(Passes::MegaLightsConstants);
            m_MegaLightsConstantBuffer = m_Device->CreateBuffer(megaLightsConstantBufferDesc);

            // MegaLightsの候補プール(コンピュートシェーダー。タイルごとに届くライトを走査して
            // 重みつきでK灯を抽出する)。レイを撃たないのでRayQueryは要らないが、
            // MegaLightsと同時にしか使わないためここで一緒に作る
            RHI::ShaderDesc megaLightsTilePoolCsDesc;
            megaLightsTilePoolCsDesc.Stage = RHI::ShaderStage::Compute;
            megaLightsTilePoolCsDesc.FilePath = shaderDirectory + L"MegaLightsTilePool.kshader";
            megaLightsTilePoolCsDesc.EntryPoint = "CSMain";
            m_MegaLightsTilePoolComputeShader = m_Device->CreateShader(megaLightsTilePoolCsDesc);
            m_MegaLightsTilePoolPipelineState =
                m_Device->CreateComputePipelineState({ m_MegaLightsTilePoolComputeShader.get() });

            RHI::BufferDesc megaLightsTilePoolConstantBufferDesc;
            megaLightsTilePoolConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
            megaLightsTilePoolConstantBufferDesc.SizeInBytes = sizeof(Passes::MegaLightsTilePoolConstants);
            m_MegaLightsTilePoolConstantBuffer = m_Device->CreateBuffer(megaLightsTilePoolConstantBufferDesc);

            // MegaLightsの確率的サンプリング本体(2パス)。
            // 【この4本はすべて RayQuery を含む】Initial は初期可視レイ、Temporal は
            // 時間検証レイ、Spatial は目標関数の可視性とバイアス補正レイ、Shade は影レイ。
            // したがってシェーダーモデル6.5が要る(パッカーの kSkipDxbc50Files を参照)。
            // レイを撃たないのは TilePool / Denoise / Accum / Resolve の4本だけで、
            // そちらは3バリアントすべてで焼かれる
            RHI::ShaderDesc megaLightsInitialCsDesc;
            megaLightsInitialCsDesc.Stage = RHI::ShaderStage::Compute;
            megaLightsInitialCsDesc.FilePath = shaderDirectory + L"MegaLightsInitialSample.kshader";
            megaLightsInitialCsDesc.EntryPoint = "CSMain";
            m_MegaLightsInitialComputeShader = m_Device->CreateShader(megaLightsInitialCsDesc);
            m_MegaLightsInitialPipelineState =
                m_Device->CreateComputePipelineState({ m_MegaLightsInitialComputeShader.get() });

            RHI::ShaderDesc megaLightsShadeCsDesc;
            megaLightsShadeCsDesc.Stage = RHI::ShaderStage::Compute;
            megaLightsShadeCsDesc.FilePath = shaderDirectory + L"MegaLightsShade.kshader";
            megaLightsShadeCsDesc.EntryPoint = "CSMain";
            m_MegaLightsShadeComputeShader = m_Device->CreateShader(megaLightsShadeCsDesc);
            m_MegaLightsShadePipelineState =
                m_Device->CreateComputePipelineState({ m_MegaLightsShadeComputeShader.get() });

            // クアッド共有(手法3)の解決パス。2x2の仲間が撃った標本を自分の面で評価し直して
            // 平均する。**レイを1本も撃たない**ので3バリアントすべてで焼ける
            // (パッカーの kSkipDxbc50Files には入れない)
            RHI::ShaderDesc megaLightsResolveCsDesc;
            megaLightsResolveCsDesc.Stage = RHI::ShaderStage::Compute;
            megaLightsResolveCsDesc.FilePath = shaderDirectory + L"MegaLightsResolve.kshader";
            megaLightsResolveCsDesc.EntryPoint = "CSMain";
            m_MegaLightsResolveComputeShader = m_Device->CreateShader(megaLightsResolveCsDesc);
            m_MegaLightsResolvePipelineState =
                m_Device->CreateComputePipelineState({ m_MegaLightsResolveComputeShader.get() });

            // 空間再利用。目標関数に可視性を入れるレイと、不偏化の分母のためのバイアス補正レイを撃つ
            RHI::ShaderDesc megaLightsSpatialCsDesc;
            megaLightsSpatialCsDesc.Stage = RHI::ShaderStage::Compute;
            megaLightsSpatialCsDesc.FilePath = shaderDirectory + L"MegaLightsSpatial.kshader";
            megaLightsSpatialCsDesc.EntryPoint = "CSMain";
            m_MegaLightsSpatialComputeShader = m_Device->CreateShader(megaLightsSpatialCsDesc);
            m_MegaLightsSpatialPipelineState =
                m_Device->CreateComputePipelineState({ m_MegaLightsSpatialComputeShader.get() });

            // 時間再利用。採用した履歴サンプルが今も見えるかを確かめる時間検証レイを1本撃つ
            RHI::ShaderDesc megaLightsTemporalCsDesc;
            megaLightsTemporalCsDesc.Stage = RHI::ShaderStage::Compute;
            megaLightsTemporalCsDesc.FilePath = shaderDirectory + L"MegaLightsTemporal.kshader";
            megaLightsTemporalCsDesc.EntryPoint = "CSMain";
            m_MegaLightsTemporalComputeShader = m_Device->CreateShader(megaLightsTemporalCsDesc);
            m_MegaLightsTemporalPipelineState =
                m_Device->CreateComputePipelineState({ m_MegaLightsTemporalComputeShader.get() });

            // デノイザ。3エントリ(時間累積 / à-trous / 復調戻し)を1ファイルに置く。
            // パッカーは1ファイル内の複数の[numthreads]を自動で見つける
            {
                RHI::ShaderDesc denoiseDesc;
                denoiseDesc.Stage = RHI::ShaderStage::Compute;
                denoiseDesc.FilePath = shaderDirectory + L"MegaLightsDenoise.kshader";
                denoiseDesc.EntryPoint = "CSTemporalAccum";
                m_MegaLightsDenoiseTemporalShader = m_Device->CreateShader(denoiseDesc);
                m_MegaLightsDenoiseTemporalPSO =
                    m_Device->CreateComputePipelineState({ m_MegaLightsDenoiseTemporalShader.get() });
                denoiseDesc.EntryPoint = "CSAtrous";
                m_MegaLightsDenoiseAtrousShader = m_Device->CreateShader(denoiseDesc);
                m_MegaLightsDenoiseAtrousPSO =
                    m_Device->CreateComputePipelineState({ m_MegaLightsDenoiseAtrousShader.get() });
                denoiseDesc.EntryPoint = "CSRemodulate";
                m_MegaLightsDenoiseRemodulateShader = m_Device->CreateShader(denoiseDesc);
                m_MegaLightsDenoiseRemodulatePSO =
                    m_Device->CreateComputePipelineState({ m_MegaLightsDenoiseRemodulateShader.get() });

                RHI::BufferDesc denoiseCbDesc;
                denoiseCbDesc.Usage = RHI::BufferUsage::Constant;
                denoiseCbDesc.SizeInBytes = sizeof(Passes::MegaLightsDenoiseConstants);
                m_MegaLightsDenoiseConstantBuffer = m_Device->CreateBuffer(denoiseCbDesc);
            }

            RHI::BufferDesc megaLightsStochasticConstantBufferDesc;
            megaLightsStochasticConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
            megaLightsStochasticConstantBufferDesc.SizeInBytes = sizeof(MegaLightsStochasticConstants);
            m_MegaLightsStochasticConstantBuffer =
                m_Device->CreateBuffer(megaLightsStochasticConstantBufferDesc);
            // 空間再利用の反復ごとに1本ずつ。中身は共有分と同じで反復番号だけが違う
            for (uint32_t spatialIteration = 0u; spatialIteration < kMegaLightsMaxSpatialIterations;
                 ++spatialIteration)
            {
                m_MegaLightsSpatialConstantBuffer[spatialIteration] =
                    m_Device->CreateBuffer(megaLightsStochasticConstantBufferDesc);
            }

            // 蓄積平均(計測専用)。レイを撃たないがMegaLightsと同時にしか使わないのでここで作る
            RHI::ShaderDesc megaLightsAccumCsDesc;
            megaLightsAccumCsDesc.Stage = RHI::ShaderStage::Compute;
            megaLightsAccumCsDesc.FilePath = shaderDirectory + L"MegaLightsAccum.kshader";
            megaLightsAccumCsDesc.EntryPoint = "CSMain";
            m_MegaLightsAccumComputeShader = m_Device->CreateShader(megaLightsAccumCsDesc);
            m_MegaLightsAccumPipelineState =
                m_Device->CreateComputePipelineState({ m_MegaLightsAccumComputeShader.get() });

            RHI::BufferDesc megaLightsAccumConstantBufferDesc;
            megaLightsAccumConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
            megaLightsAccumConstantBufferDesc.SizeInBytes = sizeof(Passes::MegaLightsAccumConstants);
            m_MegaLightsAccumConstantBuffer = m_Device->CreateBuffer(megaLightsAccumConstantBufferDesc);

            // RTAOパス(コンピュートシェーダー。半球へレイを撃ち遮蔽率と間接拡散光を求める)
            RHI::ShaderDesc rtAOCsDesc;
            rtAOCsDesc.Stage = RHI::ShaderStage::Compute;
            rtAOCsDesc.FilePath = shaderDirectory + L"RTAO.kshader";
            rtAOCsDesc.EntryPoint = "CSMain";
            m_RTAOComputeShader = m_Device->CreateShader(rtAOCsDesc);
            m_RTAOPipelineState = m_Device->CreateComputePipelineState({ m_RTAOComputeShader.get() });

            RHI::BufferDesc rtAOConstantBufferDesc;
            rtAOConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
            rtAOConstantBufferDesc.SizeInBytes = sizeof(Passes::RTAOConstants);
            m_RTAOConstantBuffer = m_Device->CreateBuffer(rtAOConstantBufferDesc);

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
                RHI::ShaderDesc ddgiTraceCsDesc;
                ddgiTraceCsDesc.Stage = RHI::ShaderStage::Compute;
                ddgiTraceCsDesc.FilePath = shaderDirectory + L"DDGIProbeTrace.kshader";
                ddgiTraceCsDesc.EntryPoint = "CSMain";
                m_DDGIProbeTraceComputeShader = m_Device->CreateShader(ddgiTraceCsDesc);
                m_DDGIProbeTracePipelineState =
                    m_Device->CreateComputePipelineState({ m_DDGIProbeTraceComputeShader.get() });

                RHI::BufferDesc ddgiTraceConstantBufferDesc;
                ddgiTraceConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
                ddgiTraceConstantBufferDesc.SizeInBytes = sizeof(Passes::DDGITraceConstants);
                // プローブ1個につき6面ぶん書き換えるため、既定の段数では
                // 更新プローブ数を増やしたときに足りなくなる(1フレーム最大64プローブ×6面=384回)
                ddgiTraceConstantBufferDesc.MaxConstantUpdatesPerFrame = 1024;
                m_DDGITraceConstantBuffer = m_Device->CreateBuffer(ddgiTraceConstantBufferDesc);

                m_RenderCapabilities.DDGIRaytracedTraceAvailable = true;
            }
            catch (const std::exception& e)
            {
                // ShouldRunRaytracedDDGITraceがパイプラインステートのnullを見ているため、
                // ここで捨てておけばレイ取得はラスタ経路のまま動く
                m_DDGIProbeTracePipelineState.reset();
                m_DDGIProbeTraceComputeShader.reset();
                m_DDGITraceConstantBuffer.reset();
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

        // TAAパス(頂点バッファなしのフルスクリーン三角形。前フレームの結果をモーションベクターで
        // 再投影して蓄積する)。出力は履歴バッファ(常にfp16)で、バッファ精度の設定に依存しないため
        // CreatePrecisionDependentPipelineStatesではなくここで一度だけ作ればよい
        RHI::ShaderDesc taaVsDesc;
        taaVsDesc.Stage = RHI::ShaderStage::Vertex;
        taaVsDesc.FilePath = shaderDirectory + L"TAA.kshader";
        taaVsDesc.EntryPoint = "VSMain";
        m_TAAVertexShader = m_Device->CreateShader(taaVsDesc);

        RHI::ShaderDesc taaPsDesc;
        taaPsDesc.Stage = RHI::ShaderStage::Pixel;
        taaPsDesc.FilePath = shaderDirectory + L"TAA.kshader";
        taaPsDesc.EntryPoint = "PSMain";
        m_TAAPixelShader = m_Device->CreateShader(taaPsDesc);

        RHI::PipelineStateDesc taaPipelineDesc;
        taaPipelineDesc.VertexShader = m_TAAVertexShader.get();
        taaPipelineDesc.PixelShader = m_TAAPixelShader.get();
        taaPipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
        taaPipelineDesc.RenderTargetFormats = { RHI::Format::R16G16B16A16_Float };
        m_TAAPipelineState = m_Device->CreatePipelineState(taaPipelineDesc);

        RHI::BufferDesc taaConstantBufferDesc;
        taaConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        taaConstantBufferDesc.SizeInBytes = sizeof(Passes::TAAConstants);
        m_TAAConstantBuffer = m_Device->CreateBuffer(taaConstantBufferDesc);

        // Tonemapパス(頂点バッファなしのフルスクリーン三角形。HDRのSceneColorをLDRへ変換する)
        RHI::ShaderDesc tonemapVsDesc;
        tonemapVsDesc.Stage = RHI::ShaderStage::Vertex;
        tonemapVsDesc.FilePath = shaderDirectory + L"Tonemap.kshader";
        tonemapVsDesc.EntryPoint = "VSMain";
        m_TonemapVertexShader = m_Device->CreateShader(tonemapVsDesc);

        RHI::ShaderDesc tonemapPsDesc;
        tonemapPsDesc.Stage = RHI::ShaderStage::Pixel;
        tonemapPsDesc.FilePath = shaderDirectory + L"Tonemap.kshader";
        tonemapPsDesc.EntryPoint = "PSMain";
        m_TonemapPixelShader = m_Device->CreateShader(tonemapPsDesc);

        RHI::PipelineStateDesc tonemapPipelineDesc;
        tonemapPipelineDesc.VertexShader = m_TonemapVertexShader.get();
        tonemapPipelineDesc.PixelShader = m_TonemapPixelShader.get();
        tonemapPipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
        tonemapPipelineDesc.RenderTargetFormats = { RHI::Format::R8G8B8A8_UNorm };
        m_TonemapPipelineState = m_Device->CreatePipelineState(tonemapPipelineDesc);

        RHI::BufferDesc tonemapConstantBufferDesc;
        tonemapConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        tonemapConstantBufferDesc.SizeInBytes = sizeof(Passes::TonemapConstants);
        m_TonemapConstantBuffer = m_Device->CreateBuffer(tonemapConstantBufferDesc);

        // 超解像パス(EASU=拡大、RCAS=シャープ化。どちらもコンピュートシェーダー)。
        // レンダーターゲットではなくUAVへ書くのでPSOにフォーマットの指定は要らない
        RHI::ShaderDesc upscaleEasuCsDesc;
        upscaleEasuCsDesc.Stage = RHI::ShaderStage::Compute;
        upscaleEasuCsDesc.FilePath = shaderDirectory + L"Upscale.kshader";
        upscaleEasuCsDesc.EntryPoint = "CSEASU";
        m_UpscaleEASUComputeShader = m_Device->CreateShader(upscaleEasuCsDesc);
        m_UpscaleEASUPipelineState = m_Device->CreateComputePipelineState({ m_UpscaleEASUComputeShader.get() });

        RHI::ShaderDesc upscaleRcasCsDesc;
        upscaleRcasCsDesc.Stage = RHI::ShaderStage::Compute;
        upscaleRcasCsDesc.FilePath = shaderDirectory + L"Upscale.kshader";
        upscaleRcasCsDesc.EntryPoint = "CSRCAS";
        m_UpscaleRCASComputeShader = m_Device->CreateShader(upscaleRcasCsDesc);
        m_UpscaleRCASPipelineState = m_Device->CreateComputePipelineState({ m_UpscaleRCASComputeShader.get() });

        RHI::BufferDesc upscaleConstantBufferDesc;
        upscaleConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        upscaleConstantBufferDesc.SizeInBytes = sizeof(Passes::UpscaleConstants);
        m_UpscaleConstantBuffer = m_Device->CreateBuffer(upscaleConstantBufferDesc);

        // 自動露出パス(輝度ヒストグラムの構築→縮約→時間方向の順応。すべてコンピュートシェーダー)
        RHI::ShaderDesc autoExposureClearCsDesc;
        autoExposureClearCsDesc.Stage = RHI::ShaderStage::Compute;
        autoExposureClearCsDesc.FilePath = shaderDirectory + L"AutoExposure.kshader";
        autoExposureClearCsDesc.EntryPoint = "CSClearHistogram";
        m_AutoExposureClearComputeShader = m_Device->CreateShader(autoExposureClearCsDesc);
        m_AutoExposureClearPipelineState =
            m_Device->CreateComputePipelineState({ m_AutoExposureClearComputeShader.get() });

        RHI::ShaderDesc autoExposureHistogramCsDesc;
        autoExposureHistogramCsDesc.Stage = RHI::ShaderStage::Compute;
        autoExposureHistogramCsDesc.FilePath = shaderDirectory + L"AutoExposure.kshader";
        autoExposureHistogramCsDesc.EntryPoint = "CSHistogram";
        m_AutoExposureHistogramComputeShader = m_Device->CreateShader(autoExposureHistogramCsDesc);
        m_AutoExposureHistogramPipelineState =
            m_Device->CreateComputePipelineState({ m_AutoExposureHistogramComputeShader.get() });

        RHI::ShaderDesc autoExposureResolveCsDesc;
        autoExposureResolveCsDesc.Stage = RHI::ShaderStage::Compute;
        autoExposureResolveCsDesc.FilePath = shaderDirectory + L"AutoExposure.kshader";
        autoExposureResolveCsDesc.EntryPoint = "CSResolve";
        m_AutoExposureResolveComputeShader = m_Device->CreateShader(autoExposureResolveCsDesc);
        m_AutoExposureResolvePipelineState =
            m_Device->CreateComputePipelineState({ m_AutoExposureResolveComputeShader.get() });

        RHI::BufferDesc exposureHistogramBufferDesc;
        exposureHistogramBufferDesc.Usage = RHI::BufferUsage::Structured;
        exposureHistogramBufferDesc.SizeInBytes = sizeof(uint32_t) * kExposureHistogramBins;
        exposureHistogramBufferDesc.StrideInBytes = sizeof(uint32_t);
        m_ExposureHistogramBuffer = m_Device->CreateBuffer(exposureHistogramBufferDesc);

        RHI::BufferDesc autoExposureConstantBufferDesc;
        autoExposureConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        autoExposureConstantBufferDesc.SizeInBytes = sizeof(Passes::AutoExposureConstants);
        m_AutoExposureConstantBuffer = m_Device->CreateBuffer(autoExposureConstantBufferDesc);

        // 露出の保存先。フレームをまたいで順応の履歴を保持するため、ウィンドウリサイズで
        // 作り直されるCreateRenderTargetsではなくここで一度だけ作る。
        // 生成直後はゼロクリアされており、texel(1,0)=0が「未初期化」を意味する
        // (CSResolveがこれを見て初回だけ順応を飛ばして即座に目標値へ合わせる)
        m_ExposureTexture = m_Device->CreateUAVTexture(2, 1, RHI::Format::R32_Float);

        // ブルームパス(ダウンサンプル/アップサンプルの2エントリ。テクスチャはCreateRenderTargetsで作る)
        RHI::ShaderDesc bloomDownCsDesc;
        bloomDownCsDesc.Stage = RHI::ShaderStage::Compute;
        bloomDownCsDesc.FilePath = shaderDirectory + L"Bloom.kshader";
        bloomDownCsDesc.EntryPoint = "CSDownsample";
        m_BloomDownsampleComputeShader = m_Device->CreateShader(bloomDownCsDesc);
        m_BloomDownsamplePipelineState =
            m_Device->CreateComputePipelineState({ m_BloomDownsampleComputeShader.get() });

        RHI::ShaderDesc bloomUpCsDesc;
        bloomUpCsDesc.Stage = RHI::ShaderStage::Compute;
        bloomUpCsDesc.FilePath = shaderDirectory + L"Bloom.kshader";
        bloomUpCsDesc.EntryPoint = "CSUpsample";
        m_BloomUpsampleComputeShader = m_Device->CreateShader(bloomUpCsDesc);
        m_BloomUpsamplePipelineState =
            m_Device->CreateComputePipelineState({ m_BloomUpsampleComputeShader.get() });

        RHI::BufferDesc bloomConstantBufferDesc;
        bloomConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        bloomConstantBufferDesc.SizeInBytes = sizeof(Passes::BloomConstants);
        m_BloomConstantBuffer = m_Device->CreateBuffer(bloomConstantBufferDesc);

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
        // 最初のフレームで一度だけ行う(m_IBLBaked参照)。ここではリソースの作成のみ行う
        m_IBLResources.CreateEnvironmentMaps(
            *m_Device, kIBLIrradianceSize, kIBLPrefilterBaseSize, kIBLPrefilterMipLevels);
        // BRDF積分LUTは2パスで焼く。パス1(CSMain)が(A, B)をスクラッチへ書き、
        // パス2(CSCombineEavg)がそれを読んでEavgを足した float4(A, B, Eavg, 0) を最終LUTへ書く。
        // 同一リソースをSRVとUAVへ同時バインドできないためスクラッチが要る(BRDFLUT.hlsl参照)
        m_BRDFLUTScratchTexture = m_Device->CreateUAVTexture(kIBLBRDFLUTSize, kIBLBRDFLUTSize, RHI::Format::R16G16_Float);
        m_IBLResources.CreateBRDFLUT(*m_Device, kIBLBRDFLUTSize);
        if (!m_BRDFLUTScratchTexture || !m_IBLResources.BRDFLUTTexture)
        {
            Core::Logger::Error("KurenaiEngine3D",
                "BRDF積分LUTのテクスチャ作成に失敗しました(スペキュラのエネルギー補正が正しく動作しません)");
        }

        RHI::ShaderDesc brdfLutCsDesc;
        brdfLutCsDesc.Stage = RHI::ShaderStage::Compute;
        brdfLutCsDesc.FilePath = shaderDirectory + L"BRDFLUT.kshader";
        brdfLutCsDesc.EntryPoint = "CSMain";
        m_BRDFLUTComputeShader = m_Device->CreateShader(brdfLutCsDesc);
        m_BRDFLUTPipelineState = m_Device->CreateComputePipelineState({ m_BRDFLUTComputeShader.get() });

        RHI::ShaderDesc brdfLutCombineCsDesc;
        brdfLutCombineCsDesc.Stage = RHI::ShaderStage::Compute;
        brdfLutCombineCsDesc.FilePath = shaderDirectory + L"BRDFLUT.kshader";
        brdfLutCombineCsDesc.EntryPoint = "CSCombineEavg";
        m_BRDFLUTCombineComputeShader = m_Device->CreateShader(brdfLutCombineCsDesc);
        if (!m_BRDFLUTCombineComputeShader)
        {
            Core::Logger::Error("KurenaiEngine3D",
                "BRDFLUT.hlsl CSCombineEavg のコンパイルに失敗しました"
                "(Kulla-Conty方式が必要とするEavgが焼かれず、同方式が正しく動作しません)");
        }
        m_BRDFLUTCombinePipelineState =
            m_Device->CreateComputePipelineState({ m_BRDFLUTCombineComputeShader.get() });

        // ボリュメトリック雲の3Dノイズ。カメラにも太陽にも空の状態にも依存しない
        // 純粋な手続き生成なので、BRDF積分LUTと同じく起動後に一度だけ焼く(m_CloudNoiseBaked)。
        // ここではリソースとパイプラインの作成だけを行う
        m_SkyResources.CreateCloudNoise(
            *m_Device, kCloudShapeNoiseSize, kCloudDetailNoiseSize, kCloudWeatherNoiseSize);
        if (!m_SkyResources.CloudShapeNoiseTexture || !m_SkyResources.CloudDetailNoiseTexture ||
            !m_SkyResources.CloudWeatherNoiseTexture)
        {
            Core::Logger::Error("KurenaiEngine3D",
                "雲のノイズテクスチャの作成に失敗しました(ボリュメトリック雲が正しく描画されません)");
        }

        RHI::ShaderDesc cloudShapeNoiseCsDesc;
        cloudShapeNoiseCsDesc.Stage = RHI::ShaderStage::Compute;
        cloudShapeNoiseCsDesc.FilePath = shaderDirectory + L"CloudNoiseGenerate.kshader";
        cloudShapeNoiseCsDesc.EntryPoint = "CSGenerateShape";
        m_CloudShapeNoiseComputeShader = m_Device->CreateShader(cloudShapeNoiseCsDesc);
        if (!m_CloudShapeNoiseComputeShader)
        {
            Core::Logger::Error("KurenaiEngine3D",
                "CloudNoiseGenerate.hlsl CSGenerateShape のコンパイルに失敗しました"
                "(雲の形状ノイズが焼かれません)");
        }
        m_CloudShapeNoisePipelineState =
            m_Device->CreateComputePipelineState({ m_CloudShapeNoiseComputeShader.get() });

        RHI::ShaderDesc cloudDetailNoiseCsDesc;
        cloudDetailNoiseCsDesc.Stage = RHI::ShaderStage::Compute;
        cloudDetailNoiseCsDesc.FilePath = shaderDirectory + L"CloudNoiseGenerate.kshader";
        cloudDetailNoiseCsDesc.EntryPoint = "CSGenerateDetail";
        m_CloudDetailNoiseComputeShader = m_Device->CreateShader(cloudDetailNoiseCsDesc);
        if (!m_CloudDetailNoiseComputeShader)
        {
            Core::Logger::Error("KurenaiEngine3D",
                "CloudNoiseGenerate.hlsl CSGenerateDetail のコンパイルに失敗しました"
                "(雲のディテールノイズが焼かれません)");
        }
        m_CloudDetailNoisePipelineState =
            m_Device->CreateComputePipelineState({ m_CloudDetailNoiseComputeShader.get() });

        RHI::ShaderDesc cloudWeatherNoiseCsDesc;
        cloudWeatherNoiseCsDesc.Stage = RHI::ShaderStage::Compute;
        cloudWeatherNoiseCsDesc.FilePath = shaderDirectory + L"CloudNoiseGenerate.kshader";
        cloudWeatherNoiseCsDesc.EntryPoint = "CSGenerateWeather";
        m_CloudWeatherNoiseComputeShader = m_Device->CreateShader(cloudWeatherNoiseCsDesc);
        if (!m_CloudWeatherNoiseComputeShader)
        {
            Core::Logger::Error("KurenaiEngine3D",
                "CloudNoiseGenerate.hlsl CSGenerateWeather のコンパイルに失敗しました"
                "(ウェザーマップが焼かれず、雲がまったく立ちません)");
        }
        m_CloudWeatherNoisePipelineState =
            m_Device->CreateComputePipelineState({ m_CloudWeatherNoiseComputeShader.get() });

        // 大気散乱のLUT(P14a: Hillaire 2020)。TransmittanceとMultiScatteringはカメラにも太陽にも
        // 依存せず、大気パラメータ(濁りを含む)だけの関数なので、濁りが変わらない限り焼き直さない
        // (m_AtmosphereLUTBakedTurbidity)。SkyViewは太陽の位置と濁りで変わるため、
        // そのどちらかが動いたときに焼き直す(m_SkyViewBakedSunPosition)。
        m_SkyResources.CreateAtmosphereLUTs(
            *m_Device, kTransmittanceLUTWidth, kTransmittanceLUTHeight, kMultiScatteringLUTSize,
            kSkyViewLUTWidth, kSkyViewLUTHeight);
        if (!m_SkyResources.TransmittanceLUT || !m_SkyResources.MultiScatteringLUT ||
            !m_SkyResources.SkyViewLUT)
        {
            Core::Logger::Error("KurenaiEngine3D",
                "大気散乱のLUTテクスチャの作成に失敗しました(日中の空が黒くなります)");
        }
        // 【ここで焼き直し要求を必ず立てる】3枚とも中身が未初期化の新しいテクスチャになったので、
        // 「前回焼いたときの条件」を捨てないと、APIをDX11/DX12で切り替えた直後など
        // この関数が再度呼ばれた場合に一度も焼かれないまま読まれて空が黒くなる
        m_AtmosphereLUTBakedTurbidity = -1.0f;
        m_SkyViewBakedTurbidity = -1.0f;
        m_SkyViewBakedSunPosition = { 0.0f, 0.0f, 0.0f };

        RHI::BufferDesc atmosphereConstantBufferDesc;
        atmosphereConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        atmosphereConstantBufferDesc.SizeInBytes = sizeof(Passes::AtmosphereConstants);
        m_AtmosphereConstantBuffer = m_Device->CreateBuffer(atmosphereConstantBufferDesc);
        if (!m_AtmosphereConstantBuffer)
        {
            Core::Logger::Error("KurenaiEngine3D",
                "大気散乱の定数バッファの作成に失敗しました(日中の空が黒くなります)");
        }

        RHI::ShaderDesc transmittanceCsDesc;
        transmittanceCsDesc.Stage = RHI::ShaderStage::Compute;
        transmittanceCsDesc.FilePath = shaderDirectory + L"AtmosphereLUT.kshader";
        transmittanceCsDesc.EntryPoint = "CSTransmittance";
        m_TransmittanceComputeShader = m_Device->CreateShader(transmittanceCsDesc);
        if (!m_TransmittanceComputeShader)
        {
            Core::Logger::Error("KurenaiEngine3D",
                "AtmosphereLUT.hlsl CSTransmittance のコンパイルに失敗しました");
        }
        m_TransmittancePipelineState =
            m_Device->CreateComputePipelineState({ m_TransmittanceComputeShader.get() });

        RHI::ShaderDesc multiScatteringCsDesc;
        multiScatteringCsDesc.Stage = RHI::ShaderStage::Compute;
        multiScatteringCsDesc.FilePath = shaderDirectory + L"AtmosphereLUT.kshader";
        multiScatteringCsDesc.EntryPoint = "CSMultiScattering";
        m_MultiScatteringComputeShader = m_Device->CreateShader(multiScatteringCsDesc);
        if (!m_MultiScatteringComputeShader)
        {
            Core::Logger::Error("KurenaiEngine3D",
                "AtmosphereLUT.hlsl CSMultiScattering のコンパイルに失敗しました");
        }
        m_MultiScatteringPipelineState =
            m_Device->CreateComputePipelineState({ m_MultiScatteringComputeShader.get() });

        RHI::ShaderDesc skyViewCsDesc;
        skyViewCsDesc.Stage = RHI::ShaderStage::Compute;
        skyViewCsDesc.FilePath = shaderDirectory + L"AtmosphereLUT.kshader";
        skyViewCsDesc.EntryPoint = "CSSkyView";
        m_SkyViewComputeShader = m_Device->CreateShader(skyViewCsDesc);
        if (!m_SkyViewComputeShader)
        {
            Core::Logger::Error("KurenaiEngine3D",
                "AtmosphereLUT.hlsl CSSkyView のコンパイルに失敗しました(日中の空が黒くなります)");
        }
        m_SkyViewPipelineState =
            m_Device->CreateComputePipelineState({ m_SkyViewComputeShader.get() });

        RHI::ShaderDesc irradianceCsDesc;
        irradianceCsDesc.Stage = RHI::ShaderStage::Compute;
        irradianceCsDesc.FilePath = shaderDirectory + L"IBLConvolve.kshader";
        irradianceCsDesc.EntryPoint = "CSIrradiance";
        m_IrradianceComputeShader = m_Device->CreateShader(irradianceCsDesc);
        m_IrradiancePipelineState = m_Device->CreateComputePipelineState({ m_IrradianceComputeShader.get() });

        RHI::ShaderDesc prefilterCsDesc;
        prefilterCsDesc.Stage = RHI::ShaderStage::Compute;
        prefilterCsDesc.FilePath = shaderDirectory + L"IBLConvolve.kshader";
        prefilterCsDesc.EntryPoint = "CSPrefilter";
        m_PrefilterComputeShader = m_Device->CreateShader(prefilterCsDesc);
        m_IBLResources.PrefilterPipelineState = m_Device->CreateComputePipelineState({ m_PrefilterComputeShader.get() });

        // 拡散イラディアンスの球面調和関数(SH L2)経路。CSIrradianceの
        // 高速な代替で、A/B比較用にトグルで切り替える(m_IBLSettings.UseSHIrradiance、既定false)。
        // 詳細はIBLConvolve.hlsl冒頭のコメント参照
        RHI::ShaderDesc projectShCsDesc;
        projectShCsDesc.Stage = RHI::ShaderStage::Compute;
        projectShCsDesc.FilePath = shaderDirectory + L"IBLConvolve.kshader";
        projectShCsDesc.EntryPoint = "CSProjectSH";
        m_ProjectSHComputeShader = m_Device->CreateShader(projectShCsDesc);
        m_ProjectSHPipelineState = m_Device->CreateComputePipelineState({ m_ProjectSHComputeShader.get() });

        RHI::ShaderDesc projectShFinalCsDesc;
        projectShFinalCsDesc.Stage = RHI::ShaderStage::Compute;
        projectShFinalCsDesc.FilePath = shaderDirectory + L"IBLConvolve.kshader";
        projectShFinalCsDesc.EntryPoint = "CSProjectSHFinal";
        m_ProjectSHFinalComputeShader = m_Device->CreateShader(projectShFinalCsDesc);
        m_ProjectSHFinalPipelineState = m_Device->CreateComputePipelineState({ m_ProjectSHFinalComputeShader.get() });

        RHI::ShaderDesc evaluateShCsDesc;
        evaluateShCsDesc.Stage = RHI::ShaderStage::Compute;
        evaluateShCsDesc.FilePath = shaderDirectory + L"IBLConvolve.kshader";
        evaluateShCsDesc.EntryPoint = "CSEvaluateSH";
        m_EvaluateSHComputeShader = m_Device->CreateShader(evaluateShCsDesc);
        m_EvaluateSHPipelineState = m_Device->CreateComputePipelineState({ m_EvaluateSHComputeShader.get() });

        // SHの部分和(CSProjectSHのグループごとの出力)と最終係数(CSProjectSHFinalの出力)。
        // グループ数は (kSHProjectionSize/8)² × 6面で固定(射影解像度はSourceSkyboxの実解像度と
        // 無関係な固定値。kSHProjectionSizeのコメント参照)
        {
            const uint32_t groupsPerSide = (kSHProjectionSize + 7) / 8;
            const uint32_t maxSHGroups = groupsPerSide * groupsPerSide * kCubeFaceCount;
            RHI::BufferDesc shPartialSumsDesc;
            shPartialSumsDesc.Usage = RHI::BufferUsage::StructuredRW;
            shPartialSumsDesc.StrideInBytes = static_cast<uint32_t>(sizeof(DirectX::XMFLOAT4));
            shPartialSumsDesc.SizeInBytes = shPartialSumsDesc.StrideInBytes * maxSHGroups * kSHCoeffCount;
            m_SHPartialSumsBuffer = m_Device->CreateBuffer(shPartialSumsDesc);

            RHI::BufferDesc shCoefficientsDesc;
            shCoefficientsDesc.Usage = RHI::BufferUsage::StructuredRW;
            shCoefficientsDesc.StrideInBytes = static_cast<uint32_t>(sizeof(DirectX::XMFLOAT4));
            shCoefficientsDesc.SizeInBytes = shCoefficientsDesc.StrideInBytes * kSHCoeffCount;
            m_SHCoefficientsBuffer = m_Device->CreateBuffer(shCoefficientsDesc);
        }

        // 手続き空(SkyGenerate.hlsl)。太陽が動くたびに焼き直すため、IBLのプリフィルタと同じく
        // 面ごとに1回ずつディスパッチする。プリフィルタの入力にしかならないので解像度は
        // オフラインDDS(512)より小さい256で足りる(生成コストが1/4になる)
        m_ProceduralSkyTexture =
            m_Device->CreateUAVTextureCube(kProceduralSkySize, RHI::Format::R16G16B16A16_Float);

        RHI::ShaderDesc skyGenerateCsDesc;
        skyGenerateCsDesc.Stage = RHI::ShaderStage::Compute;
        skyGenerateCsDesc.FilePath = shaderDirectory + L"SkyGenerate.kshader";
        skyGenerateCsDesc.EntryPoint = "CSGenerateSky";
        m_SkyGenerateComputeShader = m_Device->CreateShader(skyGenerateCsDesc);
        m_SkyGeneratePipelineState = m_Device->CreateComputePipelineState({ m_SkyGenerateComputeShader.get() });

        RHI::BufferDesc skyBakeConstantBufferDesc;
        skyBakeConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        skyBakeConstantBufferDesc.SizeInBytes = sizeof(Passes::SkyBakeConstants);
        m_SkyBakeConstantBuffer = m_Device->CreateBuffer(skyBakeConstantBufferDesc);

        // 空パラメータ(ティント4本+照度正規化済みの天頂輝度)の積分をGPUで行うコンピュートシェーダー
        // 。SkyGenerateより前に実行し、結果をm_SkyResources.ParametersBufferへ書く
        RHI::ShaderDesc skyIntegrateCsDesc;
        skyIntegrateCsDesc.Stage = RHI::ShaderStage::Compute;
        skyIntegrateCsDesc.FilePath = shaderDirectory + L"SkyIntegrate.kshader";
        skyIntegrateCsDesc.EntryPoint = "CSIntegrateSky";
        m_SkyIntegrateComputeShader = m_Device->CreateShader(skyIntegrateCsDesc);
        m_SkyIntegratePipelineState = m_Device->CreateComputePipelineState({ m_SkyIntegrateComputeShader.get() });

        RHI::BufferDesc skyIntegrateConstantBufferDesc;
        skyIntegrateConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        skyIntegrateConstantBufferDesc.SizeInBytes = sizeof(Passes::SkyIntegrateConstants);
        m_SkyIntegrateConstantBuffer = m_Device->CreateBuffer(skyIntegrateConstantBufferDesc);

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
        // m_SkyParametersBufferInitialized参照)
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
        m_DDGICaptureColor = m_Device->CreateRenderTexture(kDDGICaptureSize, kDDGICaptureSize, RHI::Format::R16G16B16A16_Float);
        m_DDGICaptureDistance = m_Device->CreateRenderTexture(kDDGICaptureSize, kDDGICaptureSize, RHI::Format::R32_Float);
        m_DDGICaptureDepth = m_Device->CreateDepthTexture(kDDGICaptureSize, kDDGICaptureSize, 0.0f);
        // 6面を組み上げるスクラッチのキューブ。更新CSは1テクセル(=1つの方向)を出力するのに
        // 6面ぶん1536本のレイを全て走査するため、面ごとの2Dテクスチャではキューブとして
        // 引けず具合が悪い。放射輝度と距離で2本要る
        m_DDGICaptureRadianceCube = m_Device->CreateUAVTextureCube(kDDGICaptureSize, RHI::Format::R16G16B16A16_Float);
        m_DDGICaptureDistanceCube = m_Device->CreateUAVTextureCube(kDDGICaptureSize, RHI::Format::R32_Float);

        RHI::ShaderDesc ddgiUpdateCsDesc;
        ddgiUpdateCsDesc.Stage = RHI::ShaderStage::Compute;
        ddgiUpdateCsDesc.FilePath = shaderDirectory + L"DDGIProbeUpdate.kshader";
        ddgiUpdateCsDesc.EntryPoint = "CSUpdateProbe";
        m_DDGIProbeUpdateComputeShader = m_Device->CreateShader(ddgiUpdateCsDesc);
        m_DDGIProbeUpdatePipelineState = m_Device->CreateComputePipelineState({ m_DDGIProbeUpdateComputeShader.get() });

        // 境界の複製は本体の書き込みが全て終わってからでなければ正しい値を読めないため、
        // 同じディスパッチ内では行えず別パスになる(オクタヘドラルの縁は対辺へ折り返して繋がるので、
        // 自分のセルの反対側のテクセルを読む必要がある)
        RHI::ShaderDesc ddgiBorderCsDesc;
        ddgiBorderCsDesc.Stage = RHI::ShaderStage::Compute;
        ddgiBorderCsDesc.FilePath = shaderDirectory + L"DDGIProbeUpdate.kshader";
        ddgiBorderCsDesc.EntryPoint = "CSCopyBorder";
        m_DDGIBorderCopyComputeShader = m_Device->CreateShader(ddgiBorderCsDesc);
        m_DDGIBorderCopyPipelineState = m_Device->CreateComputePipelineState({ m_DDGIBorderCopyComputeShader.get() });

        RHI::BufferDesc ddgiUpdateConstantBufferDesc;
        ddgiUpdateConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        ddgiUpdateConstantBufferDesc.SizeInBytes = sizeof(Passes::DDGIUpdateConstants);
        m_DDGIUpdateConstantBuffer = m_Device->CreateBuffer(ddgiUpdateConstantBufferDesc);

        // スクロールで未確定になったプローブを、焼き直されるまでサンプリングから外すパス
        RHI::ShaderDesc ddgiInvalidateCsDesc;
        ddgiInvalidateCsDesc.Stage = RHI::ShaderStage::Compute;
        ddgiInvalidateCsDesc.FilePath = shaderDirectory + L"DDGIProbeUpdate.kshader";
        ddgiInvalidateCsDesc.EntryPoint = "CSInvalidateProbes";
        m_DDGIInvalidateProbesComputeShader = m_Device->CreateShader(ddgiInvalidateCsDesc);
        m_DDGIInvalidateProbesPipelineState =
            m_Device->CreateComputePipelineState({ m_DDGIInvalidateProbesComputeShader.get() });

        // 焼き直し待ちのスロット番号を渡す。最悪ケース(全プローブが一度に未確定)でも足りる大きさ。
        // 1フレームに1回しか書かないのでリングの段数は既定のままでよい
        RHI::BufferDesc ddgiDirtyBufferDesc;
        ddgiDirtyBufferDesc.Usage = RHI::BufferUsage::StructuredReadOnly;
        ddgiDirtyBufferDesc.SizeInBytes = static_cast<uint32_t>(sizeof(uint32_t)) * kDDGIMaxProbes;
        ddgiDirtyBufferDesc.StrideInBytes = static_cast<uint32_t>(sizeof(uint32_t));
        m_DDGIDirtyProbeBuffer = m_Device->CreateBuffer(ddgiDirtyBufferDesc);

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

        RHI::BufferDesc lightingConstantBufferDesc;
        lightingConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        lightingConstantBufferDesc.SizeInBytes = sizeof(Passes::LightingConstants);
        m_LightingConstantBuffer = m_Device->CreateBuffer(lightingConstantBufferDesc);

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
            return m_MegaLightsInitialPipelineState != nullptr && m_MegaLightsShadePipelineState != nullptr &&
                   m_MegaLightsTilePoolPipelineState != nullptr && m_RenderTargets.MegaLightsTilePoolBuffer != nullptr &&
                   m_MegaLightsReservoirBuffer != nullptr;
        }
        if (m_MegaLightsSettings.Mode == MegaLightsMode::QuadShared)
        {
            // Shade ではなく Resolve が色を書く。時間・空間再利用は使わないので、
            // 履歴バッファや空間再利用のping-pongが無くても走れる
            return m_MegaLightsInitialPipelineState != nullptr && m_MegaLightsResolvePipelineState != nullptr &&
                   m_MegaLightsTilePoolPipelineState != nullptr && m_RenderTargets.MegaLightsTilePoolBuffer != nullptr &&
                   m_MegaLightsReservoirBuffer != nullptr && m_MegaLightsHistoryGuide[0] != nullptr;
        }
        return m_MegaLightsReferencePipelineState != nullptr;
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
               m_RTAOPipelineState != nullptr && m_RTAORawTexture != nullptr && m_RTAOTexture != nullptr;
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
        m_MegaLightsAccumFrames = 0;
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
            m_MegaLightsDenoiseHistoryValid = false;
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
            m_MegaLightsHistoryValid = false;
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
            m_MegaLightsDumpPath.clear();
            return;
        }

        m_MegaLightsDumpPath = path;
        m_MegaLightsDumpIssued = false;
        m_MegaLightsDumpDone = false;
        m_MegaLightsDumpCopyFrame = 0;
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
               m_DDGIProbeTracePipelineState != nullptr && m_DDGITraceConstantBuffer != nullptr;
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

        return m_GeometrySettings.MeshletRenderingEnabled && m_GBufferMeshletPipelineState != nullptr;
    }

    void KurenaiEngine3D::EnsureModelCullCapacity(uint32_t candidateCount)
    {
        if (candidateCount == 0 || !m_ModelCullPipelineState)
        {
            return;
        }
        if (m_ModelCullInstanceBuffer && m_ModelCullDrawArgsBuffer && candidateCount <= m_ModelCullCapacity)
        {
            return;
        }

        // 作り直しの頻度を下げるため、必要数ぴったりではなく少し余裕を持たせる。
        // シーン切り替えとストリーミングで候補数は増減する
        const uint32_t capacity = std::max<uint32_t>(64u, candidateCount + candidateCount / 4u);

        try
        {
            // 候補の配列。毎フレームCPUから書き直すのでStructuredReadOnly。
            // 1フレームに1回しか書かないためMaxUpdatesPerFrameは既定のままでよい
            RHI::BufferDesc instanceDesc;
            instanceDesc.Usage = RHI::BufferUsage::StructuredReadOnly;
            instanceDesc.SizeInBytes = static_cast<uint32_t>(sizeof(GpuModelCullInstance)) * capacity;
            instanceDesc.StrideInBytes = static_cast<uint32_t>(sizeof(GpuModelCullInstance));
            instanceDesc.MaxUpdatesPerFrame = 1;
            auto instanceBuffer = m_Device->CreateBuffer(instanceDesc);

            // 生き残りの DispatchMesh 引数。そのままExecuteIndirectへ渡すのでIndirectArgs。
            //
            // 【区画ごとに配列を分ける】PSOはExecuteIndirectの引数では切り替えられないため、
            // ミラーリングの有無・プリパスの不透明/カットアウトを別の配列へ詰め、
            // PSOごとに1回ずつ発行する。
            // 先頭のkModelCullArgsBaseOffsetバイトは区画ごとの発行数(uint)が占める
            RHI::BufferDesc drawArgsDesc;
            drawArgsDesc.Usage = RHI::BufferUsage::IndirectArgs;
            drawArgsDesc.SizeInBytes =
                kModelCullArgsBaseOffset + ComputeModelCullRegionStride(capacity) * kModelCullRegionCount;
            drawArgsDesc.StrideInBytes = RHI::IRHICommandList::kDispatchMeshIndirectArgStride;
            auto drawArgsBuffer = m_Device->CreateBuffer(drawArgsDesc);

            // 【作り終えてから差し替える】途中で例外が出たときに、古いバッファを
            // 手放した状態で戻ってしまうのを避ける
            m_ModelCullInstanceBuffer = std::move(instanceBuffer);
            m_ModelCullDrawArgsBuffer = std::move(drawArgsBuffer);
            m_ModelCullCapacity = capacity;
            m_ModelCullRegionStride = ComputeModelCullRegionStride(capacity);
        }
        catch (const std::exception& e)
        {
            Core::Logger::Warning(
                "KurenaiEngine3D",
                std::string("モデル単位のGPUカリングのバッファを作れませんでした(この機能を止めます): ") + e.what());
            m_ModelCullInstanceBuffer.reset();
            m_ModelCullDrawArgsBuffer.reset();
            m_ModelCullCapacity = 0;
            m_ModelCullRegionStride = 0;
        }
    }

    bool KurenaiEngine3D::IssueModelCullIndirect(
        RHI::IRHICommandList* cmd, uint32_t region, RHI::IRHIPipelineState* pipelineState,
        RHI::IRHIPipelineState*& currentPipelineState)
    {
        if (!cmd || region >= kModelCullRegionCount || !pipelineState || !m_ModelCullDrawArgsBuffer)
        {
            return false;
        }
        // GPUが書く発行数の上限。候補が1件も無い区画はExecuteIndirectごと省く
        const uint32_t maxCommandCount = m_ModelCullRegionCandidates[region];
        if (maxCommandCount == 0)
        {
            return false;
        }

        if (pipelineState != currentPipelineState)
        {
            cmd->SetPipelineState(pipelineState);
            cmd->SetConstantBuffer(0, m_FrameConstantBuffer.get());
            cmd->SetSamplerSet(m_MaterialSamplers.get());
            currentPipelineState = pipelineState;
        }

        // 【b1(ObjectConstants)はここでは張らない】コマンドシグネチャがドローごとに
        // 差し替える。ここで張っても最初のドローで上書きされるだけで、意味が無いどころか
        // 「張ってあるから大丈夫」という誤解の元になる
        cmd->DispatchMeshIndirect(
            m_ModelCullDrawArgsBuffer.get(),
            kModelCullArgsBaseOffset + region * m_ModelCullRegionStride,
            maxCommandCount,
            region * static_cast<uint32_t>(sizeof(uint32_t)));
        return true;
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
            return m_RTAOTexture.get();
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
            return m_RTAORawTexture.get();
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
            // ジオメトリパス(G-Buffer書き込み)
            RHI::PipelineStateDesc gbufferPipelineDesc;
            gbufferPipelineDesc.InputLayout = GetModelInputLayout();
            gbufferPipelineDesc.VertexShader = m_GBufferVertexShader.get();
            gbufferPipelineDesc.PixelShader = m_GBufferPixelShader.get();
            gbufferPipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
            gbufferPipelineDesc.RenderTargetFormats =
            {
                RHI::Format::R8G8B8A8_UNorm, // Albedo
                RHI::Format::R16G16_Float,   // Normal(オクタヘドラルエンコード)
                RHI::Format::R8G8B8A8_UNorm, // Material(R=Metallic, G=Roughness)
                emissiveFormat,              // Emissive(バッファ精度に依存)
                RHI::Format::R16G16_Float,   // Velocity(モーションベクター。UV単位の2Dベクトル)
                RHI::Format::R16G16B16A16_Float, // BentNormal(.rgb = bRaw、.a = 有効フラグ)
            };
            gbufferPipelineDesc.HasDepthStencil = true;
            gbufferPipelineDesc.ReverseZ = true;
            // 深度プリパス(41.22節)を通したとき、プリパスが書いた深度と同じ値になる最前面の
            // 断片だけを通すため、比較をGREATER_EQUALへ緩める。プリパスを切っていても
            // 不透明G-Bufferでは絵が変わらない(理由はRHIDesc.hのDepthAllowEqualのコメント)ので、
            // 有効/無効でPSOを2組に増やさず常にこちらにしてある
            gbufferPipelineDesc.DepthAllowEqual = true;
            m_GBufferPipelineState = m_Device->CreatePipelineState(gbufferPipelineDesc);

            // ミラーリングされたインスタンス用に、表裏判定だけを入れ替えた同じパイプラインを用意する。
            // DX12はラスタライザステートがPSOに焼き込まれ描画中に差し替えられないため、DX11/DX12で
            // 同じ構成にできるよう両バックエンドともPSOを2本持つ方式にしている
            gbufferPipelineDesc.FrontCounterClockwise = true;
            m_GBufferPipelineStateMirrored = m_Device->CreatePipelineState(gbufferPipelineDesc);

            // 水面(ModelInstance::IsWater)用。頂点シェーダー・入力レイアウト・レンダーターゲット
            // フォーマットは通常のG-Bufferとまったく同じで、ピクセルシェーダーだけをWater.hlslへ
            // 差し替える。ミラーリングとの組み合わせも通常PSOと同じ方式で2本持つ
            gbufferPipelineDesc.FrontCounterClockwise = false;
            gbufferPipelineDesc.PixelShader = m_GBufferWaterPixelShader.get();
            m_GBufferWaterPipelineState = m_Device->CreatePipelineState(gbufferPipelineDesc);
            gbufferPipelineDesc.FrontCounterClockwise = true;
            m_GBufferWaterPipelineStateMirrored = m_Device->CreatePipelineState(gbufferPipelineDesc);

            // メッシュシェーダー版のG-Bufferパス(GBufferMeshlet.hlsl)。
            // 入力レイアウトを持たない以外は上の通常PSOと同じ設定にする ―― ラスタライザ・
            // 深度・レンダーターゲットのどれか1つでもずれると、メッシュレットのON/OFFで
            // 見た目が変わってしまい「切り替えても一致するはず」という検証が成立しなくなる。
            //
            // 非対応環境ではCreateMeshPipelineStateがnullptrを返す。ポインタが空なら
            // 描画側が従来経路を使うため、ここで分岐して作成をスキップする必要はない
            if (m_Device->SupportsMeshShader() && m_GBufferMeshShader && m_GBufferAmplificationShader)
            {
                RHI::MeshPipelineStateDesc meshPipelineDesc;
                meshPipelineDesc.AmplificationShader = m_GBufferAmplificationShader.get();
                meshPipelineDesc.MeshShader = m_GBufferMeshShader.get();
                meshPipelineDesc.PixelShader = m_GBufferPixelShader.get();
                meshPipelineDesc.RenderTargetFormats = gbufferPipelineDesc.RenderTargetFormats;
                meshPipelineDesc.HasDepthStencil = true;
                meshPipelineDesc.ReverseZ = true;
                // 頂点シェーダー版と1つでもずれると切り替えで見た目が変わるため、深度比較も揃える
                meshPipelineDesc.DepthAllowEqual = true;
                meshPipelineDesc.FrontCounterClockwise = false;
                m_GBufferMeshletPipelineState = m_Device->CreateMeshPipelineState(meshPipelineDesc);

                meshPipelineDesc.FrontCounterClockwise = true;
                m_GBufferMeshletPipelineStateMirrored = m_Device->CreateMeshPipelineState(meshPipelineDesc);

                // メッシュレットの分かれ方を色で確かめるデバッグ表示用。
                // ピクセルシェーダーだけを差し替えた同じパイプライン
                if (m_GBufferMeshletDebugPixelShader)
                {
                    meshPipelineDesc.PixelShader = m_GBufferMeshletDebugPixelShader.get();
                    meshPipelineDesc.FrontCounterClockwise = false;
                    m_GBufferMeshletDebugPipelineState = m_Device->CreateMeshPipelineState(meshPipelineDesc);
                    meshPipelineDesc.FrontCounterClockwise = true;
                    m_GBufferMeshletDebugPipelineStateMirrored = m_Device->CreateMeshPipelineState(meshPipelineDesc);
                }
            }

            // 深度プリパス(41.22節)。G-Bufferとまったく同じ頂点シェーダー・入力レイアウトで
            // 深度だけを書く。レンダーターゲットは持たず、不透明マテリアル用は
            // ピクセルシェーダーそのものを持たない(段ごと省く)。
            //
            // 【頂点シェーダーを共有する理由】プリパスとG-Bufferで頂点の変換結果が
            // 1ulpでも違うと、深度が一致せずGREATER_EQUALのテストを通らなくなり、
            // その面がまるごと消える。別のシェーダーに写すと最適化の差で容易にずれる
            RHI::PipelineStateDesc depthPrepassPipelineDesc;
            depthPrepassPipelineDesc.InputLayout = GetModelInputLayout();
            depthPrepassPipelineDesc.VertexShader = m_GBufferVertexShader.get();
            depthPrepassPipelineDesc.PixelShader = nullptr;
            depthPrepassPipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
            depthPrepassPipelineDesc.HasDepthStencil = true;
            depthPrepassPipelineDesc.ReverseZ = true;
            m_DepthPrepassPipelineState = m_Device->CreatePipelineState(depthPrepassPipelineDesc);
            depthPrepassPipelineDesc.FrontCounterClockwise = true;
            m_DepthPrepassPipelineStateMirrored = m_Device->CreatePipelineState(depthPrepassPipelineDesc);

            // アルファカットアウト(glTFのalphaMode=MASK)用。切り抜かれる部分の深度まで
            // 書いてしまうとG-Buffer側のclipと食い違って穴が開くため、こちらだけ
            // 同じ判定のclipを持つピクセルシェーダーを通す(DepthPrepass.hlsl)
            if (m_DepthPrepassCutoutPixelShader)
            {
                depthPrepassPipelineDesc.PixelShader = m_DepthPrepassCutoutPixelShader.get();
                depthPrepassPipelineDesc.FrontCounterClockwise = false;
                m_DepthPrepassCutoutPipelineState = m_Device->CreatePipelineState(depthPrepassPipelineDesc);
                depthPrepassPipelineDesc.FrontCounterClockwise = true;
                m_DepthPrepassCutoutPipelineStateMirrored = m_Device->CreatePipelineState(depthPrepassPipelineDesc);
            }

            // メッシュシェーダー版の深度プリパス。
            //
            // 【これが無いとプリパスがまるごと止まる】かつてプリパスはメッシュレット経路と
            // 排他だった。プリパスが頂点シェーダーで深度を書き、G-Bufferがメッシュシェーダーで
            // 描くと、同じ頂点でも変換の丸めが一致する保証が無く、深度が1ulpずれた面が
            // GREATER_EQUALを通らずに消えるため。**G-Bufferと同じ増幅/メッシュシェーダーを
            // そのまま使えば変換は文字どおり同一のコードになり、この問題自体が消える。**
            //
            // 不透明用はピクセルシェーダーを持たない(段ごと省く)。カットアウト用は
            // G-Bufferとまったく同じ判定のclipを通す(DepthPrepass.hlsl)
            if (m_GBufferMeshShader && m_GBufferAmplificationShader)
            {
                RHI::MeshPipelineStateDesc prepassMeshDesc;
                prepassMeshDesc.AmplificationShader = m_GBufferAmplificationShader.get();
                prepassMeshDesc.MeshShader = m_GBufferMeshShader.get();
                prepassMeshDesc.PixelShader = nullptr;
                prepassMeshDesc.HasDepthStencil = true;
                prepassMeshDesc.ReverseZ = true;
                prepassMeshDesc.FrontCounterClockwise = false;
                m_DepthPrepassMeshletPipelineState = m_Device->CreateMeshPipelineState(prepassMeshDesc);
                prepassMeshDesc.FrontCounterClockwise = true;
                m_DepthPrepassMeshletPipelineStateMirrored = m_Device->CreateMeshPipelineState(prepassMeshDesc);

                if (m_DepthPrepassCutoutPixelShader)
                {
                    prepassMeshDesc.PixelShader = m_DepthPrepassCutoutPixelShader.get();
                    prepassMeshDesc.FrontCounterClockwise = false;
                    m_DepthPrepassMeshletCutoutPipelineState = m_Device->CreateMeshPipelineState(prepassMeshDesc);
                    prepassMeshDesc.FrontCounterClockwise = true;
                    m_DepthPrepassMeshletCutoutPipelineStateMirrored =
                        m_Device->CreateMeshPipelineState(prepassMeshDesc);
                }
            }

            // SSAOパス
            RHI::PipelineStateDesc ssaoPipelineDesc;
            ssaoPipelineDesc.VertexShader = m_AOVertexShader.get();
            ssaoPipelineDesc.PixelShader = m_SSAOPixelShader.get();
            ssaoPipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
            ssaoPipelineDesc.RenderTargetFormats = { aoFormat };
            m_SSAOPipelineState = m_Device->CreatePipelineState(ssaoPipelineDesc);

            // SSILパス(Visibility Bitmask)
            RHI::PipelineStateDesc ssilPipelineDesc;
            ssilPipelineDesc.VertexShader = m_AOVertexShader.get();
            ssilPipelineDesc.PixelShader = m_SSILPixelShader.get();
            ssilPipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
            ssilPipelineDesc.RenderTargetFormats = { aoFormat };
            m_SSILPipelineState = m_Device->CreatePipelineState(ssilPipelineDesc);

            // AO/GI共通のブラーパス(SSAO/SSILのどちらの出力にも同じフォーマットで書き戻す)
            RHI::PipelineStateDesc aoBlurPipelineDesc;
            aoBlurPipelineDesc.VertexShader = m_AOVertexShader.get();
            aoBlurPipelineDesc.PixelShader = m_AOBlurPixelShader.get();
            aoBlurPipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
            aoBlurPipelineDesc.RenderTargetFormats = { aoFormat };
            m_AOBlurPipelineState = m_Device->CreatePipelineState(aoBlurPipelineDesc);
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
        return useProcedural ? m_ProceduralSkyTexture.get() : m_SkyboxTexture.get();
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
            m_AerialPerspectiveTexture = m_Device->CreateRenderTexture(width, height, RHI::Format::R16G16B16A16_Float);
            // 雲パスの出力(rgb=事前乗算済みの散乱光、a=透過率)。内部レンダー解像度の1/2で持つ。
            // 【R16G16B16A16_Float固定にする理由】平面反射(CreatePlanarReflectionTargets)と同じで、
            // 散乱光はHDRの輝度をそのまま持つためLegacy8bitでは飽和して雲が白く潰れる。
            // また透過率は乗算に使うので8bitの量子化がそのままバンディングになる
            m_SkyCloudWidth = std::max(1u, width / 2);
            m_SkyCloudHeight = std::max(1u, height / 2);
            m_SkyCloudTexture =
                m_Device->CreateRenderTexture(m_SkyCloudWidth, m_SkyCloudHeight, RHI::Format::R16G16B16A16_Float);
            // 上のパスが同時に書く fogInFront(雲に最初に当たった位置の霞の透過率、P18b)。
            // 合成側(DeferredLighting.hlsl)が clearColor * (CloudSkyLight - 1) * (1 - fogInFront) を
            // フル解像度で掛けるためだけに要る。1チャンネルなのでDDGIResolveの低解像度深度と
            // 同じR32_Floatにする。
            // 【t19/t21と同じ理由で常に確保する】雲パスが登録されないフレーム(DDSスカイボックス)でも
            // t22を空のままにできない(DX12のディスクリプタテーブルを埋め切るため)
            m_SkyCloudFogTexture =
                m_Device->CreateRenderTexture(m_SkyCloudWidth, m_SkyCloudHeight, RHI::Format::R32_Float);
            // DDGIの低解像度解決パスの出力(rgb=イラディアンス、a=insideWeight)。雲と同じく1/2解像度。
            // 【常に確保する】m_DDGISettings.HalfResolutionが無効でもシェーダーのt19には何かを
            // バインドしておく必要がある(DX12のディスクリプタテーブルを埋め切るため)。
            // フォーマットを雲と揃えているのも同じ理由 ―― イラディアンスはHDRの物理量で、
            // 8bitでは飽和と量子化がそのまま間接光のバンディングになる
            m_DDGIResolveWidth = std::max(1u, width / 2);
            m_DDGIResolveHeight = std::max(1u, height / 2);
            m_GIResources.DDGIResolveTexture = m_Device->CreateRenderTexture(
                m_DDGIResolveWidth, m_DDGIResolveHeight, RHI::Format::R16G16B16A16_Float);
            // 上のパスが同時に書く「そのテクセルが代表している全解像度の深度」(41.24節)。
            // 合成側(DeferredLighting.hlsl)がGatherRed 1回で4テクセルぶんを取るためのもので、
            // t19と同じ理由で常に確保する(t21を空のままにできない)
            m_GIResources.DDGIResolveDepthTexture = m_Device->CreateRenderTexture(
                m_DDGIResolveWidth, m_DDGIResolveHeight, RHI::Format::R32_Float);
            // RT反射はコンピュートシェーダーがUAVで書くため、レンダーターゲットではなくUAVテクスチャを作る。
            // 非対応環境ではパス自体が実行されないので確保しない
            if (m_RenderCapabilities.RaytracingAvailable)
            {
                m_RenderTargets.CreateRTReflection(*m_Device, width, height);
                m_RenderTargets.CreateRTShadow(*m_Device, width, height);
                // RTAOの生バッファはコンピュートがUAVで書くためUAVテクスチャ、ブラー後は
                // 従来どおりピクセルシェーダーが書くレンダーターゲット。
                // フォーマットはSSAO/SSILと同じaoFormat(バッファ精度の設定に追従する)
                m_RTAORawTexture = m_Device->CreateUAVTexture(width, height, aoFormat);
                m_RTAOTexture = m_Device->CreateRenderTexture(width, height, aoFormat);
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

            m_HiZMipLevels = ComputeMipLevelCount(width, height);
            m_RenderTargets.CreateHiZ(*m_Device, width, height, m_HiZMipLevels);
            m_DebugViewSettings.HiZDebugMipLevel = 0;
            // 作り直した直後の中身は未定義。Hi-Zパスが1回走るまでオクルージョン判定を止める
            m_HiZValid = false;

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
                RHI::BufferDesc reservoirBufferDesc;
                reservoirBufferDesc.Usage = RHI::BufferUsage::StructuredRW;
                reservoirBufferDesc.SizeInBytes = static_cast<uint32_t>(sizeof(uint32_t) * 4) * width * height *
                                                  static_cast<uint32_t>(m_MegaLightsAllocatedSamplesPerPixel);
                reservoirBufferDesc.StrideInBytes = static_cast<uint32_t>(sizeof(uint32_t) * 4);
                m_MegaLightsReservoirBuffer = m_Device->CreateBuffer(reservoirBufferDesc);

                // 画素ごとの「遮蔽が確定した灯」のキャッシュ(uint。0xFFFFFFFFで無し)。
                // 殺しの持ち回りより寿命が長く、影の縁の暗いフリンジを消すのに要る
                // (MegaLightsInitialSample.hlsl の BlockedLights のコメント)
                RHI::BufferDesc blockedBufferDesc;
                blockedBufferDesc.Usage = RHI::BufferUsage::StructuredRW;
                blockedBufferDesc.SizeInBytes = static_cast<uint32_t>(sizeof(uint32_t)) * width * height;
                blockedBufferDesc.StrideInBytes = static_cast<uint32_t>(sizeof(uint32_t));
                m_MegaLightsBlockedLightBuffer = m_Device->CreateBuffer(blockedBufferDesc);
                // 空間再利用の出力先。近傍を読むので入力と同じバッファへは書けない。
                // 2回以上回すときは2本を ping-pong する
                m_MegaLightsReservoirSpatialBuffer = m_Device->CreateBuffer(reservoirBufferDesc);
                m_MegaLightsReservoirSpatialBuffer2 = m_Device->CreateBuffer(reservoirBufferDesc);

                // 時間再利用の履歴。**2本のping-pongにするのは、RenderGraphがWARの辺を
                // 張らないため**。1本で済ませると「今フレームのTemporalが読んだ直後に
                // 同じバッファへ書く」形になり、条件分岐でパスが1つ消えた瞬間に静かに壊れる。
                // 2本なら全ての辺がRAWで張れる(前フレームが書いた側を読み、今フレームは
                // もう片方へ書く)
                for (auto& buffer : m_MegaLightsReservoirHistory)
                {
                    buffer = m_Device->CreateBuffer(reservoirBufferDesc);
                }

                // 履歴の幾何(前フレームの法線・線形深度・材質)。
                // 【なぜ専用に持つのか】G-Bufferは毎フレーム上書きされ、前フレームの写しは
                // どこにも残らない。再投影先が「同じ面か」を判定するには前フレームの幾何が要る。
                // 1画素12バイト(法線oct 4 + View空間Z 4 + 材質 4)。
                // MegaLightsCommon.hlsli の MegaLightsHistoryGuide とストライドを一致させること
                RHI::BufferDesc guideBufferDesc;
                guideBufferDesc.Usage = RHI::BufferUsage::StructuredRW;
                guideBufferDesc.SizeInBytes = static_cast<uint32_t>(sizeof(uint32_t) * 3) * width * height;
                guideBufferDesc.StrideInBytes = static_cast<uint32_t>(sizeof(uint32_t) * 3);
                for (auto& buffer : m_MegaLightsHistoryGuide)
                {
                    buffer = m_Device->CreateBuffer(guideBufferDesc);
                }
                // 【履歴を無効にする】解像度が変わると添字の意味が変わり、前フレームの内容は
                // 別の画素のものになる。RHIにバッファのクリアが無いので、初回は
                // シェーダ側で「履歴を使わない」と判断させる
                m_MegaLightsHistoryValid = false;
                // デノイザの作業用テクスチャ。整数フォーマットが無いRHIなのですべてfloat。
                // 【履歴もping-pongにする】RenderGraphはWARの辺を張らないので、
                // 読む側と書く側が同じだと条件分岐でパスが消えた瞬間に静かに壊れる
                for (int denoiseIndex = 0; denoiseIndex < 2; ++denoiseIndex)
                {
                    m_MegaLightsDenoiseHistory[denoiseIndex] =
                        m_Device->CreateUAVTexture(width, height, RHI::Format::R32G32B32A32_Float);
                    m_MegaLightsDenoiseMoments[denoiseIndex] =
                        m_Device->CreateUAVTexture(width, height, RHI::Format::R32G32B32A32_Float);
                    m_MegaLightsDenoisePing[denoiseIndex] =
                        m_Device->CreateUAVTexture(width, height, RHI::Format::R32G32B32A32_Float);
                    m_MegaLightsDenoiseMomentPing[denoiseIndex] =
                        m_Device->CreateUAVTexture(width, height, RHI::Format::R32G32B32A32_Float);
                }
                m_RenderTargets.CreateMegaLightsDenoised(*m_Device, width, height);
                // 解像度が変わると履歴の添字の意味が変わる。バッファのクリアが無いRHIなので、
                // シェーダ側へ「履歴を読むな」と伝える
                m_MegaLightsDenoiseHistoryValid = false;
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
            m_MegaLightsAccumFrames = 0;
            m_MegaLightsAccumWarmupFrames = 0;
            m_MegaLightsDumpIssued = false;
            m_MegaLightsDumpDone = false;
            m_MegaLightsDumpCopyFrame = 0;
            m_MegaLightsAccumReadback.reset();
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
                    // visibility buffer。画素あたり64bit(上位32bit=深度、下位32bit=三角形番号)。
                    // CSRasterがUAVで書き、CSResolveがSRVで読むためStructuredRW
                    RHI::BufferDesc visibilityDesc;
                    visibilityDesc.Usage = RHI::BufferUsage::StructuredRW;
                    visibilityDesc.SizeInBytes = static_cast<uint32_t>(sizeof(uint64_t)) * width * height;
                    visibilityDesc.StrideInBytes = static_cast<uint32_t>(sizeof(uint64_t));
                    m_SoftwareRasterVisibilityBuffer = m_Device->CreateBuffer(visibilityDesc);

                    m_RenderTargets.CreateSoftwareRasterOutputs(*m_Device, width, height);
                }
                catch (const std::exception& e)
                {
                    Core::Logger::Error(
                        "KurenaiEngine3D",
                        std::string("ソフトウェアラスタライザのリソース作成に失敗したため無効にします (") +
                            std::to_string(width) + "x" + std::to_string(height) + "): " + e.what());
                    m_RenderCapabilities.SoftwareRasterAvailable = false;
                    m_SoftwareRasterVisibilityBuffer.reset();
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

    void KurenaiEngine3D::ExecuteSoftwareRasterPass(
        RHI::IRHICommandList* cmd,
        const DirectX::XMMATRIX& viewProj,
        const DirectX::XMFLOAT3& sunDirection)
    {
        // --- メッシュレコードを組み直す ---------------------------------------------------
        //
        // 描画用の頂点/インデックスバッファはbindlessで直接引けるので(ModelLoader参照)、
        // ここで作るのは「どのメッシュがどのbindless番号を持ち、通し三角形番号のどこから
        // 始まるか」の表だけ。数百件のオーダーなので毎フレーム組み直して構わない
        std::vector<SWRasterMeshInfo> meshInfos;
        meshInfos.reserve(64);

        uint32_t firstTriangle = 0;
        bool overflowed = false;

        const Rendering::FrustumPlanes swRasterFrustum = ExtractFrustumPlanes(viewProj);

        // 【このパスはクロスディザ非対応】なのでフェード中でも段は1つに決め打つ。
        // 【バッチは使わない】ここで作るのはドローではなくメッシュの表なので、
        // まとめる意味が無い
        GeometryDrawLoopDesc swRasterLoop;
        swRasterLoop.Frustum = &swRasterFrustum;
        swRasterLoop.UseDrawUnits = false;
        swRasterLoop.LODMode = GeometryLODMode::Current;
        // 半透明(alphaMode=BLEND)はハードウェア側でもG-Bufferに描かれないため揃える
        swRasterLoop.MeshFilter = GeometryMeshFilter::Opaque;
        // 【メッシュ単位のカリングは共通ループに任せない】このパスは三角形が3つ未満の
        // メッシュも落とすので、判定の順序が変わると分母がずれる。原文どおり
        // 「描かないメッシュを弾いた後」に自分で呼ぶ
        swRasterLoop.MeshCulling = false;

        ForEachGeometryDraw(
            swRasterLoop,
            [](const InstanceDrawUnit&, const Assets::Model&, float) { return false; },
            [&](const InstanceDrawUnit& unit, const Assets::Model& currentModel,
                const Assets::Mesh& mesh, float)
            {
                const Assets::ModelInstance& instance = *unit.Instance;
                if (mesh.IndexCount < 3)
                {
                    return true;
                }

                // メッシュ単位のカリング。統計はモデル単位とは別カウンタへ入れる。
                // 【描かないメッシュを弾いた後に置く】分母を「このパスが実際に描くメッシュ」に
                // 揃えないと、間引き率が薄まって効きが読めなくなる
                if (!Rendering::IsMeshVisibleWithStats(
                        m_GeometrySettings.MeshCullingEnabled, swRasterFrustum, instance,
                        currentModel, mesh, m_MeshCullTested, m_MeshCullCulled))
                {
                    return true;
                }

                const uint32_t vertexBufferIndex =
                    mesh.VertexBuffer ? mesh.VertexBuffer->GetBindlessIndex() : RHI::kInvalidBindlessIndex;
                const uint32_t indexBufferIndex =
                    mesh.IndexBuffer ? mesh.IndexBuffer->GetBindlessIndex() : RHI::kInvalidBindlessIndex;
                // bindless登録が無いメッシュ(ShaderReadableを指定せずに作られた等)は引けない。
                // シェーダー側で無効番号を判定する手段が無いため、ここで落とす
                if (vertexBufferIndex == RHI::kInvalidBindlessIndex ||
                    indexBufferIndex == RHI::kInvalidBindlessIndex)
                {
                    return true;
                }

                if (meshInfos.size() >= kSWRasterMaxMeshes)
                {
                    // 表があふれた。**列挙そのものを打ち切る**(偽を返す)
                    overflowed = true;
                    return false;
                }

                SWRasterMeshInfo info{};
                info.World = instance.World;
                info.NormalMatrix = instance.NormalMatrix;
                info.VertexBufferIndex = vertexBufferIndex;
                info.IndexBufferIndex = indexBufferIndex;
                info.FirstTriangle = firstTriangle;
                info.TriangleCount = mesh.IndexCount / 3;
                // ミラーリングされたインスタンスはワインディングが反転する。ハードウェア側が
                // FrontCounterClockwise=trueの別PSOで描いているのと同じ対処をしないと、
                // 鏡像配置のモデルだけ表裏が入れ替わって消える
                info.FrontFaceSign = instance.IsMirrored ? -1.0f : 1.0f;
                info.Flags = 0;

                firstTriangle += info.TriangleCount;
                meshInfos.push_back(info);
                return true;
            });

        if (overflowed && !m_SoftwareRasterMeshOverflowLogged)
        {
            // 毎フレーム出続けるのを避けるため最初の1回だけ報告する(m_LightTileOverflowLoggedと同じ作法)
            m_SoftwareRasterMeshOverflowLogged = true;
            Core::Logger::Warning(
                "KurenaiEngine3D",
                "ソフトウェアラスタライザのメッシュ数が上限(" + std::to_string(kSWRasterMaxMeshes) +
                    ")を超えました。超過分は描画されません");
        }

        if (meshInfos.empty())
        {
            // 描くものが1つも無くても、visibility bufferは必ずクリアしてから戻る。
            //
            // 【クリアせずに戻ってはいけない】このバッファは散布書き込みで、三角形が当たらなかった
            // 画素には前フレームの値が残る(下の「0. クリア」のコメント参照)。カリングで全インスタンスが
            // 落ちたフレームだけ前フレームの絵が焼き付いて残る、という形で出る
            cmd->ClearUnorderedAccessBufferUint(m_SoftwareRasterVisibilityBuffer.get(), 0);
            cmd->ClearUnorderedAccessBufferUint(m_SoftwareRasterIndirectArgsBuffer.get(), 0);
            return;
        }

        cmd->UpdateBuffer(
            m_SoftwareRasterMeshInfoBuffer.get(),
            meshInfos.data(),
            meshInfos.size() * sizeof(SWRasterMeshInfo));

        // --- 定数バッファ -----------------------------------------------------------------

        const uint32_t totalTriangles = firstTriangle;

        // Dispatchの1次元あたりの上限は65535。三角形数はシーン読み込み時に確定する静的な値なので
        // CPUが持てばよく、ここを間接ディスパッチにする理由は無い(巨大三角形の個数と違って
        // GPU上でしか分からない値ではない)
        const uint32_t groupsTotal = (totalTriangles + kSWRasterGroupSize - 1) / kSWRasterGroupSize;
        const uint32_t groupsX = std::min(groupsTotal, kSWRasterMaxGroupsPerAxis);
        const uint32_t groupsY = (groupsTotal + groupsX - 1) / groupsX;

        SWRasterConstants constants{};
        DirectX::XMStoreFloat4x4(&constants.ViewProj, DirectX::XMMatrixTranspose(viewProj));
        constants.RenderSize = {
            static_cast<float>(m_RenderWidth),
            static_cast<float>(m_RenderHeight),
            1.0f / static_cast<float>(m_RenderWidth),
            1.0f / static_cast<float>(m_RenderHeight),
        };
        constants.SunDirection = { sunDirection.x, sunDirection.y, sunDirection.z, 0.0f };
        constants.DispatchParams = {
            groupsX,
            totalTriangles,
            static_cast<uint32_t>(meshInfos.size()),
            static_cast<uint32_t>(std::clamp(
                m_GeometrySettings.SoftwareRasterLargeTriangleArea,
                static_cast<int>(GeometrySettings::kSWRasterMinLargeTriangleArea),
                static_cast<int>(GeometrySettings::kSWRasterMaxLargeTriangleArea))),
        };
        constants.LargeParams = { kSWRasterLargeListCapacity, 0u, 0u, 0u };

        cmd->UpdateBuffer(m_SoftwareRasterConstantBuffer.get(), &constants, sizeof(constants));

        // --- 0. クリア --------------------------------------------------------------------
        //
        // visibility bufferは散布書き込みなので、三角形が当たらなかった画素には前フレームの値が
        // 残る。0は「深度0 = 遠平面 = 当たり無し」を意味する(SWRasterPackVisibility参照)。
        // 間接ディスパッチ引数も、X成分をカウンタとして使うため毎フレーム0へ戻す必要がある
        cmd->ClearUnorderedAccessBufferUint(m_SoftwareRasterVisibilityBuffer.get(), 0);
        cmd->ClearUnorderedAccessBufferUint(m_SoftwareRasterIndirectArgsBuffer.get(), 0);

        // --- 1. CSRaster: 1スレッド = 1三角形 ---------------------------------------------
        //
        // 【UAVはディスパッチごとに張り直す】Dispatch直後に全スロットが自動解除されるため
        // (IRHICommandList::SetComputeUnorderedAccessTextureのコメント)
        cmd->SetComputePipelineState(m_SoftwareRasterPipelineState.get());
        cmd->SetComputeConstantBuffer(1, m_SoftwareRasterConstantBuffer.get());
        cmd->SetComputeShaderResourceBuffer(0, m_SoftwareRasterMeshInfoBuffer.get());
        cmd->SetComputeUnorderedAccessBuffer(0, m_SoftwareRasterVisibilityBuffer.get());
        cmd->SetComputeUnorderedAccessBuffer(1, m_SoftwareRasterLargeEntriesBuffer.get());
        cmd->SetComputeUnorderedAccessBuffer(2, m_SoftwareRasterIndirectArgsBuffer.get());
        cmd->Dispatch(groupsX, groupsY, 1);

        // --- 2. CSRasterLarge: 1スレッドグループ = 巨大三角形1個 --------------------------
        //
        // 巨大三角形の個数はGPU上でしか分からないため、グループ数をCPUから書けない。
        // これが間接ディスパッチをRHIへ足した理由。
        // 【引数バッファをUAVに張らない】DispatchIndirectは引数バッファを
        // INDIRECT_ARGUMENT状態へ遷移させるので、同じディスパッチのUAVスロットに
        // 張ったままにはできない(DX12CommandList::DispatchIndirectのコメント)
        cmd->SetComputePipelineState(m_SoftwareRasterLargePipelineState.get());
        cmd->SetComputeConstantBuffer(1, m_SoftwareRasterConstantBuffer.get());
        cmd->SetComputeShaderResourceBuffer(0, m_SoftwareRasterMeshInfoBuffer.get());
        cmd->SetComputeShaderResourceBuffer(1, m_SoftwareRasterLargeEntriesBuffer.get());
        cmd->SetComputeUnorderedAccessBuffer(0, m_SoftwareRasterVisibilityBuffer.get());
        cmd->DispatchIndirect(m_SoftwareRasterIndirectArgsBuffer.get(), 0);

        // --- 3. CSResolve: 1スレッド = 1画素 ----------------------------------------------
        //
        // visibility bufferの三角形番号からジオメトリを引き直し、深度・法線・陰影を書く
        constexpr uint32_t kResolveGroupSize = ShaderInterop::kSWRasterResolveGroupSize;
        cmd->SetComputePipelineState(m_SoftwareRasterResolvePipelineState.get());
        cmd->SetComputeConstantBuffer(1, m_SoftwareRasterConstantBuffer.get());
        cmd->SetComputeShaderResourceBuffer(0, m_SoftwareRasterMeshInfoBuffer.get());
        cmd->SetComputeShaderResourceBuffer(1, m_SoftwareRasterVisibilityBuffer.get());
        cmd->SetComputeUnorderedAccessTexture(0, m_RenderTargets.SoftwareRasterColor.get());
        cmd->SetComputeUnorderedAccessTexture(1, m_RenderTargets.SoftwareRasterDepth.get());
        cmd->SetComputeUnorderedAccessTexture(2, m_RenderTargets.SoftwareRasterNormal.get());
        cmd->SetComputeUnorderedAccessBuffer(3, m_SoftwareRasterIndirectArgsBuffer.get());
        cmd->Dispatch(
            (m_RenderWidth + kResolveGroupSize - 1) / kResolveGroupSize,
            (m_RenderHeight + kResolveGroupSize - 1) / kResolveGroupSize,
            1);
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
        // フラスタムカリングの統計はフレーム単位。ここで0に戻し、各描画パスが積み上げる。
        // モデル単位とメッシュ単位は別のカウンタで、混ぜない(KurenaiEngine3D.h参照)。
        //
        // 【0に戻す前に前フレームの値を控える】ドローコール数と同じ理由で、UIパネルは
        // Renderの外で描かれるため現在のカウンタを読むと必ずリセット直後の0になる
        m_RenderStats.FrustumCullTestedLastFrame = m_FrustumCullTested;
        m_RenderStats.FrustumCullCulledLastFrame = m_FrustumCullCulled;
        m_RenderStats.MeshCullTestedLastFrame = m_MeshCullTested;
        m_RenderStats.MeshCullCulledLastFrame = m_MeshCullCulled;
        m_FrustumCullTested = 0;
        m_FrustumCullCulled = 0;
        m_MeshCullTested = 0;
        m_MeshCullCulled = 0;
        // ドローコール数も同じくフレーム単位。各パスが自分のカウンタを積み上げる。
        //
        // 【0に戻す前に前フレームの値を控える】UIパネルはRenderの外で描かれるため、
        // 現在のカウンタを読むと必ずリセット直後の0になる(実際にそう表示されていた)。
        // 完成した最後のフレームの値を別に持たせる
        m_RenderStats.DrawCallsGBufferLastFrame = m_DrawCallsGBuffer;
        m_RenderStats.DrawCallsShadowLastFrame = m_ShadowPasses->GetDrawCalls();
        m_RenderStats.DrawCallsDepthPrepassLastFrame = m_DrawCallsDepthPrepass;
        m_DrawCallsGBuffer = 0;
        m_ShadowPasses->ResetDrawCalls();
        m_DrawCallsDepthPrepass = 0;
        // bindless区画の使用数を控える(UIパネルは m_Device へ直接触れないため。
        // m_RenderCapabilities.MeshShaderAvailable と同じ扱い)。登録はシーン読み込み時にしか起きないので、
        // フレームごとに1回問い合わせるだけで足りる
        m_RenderStats.BindlessUsedCount = m_Device ? m_Device->GetBindlessUsedCount() : 0;

        // WM_SIZE(Updateスレッド)が記録しておいたリサイズ要求を、スワップチェーンを実際に使う
        // このスレッドで反映する。このフレームのGPUコマンドをまだ1つも積んでいないこの位置で
        // 呼ぶこと(DX12SwapChain::Resizeは内部でWaitForGPUIdleを呼び、コマンドリストが
        // 記録待ちの状態であることを前提としているため)
        ApplyPendingResize();

        // ここでは要求だけを積み、実際の作り直しは従来どおり後段のUpdateSceneStreamingと
        // バッファ精度/解像度の作り直しブロックへ集約する。作り直しの契機を散らさないためこの位置に置く。
        ApplyScheduledRecreations();

        // Loaderスレッドが出来上がったシーンを置いていれば取り込み、保留中の切り替え要求があれば発注する。
        // 旧シーンの破棄(WaitForGPUIdleを伴う)もここで行うため、このフレームのGPUコマンドを
        // まだ1つも積んでいないこの位置で呼ぶこと
        UpdateSceneStreaming();

        // テクスチャの常駐ミップ差し替えを確定する。
        // **このフレームで最初にSetTextureを呼ぶより前でなければならない** ――
        // DX12CommandList::SetTextureは描画を記録するたびにテクスチャのSRVディスクリプタを
        // コピー元として読むため、記録が始まってから書き換えると読みながら書くことになる
        // (詳細はIRHIDevice::PrepareTextureContentsのコメント)
        m_TextureStreaming.CommitReady(*m_Device);

        if (m_Window->GetWidth() == 0 || m_Window->GetHeight() == 0)
        {
            return;
        }

        // WndProc(Updateスレッド)でキューイングされたメッセージを、ImGuiの状態を実際に読み書きする
        // このRenderスレッド自身からImGui_ImplWin32_WndProcHandlerへ転送する。ImGui::NewFrame()より前に
        // 行うことで、このフレームのNewFrame()が最新のマウス/キーボード状態を反映できる
        m_Window->ForwardQueuedMessagesToImGui();

        // モニタの拡大率に合わせてUIの大きさを揃える。ImGuiの状態を触るのはこのRenderスレッド
        // だけという不変条件を守るため、Window側は値をatomicへ置くだけにし、
        // 実際のスタイル再適用はここで行う
        m_UIManager->OnUIScaleChanged(m_Window->GetDpiScale() * UI::UITheme::kUIScaleMultiplier);

        m_ImGuiBackend->NewFrame();
        if (frameState.ImGuiVisible)
        {
            UI::PanelDrawContext panelContext;
            panelContext.Camera = &frameState.Camera;
            m_UIManager->Draw(panelContext);

            // オーサリングツール(Tools/KurenaiShowEditor)が足す追加のUI。
            // 【ImGuiVisibleの中に置くこと】F1で全パネルを隠したときに、これだけが
            // 画面に残ってしまうとスクリーンショットでの計測が壊れる
            if (m_ExtraImGuiCallback)
            {
                m_ExtraImGuiCallback();
            }
        }

        // ImGuiがマウス/キーボードを掴んでいるかをUpdateスレッドへ返す(Update()が読む)。
        // パネル非表示のときは掴んでいないので明示的にfalseを書く
        {
            const ImGuiIO& io = ImGui::GetIO();
            m_ImGuiWantCaptureKeyboard.store(
                frameState.ImGuiVisible && io.WantCaptureKeyboard, std::memory_order_relaxed);
            m_ImGuiWantCaptureMouse.store(frameState.ImGuiVisible && io.WantCaptureMouse, std::memory_order_relaxed);
        }

        // バッファ精度(デバッグ表示パネルのラジオボタン)と内部レンダー解像度(システムパネル)の
        // 切り替え要求をここで処理する。
        // レンダーターゲットを破棄する前に、DX12がまだ実行中かもしれない直前数フレームの
        // 描画コマンドを完了させる必要がある(LoadSceneがGPUリソースを破棄する前に
        // WaitForGPUIdleを呼ぶのと同じ理由)。このフレームのGPUコマンドはまだ1つも
        // 積んでいないため、ここで待っても待ち時間は前フレームぶんだけで済む。
        //
        // ここはApplyPendingResizeの後、かつこのフレームでm_RenderWidth/m_RenderHeightを
        // 読み始めるより前(最初の読み取りはTAAジッター)なので、解像度をまとめて差し替えてよい
        if (m_BufferPrecisionDirty || m_RenderResolutionDirty || m_PlanarReflectionResolutionDirty ||
            m_UpscaleTargetsDirty || m_MegaLightsReservoirDirty)
        {
            const bool precisionChanged = m_BufferPrecisionDirty;
            m_BufferPrecisionDirty = false;
            // MegaLightsのリザーババッファは CreateRenderTargets の中で作り直される。
            // 標本数の変更だけでもここを通す(GPUが参照していない状態が要るため)
            m_MegaLightsReservoirDirty = false;

            const uint32_t previousWidth = m_RenderWidth;
            const uint32_t previousHeight = m_RenderHeight;
            if (m_RenderResolutionDirty)
            {
                m_RenderResolutionDirty = false;
                m_RenderWidth = m_PendingRenderWidth;
                m_RenderHeight = m_PendingRenderHeight;
            }
            if (m_PlanarReflectionResolutionDirty)
            {
                m_PlanarReflectionResolutionDirty = false;
                m_ReflectionSettings.PlanarResolutionScale = m_PendingPlanarReflectionResolutionScale;
            }

            m_Device->WaitForGPUIdle();
            try
            {
                CreateRenderTargets(m_RenderWidth, m_RenderHeight);
                // 平面反射専用のレンダーターゲットも、呼び出し箇所をCreateRenderTargetsと
                // 揃えてここで作り直す(反射解像度の倍率変更だけの要求でもここを通る)
                CreatePlanarReflectionTargets();
            }
            catch (const std::exception& e)
            {
                // CreateRenderTargets自身がHDR→Legacy8bitのフォールバックを持つため、ここへ来るのは
                // 要求した解像度そのものが確保できない場合(高解像度でのVRAM不足など)。
                // 元の解像度へ戻して作り直す。それも失敗するなら復旧手段が無いのでそのまま送出する
                Core::Logger::Error(
                    "KurenaiEngine3D",
                    "内部レンダー解像度" + std::to_string(m_RenderWidth) + "x" + std::to_string(m_RenderHeight) +
                        "のレンダーターゲット作成に失敗したため、" + std::to_string(previousWidth) + "x" +
                        std::to_string(previousHeight) + "へ戻します: " + e.what());
                m_RenderWidth = previousWidth;
                m_RenderHeight = previousHeight;
                CreateRenderTargets(m_RenderWidth, m_RenderHeight);
                CreatePlanarReflectionTargets();
            }

            // 超解像の出力解像度用テクスチャ。内部解像度用とは作り直す契機が違うため
            // CreateRenderTargetsとは別に持っているが、GPUがそれらを参照していない状態で
            // 作り直す必要があるのは同じなので、上のWaitForGPUIdle()の後のここで行う
            if (m_UpscaleTargetsDirty)
            {
                m_UpscaleTargetsDirty = false;
                try
                {
                    CreateUpscaleTargets(m_PostProcessSettings.UpscaleOutputWidth, m_PostProcessSettings.UpscaleOutputHeight);
                }
                catch (const std::exception& e)
                {
                    // 確保できなければ超解像を諦めて等倍表示へ落とす。内部解像度は下がったままだが、
                    // Presentがバイリニアで拡大するので絵は出続ける(41.23節以前と同じ経路)
                    Core::Logger::Error(
                        "KurenaiEngine3D",
                        "超解像の出力解像度" + std::to_string(m_PostProcessSettings.UpscaleOutputWidth) + "x" +
                            std::to_string(m_PostProcessSettings.UpscaleOutputHeight) +
                            "のテクスチャ作成に失敗したため、超解像を無効にします: " + e.what());
                    m_PostProcessSettings.UpscaleEnabled = false;
                    m_RenderTargets.ResetUpscale();
                }
            }

            // カメラのアスペクト比はUpdateスレッドが読み取って反映する(m_RenderAspectの宣言参照)
            m_RenderAspect.store(
                static_cast<float>(m_RenderWidth) / static_cast<float>(m_RenderHeight), std::memory_order_relaxed);

            // PSOはレンダーターゲットのフォーマットだけに依存し解像度には依存しないため、
            // 作り直すのは精度が変わったときだけでよい。
            // G-Buffer(Emissive)とAO/GIのフォーマットが変わるため、それらへ描くPSOも作り直す。
            // 作り直さないとPSOが宣言するRenderTargetFormatsと実際のRTVがずれ、D3D12では
            // 仕様違反になる(DX11は検証しないため露見しない)
            if (precisionChanged)
            {
                CreatePrecisionDependentPipelineStates();
            }
        }

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

        // === 可変プリ露出の決定 ===
        // 昼(直射日光10万lx)を基準0として、そのフレームのキー照度が何段暗いかを求め、
        // ユーザー設定のEV100へ足す。これによりHDRバッファへ流れる値のレンジが
        // 昼でも夜でもおおむね一定に保たれ、夜がfp16でつぶれなくなる
        // (詳細な理由はm_EffectiveExposureEV100の宣言コメント)。
        // 露出はTonemap/Bloom/AutoExposureが同じ値で割り戻すため、これを動かしても絵は変わらない
        {
            const float keyIlluminance = std::max(sunLighting.KeyIlluminanceLux, 1e-6f);
            const float autoBias = std::log2(keyIlluminance / kSunIlluminanceLux);
            // 【下限-12段の根拠 ― 表示レンジの両端をfp16に収める】
            // このバイアスは「HDRバッファの値」と「トーンマップが受け取る表示値」の橋渡しで、
            //   表示値 = バッファの値 × 2^bias
            // という関係にある。したがってバッファの上限(fp16の65504)と下限(最小正規化数6.1e-5)は、
            // そのまま**表示できる明るさの上限と下限**になる。
            //
            //   上側: 表示16(ACESの白より十分上。ここまで出せれば発光物が白へ振り切れる)を
            //         表すには 65504 × 2^bias >= 16 → bias >= -12
            //   下側: 表示1e-4(sRGBで1階調にも満たない=見えない)がfp16の正規化域に残るには
            //         1e-4 × 2^-bias >= 6.1e-5 → bias <= 0.7
            //
            // よって-12が両立点になる。**要件が表示値で書けているので、シーンのExposureにも
            // その夜の照度にも依存しない**のがこの値の性質である。
            //
            // 【かつて-18だった理由と、それが上限を潰していたこと】
            // 元の-18は「照度の差(満月なら-18.34段)をできるだけ打ち消す」という下側だけの
            // 発想で決まっており、上側は見ていなかった。その結果 表示上限が
            // 65504 × 2^-18 = 0.25 に落ち、**夜はどんな発光物も表示0.25(sRGBで132前後)より
            // 明るくできない**状態になっていた。ドローンショーで[DroneShow]Brightnessを
            // 0.30から45へ150倍にしても画素値が1段も動かなかったのはこれが原因で、
            // 上限に張り付いたまま裾だけが飽和して色を失っていた。
            // -12にすると上限は65504 × 2^-12 = 16へ64倍広がり、Brightnessが再び効くようになる
            // (実測: 0.30/1.0/3.0 で編隊の最大画素値が 166/211/237 と動く。-18では全部153だった)。
            //
            // 【絵が変わらないことの保証】Tonemap/Bloom/AutoExposureはいずれもこの値を
            // 割り戻すので、fp16の範囲に収まっている画素は1つも動かない。実測でも
            // 地形・水面・空の画素値は-18のときと完全に一致し、飽和していた機体だけが変わった。
            // 上限0段は「昼より明るくはしない」の意味で従来どおり
            const float targetEV100 = m_PostProcessSettings.SceneExposureEV100 + std::clamp(autoBias, -12.0f, 0.0f);

            if (!m_EffectiveExposureInitialized)
            {
                // 起動直後・シーン切り替え直後は平滑化せず即座に合わせる
                m_EffectiveExposureEV100 = targetEV100;
                m_EffectiveExposureInitialized = true;
            }
            else
            {
                // 一時停止や巨大なdtで飛ばないよう上限を設ける
                const float deltaTime = std::clamp(m_RenderDeltaTime, 0.0f, 0.1f);
                const float t = std::clamp(1.0f - std::exp(-deltaTime * m_PostProcessSettings.EffectiveExposureAdaptSpeed), 0.0f, 1.0f);
                m_EffectiveExposureEV100 += (targetEV100 - m_EffectiveExposureEV100) * t;
            }
        }

        // 【検証専用】蓄積が始まる瞬間に1回だけ摂動を加える。
        // 時間再利用の「追従」(灯を消したら何フレームで消えるか、露出が跳んでも
        // 明るさが暴れないか)は、静止した絵をいくら撮っても測れない。
        // 蓄積ダンプは総和を書くので、Nを変えた2本の差が1フレームぶんになる ――
        // これで追従の時間変化を、フレームごとのGPU読み戻し無しで測れる
        if (m_MegaLightsSettings.PerturbMode != 0 && !m_MegaLightsPerturbApplied && m_MegaLightsSettings.AccumTargetFrames > 0 &&
            m_MegaLightsAccumWarmupFrames >= kMegaLightsAccumWarmup)
        {
            m_MegaLightsPerturbApplied = true;
            if (m_MegaLightsSettings.PerturbMode == 1)
            {
                // 全ライトを消す。次フレーム以降のGPULight配列から外れるので、
                // 真値は「ローカルライトの寄与が0」になる。時間再利用が履歴を抱えていると
                // すぐには0にならず、その残り方がゴーストそのもの
                for (Assets::Light& light : m_Lights)
                {
                    light.Enabled = false;
                }
                Core::Logger::Info("KurenaiEngine3D", "【検証】全ライトを消しました(ゴースト測定)");
            }
            else if (m_MegaLightsSettings.PerturbMode == 2)
            {
                // 実効プリ露出を+2段跳ばす。ライトの放射輝度は露出を掛け込んで作られるので、
                // 履歴のWは前フレームの露出のままになる。補正が効いていれば絵は変わらない
                m_EffectiveExposureEV100 += 2.0f;
                Core::Logger::Info(
                    "KurenaiEngine3D", "【検証】実効プリ露出を+2段跳ばしました(プリ露出補正の確認)");
            }
        }

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

        // モデルLODの段を、レンダーグラフを組む前にこの1回だけ決める。
        // 【パスごとに測り直してはいけない】深度プリパスとG-Bufferが違う段を選ぶと、
        // プリパスが深度を書いた画素をG-Bufferが描かず、画面に穴が開く。
        // G-Bufferパスのラムダはそもそもカメラ位置をキャプチャしていない(半透明パスだけが持つ)ので、
        // ここで決めてm_InstanceLODStatesへ置く形にしてある
        UpdateModelLOD(cameraPosition, m_RenderDeltaTime);

        // モデルのストリーミング。【LODの後に呼ぶ】どの段を読むかは選ばれた段で決まる
        UpdateModelStreaming(cameraPosition);

        // インスタンシングのバッチを組み直す。【LODとストリーミングの後】まとめられるかどうかは
        // 「そのフレームに選ばれた段」と「読み込み済みか」で決まる。レンダーグラフの構築より前に
        // 1回だけ呼ぶこと ―― パスごとに組み直すと、深度プリパスとG-Bufferが違うまとめ方をする
        BuildInstanceBatches(commandList);

        // レイトレーシングを常駐の増減へ追随させる(ストリーミング時のみ働く)
        UpdateRaytracingRebuild();

        // テクスチャの常駐ミップの目標を更新し、差のあるものをワーカーへ積む。
        // 実際の差し替えは次フレーム以降のCommitReady(このフレームの先頭で呼んだもの)で確定する。
        // 画面の高さは内部レンダー解像度を使う(ウィンドウ解像度ではない。
        // 内部解像度を下げているときは必要なテクセル密度もその分下がる)
        m_TextureStreaming.UpdateTargets(
            cameraPosition, std::tan(frameState.Camera.GetFovY() * 0.5f), m_RenderHeight, m_RenderDeltaTime);

        // --- TAAのサブピクセルジッター ---
        // 投影行列を1ピクセル未満だけずらして、同じ画素が毎フレームわずかに違う位置をサンプルする
        // ようにする。TAAが複数フレームぶんを蓄積することで実質的なスーパーサンプリングになる。
        // TAA無効時はジッターも必ず0にすること(ジッターだけ残ると画面が振動するだけになる)
        ++m_TAAFrameIndex;

        // --- MegaLights候補プールのタイル格子ジッター ---
        // 書き手・Initial/Spatial・Presentへ配る値をここで一度だけ決める。
        // 各パスが個別にフレーム番号から導くと、式の片側だけを直した際に別タイルを静かに読むため
        const bool megaLightsTileJitterEnabled = m_MegaLightsSettings.TileJitterMode != 0;
        DirectX::XMUINT2 megaLightsTileOffset{ 0u, 0u };
        if (m_MegaLightsSettings.TileJitterMode == 1)
        {
            // Halton(2,3)を16段階へ量子化する。RadicalInverseは[0,1)だが、丸め誤差でも
            // 16にならないようタイル幅-1で明示的に押さえる
            megaLightsTileOffset.x = std::min<uint32_t>(
                static_cast<uint32_t>(RadicalInverse(m_TAAFrameIndex, 2u) * kLightTileSize),
                kLightTileSize - 1u);
            megaLightsTileOffset.y = std::min<uint32_t>(
                static_cast<uint32_t>(RadicalInverse(m_TAAFrameIndex, 3u) * kLightTileSize),
                kLightTileSize - 1u);
        }
        // 無効時だけ従来のタイル数をそのまま使い、添字・乱数の種・ディスパッチ数を保存する。
        // モード2は対照実験なので、オフセット0でも有効側と同じ+1タイルを通す
        const uint32_t megaLightsEffectiveTilesX =
            megaLightsTileJitterEnabled ? (m_RenderTargets.LightTileCountX + 1u) : m_RenderTargets.LightTileCountX;
        const uint32_t megaLightsEffectiveTilesY =
            megaLightsTileJitterEnabled ? (m_RenderTargets.LightTileCountY + 1u) : m_RenderTargets.LightTileCountY;

        DirectX::XMFLOAT2 jitterOffsetPixels{ 0.0f, 0.0f };
        if (m_PostProcessSettings.TAAEnabled)
        {
            // Halton列の添字は1から始める。添字0はradical inverseの定義上どの基数でも0となり、
            // オフセットがピクセルの角(-0.5, -0.5)へ偏ってしまう
            const uint32_t haltonIndex = (m_TAAFrameIndex % kTAAJitterSampleCount) + 1;
            jitterOffsetPixels.x = (RadicalInverse(haltonIndex, 2) - 0.5f) * m_PostProcessSettings.TAAJitterScale;
            jitterOffsetPixels.y = (RadicalInverse(haltonIndex, 3) - 0.5f) * m_PostProcessSettings.TAAJitterScale;
        }
        // ピクセル単位のオフセットをNDCとUVの2つの単位へ直す。
        // ピクセル座標は右が+x・下が+yなのに対しNDCは上が+yなので、yだけ符号が反転する
        // (この符号を落とすと縦方向のジッターと速度が逆向きになる)
        const DirectX::XMFLOAT2 jitterNdc{
            2.0f * jitterOffsetPixels.x / static_cast<float>(m_RenderWidth),
            -2.0f * jitterOffsetPixels.y / static_cast<float>(m_RenderHeight),
        };
        // NDC→UVは xy * (0.5, -0.5) + 0.5 なので、ジッターのUV換算はピクセル数/解像度そのものになる
        const DirectX::XMFLOAT2 jitterUv{
            jitterOffsetPixels.x / static_cast<float>(m_RenderWidth),
            jitterOffsetPixels.y / static_cast<float>(m_RenderHeight),
        };

        // ビュー行列と「ジッター済み」射影行列をここで一度だけ確定させ、以降のカメラ由来の行列は
        // すべてこれらから作る。
        //
        // 【なぜ行列の掛け算でジッターを入れられるのか】Camera::GetProjectionMatrixは行ベクトル規約
        // (clip = view * P)で、第3列が(0,0,1,0)すなわち clip.w = viewZ である。
        // XMMatrixTranslationは行ベクトル規約では第3行が(jx, jy, 0, 1)になるので、P * T を展開すると
        // 変化するのは要素[2][0]と[2][1]、つまり clip.xy += jitterNdc * clip.w だけになる。
        // w除算後には ndc.xy += jitterNdc という定数オフセットになり、狙いどおり平行移動として効く。
        //
        // 【なぜ全パスで統一するのか】深度バッファはこのジッター済み行列でラスタライズされる。
        // 深度から位置を復元する側(SSAO/SSIL/SSR/スクリーンスペースシャドウ)がジッター前の行列を
        // 使うと、再構成した位置がサブピクセルぶんずれて自己遮蔽やハローの原因になる。
        // なお射影行列の_33/_43(深度のリニアライズ係数)はジッターでは変化しない
        const DirectX::XMMATRIX viewMatrix = frameState.Camera.GetViewMatrix();
        const DirectX::XMMATRIX jitteredProj =
            frameState.Camera.GetProjectionMatrix() * DirectX::XMMatrixTranslation(jitterNdc.x, jitterNdc.y, 0.0f);

        // --- メッシュレットLODの段を選ぶ入力を、このフレームぶん一度だけ確定させる ---
        //
        // 【全パスへ同じものを配る】シャドウと深度プリパスは同じ増幅シェーダーを使うが、
        // ViewProjは光源やカスケードのものに差し替わっている。各パスのカメラで段を選ぶと
        // 影を落とす形と本体の形が違う段になるため、主カメラの値をここで決めて配る。
        //
        // 【ジッターの影響を受けない値を使う】拡大率_22はジッター(平行移動)では変化しない。
        // 仮に変化する量を使うと、段の境目でTAAのジッター周期に合わせて段が振動する
        {
            DirectX::XMFLOAT4X4 projForLOD;
            DirectX::XMStoreFloat4x4(&projForLOD, frameState.Camera.GetProjectionMatrix());
            m_MeshletLODFrame.CameraPos = frameState.Camera.GetPosition();
            // 射影行列の_22 = 1/tan(fovY/2)。画面の高さ全体が 2*tan(fovY/2) なので、
            // 距離1メートルの1メートルは _22 * 高さ / 2 画素になる
            m_MeshletLODFrame.PixelScale = 0.5f * projForLOD._22 * static_cast<float>(m_RenderHeight);
            m_MeshletLODFrame.Quality = m_GeometrySettings.MeshletLODEnabled ? m_GeometrySettings.MeshletLODQuality : 0.0f;
            m_MeshletLODFrame.Forced = m_GeometrySettings.MeshletLODEnabled ? m_GeometrySettings.MeshletLODForcedLevel : -1;
            m_MeshletLODFrame.DebugColorByLOD = m_GeometrySettings.MeshletLODDebugColorEnabled;
        }

        // --- ドローンショーの機体を評価する ---
        //
        // 【ライトリストの組み立てより前でなければならない】機体を光源として送るので、
        // ここが後ろにあると灯が1フレーム遅れる(編隊が動いている間ずっと、光だけが
        // 前フレームの位置から当たり続ける)。GPUバッファへの転送は下のグラフ構築直前のまま。
        // m_DroneShowTimeの更新はRenderThreadMainで既に済んでいる
        m_DroneInstances.clear();
        if (m_DroneShowEnabled)
        {
            m_DroneShow.Evaluate(m_DroneShowTime, m_DroneShowCenter, m_DroneShowScale, m_DroneInstances);
            // 【バッファの容量を超える機体は描かない】m_DroneShowResources.BufferはkMaxDrones分を固定確保して
            // いるので、それを超えた分をUpdateBufferへ渡すと書き込みが範囲外になる。
            // .kshowの機体数はエディタ側で上限を掛けているが、外から来たファイルでも
            // 壊れないよう、ここで切り詰める(光源を作るのも切り詰めた後の配列から)
            if (m_DroneInstances.size() > kMaxDrones)
            {
                m_DroneInstances.resize(kMaxDrones);
            }
        }

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
            gpuLights.push_back(MakeGPULight(light, m_EffectiveExposureEV100));
        }
        // ここまでが作者の置いたライト。以降のプロキシと切り分けるために数を控える
        const size_t manualLightCount = gpuLights.size();

        // --- エミッシブ光源のプロキシを後ろへ連結する ---
        //
        // 【手置きの後ろに置く】容量超過の切り捨ては下でプロキシ側だけに掛ける。
        // 全体をカメラ距離でソートして切ると、**手置きの遠いライトが黙って消える**。
        //
        // 【毎フレーム作り直す】m_EmissiveLightSettings.Intensity のスライダーとτを即座に反映するため。
        // プロキシ側は倍率も露出も持たない値(RadianceBase)で保持してある
        m_RenderStats.EmissiveLightsUsedCount = 0;
        // 切り捨てが起きたときだけ、採用した集合の指紋を残す(起きなければ0のまま)。
        //
        // 【プローブの署名に要る】採用順はカメラからの照度で決まるので、**カメラを動かすだけで
        // プローブが焼く光源の集合が変わる**。署名が変わらないと反射プローブはOnDemandで
        // 焼き直さず、DDGIは更新を止めたまま、収束済みのプローブだけ古い集合で残る。
        // 切り捨てが起きない限り集合はシーン固定なので、そのときは0で十分
        m_EmissiveLightsSelectionHash = 0;
        if (m_EmissiveLightSettings.LightsEnabled && !m_EmissiveProxies.empty() && manualLightCount < kMaxLights)
        {
            const size_t budget = std::min<size_t>(
                static_cast<size_t>(std::max(0, m_EmissiveLightSettings.LightsMaxCount)), kMaxLights - manualLightCount);

            if (m_EmissiveProxies.size() <= budget)
            {
                for (const Assets::EmissiveProxy& proxy : m_EmissiveProxies)
                {
                    gpuLights.push_back(MakeGPULightFromEmissiveProxy(
                        proxy, m_EmissiveLightSettings.Intensity, m_EmissiveLightSettings.LightsCutoffIrradiance,
                        m_EmissiveLightsMaxRange));
                }
            }
            else
            {
                // 【スコアはカメラ位置に届く表示空間の照度】単なるカメラ距離だと、
                // 遠くの明るい看板より近くの暗い豆電球が残る。
                // 同値のときは (インスタンス, メッシュ, かたまり) の辞書順で決める ――
                // 順序が揺れるとライトが出入りしてちらつく
                std::vector<size_t> order(m_EmissiveProxies.size());
                for (size_t i = 0; i < order.size(); ++i)
                {
                    order[i] = i;
                }
                const auto scoreOf = [this, &cameraPosition](size_t index)
                {
                    const Assets::EmissiveProxy& p = m_EmissiveProxies[index];
                    const float dx = p.Position[0] - cameraPosition.x;
                    const float dy = p.Position[1] - cameraPosition.y;
                    const float dz = p.Position[2] - cameraPosition.z;
                    const float distSq = dx * dx + dy * dy + dz * dz;
                    const float peak = std::max({ p.RadianceBase[0], p.RadianceBase[1], p.RadianceBase[2] }) *
                                       m_EmissiveLightSettings.Intensity * p.Area;
                    return peak / std::max(distSq, p.SourceRadius * p.SourceRadius + 1e-6f);
                };
                std::stable_sort(
                    order.begin(), order.end(),
                    [this, &scoreOf](size_t a, size_t b)
                    {
                        const float sa = scoreOf(a);
                        const float sb = scoreOf(b);
                        if (sa != sb) { return sa > sb; }
                        const Assets::EmissiveProxy& pa = m_EmissiveProxies[a];
                        const Assets::EmissiveProxy& pb = m_EmissiveProxies[b];
                        if (pa.InstanceIndex != pb.InstanceIndex) { return pa.InstanceIndex < pb.InstanceIndex; }
                        if (pa.MeshIndex != pb.MeshIndex) { return pa.MeshIndex < pb.MeshIndex; }
                        return pa.ClusterIndex < pb.ClusterIndex;
                    });
                // FNV-1a(64bit)。採用したプロキシの識別子だけを順に混ぜる。
                // 位置や強さは混ぜない ―― それらはシーン固定で、変わるのは「どれを採ったか」だけ
                uint64_t selectionHash = 1469598103934665603ull;
                const auto mixIndex = [&selectionHash](uint32_t value)
                {
                    const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
                    for (size_t b = 0; b < sizeof(value); ++b)
                    {
                        selectionHash ^= bytes[b];
                        selectionHash *= 1099511628211ull;
                    }
                };
                for (size_t i = 0; i < budget; ++i)
                {
                    const Assets::EmissiveProxy& proxy = m_EmissiveProxies[order[i]];
                    mixIndex(proxy.InstanceIndex);
                    mixIndex(proxy.MeshIndex);
                    mixIndex(proxy.ClusterIndex);
                    gpuLights.push_back(MakeGPULightFromEmissiveProxy(
                        proxy, m_EmissiveLightSettings.Intensity, m_EmissiveLightSettings.LightsCutoffIrradiance, m_EmissiveLightsMaxRange));
                }
                m_EmissiveLightsSelectionHash = selectionHash;

                // 【切り捨ては発光を捨てている】併合で減らせないか先に疑うこと。
                // EmeraldSquare の実測では、面積の大きい順に上位256個を残しても
                // 総面積の46.7%にしかならない(上位1024個でも84.9%)
                if (!m_EmissiveLightsCapLogged)
                {
                    Core::Logger::Warning(
                        "KurenaiEngine3D",
                        "エミッシブ光源が上限(" + std::to_string(budget) + ")を超えたため" +
                            std::to_string(m_EmissiveProxies.size() - budget) +
                            "個を捨てました。捨てたぶんの発光は絵から消えます" +
                            (ShouldSuppressEmissiveForGI()
                                 ? "。**しかもDDGIからは抑止されたまま**です ―― 捨てた面は"
                                   "直接光にも間接光にも入らず、純粋なエネルギー損失になります"
                                 : ""));
                    m_EmissiveLightsCapLogged = true;
                }
            }
            m_RenderStats.EmissiveLightsUsedCount = static_cast<uint32_t>(gpuLights.size() - manualLightCount);

            // 【「効いていない」と「暗すぎて見えない」を切り分けられるようにする】
            // 絵の差だけを見ていると、経路が走っていないのか寄与が小さいだけなのかが分からない。
            // 実際に送った灯数と、代表1灯の強さ・Range・κ を1回だけ出す
            if (!m_EmissiveLightsValuesLogged && m_RenderStats.EmissiveLightsUsedCount > 0)
            {
                const GPULight& sample = gpuLights[manualLightCount];
                // 【RGBの最大を出す。Rだけを出さない】Rangeはmax(R,G,B)から解いているので、
                // 色付きの自発光(赤い看板など)でRだけを見るとログからRangeを検算できない
                const float samplePeak =
                    std::max({ sample.ColorRange.x, sample.ColorRange.y, sample.ColorRange.z });
                Core::Logger::Info(
                    "KurenaiEngine3D",
                    "エミッシブ光源を送信: " + std::to_string(m_RenderStats.EmissiveLightsUsedCount) + "灯(手置き " +
                        std::to_string(manualLightCount) + "灯) / 先頭の灯 強さ(RGBの最大) " +
                        std::to_string(samplePeak) + " Range " + std::to_string(sample.ColorRange.w) +
                        "m 半径 " + std::to_string(sample.Params.z) + "m κ " + std::to_string(sample.Params.w));
                m_EmissiveLightsValuesLogged = true;
            }
        }

        // ここまでが「焼き込みに入れてよい灯」。プローブと DDGI はこの数までしか舐めない。
        //
        // 【ドローンの灯をこの後ろへ置く理由】編隊は毎フレーム動く。反射プローブは
        // OnDemand で焼くので「焼いた瞬間の編隊」が環境キューブに固定で残り、DDGI は
        // ヒステリシスで編隊を追いかけ続けて収束しない。どちらも動く光を入れる前提の
        // 構造になっていないため、この2つからは外す
        size_t bakedLightCount = gpuLights.size();

        // --- ドローンショーの機体を光源として後ろへ連結する ---
        //
        // 【エミッシブプロキシとは単位系が違う】プロキシは露出を掛けない(自発光がG-Bufferで
        // 露出を通らないため、I*exposure の中で相殺する)。一方ドローンのスプライトは
        // Passes::DroneShowConstants.Params0.x = Brightness * effectiveExposure として露出を通っており、
        // 単位系としては手置きライト(カンデラ)の側にいる。**ここは掛ける側が正しい**。
        // 向こうの慣習を写すと桁で外す(docs/ImplementationDetail.md 62.4の表)
        m_DroneShowLightUsedCount = 0;
        if (m_DroneShowEnabled && m_DroneShowCastLight && !m_DroneInstances.empty())
        {
            // 手置き+プロキシを押し出さないよう、残り容量だけを使う
            const size_t budget =
                (gpuLights.size() < kMaxLights) ? (kMaxLights - gpuLights.size()) : 0u;
            const uint32_t sampleCount = static_cast<uint32_t>(
                std::min<size_t>(budget, static_cast<size_t>(std::max(0, m_DroneShowLightSampleCount))));

            m_DroneShow.BuildLightSamples(m_DroneInstances, sampleCount, m_DroneLightSamples);

            // 【bakedLightCountを添字に使い回さない】あちらは「焼き込みに入れてよい灯の数」で、
            // 容量超過の切り詰めが走ると意味が変わる(下の再代入を参照)。
            // ここで欲しいのは「ドローンの灯の先頭の位置」という別の量なので、別に持つ
            const size_t firstDroneLightIndex = gpuLights.size();

            const float exposure = ComputeExposure(m_EffectiveExposureEV100);
            const float cutoffLux = std::max(m_DroneShowLightCutoffLux, 1e-9f);
            // 演出用の倍率。1.0がスプライトから導いた物理的な値。
            // 【Rangeにも効かせる】強くした灯を同じRangeで打ち切ると、届くはずの距離で
            // 切れて「明るくしたのに広がらない」になる。下でpeakから解き直すので自動的に効く
            const float lightScale = std::max(m_DroneShowCastLightScale, 0.0f);
            for (const DroneLightSample& rawSample : m_DroneLightSamples)
            {
                DroneLightSample sample = rawSample;
                sample.Intensity = { rawSample.Intensity.x * lightScale, rawSample.Intensity.y * lightScale,
                                     rawSample.Intensity.z * lightScale };

                GPULight light{};
                light.PositionType = { sample.Position.x, sample.Position.y, sample.Position.z,
                                       static_cast<float>(Assets::LightType::Point) };

                // 【Rangeは打ち切り照度から逆算する】MakeGPULightFromEmissiveProxy と同じ考え方。
                // 点光源(型1)の減衰に半径の項は無いので、あちらの -R² は付けない。
                // RGBの最大から解くのは、色付きの灯で1chだけ見るとRangeが検算できないため
                const float peak = std::max({ sample.Intensity.x, sample.Intensity.y, sample.Intensity.z });
                float range = (peak > 0.0f) ? std::sqrt(peak / cutoffLux) : 0.0f;
                // 下限は光源自身の広がりを覆う分。上限はエミッシブ光源と同じシーンAABB対角で、
                // タイルライトカリングが全タイルにヒットするのを止める安全弁
                range = std::max(range, 2.0f * sample.SourceRadius);
                if (m_EmissiveLightsMaxRange > 0.0f)
                {
                    range = std::min(range, m_EmissiveLightsMaxRange);
                }

                light.ColorRange = { sample.Intensity.x * exposure, sample.Intensity.y * exposure,
                                     sample.Intensity.z * exposure, range };
                light.DirectionAngle = { 0.0f, -1.0f, 0.0f, 0.0f };
                // 【スクリーンスペースシャドウは立てない】画素あたりのシャドウレイ本数には
                // 上限(Defaults::ScreenSpaceShadowMaxLightsPerPixel)があり、数十灯を入れると
                // 手置きライトの接触影を食い潰す(docs/ImplementationDetail.md 62.7と同じ理由)。
                // Params.z は光源の半径で、減衰には効かずMegaLightsの半影の広がりだけを決める
                light.Params = { 0.0f, static_cast<float>(kLightShadowRaytraced), sample.SourceRadius, 0.0f };
                gpuLights.push_back(light);
            }
            m_DroneShowLightUsedCount = static_cast<uint32_t>(m_DroneLightSamples.size());

            // 【「効いていない」と「暗すぎて見えない」を切り分ける】エミッシブ光源と同じ理由で、
            // 実際に送った灯数と代表1灯の実効値を1回だけ出す
            if (!m_DroneShowLightValuesLogged && m_DroneShowLightUsedCount > 0)
            {
                float totalCd = 0.0f;
                for (const DroneLightSample& sample : m_DroneLightSamples)
                {
                    totalCd += std::max({ sample.Intensity.x, sample.Intensity.y, sample.Intensity.z });
                }
                const GPULight& first = gpuLights[firstDroneLightIndex];
                Core::Logger::Info(
                    "KurenaiEngine3D",
                    "ドローンを光源として送信: " + std::to_string(m_DroneShowLightUsedCount) + "灯(機体 " +
                        std::to_string(m_DroneInstances.size()) + "機) / 倍率 " +
                        std::to_string(m_DroneShowCastLightScale) + " / 総光度(RGBの最大の和。倍率込み) " +
                        std::to_string(totalCd * m_DroneShowCastLightScale) + "cd / 先頭の灯 露出後の強さ " +
                        std::to_string(std::max({ first.ColorRange.x, first.ColorRange.y, first.ColorRange.z })) +
                        " Range " + std::to_string(first.ColorRange.w) + "m 半径 " +
                        std::to_string(first.Params.z) + "m");
                m_DroneShowLightValuesLogged = true;
            }

            // 【条件をbudgetで見る】m_DroneShowLightUsedCount == 0 で判定すると、
            // 灯数の設定を0にしただけのときにも「容量を使い切っています」と誤報する
            if (budget == 0u && !m_DroneShowLightTileOverflowLogged)
            {
                Core::Logger::Warning(
                    "KurenaiEngine3D",
                    "ドローンを光源として送れませんでした(ライトの容量" + std::to_string(kMaxLights) +
                        "灯を手置きライトとエミッシブ光源で使い切っています)");
                m_DroneShowLightTileOverflowLogged = true;
            }
        }
        else
        {
            m_DroneLightSamples.clear();
        }

        // 容量(kMaxLights)を超える場合は、カメラに近い順に先頭kMaxLights灯のみ採用する。
        // 全画面ディファードなのでフラスタムカリングは効果が薄く、これは容量超過時の
        // 安全弁としてのみ機能する。
        //
        // 【ここへ来るのは手置きライトだけで超えたとき】プロキシは上で別枠に収めてある
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

            // 【bakedLightCountの前提が崩れる】上のソートは配列全体を並べ替えるので、
            // 「先頭bakedLightCount灯が焼き込みに入れてよい灯」という対応が失われる。
            // bakedLightCount自身もkMaxLightsを超えうるので、そのまま
            // ActiveLightCountとして渡すと配列長を超えた読み出しになる。
            // ここまで来るのは手置きライトだけで1024灯を超えた場合で、そのときは
            // どれが焼き込み対象かを区別できないため、**全灯を焼き込みへ入れる**側へ倒す
            // (プローブに入り過ぎるほうが、範囲外を読むより安全)
            bakedLightCount = gpuLights.size();

            if (!m_LightOverflowLogged)
            {
                Core::Logger::Warning(
                    "KurenaiEngine3D",
                    "ライト数が上限(" + std::to_string(kMaxLights) + ")を超えたため、カメラに近い順に描画します"
                    "(この場合ドローンの灯と焼き込み対象の切り分けは失われます)");
                m_LightOverflowLogged = true;
            }
        }

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
        const bool usingProceduralSky = (skyTexture == m_ProceduralSkyTexture.get());

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
        const bool skyIntegrateThisFrame = bakeSkyThisFrame || !m_SkyParametersBufferInitialized;

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
            m_IBLBaked = false;
            m_IBLIrradianceBaked = false;
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
        const bool meshletPathActive = m_GeometrySettings.MeshletRenderingEnabled && m_GBufferMeshletPipelineState != nullptr;

        // 増幅シェーダーのHi-Zオクルージョンカリング(Stage 5-2)をこのフレームで行うか。
        // 判定を書いてあるのは増幅シェーダーだけなので、メッシュレット経路に乗らないフレームでは
        // 1つも間引けず、Hi-Zを構築する意味も無い(下のHi-Zパスの登録条件がこれを見る)
        const bool occlusionCullingActive = m_GeometrySettings.OcclusionCullingEnabled && meshletPathActive;

        // メッシュレットカリングの統計をこのフレームで数えるか。
        // 増幅シェーダーが走らなければ数える相手がいない
        const bool meshletCullStatsActive =
            m_GeometrySettings.MeshletCullStatsEnabled && meshletPathActive && m_MeshletCullStatsBuffer != nullptr;

        FrameConstants constants;
        const DirectX::XMMATRIX viewProj = viewMatrix * jitteredProj;
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
        const DirectX::XMMATRIX reflectedViewProj = reflectMatrix * viewMatrix * jitteredProj;
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
        DirectX::XMStoreFloat4x4(&constants.View, DirectX::XMMatrixTranspose(viewMatrix));
        // ジッター済みの射影行列を渡す。SSAO/SSILはこの行列でView空間の点を画面へ投影して
        // 深度バッファと突き合わせるため、深度を描いたときと同じ行列でなければサブピクセルぶんずれる
        DirectX::XMStoreFloat4x4(&constants.Proj, DirectX::XMMatrixTranspose(jitteredProj));
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
            constants.TAAParams = { jitterUv.x, jitterUv.y, m_TAAPrevJitterUv.x, m_TAAPrevJitterUv.y };
        }
        else
        {
            constants.PrevViewProj = constants.ViewProj;
            constants.TAAParams = { jitterUv.x, jitterUv.y, jitterUv.x, jitterUv.y };
        }

        // DDGI(22章)。一度も焼けていない間はアトラスの中身が未定義なので無効にしておく
        // (反射プローブの「一度でも焼けたか」と同じ方針)
        const bool ddgiActive = m_DDGISettings.Enabled && m_GIResources.HasGIVolume && m_DDGIBaked;
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
            m_DDGISettings.HalfResolution && m_GIResources.DDGIResolveTexture && m_DDGISettings.Enabled && m_GIResources.HasGIVolume && m_DDGIBaked;
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
        DirectX::XMStoreFloat4x4(&projForPixelAngle, jitteredProj);
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
            occlusionCullingActive && m_HiZValid && m_TAAPrevViewProjValid;

        // 深度プリパスが走るなら、その深度からHi-Zを作れる。**そのフレームのG-Bufferは
        // 前フレームのHi-Zを待たなくてよい** ―― 上の2条件はどちらも要らなくなる。
        // 条件の意味と、プリパス自身が今フレームのHi-Zを使えない理由は、
        // 下の hiZFromDepthPrepass を定義している箇所のコメントにある
        const bool depthPrepassRuns = m_GeometrySettings.DepthPrepassEnabled
            && m_DepthPrepassPipelineState && m_DepthPrepassCutoutPipelineState;
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
            static_cast<float>(m_HiZMipLevels),
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
        DirectX::XMStoreFloat4x4(&projectionForDepthLinearize, jitteredProj);
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

        Passes::LightingConstants lightingConstants{};
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

        // --- ドローンショーの機体をGPUへ送る ---
        // ライトリストとまったく同じ理由でグラフ構築の前に1回だけ更新する。このバッファは
        // 本描画パスと平面反射パスの2箇所から読まれるため、パスの中で更新すると
        // 先に走る側が未更新の内容を読んでしまう。
        // 【m_DroneInstancesを作るのはここではない】ライトリストの組み立てが機体の位置を要るため、
        // Evaluateはそれより前(gpuLightsの直前)へ移してある。ここは転送だけ
        if (m_DroneShowEnabled && !m_DroneInstances.empty())
        {
            commandList->UpdateBuffer(
                m_DroneShowResources.Buffer.get(), m_DroneInstances.data(), m_DroneInstances.size() * sizeof(GPUDrone));
        }

        // 各パスをリソースの読み書き依存関係から自動的に順序付けて実行するレンダーグラフ。
        // トランジェントリソースの確保は行わず、既存の永続確保済みテクスチャ(G-Buffer・SceneColor等)を
        // そのまま読み書きする(詳細はRenderGraph.h参照)
        // --- パス群へ配るフレームのスナップショットと、登録中に確定していく出力(段階6) ---
        // 【graph.Execute() が終わるまで生かすこと】パスの Execute ラムダはこの2つより
        // 長生きするので、ここより内側のスコープへ置くと参照が浮く
        Rendering::RenderFrameContext frameContext{};
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
        frameContext.MegaLightsEffectiveTilesX = megaLightsEffectiveTilesX;
        frameContext.MegaLightsEffectiveTilesY = megaLightsEffectiveTilesY;
        frameContext.MegaLightsTileOffset = megaLightsTileOffset;
        frameContext.ManualExposureScale = manualExposureScale;
        frameContext.KeyReferenceEV100 = keyReferenceEV100;
        frameContext.ViewMatrix = viewMatrix;
        frameContext.JitteredProj = jitteredProj;
        frameContext.InvViewProj = invViewProj;
        frameContext.JitterUv = jitterUv;
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

        Rendering::RenderBlackboard blackboard{};

        Core::RenderGraph graph(commandList, m_GPUProfiler.get(), &m_CPUProfiler);

        // --- 環境(空・大気・雲・IBL)の焼き込みパス群(段階6で Passes/EnvironmentPasses へ移設) ---
        // 【必ずグラフの先頭で登録すること】依存解決は登録順の前方走査なので、
        // ここより後ろへ動かすとSkyIntegrateが未初期化のLUTを読む
        m_EnvironmentPasses->Register(graph, frameContext, blackboard);

        RHI::Viewport shadowViewport;
        shadowViewport.Width = static_cast<float>(kShadowMapSize);
        shadowViewport.Height = static_cast<float>(kShadowMapSize);

        RHI::Viewport gbufferViewport;
        gbufferViewport.Width = static_cast<float>(m_RenderWidth);
        gbufferViewport.Height = static_cast<float>(m_RenderHeight);
        frameContext.GBufferViewport = gbufferViewport;
        frameContext.ShadowViewport = shadowViewport;
        frameContext.BakedLightCount = bakedLightCount;

        // プローブのキューブ面キャプチャが使う射影。**反射プローブとDDGIで同じものを使う**
        // ため、どちらの群からも引けるようフレームのスナップショットへ載せる
        const DirectX::XMMATRIX probeFaceProjection =
            ComputeCubeFaceProjection(frameState.Camera.GetNearZ(), frameState.Camera.GetFarZ());
        frameContext.ProbeFaceProjection = probeFaceProjection;

        // プローブのキャプチャが読むテクスチャ一式。反射プローブとDDGIが同じ組を読む
        const std::vector<RHI::IRHITexture*> probeCaptureReads = {
            m_RenderTargets.ShadowCascadeArray.get(),
            skyTexture, m_IBLResources.IrradianceTexture.get(), m_IBLResources.PrefilteredEnvTexture.get(), m_IBLResources.BRDFLUTTexture.get(),
        };
        frameContext.ProbeCaptureReads = &probeCaptureReads;
        frameContext.CascadeViewProj = cascadeViewProj;

        // --- シャドウのパス群(段階6で Passes/ShadowPasses へ移設) ---
        // 【この位置で登録すること】依存が同点のときRenderGraphは最小登録番号を選ぶ。
        // 登録順そのものが実行順の一部になっている
        m_ShadowPasses->RegisterCascades(graph, frameContext);

        // --- 反射プローブのパス群(段階6で Passes/ReflectionProbePasses へ移設) ---
        // 【この位置で登録すること】依存が同点のときRenderGraphは最小登録番号を選ぶ。
        // 登録順そのものが実行順の一部になっている
        m_ReflectionProbePasses->Register(graph, frameContext, blackboard);

        // --- DDGIのパス群(段階6で Passes/DDGIPasses へ移設) ---
        // 【この位置で登録すること】依存が同点のときRenderGraphは最小登録番号を選ぶ。
        // 登録順そのものが実行順の一部になっている
        m_DDGIPasses->RegisterProbeUpdate(graph, frameContext, blackboard);

        // --- ジオメトリのパス群(段階6で Passes/GeometryPasses へ移設) ---
        // 【この位置で登録すること】依存が同点のときRenderGraphは最小登録番号を選ぶ。
        // 登録順そのものが実行順の一部になっている
        m_GeometryPasses->Register(graph, frameContext, blackboard);

        // --- MegaLightsのパス群(段階6で Passes/MegaLightsPasses へ移設) ---
        // 【この位置で登録すること】依存が同点のときRenderGraphは最小登録番号を選ぶ。
        // 登録順そのものが実行順の一部になっている
        m_MegaLightsPasses->Register(graph, frameContext, blackboard);

        // --- RTシャドウ(段階6で Passes/ShadowPasses へ移設) ---
        m_ShadowPasses->RegisterRaytraced(graph, frameContext);

        // --- 直接光・AO/GI・雲のパス群(段階6で Passes/LightingPasses へ移設) ---
        m_LightingPasses->RegisterDirectAndAO(graph, frameContext, blackboard);

        // --- DDGIの解決(段階6で Passes/DDGIPasses へ移設) ---
        m_DDGIPasses->RegisterResolve(graph, frameContext);

        // --- 合成と半透明のパス群(段階6で Passes/LightingPasses へ移設) ---
        // 【この位置で登録すること】依存が同点のときRenderGraphは最小登録番号を選ぶ
        m_LightingPasses->RegisterSceneLighting(graph, frameContext, blackboard);

        // --- 反射のパス群(段階6で Passes/ReflectionPasses へ移設) ---
        // 【この位置で登録すること】依存が同点のときRenderGraphは最小登録番号を選ぶ。
        // 登録順そのものが実行順の一部になっている
        m_ReflectionPasses->Register(graph, frameContext, blackboard);

        // --- ポストプロセスのパス群(段階6で Passes/PostProcessPasses へ移設) ---
        // 【この位置で登録すること】依存が同点のときRenderGraphは最小登録番号を選ぶ。
        // 登録順そのものが実行順の一部になっている
        m_PostProcessPasses->Register(graph, frameContext, blackboard);

        // --- Present パス群(段階6で Passes/PresentPass へ移設) ---
        // 【この位置で登録すること】RenderGraph は依存が同点のとき最小登録番号を選ぶため、
        // 登録順そのものが実行順の一部になっている。移設で順番が動くと実行順が変わる
        m_PresentPass->Register(graph, commandList, frameContext, blackboard);

        // 1枚だけなら既存のテクスチャダンプと同じフレームを使う。複数枚では焼き込みを捕まえるため最初から出す。
        const uint32_t manifestTargetFrame =
            m_TextureDumpFrame >= 0 ? static_cast<uint32_t>(m_TextureDumpFrame) : kMegaLightsAccumWarmup;
        const bool writeSingleManifest =
            m_PassManifestTargetFrames == 1 && !m_PassManifestIssued && m_TAAFrameIndex >= manifestTargetFrame;
        const bool writeManifestSequence =
            m_PassManifestTargetFrames > 1 && m_PassManifestIssuedFrames < m_PassManifestTargetFrames;
        if (!m_PassManifestPath.empty() && (writeSingleManifest || writeManifestSequence))
        {
            const uint32_t manifestSequenceIndex = m_PassManifestIssuedFrames;
            m_PassManifestIssued = true;
            ++m_PassManifestIssuedFrames;
            std::string executionOrderError;
            const std::string manifest = graph.BuildPassManifest(&executionOrderError);
            if (!executionOrderError.empty())
            {
                Core::Logger::Error("KurenaiEngine3D", "パスマニフェストの実行順を解決できませんでした: " + executionOrderError);
            }

            const std::wstring outputPath = MakeTextureDumpSequencePath(
                m_PassManifestPath, m_PassManifestTargetFrames, manifestSequenceIndex);
            std::ofstream file(outputPath, std::ios::binary);
            if (!file)
            {
                Core::Logger::Error(
                    "KurenaiEngine3D", "パスマニフェストを書き出せませんでした(ファイルを開けない): " +
                        Core::WideToUtf8(outputPath));
            }
            else
            {
                file.write(manifest.data(), static_cast<std::streamsize>(manifest.size()));
                if (!file)
                {
                    Core::Logger::Error(
                        "KurenaiEngine3D", "パスマニフェストを書き出せませんでした(書き込み失敗): " +
                            Core::WideToUtf8(outputPath));
                }
                else
                {
                    Core::Logger::Info(
                        "KurenaiEngine3D", "パスマニフェストを書き出しました: " + Core::WideToUtf8(outputPath));
                }
            }
        }

        graph.Execute();

        // --- メッシュレットカリングの統計を読み戻す(Stage 5-2) ---
        //
        // 【GPUを待たない】直前に積んだコピーはまだ実行されていないので、リングの中で
        // **最も古いもの**(kMeshletCullStatsRingSize-1 = 2フレーム前に書いたもの)を読む。
        // DX12はkFrameCount(=2)フレームぶんCPUが先行するため、2フレーム前のGPU実行は
        // 完了している。待ちを入れるとフレームが直列化し、計測のために計測対象を壊す。
        //
        // 【読めなかったフレームは足さない】DX11のMap(DO_NOT_WAIT)はまだ実行中ならfalseを返す。
        // 0として集計に足すと間引き率が実際より低く出るので、そのフレームは丸ごと飛ばす
        if (meshletCullStatsActive)
        {
            const uint32_t oldestIndex = (m_MeshletCullStatsRingIndex + 1) % kMeshletCullStatsRingSize;
            uint32_t counters[kMeshletCullStatsCount] = {};
            if (m_MeshletCullStatsReadback[oldestIndex] &&
                m_MeshletCullStatsReadback[oldestIndex]->ReadbackData(counters, sizeof(counters)))
            {
                m_RenderStats.MeshletCullTested = counters[0];
                m_RenderStats.MeshletCullFrustumCulled = counters[1];
                m_RenderStats.MeshletCullOcclusionCulled = counters[2];

                m_FrameStatsMeshletTestedSum += m_RenderStats.MeshletCullTested;
                m_FrameStatsMeshletFrustumCulledSum += m_RenderStats.MeshletCullFrustumCulled;
                m_FrameStatsMeshletOcclusionCulledSum += m_RenderStats.MeshletCullOcclusionCulled;
                ++m_FrameStatsMeshletSampleCount;
            }
            m_MeshletCullStatsRingIndex = (m_MeshletCullStatsRingIndex + 1) % kMeshletCullStatsRingSize;
        }
        else
        {
            // 統計を切っている間に古い値が残っていると、UIやログが「今もこの数だけ間引いている」
            // ように見える。切った時点で0へ戻す
            m_RenderStats.MeshletCullTested = 0;
            m_RenderStats.MeshletCullFrustumCulled = 0;
            m_RenderStats.MeshletCullOcclusionCulled = 0;
        }

        // --- モデル単位のGPUカリングの結果を読み戻す(Stage 5-3) ---
        // リングの理由も「読めなかったフレームは足さない」もメッシュレット統計と同じ
        if (blackboard.ModelCullReady)
        {
            // 今フレームのCPU側の結果を、GPUのコピーとまったく同じ位置へ積む。
            // 読むときに同じ位置から取れば、比べるのは同じフレームのもの同士になる
            m_ModelCullCpuFrustumHistory[m_ModelCullRingIndex] = m_ModelCullCpuFrustumCulled;
            // 【比べる相手はG-Bufferぶんの候補数】GPU側の「判定」もそこだけを数えている
            m_ModelCullCandidateHistory[m_ModelCullRingIndex] =
                m_ModelCullCandidateCount - m_ModelCullPrepassCandidateCount;

            const uint32_t oldest = (m_ModelCullRingIndex + 1) % kMeshletCullStatsRingSize;
            uint32_t counters[kModelCullCounterCount] = {};
            if (m_ModelCullReadback[oldest] &&
                m_ModelCullReadback[oldest]->ReadbackData(counters, sizeof(counters)))
            {
                m_ModelCullTested = counters[0];
                m_ModelCullFrustumCulled = counters[1];
                m_ModelCullOcclusionCulled = counters[2];
                m_ModelCullSurvived = counters[3];
                for (uint32_t region = 0; region < kModelCullRegionCount; ++region)
                {
                    m_ModelCullRegionIssued[region] = counters[4 + region];
                }
                m_ModelCullComparedCpuFrustumCulled = m_ModelCullCpuFrustumHistory[oldest];
                m_ModelCullComparedCandidateCount = m_ModelCullCandidateHistory[oldest];
            }
            m_ModelCullRingIndex = (m_ModelCullRingIndex + 1) % kMeshletCullStatsRingSize;
        }
        else
        {
            m_ModelCullTested = 0;
            m_ModelCullFrustumCulled = 0;
            m_ModelCullOcclusionCulled = 0;
            m_ModelCullSurvived = 0;
            std::fill(std::begin(m_ModelCullRegionIssued), std::end(m_ModelCullRegionIssued), 0u);
        }

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
        m_SwapChain->Present(m_SystemSettings.VSyncEnabled);
        m_CPUProfiler.EndScope(); // PresentSubmit

        // GPUの完了待ち(DX12のフレームパイプライン化に伴うフェンス待ち)は実際のCPU負荷ではなく
        // GPU側の処理時間の反映なので、PresentSubmitの計測値からは除外しておく
        m_CPUProfiler.SubtractFromScope("PresentSubmit", m_Device->GetLastFrameGPUWaitTimeMs());

        // --- 次フレームがこのフレームを「前フレーム」として参照するための状態を確定させる ---
        // 早期returnより後のここで行うことで、描画を行わなかったフレームでは前フレームの状態が
        // そのまま保たれ、履歴テクスチャの中身と行列の対応が1フレームずれない
        m_TAAPrevViewProj = constants.ViewProj;
        m_TAAPrevJitterUv = jitterUv;
        // Hi-Zオクルージョンカリングが「1フレームぶんの視差ずれ」を見積もるのに使う。
        // m_TAAPrevViewProjと同じ場所・同じタイミングで書くので有効性の管理も同じで済む
        m_PrevCameraPosition = { constants.CameraPosition.x, constants.CameraPosition.y, constants.CameraPosition.z };
        m_TAAPrevViewProjValid = true;
        // --- GPU計測の書き出し(計測専用) ---
        // 【毎フレーム走る場所へ置くこと】Perfログを出す関数は1秒に1回しか先へ進まない
        // (集計期間に達するまで早期returnする)。そこへ置くと収集が毎秒になり、
        // 300フレーム集めるのに5分かかって測定が終わらない
        // 【Perfログとは別に集める】あちらは0.05ms未満を落とし1フレームの代表値しか出さない。
        // ここでは**閾値なしで全パスを、指定枚数ぶん平均**する
        if (!m_PerfDumpPath.empty() && !m_PerfDumpDone && m_GPUProfiler)
        {
            ++m_PerfDumpWarmupFrames;
            // 整定を待つ。内部解像度の切り替えとストリーミングが片付くまで
            if (m_PerfDumpWarmupFrames > static_cast<int32_t>(kMegaLightsAccumWarmup))
            {
                for (const RHI::GPUTimingResult& pass : m_GPUProfiler->GetResults())
                {
                    m_PerfDumpTotals[pass.Name] += static_cast<double>(pass.TimeMs);
                }
                ++m_PerfDumpCollected;

                if (m_PerfDumpCollected >= m_PerfDumpTargetFrames)
                {
                    m_PerfDumpDone = true;
                    std::ofstream file(m_PerfDumpPath, std::ios::trunc);
                    if (file)
                    {
                        file << "pass,avg_ms\n";
                        for (const auto& entry : m_PerfDumpTotals)
                        {
                            file << entry.first << ','
                                 << (entry.second / static_cast<double>(m_PerfDumpCollected)) << '\n';
                        }
                        file << "__frames," << m_PerfDumpCollected << '\n';
                        Core::Logger::Info(
                            "KurenaiEngine3D",
                            "GPU計測を書き出しました: " + Core::WideToUtf8(m_PerfDumpPath) + " (" +
                                std::to_string(m_PerfDumpCollected) + "フレームの平均)");
                    }
                    else
                    {
                        Core::Logger::Error(
                            "KurenaiEngine3D",
                            "GPU計測を書き出せませんでした(ファイルを開けない): " +
                                Core::WideToUtf8(m_PerfDumpPath));
                    }
                }
            }
        }

        // --- 中間レンダーターゲットの生値ダンプ(検証専用) ---
        // 【perfdumpと同じく毎フレーム走る場所へ置く】積んだコピーを数フレーム後に読む仕組みなので、
        // ここが毎フレーム呼ばれないと待ちフレームがいつまでも進まない
        ResolveTextureDumps();

        m_TAAPrevEffectiveExposureEV100 = m_EffectiveExposureEV100;

        // MegaLightsの時間再利用も同じ場所でping-pongを反転する。
        // 今フレームの書き込み先が、次フレームでは履歴(読み込み元)になる
        {
            const bool temporalRan = ShouldRunMegaLights() && m_MegaLightsSettings.Mode == MegaLightsMode::Stochastic &&
                                     m_MegaLightsSettings.TemporalEnabled && m_MegaLightsTemporalPipelineState &&
                                     m_MegaLightsReservoirHistory[0] && m_MegaLightsHistoryGuide[0];
            // 【手法3もガイドを書くので同じ反転が要る】あちらは時間再利用を持たないが、
            // デノイザが読む「前フレームの幾何」を Resolve が書いている。反転しないと
            // 同じフレームで書いた側を読むことになり、比べたい「別のフレームの同じ点」に
            // ならない(そのうえ RenderGraph は WAR の辺を張らないので競合する)
            const bool quadGuideRan = ShouldRunMegaLights() &&
                                      m_MegaLightsSettings.Mode == MegaLightsMode::QuadShared &&
                                      m_MegaLightsResolvePipelineState && m_MegaLightsHistoryGuide[0];
            if (temporalRan || quadGuideRan)
            {
                m_MegaLightsHistoryIndex ^= 1u;
                // 【1フレーム走ってから有効にする】書いた側を次フレームが読むので、
                // 反転したあとに立てる。立てるのが早いと未初期化の内容を履歴として読む
                m_MegaLightsHistoryValid = true;
            }
            else
            {
                // 走らなかったフレームを挟むと履歴が途切れる(中身が古い or 未初期化)
                m_MegaLightsHistoryValid = false;
            }
            // 露出はパスの有無に関わらず記録する(次に走ったときの比較の基準になる)
            m_MegaLightsPrevEffectiveExposureEV100 = m_EffectiveExposureEV100;

            // デノイザの履歴も同じ場所で反転する。今フレームの書き込み先が次フレームの履歴になる
            // 【時間再利用の有無には依存しない】デノイザは「出た色」をならすもので、
            // リザーバを混ぜる時間再利用とは独立に効く。条件を混ぜると、片方を切ったときに
            // もう片方の履歴まで無効になって原因が分からなくなる
            const bool denoiseRan = ShouldRunMegaLights() &&
                                    (m_MegaLightsSettings.Mode == MegaLightsMode::Stochastic ||
                                     m_MegaLightsSettings.Mode == MegaLightsMode::QuadShared) &&
                                    m_MegaLightsSettings.DenoiseEnabled && m_MegaLightsDenoiseTemporalPSO &&
                                    m_RenderTargets.MegaLightsDenoisedTexture != nullptr;
            if (denoiseRan)
            {
                m_MegaLightsDenoiseHistoryIndex ^= 1u;
                m_MegaLightsDenoiseHistoryValid = true;
            }
            else
            {
                // 走らなかったフレームを挟むと履歴が途切れる(中身が古い)
                m_MegaLightsDenoiseHistoryValid = false;
            }
        }

        if (m_PostProcessSettings.TAAEnabled)
        {
            // 今フレームの書き込み先が、次フレームでは履歴(読み込み元)になる
            m_TAAHistoryIndex ^= 1u;
            m_TAAHistoryValid.store(true, std::memory_order_relaxed);
        }
        else
        {
            // 無効の間は履歴を更新していないので、再度有効化されたときに古い絵が混ざらないよう落としておく
            m_TAAHistoryValid.store(false, std::memory_order_relaxed);
        }
    }
}
