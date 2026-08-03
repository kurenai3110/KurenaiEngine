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
#include "UI/UIManager.h"
#include "UI/UITheme.h"

namespace Kurenai
{
    namespace
    {
        using Core::GetModuleDirectory;
        using Core::WideToUtf8;

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
            // w: スペキュラのマルチスキャッタリング・エネルギー補正の方式
            // (m_SpecularCompensationMode。0=Off / 1=Linear / 2=Series / 3=Kulla-Conty。
            // 共有ヘッダーSpecularEnergy.hlsliのKURENAI_SPEC_COMP_*と一致させること。14.9節)
            DirectX::XMFLOAT4 ShadowParams;
            // 半透明パス(Transparent.hlsl)専用。x=t8のライトリストの有効数。DirectLighting.hlslは
            // 専用のLightingConstants(b1)で受け取るためこのフィールドを使わない(末尾に追加のため
            // 既存シェーダのオフセットは変わらない)
            DirectX::XMFLOAT4 ActiveLightCount;
            // 拡散IBLの取得元切り替え(末尾に追加のため既存シェーダのオフセットは変わらない)。
            // x: 0(既定)=プリフィルタ済み鏡面の最終ミップ(roughness=1)、1=従来の専用
            // イラディアンスマップ(t8。検証用に残している経路)。CSPrefilterはV=R=Nを仮定して
            // いるため、roughness=1(α=1)ではGGXインポータンスサンプリングの実効カーネルが
            // コサイン畳み込みへ厳密に退化し、格納値もCSIrradianceと同じE(N)/πになる(14.10節)。
            // 反射プローブの拡散イラディアンスにもまったく同じ規則を適用する(19.7節)
            DirectX::XMFLOAT4 IBLParams;
            // 反射プローブ用(末尾に追加)。x=有効プローブ数(0ならプローブを使わずグローバルIBLのみ)、
            // y=影響範囲のデバッグ表示フラグ、z=視差補正の有効フラグ、w=プローブ間ブレンドの有効フラグ。
            // DeferredLighting.hlslとSSR.hlslが読む
            DirectX::XMFLOAT4 ProbeParams;
            // 反射プローブの距離キューブ用(末尾に追加、19.12節)。x=視差補正に距離キューブを使うフラグ、
            // y=距離キューブによる遮蔽判定(光漏れ抑制)の有効フラグ、z=距離キューブの1面の解像度
            // (テクセル。ReflectionProbe.hlsliのProbeDistanceBiasが1テクセル幅の見積もりに使う。
            // ハードコードせずここから渡すのは、kProbeCaptureSizeを変えたときに黙ってずれないため)、
            // w=焼いた時点の実効プリ露出から現在の実効プリ露出への換算倍率(19.14節。
            // m_ProbeBakedExposureEV100のコメントに理由がある)
            DirectX::XMFLOAT4 ProbeParams2;
            // TAA用(末尾に追加のため既存シェーダのオフセットは変わらない)。前フレームの
            // ビュー射影行列(TAAのジッターを含んだままのもの)。GBuffer.hlslが頂点をこの行列でも
            // 変換し、今フレームの投影位置との差からモーションベクター(速度)を求める。
            // 初回フレームとTAAの履歴リセット時は今フレームのViewProjと同じ値を入れる
            // (未定義値が速度バッファへ焼き込まれるのを防ぐため)
            DirectX::XMFLOAT4X4 PrevViewProj;
            // TAAのサブピクセルジッター量(末尾に追加)。xy=今フレーム、zw=前フレーム。
            // 単位はUV(=ピクセルオフセット / レンダー解像度)。
            //
            // 【なぜ速度からジッターを引くのか】ジッターは投影行列に入れてあるので、ViewProjと
            // PrevViewProjで投影した位置の差にはジッターの差も混ざる。しかしジッターは
            // 「同じ面のどこをサンプルしたか」の違いであって「ものが動いた量」ではない。
            // 引いておかないとTAAが履歴を引く位置が毎フレーム±0.5px揺れ、いつまでも収束しない
            DirectX::XMFLOAT4 TAAParams;
            // DDGI用(さらに末尾に追加、22章)。サンプリング側(DeferredLighting.hlsl)が必要とする値だけを
            // 置く。ヒステリシスや最大レイ距離は焼く側にしか要らないのでDDGIUpdateConstantsが持つ。
            //   DDGIParams0: xyz=ボリュームの最小コーナー(ワールド)、w=有効フラグ(0なら従来のIBLのまま)
            //   DDGIParams1: xyz=プローブ間隔、w=法線バイアス(遮蔽判定の照会点を面から浮かせる量)
            //   DDGIParams2: xyz=各軸のプローブ数、w=視線バイアス
            //   DDGIParams3: x=イラディアンスの1辺のテクセル数(境界を含まない)、
            //                y=距離モーメントの1辺のテクセル数(同)、z=拡散間接光の強度倍率、w=未使用
            // テクセル数をハードコードせずここから渡すのは、ProbeParams2.zと同じ理由
            // (C++側の定数を変えたときにシェーダーとの対応が黙ってずれないため)
            DirectX::XMFLOAT4 DDGIParams0;
            DirectX::XMFLOAT4 DDGIParams1;
            DirectX::XMFLOAT4 DDGIParams2;
            //                y=距離モーメントの1辺のテクセル数(同)、z=拡散間接光の強度倍率、
            //                w=境界の幅(テクセル)
            DirectX::XMFLOAT4 DDGIParams3;
            // DDGIParams4: x=このフレームの実効プリ露出(m_EffectiveExposureEV100の線形倍率)、yzw=未使用。
            //
            // 【アトラスは露出非依存の単位で持つ】ライトの色にはCPU側で実効プリ露出が
            // 事前乗算されており(21.5節)、その倍率は時刻に連動して最大18段(約26万倍)動く。
            // アトラスへプリ露出済みの値をそのまま溜めると、時刻が変わった瞬間に
            // 「古い露出で焼かれた数値」を新しい露出の値として読むことになる。
            // DDGIは多重バウンスで自分自身へフィードバックするため、このズレが増幅され続け、
            // 夜を挟んで昼に戻すと画面が数倍明るいまま戻らなくなる(実測で12時の平均輝度が
            // 45.6→132.9)。そこで書き込み時にこの倍率で割り、読み出し時に掛け直して、
            // アトラスの中身を露出に依存しない物理量に保つ。
            // R32で確保してある(22.6節)ので、夜の小さな値でもfp32の範囲に余裕がある
            DirectX::XMFLOAT4 DDGIParams4;
            // 水面用(さらに末尾に追加、P2: 水面マテリアル基盤)。x=水面法線マップのスクロール
            // オフセット(0〜1、CPU側で既にfmod済み)、y=波のスケール倍率(m_WaterWaveScale)、
            // z=波の強さ(m_WaterWaveStrength、0〜1)、w=未使用。Water.hlslのPSMainが読む。
            // 末尾に足す限り、既に宣言済みのシェーダのcbufferオフセットは1バイトも動かない
            // (DDGIParams0〜4を末尾に追加したときと同じ規約)
            DirectX::XMFLOAT4 TimeParams;
            // 空の解析評価用(さらに末尾に追加、P3)。DeferredLighting.hlslが背景画素で
            // Sky.hlsliのSkyColorを画面解像度で評価するために使う。太陽方向以外の値
            // (ティント4本・天頂輝度)は手続き空のベイクと同じタイミングでSkyIntegrate.hlslが
            // m_SkyParametersBufferへ書き、両者が同じ空を描くことを保証する(P9)。
            // SkySunDirection: xyz=太陽が「ある」向き、w=未使用。
            //   【正規化はシェーダ側で行う】sunLighting.SunPositionは解析的にはほぼ単位長だが、
            //   SkyGenerate.hlsl側の慣習(呼び出し側=SkyParameters組み立て時にnormalizeする)に
            //   合わせ、C++側では正規化せずそのまま渡す(DeferredLighting.hlslのMakeSkyParameters参照)。
            //   **LightDirectionでは代用できない**——あちらは支配ライトの向きで、月が支配的な
            //   夜には月の向きになる。Perez分布のcircumsolar項は常に太陽を基準にする
            DirectX::XMFLOAT4 SkySunDirection;
            // SkyParams: x=未使用(P9で天頂輝度はSkyParametersBufferへ移動)、
            //   y=背景を解析評価するかのフラグ(1=解析、0=キューブマップをサンプル)、
            //   z=太陽照度/空照度比(SunToSkyIlluminanceRatio。sunLighting.KeyIlluminanceLux /
            //   sunLighting.SkyIlluminanceLuxから求める。Sky.hlsliのEvaluateCloudLayerが雲の
            //   明るさの基準を太陽の照度にするために使う。雲を照らしているのは空ではなく
            //   太陽であるため、天頂輝度基準では雲が原理的に空より暗くしかならなかった
            //   問題への対処)、w=未使用。
            //   yは手続き空が無効(.ksceneのDDSスカイボックス使用時)は常に0にする
            //   (DDSは任意の絵でPerezモデルとは無関係なため、解析評価してはいけない)。
            //   ティント4本(SkyZenithTint/SkyHorizonTint/SkyGroundTint/SkySunGlowTint)は
            //   P9でm_SkyParametersBuffer(GPUSkyParameters、SkyIntegrate.hlslが書く)へ移り
            //   このFrameConstantsからは削除した(DeferredLighting.hlsl/SSR.hlslのFrameConstants
            //   宣言も同時に更新済み。フィールドを削ると後続のオフセットが全部ずれるため、
            //   末尾のCloudParams0/1・PlanarReflectionPlaneまで含めて3シェーダーと1フィールドずつ
            //   突き合わせて一致を確認すること)
            DirectX::XMFLOAT4 SkyParams;
            // 雲(さらに末尾に追加、P5)。DeferredLighting.hlsl/SSR.hlslのFrameConstants宣言と
            // 同じ順・同じ型であること(2つのシェーダーが背景と水面反射で同じ雲を描くための前提。
            // Sky.hlsli冒頭のコメント・各シェーダーのMakeSkyParametersのコメント参照)。
            // CloudParams0: x=被覆率(0で雲なし。Sky.hlsliのSkyColorが早期脱出する)、
            //               y=雲底の高度[m](カメラのワールドY基準)、
            //               z=UVスケール[ノイズ空間の距離/m]、w=消散係数
            DirectX::XMFLOAT4 CloudParams0;
            // CloudParams1: xy=風によるノイズ空間の移動量(CPU側でSky.hlsliのkCloudNoisePeriodと
            //               同じ周期でstd::fmod済み。m_CloudScrollOffset参照)、
            //               z=Henyey-Greensteinの非対称パラメータ、w=未使用
            DirectX::XMFLOAT4 CloudParams1;
            // 巻雲(P11、さらに末尾に追加)。DeferredLighting.hlsl/SSR.hlsl/PlanarReflection.hlslの
            // FrameConstants宣言と同じ順・同じ型であること(3シェーダーすべてを更新すること。
            // 末尾のPlanarReflectionPlaneを含めて1フィールドずつ突き合わせて一致を確認すること)。
            // CloudParams2: x=巻雲の被覆率(0で巻雲なし。Sky.hlsliのSkyColorが早期脱出する)、
            //               y=雲底の高度[m](カメラのワールドY基準)、
            //               z=UVスケール[ノイズ空間の距離/m]、w=消散係数
            DirectX::XMFLOAT4 CloudParams2;
            // CloudParams3: xy=風によるノイズ空間の移動量(CPU側でSky.hlsliのkCloudNoisePeriodと
            //               同じ周期でstd::fmod済み。m_CirrusScrollOffset参照)、
            //               z=fBmのUV(U方向)を伸ばす異方性スケール(m_CirrusAnisotropy)、w=未使用
            DirectX::XMFLOAT4 CloudParams3;
            // 平面反射(P6、さらに末尾に追加)。xyz=水面平面の法線(現状は常に(0,1,0))、
            // w=平面の距離項。PlanarReflection.hlslのVSMainが
            // SV_ClipDistance0 = dot(worldPos, xyz) + w として使い、水面より上で正になるようにする
            // (水面より下のジオメトリを反射に映さないため)。このシェーダー以外は参照しない
            DirectX::XMFLOAT4 PlanarReflectionPlane;
            // 大気遠近(P8、さらに末尾に追加)。AerialPerspective.hlsl/PlanarReflection.hlslが読む。
            // x=基準高度での消散係数[1/m]、y=スケールハイト[m]、z=基準高度[m](ワールドY)、
            // w=有効フラグ(0で無効。UIでオフ、またはシーンが手続き空を使っていない場合に0にする。
            // 手続き空が無効なシーンでは大気遠近のin-scatter項(SkyColorの解析評価)が意味を持たない
            // ため、SSR.hlslのwaterAnalyticSkyFlagと同じ判断をRender()側で行う)
            DirectX::XMFLOAT4 FogParams0;
            // x=不透明度の上限(1.0で完全に空の色まで行く)、yzw=未使用
            DirectX::XMFLOAT4 FogParams1;
            // 水中項(P8、さらに末尾に追加)。xyz=水体の色(リニア)、w=未使用。Water.hlslのPSMainが
            // メッシュ自身のBaseColorFactorの代わりにこの色を出力Albedoに使う
            // (見下ろした水面がFresnel最小(約0.02)でほぼ真っ黒になる問題への対処。詳細はWater.hlsl参照)
            DirectX::XMFLOAT4 WaterBodyColor;
        };

        // DDGIのプローブ更新CS(DDGIProbeUpdate.hlsl)専用の定数バッファ。
        // 焼く側にしか要らない値(どのプローブを焼いているか・ヒステリシス・距離のクランプ上限)を持つ
        struct alignas(16) DDGIUpdateConstants
        {
            // x=いま焼いているプローブの通し番号、y=ヒステリシス、z=距離モーメントのクランプ上限、
            // w=キャプチャキューブの1面の解像度(レイの立体角の重み付けに使う)
            DirectX::XMFLOAT4 Params0;
            // x=イラディアンスの1辺のテクセル数(境界を含まない)、y=距離モーメントの1辺のテクセル数、
            // z=境界の幅、w=履歴を無視して上書きするフラグ(初回ベイク時に1。
            // ヒステリシスは「前の値」があって初めて意味を持つため、未初期化のアトラスと混ぜてはいけない)
            DirectX::XMFLOAT4 Params1;
            // xyz=アトラス上でのプローブ格子の並び(x=各軸のプローブ数)。アトラスの列数は
            // ProbeCounts.x * ProbeCounts.y、行数はProbeCounts.zになる。
            // w=このフレームの実効プリ露出。積分した放射輝度をこれで割ってから格納する
            // (理由はFrameConstants::DDGIParams4のコメント参照)
            DirectX::XMFLOAT4 Params2;
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
            // 支配ライト(太陽 or 月)の、光が進む向き(サーフェスに当たる方向)。
            // カスケードシャドウの行列もこの向きから作ること
            DirectX::XMFLOAT3 Direction;
            DirectX::XMFLOAT4 Color;
            DirectX::XMFLOAT4 Ambient; // rgb=環境光の色, a=昼度(0=夜,1=昼)
            // 支配ライトが太陽か月か(ImGuiの表示とデバッグ用)
            bool DominantIsSun;
            // 手続き空の天頂輝度を正規化する際の目標照度[lx]。薄明係数と月明かりで変調済み
            float SkyIlluminanceLux;
            // 薄明係数(仰角[-15°,+15°] = 時刻でちょうど5-7時/17-19時)
            float TwilightFactor;
            // 太陽が「ある」向き。手続き空(SkyGenerate.hlsl)がPerez分布のcircumsolar項の
            // 基準に使う。月が支配的なときも**常に太陽の位置**であることに注意
            DirectX::XMFLOAT3 SunPosition;
            // このフレームのキーとなる照度[lx]。可変プリ露出の基準になる
            float KeyIlluminanceLux;
        };

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
        // m_AutoExposureNightRolloffEV(夜の露出切り詰め量)を動かすこと
        constexpr float kMoonSkyIlluminanceLux = 0.05f;
        // 星明かりだけの夜空の照度[lx]。月が地平線下にあるときの下限になる。
        // 月の位置が手動指定になったことで「月の出ていない夜」がスライダー一つで作れるように
        // なったが、そこで夜空の目標照度が厳密に0になると空が真っ黒になり、
        // 自動露出が持ち上げようのない画になる。星明かりは実在する量(約0.001lx)なので、
        // アート的な下駄ではなく物理値としてここに置く
        constexpr float kStarlightIlluminanceLux = 0.001f;

        // edge0とedge1の間をなめらかに0→1で補間する(edge0以下は0、edge1以上は1)
        float Smoothstep(float edge0, float edge1, float x)
        {
            const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        }

        // --- 手続き空(SkyGenerate.hlsl)の色味・照度正規化はP9でGPU側(SkyIntegrate.hlsl)へ
        //     一本化した。以前はComputeSkyTint/ComputeSkyZenithScale等としてここにCPUミラーが
        //     あり、Sky.hlsliの同じ式と「片方を直したら必ずもう片方も直す」規約でしか整合を
        //     保てなかった。GPUSkyParameters/m_SkyParametersBufferの定義とコメントは
        //     このファイル内の該当箇所(GPU用構造体の宣言、Render()のbakeSkyThisFrameブロック)を
        //     参照。式の実体はShaders/3D/Sky.hlsliのComputeSkyTintSet/PerezRelativeLuminance/
        //     SkyTintFromSetと、それを呼ぶShaders/3D/SkyIntegrate.hlslにある ---

        // Sky.hlsliのkCloudNoisePeriodと同じ値であること(P5)。CPU側(RenderThreadMainの
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

        // 巻雲側(P11)の「全天が巻雲のときの透過率」。積雲のkCloudOvercastTransmittance(0.35)より
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

        // 被覆率から求める全天の平均透過率(判断B)。P11で巻雲(2層目)を加味し、
        // T = T_cumulus(積雲の被覆率) * T_cirrus(巻雲の被覆率) という2層の積の形へ拡張した。
        // 巻雲を無効化・被覆率0にした場合はT_cirrus=1.0になり、積雲だけだったP5〜P10と
        // 同じ値に戻る
        float ComputeCloudAverageTransmittance(
            bool cloudEnabled, float coverage, bool cirrusEnabled, float cirrusCoverage)
        {
            const float cumulusTransmittance =
                ComputeCloudLayerTransmittance(cloudEnabled, coverage, kCloudOvercastTransmittance);
            const float cirrusTransmittance =
                ComputeCloudLayerTransmittance(cirrusEnabled, cirrusCoverage, kCirrusOvercastTransmittance);
            return cumulusTransmittance * cirrusTransmittance;
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
            // 【なぜ時刻ベースをやめたか】従来は Smoothstep(6,7) * (1 - Smoothstep(17,18)) と
            // 時刻で遷移させていた。この窓は仰角0度〜15度にちょうど一致しており偶然うまく
            // 成立していたが、遷移を長くしようと窓を5-7時/17-19時へ広げると
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
            // 「常に満月かつ常に真夜中に南中する」という二重の簡略化だった。
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
            // 太陽と月は「支配的な方」を選んで切り替える。月を反太陽方向に固定していたときは
            // 切替点(太陽の仰角0度)で月の係数もちょうど0になり、向きが反転しても
            // 何も見えないためポップが原理的に起きなかった。
            // 月の位置が独立になるとこの保証が失われ、太陽が沈む瞬間に月が高く昇っていると
            // 0.25lxの直接光が向きだけ突然入れ替わる(夜の影が見える明るさなので実際に目に付く)。
            // そこで月の立ち上がりを太陽の仰角0°→-5°に遅らせ、
            // **切替点では太陽も月も厳密に0**という元の性質を取り戻す
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
            // 空が届ける照度は薄明係数で変調する。GPU側の照度正規化積分(SkyIntegrate.hlsl、P9)
            // により「目標照度ちょうど」が保証されるようになったので、時刻による空の明るさは
            // ここの係数だけで素直に制御できる。
            // 夜側は月明かりで散乱する空の照度を足す(満月時の夜空はおよそ0.05lx相当)。
            // 月が地平線下でも星明かりぶんは残る(月の位置を手動指定にしたことで
            // 「月の出ていない夜」が作れるようになったため、そこで0にしない)
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

        // Shaders/GBuffer.hlsl・Shaders/Shadow.hlslのObjectConstants(register b1)と
        // レイアウトを一致させる必要がある。DX12のルートシグネチャがCBVをb0/b1の2枠しか
        // 持たないため、モデル行列もマテリアル係数(Emissive/AlphaCutoff含む)と同居させている
        // (Architecture.html参照)。float3(EmissiveFactor)以降が16バイト境界をまたがないよう、
        // 直前のMetallicFactor/RoughnessFactor/TangentSignFlip/AlphaCutoffで先に16バイトを
        // 埋めてからEmissiveFactor+OcclusionStrengthで次の16バイトを埋める配置にしている
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
            // glTFのocclusionTexture.strength(既定1.0)。かつては純粋な詰め物(Padding)だった枠を
            // そのまま使っているため、定数バッファのサイズ・オフセットは一切変わっていない
            float OcclusionStrength;
            // glTFのbaseColorFactor(既定[1,1,1,1])。BaseColorTextureと乗算して使う。
            // GBuffer.hlsl(不透明)・Transparent.hlsl(半透明)・ProbeCapture.hlsl(プローブ焼き込み)
            // が同じ位置で宣言している。Shadow.hlslは深度しか書かないため先頭までしか宣言していないが、
            // 定数バッファの末尾を読まないだけなのでレイアウトの不一致にはならない(14章参照)
            float BaseColorFactor[4];
            // マテリアル種別ID(末尾に追加、P2: 水面マテリアル基盤)。0=通常マテリアル、
            // 1=水面(kMaterialIDWater、Shaders/3D/GBufferCommon.hlsliの値と一致させること)。
            // 末尾に足す限り、既に宣言済みのシェーダのcbufferオフセットは1バイトも動かない
            // (Shadow.hlsl等が先頭までしか宣言していなくても影響しない、という上のBaseColorFactorの
            // コメントと同じ理由)
            float MaterialID;
        };

        // instance.World/NormalMatrix/TangentSignFlipはAssets::LoadScene(SceneLoader.cpp)が
        // TRS(平行移動・回転・スケール)から計算済み(HLSL側のmul(vec, matrix)規約に合わせて
        // 転置済み)なので、ここでは単純にコピーするだけでよい
        // emissiveIntensity: シーン全体の自発光の強度倍率(m_EmissiveIntensity)。glTFの
        // emissiveFactorは通常1.0以下に収まるため、これを掛けないとG-Bufferのエミッシブを
        // HDR化しても照明器具の輝度が1.0を超えず、ブルームが効かない
        // occlusionMapEnabled: マテリアルの遮蔽マップを使うか(m_OcclusionMapEnabled)。
        // 各パスは lerp(1, occlusionSample, OcclusionStrength) で遮蔽率を求めるため、
        // ここで0を渡せばシェーダー側に手を入れずに遮蔽マップの寄与だけを消せる
        ObjectConstants MakeObjectConstants(
            const Assets::ModelInstance& instance, const Assets::Mesh& mesh, float emissiveIntensity,
            bool occlusionMapEnabled)
        {
            ObjectConstants constants{};
            constants.World = instance.World;
            constants.NormalMatrix = instance.NormalMatrix;
            constants.MetallicFactor = mesh.MetallicFactor;
            constants.RoughnessFactor = mesh.RoughnessFactor;
            constants.TangentSignFlip = instance.TangentSignFlip;
            constants.AlphaCutoff = mesh.AlphaCutoff;
            constants.EmissiveFactor[0] = mesh.EmissiveFactor[0] * emissiveIntensity;
            constants.EmissiveFactor[1] = mesh.EmissiveFactor[1] * emissiveIntensity;
            constants.EmissiveFactor[2] = mesh.EmissiveFactor[2] * emissiveIntensity;
            constants.OcclusionStrength = occlusionMapEnabled ? mesh.OcclusionStrength : 0.0f;
            constants.BaseColorFactor[0] = mesh.BaseColorFactor[0];
            constants.BaseColorFactor[1] = mesh.BaseColorFactor[1];
            constants.BaseColorFactor[2] = mesh.BaseColorFactor[2];
            constants.BaseColorFactor[3] = mesh.BaseColorFactor[3];
            // 水面(kMaterialIDWater、Shaders/3D/GBufferCommon.hlsliと一致させること)。
            // 水面以外は0.0f(通常マテリアル)のまま
            constants.MaterialID = instance.IsWater ? 1.0f : 0.0f;
            return constants;
        }

        // Present.hlsl側のModeと一致させる必要がある
        struct alignas(16) PresentConstants
        {
            int32_t Mode;
            float MipLevel; // Mode==6(Hi-Z)でSampleLevelに渡すミップレベル
            // Mode==10(シャドウマップ配列)ではカスケード番号、
            // Mode==12(反射プローブのキューブマップ配列)では表示するプローブ番号として使う
            float ArraySlice;
            // デバッグ表示の輝度倍率(m_DebugViewGain)。色として表示するMode 0/3/4にだけ効く
            float Gain;
            // Mode==11(タイルライトカリングのヒートマップ)専用。
            // x=タイル数X, y=タイルの1辺のピクセル数, z=1タイルあたりの容量, w=ヒートマップの上限ライト数
            DirectX::XMFLOAT4 TileParams;
            // Mode==11専用。xy=レンダー解像度(UVからタイル座標を求めるのに使う), zw=未使用
            DirectX::XMFLOAT4 TileRenderSize;
        };

        // Tonemap.hlsl側のcbuffer TonemapConstantsと一致させる必要がある
        struct alignas(16) TonemapConstants
        {
            // KurenaiEngine3D::TonemapCurve(0=Reinhard, 1=ACES, 2=AgX)
            int32_t Curve;
            // 手動露出時に掛ける倍率。プリ露出は時刻連動で変動するため、ユーザー設定EV100との
            // 差分 2^(実効EV100 - 設定EV100) を割り戻して固定露出の絵に戻す(1.0固定ではない)
            float ExposureScale;
            // ディザの強さ(0=無効、1=±1LSB)
            float DitherStrength;
            // 1.0=自動露出、0.0=手動
            float UseAutoExposure;
            // CPU側でライト強度へ事前乗算済みのEV100(プリ露出)
            float PreExposureEV100;
            // ブルームの合成比(0で無効)
            float BloomStrength;
            // 薄明視の適用量(0で無効、1で完全適用)
            float MesopicStrength;
            // 目が順応している明るさ(EV100)。構図にも露出設定にも依存しない
            float MesopicAdaptationEV100;
            // TAAの蓄積で失われた高域を戻すシャープネス(0で無効)。TAAが無効のときは常に0。
            //
            // 【なぜTAAではなくここなのか】以前はTAAの入力へ掛けていたが、アンシャープマスクが
            // 増幅する高域は「ジッターで毎フレーム変動する成分」そのもので、静止時のちらつきを
            // 実測で約53%増やしていた。ここはトーンマップ後のLDR値に対して掛かるだけで
            // どこへもフィードバックされないため、ちらつきにもリンギングの累積にも寄与しない
            float Sharpness;
            // シャープネスの近傍タップに使う1テクセルぶんのUV(1/レンダー解像度)
            float InvRenderWidth;
            float InvRenderHeight;
            float TonemapPadding;
        };

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
            // P7: Preetham xyYモデル用のパラメータ。x=タービディティ、y=Preethamの重み
            // (0=従来ティントのみ、1=Preethamのみ)、zw=予備
            DirectX::XMFLOAT4 ModelParams;
        };

        // SkyIntegrate.hlsl側のcbuffer SkyIntegrateConstantsと一致させる必要がある
        struct alignas(16) SkyIntegrateConstants
        {
            // xyz=太陽が「ある」向き(正規化済み。光が進む向きとは符号が逆)、w=未使用
            DirectX::XMFLOAT4 SunDirection;
            // x=目標照度[lx](SunLighting::SkyIlluminanceLux)、y=実効プリ露出(effectiveExposure)、
            // z=タービディティ(P7、m_SkyTurbidity)、w=未使用
            DirectX::XMFLOAT4 IntegrateParams;
        };

        // SkyGenerate.hlsl側のcbuffer SkyBakeConstantsと一致させる必要がある
        struct alignas(16) SkyBakeConstants
        {
            // 処理対象の面(D3D標準順: +X=0,-X=1,+Y=2,-Y=3,+Z=4,-Z=5)
            uint32_t Face;
            // 雲(P5、判断B)による平均透過率。SkyParametersBuffer[0].Luminance.x
            // (雲を考慮しない晴天基準の天頂輝度)にこの値を掛けてからキューブへ焼く
            float CloudTransmittance;
            float Padding0[2];
            // 太陽が「ある」向き(正規化済み。光が進む向きとは符号が逆)
            DirectX::XMFLOAT4 SunDirection;
        };

        // Bloom.hlsl側のcbuffer BloomConstantsと一致させる必要がある
        struct alignas(16) BloomConstants
        {
            DirectX::XMUINT2 SrcSize;
            DirectX::XMUINT2 DstSize;

            float Threshold;
            float SoftKnee;
            // 1.0なら最初のダウンサンプル(Karis平均としきい値を適用する)
            float ApplyKarisAndThreshold;
            // 1.0=自動露出、0.0=手動(Tonemapと同じ意味)
            float UseAutoExposure;

            // CPU側でライト強度へ事前乗算済みのEV100(プリ露出)
            float PreExposureEV100;
            // 手動露出時に掛ける倍率(TonemapConstants::ExposureScaleと同じ値)
            float ExposureScale;
            float Padding[2];
        };

        // AutoExposure.hlsl側のcbuffer AutoExposureConstantsと一致させる必要がある
        struct alignas(16) AutoExposureConstants
        {
            DirectX::XMUINT2 InputSize;
            float MinEV100;
            float MaxEV100;

            float PreExposureEV100;
            float DeltaTime;
            float AdaptationSpeedUp;
            float AdaptationSpeedDown;

            float LowPercentile;
            float HighPercentile;
            float ExposureCompensation;

            // 暗いシーンをわざと暗いまま写すための補正カーブ(AutoExposure.hlsl参照)
            float NightRolloffEV;
            float NightRolloffDarkEV100;
            float NightRolloffBrightEV100;

            // 測光値の上側クランプ(構図依存を抑える。AutoExposure.hlsl参照)
            float KeyReferenceEV100;
            float KeyCeilingEV;

            // 0以外なら順応を飛ばして測光値へ即座に合わせる(シーン切り替え時。
            // m_AutoExposureResetRequested参照)
            float ResetAdaptation;
            float Padding[3];
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
            // w: 水面の解析空フォールバックを使うか(1=使う、P4)。Render()側で
            // m_WaterAnalyticSkyReflection && usingProceduralSky の両方が立っているときだけ1にする
            // (手続き空が無効なシーンではDDSは任意の絵でPerezモデルとは無関係なため、
            // このトグルの値に関わらず必ず0にする)
            DirectX::XMFLOAT4 Params0; // x: 最大レイ距離, y: ヒット判定の厚み, z: ラフネスカットオフ, w: 水面の解析空フォールバック
            // 平面反射(P6、末尾に追加)。x: 平面反射が有効か(1=使う。m_PlanarReflectionEnabled &&
            // 水面インスタンスが存在するときのみ1)、y: 波の法線による画面UVのずらし量
            // (m_PlanarReflectionDistortion)、zw: 未使用
            DirectX::XMFLOAT4 Params1;
        };

        // RTReflection.hlsl側のcbuffer RTReflectionConstantsと一致させる必要がある
        struct alignas(16) RTReflectionConstants
        {
            DirectX::XMFLOAT4 Params0; // xy: 出力サイズ(ピクセル), z: 最大レイ距離, w: ラフネスカットオフ
            DirectX::XMFLOAT4 Params1; // x: 影レイを撃つか(1で撃つ), yzw: 未使用
        };

        // RTShadow.hlsl側のcbuffer RTShadowConstantsと一致させる必要がある
        struct alignas(16) RTShadowConstants
        {
            // xy: 出力サイズ(ピクセル), z: 太陽の見かけの半径(ラジアン), w: 1ピクセルあたりのレイ本数
            DirectX::XMFLOAT4 Params0;
        };

        // RTAO.hlsl側のcbuffer RTAOConstantsと一致させる必要がある
        struct alignas(16) RTAOConstants
        {
            // xy: 出力サイズ(ピクセル), z: レイの最大距離, w: 遮蔽率のコントラスト(べき乗)
            DirectX::XMFLOAT4 Params0;
            // x: レイ本数, y: 間接光の強さ, z: バウンス面へ影レイを撃つか, w: 未使用
            DirectX::XMFLOAT4 Params1;
        };

        // TAA.hlsl側のcbuffer TAAConstants(register b1)と並びを一致させる必要がある。
        // TAAパスはb0(FrameConstants)を使わず、必要な行列もすべてこちらへ入れている。
        // FrameConstantsは末尾追加を重ねて700バイトを超えており、cbufferは途中のフィールドを
        // 飛ばせないため、末尾の2つを読むためだけに全フィールドを宣言する羽目になるのを避けている
        struct alignas(16) TAAConstants
        {
            DirectX::XMFLOAT4X4 InvViewProj;  // 今フレームのジッター済み逆VP(空の速度の補完に使う)
            DirectX::XMFLOAT4X4 PrevViewProj; // 前フレームのジッター済みVP
            DirectX::XMFLOAT4 JitterUv;       // xy=今フレームのジッター(UV単位), zw=前フレーム
            DirectX::XMFLOAT4 ScreenParams;   // xy=レンダー解像度, zw=その逆数
            // x: 今フレームの色を混ぜる割合(m_TAABlendWeight)
            // y: 近傍クリップのボックス幅(標準偏差の何倍か。m_TAAClipGamma)
            // z: 履歴が使えるか(0=使えない。TAA.hlslは履歴をサンプルすらしない)
            // w: プリ露出の変化を打ち消す倍率(今フレームの露出 / 前フレームの露出)
            DirectX::XMFLOAT4 Params0;
            // x: 近傍クリップの方式(TAAClipMode)
            // y: 静止時のちらつき抑制の強さ(m_TAAAntiFlicker)。zwは未使用
            DirectX::XMFLOAT4 Params1;
        };

        // DirectLighting.hlsl側のstruct GPULightと並び・ストライド(64バイト)を一致させる必要がある
        struct alignas(16) GPULight
        {
            DirectX::XMFLOAT4 PositionType;   // xyz=ワールド座標, w=LightType
            DirectX::XMFLOAT4 ColorRange;     // rgb=露出済み放射輝度, w=Range
            DirectX::XMFLOAT4 DirectionAngle; // xyz=向き(正規化済み), w=spotAngleScale
            // x=spotAngleOffset, y=CastShadow(1でスクリーンスペースシャドウを撃つ / 0で撃たない),
            // zw=未使用(エリアライト用に予約)
            DirectX::XMFLOAT4 Params;
        };
        static_assert(sizeof(GPULight) == 64, "GPULightはDirectLighting.hlsl側と64バイトで一致させる必要がある");

        // t5の構造化バッファに詰めるライトの最大数。実データ(BistroInterior.fbxで4灯)に対しては
        // 十分すぎる余裕を持たせてあるが、構造化バッファなのでこの容量自体がGPU時間へ影響することはない
        // (シェーダはLightCount.xまでしかループしないため)
        constexpr uint32_t kMaxLights = 1024;

        // DirectLighting.hlsl側のcbuffer LightingConstantsと一致させる必要がある。
        // b0はFrameConstantsが使っており定数バッファスロットは2本しか無いため、
        // 直接光パス固有のパラメータはすべてここへ足していく
        struct alignas(16) LightingConstants
        {
            // x=有効ライト数, y=ピクセルあたりに撃つスクリーンスペースシャドウのレイ数の上限, zw=未使用
            DirectX::XMUINT4 LightCount;
            // スクリーンスペースシャドウ(ScreenSpaceShadow.hlsli)のパラメータ。
            // x=レイマーチのステップ数, y=最大レイ長(ワールド単位), z=遮蔽とみなす深度差の上限(thickness),
            // w=有効フラグ(0で無効)
            DirectX::XMFLOAT4 SSSParams0;
            // x=深度リニアライズ定数a, y=同b(viewZ = b / (depth - a))、
            // z=レイ始点の法線方向への押し出し量(View空間深度に比例させる係数)、w=画面端フェード幅(UV)
            DirectX::XMFLOAT4 SSSParams1;
            // タイルライトカリング(LightCulling.hlsl)のパラメータ。
            // x=タイル数X, y=タイルの1辺のピクセル数, z=1タイルあたりの容量, w=カリング有効フラグ
            DirectX::XMUINT4 TileParams;
        };

        // kLightTileSize / kLightTileCapacity / kLightTileStride はKurenaiEngine3Dのstatic constexprへ
        // 移した(DebugViewPanelがヒートマップの上限として参照するため)。定義はKurenaiEngine3D.h

        // LightCulling.hlsl側のcbuffer LightCullingConstantsと一致させる必要がある
        struct alignas(16) LightCullingConstants
        {
            DirectX::XMFLOAT4X4 View;
            // x=タイル数X, y=タイル数Y, z=有効ライト数, w=1タイルあたりの容量
            DirectX::XMUINT4 TileParams;
            // x=レンダー解像度の幅, y=同 高さ, zw=未使用
            DirectX::XMUINT4 RenderSize;
            // x=射影行列の(0,0)成分, y=同(1,1)成分, z=深度リニアライズ定数a, w=同b
            DirectX::XMFLOAT4 ProjParams;
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
            // Params.y = このライトがスクリーンスペースシャドウを落とすか。ライトごとに切れるようにしてあるのは、
            // ピクセルあたりのシャドウレイ数に上限(LightingConstants.LightCount.y)があり、
            // 「影を出したいライト」に予算を回せるようにするため
            gpuLight.Params = { angleOffset, light.CastShadow ? 1.0f : 0.0f, 0.0f, 0.0f };
            return gpuLight;
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

    KurenaiEngine3D::KurenaiEngine3D(
        GraphicsAPI api, uint32_t renderWidth, uint32_t renderHeight, size_t initialSceneIndex)
        : KurenaiEngineBase(L"Kurenai Engine", 1280, 720, api)
        , m_GraphicsAPI(api)
        , m_InitialSceneIndex(initialSceneIndex)
        , m_RenderWidth(std::max(1u, renderWidth))
        , m_RenderHeight(std::max(1u, renderHeight))
    {
        m_ImGuiBackend = m_Device->CreateImGuiBackend(m_Window->GetHandle());
        m_GPUProfiler = m_Device->CreateGPUProfiler();

        // imgui.iniの保存先を起動時の作業ディレクトリに依存させず、KurenaiEngine.dllと同じフォルダに固定する。
        // ImGuiはIniFilenameのポインタを保持するだけでコピーしないため、m_ImGuiIniPathで寿命を維持する
        m_ImGuiIniPath = WideToUtf8(GetModuleDirectory() + L"imgui.ini");
        ImGui::GetIO().IniFilename = m_ImGuiIniPath.c_str();

        // UIパネル群はImGuiコンテキストの生成後に作る(パネルの構築自体はImGuiを呼ばないが、
        // 以降の段階でスタイル・フォント設定をここへ足す前提で順序を固定しておく)
        m_UIManager = std::make_unique<UI::UIManager>(*this);

        // アスペクト比はm_RenderAspectを唯一の出所にする(解像度は実行時に変わるため)。
        // ここではまだUpdateスレッドが動いていないのでm_Cameraへ直接書いてよい
        m_RenderAspect.store(
            static_cast<float>(m_RenderWidth) / static_cast<float>(m_RenderHeight), std::memory_order_relaxed);
        m_Camera.SetAspectRatio(m_RenderAspect.load(std::memory_order_relaxed));

        CreateSceneResources();

        m_LastFrameTime = std::chrono::steady_clock::now();
    }

    KurenaiEngine3D::~KurenaiEngine3D() = default;

    void KurenaiEngine3D::CreateSceneResources()
    {
        // Shaders/AssetsはビルドでKurenaiEngine.dllと同じフォルダにコピーされる
        const std::wstring dataRoot = GetModuleDirectory();
        const std::wstring shaderDirectory = dataRoot + L"Shaders\\";

        const std::vector<RHI::InputElementDesc> modelInputLayout = GetModelInputLayout();

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

        // 水面(ModelInstance::IsWater)専用のピクセルシェーダー(P2: 水面マテリアル基盤)。
        // 頂点シェーダーはWater.hlslもGBufferCommon.hlsli由来の同じVSMainを使うため、
        // m_GBufferVertexShaderをそのまま共有する(専用のVSは作らない)
        RHI::ShaderDesc gbufferWaterPsDesc;
        gbufferWaterPsDesc.Stage = RHI::ShaderStage::Pixel;
        gbufferWaterPsDesc.FilePath = shaderDirectory + L"Water.hlsl";
        gbufferWaterPsDesc.EntryPoint = "PSMain";
        m_GBufferWaterPixelShader = m_Device->CreateShader(gbufferWaterPsDesc);

        // G-BufferのPSOはEmissiveのフォーマットがバッファ精度に依存するため、
        // この関数の末尾でCreatePrecisionDependentPipelineStates()がまとめて作る

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

        // SSAO/SSIL/AOブラーのPSOは出力先(AO/GIバッファ)のフォーマットがバッファ精度に依存するため、
        // この関数の末尾でCreatePrecisionDependentPipelineStates()がまとめて作る

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
        transparentPipelineDesc.FrontCounterClockwise = true;
        m_TransparentPipelineStateMirrored = m_Device->CreatePipelineState(transparentPipelineDesc);

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

        // タイルライトカリングパス(コンピュートシェーダー)。タイルごとに届くライトのインデックスリストを作る。
        // ライトグリッド本体(m_LightTileBuffer)は解像度に依存するためCreateRenderTargetsで作る
        RHI::ShaderDesc lightCullingCsDesc;
        lightCullingCsDesc.Stage = RHI::ShaderStage::Compute;
        lightCullingCsDesc.FilePath = shaderDirectory + L"LightCulling.hlsl";
        lightCullingCsDesc.EntryPoint = "CSMain";
        m_LightCullingComputeShader = m_Device->CreateShader(lightCullingCsDesc);
        m_LightCullingPipelineState = m_Device->CreateComputePipelineState({ m_LightCullingComputeShader.get() });

        RHI::BufferDesc lightCullingConstantBufferDesc;
        lightCullingConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        lightCullingConstantBufferDesc.SizeInBytes = sizeof(LightCullingConstants);
        m_LightCullingConstantBuffer = m_Device->CreateBuffer(lightCullingConstantBufferDesc);

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

        // 大気遠近パス(P8。頂点バッファなしのフルスクリーン三角形。反射パスの出力とG-Buffer深度から
        // フォグを合成する)。専用のb1定数バッファは持たない(パラメータはFrameConstants末尾の
        // FogParams0/1に入れているため。AerialPerspective.hlsl冒頭参照)
        RHI::ShaderDesc aerialPerspectiveVsDesc;
        aerialPerspectiveVsDesc.Stage = RHI::ShaderStage::Vertex;
        aerialPerspectiveVsDesc.FilePath = shaderDirectory + L"AerialPerspective.hlsl";
        aerialPerspectiveVsDesc.EntryPoint = "VSMain";
        m_AerialPerspectiveVertexShader = m_Device->CreateShader(aerialPerspectiveVsDesc);

        RHI::ShaderDesc aerialPerspectivePsDesc;
        aerialPerspectivePsDesc.Stage = RHI::ShaderStage::Pixel;
        aerialPerspectivePsDesc.FilePath = shaderDirectory + L"AerialPerspective.hlsl";
        aerialPerspectivePsDesc.EntryPoint = "PSMain";
        m_AerialPerspectivePixelShader = m_Device->CreateShader(aerialPerspectivePsDesc);

        RHI::PipelineStateDesc aerialPerspectivePipelineDesc;
        aerialPerspectivePipelineDesc.VertexShader = m_AerialPerspectiveVertexShader.get();
        aerialPerspectivePipelineDesc.PixelShader = m_AerialPerspectivePixelShader.get();
        aerialPerspectivePipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
        aerialPerspectivePipelineDesc.RenderTargetFormats = { RHI::Format::R16G16B16A16_Float };
        m_AerialPerspectivePipelineState = m_Device->CreatePipelineState(aerialPerspectivePipelineDesc);

        // RT反射パス(コンピュートシェーダー。TLASへ鏡面レイを撃ち反射色を求める)。
        // RTReflection.hlslはRayQueryを含むためシェーダーモデル6.5でしかコンパイルできない。
        // 非対応環境ではシェーダー自体を作らず、UIからもRaytracedを選べないようにする
        m_RaytracingAvailable = m_Device->SupportsRaytracing();
        if (m_RaytracingAvailable)
        {
            RHI::ShaderDesc rtReflectionCsDesc;
            rtReflectionCsDesc.Stage = RHI::ShaderStage::Compute;
            rtReflectionCsDesc.FilePath = shaderDirectory + L"RTReflection.hlsl";
            rtReflectionCsDesc.EntryPoint = "CSMain";
            m_RTReflectionComputeShader = m_Device->CreateShader(rtReflectionCsDesc);
            m_RTReflectionPipelineState = m_Device->CreateComputePipelineState({ m_RTReflectionComputeShader.get() });

            RHI::BufferDesc rtReflectionConstantBufferDesc;
            rtReflectionConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
            rtReflectionConstantBufferDesc.SizeInBytes = sizeof(RTReflectionConstants);
            m_RTReflectionConstantBuffer = m_Device->CreateBuffer(rtReflectionConstantBufferDesc);

            // RTシャドウパス(コンピュートシェーダー。TLASへ太陽の円盤方向の影レイを撃ち可視率を求める)。
            // RTReflectionと同じくRayQueryを含むためシェーダーモデル6.5が必要
            RHI::ShaderDesc rtShadowCsDesc;
            rtShadowCsDesc.Stage = RHI::ShaderStage::Compute;
            rtShadowCsDesc.FilePath = shaderDirectory + L"RTShadow.hlsl";
            rtShadowCsDesc.EntryPoint = "CSMain";
            m_RTShadowComputeShader = m_Device->CreateShader(rtShadowCsDesc);
            m_RTShadowPipelineState = m_Device->CreateComputePipelineState({ m_RTShadowComputeShader.get() });

            RHI::BufferDesc rtShadowConstantBufferDesc;
            rtShadowConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
            rtShadowConstantBufferDesc.SizeInBytes = sizeof(RTShadowConstants);
            m_RTShadowConstantBuffer = m_Device->CreateBuffer(rtShadowConstantBufferDesc);

            // RTAOパス(コンピュートシェーダー。半球へレイを撃ち遮蔽率と間接拡散光を求める)
            RHI::ShaderDesc rtAOCsDesc;
            rtAOCsDesc.Stage = RHI::ShaderStage::Compute;
            rtAOCsDesc.FilePath = shaderDirectory + L"RTAO.hlsl";
            rtAOCsDesc.EntryPoint = "CSMain";
            m_RTAOComputeShader = m_Device->CreateShader(rtAOCsDesc);
            m_RTAOPipelineState = m_Device->CreateComputePipelineState({ m_RTAOComputeShader.get() });

            RHI::BufferDesc rtAOConstantBufferDesc;
            rtAOConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
            rtAOConstantBufferDesc.SizeInBytes = sizeof(RTAOConstants);
            m_RTAOConstantBuffer = m_Device->CreateBuffer(rtAOConstantBufferDesc);

            Core::Logger::Info(
                "KurenaiEngine3D", "レイトレーシングを利用できます(反射・シャドウ・AO/GIでRaytracedを選択可能)");
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
        taaVsDesc.FilePath = shaderDirectory + L"TAA.hlsl";
        taaVsDesc.EntryPoint = "VSMain";
        m_TAAVertexShader = m_Device->CreateShader(taaVsDesc);

        RHI::ShaderDesc taaPsDesc;
        taaPsDesc.Stage = RHI::ShaderStage::Pixel;
        taaPsDesc.FilePath = shaderDirectory + L"TAA.hlsl";
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
        taaConstantBufferDesc.SizeInBytes = sizeof(TAAConstants);
        m_TAAConstantBuffer = m_Device->CreateBuffer(taaConstantBufferDesc);

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

        RHI::BufferDesc tonemapConstantBufferDesc;
        tonemapConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        tonemapConstantBufferDesc.SizeInBytes = sizeof(TonemapConstants);
        m_TonemapConstantBuffer = m_Device->CreateBuffer(tonemapConstantBufferDesc);

        // 自動露出パス(輝度ヒストグラムの構築→縮約→時間方向の順応。すべてコンピュートシェーダー)
        RHI::ShaderDesc autoExposureClearCsDesc;
        autoExposureClearCsDesc.Stage = RHI::ShaderStage::Compute;
        autoExposureClearCsDesc.FilePath = shaderDirectory + L"AutoExposure.hlsl";
        autoExposureClearCsDesc.EntryPoint = "CSClearHistogram";
        m_AutoExposureClearComputeShader = m_Device->CreateShader(autoExposureClearCsDesc);
        m_AutoExposureClearPipelineState =
            m_Device->CreateComputePipelineState({ m_AutoExposureClearComputeShader.get() });

        RHI::ShaderDesc autoExposureHistogramCsDesc;
        autoExposureHistogramCsDesc.Stage = RHI::ShaderStage::Compute;
        autoExposureHistogramCsDesc.FilePath = shaderDirectory + L"AutoExposure.hlsl";
        autoExposureHistogramCsDesc.EntryPoint = "CSHistogram";
        m_AutoExposureHistogramComputeShader = m_Device->CreateShader(autoExposureHistogramCsDesc);
        m_AutoExposureHistogramPipelineState =
            m_Device->CreateComputePipelineState({ m_AutoExposureHistogramComputeShader.get() });

        RHI::ShaderDesc autoExposureResolveCsDesc;
        autoExposureResolveCsDesc.Stage = RHI::ShaderStage::Compute;
        autoExposureResolveCsDesc.FilePath = shaderDirectory + L"AutoExposure.hlsl";
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
        autoExposureConstantBufferDesc.SizeInBytes = sizeof(AutoExposureConstants);
        m_AutoExposureConstantBuffer = m_Device->CreateBuffer(autoExposureConstantBufferDesc);

        // 露出の保存先。フレームをまたいで順応の履歴を保持するため、ウィンドウリサイズで
        // 作り直されるCreateRenderTargetsではなくここで一度だけ作る。
        // 生成直後はゼロクリアされており、texel(1,0)=0が「未初期化」を意味する
        // (CSResolveがこれを見て初回だけ順応を飛ばして即座に目標値へ合わせる)
        m_ExposureTexture = m_Device->CreateUAVTexture(2, 1, RHI::Format::R32_Float);

        // ブルームパス(ダウンサンプル/アップサンプルの2エントリ。テクスチャはCreateRenderTargetsで作る)
        RHI::ShaderDesc bloomDownCsDesc;
        bloomDownCsDesc.Stage = RHI::ShaderStage::Compute;
        bloomDownCsDesc.FilePath = shaderDirectory + L"Bloom.hlsl";
        bloomDownCsDesc.EntryPoint = "CSDownsample";
        m_BloomDownsampleComputeShader = m_Device->CreateShader(bloomDownCsDesc);
        m_BloomDownsamplePipelineState =
            m_Device->CreateComputePipelineState({ m_BloomDownsampleComputeShader.get() });

        RHI::ShaderDesc bloomUpCsDesc;
        bloomUpCsDesc.Stage = RHI::ShaderStage::Compute;
        bloomUpCsDesc.FilePath = shaderDirectory + L"Bloom.hlsl";
        bloomUpCsDesc.EntryPoint = "CSUpsample";
        m_BloomUpsampleComputeShader = m_Device->CreateShader(bloomUpCsDesc);
        m_BloomUpsamplePipelineState =
            m_Device->CreateComputePipelineState({ m_BloomUpsampleComputeShader.get() });

        RHI::BufferDesc bloomConstantBufferDesc;
        bloomConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        bloomConstantBufferDesc.SizeInBytes = sizeof(BloomConstants);
        m_BloomConstantBuffer = m_Device->CreateBuffer(bloomConstantBufferDesc);

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
        // スワップチェインへ描くパスは深度テストこそ使わないが、SetRenderTarget(swapChain)が
        // スワップチェインのDSVをバインドするため、DSVフォーマットの申告だけは必要になる
        presentPipelineDesc.DepthTargetAttached = true;
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
        // 影も同様に、ミラーリングされたインスタンスは表裏が入れ替わる。放置すると
        // シャドウマップへ内側の面の深度が書かれ、影の形と自己遮蔽の出方がずれる
        shadowPipelineDesc.FrontCounterClockwise = true;
        m_ShadowPipelineStateMirrored = m_Device->CreatePipelineState(shadowPipelineDesc);

        // シャドウマップはG-Bufferと異なりウィンドウ/レンダー解像度に依存しないため固定サイズで一度だけ作成する。
        // 全カスケードを1つのTexture2DArrayにまとめ、スライスごとのDSVで1カスケードずつ描き込む
        m_ShadowCascadeArray = m_Device->CreateDepthTextureArray(kShadowMapSize, kShadowMapSize, kCascadeCount);

        // 既定のスカイボックス。.ksceneの[Scene]Skyboxで差し替えられる(LoadScene参照)ため、
        // 現在読み込んでいるパスを覚えておき、同じパスなら読み直さない
        m_DefaultSkyboxPath = dataRoot + L"Assets\\Skybox\\Sky.dds";
        m_CurrentSkyboxPath = m_DefaultSkyboxPath;
        m_SkyboxTexture = m_Device->CreateTextureFromFile(m_CurrentSkyboxPath, false);

        // 水面法線マップの既定(P2)。.ksceneに[Water]NormalMapが無いシーンではこのフラット法線
        // (128,128,255,255=接線空間で真上を向く法線)がWater.hlslのt6へバインドされ続ける。
        // ModelLoader.cppが法線マップ未指定のマテリアルに使うプレースホルダーと同じ値
        m_CurrentWaterNormalMapPath.clear();
        m_WaterNormalMapTexture = m_Device->CreateSolidColorTexture(128, 128, 255, 255);

        CreateSamplerSets();

        // IBL(Image Based Lighting)の3つの畳み込み結果を保持するテクスチャと、それを生成する
        // コンピュートシェーダー一式。実際の畳み込み(スカイボックスのサンプリング)はRender()の
        // 最初のフレームで一度だけ行う(m_IBLBaked参照)。ここではリソースの作成のみ行う
        m_IrradianceTexture = m_Device->CreateUAVTextureCube(kIBLIrradianceSize, RHI::Format::R16G16B16A16_Float);
        m_PrefilteredEnvTexture = m_Device->CreateMippedUAVTextureCube(
            kIBLPrefilterBaseSize, RHI::Format::R16G16B16A16_Float, kIBLPrefilterMipLevels);
        // BRDF積分LUTは2パスで焼く。パス1(CSMain)が(A, B)をスクラッチへ書き、
        // パス2(CSCombineEavg)がそれを読んでEavgを足した float4(A, B, Eavg, 0) を最終LUTへ書く。
        // 同一リソースをSRVとUAVへ同時バインドできないためスクラッチが要る(BRDFLUT.hlsl参照)
        m_BRDFLUTScratchTexture = m_Device->CreateUAVTexture(kIBLBRDFLUTSize, kIBLBRDFLUTSize, RHI::Format::R16G16_Float);
        m_BRDFLUTTexture = m_Device->CreateUAVTexture(kIBLBRDFLUTSize, kIBLBRDFLUTSize, RHI::Format::R16G16B16A16_Float);
        if (!m_BRDFLUTScratchTexture || !m_BRDFLUTTexture)
        {
            Core::Logger::Error("KurenaiEngine3D",
                "BRDF積分LUTのテクスチャ作成に失敗しました(スペキュラのエネルギー補正が正しく動作しません)");
        }

        RHI::ShaderDesc brdfLutCsDesc;
        brdfLutCsDesc.Stage = RHI::ShaderStage::Compute;
        brdfLutCsDesc.FilePath = shaderDirectory + L"BRDFLUT.hlsl";
        brdfLutCsDesc.EntryPoint = "CSMain";
        m_BRDFLUTComputeShader = m_Device->CreateShader(brdfLutCsDesc);
        m_BRDFLUTPipelineState = m_Device->CreateComputePipelineState({ m_BRDFLUTComputeShader.get() });

        RHI::ShaderDesc brdfLutCombineCsDesc;
        brdfLutCombineCsDesc.Stage = RHI::ShaderStage::Compute;
        brdfLutCombineCsDesc.FilePath = shaderDirectory + L"BRDFLUT.hlsl";
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

        // ボリュメトリック雲の3Dノイズ(P13a)。カメラにも太陽にも空の状態にも依存しない
        // 純粋な手続き生成なので、BRDF積分LUTと同じく起動後に一度だけ焼く(m_CloudNoiseBaked)。
        // ここではリソースとパイプラインの作成だけを行う
        m_CloudShapeNoiseTexture = m_Device->CreateUAVTexture3D(
            kCloudShapeNoiseSize, kCloudShapeNoiseSize, kCloudShapeNoiseSize, RHI::Format::R8G8B8A8_UNorm);
        m_CloudDetailNoiseTexture = m_Device->CreateUAVTexture3D(
            kCloudDetailNoiseSize, kCloudDetailNoiseSize, kCloudDetailNoiseSize, RHI::Format::R8G8B8A8_UNorm);
        if (!m_CloudShapeNoiseTexture || !m_CloudDetailNoiseTexture)
        {
            Core::Logger::Error("KurenaiEngine3D",
                "雲の3Dノイズテクスチャの作成に失敗しました(ボリュメトリック雲が正しく描画されません)");
        }

        RHI::ShaderDesc cloudShapeNoiseCsDesc;
        cloudShapeNoiseCsDesc.Stage = RHI::ShaderStage::Compute;
        cloudShapeNoiseCsDesc.FilePath = shaderDirectory + L"CloudNoiseGenerate.hlsl";
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
        cloudDetailNoiseCsDesc.FilePath = shaderDirectory + L"CloudNoiseGenerate.hlsl";
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

        // 手続き空(SkyGenerate.hlsl)。太陽が動くたびに焼き直すため、IBLのプリフィルタと同じく
        // 面ごとに1回ずつディスパッチする。プリフィルタの入力にしかならないので解像度は
        // オフラインDDS(512)より小さい256で足りる(生成コストが1/4になる)
        m_ProceduralSkyTexture =
            m_Device->CreateUAVTextureCube(kProceduralSkySize, RHI::Format::R16G16B16A16_Float);

        RHI::ShaderDesc skyGenerateCsDesc;
        skyGenerateCsDesc.Stage = RHI::ShaderStage::Compute;
        skyGenerateCsDesc.FilePath = shaderDirectory + L"SkyGenerate.hlsl";
        skyGenerateCsDesc.EntryPoint = "CSGenerateSky";
        m_SkyGenerateComputeShader = m_Device->CreateShader(skyGenerateCsDesc);
        m_SkyGeneratePipelineState = m_Device->CreateComputePipelineState({ m_SkyGenerateComputeShader.get() });

        RHI::BufferDesc skyBakeConstantBufferDesc;
        skyBakeConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        skyBakeConstantBufferDesc.SizeInBytes = sizeof(SkyBakeConstants);
        m_SkyBakeConstantBuffer = m_Device->CreateBuffer(skyBakeConstantBufferDesc);

        // 空パラメータ(ティント4本+照度正規化済みの天頂輝度)の積分をGPUで行うコンピュートシェーダー
        // (P9)。SkyGenerateより前に実行し、結果をm_SkyParametersBufferへ書く
        RHI::ShaderDesc skyIntegrateCsDesc;
        skyIntegrateCsDesc.Stage = RHI::ShaderStage::Compute;
        skyIntegrateCsDesc.FilePath = shaderDirectory + L"SkyIntegrate.hlsl";
        skyIntegrateCsDesc.EntryPoint = "CSIntegrateSky";
        m_SkyIntegrateComputeShader = m_Device->CreateShader(skyIntegrateCsDesc);
        m_SkyIntegratePipelineState = m_Device->CreateComputePipelineState({ m_SkyIntegrateComputeShader.get() });

        RHI::BufferDesc skyIntegrateConstantBufferDesc;
        skyIntegrateConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        skyIntegrateConstantBufferDesc.SizeInBytes = sizeof(SkyIntegrateConstants);
        m_SkyIntegrateConstantBuffer = m_Device->CreateBuffer(skyIntegrateConstantBufferDesc);

        // SkyIntegrate.hlslが書き、SkyGenerate.hlsl/DeferredLighting.hlsl/SSR.hlslが読む
        // 要素数1のStructuredRWバッファ(m_LightTileBufferと同じ作法)。
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
        RHI::BufferDesc skyParametersBufferDesc;
        skyParametersBufferDesc.Usage = RHI::BufferUsage::StructuredRW;
        skyParametersBufferDesc.SizeInBytes = sizeof(GPUSkyParameters);
        skyParametersBufferDesc.StrideInBytes = sizeof(GPUSkyParameters);
        m_SkyParametersBuffer = m_Device->CreateBuffer(skyParametersBufferDesc);

        RHI::BufferDesc iblPrefilterConstantBufferDesc;
        iblPrefilterConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        iblPrefilterConstantBufferDesc.SizeInBytes = sizeof(IBLFaceConstants);
        m_IBLPrefilterConstantBuffer = m_Device->CreateBuffer(iblPrefilterConstantBufferDesc);

        // --- 反射プローブ(19章) ---
        // キャプチャ先(1面ぶんを6面で使い回す)。キューブへ写す前のHDR値を保つためFloatにする
        m_ProbeCaptureColor = m_Device->CreateRenderTexture(kProbeCaptureSize, kProbeCaptureSize, RHI::Format::R16G16B16A16_Float);
        // 同じキャプチャの2枚目(SV_TARGET1)。プローブからのワールド距離をそのまま入れるため、
        // [0,1]に収まらず精度も必要になる。R32_Floatなら室内スケールでも十分な絶対精度がある
        m_ProbeCaptureDistance = m_Device->CreateRenderTexture(kProbeCaptureSize, kProbeCaptureSize, RHI::Format::R32_Float);
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
        // 距離キューブ(19.12節)。畳み込まないためミップは1段だけでよく、スクラッチのキューブも要らない
        // (キャプチャからこの配列のスライスへ直接書き込む)。
        // 128²×6面×8枚×4バイト = 3.1MB
        m_ProbeDistanceArray = m_Device->CreateMippedUAVTextureCubeArray(
            kProbeCaptureSize, RHI::Format::R32_Float, 1, kMaxReflectionProbes);

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
        // レンダーターゲットは2枚(放射輝度と距離)。ProbeCapture.hlslのPSOutputと並びを一致させること
        probeCapturePipelineDesc.RenderTargetFormats = { RHI::Format::R16G16B16A16_Float, RHI::Format::R32_Float };
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

        // --- 平面反射(P6) ---
        // 水面に不透明ジオメトリの鏡像を映す専用フォワードパス。設計判断はPlanarReflection.hlsl
        // 冒頭のコメントを参照。反射先のテクスチャはレンダー解像度に依存するため、実際の確保は
        // CreatePlanarReflectionTargets(CreateRenderTargetsと同じ呼び出し箇所)が行う。
        // ここではProbeCaptureと同様、解像度に依存しないシェーダー・PSO・定数バッファのみ作る
        RHI::ShaderDesc planarReflectionVsDesc;
        planarReflectionVsDesc.Stage = RHI::ShaderStage::Vertex;
        planarReflectionVsDesc.FilePath = shaderDirectory + L"PlanarReflection.hlsl";
        planarReflectionVsDesc.EntryPoint = "VSMain";
        m_PlanarReflectionVertexShader = m_Device->CreateShader(planarReflectionVsDesc);

        RHI::ShaderDesc planarReflectionPsDesc;
        planarReflectionPsDesc.Stage = RHI::ShaderStage::Pixel;
        planarReflectionPsDesc.FilePath = shaderDirectory + L"PlanarReflection.hlsl";
        planarReflectionPsDesc.EntryPoint = "PSMain";
        m_PlanarReflectionPixelShader = m_Device->CreateShader(planarReflectionPsDesc);

        RHI::PipelineStateDesc planarReflectionPipelineDesc;
        planarReflectionPipelineDesc.InputLayout = modelInputLayout;
        planarReflectionPipelineDesc.VertexShader = m_PlanarReflectionVertexShader.get();
        planarReflectionPipelineDesc.PixelShader = m_PlanarReflectionPixelShader.get();
        planarReflectionPipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
        // レンダーターゲットは1枚(放射輝度のみ。ProbeCaptureと違い視差補正用の距離は要らない。
        // PlanarReflection.hlsl冒頭参照)。バッファ精度(Legacy8bit)の対象外にしてあり常にHDR固定
        planarReflectionPipelineDesc.RenderTargetFormats = { RHI::Format::R16G16B16A16_Float };
        planarReflectionPipelineDesc.HasDepthStencil = true;
        planarReflectionPipelineDesc.ReverseZ = true;
        m_PlanarReflectionPipelineState = m_Device->CreatePipelineState(planarReflectionPipelineDesc);

        // 鏡映カメラで描くとワインディングが全反転するため、m_GBufferPipelineStateMirroredと
        // 同じ仕組み(FrontCounterClockwiseの反転)で吸収する。選択条件はinstance.IsMirroredの
        // 否定になる点がGBufferパスと異なる(Render()側のExecute内参照)
        planarReflectionPipelineDesc.FrontCounterClockwise = true;
        m_PlanarReflectionPipelineStateMirrored = m_Device->CreatePipelineState(planarReflectionPipelineDesc);

        // captureProbeFaceと同じ役割の専用FrameConstants
        RHI::BufferDesc planarReflectionConstantBufferDesc;
        planarReflectionConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        planarReflectionConstantBufferDesc.SizeInBytes = sizeof(FrameConstants);
        m_PlanarReflectionConstantBuffer = m_Device->CreateBuffer(planarReflectionConstantBufferDesc);

        // --- DDGI(22章) ---
        // キャプチャ経路は反射プローブとまったく同じ(ProbeCapture.hlslとm_ProbeCapturePipelineStateを
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
        ddgiUpdateCsDesc.FilePath = shaderDirectory + L"DDGIProbeUpdate.hlsl";
        ddgiUpdateCsDesc.EntryPoint = "CSUpdateProbe";
        m_DDGIProbeUpdateComputeShader = m_Device->CreateShader(ddgiUpdateCsDesc);
        m_DDGIProbeUpdatePipelineState = m_Device->CreateComputePipelineState({ m_DDGIProbeUpdateComputeShader.get() });

        // 境界の複製は本体の書き込みが全て終わってからでなければ正しい値を読めないため、
        // 同じディスパッチ内では行えず別パスになる(オクタヘドラルの縁は対辺へ折り返して繋がるので、
        // 自分のセルの反対側のテクセルを読む必要がある)
        RHI::ShaderDesc ddgiBorderCsDesc;
        ddgiBorderCsDesc.Stage = RHI::ShaderStage::Compute;
        ddgiBorderCsDesc.FilePath = shaderDirectory + L"DDGIProbeUpdate.hlsl";
        ddgiBorderCsDesc.EntryPoint = "CSCopyBorder";
        m_DDGIBorderCopyComputeShader = m_Device->CreateShader(ddgiBorderCsDesc);
        m_DDGIBorderCopyPipelineState = m_Device->CreateComputePipelineState({ m_DDGIBorderCopyComputeShader.get() });

        RHI::BufferDesc ddgiUpdateConstantBufferDesc;
        ddgiUpdateConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        ddgiUpdateConstantBufferDesc.SizeInBytes = sizeof(DDGIUpdateConstants);
        m_DDGIUpdateConstantBuffer = m_Device->CreateBuffer(ddgiUpdateConstantBufferDesc);

        // シーン読み込み前でもSRVをバインドできるよう、この時点で1プローブぶんのダミーを確保しておく
        RecreateDDGIAtlases();

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

        // レンダーターゲットを先に作る。CreateRenderTargetsはHDRフォーマットの作成に失敗した場合に
        // m_BufferPrecisionをLegacy8bitへ落とすフォールバックを持つため、PSOはその結果が
        // 確定した後に作らなければフォーマットがずれる
        CreateRenderTargets(m_RenderWidth, m_RenderHeight);
        // 平面反射(P6)専用のレンダーターゲットも、レンダー解像度が確定したこのタイミングで作る
        // (呼び出し箇所はCreateRenderTargetsと同じ2か所。もう1か所はRender()の解像度変更ハンドリング)
        CreatePlanarReflectionTargets();
        CreatePrecisionDependentPipelineStates();

        DiscoverScenes();

        // 起動時の1シーン目だけは同期的に読み込む。この時点ではRender/Loaderのどちらのスレッドも
        // まだ動いていないため、通常のハンドオフを経由せず直接読み込んで反映してよい
        // (初回フレームより前にシーンが揃う従来の挙動を保つ)。
        // m_LoaderSkyboxPathはCreateSceneResourcesが読み込んだ既定スカイボックスに合わせておく
        m_LoaderSkyboxPath = m_CurrentSkyboxPath;
        // m_LoaderWaterNormalMapPathも同様(P2)。CreateSceneResourcesはフラット法線フォールバック
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
        return m_BufferPrecision == BufferPrecision::Legacy8bit ? RHI::Format::R8G8B8A8_UNorm
                                                                : RHI::Format::R11G11B10_Float;
    }

    RHI::Format KurenaiEngine3D::GetAOFormat() const
    {
        // AO/GIバッファ: rgb=間接拡散光(HDR)、a=遮蔽率。間接光は暗い室内では0.02〜0.1に収まり、
        // UNorm8ではコード5〜26の約20階調しか使えずポスタリゼーションする。
        // aに遮蔽率を持つためアルファ付きのR16G16B16A16_Floatを使う
        return m_BufferPrecision == BufferPrecision::Legacy8bit ? RHI::Format::R8G8B8A8_UNorm
                                                                : RHI::Format::R16G16B16A16_Float;
    }

    bool KurenaiEngine3D::ShouldRunRaytracedReflection() const
    {
        return m_ReflectionMode == ReflectionMode::Raytraced && m_RaytracingScene.IsValid() &&
               m_RTReflectionPipelineState != nullptr && m_RTReflectionTexture != nullptr;
    }

    bool KurenaiEngine3D::ShouldRunRaytracedShadow() const
    {
        return m_ShadowMode == ShadowMode::Raytraced && m_RaytracingScene.IsValid() &&
               m_RTShadowPipelineState != nullptr && m_RTShadowTexture != nullptr;
    }

    bool KurenaiEngine3D::ShouldRunRaytracedAO() const
    {
        return m_AOTechnique == AOTechnique::Raytraced && m_RaytracingScene.IsValid() &&
               m_RTAOPipelineState != nullptr && m_RTAORawTexture != nullptr && m_RTAOTexture != nullptr;
    }

    RHI::IRHITexture* KurenaiEngine3D::GetActiveAOTexture() const
    {
        if (!m_AOEnabled)
        {
            return m_AODisabledTexture.get();
        }
        if (ShouldRunRaytracedAO())
        {
            return m_RTAOTexture.get();
        }
        if (m_AOTechnique == AOTechnique::SSILVisibilityBitmask)
        {
            return m_SSILTexture.get();
        }
        // SSAO、およびRaytracedを選んでいても実行できないフレーム(高速化構造が無い等)
        return m_SSAOTexture.get();
    }

    RHI::IRHITexture* KurenaiEngine3D::GetActiveAORawTexture() const
    {
        if (!m_AOEnabled)
        {
            return m_AODisabledTexture.get();
        }
        if (ShouldRunRaytracedAO())
        {
            return m_RTAORawTexture.get();
        }
        if (m_AOTechnique == AOTechnique::SSILVisibilityBitmask)
        {
            return m_SSILRawTexture.get();
        }
        return m_SSAORawTexture.get();
    }

    RHI::IRHITexture* KurenaiEngine3D::GetActiveReflectionOutput() const
    {
        if (m_ReflectionMode == ReflectionMode::ScreenSpace)
        {
            return m_SSRTexture.get();
        }
        if (ShouldRunRaytracedReflection())
        {
            return m_RTReflectionTexture.get();
        }
        // 反射なし、またはRT反射を実行しなかった場合はLightingパスの結果をそのまま後段へ渡す
        return m_SceneColor.get();
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
            };
            gbufferPipelineDesc.HasDepthStencil = true;
            gbufferPipelineDesc.ReverseZ = true;
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
                    (m_BufferPrecision == BufferPrecision::Legacy8bit ? "Legacy8bit" : "HDR") + "): " + e.what());
            throw;
        }
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
        // (s0 = MaterialSampler、s1 = ColorSampler、s2 = DataSampler、s3 = VolumeSampler)。

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
        const bool useProcedural = m_ProceduralSkyEnabled && m_Scene.SkyboxPath.empty();
        return useProcedural ? m_ProceduralSkyTexture.get() : m_SkyboxTexture.get();
    }

    void KurenaiEngine3D::CreateRenderTargets(uint32_t width, uint32_t height)
    {
        if (width == 0 || height == 0)
        {
            return;
        }

        // 中間バッファのフォーマットはm_BufferPrecisionで切り替える(A/B比較用。BufferPrecision参照)。
        // Legacy8bitはM7以前の「すべてR8G8B8A8_UNorm」構成をそのまま再現する
        const bool legacyPrecision = (m_BufferPrecision == BufferPrecision::Legacy8bit);

        // Albedoは両構成ともリニアのR8G8B8A8_UNormのままにする。
        // sRGB格納(R8G8B8A8_UNorm_SRGB)にすれば符号点が暗部へ寄り、暗いマテリアルの量子化は
        // 細かくなる(リニア反射率L=0.02で約4.3倍)。しかし実測すると最終画像への寄与は
        // 平均0.03/255と測定限界以下だった。アルベドの量子化は面ごとの一定オフセットとして出るため、
        // 狙っていた暗部のバンディング(=照明の滑らかな変化が最終8bitで潰れる現象)には
        // そもそも効かない。加えてL>0.244では逆に粗くなり、金属はアルベドバッファの値を
        // F0として使う(DeferredLighting.hlsl)ぶん確実にその領域へ入るため、
        // 利点が確認できないまま欠点だけを抱えることになる。詳細はArchitecture.html 17.4節
        // フォーマットの決定はGetEmissiveFormat/GetAOFormatに一本化している。ここへ直接書くと
        // 同じ値を宣言するPSO側(CreatePrecisionDependentPipelineStates)とずれ、
        // D3D12では仕様違反になる(実際にそれで発生していた)
        const RHI::Format emissiveFormat = GetEmissiveFormat();
        const RHI::Format aoFormat = GetAOFormat();

        try
        {
            m_GBufferAlbedo = m_Device->CreateRenderTexture(width, height, RHI::Format::R8G8B8A8_UNorm);
            m_GBufferNormal = m_Device->CreateRenderTexture(width, height, RHI::Format::R16G16_Float);
            m_GBufferMaterial = m_Device->CreateRenderTexture(width, height, RHI::Format::R8G8B8A8_UNorm);
            m_GBufferEmissive = m_Device->CreateRenderTexture(width, height, emissiveFormat);
            // Reverse-Zのため近平面側(NDC z=1.0)ではなく遠平面側(NDC z=0.0)にクリアする
            m_GBufferDepth = m_Device->CreateDepthTexture(width, height, 0.0f);
            m_DirectLightTexture = m_Device->CreateRenderTexture(width, height, RHI::Format::R32G32B32A32_Float);
            m_SSAORawTexture = m_Device->CreateRenderTexture(width, height, aoFormat);
            m_SSAOTexture = m_Device->CreateRenderTexture(width, height, aoFormat);
            m_SSILRawTexture = m_Device->CreateRenderTexture(width, height, aoFormat);
            m_SSILTexture = m_Device->CreateRenderTexture(width, height, aoFormat);
            m_SceneColor = m_Device->CreateRenderTexture(width, height, RHI::Format::R16G16B16A16_Float);
            m_SSRTexture = m_Device->CreateRenderTexture(width, height, RHI::Format::R16G16B16A16_Float);
            // 大気遠近パス(P8)の出力。m_SSRTextureと同じ作法(HDR、R16G16B16A16_Float)で永続確保する
            m_AerialPerspectiveTexture = m_Device->CreateRenderTexture(width, height, RHI::Format::R16G16B16A16_Float);
            // RT反射はコンピュートシェーダーがUAVで書くため、レンダーターゲットではなくUAVテクスチャを作る。
            // 非対応環境ではパス自体が実行されないので確保しない
            if (m_RaytracingAvailable)
            {
                m_RTReflectionTexture = m_Device->CreateUAVTexture(width, height, RHI::Format::R16G16B16A16_Float);
                // RTシャドウの可視率(0〜1のスカラー)。RWTexture2D<float>として書くため単チャンネルの
                // R32_Floatにする(型付きUAVの読み書きが保証されているのはR32系のみ。AutoExposure.hlsl参照)
                m_RTShadowTexture = m_Device->CreateUAVTexture(width, height, RHI::Format::R32_Float);
                // RTAOの生バッファはコンピュートがUAVで書くためUAVテクスチャ、ブラー後は
                // 従来どおりピクセルシェーダーが書くレンダーターゲット。
                // フォーマットはSSAO/SSILと同じaoFormat(バッファ精度の設定に追従する)
                m_RTAORawTexture = m_Device->CreateUAVTexture(width, height, aoFormat);
                m_RTAOTexture = m_Device->CreateRenderTexture(width, height, aoFormat);
            }
            m_TonemapTexture = m_Device->CreateRenderTexture(width, height, RHI::Format::R8G8B8A8_UNorm);

            // モーションベクター(速度バッファ)。G-Bufferの5枚目として、GBuffer.hlslが
            // 「この画素に映っているものが前フレームでは画面のどこにいたか」をUV単位の2Dベクトルで書く。
            // 2成分しか要らないのでR16G16_Float。1画素ぶんの移動量が1/解像度(1920幅なら約0.00052)と
            // 小さいため、絶対精度ではなく相対精度で効く浮動小数点フォーマットが適している
            m_GBufferVelocity = m_Device->CreateRenderTexture(width, height, RHI::Format::R16G16_Float);

            // TAAの履歴バッファ2枚。読みながら同じテクスチャへ書けないので、毎フレーム役割を入れ替える
            // (m_TAAHistoryIndexが今フレームの書き込み先)。バッファ精度をLegacy8bitに落としても
            // m_SceneColorと同じく常にfp16のままにする。履歴は何十フレームぶんもの蓄積結果であり、
            // ここを8bitにすると量子化誤差が積み上がってバンディングになるため
            m_TAAHistory[0] = m_Device->CreateRenderTexture(width, height, RHI::Format::R16G16B16A16_Float);
            m_TAAHistory[1] = m_Device->CreateRenderTexture(width, height, RHI::Format::R16G16B16A16_Float);

            m_HiZMipLevels = ComputeMipLevelCount(width, height);
            m_HiZTexture = m_Device->CreateHiZTexture(width, height, m_HiZMipLevels);
            m_HiZDebugMipLevel = 0;

            // タイルライトカリングのライトグリッド。タイル数は解像度に依存するためここで作り直す。
            // 端のタイルは部分的にしか埋まらないので切り上げる
            m_LightTileCountX = (width + kLightTileSize - 1) / kLightTileSize;
            m_LightTileCountY = (height + kLightTileSize - 1) / kLightTileSize;
            RHI::BufferDesc lightTileBufferDesc;
            lightTileBufferDesc.Usage = RHI::BufferUsage::StructuredRW;
            lightTileBufferDesc.SizeInBytes =
                static_cast<uint32_t>(sizeof(uint32_t)) * kLightTileStride * m_LightTileCountX * m_LightTileCountY;
            lightTileBufferDesc.StrideInBytes = static_cast<uint32_t>(sizeof(uint32_t));
            m_LightTileBuffer = m_Device->CreateBuffer(lightTileBufferDesc);
            m_LightTileOverflowLogged = false;

            // ブルームのピラミッド。第0段が半解像度で、以降1段ごとに半分になる。
            // 1x1まで落とさず段数を固定しているのは、これ以上小さくしても裾の広がりが
            // 見た目に寄与しないため(解像度が低いと逆にアップサンプル時のちらつき源になる)。
            // レベルごとに独立したテクスチャにしている理由はBloom.hlsl冒頭を参照
            m_BloomLevelSizes.clear();
            m_BloomDownTextures.clear();
            m_BloomUpTextures.clear();
            uint32_t bloomWidth = std::max(1u, width / 2);
            uint32_t bloomHeight = std::max(1u, height / 2);
            for (uint32_t level = 0; level < kBloomLevelCount; ++level)
            {
                m_BloomLevelSizes.push_back({ bloomWidth, bloomHeight });
                // アルファを使わないHDRバッファなのでR11G11B10_Floatで足りる。
                // Legacy8bit構成でもブルームはHDR値を扱う必要があるためここは常にHDRのままにする
                // (8bitにすると1.0でクリップされ、ブルームの意味が失われる)
                m_BloomDownTextures.push_back(
                    m_Device->CreateUAVTexture(bloomWidth, bloomHeight, RHI::Format::R16G16B16A16_Float));
                m_BloomUpTextures.push_back(
                    m_Device->CreateUAVTexture(bloomWidth, bloomHeight, RHI::Format::R16G16B16A16_Float));

                bloomWidth = std::max(1u, bloomWidth / 2);
                bloomHeight = std::max(1u, bloomHeight / 2);
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
            m_BufferPrecision = BufferPrecision::Legacy8bit;
            CreateRenderTargets(width, height);
            return;
        }

        // 履歴バッファを作り直した直後は中身が未定義なので、TAAへ「今フレームは履歴を使うな」と伝える。
        // fp16の未初期化領域はNaNのことがあり、lerp(NaN, x, 1.0)もNaNになるため、
        // ブレンド率を0にするだけでは足りず「サンプルそのものを行わない」必要がある(TAA.hlsl参照)
        m_TAAHistoryValid = false;
        m_TAAHistoryIndex = 0;

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
            1u, static_cast<uint32_t>(static_cast<float>(m_RenderWidth) * m_PlanarReflectionResolutionScale));
        const uint32_t height = std::max(
            1u, static_cast<uint32_t>(static_cast<float>(m_RenderHeight) * m_PlanarReflectionResolutionScale));

        try
        {
            // SceneColorと同じHDR形式(R16G16B16A16_Float)。水面はラフネスが低く反射がそのまま
            // 見えるため、CreateRenderTargetsのLegacy8bitフォールバックの対象外にして常にHDR固定にする
            m_PlanarReflectionColor = m_Device->CreateRenderTexture(width, height, RHI::Format::R16G16B16A16_Float);
            // Reverse-Zのため遠平面側(NDC z=0.0)にクリアする(G-Buffer/ProbeCapture深度と同じ)
            m_PlanarReflectionDepth = m_Device->CreateDepthTexture(width, height, 0.0f);
        }
        catch (const std::exception& e)
        {
            Core::Logger::Error(
                "KurenaiEngine3D",
                std::string("平面反射のレンダーターゲット作成に失敗しました (") + std::to_string(width) + "x" +
                    std::to_string(height) + "): " + e.what());
            throw;
        }

        m_PlanarReflectionWidth = width;
        m_PlanarReflectionHeight = height;

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

        if (scale == m_PlanarReflectionResolutionScale)
        {
            return;
        }

        // レンダーターゲットの作り直しはGPUがまだ参照しているかもしれない状態では行えないため、
        // RequestRenderResolutionと同じく要求を記録するだけにしてRender()の先頭でまとめて反映する
        m_PendingPlanarReflectionResolutionScale = scale;
        m_PlanarReflectionResolutionDirty = true;
    }

    void KurenaiEngine3D::RequestSceneLoad(size_t sceneIndex)
    {
        if (sceneIndex >= m_SceneFilePaths.size())
        {
            Core::Logger::Error(
                "KurenaiEngine3D",
                "RequestSceneLoad: シーン番号" + std::to_string(sceneIndex) + "が範囲外です(シーン数: " +
                    std::to_string(m_SceneFilePaths.size()) + ")。要求を無視します");
            return;
        }

        // UIパネルもRenderスレッドで動くため、ここは単なるRenderスレッド内の受け渡しでよい。
        // 実際の発注はUpdateSceneStreaming(フレーム先頭)がまとめて行う
        m_PendingSceneRequest = static_cast<int>(sceneIndex);
    }

    void KurenaiEngine3D::UpdateSceneStreaming()
    {
        // --- 出来上がったシーンがあれば取り込む ---
        std::unique_ptr<LoadedScene> loaded;
        {
            std::lock_guard<std::mutex> lock(m_LoadedSceneMutex);
            loaded = std::move(m_LoadedScene);
        }
        if (loaded)
        {
            ApplyLoadedScene(*loaded);
            m_SceneLoadInFlight = false;
        }

        // --- 保留中の切り替え要求をLoaderスレッドへ発注する ---
        // 読み込み中は発注しない(最後の要求はm_PendingSceneRequestに残るので取りこぼさない)
        if (m_PendingSceneRequest < 0 || m_SceneLoadInFlight)
        {
            return;
        }

        const size_t sceneIndex = static_cast<size_t>(m_PendingSceneRequest);
        m_PendingSceneRequest = -1;

        // 旧シーンのGPUリソースを手放す前に、GPUが旧シーンを参照するコマンド(直前まで提出されていた
        // 描画コマンド)の実行を終えるまで待つ。特にDX12はCPUがGPU完了を待たずに次フレームの記録を
        // 始める多重バッファリング設計のため、これを省くとGPUがまだ読んでいるバッファ/テクスチャを
        // 解放してしまう(詳細はIRHIDevice::WaitForGPUIdleのコメント参照)。
        // このフレームのGPUコマンドはまだ1つも積んでいないため、待ち時間は前フレームぶんだけで済む
        m_Device->WaitForGPUIdle();

        // 読み込み開始と同時に旧シーンを手放す。読み込み完了まで待ってから捨てると新旧の
        // GPUリソースが同時に載ってVRAMがほぼ2倍になるため、先に空にする方を選んでいる。
        // その代わり読み込み中はシーンが描かれない(UIとスカイボックスのみになる)
        RetiredAssets retired;
        retired.Scene = std::move(m_Scene);
        retired.RaytracingScene = std::move(m_RaytracingScene);
        m_Scene = Assets::Scene{};
        m_RaytracingScene = Assets::RaytracingScene{};
        RetireAssets(std::move(retired));

        {
            std::lock_guard<std::mutex> lock(m_LoadRequestMutex);
            m_LoadRequestSceneIndex = static_cast<int>(sceneIndex);
        }
        m_LoadRequestCV.notify_one();
        m_SceneLoadInFlight = true;
    }

    void KurenaiEngine3D::RetireAssets(RetiredAssets&& retired)
    {
        std::lock_guard<std::mutex> lock(m_RetiredAssetsMutex);
        m_RetiredAssets.push_back(std::move(retired));
    }

    void KurenaiEngine3D::LoaderThreadMain()
    {
        // TextureImage::LoadFromFileがWICを使う経路(.dds/.tga以外)に備えてCOMを初期化しておく。
        // COMはスレッドごとに初期化が必要で、未初期化のままWICを呼ぶとハングする
        // (packedアセットは.ktex=DDSなので通常この経路には入らないが、保険として揃えておく)
        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

        // 破棄依頼を引き取って実際に解放する。アセット用ディスクリプタヒープを触るのは
        // このスレッドだけ、という不変条件を保つための処理(RetiredAssetsのコメント参照)
        const auto destroyRetiredAssets = [this]()
        {
            std::vector<RetiredAssets> retired;
            {
                std::lock_guard<std::mutex> lock(m_RetiredAssetsMutex);
                retired.swap(m_RetiredAssets);
            }
            // retiredのデストラクタでGPUリソースが解放される
        };

        for (;;)
        {
            int sceneIndex = -1;
            {
                std::unique_lock<std::mutex> lock(m_LoadRequestMutex);
                m_LoadRequestCV.wait(lock, [this] { return m_LoadRequestSceneIndex >= 0 || m_StopLoaderThread; });
                if (m_StopLoaderThread && m_LoadRequestSceneIndex < 0)
                {
                    break;
                }
                sceneIndex = m_LoadRequestSceneIndex;
                m_LoadRequestSceneIndex = -1;
            }

            // 先に破棄を済ませてから読み込む(Renderスレッドは手放す前にWaitForGPUIdle済み)。
            // 新シーンを作る前に旧シーンを解放することで、VRAMの二重常駐を避ける
            destroyRetiredAssets();

            if (sceneIndex < 0)
            {
                continue;
            }

            std::unique_ptr<LoadedScene> loaded = LoadSceneOnLoaderThread(static_cast<size_t>(sceneIndex));
            if (!loaded)
            {
                // 読み込みに失敗した場合も「読み込み中」状態を解除しないとUIが固まるため、
                // 空の完成品を渡してRenderスレッドに終了を知らせる(シーンは空のままになる)
                loaded = std::make_unique<LoadedScene>();
                loaded->SceneIndex = static_cast<size_t>(sceneIndex);
                loaded->Camera = ComputeInitialCamera(loaded->Scene);
            }

            {
                std::lock_guard<std::mutex> lock(m_LoadedSceneMutex);
                m_LoadedScene = std::move(loaded);
            }
        }

        // 停止時に残っている破棄依頼をこのスレッドで片付ける
        destroyRetiredAssets();

        if (SUCCEEDED(comResult))
        {
            CoUninitialize();
        }
    }

    std::unique_ptr<KurenaiEngine3D::LoadedScene> KurenaiEngine3D::LoadSceneOnLoaderThread(size_t sceneIndex)
    {
        if (sceneIndex >= m_SceneFilePaths.size())
        {
            Core::Logger::Error("KurenaiEngine3D", "LoadSceneOnLoaderThread: シーン番号が範囲外です");
            return nullptr;
        }

        // [Model]Pathの基準ディレクトリ(Assetsルート)。.kmodel自身の内部パス(.kmodelがある
        // ディレクトリからの相対)とは基準が異なる点に注意(SceneLoader.h参照)
        const std::wstring assetRootDirectory = GetModuleDirectory() + L"Assets\\";

        auto loaded = std::make_unique<LoadedScene>();
        loaded->SceneIndex = sceneIndex;

        try
        {
            loaded->Scene = Assets::LoadScene(*m_Device, m_SceneFilePaths[sceneIndex], assetRootDirectory);
        }
        catch (const std::exception& e)
        {
            Core::Logger::Error(
                "KurenaiEngine3D",
                "シーンの読み込みに失敗しました: " + WideToUtf8(m_SceneFilePaths[sceneIndex]) + " : " + e.what());
            return nullptr;
        }

        // [Scene]Skyboxでスカイボックスを差し替える(指定が無ければ既定へ戻す)。
        // 「今どのスカイボックスを読み込み済みか」を知っているのはこのスレッドだけなので、
        // 差し替えが要るかの判定もここで行う(不要ならSkyboxTextureをnullptrのままにして
        // Renderスレッドへ「現状維持」を伝える)
        const std::wstring desiredSkyboxPath =
            loaded->Scene.SkyboxPath.empty() ? m_DefaultSkyboxPath : loaded->Scene.SkyboxPath;
        if (desiredSkyboxPath != m_LoaderSkyboxPath)
        {
            try
            {
                loaded->SkyboxTexture = m_Device->CreateTextureFromFile(desiredSkyboxPath, false);
                loaded->SkyboxPath = desiredSkyboxPath;
                m_LoaderSkyboxPath = desiredSkyboxPath;
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

        // [Water]NormalMapで水面法線マップを差し替える(P2: 水面マテリアル基盤)。
        // スカイボックスと同じ「このスレッドだけが現在の読み込み済みパスを知っている」方式だが、
        // 空文字列が「1x1のフラット法線フォールバックを使う」という有効な指定である点が異なる
        // (スカイボックスの空文字列は「既定のSky.ddsを使う」という意味で、常に何らかのファイルを
        // 読む。水面はファイルを読まない状態そのものが正しいシーンがあるため、ここは分岐が要る)
        const std::wstring& desiredWaterNormalMapPath = loaded->Scene.WaterNormalMapPath;
        if (desiredWaterNormalMapPath != m_LoaderWaterNormalMapPath)
        {
            if (desiredWaterNormalMapPath.empty())
            {
                // フラット法線(128,128,255,255=接線空間で真上を向く法線)へ戻す。
                // ModelLoader.cppが法線マップ未指定のマテリアルに使うプレースホルダーと同じ値
                loaded->WaterNormalMapTexture = m_Device->CreateSolidColorTexture(128, 128, 255, 255);
                loaded->WaterNormalMapPath.clear();
                m_LoaderWaterNormalMapPath.clear();
                Core::Logger::Info("KurenaiEngine3D", "水面法線マップをフラットへ戻しました(NormalMap未指定)");
            }
            else
            {
                try
                {
                    loaded->WaterNormalMapTexture = m_Device->CreateTextureFromFile(desiredWaterNormalMapPath, false);
                    loaded->WaterNormalMapPath = desiredWaterNormalMapPath;
                    m_LoaderWaterNormalMapPath = desiredWaterNormalMapPath;
                    Core::Logger::Info(
                        "KurenaiEngine3D", "水面法線マップを差し替えました: " + WideToUtf8(desiredWaterNormalMapPath));
                }
                catch (const std::exception& e)
                {
                    // 読み込みに失敗しても現在の水面法線マップ(またはフラット法線)のまま描画を続ける
                    Core::Logger::Error(
                        "KurenaiEngine3D",
                        "水面法線マップの読み込みに失敗しました。現在の状態を維持します: " +
                            WideToUtf8(desiredWaterNormalMapPath) + " : " + e.what());
                }
            }
        }

        // レイトレーシングの高速化構造(BLAS/TLAS)とシーンジオメトリの統合バッファを構築する。
        // 非対応環境(DX11、Tier 1.1未満のアダプタ)では何も作らず、描画側は従来の
        // スクリーンスペース手法のまま動く。構築に失敗しても描画は継続する
        if (m_Device->SupportsRaytracing())
        {
            loaded->RaytracingScene.Build(*m_Device, loaded->Scene);
        }

        loaded->Camera = ComputeInitialCamera(loaded->Scene);
        return loaded;
    }

    void KurenaiEngine3D::ApplyLoadedScene(LoadedScene& loaded)
    {
        m_Scene = std::move(loaded.Scene);
        m_RaytracingScene = std::move(loaded.RaytracingScene);
        m_CurrentSceneIndex = loaded.SceneIndex;

        // [Sun]/[Camera]セクションが無いシーンでは、Sceneの側でこのメンバの既定値
        // (従来のKurenaiEngine3Dの初期値と同じ)が使われるため、常にそのまま反映してよい
        m_TimeOfDay = m_Scene.SunTimeOfDay;
        m_SunAzimuthDegrees = m_Scene.SunAzimuthDegrees;
        // .ksceneが持つのは「影を出すか」の真偽値だけなので、手法の選択はエンジン側で決める
        // (反射のm_ReflectionModeと同じ扱い)。規則はDefaultShadowModeに1か所だけ置いてある
        m_ShadowMode = m_Scene.ShadowEnabled ? DefaultShadowMode(m_RaytracingAvailable) : ShadowMode::Off;
        m_SunEnabled = m_Scene.SunEnabled;
        m_AOEnabled = m_Scene.AOEnabled;
        // .ksceneが持つのは「反射を使うか」の真偽値だけなので、手法の選択はエンジン側で決める。
        // 規則はDefaultReflectionModeに1か所だけ置いてある
        m_ReflectionMode = m_Scene.SSREnabled ? DefaultReflectionMode(m_RaytracingAvailable) : ReflectionMode::Off;
        if (m_Scene.HasIBLIntensityOverride)
        {
            m_IBLIntensity = m_Scene.IBLIntensity;
        }
        // 水面(P2)。[Water]が無いシーンでもScene::WaterWaveScale等はリテラル既定値
        // (EngineDefaults.hを複製したもの、Scene.h参照)を持っているため、常にそのまま反映してよい
        // (m_TimeOfDay/m_SunAzimuthDegreesと同じ扱い)
        m_WaterWaveScale = m_Scene.WaterWaveScale;
        m_WaterWaveSpeed = m_Scene.WaterWaveSpeed;
        m_WaterWaveStrength = m_Scene.WaterWaveStrength;

        // スカイボックスが差し替わった場合のみ非nullptr。IBLの拡散イラディアンス・プリフィルタ済み
        // 鏡面はスカイボックスから焼かれるため、差し替えたらm_IBLBakedを倒して焼き直させる
        if (loaded.SkyboxTexture)
        {
            // 旧スカイボックスもアセット由来なのでLoaderスレッドへ破棄を委ねる。
            // 直前(UpdateSceneStreaming)のWaitForGPUIdleによりGPUはもう参照していない
            RetiredAssets retiredSkybox;
            retiredSkybox.SkyboxTexture = std::move(m_SkyboxTexture);
            RetireAssets(std::move(retiredSkybox));

            m_SkyboxTexture = std::move(loaded.SkyboxTexture);
            m_CurrentSkyboxPath = loaded.SkyboxPath;
            m_IBLBaked = false;
            // 検証用の拡散イラディアンスマップも古いスカイボックス由来のものになるため倒す
            // (実際に焼き直すのは検証トグル・デバッグ表示が有効なときだけ)
            m_IBLIrradianceBaked = false;
        }

        // 水面法線マップが差し替わった場合のみ非nullptr(P2)。スカイボックスとまったく同じ方式で
        // 旧テクスチャをLoaderスレッドへ破棄依頼する
        if (loaded.WaterNormalMapTexture)
        {
            RetiredAssets retiredWaterNormalMap;
            retiredWaterNormalMap.WaterNormalMapTexture = std::move(m_WaterNormalMapTexture);
            RetireAssets(std::move(retiredWaterNormalMap));

            m_WaterNormalMapTexture = std::move(loaded.WaterNormalMapTexture);
            m_CurrentWaterNormalMapPath = loaded.WaterNormalMapPath;
        }

        // アセット由来のライトをユーザー編集用のコピーへ複製する(m_Scene.Lightsは直接編集しない。
        // シーンを再読み込みすればアセット既定値に戻るようにするため)。m_Scene.Lightsは
        // SceneLoaderが各ModelInstanceのModel::Lightsをワールド空間へ変換し、.kscene自身の
        // [Light]セクションのライトと合成済みのシーン全体のライト一覧(Scene.h参照)
        m_Lights = m_Scene.Lights;
        m_SelectedLightIndex = m_Lights.empty() ? -1 : 0;
        m_LightOverflowLogged = false;
        // 平面反射(P6)。新しいシーンでは水面の構成が変わるため、複数水面高さの警告も仕切り直す
        m_PlanarReflectionMultipleWaterLogged = false;

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
        // Realtimeのラウンドロビンは先頭から仕切り直す(シーンが変わればプローブの数も並びも変わる)
        m_ProbeRealtimeProbeIndex = 0;
        m_ProbeRealtimeFace = 0;

        // DDGIボリューム(22章)。現状は先頭の1つだけを使う。複数ボリュームは重なりと優先順位を
        // 決める仕組みがまだ無いため、2つ目以降は警告を出して切り捨てる
        m_HasGIVolume = !m_Scene.GIVolumes.empty();
        if (m_Scene.GIVolumes.size() > 1)
        {
            Core::Logger::Warning(
                "KurenaiEngine3D",
                "[GIVolume]が複数ありますが、現状は先頭の1つだけを使用します: " +
                    std::to_string(m_Scene.GIVolumes.size()) + "個");
        }
        if (m_HasGIVolume)
        {
            m_GIVolume = m_Scene.GIVolumes.front();
            const uint64_t probeCount =
                static_cast<uint64_t>(m_GIVolume.ProbeCounts[0]) *
                static_cast<uint64_t>(m_GIVolume.ProbeCounts[1]) *
                static_cast<uint64_t>(m_GIVolume.ProbeCounts[2]);
            if (probeCount > kDDGIMaxProbes)
            {
                // 切り捨てでは格子が歪んで意味を成さない(反射プローブのように「先頭N個」で
                // 済ませられない)ため、ボリュームごと無効にして従来のIBLのまま描く
                Core::Logger::Error(
                    "KurenaiEngine3D",
                    "[GIVolume]のプローブ数が上限(" + std::to_string(kDDGIMaxProbes) + ")を超えたためDDGIを無効にします: " +
                        std::to_string(probeCount) + "個。ProbeCountsを減らすかProbeSpacingを広げてください");
                m_HasGIVolume = false;
            }
        }
        RecreateDDGIAtlases();

        // SSAO/SSILの半径やSSRの距離はシーンの規模から決まるため、差し替え後のm_Sceneで計算し直す
        ResetSceneDependentParams();

        // 露出の追従状態はシーンをまたいで持ち越さない。時刻が入れ替わると実効プリ露出は
        // 最大18段跳ぶため、追従の途中で反射プローブが焼かれると桁違いの明るさで固定される
        // (m_ProbeBakedExposureEV100・m_AutoExposureResetRequestedのコメント参照)
        m_EffectiveExposureInitialized = false;
        m_AutoExposureResetRequested = true;

        // TAAの履歴には前のシーンの絵が入っており、この後カメラも新シーンの初期位置へ飛ぶため、
        // 再投影しても対応する画素が存在しない。捨てて今フレームの色から積み直す。
        // ApplyLoadedSceneはRenderスレッドから呼ばれるため、m_Cameraは直接書けないがatomicなら書ける
        // (Renderスレッドが読む。カメラ自体はこの後m_AppliedSceneCamera経由でUpdateスレッドへ渡す)
        m_TAAHistoryValid.store(false, std::memory_order_relaxed);

        // 初期カメラとウィンドウタイトルはUpdateスレッドが適用する。m_Cameraの書き込み手を
        // 1スレッドに保ち、ウィンドウタイトルもウィンドウを所有するスレッドから設定するため
        // (UpdateAppliedSceneHandoff参照)
        {
            const wchar_t* apiName = (m_GraphicsAPI == GraphicsAPI::DX12) ? L"DX12" : L"DX11";
            std::lock_guard<std::mutex> lock(m_AppliedSceneMutex);
            m_AppliedSceneCamera = loaded.Camera;
            m_AppliedSceneTitle = std::wstring(L"Kurenai Engine [") + apiName + L"] - " + m_Scene.Name;
        }
        m_AppliedScenePending.store(true, std::memory_order_release);
    }

    void KurenaiEngine3D::RecreateDDGIAtlases()
    {
        // アトラスの並び: 列 = ProbeCounts.x * ProbeCounts.y、行 = ProbeCounts.z。
        // XY平面のスライスを横に並べ、Zをそのまま行にする(RTXGIと同じ並び)。
        // プローブ番号との対応は index = x + y*Cx + z*Cx*Cy で、シェーダー側の
        // DDGIProbeAtlasCoord()と一致させること
        const uint32_t countX = m_HasGIVolume ? m_GIVolume.ProbeCounts[0] : 1u;
        const uint32_t countY = m_HasGIVolume ? m_GIVolume.ProbeCounts[1] : 1u;
        const uint32_t countZ = m_HasGIVolume ? m_GIVolume.ProbeCounts[2] : 1u;

        m_DDGIProbeCount = countX * countY * countZ;

        const uint32_t columns = countX * countY;
        const uint32_t rows = countZ;

        // 【R32系である必要がある】更新CSはヒステリシス(前の値と新しい値のlerp)のために
        // アトラスをRWTexture2Dとして読んでから書く。型付きUAV読み出しはR32系しか保証されておらず、
        // fp16で読むにはTypedUAVLoadAdditionalFormatsが要る(AutoExposure.hlslが同じ理由で
        // R32_Floatを2テクセル並べる構成にしている)。
        // アトラスは455プローブでも合計1.4MB程度と小さいため、精度と可搬性を取って素直にR32にする
        m_DDGIIrradianceAtlas = m_Device->CreateUAVTexture(
            columns * kDDGIIrradianceCell, rows * kDDGIIrradianceCell, RHI::Format::R32G32B32A32_Float);
        // R=平均距離、G=平均二乗距離
        m_DDGIDistanceAtlas = m_Device->CreateUAVTexture(
            columns * kDDGIDistanceCell, rows * kDDGIDistanceCell, RHI::Format::R32G32_Float);

        // 確保し直した直後のアトラスは中身が未定義なので、一巡目からやり直す
        m_DDGIBaked = false;
        m_DDGIWarmingUp = true;
        m_DDGIUpdateCursor = 0;
        m_DDGIOverwriteRemaining = 0;
        m_DDGILastExposureValid = false;

        if (m_HasGIVolume)
        {
            Core::Logger::Info(
                "KurenaiEngine3D",
                "DDGIボリューム '" + m_GIVolume.Name + "' を確保しました: " +
                    std::to_string(countX) + "x" + std::to_string(countY) + "x" + std::to_string(countZ) +
                    " = " + std::to_string(m_DDGIProbeCount) + "プローブ, アトラス " +
                    std::to_string(columns * kDDGIIrradianceCell) + "x" + std::to_string(rows * kDDGIIrradianceCell) +
                    " / " + std::to_string(columns * kDDGIDistanceCell) + "x" + std::to_string(rows * kDDGIDistanceCell));
        }
    }

    DirectX::XMFLOAT3 KurenaiEngine3D::ComputeDDGIProbePosition(uint32_t probeIndex) const
    {
        const uint32_t countX = m_GIVolume.ProbeCounts[0];
        const uint32_t countY = m_GIVolume.ProbeCounts[1];

        const uint32_t x = probeIndex % countX;
        const uint32_t y = (probeIndex / countX) % countY;
        const uint32_t z = probeIndex / (countX * countY);

        return DirectX::XMFLOAT3{
            m_GIVolume.Origin[0] + static_cast<float>(x) * m_GIVolume.ProbeSpacing[0],
            m_GIVolume.Origin[1] + static_cast<float>(y) * m_GIVolume.ProbeSpacing[1],
            m_GIVolume.Origin[2] + static_cast<float>(z) * m_GIVolume.ProbeSpacing[2],
        };
    }

    uint64_t KurenaiEngine3D::ComputeProbeBakeSignature() const
    {
        // FNV-1a(64bit)。焼き上がりに影響する値だけを順に混ぜる。衝突しても起きるのは
        // 「本来必要な焼き直しを1回取りこぼす」だけで破綻はしないため、この程度の強度で足りる
        uint64_t hash = 1469598103934665603ull;
        const auto mixBytes = [&hash](const void* data, size_t size)
        {
            const auto* bytes = static_cast<const unsigned char*>(data);
            for (size_t i = 0; i < size; ++i)
            {
                hash ^= bytes[i];
                hash *= 1099511628211ull;
            }
        };
        const auto mixFloat = [&mixBytes](float value) { mixBytes(&value, sizeof(value)); };
        const auto mixBool = [&mixBytes](bool value) { const unsigned char v = value ? 1u : 0u; mixBytes(&v, sizeof(v)); };

        // 太陽と昼夜サイクル。ProbeCapture.hlslは共有のFrameConstantsから太陽の向き・色を読むため、
        // 時刻を動かすと焼き上がりが変わる
        mixFloat(m_TimeOfDay);
        mixFloat(m_SunAzimuthDegrees);
        mixBool(m_SunEnabled);
        // 影の手法ではなく「影を落とすかどうか」だけを混ぜる。ProbeCapture.hlslが読むのは
        // 常にカスケードシャドウマップで、そのシャドウマップはRTシャドウ選択時も同じように
        // 描かれるため、CascadedShadowMapとRaytracedでプローブの焼き上がりは変わらない
        mixBool(m_ShadowMode != ShadowMode::Off);
        // 月は時刻に連動せず手動指定なので、太陽とは別に混ぜる必要がある。太陽が沈むと
        // 平行光源の枠が月へ切り替わり、キャプチャの直接光がそのまま変わる
        mixFloat(m_MoonAzimuthDegrees);
        mixFloat(m_MoonElevationDegrees);
        // キャプチャ内の環境項はグローバルIBLを引くため、その強度も焼き上がりに影響する。
        // 手続き空か.ksceneのDDSかで空そのものが変わるため、その切り替えも含める
        mixFloat(m_IBLEnabled ? m_IBLIntensity : 0.0f);
        mixBool(m_ProceduralSkyEnabled);
        // 自発光の強度倍率はキャプチャのエミッシブ項へそのまま乗る
        mixFloat(m_EmissiveIntensity);

        // ライトは構造体ごとダンプすると詰め物(padding)の未初期化バイトを拾い得るため、
        // 使うフィールドだけを明示的に混ぜる
        for (const Assets::Light& light : m_Lights)
        {
            mixBytes(&light.Type, sizeof(light.Type));
            for (int i = 0; i < 3; ++i) mixFloat(light.Position[i]);
            for (int i = 0; i < 3; ++i) mixFloat(light.Direction[i]);
            for (int i = 0; i < 3; ++i) mixFloat(light.Color[i]);
            mixFloat(light.Intensity);
            mixFloat(light.Range);
            mixFloat(light.SpotInnerConeAngle);
            mixFloat(light.SpotOuterConeAngle);
            mixBool(light.Enabled);
        }

        // プローブの位置はキャプチャ地点そのものなので含める(影響範囲は含めない。
        // 形状・半径・ブレンド距離を変えてもどこから撮るかは変わらないため)
        for (const Assets::ReflectionProbe& probe : m_ReflectionProbes)
        {
            for (int i = 0; i < 3; ++i) mixFloat(probe.Position[i]);
        }

        return hash;
    }

    void KurenaiEngine3D::ResetSceneDependentParams()
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

        // RT反射のレイ距離はSSRより長く取る。SSRは「画面外へ出たら打ち切り」で早々に確信度0へ
        // 落ちるためシーン対角の半分でも十分だったが、RTは画面外も追えるので短く切ると
        // 本来映るはずの建物を通り越して空が映ってしまう。シーン対角そのものを上限にする
        m_RTReflectionMaxDistance = std::clamp(diagonal, 1.0f, 500.0f);

        // RTAOのレイ距離はSSAO/SSILの半径より長く取る。スクリーンスペース手法は
        // 半径を伸ばすほど画面上のサンプル間隔が粗くなって破綻するが、RTには
        // その制約が無く、部屋の広さ程度まで伸ばしたほうがバウンス光が正しく回る
        m_RTAOMaxDistance = std::clamp(diagonal * 0.03f, 0.1f, 10.0f);
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

    void KurenaiEngine3D::TickFrame()
    {
        const auto now = std::chrono::steady_clock::now();
        const float deltaTime = std::chrono::duration<float>(now - m_LastFrameTime).count();
        m_LastFrameTime = now;

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
            const float renderDeltaTime = std::chrono::duration<float>(now - m_LastRenderFrameTime).count();
            m_LastRenderFrameTime = now;
            // 自動露出の時間方向の順応で使う(次フレームのRender()が読む)
            m_RenderDeltaTime = renderDeltaTime;

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

            // 水面のスクロール位相(P2)。太陽の自動進行とまったく同じ場所・同じ理由
            // (m_TimeAutoAdvance/m_WaterTimeFrozenがRenderingパネル(Renderスレッドから描画)でも
            // 書き換えられるため、両方をRenderスレッド専有にすることで追加の排他制御なしに済ませる)
            if (!m_WaterTimeFrozen)
            {
                m_WaterScrollOffset = std::fmod(m_WaterScrollOffset + renderDeltaTime * m_WaterWaveSpeed, 1.0f);
            }

            // 雲のスクロール位相(P5)。水面とまったく同じ場所・同じ理由でRenderスレッド専有のまま進める。
            // 【風速の単位について】m_CloudWindSpeedは実世界の速度[m/s]として持つ(UIで直感的に
            // 扱えるようにするため)。Sky.hlsliのノイズ空間はワールド距離にCloudUvScaleを掛けた
            // ものなので、ノイズ空間上の移動量へ換算するにはここでCloudUvScaleを掛ける必要がある。
            // 【なぜベイクをdirtyにしないのか】風のスクロールはIBLキューブの明るさに一切影響しない
            // (判断A: キューブには雲を焼かない)。ここでm_SkyBakeDirtyを立てると、風が吹くたびに
            // 毎フレーム空生成6回+プリフィルタ36回のディスパッチが走ってしまい、判断Aの利点が
            // 丸ごと消える。被覆率のような「キューブの明るさに効く」パラメータだけがdirtyを立てる
            // (RenderingPanel::DrawCloudSection参照)
            if (!m_CloudTimeFrozen)
            {
                const float windRadians = DirectX::XMConvertToRadians(m_CloudWindDirectionDegrees);
                const float windDirX = std::cos(windRadians);
                const float windDirZ = std::sin(windRadians);
                const float advanceNoiseSpace = m_CloudWindSpeed * m_CloudUvScale * renderDeltaTime;
                // Sky.hlsliのkCloudNoisePeriodと同じ値でwrapする(このファイル冒頭近くの
                // kCloudNoisePeriod定数のコメント参照)
                m_CloudScrollOffset.x =
                    std::fmod(m_CloudScrollOffset.x + windDirX * advanceNoiseSpace, kCloudNoisePeriod);
                m_CloudScrollOffset.y =
                    std::fmod(m_CloudScrollOffset.y + windDirZ * advanceNoiseSpace, kCloudNoisePeriod);

                // 巻雲(P11)。積雲とまったく同じ形(kCloudNoisePeriodでstd::fmod)で進める。
                // 風向はm_CloudWindDirectionDegreesを積雲と共有し、速度・UVスケールだけ
                // 巻雲側の値(m_CirrusWindSpeed/m_CirrusUvScale)を使う。凍結トグル
                // (m_CloudTimeFrozen)も積雲と共有する——片方にしか効かないとA/B比較で
                // スクロールが揺れる側だけ残ってしまい対照が取れなくなるため
                const float cirrusAdvanceNoiseSpace = m_CirrusWindSpeed * m_CirrusUvScale * renderDeltaTime;
                m_CirrusScrollOffset.x =
                    std::fmod(m_CirrusScrollOffset.x + windDirX * cirrusAdvanceNoiseSpace, kCloudNoisePeriod);
                m_CirrusScrollOffset.y =
                    std::fmod(m_CirrusScrollOffset.y + windDirZ * cirrusAdvanceNoiseSpace, kCloudNoisePeriod);
            }

            // m_Scene・ポストプロセスのパラメータ・UIの状態はすべてこのRenderスレッド専有に
            // なったため、以前あったm_SceneMutexによる保護は不要になっている
            // (経緯はdocs/Architecture.html 23章)
            const auto cpuStart = std::chrono::steady_clock::now();
            Render(frameState);
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
        {
            std::lock_guard<std::mutex> lock(m_AppliedSceneMutex);
            camera = m_AppliedSceneCamera;
            title = m_AppliedSceneTitle;
        }
        m_AppliedScenePending.store(false, std::memory_order_relaxed);

        // m_Cameraの書き込み手はこのUpdateスレッド1つに保つ(Renderスレッドは触らない)
        m_Camera = camera;
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
        // 昼夜サイクルの自動進行(m_TimeOfDay)はRenderThreadMain側で行う(RenderThreadMain参照)
    }

    void KurenaiEngine3D::Render(const FrameState& frameState)
    {
        // WM_SIZE(Updateスレッド)が記録しておいたリサイズ要求を、スワップチェーンを実際に使う
        // このスレッドで反映する。このフレームのGPUコマンドをまだ1つも積んでいないこの位置で
        // 呼ぶこと(DX12SwapChain::Resizeは内部でWaitForGPUIdleを呼び、コマンドリストが
        // 記録待ちの状態であることを前提としているため)
        ApplyPendingResize();

        // Loaderスレッドが出来上がったシーンを置いていれば取り込み、保留中の切り替え要求があれば発注する。
        // 旧シーンの破棄(WaitForGPUIdleを伴う)もここで行うため、このフレームのGPUコマンドを
        // まだ1つも積んでいないこの位置で呼ぶこと
        UpdateSceneStreaming();

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
        if (m_BufferPrecisionDirty || m_RenderResolutionDirty || m_PlanarReflectionResolutionDirty)
        {
            const bool precisionChanged = m_BufferPrecisionDirty;
            m_BufferPrecisionDirty = false;

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
                m_PlanarReflectionResolutionScale = m_PendingPlanarReflectionResolutionScale;
            }

            m_Device->WaitForGPUIdle();
            try
            {
                CreateRenderTargets(m_RenderWidth, m_RenderHeight);
                // 平面反射(P6)専用のレンダーターゲットも、呼び出し箇所をCreateRenderTargetsと
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

        auto* commandList = m_Device->GetImmediateCommandList();
        m_GPUProfiler->BeginFrame();
        m_CPUProfiler.BeginFrame();

        // 太陽・月・空の状態を求める(すべて絶対的な測光量[lx]。露出はまだ掛かっていない)
        const SunLighting sunLighting = ComputeSunLighting(
            m_TimeOfDay, m_SunAzimuthDegrees, m_MoonAzimuthDegrees, m_MoonElevationDegrees);

        // === 可変プリ露出の決定 ===
        // 昼(直射日光10万lx)を基準0として、そのフレームのキー照度が何段暗いかを求め、
        // ユーザー設定のEV100へ足す。これによりHDRバッファへ流れる値のレンジが
        // 昼でも夜でもおおむね一定に保たれ、夜がfp16でつぶれなくなる
        // (詳細な理由はm_EffectiveExposureEV100の宣言コメント)。
        // 露出はTonemap/Bloom/AutoExposureが同じ値で割り戻すため、これを動かしても絵は変わらない
        {
            const float keyIlluminance = std::max(sunLighting.KeyIlluminanceLux, 1e-6f);
            const float autoBias = std::log2(keyIlluminance / kSunIlluminanceLux);
            // 下限-18段は満月の夜(キー照度0.3lx前後)がちょうど収まる範囲。
            // 上限0段は「昼より明るくはしない」の意味
            const float targetEV100 = m_SceneExposureEV100 + std::clamp(autoBias, -18.0f, 0.0f);

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
                const float t = std::clamp(1.0f - std::exp(-deltaTime * m_EffectiveExposureAdaptSpeed), 0.0f, 1.0f);
                m_EffectiveExposureEV100 += (targetEV100 - m_EffectiveExposureEV100) * t;
            }
        }
        const float effectiveExposure = ComputeExposure(m_EffectiveExposureEV100);

        // 手動露出時にTonemap/Bloomが掛ける倍率。
        // HDRバッファには「実効EV100」でプリ露出された値が入っているが、ユーザーが見たいのは
        // 「設定EV100で撮った絵」なので、その差分を割り戻す。
        // 実効EV100は夜に最大18段下がる(=バッファ上の値が26万倍明るくなる)ため、
        // ここを1.0に固定していると夜が昼と同じ明るさで出てしまい、
        // 自動露出をオフにしても露出が時刻に追従し続ける状態になる
        const float manualExposureScale = std::exp2(m_EffectiveExposureEV100 - m_SceneExposureEV100);

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

        // --- TAAのサブピクセルジッター ---
        // 投影行列を1ピクセル未満だけずらして、同じ画素が毎フレームわずかに違う位置をサンプルする
        // ようにする。TAAが複数フレームぶんを蓄積することで実質的なスーパーサンプリングになる。
        // TAA無効時はジッターも必ず0にすること(ジッターだけ残ると画面が振動するだけになる)
        ++m_TAAFrameIndex;
        DirectX::XMFLOAT2 jitterOffsetPixels{ 0.0f, 0.0f };
        if (m_TAAEnabled)
        {
            // Halton列の添字は1から始める。添字0はradical inverseの定義上どの基数でも0となり、
            // オフセットがピクセルの角(-0.5, -0.5)へ偏ってしまう
            const uint32_t haltonIndex = (m_TAAFrameIndex % kTAAJitterSampleCount) + 1;
            jitterOffsetPixels.x = (RadicalInverse(haltonIndex, 2) - 0.5f) * m_TAAJitterScale;
            jitterOffsetPixels.y = (RadicalInverse(haltonIndex, 3) - 0.5f) * m_TAAJitterScale;
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

        // タイルライトカリングの1タイルあたりの容量超過の可能性を早めに知らせる。
        // 実際に超過したかどうかはGPU側にしか無く(バッファのリードバック経路がRHIに無い)、
        // ここで分かるのは「シーンのライト数が容量を超えているので、1つのタイルに集中すれば
        // 超過し得る」という条件までである。実際の超過はデバッグ表示(DebugView::LightTiles)の
        // マゼンタで確認する
        if (m_LightCullingEnabled && gpuLights.size() > kLightTileCapacity && !m_LightTileOverflowLogged)
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
        // 【P3で前倒しした理由】この下のFrameConstants(constants.SkyParams.y)が
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
                cosAngle < std::cos(DirectX::XMConvertToRadians(m_SkyBakeAngleThresholdDegrees));
            // 露出が0.05段(約3.5%)以上動いたら焼き直す。時刻変化に伴う露出の追従でも
            // 動くため、太陽の角度閾値とあわせて実質的に連続した更新になる
            const bool exposureMoved =
                std::abs(m_EffectiveExposureEV100 - m_LastBakedExposureEV100) > 0.05f;
            // タービディティ(P7)が動いたら焼き直す。PreethamのxyYモデルの形自体が変わるため、
            // exposureMovedと同じ形の判定をここへ追加する
            const bool turbidityMoved = std::abs(m_SkyTurbidity - m_LastBakedTurbidity) > 0.01f;
            if (sunMoved || exposureMoved || turbidityMoved)
            {
                m_SkyBakeDirty = true;
            }
        }

        // このフレームで手続き空を焼くかどうか。下のSkyGenerateパス登録とキャッシュ更新の
        // 両方をこのフラグで判定する
        const bool bakeSkyThisFrame = usingProceduralSky && m_SkyBakeDirty;

        // このフレームでSkyIntegrateパス(P9、m_SkyParametersBufferへ書く)を実行するかどうか。
        // 通常はbakeSkyThisFrameと同じタイミングだが、m_SkyParametersBufferが一度も書かれていない
        // 場合はusingProceduralSkyがfalse(.ksceneのDDSスカイボックス使用時)でも1回だけ実行する。
        //
        // 【なぜCPU側からのゼロ初期化ではなくこの形にしたのか】DX12のStructuredRWバッファは
        // UAV/SRVでのGPUアクセス専用にDEFAULTヒープへ作成されており、CPUから書き込むための
        // マップ済みポインタ・ステージングリングを一切持たない(m_SkyParametersBuffer作成箇所の
        // コメント参照)。そのためUpdateBufferでのゼロ埋めはDX12でクラッシュする。GPU側のパスを
        // 1回だけ走らせれば、DX11/DX12のどちらでも安全に(積分結果自体は使われないが)未初期化状態を
        // 解消できる。太陽方向・目標照度はusingProceduralSkyに関わらず既に計算済みのsunLightingを
        // そのまま使えるため、余分な分岐を増やさずに済む
        const bool skyIntegrateThisFrame = bakeSkyThisFrame || !m_SkyParametersBufferInitialized;

        // --- 空パラメータ(tintと天頂輝度)の確定はP9でGPU側(SkyIntegrate.hlsl)へ移った ---
        // 【なぜベイクと同じタイミングか】背景の解析評価(DeferredLighting.hlsl)は、下のFrameConstants
        // (SkySunDirection)とm_SkyParametersBuffer(SkyIntegrate.hlslの出力)を組み合わせて使う。
        // ベイクと同じタイミングでSkyIntegrateパスを実行することで、背景とキューブマップ
        // (IBL・反射)が常に同一の空パラメータを見る。毎フレーム走らせると、太陽の角度閾値で
        // ベイクを間引いている間だけ背景とIBLの空がずれてしまう。実際のディスパッチとcbuffer更新は
        // 下のSkyIntegrateパス登録側で行うため、ここではフラグ更新のみ済ませる
        if (bakeSkyThisFrame)
        {
            // 雲(P5、判断B)による平均透過率をベイクと同じタイミングで確定させ、メンバへキャッシュする。
            // **この値はm_SkyParametersBuffer側の天頂輝度には掛けない**——キューブへ焼く
            // SkyBakeConstants::CloudTransmittance(下のSkyGenerateパス参照)にだけ掛ける。
            // SkyParametersBufferの天頂輝度を減光すると、雲の隙間から見える青空まで暗くなり、
            // Sky.hlsli側のSkyColorがそこへさらに雲を重ねることで二重に暗くなってしまう
            m_ActiveCloudTransmittance = ComputeCloudAverageTransmittance(
                m_CloudEnabled, m_CloudCoverage, m_CirrusEnabled, m_CirrusCoverage);

            // 空が変わったのでプリフィルタ済み鏡面も焼き直す必要がある。
            // 焼き直し要否のフラグ更新はここ(キャッシュを書いた場所)に一本化し、
            // 下のSkyGenerateパス登録側では行わない(二重更新・更新漏れを避けるため)
            m_SkyBakeDirty = false;
            m_LastBakedSunPosition = sunLighting.SunPosition;
            m_LastBakedExposureEV100 = m_EffectiveExposureEV100;
            m_LastBakedTurbidity = m_SkyTurbidity;
            m_IBLBaked = false;
            m_IBLIrradianceBaked = false;
        }

        // 平面反射(P6): 水面インスタンスを探し、その高さ(ワールドY)を水面の平面とする。
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
        // 【反射の手法がSSRのときだけ実行する】このパスの出力(m_PlanarReflectionColor)を読むのは
        // SSR.hlslだけである。手法がRaytracedやOffのときに走らせても、不透明メッシュ全体を
        // もう1回フォワードで描いた結果を誰も読まないまま捨てることになる
        // (DXR対応環境ではDefaultReflectionModeがRaytracedを返すため、この条件が無いと
        //  DX12では常に丸ごと無駄になる。実測でもDX12起動時に水面へ映っていたのはRT反射の結果で、
        //  平面反射パスの出力ではなかった)
        const bool planarReflectionPassRuns =
            m_PlanarReflectionEnabled && hasWaterInstance && m_ReflectionMode == ReflectionMode::ScreenSpace;

        // 大気遠近パス(P8)を実行するか。UIで無効化されているか、密度が0以下(効果が無い)なら
        // パス自体を登録しない(GetActiveReflectionOutput()の結果がそのままTAA/Tonemapへ渡る)。
        // 手続き空が無効なシーンかどうかの判断(FogParams0.w)はパスの実行有無とは別に、
        // 下のconstants.FogParams0組み立て時にusingProceduralSkyを見て決める
        // (SSRパスのwaterAnalyticSkyFlagと同じ、パスの実行可否とシェーダー内の有効フラグを分ける設計)
        const bool fogPassRuns = m_FogEnabled && m_FogDensity > 0.0f;

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
        constants.CameraPosition = { cameraPosition.x, cameraPosition.y, cameraPosition.z, 0.0f };
        constants.LightDirection = { sunLighting.Direction.x, sunLighting.Direction.y, sunLighting.Direction.z, 0.0f };
        // 太陽を無効にする場合は色をゼロにするだけでよい(シェーダー側は太陽の寄与に
        // LightColor.rgbを乗算するため、これで完全に消える)。TimeOfDayを夜にする方法と違い
        // 昼度(AmbientColor.a)は下がらないので、環境光だけで照らす状態を作れる
        // sunLighting.Color は絶対的な測光量[lx]なので、ここで実効プリ露出を掛けて表示レンジへ移す
        constants.LightColor = m_SunEnabled
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
        // rgb(環境光の色)にm_AmbientScaleを乗算する。Enable IBL無効時のフォールバックアンビエント
        // (DeferredLighting.hlsl)の強度調整用で、alpha(dayFactor、IBLの夜間減光・背景スカイの
        // 昼夜ブレンドに使う)には掛けない
        constants.AmbientColor =
        {
            sunLighting.Ambient.x * m_AmbientScale * effectiveExposure,
            sunLighting.Ambient.y * m_AmbientScale * effectiveExposure,
            sunLighting.Ambient.z * m_AmbientScale * effectiveExposure,
            sunLighting.Ambient.w,
        };
        constants.CascadeSplits = { cascadeSplits[0], cascadeSplits[1], cascadeSplits[2], cascadeSplits[3] };
        const float iblIntensity = m_IBLEnabled ? m_IBLIntensity : 0.0f;
        const float specularEnergyCompensation = static_cast<float>(m_SpecularCompensationMode);
        constants.ShadowParams = {
            m_ShadowLightSize,
            static_cast<float>(kIBLPrefilterMipLevels - 1),
            iblIntensity,
            specularEnergyCompensation,
        };
        constants.ActiveLightCount = { static_cast<float>(gpuLights.size()), 0.0f, 0.0f, 0.0f };
        constants.IBLParams = { m_IBLUseDedicatedIrradiance ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f };

        // 空の解析評価用(P3)。DeferredLighting.hlslが背景画素でSky.hlsliのSkyColorを画面解像度で
        // 評価するために使う。ティントと天頂輝度はP9でm_SkyParametersBuffer(直近の手続き空ベイクで
        // SkyIntegrate.hlslが書いた値。上のbakeSkyThisFrameブロック参照)へ移り、DeferredLighting.hlsl/
        // SSR.hlslがStructuredBufferとして直接読むため、ここでFrameConstantsへ詰める必要は無くなった。
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
            // x=未使用(P9で天頂輝度はSkyParametersBufferへ移動)
            0.0f,
            // 手続き空が無効(.ksceneのDDSスカイボックス使用時)は、この設定に関わらず
            // 常にキューブマップを使う。DDSは任意の絵でPerezモデルとは無関係なため、
            // 解析評価してはいけない
            (m_SkyAnalyticBackground && usingProceduralSky) ? 1.0f : 0.0f,
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
        if (m_ProbeUpdateMode != ProbeUpdateMode::Realtime && m_ProbeBaked && !m_ReflectionProbes.empty() &&
            std::abs(m_EffectiveExposureEV100 - m_ProbeBakedExposureEV100) > kProbeRebakeExposureEV)
        {
            m_ProbeBakeRequested = true;
            // このフレームの後半で今の露出で焼かれるため、換算倍率もここで合わせておく。
            // ここで合わせないと、焼き直したフレームだけ1フレーム古い倍率が掛かって明滅する
            m_ProbeBakedExposureEV100 = m_EffectiveExposureEV100;
        }

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
        constants.ProbeParams2 = {
            m_ProbeDepthParallaxEnabled ? 1.0f : 0.0f,
            m_ProbeOcclusionEnabled ? 1.0f : 0.0f,
            static_cast<float>(kProbeCaptureSize),
            // 焼いた時点の実効プリ露出から現在の実効プリ露出への換算倍率
            // (m_ProbeBakedExposureEV100のコメント参照)。ComputeExposure(ev)=1/(1.2*2^ev)
            // なので、比は 2^(焼いたEV - 現在のEV) になる。
            // フルベイクが走るフレームだけは1フレームぶん古い倍率になるが、それが問題になるのは
            // 「焼き直しと大きな露出変化が同じフレームで起きる」ときだけで、シーン読み込み時は
            // 上のm_EffectiveExposureInitialized=falseで露出が既に確定しているため起きない
            std::exp2(m_ProbeBakedExposureEV100 - m_EffectiveExposureEV100),
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
        // (反射プローブのm_ProbeBakedと同じ方針)
        const bool ddgiActive = m_DDGIEnabled && m_HasGIVolume && m_DDGIBaked;
        constants.DDGIParams0 = {
            m_GIVolume.Origin[0], m_GIVolume.Origin[1], m_GIVolume.Origin[2],
            ddgiActive ? 1.0f : 0.0f,
        };
        constants.DDGIParams1 = {
            m_GIVolume.ProbeSpacing[0], m_GIVolume.ProbeSpacing[1], m_GIVolume.ProbeSpacing[2],
            m_GIVolume.NormalBias,
        };
        constants.DDGIParams2 = {
            static_cast<float>(m_GIVolume.ProbeCounts[0]),
            static_cast<float>(m_GIVolume.ProbeCounts[1]),
            static_cast<float>(m_GIVolume.ProbeCounts[2]),
            m_GIVolume.ViewBias,
        };
        constants.DDGIParams3 = {
            static_cast<float>(kDDGIIrradianceTexels),
            static_cast<float>(kDDGIDistanceTexels),
            m_DDGIIntensity,
            static_cast<float>(kDDGIProbeBorder),
        };
        constants.DDGIParams4 = { effectiveExposure, 0.0f, 0.0f, 0.0f };
        // 水面(P2)。スクロール位相はRenderThreadMainがm_WaterTimeFrozen/m_WaterWaveSpeedに
        // 応じて毎フレーム進める(m_TimeOfDayの自動進行と同じ場所・同じ方式)。
        // y=波のスケール倍率(m_WaterWaveScale)、z=波の強さ(m_WaterWaveStrength、0〜1)を
        // Water.hlslへ渡す(UIのスライダーが見た目へ反映されるようにするため)
        constants.TimeParams = { m_WaterScrollOffset, m_WaterWaveScale, m_WaterWaveStrength, 0.0f };

        // 雲(P5)。DeferredLighting.hlsl(背景)とSSR.hlsl(水面反射)の両方が同じ値を読むため、
        // ここで一度だけ組み立てる。m_CloudEnabled=falseのときはCloudParams0.xへ0を渡し、
        // Sky.hlsli側のSkyColorが早期脱出する経路(判断C)を通す
        constants.CloudParams0 = {
            m_CloudEnabled ? m_CloudCoverage : 0.0f,
            m_CloudAltitude,
            m_CloudUvScale,
            m_CloudDensity,
        };
        constants.CloudParams1 = { m_CloudScrollOffset.x, m_CloudScrollOffset.y, m_CloudForwardG, 0.0f };
        // 巻雲(P11)。積雲と同じ理由でここで一度だけ組み立てる。m_CirrusEnabled=falseのときは
        // CloudParams2.xへ0を渡し、Sky.hlsli側のSkyColorが早期脱出する経路(判断C)を通す
        constants.CloudParams2 = {
            m_CirrusEnabled ? m_CirrusCoverage : 0.0f,
            m_CirrusAltitude,
            m_CirrusUvScale,
            m_CirrusDensity,
        };
        constants.CloudParams3 = { m_CirrusScrollOffset.x, m_CirrusScrollOffset.y, m_CirrusAnisotropy, 0.0f };
        // 平面反射(P6)。このフィールドを参照するのはPlanarReflection.hlslだけで、そちらは
        // 専用のm_PlanarReflectionConstantBufferで明示的に上書きした値を使う(下のPlanarReflection
        // パス登録箇所参照)。共有のm_FrameConstantBufferにも一貫した値を入れておく
        constants.PlanarReflectionPlane = { 0.0f, 1.0f, 0.0f, hasWaterInstance ? -waterPlaneY : 0.0f };

        // 大気遠近(P8)。AerialPerspective.hlsl/PlanarReflection.hlslの両方が読む。
        // 手続き空が無効(.ksceneのDDSスカイボックス使用時)は、m_FogEnabledの値に関わらず
        // 常に無効化する――DDSは任意の絵でPerezモデルとは無関係なため、in-scatter項の
        // 解析評価(SkyColor)をしてはいけない(SSRパスのwaterAnalyticSkyFlagと同じ判断)
        const float fogEnabledFlag = (m_FogEnabled && m_FogDensity > 0.0f && usingProceduralSky) ? 1.0f : 0.0f;
        constants.FogParams0 = { m_FogDensity, m_FogScaleHeight, m_FogRefHeight, fogEnabledFlag };
        constants.FogParams1 = { m_FogMaxOpacity, 0.0f, 0.0f, 0.0f };
        // 水中項(P8)。Water.hlslのPSMainが読む
        constants.WaterBodyColor = { m_WaterBodyColor.x, m_WaterBodyColor.y, m_WaterBodyColor.z, 0.0f };

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
            (m_ShadowMode == ShadowMode::Raytraced && !ShouldRunRaytracedShadow())
                ? ShadowMode::CascadedShadowMap
                : m_ShadowMode;

        LightingConstants lightingConstants{};
        lightingConstants.LightCount =
        {
            static_cast<uint32_t>(gpuLights.size()),
            static_cast<uint32_t>(std::max(0, m_ScreenSpaceShadowMaxLightsPerPixel)),
            static_cast<uint32_t>(effectiveShadowMode),
            0u,
        };
        lightingConstants.SSSParams0 =
        {
            static_cast<float>(m_ScreenSpaceShadowStepCount),
            m_ScreenSpaceShadowMaxRayLength,
            m_ScreenSpaceShadowThickness,
            m_ScreenSpaceShadowEnabled ? 1.0f : 0.0f,
        };
        lightingConstants.SSSParams1 =
        {
            depthLinearizeA,
            depthLinearizeB,
            m_ScreenSpaceShadowNormalBias,
            m_ScreenSpaceShadowEdgeFade,
        };
        lightingConstants.TileParams =
        {
            m_LightTileCountX,
            kLightTileSize,
            kLightTileCapacity,
            m_LightCullingEnabled ? 1u : 0u,
        };

        // ライトリストの中身の更新。以前は直接光パスと半透明パスの中でそれぞれ呼んでいたが、
        // タイルライトカリングパスが両者より先にこのバッファを読むようになったため、
        // グラフを組み立てる前に1箇所でまとめて済ませる(2回の更新が1回に減る副次的な効果もある)。
        // 0灯のフレームでは更新自体を省略してよい(シェーダはライト数までしかループしないため)
        if (!gpuLights.empty())
        {
            commandList->UpdateBuffer(m_LightBuffer.get(), gpuLights.data(), gpuLights.size() * sizeof(GPULight));
        }

        // 各パスをリソースの読み書き依存関係から自動的に順序付けて実行するレンダーグラフ。
        // トランジェントリソースの確保は行わず、既存の永続確保済みテクスチャ(G-Buffer・SceneColor等)を
        // そのまま読み書きする(詳細はRenderGraph.h参照)
        Core::RenderGraph graph(commandList, m_GPUProfiler.get(), &m_CPUProfiler);

        // --- 空パラメータの積分パス(P9): 色味の決定とθ64×φ256=16,384サンプルの照度正規化積分を
        //     GPUで行い、結果(ティント4本+正規化済みの天頂輝度)をm_SkyParametersBufferへ書く。
        //     以前はKurenaiEngine3D.cpp(ComputeSkyTint/ComputeSkyZenithScale)がCPUで計算していたが、
        //     Sky.hlsli側の式と二重実装になっていたためGPU側(SkyIntegrate.hlsl)へ一本化した。
        //     このバッファをSkyGenerate/DeferredLighting/SSRの3者が読むため、下のSkyGenerateパスより
        //     必ず先に実行する必要がある。実行条件はbakeSkyThisFrameではなくskyIntegrateThisFrame
        //     (手続き空が無効なシーンでも初回の1回だけは走らせ、未初期化状態を解消する。
        //     理由はm_SkyParametersBuffer作成箇所とskyIntegrateThisFrame宣言のコメント参照) ---
        if (skyIntegrateThisFrame)
        {
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "SkyIntegrate",
                .BufferWrites = { m_SkyParametersBuffer.get() },
                .Execute = [this, &sunLighting, effectiveExposure](RHI::IRHICommandList* cmd)
                {
                    SkyIntegrateConstants integrateConstants{};
                    integrateConstants.SunDirection = {
                        sunLighting.SunPosition.x, sunLighting.SunPosition.y, sunLighting.SunPosition.z, 0.0f
                    };
                    integrateConstants.IntegrateParams = {
                        sunLighting.SkyIlluminanceLux, effectiveExposure, m_SkyTurbidity, 0.0f
                    };
                    cmd->UpdateBuffer(m_SkyIntegrateConstantBuffer.get(), &integrateConstants, sizeof(integrateConstants));

                    cmd->SetComputePipelineState(m_SkyIntegratePipelineState.get());
                    cmd->SetComputeConstantBuffer(0, m_SkyIntegrateConstantBuffer.get());
                    cmd->SetComputeUnorderedAccessBuffer(0, m_SkyParametersBuffer.get());
                    // 1グループ×256スレッド固定(SkyIntegrate.hlsl参照)
                    cmd->Dispatch(1, 1, 1);
                },
            });
            m_SkyParametersBufferInitialized = true;
        }

        // --- 手続き空の生成パス: Perez分布をGPUで評価してキューブマップを焼く。
        //     太陽が動くと空の輝度分布の形も変わるため、オフラインDDSと違い焼き直しが要る
        //     (詳細はSkyGenerate.hlsl冒頭)。焼き直しの要否・雲の平均透過率のキャッシュ・
        //     m_SkyBakeDirty等のフラグ更新はすべて上のbakeSkyThisFrameブロックで済ませてあるため、
        //     ここではそのキャッシュ(m_ActiveCloudTransmittance)と、直前のSkyIntegrateパスが
        //     書いたm_SkyParametersBufferを使ってパスを登録するだけでよい(P3で前倒し、P9で
        //     ティント・天頂輝度の組み立てをSkyIntegrateパスへ切り出した) ---
        if (bakeSkyThisFrame)
        {
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "SkyGenerate",
                .Writes = { m_ProceduralSkyTexture.get() },
                .BufferReads = { m_SkyParametersBuffer.get() },
                .Execute = [this, &sunLighting](RHI::IRHICommandList* cmd)
                {
                    cmd->SetComputePipelineState(m_SkyGeneratePipelineState.get());
                    cmd->SetComputeShaderResourceBuffer(0, m_SkyParametersBuffer.get());
                    for (uint32_t face = 0; face < kCubeFaceCount; ++face)
                    {
                        SkyBakeConstants skyConstants{};
                        skyConstants.Face = face;
                        // 雲(P5、判断B)。SkyParametersBuffer側の天頂輝度は雲を考慮しない晴天の値の
                        // ままで、キューブへ焼く値にだけm_ActiveCloudTransmittance(被覆率が
                        // 変わらない限り1.0)を掛ける。理由はm_ActiveCloudTransmittanceの
                        // 代入元(上のbakeSkyThisFrameブロック)のコメント参照
                        skyConstants.CloudTransmittance = m_ActiveCloudTransmittance;
                        skyConstants.SunDirection = {
                            sunLighting.SunPosition.x, sunLighting.SunPosition.y, sunLighting.SunPosition.z, 0.0f
                        };
                        cmd->UpdateBuffer(m_SkyBakeConstantBuffer.get(), &skyConstants, sizeof(skyConstants));
                        cmd->SetComputeConstantBuffer(0, m_SkyBakeConstantBuffer.get());
                        cmd->SetComputeUnorderedAccessTextureCubeFace(0, m_ProceduralSkyTexture.get(), face, 0);
                        cmd->Dispatch((kProceduralSkySize + 7) / 8, (kProceduralSkySize + 7) / 8, 1);
                    }
                },
            });
        }

        // --- BRDF積分LUTのベイクパス: (NdotV, ラフネス)の2Dテーブルで、スカイボックスにも
        //     太陽の位置にも一切依存しないため起動後に一度だけ焼く。
        //     プリフィルタ済み鏡面(下記)が空の変化へ追従して焼き直されるようになっても、
        //     こちらが巻き込まれないよう別パス・別フラグに分離してある(m_BRDFLUTBaked参照) ---
        if (!m_BRDFLUTBaked)
        {
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "BRDFLUTBake",
                // 2パス構成のためスクラッチも書き込み対象として挙げる(RenderGraphが
                // パス内の依存を追えるように)。中身の説明はBRDFLUT.hlsl参照
                .Writes = { m_BRDFLUTTexture.get(), m_BRDFLUTScratchTexture.get() },
                .Execute = [this](RHI::IRHICommandList* cmd)
                {
                    // パス1: (A, B)をスクラッチへ焼く
                    cmd->SetComputePipelineState(m_BRDFLUTPipelineState.get());
                    cmd->SetComputeUnorderedAccessTexture(0, m_BRDFLUTScratchTexture.get(), 0);
                    cmd->Dispatch((kIBLBRDFLUTSize + 7) / 8, (kIBLBRDFLUTSize + 7) / 8, 1);

                    // パス2: スクラッチをSRVで読み、Eavgを足した float4(A, B, Eavg, 0) を最終LUTへ。
                    // UAVはDispatch直後に自動で解除されるため、ここで張り直す必要がある
                    // (IRHICommandList.hのバインド寿命の説明を参照)
                    cmd->SetComputePipelineState(m_BRDFLUTCombinePipelineState.get());
                    cmd->SetComputeTexture(0, m_BRDFLUTScratchTexture.get());
                    cmd->SetComputeUnorderedAccessTexture(0, m_BRDFLUTTexture.get(), 0);
                    cmd->Dispatch((kIBLBRDFLUTSize + 7) / 8, (kIBLBRDFLUTSize + 7) / 8, 1);
                },
            });
            m_BRDFLUTBaked = true;
        }

        // --- 雲の3Dノイズのベイクパス(P13a): 形状(128^3)とディテール(32^3)を起動後に一度だけ焼く。
        //     カメラにも太陽にも空の状態にも依存しない純粋な手続き生成なので、BRDF積分LUTと
        //     まったく同じ理由で焼き直さない。2枚は互いに独立なので1パスの中で連続して
        //     ディスパッチしてよい(SRVとして読み合う関係が無く、BRDFLUTの2パス構成のような
        //     中間バッファも要らない) ---
        if (!m_CloudNoiseBaked && m_CloudShapeNoisePipelineState && m_CloudDetailNoisePipelineState)
        {
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "CloudNoiseBake",
                .Writes = { m_CloudShapeNoiseTexture.get(), m_CloudDetailNoiseTexture.get() },
                .Execute = [this](RHI::IRHICommandList* cmd)
                {
                    // スレッドグループは4x4x4。3次元なのでグループあたり64スレッドで、
                    // 2次元パスの8x8(=64)と同じ粒度になる
                    constexpr uint32_t kGroupSize = 4;

                    cmd->SetComputePipelineState(m_CloudShapeNoisePipelineState.get());
                    cmd->SetComputeUnorderedAccessTexture(0, m_CloudShapeNoiseTexture.get(), 0);
                    const uint32_t shapeGroups = (kCloudShapeNoiseSize + kGroupSize - 1) / kGroupSize;
                    cmd->Dispatch(shapeGroups, shapeGroups, shapeGroups);

                    // UAVはDispatch直後に自動で解除されるため張り直す
                    // (IRHICommandList.hのバインド寿命の説明を参照)
                    cmd->SetComputePipelineState(m_CloudDetailNoisePipelineState.get());
                    cmd->SetComputeUnorderedAccessTexture(0, m_CloudDetailNoiseTexture.get(), 0);
                    const uint32_t detailGroups = (kCloudDetailNoiseSize + kGroupSize - 1) / kGroupSize;
                    cmd->Dispatch(detailGroups, detailGroups, detailGroups);
                },
            });
            m_CloudNoiseBaked = true;
        }

        // --- プリフィルタ済み鏡面の畳み込みパス: スカイボックスを入力に、ミップごとに異なる
        //     ラフネスで畳み込む(面×ミップの組み合わせごとに1回ずつディスパッチ) ---
        if (!m_IBLBaked)
        {
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "IBLPrefilter",
                .Reads = { skyTexture },
                .Writes = { m_PrefilteredEnvTexture.get() },
                .Execute = [this, skyTexture](RHI::IRHICommandList* cmd)
                {
                    cmd->SetComputePipelineState(m_PrefilterPipelineState.get());
                    cmd->SetComputeTexture(0, skyTexture);
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

        // --- 専用の拡散イラディアンスマップ(検証用に残している経路) ---
        // 既定の描画経路はプリフィルタ済み鏡面の最終ミップ(roughness=1)である。CSPrefilterは
        // V=R=Nを仮定しているためroughness=1のGGXはコサイン畳み込みへ厳密に退化し、専用マップと
        // 同じE(N)/πを与える(14.10節。White Furnace Testで画素一致を確認済み)。そのため通常の
        // 描画では1テクセルあたり約15,876サンプル(全体で約9,750万サンプル)のCSIrradianceを
        // 一切実行しない。この畳み込み処理自体はいつでも検証できるよう残してあり、ImGuiの
        // 「Use Dedicated Irradiance Map」トグルか、Render Targetsでイラディアンス表示を選んだ
        // ときだけ焼く。RenderGraphがReads/Writesから順序付けるため、トグルを入れたその同じ
        // フレームでLightingパスより先に実行される
        const bool needIrradianceBake =
            m_IBLUseDedicatedIrradiance || m_DebugView == DebugView::IBLIrradiance;
        if (needIrradianceBake && !m_IBLIrradianceBaked)
        {
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "IBLIrradianceBake",
                .Reads = { skyTexture },
                .Writes = { m_IrradianceTexture.get() },
                .Execute = [this, skyTexture](RHI::IRHICommandList* cmd)
                {
                    // 拡散イラディアンス(本物のTextureCube、32x32x6面)。HLSLはリソースを動的に
                    // スライス選択できないため、面ごとに1回ずつディスパッチする
                    cmd->SetComputePipelineState(m_IrradiancePipelineState.get());
                    cmd->SetComputeTexture(0, skyTexture);
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
                },
            });
            m_IBLIrradianceBaked = true;
            Core::Logger::Info("KurenaiEngine3D", "検証用の拡散イラディアンスマップを焼きました(通常の描画経路では使用しません)");
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
                .DepthTarget = m_ShadowCascadeArray.get(),
                .DepthTargetArraySlice = cascade,
                .Execute = [this, &shadowViewport, cascade, &cascadeViewProj](RHI::IRHICommandList* cmd)
                {
                    cmd->SetViewport(shadowViewport);
                    // 深度1.0(最遠)にクリアしておく。無効時はこの後の描画をスキップするため、
                    // シェーダー側は深度比較で常に「影なし」と判定する(ComputeShadowFactor参照)
                    cmd->ClearDepth(1.0f);

                    // RTシャドウ選択時もここは描く。半透明(Transparent.hlsl)と反射プローブの
                    // キャプチャ(ProbeCapture.hlsl)はカメラ視点の可視率テクスチャを使えず、
                    // カスケードシャドウマップを必要とするため(26章)
                    if (m_ShadowMode != ShadowMode::Off)
                    {
                        CascadeConstants cascadeConstants{};
                        DirectX::XMStoreFloat4x4(&cascadeConstants.ViewProj, DirectX::XMMatrixTranspose(cascadeViewProj[cascade]));
                        cmd->UpdateBuffer(m_ShadowCascadeConstantBuffer.get(), &cascadeConstants, sizeof(cascadeConstants));

                        cmd->SetPipelineState(m_ShadowPipelineState.get());
                        cmd->SetConstantBuffer(0, m_ShadowCascadeConstantBuffer.get());

                        // ミラーリングされたインスタンスは表裏が入れ替わるため、GBufferパスと同じく
                        // 表裏判定を反転したパイプラインへ切り替える(切り替え時はb0も張り直す)
                        RHI::IRHIPipelineState* currentPipelineState = m_ShadowPipelineState.get();
                        const auto bindPipelineState = [&](bool mirrored)
                        {
                            RHI::IRHIPipelineState* const wanted =
                                mirrored ? m_ShadowPipelineStateMirrored.get() : m_ShadowPipelineState.get();
                            if (wanted == currentPipelineState)
                            {
                                return;
                            }
                            cmd->SetPipelineState(wanted);
                            cmd->SetConstantBuffer(0, m_ShadowCascadeConstantBuffer.get());
                            currentPipelineState = wanted;
                        };

                        for (const auto& instance : m_Scene.Instances)
                        {
                            for (const auto& mesh : instance.Model.Meshes)
                            {
                                bindPipelineState(instance.IsMirrored);

                                // シャドウパスはWorld以外を使わないが、GBufferパスと同じルートシグネチャ/
                                // 定数バッファ(b1)を共有しているため必ずバインドする必要がある
                                const ObjectConstants objectConstants =
                                    MakeObjectConstants(instance, mesh, m_EmissiveIntensity, m_OcclusionMapEnabled);
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

        // --- 反射プローブの更新(19章・19.10節) ---
        // 更新モードに応じて「フルベイク(全プローブの全面を1フレームで焼く)」か
        // 「時間分割(1フレームに1面だけ焼く)」のどちらかを実行する。両者はスクラッチの
        // キューブマップ(m_ProbeRadianceCube)を共有するため、同じフレームで両方を走らせてはならない

        const DirectX::XMMATRIX probeFaceProjection =
            ComputeCubeFaceProjection(frameState.Camera.GetNearZ(), frameState.Camera.GetFarZ());

        // プローブ1面ぶんのキャプチャ(フォワード描画 → スクラッチのキューブ面へコピー)。
        // フルベイクと時間分割の両方から呼ぶためラムダへ切り出してある
        const auto captureProbeFace =
            [this, &constants, probeFaceProjection, skyTexture](RHI::IRHICommandList* cmd, size_t probeIndex, uint32_t face)
        {
            const Assets::ReflectionProbe& probe = m_ReflectionProbes[probeIndex];
            const DirectX::XMFLOAT3 probePosition{ probe.Position[0], probe.Position[1], probe.Position[2] };

            RHI::Viewport probeViewport;
            probeViewport.Width = static_cast<float>(kProbeCaptureSize);
            probeViewport.Height = static_cast<float>(kProbeCaptureSize);
            // 2枚目は距離(19.12節)。ProbeCapture.hlslのPSOutputと並びを一致させること
            RHI::IRHITexture* const captureTargets[] = { m_ProbeCaptureColor.get(), m_ProbeCaptureDistance.get() };

            // 太陽・カスケード・ライト数・IBL設定は共有のFrameConstantsをそのまま使い、
            // 視点に関わる2つだけをプローブのものへ差し替える(ProbeCapture.hlsl冒頭参照)。
            // Viewはカメラのまま残す(カスケード選択の深度がカメラ視錐台基準のため)
            FrameConstants captureConstants = constants;
            const DirectX::XMMATRIX faceViewProj = ComputeCubeFaceView(probePosition, face) * probeFaceProjection;
            DirectX::XMStoreFloat4x4(&captureConstants.ViewProj, DirectX::XMMatrixTranspose(faceViewProj));
            captureConstants.CameraPosition = { probePosition.x, probePosition.y, probePosition.z, 0.0f };
            // TAA関連のフィールドはカメラ視点のものが入ったままなので、プローブ視点として意味を成すよう
            // 明示的に潰しておく(前フレーム=今フレーム、ジッター無し=速度0)。ProbeCapture.hlslは
            // 現状これらを読まないが、将来読んだときに黙ってカメラの値を拾うのを防ぐため
            captureConstants.PrevViewProj = captureConstants.ViewProj;
            captureConstants.TAAParams = { 0.0f, 0.0f, 0.0f, 0.0f };
            cmd->UpdateBuffer(m_ProbeCaptureConstantBuffer.get(), &captureConstants, sizeof(captureConstants));

            cmd->SetRenderTargets(captureTargets, 2, m_ProbeCaptureDepth.get());
            cmd->SetViewport(probeViewport);
            // 両方のレンダーターゲットが0でクリアされる。距離側の0は「ジオメトリ無し」を意味しないが、
            // コピー側は深度が書かれたかどうかで判定するためこれで問題ない
            cmd->ClearRenderTarget({ 0.0f, 0.0f, 0.0f, 0.0f });
            // Reverse-Zのため遠平面側(NDC z=0.0)にクリアする。コピー側はこの0を
            // 「何も描かれなかった=スカイ」の判定に使う
            cmd->ClearDepth(0.0f);

            cmd->SetPipelineState(m_ProbeCapturePipelineState.get());
            cmd->SetConstantBuffer(0, m_ProbeCaptureConstantBuffer.get());
            cmd->SetSamplerSet(m_MaterialSamplers.get());

            // メッシュによらず共通のバインドはループの外で1回だけ行う。テクスチャのバインドは
            // 上書きするまで維持される(IRHICommandList::SetTexture参照)。以前のDX12は
            // SetTexture(0, ...)のたびにSRVテーブルのブロックを割り当て直していたため、
            // ここで先にバインドしても描画には引き継がれず、プローブが真っ黒に焼ける不具合が出ていた。
            // DX12側がバインド状態のシャドウコピーを持つようになり寿命がDX11と揃ったため解消済み
            cmd->SetTexture(4, m_ShadowCascadeArray.get());
            cmd->SetShaderResourceBuffer(8, m_LightBuffer.get());
            cmd->SetTexture(9, m_IrradianceTexture.get());
            cmd->SetTexture(10, m_PrefilteredEnvTexture.get());
            cmd->SetTexture(11, m_BRDFLUTTexture.get());
            // DDGI(22章)の多重バウンス。ProbeCapture.hlslは拡散の環境光をここから引く。
            // 参照するのは「前フレームまでに焼けているアトラス」で、同じフレームの中でも
            // 既に更新済みのプローブぶんは新しい値になる。DDGIは元々ヒステリシスで
            // 時間収束させる手法なので、この程度の混在は問題にならない
            cmd->SetTexture(12, m_DDGIIrradianceAtlas.get());
            cmd->SetTexture(13, m_DDGIDistanceAtlas.get());

            for (const auto& instance : m_Scene.Instances)
            {
                for (const auto& mesh : instance.Model.Meshes)
                {
                    // 半透明メッシュはプローブへ焼かない。ProbeCapture.hlslは不透明として描くため、
                    // ガラスを焼き込むと「向こう側が見えるはずの面」が不透明の壁としてキューブに
                    // 残り、その裏にある本来映るべき景色が欠ける。半透明を正しく焼くには
                    // キャプチャ側にも奥から手前への描画順とブレンドが要り、コストに見合わない
                    // (プローブへ半透明を含めないのは一般的な割り切り)
                    if (mesh.IsTransparent)
                    {
                        continue;
                    }

                    const ObjectConstants objectConstants = MakeObjectConstants(instance, mesh, m_EmissiveIntensity, m_OcclusionMapEnabled);
                    cmd->UpdateBuffer(m_ObjectConstantBuffer.get(), &objectConstants, sizeof(objectConstants));
                    cmd->SetConstantBuffer(1, m_ObjectConstantBuffer.get());

                    cmd->SetVertexBuffer(mesh.VertexBuffer.get());
                    cmd->SetIndexBuffer(mesh.IndexBuffer.get());

                    // メッシュごとに変わるマテリアルのテクスチャだけをここでバインドする
                    cmd->SetTexture(0, mesh.BaseColorTexture);
                    cmd->SetTexture(1, mesh.NormalTexture);
                    cmd->SetTexture(2, mesh.MetallicRoughnessTexture);
                    cmd->SetTexture(3, mesh.EmissiveTexture);
                    // t4はカスケードシャドウマップ配列が占めているため遮蔽マップはt5
                    cmd->SetTexture(5, mesh.OcclusionTexture);

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
            // ジオメトリが描かれなかったテクセルを埋める空。手続き空が有効なフレームでは
            // そちらを使わないと、プローブにだけ古いDDSの空が焼き込まれて本編と食い違う
            // (このフレームで使う空はRender冒頭のskyTextureに確定させてある)
            cmd->SetComputeTexture(0, skyTexture);
            cmd->SetComputeTexture(1, m_ProbeCaptureColor.get());
            cmd->SetComputeTexture(2, m_ProbeCaptureDepth.get());
            cmd->SetComputeTexture(3, m_ProbeCaptureDistance.get());
            cmd->SetComputeUnorderedAccessTextureCubeFace(0, m_ProbeRadianceCube.get(), face, 0, 0);
            // 距離は畳み込まないため、スクラッチのキューブを経由せずプローブのスライスへ直接書く
            cmd->SetComputeUnorderedAccessTextureCubeFace(
                1, m_ProbeDistanceArray.get(), face, 0, static_cast<uint32_t>(probeIndex));
            cmd->Dispatch((kProbeCaptureSize + 7) / 8, (kProbeCaptureSize + 7) / 8, 1);
        };

        // 組み上がったスクラッチのキューブマップを、IBLとまったく同じ手順で畳み込んで
        // プローブのスライスへ書き込む。入力が違うだけでシェーダーはIBLBakeパスと共通
        const auto convolveProbe = [this](RHI::IRHICommandList* cmd, size_t probeIndex)
        {
            const uint32_t cubeIndex = static_cast<uint32_t>(probeIndex);

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
        };

        // キャプチャパスがReadsにシャドウマップとグローバルの畳み込み結果を挙げることで、
        // レンダーグラフがこれらをシャドウパス・IBLBakeパスより後ろへ順序付ける。
        // 空はm_SkyboxTextureではなくこのフレームで実際に使うskyTextureを挙げる。手続き空のときは
        // SkyGenerateパスがそれのWriterなので、これによりベイクが空の焼き直しより後ろへ順序付けられる
        const std::vector<RHI::IRHITexture*> probeCaptureReads = {
            m_ShadowCascadeArray.get(),
            skyTexture, m_IrradianceTexture.get(), m_PrefilteredEnvTexture.get(), m_BRDFLUTTexture.get(),
        };
        const size_t probeCount = m_ReflectionProbes.size();

        // OnDemandは、焼き上がりに影響する状態(時刻・太陽・ライト)が変わったフレームだけ焼き直す。
        // 一度も焼けていない間はシーン読み込み時の要求が既に立っているのでここでは何もしない
        if (m_ProbeUpdateMode == ProbeUpdateMode::OnDemand && probeCount > 0 && m_ProbeBaked &&
            ComputeProbeBakeSignature() != m_ProbeBakeSignature)
        {
            m_ProbeBakeRequested = true;
        }

        if (m_ProbeBakeRequested && probeCount > 0)
        {
            // --- フルベイク: 全プローブの6面を1フレームで焼く ---
            // プローブごとに別パスへ分けることで、GPUプロファイラで1プローブぶんのコストを読める。
            // 各パスがm_ProbeRadianceCubeへ書くため、レンダーグラフのWrite-after-Write依存で
            // 登録順に直列化される(スクラッチを共有しても取り違えは起きない)
            for (size_t probeIndex = 0; probeIndex < probeCount; ++probeIndex)
            {
                graph.AddPass(Core::RenderGraphPassDesc{
                    .Name = "ProbeBake" + std::to_string(probeIndex),
                    .Reads = probeCaptureReads,
                    .Writes = {
                        m_ProbeCaptureColor.get(), m_ProbeCaptureDistance.get(), m_ProbeCaptureDepth.get(),
                        m_ProbeRadianceCube.get(),
                        m_ProbeIrradianceArray.get(), m_ProbePrefilteredArray.get(), m_ProbeDistanceArray.get(),
                    },
                    .Execute = [&captureProbeFace, &convolveProbe, probeIndex](RHI::IRHICommandList* cmd)
                    {
                        for (uint32_t face = 0; face < kCubeFaceCount; ++face)
                        {
                            captureProbeFace(cmd, probeIndex, face);
                        }
                        convolveProbe(cmd, probeIndex);
                    },
                });
            }

            m_ProbeBakeRequested = false;
            // このフレームの描画時点ではまだ焼き上がっていない(同じコマンドリスト内でこの後の
            // Lightingパスが読むのは問題ないが、gpuProbesは既に確定済み)。次フレームから
            // プローブが有効になるよう、ここでフラグだけ立てる
            m_ProbeBaked = true;
            m_ProbeBakeSignature = ComputeProbeBakeSignature();
            // このフレームの実効プリ露出で焼かれるので、読み出し側の換算倍率もここで更新する
            m_ProbeBakedExposureEV100 = m_EffectiveExposureEV100;
            // 全プローブが今焼けたので、時間分割は先頭から仕切り直す
            m_ProbeRealtimeProbeIndex = 0;
            m_ProbeRealtimeFace = 0;
        }
        else if (m_ProbeUpdateMode == ProbeUpdateMode::Realtime && probeCount > 0 && m_ProbeBaked)
        {
            // --- 時間分割: 1フレームにつき1面だけ焼き、6面揃った時点で畳み込んで次のプローブへ回る ---
            // 畳み込みは6面が揃うまで走らないため、その間プローブのスライスは前回の内容のまま
            // 表示され続ける(描きかけのキューブが映り込むことはない)。
            // m_ProbeBakedがtrueであること、つまり最低1回フルベイクが済んでいることが前提
            if (m_ProbeRealtimeProbeIndex >= probeCount)
            {
                m_ProbeRealtimeProbeIndex = 0;
                m_ProbeRealtimeFace = 0;
            }
            const size_t realtimeProbe = m_ProbeRealtimeProbeIndex;
            const uint32_t realtimeFace = m_ProbeRealtimeFace;

            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "ProbeRealtimeCapture",
                .Reads = probeCaptureReads,
                .Writes = {
                    m_ProbeCaptureColor.get(), m_ProbeCaptureDistance.get(), m_ProbeCaptureDepth.get(),
                    m_ProbeRadianceCube.get(), m_ProbeDistanceArray.get(),
                },
                .Execute = [&captureProbeFace, realtimeProbe, realtimeFace](RHI::IRHICommandList* cmd)
                {
                    captureProbeFace(cmd, realtimeProbe, realtimeFace);
                },
            });

            if (realtimeFace + 1 == kCubeFaceCount)
            {
                // 畳み込みだけを別パスにしてあるのは、6フレームに1回だけ乗るこのコストを
                // 毎フレームのキャプチャと分けて計測できるようにするため。
                // Readsにスクラッチのキューブマップがあるのでキャプチャパスのあとに順序付けられる
                graph.AddPass(Core::RenderGraphPassDesc{
                    .Name = "ProbeRealtimeConvolve",
                    .Reads = { m_ProbeRadianceCube.get() },
                    .Writes = { m_ProbeIrradianceArray.get(), m_ProbePrefilteredArray.get() },
                    .Execute = [&convolveProbe, realtimeProbe](RHI::IRHICommandList* cmd)
                    {
                        convolveProbe(cmd, realtimeProbe);
                    },
                });
            }

            m_ProbeRealtimeFace = realtimeFace + 1;
            if (m_ProbeRealtimeFace >= kCubeFaceCount)
            {
                m_ProbeRealtimeFace = 0;
                m_ProbeRealtimeProbeIndex = static_cast<uint32_t>((realtimeProbe + 1) % probeCount);
            }
            // 常に焼き直しているのでOnDemandの署名も追随させておく。こうしておかないと
            // Realtimeから切り替えた直後に不要なフルベイクが1回走る
            m_ProbeBakeSignature = ComputeProbeBakeSignature();
            // 露出の換算倍率も追随させる。1面ずつ焼くため厳密には面ごとに焼いた露出が違うが、
            // 実効プリ露出の変化は毎秒2倍程度(m_EffectiveExposureAdaptSpeed)なので
            // 6フレームぶんのずれは数%にとどまり、常時焼き直している以上すぐ解消する
            m_ProbeBakedExposureEV100 = m_EffectiveExposureEV100;
        }

        // --- DDGIのプローブ更新(22章) ---
        // 反射プローブとまったく同じキャプチャ経路を使い、解像度だけkDDGICaptureSizeへ落とす。
        // 6面×16×16 = 1536テクセルがそのままDDGIの「1536本のレイ」になる。
        // フルベイクは持たず、初回も含めて常に1フレームm_DDGIProbesPerFrame個ずつ時間分割で回す
        // (理由はKurenaiEngine3D.hのm_DDGIWarmingUpのコメント参照)

        // プローブ1面ぶんのキャプチャ → スクラッチのキューブ2本(放射輝度・距離)の該当面へコピー。
        // コピーCSはIBLConvolve.hlslのCSCopyCaptureToCubeFaceをそのまま使う。u1の宣言が
        // RWTexture2DArray<float>なので、キューブ配列だけでなく単体のキューブ(=6要素の2D配列)の
        // 面へもそのまま書ける
        const auto captureDDGIProbeFace =
            [this, &constants, probeFaceProjection, skyTexture](RHI::IRHICommandList* cmd, uint32_t probeIndex, uint32_t face)
        {
            const DirectX::XMFLOAT3 probePosition = ComputeDDGIProbePosition(probeIndex);

            RHI::Viewport ddgiViewport;
            ddgiViewport.Width = static_cast<float>(kDDGICaptureSize);
            ddgiViewport.Height = static_cast<float>(kDDGICaptureSize);
            RHI::IRHITexture* const captureTargets[] = { m_DDGICaptureColor.get(), m_DDGICaptureDistance.get() };

            FrameConstants captureConstants = constants;
            const DirectX::XMMATRIX faceViewProj = ComputeCubeFaceView(probePosition, face) * probeFaceProjection;
            DirectX::XMStoreFloat4x4(&captureConstants.ViewProj, DirectX::XMMatrixTranspose(faceViewProj));
            captureConstants.CameraPosition = { probePosition.x, probePosition.y, probePosition.z, 0.0f };
            cmd->UpdateBuffer(m_ProbeCaptureConstantBuffer.get(), &captureConstants, sizeof(captureConstants));

            cmd->SetRenderTargets(captureTargets, 2, m_DDGICaptureDepth.get());
            cmd->SetViewport(ddgiViewport);
            cmd->ClearRenderTarget({ 0.0f, 0.0f, 0.0f, 0.0f });
            // Reverse-Zのため遠平面側(NDC z=0.0)。コピー側はこの0を「空」の判定に使う
            cmd->ClearDepth(0.0f);

            // PSOは反射プローブと共通(同じシェーダー・同じレンダーターゲットフォーマット)
            cmd->SetPipelineState(m_ProbeCapturePipelineState.get());
            cmd->SetConstantBuffer(0, m_ProbeCaptureConstantBuffer.get());
            cmd->SetSamplerSet(m_MaterialSamplers.get());

            cmd->SetTexture(4, m_ShadowCascadeArray.get());
            cmd->SetShaderResourceBuffer(8, m_LightBuffer.get());
            cmd->SetTexture(9, m_IrradianceTexture.get());
            cmd->SetTexture(10, m_PrefilteredEnvTexture.get());
            cmd->SetTexture(11, m_BRDFLUTTexture.get());
            // DDGI(22章)の多重バウンス。ProbeCapture.hlslは拡散の環境光をここから引く。
            // 参照するのは「前フレームまでに焼けているアトラス」で、同じフレームの中でも
            // 既に更新済みのプローブぶんは新しい値になる。DDGIは元々ヒステリシスで
            // 時間収束させる手法なので、この程度の混在は問題にならない
            cmd->SetTexture(12, m_DDGIIrradianceAtlas.get());
            cmd->SetTexture(13, m_DDGIDistanceAtlas.get());

            for (const auto& instance : m_Scene.Instances)
            {
                for (const auto& mesh : instance.Model.Meshes)
                {
                    // 半透明メッシュを焼かない理由は反射プローブと同じ(不透明として描かれるため、
                    // ガラスが壁になって裏の景色が欠ける)
                    if (mesh.IsTransparent)
                    {
                        continue;
                    }

                    const ObjectConstants objectConstants = MakeObjectConstants(instance, mesh, m_EmissiveIntensity, m_OcclusionMapEnabled);
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

            // 描いたカラー/深度をコンピュートからSRVで読むため、先にRTVを外す(DX11の制約)
            cmd->SetRenderTargets(nullptr, 0, nullptr);

            IBLFaceConstants faceConstants{};
            faceConstants.Face = face;
            cmd->SetComputePipelineState(m_ProbeCubeCopyPipelineState.get());
            cmd->UpdateBuffer(m_IBLPrefilterConstantBuffer.get(), &faceConstants, sizeof(faceConstants));
            cmd->SetComputeConstantBuffer(0, m_IBLPrefilterConstantBuffer.get());
            cmd->SetComputeSamplerSet(m_MaterialSamplers.get());
            cmd->SetComputeTexture(0, skyTexture);
            cmd->SetComputeTexture(1, m_DDGICaptureColor.get());
            cmd->SetComputeTexture(2, m_DDGICaptureDepth.get());
            cmd->SetComputeTexture(3, m_DDGICaptureDistance.get());
            cmd->SetComputeUnorderedAccessTextureCubeFace(0, m_DDGICaptureRadianceCube.get(), face, 0, 0);
            cmd->SetComputeUnorderedAccessTextureCubeFace(1, m_DDGICaptureDistanceCube.get(), face, 0, 0);
            cmd->Dispatch((kDDGICaptureSize + 7) / 8, (kDDGICaptureSize + 7) / 8, 1);
        };

        // 組み上がったキューブ2本から、オクタヘドラルアトラスの該当セルを焼き直す。
        // 境界の複製は本体の書き込みが全て終わってからでないと正しい値を読めないので別ディスパッチ
        const auto updateDDGIProbe = [this, effectiveExposure](RHI::IRHICommandList* cmd, uint32_t probeIndex, bool overwrite)
        {
            DDGIUpdateConstants updateConstants{};
            updateConstants.Params0 = {
                static_cast<float>(probeIndex),
                m_GIVolume.Hysteresis,
                m_GIVolume.MaxRayDistance,
                static_cast<float>(kDDGICaptureSize),
            };
            updateConstants.Params1 = {
                static_cast<float>(kDDGIIrradianceTexels),
                static_cast<float>(kDDGIDistanceTexels),
                static_cast<float>(kDDGIProbeBorder),
                overwrite ? 1.0f : 0.0f,
            };
            updateConstants.Params2 = {
                static_cast<float>(m_GIVolume.ProbeCounts[0]),
                static_cast<float>(m_GIVolume.ProbeCounts[1]),
                static_cast<float>(m_GIVolume.ProbeCounts[2]),
                effectiveExposure,
            };
            cmd->UpdateBuffer(m_DDGIUpdateConstantBuffer.get(), &updateConstants, sizeof(updateConstants));

            // 本体の書き込み。スレッドは2つの解像度の広いほうに合わせて起動し、
            // それぞれの範囲外はシェーダー側で弾く
            constexpr uint32_t kUpdateThreads = (kDDGIIrradianceTexels > kDDGIDistanceTexels)
                ? kDDGIIrradianceTexels : kDDGIDistanceTexels;
            cmd->SetComputePipelineState(m_DDGIProbeUpdatePipelineState.get());
            cmd->SetComputeConstantBuffer(0, m_DDGIUpdateConstantBuffer.get());
            cmd->SetComputeSamplerSet(m_MaterialSamplers.get());
            cmd->SetComputeTexture(0, m_DDGICaptureRadianceCube.get());
            cmd->SetComputeTexture(1, m_DDGICaptureDistanceCube.get());
            cmd->SetComputeUnorderedAccessTexture(0, m_DDGIIrradianceAtlas.get());
            cmd->SetComputeUnorderedAccessTexture(1, m_DDGIDistanceAtlas.get());
            cmd->Dispatch((kUpdateThreads + 7) / 8, (kUpdateThreads + 7) / 8, 1);

            // 境界の複製。セル全体(境界込み)を走査するので広いほうのセルサイズに合わせる
            constexpr uint32_t kBorderThreads = (kDDGIIrradianceCell > kDDGIDistanceCell)
                ? kDDGIIrradianceCell : kDDGIDistanceCell;
            cmd->SetComputePipelineState(m_DDGIBorderCopyPipelineState.get());
            cmd->SetComputeConstantBuffer(0, m_DDGIUpdateConstantBuffer.get());
            cmd->SetComputeUnorderedAccessTexture(0, m_DDGIIrradianceAtlas.get());
            cmd->SetComputeUnorderedAccessTexture(1, m_DDGIDistanceAtlas.get());
            cmd->Dispatch((kBorderThreads + 7) / 8, (kBorderThreads + 7) / 8, 1);
        };

        if (m_DDGIEnabled && m_HasGIVolume && m_DDGIProbeCount > 0)
        {
            const uint32_t perFrame = std::min<uint32_t>(
                static_cast<uint32_t>(std::max(m_DDGIProbesPerFrame, 1)), m_DDGIProbeCount);
            const bool warmingUp = m_DDGIWarmingUp;

            // 実効プリ露出が大きく動いたら、一巡ぶんだけ上書きへ切り替えて即座に追従させる
            // (理由はKurenaiEngine3D.hのm_DDGIOverwriteRemainingのコメント参照)。
            // 一巡目(warmingUp)は元から上書きなので何もしない
            if (!warmingUp)
            {
                if (!m_DDGILastExposureValid)
                {
                    m_DDGILastExposureEV100 = m_EffectiveExposureEV100;
                    m_DDGILastExposureValid = true;
                }
                else if (std::abs(m_EffectiveExposureEV100 - m_DDGILastExposureEV100) > kDDGIExposureRewarmEV)
                {
                    m_DDGIOverwriteRemaining = m_DDGIProbeCount;
                    m_DDGILastExposureEV100 = m_EffectiveExposureEV100;
                }
            }
            // このフレームで上書きするぶんを先に確定させる(ラムダへ値で渡すため)
            const uint32_t overwriteThisFrame = std::min(m_DDGIOverwriteRemaining, perFrame);
            m_DDGIOverwriteRemaining -= overwriteThisFrame;

            for (uint32_t i = 0; i < perFrame; ++i)
            {
                const uint32_t probeIndex = (m_DDGIUpdateCursor + i) % m_DDGIProbeCount;
                // 一巡目はヒステリシスを使わず上書きする(混ぜる相手の「前の値」が未初期化のため)。
                // 露出が急変した直後も同じく上書きで追従させる
                const bool overwrite = warmingUp || (i < overwriteThisFrame);

                graph.AddPass(Core::RenderGraphPassDesc{
                    .Name = "DDGIUpdate" + std::to_string(probeIndex),
                    .Reads = probeCaptureReads,
                    .Writes = {
                        m_DDGICaptureColor.get(), m_DDGICaptureDistance.get(), m_DDGICaptureDepth.get(),
                        m_DDGICaptureRadianceCube.get(), m_DDGICaptureDistanceCube.get(),
                        m_DDGIIrradianceAtlas.get(), m_DDGIDistanceAtlas.get(),
                    },
                    .Execute = [&captureDDGIProbeFace, &updateDDGIProbe, probeIndex, overwrite](RHI::IRHICommandList* cmd)
                    {
                        for (uint32_t face = 0; face < kCubeFaceCount; ++face)
                        {
                            captureDDGIProbeFace(cmd, probeIndex, face);
                        }
                        updateDDGIProbe(cmd, probeIndex, overwrite);
                    },
                });
            }

            const uint32_t nextCursor = m_DDGIUpdateCursor + perFrame;
            if (warmingUp && nextCursor >= m_DDGIProbeCount)
            {
                // 全プローブが一度ずつ書かれた。ここから先はヒステリシスで滑らかに追従させ、
                // 同時にサンプリング側(DDGIParams0.w)を有効にする
                m_DDGIWarmingUp = false;
                m_DDGIBaked = true;
                m_DDGILastExposureEV100 = m_EffectiveExposureEV100;
                m_DDGILastExposureValid = true;
                Core::Logger::Info(
                    "KurenaiEngine3D",
                    "DDGIの初回一巡が完了しました(" + std::to_string(m_DDGIProbeCount) + "プローブ)");
            }
            m_DDGIUpdateCursor = nextCursor % m_DDGIProbeCount;
        }

        // --- ジオメトリパス: G-Bufferへ書き込む(常に指定した内部解像度) ---
        graph.AddPass(Core::RenderGraphPassDesc{
            .Name = "GBuffer",
            // 5枚目の速度(モーションベクター)まで含め、並びはGBuffer.hlslのPSOutputおよび
            // CreatePrecisionDependentPipelineStatesのRenderTargetFormatsと一致させること
            .RenderTargets = { m_GBufferAlbedo.get(), m_GBufferNormal.get(), m_GBufferMaterial.get(),
                               m_GBufferEmissive.get(), m_GBufferVelocity.get() },
            .DepthTarget = m_GBufferDepth.get(),
            .Execute = [this, &gbufferViewport](RHI::IRHICommandList* cmd)
            {
                cmd->SetViewport(gbufferViewport);
                // ClearRenderTargetはバインド済みの全レンダーターゲットを同じ色でクリアするため、
                // 速度バッファもここで0(=動いていない)になる。ジオメトリが描かれない画素
                // (空)の速度は0のまま残るが、空はカメラ回転で動くのでTAA側で別途補う(TAA.hlsl参照)
                cmd->ClearRenderTarget({ 0.0f, 0.0f, 0.0f, 0.0f });
                // Reverse-Zのため遠平面側(NDC z=0.0)にクリアする(GBuffer.hlsl参照)
                cmd->ClearDepth(0.0f);

                cmd->SetPipelineState(m_GBufferPipelineState.get());
                cmd->SetConstantBuffer(0, m_FrameConstantBuffer.get());
                cmd->SetSamplerSet(m_MaterialSamplers.get());

                // ミラーリング(Worldの行列式が負)されたインスタンス・水面(ModelInstance::IsWater)
                // インスタンスの組み合わせ(4通り)に応じてパイプラインを切り替える。上で通常の
                // パイプラインを先にバインドしてあるため、どちらも含まないシーンでは以下のラムダは
                // 一度も切り替えを行わず、発行されるコマンド列はこの機能の追加前と完全に同一になる。
                // DX12のSetPipelineStateはルートシグネチャを張り直して既存のバインドを
                // 無効化するので、切り替えたときはパス共通のバインドもやり直す
                RHI::IRHIPipelineState* currentPipelineState = m_GBufferPipelineState.get();
                const auto bindPipelineState = [&](bool mirrored, bool water)
                {
                    RHI::IRHIPipelineState* const wanted = water
                        ? (mirrored ? m_GBufferWaterPipelineStateMirrored.get() : m_GBufferWaterPipelineState.get())
                        : (mirrored ? m_GBufferPipelineStateMirrored.get() : m_GBufferPipelineState.get());
                    if (wanted == currentPipelineState)
                    {
                        return;
                    }
                    cmd->SetPipelineState(wanted);
                    cmd->SetConstantBuffer(0, m_FrameConstantBuffer.get());
                    cmd->SetSamplerSet(m_MaterialSamplers.get());
                    currentPipelineState = wanted;
                };

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

                        bindPipelineState(instance.IsMirrored, instance.IsWater);

                        const ObjectConstants objectConstants =
                            MakeObjectConstants(instance, mesh, m_EmissiveIntensity, m_OcclusionMapEnabled);
                        cmd->UpdateBuffer(m_ObjectConstantBuffer.get(), &objectConstants, sizeof(objectConstants));
                        cmd->SetConstantBuffer(1, m_ObjectConstantBuffer.get());

                        cmd->SetVertexBuffer(mesh.VertexBuffer.get());
                        cmd->SetIndexBuffer(mesh.IndexBuffer.get());
                        cmd->SetTexture(0, mesh.BaseColorTexture);
                        cmd->SetTexture(1, mesh.NormalTexture);
                        cmd->SetTexture(2, mesh.MetallicRoughnessTexture);
                        cmd->SetTexture(3, mesh.EmissiveTexture);
                        cmd->SetTexture(5, mesh.OcclusionTexture);
                        if (instance.IsWater)
                        {
                            // Water.hlslのPSMainだけが読むt6。通常のGBuffer PSOはt6を宣言していないため
                            // 水面以外のインスタンスではバインドしない
                            cmd->SetTexture(6, m_WaterNormalMapTexture.get());
                        }
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

        // --- タイルライトカリングパス: 画面を16x16のタイルに分け、タイルごとに「そのタイルに届くライト」の
        //     インデックスリストをコンピュートシェーダーで作る。直接光パスはそのリストだけをループする。
        //     BufferReads/BufferWritesを宣言しているのは、このパスと直接光パスがどちらもm_GBufferDepthを
        //     Readsするだけの「読み手同士」で、テクスチャの依存だけでは両者の間に順序が張られないため ---
        if (m_LightCullingEnabled)
        {
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "LightCull",
                .Reads = { m_GBufferDepth.get() },
                .BufferReads = { m_LightBuffer.get() },
                .BufferWrites = { m_LightTileBuffer.get() },
                .Execute = [this, &gpuLights, viewMatrix, jitteredProj](RHI::IRHICommandList* cmd)
                {
                    LightCullingConstants cullingConstants{};
                    DirectX::XMStoreFloat4x4(
                        &cullingConstants.View, DirectX::XMMatrixTranspose(viewMatrix));
                    cullingConstants.TileParams =
                    {
                        m_LightTileCountX,
                        m_LightTileCountY,
                        static_cast<uint32_t>(gpuLights.size()),
                        kLightTileCapacity,
                    };
                    cullingConstants.RenderSize = { m_RenderWidth, m_RenderHeight, 0u, 0u };

                    // タイル錐台の側面を組み立てるのに射影行列の(0,0)/(1,1)成分が要る。
                    // 深度リニアライズ定数(z/w)は直接光パスへ渡しているものと同じ。
                    // ここで読む_11/_22/_33/_43はいずれもTAAのジッター(_31/_32のみを書き換える)では
                    // 変化しないが、深度バッファを描いたときと同じ行列から導くという規約に揃えている
                    DirectX::XMFLOAT4X4 projection;
                    DirectX::XMStoreFloat4x4(&projection, jitteredProj);
                    cullingConstants.ProjParams =
                    {
                        projection._11,
                        projection._22,
                        projection._33,
                        projection._43,
                    };

                    cmd->UpdateBuffer(m_LightCullingConstantBuffer.get(), &cullingConstants, sizeof(cullingConstants));

                    cmd->SetComputePipelineState(m_LightCullingPipelineState.get());
                    cmd->SetComputeConstantBuffer(0, m_LightCullingConstantBuffer.get());
                    cmd->SetComputeShaderResourceBuffer(0, m_LightBuffer.get());
                    cmd->SetComputeTexture(1, m_GBufferDepth.get());
                    cmd->SetComputeUnorderedAccessBuffer(0, m_LightTileBuffer.get());
                    cmd->Dispatch(m_LightTileCountX, m_LightTileCountY, 1);
                },
            });
        }

        // --- RTシャドウパス: TLASへ太陽の見かけの円盤方向へ影レイを撃ち、可視率(0〜1)を
        //     単チャンネルのテクスチャへ書く。直後の直接光パスがt6でこれを読む ---
        if (ShouldRunRaytracedShadow())
        {
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "RTShadow",
                .Reads = { m_GBufferNormal.get(), m_GBufferDepth.get() },
                .Writes = { m_RTShadowTexture.get() },
                .Execute = [this](RHI::IRHICommandList* cmd)
                {
                    RTShadowConstants rtShadowConstants{};
                    rtShadowConstants.Params0 =
                    {
                        static_cast<float>(m_RenderWidth),
                        static_cast<float>(m_RenderHeight),
                        DirectX::XMConvertToRadians(m_RTShadowSunAngularRadiusDegrees),
                        static_cast<float>(std::max(1, m_RTShadowSampleCount)),
                    };
                    cmd->UpdateBuffer(m_RTShadowConstantBuffer.get(), &rtShadowConstants, sizeof(rtShadowConstants));

                    cmd->SetComputePipelineState(m_RTShadowPipelineState.get());
                    cmd->SetComputeConstantBuffer(0, m_FrameConstantBuffer.get());
                    cmd->SetComputeConstantBuffer(1, m_RTShadowConstantBuffer.get());

                    // レジスタ割り当てはRTShadow.hlsl側の宣言と一致させること。
                    // このシェーダはLoad(整数座標)しか使わないためサンプラーはバインドしない
                    cmd->SetComputeAccelerationStructure(0, m_RaytracingScene.GetTopLevelAS());
                    cmd->SetComputeTexture(1, m_GBufferNormal.get());
                    cmd->SetComputeTexture(2, m_GBufferDepth.get());

                    // UAVはDispatch直後に解除されるため毎回バインドし直す(IRHICommandList.h参照)
                    cmd->SetComputeUnorderedAccessTexture(0, m_RTShadowTexture.get());
                    cmd->Dispatch((m_RenderWidth + 7) / 8, (m_RenderHeight + 7) / 8, 1);
                },
            });
        }

        // 直接光パスがt6へバインドする可視率テクスチャ。DirectLighting.hlslは
        // LightCount.zがRaytracedのときしか読まないが、DX12はSetPipelineStateのたびに
        // ルート引数が無効化されるため、シェーダが宣言しているリソースは必ず何かをバインドする
        // 必要がある(nullptrはSetTextureが受け付けない)。非対応環境では読まれないダミーとして
        // 深度テクスチャを張る(Presentのデバッグ用t1/t2/t4に既定値を持たせているのと同じ理由)
        RHI::IRHITexture* const rtShadowTextureForBinding =
            m_RTShadowTexture ? m_RTShadowTexture.get() : m_GBufferDepth.get();

        // --- 直接光パス: G-Buffer+シャドウマップ(またはRTシャドウの可視率)からPBRの直接光
        //     (拡散+鏡面反射、シャドウ適用済み)を計算しHDRで書き出す(常に指定した内部解像度)。
        //     DeferredLighting/SSILの両方から読まれる ---
        graph.AddPass(Core::RenderGraphPassDesc{
            .Name = "DirectLight",
            .Reads =
            {
                m_GBufferAlbedo.get(), m_GBufferNormal.get(), m_GBufferMaterial.get(), m_GBufferDepth.get(),
                m_ShadowCascadeArray.get(),
                // RTシャドウの可視率。RTシャドウパスを実行しないフレームではm_GBufferDepthと
                // 同じポインタになるが、RenderGraphは同じ書き手への多重エッジを弾くため無害
                rtShadowTextureForBinding,
                // スペキュラのエネルギー補正(14.9節)でEss=brdf.x+brdf.yを引くためBRDF積分LUTを読む。
                // Readsに挙げることでRenderGraphがBRDFLUTBakeパス(このLUTのWriter)より後に順序付ける
                m_BRDFLUTTexture.get(),
            },
            .RenderTargets = { m_DirectLightTexture.get() },
            .Execute = [this, &gbufferViewport, &gpuLights, &lightingConstants, rtShadowTextureForBinding](
                           RHI::IRHICommandList* cmd)
            {
                cmd->SetViewport(gbufferViewport);

                cmd->SetPipelineState(m_DirectLightPipelineState.get());
                cmd->SetConstantBuffer(0, m_FrameConstantBuffer.get());

                // UpdateBufferはSetConstantBufferより前に呼ぶ必要がある。DX12の定数バッファは
                // リングバッファで、GetGPUVirtualAddress()が現在のリングスロットのアドレスを返すため
                cmd->UpdateBuffer(m_LightingConstantBuffer.get(), &lightingConstants, sizeof(lightingConstants));
                cmd->SetConstantBuffer(1, m_LightingConstantBuffer.get());

                cmd->SetSamplerSet(m_ScreenSpaceSamplers.get());
                cmd->SetTexture(0, m_GBufferAlbedo.get());
                cmd->SetTexture(1, m_GBufferNormal.get());
                cmd->SetTexture(2, m_GBufferMaterial.get());
                cmd->SetTexture(3, m_GBufferDepth.get());
                cmd->SetTexture(4, m_ShadowCascadeArray.get());
                // RTシャドウの可視率。LightCount.zがRaytracedのときだけ読まれる
                cmd->SetTexture(6, rtShadowTextureForBinding);

                // ライトが1つも無いフレームでもSetShaderResourceBufferは必ず呼ぶ(SetPipelineStateが
                // 毎回ルート引数を無効化するため、シェーダが宣言しているリソースを未バインドのまま
                // Drawすることになってしまう)。バッファの中身の更新はグラフ構築前に1回だけ済ませてある
                cmd->SetShaderResourceBuffer(8, m_LightBuffer.get());
                // タイルライトカリングが書いたライトグリッド。カリング無効時もシェーダが宣言している
                // リソースは必ずバインドする(上と同じ理由)
                cmd->SetShaderResourceBuffer(5, m_LightTileBuffer.get());
                // スペキュラのエネルギー補正(14.9節)用のBRDF積分LUT。t8はライトリスト
                // (StructuredBuffer)が占有しているためt9に置く
                cmd->SetTexture(9, m_BRDFLUTTexture.get());

                cmd->Draw(3, 0);
            },
        });

        // --- AO/GIパス: 選択中の手法(SSAO / SSIL / RTAO)で遮蔽率(・間接拡散光)を計算し、
        //     ブラーで均す(常に指定した内部解像度)。出力フォーマットはどれもrgb=間接拡散光, a=遮蔽率で共通 ---
        if (m_AOEnabled)
        {
            RHI::IRHITexture* const aoRawTexture = GetActiveAORawTexture();
            RHI::IRHITexture* const aoBlurredTexture = GetActiveAOTexture();
            const bool useSSIL = !ShouldRunRaytracedAO() && m_AOTechnique == AOTechnique::SSILVisibilityBitmask;

            if (ShouldRunRaytracedAO())
            {
                // RTAOパス。SSAO/SSILと違いコンピュートでUAVへ書くため、レンダーターゲットではなく
                // Writesで宣言する。レジスタ割り当てはRTAO.hlsl側の宣言と一致させること
                graph.AddPass(Core::RenderGraphPassDesc{
                    .Name = "RTAO",
                    // 直接光バッファは、バウンス面が画面に映っているときの再放射の放射輝度として読む
                    // (SSILと同じ理由でDirectLightパスより後に順序付けられる。RTAO.hlsl参照)
                    .Reads = { m_GBufferNormal.get(), m_GBufferDepth.get(), m_DirectLightTexture.get() },
                    .Writes = { aoRawTexture },
                    .Execute = [this](RHI::IRHICommandList* cmd)
                    {
                        RTAOConstants rtAOConstants{};
                        rtAOConstants.Params0 = {
                            static_cast<float>(m_RenderWidth), static_cast<float>(m_RenderHeight),
                            m_RTAOMaxDistance, m_RTAOPower
                        };
                        rtAOConstants.Params1 = {
                            static_cast<float>(std::max(1, m_RTAOSampleCount)), m_RTAOIntensity,
                            m_RTAOBounceShadowRayEnabled ? 1.0f : 0.0f, 0.0f
                        };
                        cmd->UpdateBuffer(m_RTAOConstantBuffer.get(), &rtAOConstants, sizeof(rtAOConstants));

                        cmd->SetComputePipelineState(m_RTAOPipelineState.get());
                        cmd->SetComputeConstantBuffer(0, m_FrameConstantBuffer.get());
                        cmd->SetComputeConstantBuffer(1, m_RTAOConstantBuffer.get());

                        cmd->SetComputeAccelerationStructure(0, m_RaytracingScene.GetTopLevelAS());
                        cmd->SetComputeTexture(1, m_GBufferNormal.get());
                        cmd->SetComputeTexture(2, m_GBufferDepth.get());
                        cmd->SetComputeShaderResourceBuffer(3, m_RaytracingScene.GetVertexAttributeBuffer());
                        cmd->SetComputeShaderResourceBuffer(4, m_RaytracingScene.GetIndexBuffer());
                        cmd->SetComputeShaderResourceBuffer(5, m_RaytracingScene.GetMeshInfoBuffer());
                        cmd->SetComputeShaderResourceBuffer(6, m_RaytracingScene.GetInstanceInfoBuffer());
                        cmd->SetComputeShaderResourceBuffer(7, m_RaytracingScene.GetMaterialBuffer());
                        cmd->SetComputeTexture(8, m_DirectLightTexture.get());

                        // UAVはDispatch直後に解除されるため毎回バインドし直す(IRHICommandList.h参照)
                        cmd->SetComputeUnorderedAccessTexture(0, m_RTAORawTexture.get());
                        cmd->Dispatch((m_RenderWidth + 7) / 8, (m_RenderHeight + 7) / 8, 1);
                    },
                });
            }
            else
            {
                graph.AddPass(Core::RenderGraphPassDesc{
                    .Name = "AO",
                    .Reads = useSSIL
                        ? std::vector<RHI::IRHITexture*>{ m_GBufferNormal.get(), m_GBufferDepth.get(), m_DirectLightTexture.get() }
                        : std::vector<RHI::IRHITexture*>{ m_GBufferNormal.get(), m_GBufferDepth.get() },
                    .RenderTargets = { aoRawTexture },
                    .Execute = [this, &gbufferViewport, useSSIL](RHI::IRHICommandList* cmd)
                    {
                        cmd->SetViewport(gbufferViewport);
                        cmd->SetConstantBuffer(0, m_FrameConstantBuffer.get());
                        cmd->SetSamplerSet(m_ScreenSpaceSamplers.get());

                        if (useSSIL)
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
                        else
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
                    },
                });
            }

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

        // デバッグ表示(ブラー前確認用)のため、ブラー前の生バッファへの参照も別途保持しておく。
        // 上のパスが書いた先と必ず一致させるため、どちらも同じアクセサから取る
        RHI::IRHITexture* const activeAOTexture = GetActiveAOTexture();
        RHI::IRHITexture* const activeAORawTexture = GetActiveAORawTexture();

        // --- ライティングパス: G-Bufferを読み、SceneColorへ出力(常に指定した内部解像度) ---
        graph.AddPass(Core::RenderGraphPassDesc{
            .Name = "Lighting",
            .Reads = {
                m_GBufferAlbedo.get(), m_DirectLightTexture.get(), m_GBufferMaterial.get(), m_GBufferDepth.get(),
                skyTexture, activeAOTexture, m_GBufferEmissive.get(), m_GBufferNormal.get(),
                m_IrradianceTexture.get(), m_PrefilteredEnvTexture.get(), m_BRDFLUTTexture.get(),
                // ProbeBakeパスより後に順序付けさせるために挙げる(実際のバインドはExecute内)
                m_ProbeIrradianceArray.get(), m_ProbePrefilteredArray.get(), m_ProbeDistanceArray.get(),
                // 同じくDDGIUpdateパスより後に順序付けさせるために挙げる(22章)
                m_DDGIIrradianceAtlas.get(), m_DDGIDistanceAtlas.get(),
            },
            .RenderTargets = { m_SceneColor.get() },
            // 空パラメータ(P9)。SkyIntegrateパスより後に順序付けさせるために挙げる
            // (実際のバインドはExecute内)
            .BufferReads = { m_SkyParametersBuffer.get() },
            .Execute = [this, &gbufferViewport, activeAOTexture, skyTexture](RHI::IRHICommandList* cmd)
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
                cmd->SetTexture(4, skyTexture);
                cmd->SetTexture(5, activeAOTexture);
                cmd->SetTexture(6, m_GBufferEmissive.get());
                cmd->SetTexture(7, m_GBufferNormal.get());
                cmd->SetTexture(8, m_IrradianceTexture.get());
                cmd->SetTexture(9, m_PrefilteredEnvTexture.get());
                cmd->SetTexture(10, m_BRDFLUTTexture.get());
                // 反射プローブ(19章)。FrameConstants.ProbeParams.xが0のとき(未ベイク・無効時)は
                // シェーダー側が選択ループを回さないため中身は参照されないが、DX12は
                // ディスクリプタテーブルに未初期化のスロットが残ると動作が未定義になるため常にバインドする
                cmd->SetTexture(11, m_ProbeIrradianceArray.get());
                cmd->SetTexture(12, m_ProbePrefilteredArray.get());
                cmd->SetShaderResourceBuffer(13, m_ProbeBuffer.get());
                cmd->SetTexture(14, m_ProbeDistanceArray.get());
                // DDGI(22章)。反射プローブと同じ理由で、無効時も含めて常にバインドする
                cmd->SetTexture(15, m_DDGIIrradianceAtlas.get());
                cmd->SetTexture(16, m_DDGIDistanceAtlas.get());
                // 空パラメータ(P9)。DeferredLighting.hlsl側はt17(t0〜t16が既に使用済み)
                cmd->SetShaderResourceBuffer(17, m_SkyParametersBuffer.get());
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
            // ProbeBakeパスより後に順序付けさせるために挙げる(実際のバインドはExecute内)。
            // 半透明パスもLightingパスと同じ環境ソース(反射プローブ+グローバルIBL)を使うため、
            // 焼き上がる前のプローブを読まないようにする必要がある
            .Reads = { m_ProbeIrradianceArray.get(), m_ProbePrefilteredArray.get(), m_ProbeDistanceArray.get() },
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

                // ライトバッファの中身の更新はグラフ構築前に1回だけ済ませてある
                // (タイルライトカリングパスがこのパスより先に読むため、パス内で更新できない)

                // メッシュによらずパス全体で共通のテクスチャはここで一度だけバインドする。
                // テクスチャのバインドは上書きするまで維持されるため(IRHICommandList::SetTexture参照)、
                // メッシュごとのループ内で張り直す必要はない
                cmd->SetTexture(4, m_ShadowCascadeArray.get());
                cmd->SetShaderResourceBuffer(8, m_LightBuffer.get());
                // IBL(14章)。このパスにはSSRが適用されないため、半透明サーフェスの環境の
                // 映り込みはこの環境ソースだけが担う
                cmd->SetTexture(9, m_IrradianceTexture.get());
                cmd->SetTexture(10, m_PrefilteredEnvTexture.get());
                cmd->SetTexture(11, m_BRDFLUTTexture.get());
                // 反射プローブ(19章)。Lightingパスと同じReflectionProbe.hlsliを共有しており、
                // 半透明サーフェスも室内なら室内の環境が映るようになる。t0〜t4とt8〜t11が
                // 埋まっているため、このパスではt5〜t7を割り当てている(Transparent.hlsl冒頭)。
                // マテリアルの遮蔽マップ(OcclusionTexture)はt5〜t7と衝突するためt13を使う
                // ProbeParams.xが0でも常にバインドするのはLightingパスと同じ理由
                cmd->SetTexture(5, m_ProbePrefilteredArray.get());
                cmd->SetTexture(6, m_ProbeIrradianceArray.get());
                cmd->SetShaderResourceBuffer(7, m_ProbeBuffer.get());
                cmd->SetTexture(12, m_ProbeDistanceArray.get());

                // 半透明は奥から手前への描画順そのものが正しさの前提なので並べ替えられない。
                // そのため必要になった時点でパイプラインを切り替える(GBufferパスと同じ方式)
                RHI::IRHIPipelineState* currentPipelineState = m_TransparentPipelineState.get();
                const auto bindPipelineState = [&](bool mirrored)
                {
                    RHI::IRHIPipelineState* const wanted =
                        mirrored ? m_TransparentPipelineStateMirrored.get() : m_TransparentPipelineState.get();
                    if (wanted == currentPipelineState)
                    {
                        return;
                    }
                    cmd->SetPipelineState(wanted);
                    cmd->SetConstantBuffer(0, m_FrameConstantBuffer.get());
                    cmd->SetSamplerSet(m_MaterialSamplers.get());
                    currentPipelineState = wanted;
                };

                for (const TransparentDraw& draw : draws)
                {
                    bindPipelineState(draw.Instance->IsMirrored);

                    const ObjectConstants objectConstants =
                        MakeObjectConstants(*draw.Instance, *draw.Mesh, m_EmissiveIntensity, m_OcclusionMapEnabled);
                    cmd->UpdateBuffer(m_ObjectConstantBuffer.get(), &objectConstants, sizeof(objectConstants));
                    cmd->SetConstantBuffer(1, m_ObjectConstantBuffer.get());

                    cmd->SetVertexBuffer(draw.Mesh->VertexBuffer.get());
                    cmd->SetIndexBuffer(draw.Mesh->IndexBuffer.get());
                    // メッシュごとに変わるマテリアルテクスチャのみ差し替える
                    // (t4のシャドウとt8以降のライト/IBLはループ前に一度バインドしたものがそのまま残る。
                    // t13だけはマテリアルの遮蔽マップなのでメッシュごとに差し替える)
                    cmd->SetTexture(0, draw.Mesh->BaseColorTexture);
                    cmd->SetTexture(1, draw.Mesh->NormalTexture);
                    cmd->SetTexture(2, draw.Mesh->MetallicRoughnessTexture);
                    cmd->SetTexture(3, draw.Mesh->EmissiveTexture);
                    cmd->SetTexture(13, draw.Mesh->OcclusionTexture);

                    cmd->DrawIndexed(draw.Mesh->IndexCount, 0, 0);
                }
            },
        });

        // --- 平面反射パス(P6): 水面に不透明ジオメトリの鏡像を映すフォワードパス ---
        // 水面が無いシーン・無効化時はパスを登録しない(SSR側のフラグも0になる。下のSSRパス参照)
        if (planarReflectionPassRuns)
        {
            RHI::Viewport planarReflectionViewport;
            planarReflectionViewport.Width = static_cast<float>(m_PlanarReflectionWidth);
            planarReflectionViewport.Height = static_cast<float>(m_PlanarReflectionHeight);

            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "PlanarReflection",
                // ProbeCapture/captureProbeFaceと同じ理由でシャドウ・IBL・DDGIを挙げ、
                // これらを書くパスより後ろへ順序付ける(実際のバインドはExecute内)
                .Reads = {
                    m_ShadowCascadeArray.get(), m_IrradianceTexture.get(), m_PrefilteredEnvTexture.get(),
                    m_BRDFLUTTexture.get(), m_DDGIIrradianceAtlas.get(), m_DDGIDistanceAtlas.get(),
                },
                .RenderTargets = { m_PlanarReflectionColor.get() },
                .DepthTarget = m_PlanarReflectionDepth.get(),
                // 大気遠近(P8)。空パラメータ(m_SkyParametersBuffer)をSkyIntegrateパスの後へ
                // 順序付けさせるために挙げる(実際のバインドはExecute内。SSRパスの同じ宣言と同じ理由)
                .BufferReads = { m_LightBuffer.get(), m_SkyParametersBuffer.get() },
                .Execute = [this, &constants, &planarReflectionViewport, reflectedViewProj, reflectMatrix,
                            waterPlaneY](RHI::IRHICommandList* cmd)
                {
                    // captureProbeFaceとまったく同じ作法(constants.ViewProj/CameraPosition/
                    // PrevViewProj/TAAParams/PlanarReflectionPlaneだけをこのパス用に差し替える)。
                    // Viewはカメラのビュー行列のままにする(PlanarReflection.hlsl冒頭の
                    // 【Viewをカメラのままにする理由】参照。ProbeCaptureとは異なる理由による)
                    FrameConstants reflectionConstants = constants;
                    DirectX::XMStoreFloat4x4(&reflectionConstants.ViewProj, DirectX::XMMatrixTranspose(reflectedViewProj));
                    const DirectX::XMVECTOR reflectedCameraPos =
                        DirectX::XMVector3Transform(DirectX::XMLoadFloat4(&constants.CameraPosition), reflectMatrix);
                    DirectX::XMFLOAT4 reflectedCameraPosFloat;
                    DirectX::XMStoreFloat4(&reflectedCameraPosFloat, reflectedCameraPos);
                    reflectionConstants.CameraPosition = { reflectedCameraPosFloat.x, reflectedCameraPosFloat.y, reflectedCameraPosFloat.z, 0.0f };
                    // TAA関連はカメラ視点のものが入ったままなので明示的に潰す(captureProbeFaceと同じ理由)
                    reflectionConstants.PrevViewProj = reflectionConstants.ViewProj;
                    reflectionConstants.TAAParams = { 0.0f, 0.0f, 0.0f, 0.0f };
                    reflectionConstants.PlanarReflectionPlane = { 0.0f, 1.0f, 0.0f, -waterPlaneY };
                    cmd->UpdateBuffer(m_PlanarReflectionConstantBuffer.get(), &reflectionConstants, sizeof(reflectionConstants));

                    // RenderTargets/DepthTargetはパス宣言(.RenderTargets/.DepthTarget)により
                    // RenderGraphが自動的にバインド済みのため、ここではビューポート設定と
                    // クリアだけでよい(GBuffer/Lightingパスと同じ流儀。captureProbeFaceは
                    // .Writesのみの宣言のため例外的に手動バインドしている)
                    cmd->SetViewport(planarReflectionViewport);
                    cmd->ClearRenderTarget({ 0.0f, 0.0f, 0.0f, 0.0f });
                    // Reverse-Zのため遠平面側(NDC z=0.0)にクリアする
                    cmd->ClearDepth(0.0f);

                    cmd->SetPipelineState(m_PlanarReflectionPipelineState.get());
                    cmd->SetConstantBuffer(0, m_PlanarReflectionConstantBuffer.get());
                    cmd->SetSamplerSet(m_MaterialSamplers.get());

                    // captureProbeFaceと同じ順・同じレジスタでバインドする(PlanarReflection.hlsl参照)
                    cmd->SetTexture(4, m_ShadowCascadeArray.get());
                    cmd->SetShaderResourceBuffer(8, m_LightBuffer.get());
                    cmd->SetTexture(9, m_IrradianceTexture.get());
                    cmd->SetTexture(10, m_PrefilteredEnvTexture.get());
                    cmd->SetTexture(11, m_BRDFLUTTexture.get());
                    cmd->SetTexture(12, m_DDGIIrradianceAtlas.get());
                    cmd->SetTexture(13, m_DDGIDistanceAtlas.get());
                    // 大気遠近(P8)のin-scatter項が読む空パラメータ(PlanarReflection.hlsl参照)
                    cmd->SetShaderResourceBuffer(14, m_SkyParametersBuffer.get());

                    // 鏡映カメラで描くとワインディングが全反転するため、PSOの切り替えは
                    // instance.IsMirroredの否定で行う(このファイル冒頭のPSO生成箇所のコメント参照)
                    RHI::IRHIPipelineState* currentPipelineState = m_PlanarReflectionPipelineState.get();
                    const auto bindPipelineState = [&](bool mirrored)
                    {
                        RHI::IRHIPipelineState* const wanted =
                            mirrored ? m_PlanarReflectionPipelineStateMirrored.get() : m_PlanarReflectionPipelineState.get();
                        if (wanted == currentPipelineState)
                        {
                            return;
                        }
                        cmd->SetPipelineState(wanted);
                        cmd->SetConstantBuffer(0, m_PlanarReflectionConstantBuffer.get());
                        cmd->SetSamplerSet(m_MaterialSamplers.get());
                        currentPipelineState = wanted;
                    };

                    for (const auto& instance : m_Scene.Instances)
                    {
                        for (const auto& mesh : instance.Model.Meshes)
                        {
                            // 半透明メッシュは反射に含めない(ProbeCaptureと同じ割り切り。
                            // PlanarReflection.hlsl冒頭参照)
                            if (mesh.IsTransparent)
                            {
                                continue;
                            }

                            bindPipelineState(!instance.IsMirrored);

                            const ObjectConstants objectConstants =
                                MakeObjectConstants(instance, mesh, m_EmissiveIntensity, m_OcclusionMapEnabled);
                            cmd->UpdateBuffer(m_ObjectConstantBuffer.get(), &objectConstants, sizeof(objectConstants));
                            cmd->SetConstantBuffer(1, m_ObjectConstantBuffer.get());

                            cmd->SetVertexBuffer(mesh.VertexBuffer.get());
                            cmd->SetIndexBuffer(mesh.IndexBuffer.get());
                            cmd->SetTexture(0, mesh.BaseColorTexture);
                            cmd->SetTexture(1, mesh.NormalTexture);
                            cmd->SetTexture(2, mesh.MetallicRoughnessTexture);
                            cmd->SetTexture(3, mesh.EmissiveTexture);
                            cmd->SetTexture(5, mesh.OcclusionTexture);
                            cmd->DrawIndexed(mesh.IndexCount, 0, 0);
                        }
                    }
                },
            });
        }

        // --- 反射パス: Lightingパスが適用した鏡面IBLを、実際に追跡した反射で差し替える(20章)。
        //     ScreenSpaceならSSR(レイマーチ)、RaytracedならRT反射(RayQuery)。
        //     Offならスキップし、後段のTonemapが直接m_SceneColorを読む ---
        if (m_ReflectionMode == ReflectionMode::ScreenSpace)
        {
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "SSR",
                // SSRはLightingパスが適用した鏡面IBLを「差し替える」ため、そのとき使ったものと
                // 同じ環境ソース(プローブ配列・グローバルのプリフィルタ済み鏡面)とBRDF LUT・AOを
                // 読む必要がある(20章)。
                // 手続き空はm_PrefilteredEnvTextureの焼き込み経由で入ってくるため、
                // 空のキューブマップをここで直接バインドする必要はない
                .Reads = {
                    m_SceneColor.get(), m_GBufferNormal.get(), m_GBufferMaterial.get(), m_GBufferDepth.get(),
                    m_GBufferAlbedo.get(), activeAOTexture, m_BRDFLUTTexture.get(), m_PrefilteredEnvTexture.get(),
                    m_ProbePrefilteredArray.get(), m_ProbeDistanceArray.get(),
                    // 平面反射(P6)。パスが登録されなかったフレームでもこのReadsは無害
                    // (今フレームのWriterが無いため単に依存辺が張られないだけ)
                    m_PlanarReflectionColor.get(),
                },
                .RenderTargets = { m_SSRTexture.get() },
                // 空パラメータ(P9)。SkyIntegrateパスより後に順序付けさせるために挙げる
                // (実際のバインドはExecute内)
                .BufferReads = { m_SkyParametersBuffer.get() },
                .Execute = [this, &gbufferViewport, activeAOTexture, usingProceduralSky,
                            planarReflectionPassRuns](RHI::IRHICommandList* cmd)
                {
                    // 水面の解析空フォールバック(P4)。手続き空が無効(.ksceneがDDSスカイボックスを
                    // 明示するシーン)なときは、m_WaterAnalyticSkyReflectionの値に関わらず必ず0にする
                    // ――DDSは任意の絵でPerezモデルとは無関係なため、SSR.hlsl側のSkyColorで
                    // 解析評価してはいけない(usingProceduralSkyはRender()前半で既に確定済み。
                    // DeferredLighting.hlsl向けのconstants.SkyParams.y代入と同じ判断)
                    const float waterAnalyticSkyFlag =
                        (m_WaterAnalyticSkyReflection && usingProceduralSky) ? 1.0f : 0.0f;
                    // 平面反射(P6)。このフレームでPlanarReflectionパスを実際に実行したときだけ
                    // 有効にする(登録されなかったフレームにm_PlanarReflectionColorの中身は
                    // 前フレーム/未定義の残骸なので、フラグをそのままSSR.hlsl側へ渡してはいけない)
                    const float planarReflectionFlag = planarReflectionPassRuns ? 1.0f : 0.0f;

                    SSRConstants ssrConstants{};
                    ssrConstants.Params0 =
                        { m_SSRMaxDistance, m_SSRThickness, m_SSRRoughnessCutoff, waterAnalyticSkyFlag };
                    ssrConstants.Params1 = { planarReflectionFlag, m_PlanarReflectionDistortion, 0.0f, 0.0f };
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
                    cmd->SetTexture(4, m_GBufferAlbedo.get());
                    cmd->SetTexture(5, activeAOTexture);
                    cmd->SetTexture(6, m_BRDFLUTTexture.get());
                    cmd->SetTexture(7, m_PrefilteredEnvTexture.get());
                    cmd->SetTexture(8, m_ProbePrefilteredArray.get());
                    cmd->SetShaderResourceBuffer(9, m_ProbeBuffer.get());
                    cmd->SetTexture(10, m_ProbeDistanceArray.get());
                    // 平面反射(P6)。DX12はディスクリプタテーブルに未初期化のスロットが残ると
                    // 動作が未定義になるため、パスが無効なフレームでも常にバインドする
                    // (反射プローブ・DDGIと同じ理由)
                    cmd->SetTexture(11, m_PlanarReflectionColor.get());
                    // 空パラメータ(P9)。SSR.hlsl側はt12(t0〜t11が既に使用済み)
                    cmd->SetShaderResourceBuffer(12, m_SkyParametersBuffer.get());
                    cmd->Draw(3, 0);
                },
            });
        }
        else if (ShouldRunRaytracedReflection())
        {
            // RT反射パス。読むものはSSRとほぼ同じ(同じ鏡面IBLを差し替えるため)で、
            // これに加えてTLASとシーンジオメトリの統合バッファを読む。
            // レジスタ割り当てはRTReflection.hlsl側の宣言と一致させること
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "RTReflection",
                .Reads = {
                    m_SceneColor.get(), m_GBufferNormal.get(), m_GBufferMaterial.get(), m_GBufferDepth.get(),
                    m_GBufferAlbedo.get(), activeAOTexture, m_BRDFLUTTexture.get(), m_PrefilteredEnvTexture.get(),
                    m_ProbePrefilteredArray.get(),
                },
                .Writes = { m_RTReflectionTexture.get() },
                .Execute = [this, activeAOTexture](RHI::IRHICommandList* cmd)
                {
                    RTReflectionConstants rtConstants{};
                    rtConstants.Params0 = {
                        static_cast<float>(m_RenderWidth), static_cast<float>(m_RenderHeight),
                        m_RTReflectionMaxDistance, m_RTReflectionRoughnessCutoff
                    };
                    rtConstants.Params1 = { m_RTReflectionShadowRayEnabled ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f };
                    cmd->UpdateBuffer(m_RTReflectionConstantBuffer.get(), &rtConstants, sizeof(rtConstants));

                    cmd->SetComputePipelineState(m_RTReflectionPipelineState.get());
                    cmd->SetComputeSamplerSet(m_ScreenSpaceSamplers.get());
                    cmd->SetComputeConstantBuffer(0, m_FrameConstantBuffer.get());
                    cmd->SetComputeConstantBuffer(1, m_RTReflectionConstantBuffer.get());

                    cmd->SetComputeAccelerationStructure(0, m_RaytracingScene.GetTopLevelAS());
                    cmd->SetComputeTexture(1, m_SceneColor.get());
                    cmd->SetComputeTexture(2, m_GBufferNormal.get());
                    cmd->SetComputeTexture(3, m_GBufferMaterial.get());
                    cmd->SetComputeTexture(4, m_GBufferDepth.get());
                    cmd->SetComputeTexture(5, m_GBufferAlbedo.get());
                    cmd->SetComputeTexture(6, activeAOTexture);
                    cmd->SetComputeTexture(7, m_BRDFLUTTexture.get());
                    cmd->SetComputeTexture(8, m_PrefilteredEnvTexture.get());
                    cmd->SetComputeTexture(9, m_ProbePrefilteredArray.get());
                    cmd->SetComputeShaderResourceBuffer(10, m_ProbeBuffer.get());
                    cmd->SetComputeShaderResourceBuffer(11, m_RaytracingScene.GetVertexAttributeBuffer());
                    cmd->SetComputeShaderResourceBuffer(12, m_RaytracingScene.GetIndexBuffer());
                    cmd->SetComputeShaderResourceBuffer(13, m_RaytracingScene.GetMeshInfoBuffer());
                    cmd->SetComputeShaderResourceBuffer(14, m_RaytracingScene.GetInstanceInfoBuffer());
                    cmd->SetComputeShaderResourceBuffer(15, m_RaytracingScene.GetMaterialBuffer());

                    // UAVはDispatch直後に解除されるため毎回バインドし直す(IRHICommandList.h参照)
                    cmd->SetComputeUnorderedAccessTexture(0, m_RTReflectionTexture.get());
                    cmd->Dispatch((m_RenderWidth + 7) / 8, (m_RenderHeight + 7) / 8, 1);
                },
            });
        }

        // --- 大気遠近パス(P8): 反射パス(SSR/RT反射)の後、TAAパスの直前に置く。
        //     Lightingパスの中に入れない理由・TAAより前へ置く理由はShaders/3D/AerialPerspective.hlsl
        //     冒頭のコメント参照。無効時はパス自体を登録せず、reflectionOutputがそのまま
        //     TAA(またはTonemap)への入力になる ---
        RHI::IRHITexture* const reflectionOutput = GetActiveReflectionOutput();
        if (fogPassRuns)
        {
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "AerialPerspective",
                .Reads = { reflectionOutput, m_GBufferDepth.get() },
                .RenderTargets = { m_AerialPerspectiveTexture.get() },
                // 空パラメータ(P9)。SkyIntegrateパスの後へ順序付けさせるために挙げる
                // (実際のバインドはExecute内。SSRパスの同じ宣言と同じ理由)
                .BufferReads = { m_SkyParametersBuffer.get() },
                .Execute = [this, &gbufferViewport, reflectionOutput](RHI::IRHICommandList* cmd)
                {
                    cmd->SetViewport(gbufferViewport);
                    cmd->SetPipelineState(m_AerialPerspectivePipelineState.get());
                    cmd->SetConstantBuffer(0, m_FrameConstantBuffer.get());
                    cmd->SetSamplerSet(m_ScreenSpaceSamplers.get());
                    cmd->SetTexture(0, reflectionOutput);
                    cmd->SetTexture(1, m_GBufferDepth.get());
                    cmd->SetShaderResourceBuffer(2, m_SkyParametersBuffer.get());
                    cmd->Draw(3, 0);
                },
            });
        }

        // --- TAAパス: 前フレームのTAA結果をモーションベクターで再投影し、今フレームの色へ蓄積する。
        //     ジッターで散らしたサンプルがここで平均され、実質的なスーパーサンプリングになる。
        //     トーンマップ前のHDRの段階で行うのは、露出・ブルームがTAAで安定した絵を入力に
        //     できるようにするため(逆順にするとブルームがフレームごとのちらつきを拾う)。
        //     入力はGetActiveReflectionOutput()(反射Off/SSR/RT反射のいずれか、または大気遠近が
        //     有効ならその出力)で、SSRだけを見ていた従来の判定ではRT反射有効時にTAAが古い
        //     SceneColorを拾ってしまうため、ここも合わせて直す ---
        RHI::IRHITexture* const taaInputColor = fogPassRuns ? m_AerialPerspectiveTexture.get() : reflectionOutput;
        if (m_TAAEnabled)
        {
            // 今フレームの書き込み先と、前フレームの結果(履歴)。Render()の末尾で役割が入れ替わる
            const uint32_t historyWriteIndex = m_TAAHistoryIndex;
            const uint32_t historyReadIndex = 1u - historyWriteIndex;
            RHI::IRHITexture* const historyTexture = m_TAAHistory[historyReadIndex].get();

            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "TAA",
                // 履歴(historyTexture)は今フレーム誰も書かないので依存の辺は張られないが、
                // 実際にバインドするテクスチャはReadsにも宣言しておくというRenderGraphの規約に従う
                .Reads = { taaInputColor, historyTexture, m_GBufferVelocity.get(), m_GBufferDepth.get() },
                .RenderTargets = { m_TAAHistory[historyWriteIndex].get() },
                .Execute = [this, &gbufferViewport, taaInputColor, historyTexture, invViewProj, jitterUv,
                            effectiveExposure](RHI::IRHICommandList* cmd)
                {
                    TAAConstants taaConstants{};
                    DirectX::XMStoreFloat4x4(&taaConstants.InvViewProj, DirectX::XMMatrixTranspose(invViewProj));
                    taaConstants.PrevViewProj = m_TAAPrevViewProjValid ? m_TAAPrevViewProj : DirectX::XMFLOAT4X4{};
                    taaConstants.JitterUv = { jitterUv.x, jitterUv.y, m_TAAPrevJitterUv.x, m_TAAPrevJitterUv.y };
                    taaConstants.ScreenParams = {
                        static_cast<float>(m_RenderWidth),
                        static_cast<float>(m_RenderHeight),
                        1.0f / static_cast<float>(m_RenderWidth),
                        1.0f / static_cast<float>(m_RenderHeight),
                    };

                    // 履歴が無効な間は「サンプルすらするな」をシェーダへ伝える(TAA.hlsl参照)。
                    // 作りたてのfp16バッファはNaNを含みうるため、混ぜる割合を0にするだけでは足りない
                    const bool historyValid = m_TAAHistoryValid.load(std::memory_order_relaxed);

                    // プリ露出はm_EffectiveExposureEV100の時間順応で毎フレーム変わる。履歴は前フレームの
                    // 露出で焼かれた明るさのままなので、比率を掛けて今の露出へ揃える。
                    // 揃えないと露出が動いている間ずっと明るさの尾を引く
                    const float previousExposure = ComputeExposure(m_TAAPrevEffectiveExposureEV100);
                    const float exposureRescale =
                        (historyValid && previousExposure > 0.0f) ? (effectiveExposure / previousExposure) : 1.0f;

                    taaConstants.Params0 = {
                        m_TAABlendWeight,
                        m_TAAClipGamma,
                        historyValid ? 1.0f : 0.0f,
                        exposureRescale,
                    };
                    taaConstants.Params1 = {
                        static_cast<float>(m_TAAClipMode), m_TAAAntiFlicker, 0.0f, 0.0f
                    };
                    cmd->UpdateBuffer(m_TAAConstantBuffer.get(), &taaConstants, sizeof(taaConstants));

                    cmd->SetViewport(gbufferViewport);
                    cmd->SetPipelineState(m_TAAPipelineState.get());
                    cmd->SetConstantBuffer(1, m_TAAConstantBuffer.get());
                    cmd->SetSamplerSet(m_ScreenSpaceSamplers.get());
                    // t0〜t3はすべて必ずバインドすること。SRVのバインドは上書きするまで維持されるため、
                    // 省くと直前のパスが張ったテクスチャを読んでしまう(Tonemapのt2で実際に不具合を出した)
                    cmd->SetTexture(0, taaInputColor);
                    cmd->SetTexture(1, historyTexture);
                    cmd->SetTexture(2, m_GBufferVelocity.get());
                    cmd->SetTexture(3, m_GBufferDepth.get());
                    cmd->Draw(3, 0);
                },
            });
        }

        // --- Tonemapパス: HDRのSceneColor(反射パス有効時はその出力、TAA有効時はさらにTAA適用後)を
        //     LDRへ変換する。反射等のHDR演算がすべて完了した後、Present直前の独立したステージとして
        //     常に実行する ---
        // この行はTAAパスのAddPassより後に置くこと。ラムダは値キャプチャなので、先に差し替えると
        // TAAが自分の出力を入力として読む形になる(RenderGraphが循環を検出して例外を投げる)
        RHI::IRHITexture* hdrSceneColor = m_TAAEnabled ? m_TAAHistory[m_TAAHistoryIndex].get() : taaInputColor;

        // --- 自動露出パス: SceneColorの輝度ヒストグラムから目標EV100を求め、時間方向に順応させる。
        //     結果はm_ExposureTextureへ書かれ、後段のTonemapパスが読む(AutoExposure.hlsl参照) ---
        if (m_AutoExposureEnabled)
        {
            // シーン切り替え直後の1回だけ順応を飛ばす。パスを積んだ時点で消費しておくことで、
            // Executeが呼ばれる保証(グラフの枝刈り)に依存せず必ず1回で消える
            const bool resetAdaptation = m_AutoExposureResetRequested;
            m_AutoExposureResetRequested = false;

            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "AutoExposure",
                .Reads = { hdrSceneColor, m_GBufferDepth.get() },
                .Writes = { m_ExposureTexture.get() },
                .Execute = [this, hdrSceneColor, keyReferenceEV100, usingProceduralSky, resetAdaptation](
                    RHI::IRHICommandList* cmd)
                {
                    AutoExposureConstants autoExposureConstants{};
                    autoExposureConstants.InputSize = { m_RenderWidth, m_RenderHeight };
                    // Min>Maxのような不正な範囲だとヒストグラムのビン割りが破綻するため順序を保証する
                    autoExposureConstants.MinEV100 = std::min(m_AutoExposureMinEV100, m_AutoExposureMaxEV100);
                    autoExposureConstants.MaxEV100 = std::max(m_AutoExposureMinEV100, m_AutoExposureMaxEV100);
                    autoExposureConstants.PreExposureEV100 = m_EffectiveExposureEV100;
                    // 一時停止やシーン読み込み直後の巨大なdtで順応が飛ばないよう上限を設ける
                    autoExposureConstants.DeltaTime = std::clamp(m_RenderDeltaTime, 0.0f, 0.1f);
                    autoExposureConstants.AdaptationSpeedUp = m_AutoExposureSpeedUp;
                    autoExposureConstants.AdaptationSpeedDown = m_AutoExposureSpeedDown;
                    autoExposureConstants.LowPercentile = std::min(m_AutoExposureLowPercentile, m_AutoExposureHighPercentile);
                    autoExposureConstants.HighPercentile = std::max(m_AutoExposureLowPercentile, m_AutoExposureHighPercentile);
                    autoExposureConstants.ExposureCompensation = m_AutoExposureCompensation;
                    autoExposureConstants.NightRolloffEV = m_AutoExposureNightRolloffEV;
                    // 折れ点は必ずDark < Brightにする(逆転すると補正が不連続になる)
                    autoExposureConstants.NightRolloffDarkEV100 =
                        std::min(m_AutoExposureNightRolloffDarkEV100, m_AutoExposureNightRolloffBrightEV100);
                    autoExposureConstants.NightRolloffBrightEV100 =
                        std::max(m_AutoExposureNightRolloffDarkEV100, m_AutoExposureNightRolloffBrightEV100);
                    // 構図に依存しないシーンの基準EV。測光値の上限の足がかりになる。
                    //
                    // **手続き空を使っていないシーンではクランプを無効にする**。
                    // 基準EVはこのエンジンの太陽・月・空モデルが出す照度から求めているので、
                    // .ksceneが独自のスカイボックスを指定しているシーン(White Furnace Testなど)
                    // では、そのシーンを実際に照らしている光と無関係な値になってしまう。
                    // 実際、無効化前はWhite Furnace Testの一様グレーが107から208まで持ち上がり、
                    // 白飛びまで余裕が無くなっていた(一様性そのものは保たれていたが、
                    // 飽和させてしまうとエネルギー保存の検証が成立しなくなる)
                    autoExposureConstants.KeyReferenceEV100 = keyReferenceEV100;
                    autoExposureConstants.KeyCeilingEV =
                        usingProceduralSky ? m_AutoExposureKeyCeilingEV : 1.0e4f;
                    autoExposureConstants.ResetAdaptation = resetAdaptation ? 1.0f : 0.0f;
                    cmd->UpdateBuffer(m_AutoExposureConstantBuffer.get(), &autoExposureConstants, sizeof(autoExposureConstants));

                    // 1) ヒストグラムをゼロクリア
                    cmd->SetComputePipelineState(m_AutoExposureClearPipelineState.get());
                    cmd->SetComputeConstantBuffer(1, m_AutoExposureConstantBuffer.get());
                    cmd->SetComputeUnorderedAccessBuffer(0, m_ExposureHistogramBuffer.get());
                    cmd->Dispatch(1, 1, 1);

                    // 2) SceneColorから輝度ヒストグラムを構築
                    //    (UAVはDispatch直後に解除されるため毎回バインドし直す。IRHICommandList.h参照)
                    cmd->SetComputePipelineState(m_AutoExposureHistogramPipelineState.get());
                    cmd->SetComputeConstantBuffer(1, m_AutoExposureConstantBuffer.get());
                    cmd->SetComputeTexture(0, hdrSceneColor);
                    // 空(背景)を測光から外すために深度を読む(AutoExposure.hlsl参照)
                    cmd->SetComputeTexture(1, m_GBufferDepth.get());
                    cmd->SetComputeUnorderedAccessBuffer(0, m_ExposureHistogramBuffer.get());
                    cmd->Dispatch((m_RenderWidth + 15) / 16, (m_RenderHeight + 15) / 16, 1);

                    // 3) 縮約して目標EV100を求め、前フレームの値から指数的に順応させて書き戻す
                    cmd->SetComputePipelineState(m_AutoExposureResolvePipelineState.get());
                    cmd->SetComputeConstantBuffer(1, m_AutoExposureConstantBuffer.get());
                    cmd->SetComputeUnorderedAccessBuffer(0, m_ExposureHistogramBuffer.get());
                    cmd->SetComputeUnorderedAccessTexture(1, m_ExposureTexture.get());
                    cmd->Dispatch(1, 1, 1);
                },
            });
        }

        // --- ブルームパス: SceneColorから半解像度のピラミッドを作り、段階的にダウンサンプル→
        //     3x3テントでアップサンプルしながら加算する。最終段(m_BloomUpTextures[0])をTonemapが読む ---
        if (m_BloomEnabled && !m_BloomDownTextures.empty())
        {
            std::vector<RHI::IRHITexture*> bloomWrites;
            bloomWrites.reserve(m_BloomDownTextures.size() + m_BloomUpTextures.size());
            for (const auto& texture : m_BloomDownTextures)
            {
                bloomWrites.push_back(texture.get());
            }
            for (const auto& texture : m_BloomUpTextures)
            {
                bloomWrites.push_back(texture.get());
            }

            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "Bloom",
                .Reads = { hdrSceneColor, m_ExposureTexture.get() },
                .Writes = std::move(bloomWrites),
                .Execute = [this, hdrSceneColor, manualExposureScale](RHI::IRHICommandList* cmd)
                {
                    const uint32_t levelCount = static_cast<uint32_t>(m_BloomDownTextures.size());

                    BloomConstants bloomConstants{};
                    bloomConstants.Threshold = m_BloomThreshold;
                    bloomConstants.SoftKnee = m_BloomSoftKnee;
                    // しきい値を「表示上の白」基準の直感的な値のままにするため、
                    // ピラミッドの入力段で露出を反映する(Bloom.hlsl ExposureScale()参照)。
                    // Tonemapと同じ倍率でなければ、ブルームだけ露出がずれて合成比が狂う
                    bloomConstants.UseAutoExposure = m_AutoExposureEnabled ? 1.0f : 0.0f;
                    bloomConstants.PreExposureEV100 = m_EffectiveExposureEV100;
                    bloomConstants.ExposureScale = manualExposureScale;

                    // --- ダウンサンプル: SceneColor -> down[0] -> down[1] -> ... ---
                    cmd->SetComputePipelineState(m_BloomDownsamplePipelineState.get());
                    cmd->SetComputeSamplerSet(m_ScreenSpaceSamplers.get());
                    for (uint32_t level = 0; level < levelCount; ++level)
                    {
                        const bool isFirst = (level == 0);
                        RHI::IRHITexture* source = isFirst ? hdrSceneColor : m_BloomDownTextures[level - 1].get();
                        const DirectX::XMUINT2 srcSize = isFirst
                            ? DirectX::XMUINT2{ m_RenderWidth, m_RenderHeight }
                            : m_BloomLevelSizes[level - 1];
                        const DirectX::XMUINT2 dstSize = m_BloomLevelSizes[level];

                        bloomConstants.SrcSize = srcSize;
                        bloomConstants.DstSize = dstSize;
                        // 最初のダウンサンプルだけKaris平均としきい値を適用する(理由はBloom.hlsl冒頭)
                        bloomConstants.ApplyKarisAndThreshold = isFirst ? 1.0f : 0.0f;
                        cmd->UpdateBuffer(m_BloomConstantBuffer.get(), &bloomConstants, sizeof(bloomConstants));

                        cmd->SetComputeConstantBuffer(1, m_BloomConstantBuffer.get());
                        cmd->SetComputeTexture(0, source);
                        cmd->SetComputeTexture(2, m_ExposureTexture.get());
                        // UAVはDispatch直後に解除されるため毎回バインドし直す(IRHICommandList.h参照)
                        cmd->SetComputeUnorderedAccessTexture(0, m_BloomDownTextures[level].get());
                        cmd->Dispatch((dstSize.x + 7) / 8, (dstSize.y + 7) / 8, 1);
                    }

                    // --- アップサンプル: 最下段から上へ、down[level] + tent(1段下) を up[level] へ書く ---
                    cmd->SetComputePipelineState(m_BloomUpsamplePipelineState.get());
                    cmd->SetComputeSamplerSet(m_ScreenSpaceSamplers.get());
                    for (int32_t level = static_cast<int32_t>(levelCount) - 2; level >= 0; --level)
                    {
                        // 最下段の1つ上だけは、まだup[]が書かれていないのでdown[]の最下段を読む
                        const bool readsDownChain = (level == static_cast<int32_t>(levelCount) - 2);
                        RHI::IRHITexture* lower = readsDownChain
                            ? m_BloomDownTextures[level + 1].get()
                            : m_BloomUpTextures[level + 1].get();

                        const DirectX::XMUINT2 srcSize = m_BloomLevelSizes[level + 1];
                        const DirectX::XMUINT2 dstSize = m_BloomLevelSizes[level];

                        bloomConstants.SrcSize = srcSize;
                        bloomConstants.DstSize = dstSize;
                        bloomConstants.ApplyKarisAndThreshold = 0.0f;
                        cmd->UpdateBuffer(m_BloomConstantBuffer.get(), &bloomConstants, sizeof(bloomConstants));

                        cmd->SetComputeConstantBuffer(1, m_BloomConstantBuffer.get());
                        cmd->SetComputeTexture(0, m_BloomDownTextures[level].get());
                        cmd->SetComputeTexture(1, lower);
                        cmd->SetComputeUnorderedAccessTexture(0, m_BloomUpTextures[level].get());
                        cmd->Dispatch((dstSize.x + 7) / 8, (dstSize.y + 7) / 8, 1);
                    }
                },
            });
        }

        // Tonemapがブルームとして読むテクスチャ。無効時も有効なテクスチャを常にt2へバインドする
        // 必要があるため、その場合はピラミッド最上段(内容は前フレームのまま)を渡し、
        // BloomStrength=0で寄与しないようにする
        RHI::IRHITexture* bloomResultTexture =
            m_BloomUpTextures.empty() ? hdrSceneColor : m_BloomUpTextures[0].get();

        graph.AddPass(Core::RenderGraphPassDesc{
            .Name = "Tonemap",
            .Reads = { hdrSceneColor, m_ExposureTexture.get(), bloomResultTexture },
            .RenderTargets = { m_TonemapTexture.get() },
            .Execute = [this, &gbufferViewport, hdrSceneColor, bloomResultTexture, manualExposureScale,
                        keyReferenceEV100](RHI::IRHICommandList* cmd)
            {
                TonemapConstants tonemapConstants{};
                tonemapConstants.Curve = static_cast<int32_t>(m_TonemapCurve);
                // 手動露出時: プリ露出は時刻連動で変動するので、設定EV100との差分を割り戻して
                // 「設定EV100で固定した絵」へ戻す(manualExposureScaleの算出箇所のコメント参照)
                tonemapConstants.ExposureScale = manualExposureScale;
                tonemapConstants.DitherStrength = m_DitherEnabled ? 1.0f : 0.0f;
                tonemapConstants.UseAutoExposure = m_AutoExposureEnabled ? 1.0f : 0.0f;
                tonemapConstants.PreExposureEV100 = m_EffectiveExposureEV100;
                tonemapConstants.BloomStrength =
                    (m_BloomEnabled && !m_BloomUpTextures.empty()) ? m_BloomStrength : 0.0f;
                tonemapConstants.MesopicStrength = m_MesopicStrength;
                // 目の順応は画面の構図ではなくシーンの明るさで決まるので、
                // 自動露出の測光値ではなくキー照度から求めた基準EVを使う
                tonemapConstants.MesopicAdaptationEV100 = keyReferenceEV100;
                // シャープネスはTAAの蓄積で失われた高域を戻すためのものなので、TAAが無効なら0。
                // そうしないとTAA導入前の絵と変わってしまう
                tonemapConstants.Sharpness = m_TAAEnabled ? m_TAASharpness : 0.0f;
                tonemapConstants.InvRenderWidth = 1.0f / static_cast<float>(m_RenderWidth);
                tonemapConstants.InvRenderHeight = 1.0f / static_cast<float>(m_RenderHeight);
                cmd->UpdateBuffer(m_TonemapConstantBuffer.get(), &tonemapConstants, sizeof(tonemapConstants));

                cmd->SetViewport(gbufferViewport);
                cmd->SetPipelineState(m_TonemapPipelineState.get());
                cmd->SetConstantBuffer(1, m_TonemapConstantBuffer.get());
                cmd->SetSamplerSet(m_ScreenSpaceSamplers.get());
                cmd->SetTexture(0, hdrSceneColor);
                cmd->SetTexture(1, m_ExposureTexture.get());
                // t2は必ずバインドすること。SRVのバインドは上書きするまで維持されるため、
                // ここを省くと直前のパスが張ったテクスチャをブルームとして読んでしまう
                // (実際にG-Bufferのバッファを読んで画面全体が緑に転ぶ不具合を出した)
                cmd->SetTexture(2, bloomResultTexture);
                cmd->Draw(3, 0);
            },
        });

        // --- Presentパス: 選択中のレンダーターゲットを、アスペクト比を保ってバックバッファへ出力 ---
        // デバッグ表示(Render Targets UI)で選択されたバッファに応じて表示ソースを切り替える。
        // 深度バッファ(GBuffer深度・シャドウマップ)はPresent.hlsl側でグレースケール化するためMode=1を渡す
        RHI::IRHITexture* presentSourceTexture = m_TonemapTexture.get();
        // Mode 9(IBL Irradiance/Prefilterのキューブマップ表示)専用。他のModeでは使われないが、
        // t1には常に何らかの有効なTextureCubeをバインドしておく必要があるため既定値を持たせる
        RHI::IRHITexture* presentDebugCubeTexture = skyTexture;
        // Mode 10(シャドウマップのカスケード表示)専用。t1と同じ理由で、t2にも常に有効な
        // Texture2DArrayをバインドしておく必要があるためシャドウマップ配列自身を既定値にする
        RHI::IRHITexture* presentDebugArrayTexture = m_ShadowCascadeArray.get();
        // Mode 12(反射プローブのキューブマップ配列)専用。TextureCube(t1)ともTexture2DArray(t2)とも
        // 型が違うためさらに別スロット(t4)が要る。こちらも常に有効なテクスチャをバインドしておく
        RHI::IRHITexture* presentDebugCubeArrayTexture = m_ProbeIrradianceArray.get();
        // Mode 18(雲の3Dノイズ、P13a)専用。Texture3Dはここまでのどの型とも別なのでさらに
        // 別スロット(t5)が要る。他と同じく常に有効なテクスチャをバインドしておく
        RHI::IRHITexture* presentDebugVolumeTexture = m_CloudShapeNoiseTexture.get();
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
            // Texture2DArrayはSourceTexture(t0、Texture2D)へバインドできないため、専用の
            // DebugArrayTexture(t2)を表示スライス指定付きでサンプルする(IBLキューブマップの
            // Mode 9と同じ方式。Present.hlsl参照)
            presentDebugArrayTexture = m_ShadowCascadeArray.get();
            presentMode = 10;
            presentSourceWidth = kShadowMapSize;
            presentSourceHeight = kShadowMapSize;
            break;
        case DebugView::RTShadow:
            // 可視率(0〜1のスカラー)をそのままグレースケール表示する。RTシャドウを実行していない
            // フレーム(非対応環境・手法がRaytraced以外)はテクスチャの中身が意味を持たないため、
            // 最終結果のまま何も切り替えない
            if (ShouldRunRaytracedShadow())
            {
                presentSourceTexture = m_RTShadowTexture.get();
                presentMode = 5;
            }
            break;
        case DebugView::SSR:
            // 反射がOffのときは反射パスをスキップしているため、Tonemapパスの入力もSceneColorになり
            // 結果的にFinalと同一表示になる(SSR / RT反射のどちらでも同じ扱い)
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
            // キューブマップ配列のためMode 9(TextureCube)ではなくMode 12(TextureCubeArray)を使う
            presentDebugCubeArrayTexture = m_ProbeIrradianceArray.get();
            presentMode = 12;
            break;
        case DebugView::ProbePrefilter:
            presentDebugCubeArrayTexture = m_ProbePrefilteredArray.get();
            presentMode = 12;
            break;
        case DebugView::ProbeInfluence:
            // 塗り分けはDeferredLighting.hlsl側(FrameConstants.ProbeParams.y)で行うため、
            // Presentは通常どおり最終結果を表示するだけでよい
            presentSourceTexture = m_TonemapTexture.get();
            break;
        case DebugView::ProbeDistance:
            // 距離キューブ(19.12節)。格納値はワールド距離なので専用のMode 13でGain倍して
            // グレースケール表示する(Mode 12でそのまま出すと数メートルで白飛びする)
            presentDebugCubeArrayTexture = m_ProbeDistanceArray.get();
            presentMode = 13;
            break;
        case DebugView::IBLBRDFLUT:
            presentSourceTexture = m_BRDFLUTTexture.get();
            presentMode = 0; // (A, B, Eavg)の生値をそのままRGBとして表示(値域はおおむね[0,1])
            presentSourceWidth = kIBLBRDFLUTSize;
            presentSourceHeight = kIBLBRDFLUTSize;
            break;
        case DebugView::Bloom:
            // ピラミッド最上段(半解像度、HDR)。Mode 4でトーンマッピングしてから表示する
            if (!m_BloomUpTextures.empty())
            {
                presentSourceTexture = m_BloomUpTextures[0].get();
                presentMode = 4;
                presentSourceWidth = m_BloomLevelSizes[0].x;
                presentSourceHeight = m_BloomLevelSizes[0].y;
            }
            break;
        case DebugView::LightTiles:
            // ライトグリッドは構造化バッファなのでSourceTexture(t0)では受け取れず、専用のt3から読む
            // (Present.hlsl Mode 11)。t0には何かをバインドしておく必要があるため、
            // 解像度だけ合わせてm_TonemapTextureをそのまま渡す(Mode 11では読まれない)
            presentSourceTexture = m_TonemapTexture.get();
            presentMode = 11;
            break;
        case DebugView::MotionVector:
            // 速度バッファ。格納値はUV単位(1画素ぶんの移動で1/解像度、1920幅なら約0.0005)と
            // 極端に小さく、そのまま色として出しても真っ黒にしか見えない。専用のMode 14で
            // ピクセル単位へ換算してから中間灰色を原点に色付けする
            presentSourceTexture = m_GBufferVelocity.get();
            presentMode = 14;
            break;
        case DebugView::SceneColorRaw:
            // トーンマップもガンマも通さないリニア値をそのまま出す。スペキュラのエネルギー補正の
            // 各方式を数値で突き合わせるための測定用(14.9.9節)。
            // バックバッファが8bit UNormのため1.0を超える値はクリップする ―― 測定時は
            // EV100を上げてピークが1.0未満に収まるようにしてから読むこと。
            // TAAが有効な場合、hdrSceneColorはTAAの蓄積結果(23章)を指す。静止して収束させれば
            // ジッターの平均が取れたぶん単フレームより安定した値が読めるが、カメラを動かした
            // 直後の数フレームは履歴が混ざっているため、値を読むのは静止させてから
            presentSourceTexture = hdrSceneColor;
            presentMode = 0;
            break;
        case DebugView::DDGIIrradiance:
        case DebugView::DDGIDistance:
        {
            // アトラスはただのTexture2Dなのでt0でそのまま受け取れる(22章)。
            // 反射プローブのキューブと違い専用スロットは要らない。
            // アトラスは横長(列=Cx*Cy、行=Cz)なので、レターボックスがその比率に合うよう
            // 実寸を渡す。渡さないと画面いっぱいへ引き伸ばされ、セルが正方形に見えなくなる
            const bool isIrradiance = (m_DebugView == DebugView::DDGIIrradiance);
            const uint32_t cell = isIrradiance ? kDDGIIrradianceCell : kDDGIDistanceCell;
            const uint32_t columns = m_GIVolume.ProbeCounts[0] * m_GIVolume.ProbeCounts[1];
            const uint32_t rows = m_GIVolume.ProbeCounts[2];

            presentSourceTexture = isIrradiance ? m_DDGIIrradianceAtlas.get() : m_DDGIDistanceAtlas.get();
            // Present.hlslのMode 14はモーションベクター(TAA、23章)が既に使っているため、
            // DDGIのイラディアンス/距離モーメントはMode 15/16にずらしてある
            presentMode = isIrradiance ? 15 : 16;
            presentSourceWidth = m_HasGIVolume ? columns * cell : cell;
            presentSourceHeight = m_HasGIVolume ? rows * cell : cell;
            break;
        }
        case DebugView::WaterMask:
            // G-BufferのMaterial.a(水面のマテリアルID)をそのままグレースケール表示する(P2)。
            // 0/1の二値なのでMode 3(Gain倍する遮蔽率表示)ではなく専用のMode 17を使う
            presentSourceTexture = m_GBufferMaterial.get();
            presentMode = 17;
            break;
        case DebugView::PlanarReflection:
            // 平面反射(P6)パスの出力。パスが今フレーム実行されていない(無効化・水面なし)場合、
            // m_PlanarReflectionColorの中身は前フレーム/未定義の残骸なので最終結果のまま何も
            // 切り替えない(RTShadowデバッグ表示と同じ方針)
            if (planarReflectionPassRuns)
            {
                // HDRのためMode 4でReinhardトーンマッピング+ガンマ補正して表示する
                // (DirectLight/Bloomと同じ扱い)。専用のMode追加は不要でPresent.hlslは無変更のまま使える
                presentSourceTexture = m_PlanarReflectionColor.get();
                presentMode = 4;
                presentSourceWidth = m_PlanarReflectionWidth;
                presentSourceHeight = m_PlanarReflectionHeight;
            }
            break;
        case DebugView::CloudNoiseSlice:
        {
            // 雲の3Dノイズ(P13a)。形状(128^3)とディテール(32^3)を切り替えて任意のスライスを見る。
            // 正方形のテクスチャなので表示も正方形にする(レターボックスの計算に渡す)
            const bool showDetail = m_CloudNoiseDebugShowDetail;
            presentDebugVolumeTexture =
                showDetail ? m_CloudDetailNoiseTexture.get() : m_CloudShapeNoiseTexture.get();
            presentMode = 18;
            const uint32_t size = showDetail ? kCloudDetailNoiseSize : kCloudShapeNoiseSize;
            presentSourceWidth = size;
            presentSourceHeight = size;
            break;
        }
        }

        PresentConstants presentConstants{};
        presentConstants.Mode = presentMode;
        presentConstants.TileParams =
        {
            static_cast<float>(m_LightTileCountX),
            static_cast<float>(kLightTileSize),
            static_cast<float>(kLightTileCapacity),
            // ヒートマップで赤に振り切る基準のライト数。容量そのものを基準にすると
            // 実データ(数灯)ではほぼ真っ青で差が読めないため、別のつまみにしてある
            static_cast<float>(std::max(1, m_LightTileHeatmapMax)),
        };
        presentConstants.TileRenderSize =
        {
            static_cast<float>(m_RenderWidth),
            static_cast<float>(m_RenderHeight),
            0.0f,
            0.0f,
        };
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
        // ArraySliceはMode 10ではカスケード番号、Mode 12ではプローブ番号として使う。
        // プローブが1つも無い場合でも配列の範囲外を引かないようクランプする
        if (m_DebugView == DebugView::ProbeIrradiance || m_DebugView == DebugView::ProbePrefilter ||
            m_DebugView == DebugView::ProbeDistance)
        {
            presentConstants.ArraySlice = static_cast<float>(
                std::clamp(m_ProbeDebugIndex, 0, std::max(0, static_cast<int32_t>(m_ReflectionProbes.size()) - 1)));
        }
        else if (m_DebugView == DebugView::CloudNoiseSlice)
        {
            // Mode 18ではW座標(0〜1)として使う。3Dテクスチャなので配列番号ではなく連続値
            presentConstants.ArraySlice = std::clamp(m_CloudNoiseDebugSlice, 0.0f, 1.0f);
        }
        else
        {
            presentConstants.ArraySlice =
                static_cast<float>(std::clamp(m_ShadowDebugCascade, 0, static_cast<int32_t>(kCascadeCount) - 1));
        }
        // Finalの見た目は倍率の影響を受けてはならないため、デバッグ表示のときだけ倍率を掛ける
        // (Gainはゼロ初期化のままだと0倍=真っ黒になるので、必ず明示的に設定すること)
        if (m_DebugView == DebugView::ProbeDistance || m_DebugView == DebugView::DDGIDistance)
        {
            // 距離は色ではなくワールド距離なので、Debug View Gain(1倍以上)ではなく
            // 「白になる距離」の逆数を渡す。Present.hlsl Mode 13/15の式は他と同じ「値×Gain」のまま。
            // DDGI側は距離がMaxRayDistanceでクランプされているので、そこを白にすると
            // 「クランプに当たっている方向」が一目で分かる
            const float whiteAt = (m_DebugView == DebugView::DDGIDistance)
                ? m_GIVolume.MaxRayDistance
                : m_ProbeDistanceDebugRange;
            presentConstants.Gain = 1.0f / std::max(whiteAt, 0.01f);
        }
        else if (m_DebugView == DebugView::DDGIIrradiance)
        {
            // アトラスは露出非依存の物理量で持っている(FrameConstants::DDGIParams4 参照)ため、
            // そのまま出すと昼は数万倍の値になって白飛びする。表示だけ実効プリ露出を掛けて
            // 他のバッファと同じ表示レンジへ揃える。こうしておくと
            // 「IBL - イラディアンス」の表示と直接見比べられる(22.9.1節の検証がこれに依存している)
            presentConstants.Gain = m_DebugViewGain * effectiveExposure;
        }
        else
        {
            presentConstants.Gain = (m_DebugView == DebugView::Final) ? 1.0f : m_DebugViewGain;
        }
        commandList->UpdateBuffer(m_PresentConstantBuffer.get(), &presentConstants, sizeof(presentConstants));

        // レターボックス/ピラーボックスの余白もクリア色のまま残るよう、絞ったビューポートで描画する
        const RHI::Viewport letterboxViewport = ComputeLetterboxViewport(
            m_Window->GetWidth(), m_Window->GetHeight(), presentSourceWidth, presentSourceHeight);

        graph.AddPass(Core::RenderGraphPassDesc{
            .Name = "Present",
            .Reads = { presentSourceTexture, presentDebugCubeTexture, presentDebugArrayTexture,
                       presentDebugCubeArrayTexture, presentDebugVolumeTexture },
            // DebugView::LightTilesでライトグリッドを読むため、カリングパスより後に順序付ける
            .BufferReads = { m_LightTileBuffer.get() },
            .SwapChainTarget = m_SwapChain.get(),
            .Execute = [this, &letterboxViewport, presentSourceTexture, presentDebugCubeTexture,
                        presentDebugArrayTexture, presentDebugCubeArrayTexture,
                        presentDebugVolumeTexture](RHI::IRHICommandList* cmd)
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
                cmd->SetTexture(2, presentDebugArrayTexture);
                // Mode 11(ライトグリッドのヒートマップ)以外でも、シェーダが宣言しているリソースは
                // 必ずバインドする(SetPipelineStateが毎回ルート引数を無効化するため)
                cmd->SetShaderResourceBuffer(3, m_LightTileBuffer.get());
                cmd->SetTexture(4, presentDebugCubeArrayTexture);
                cmd->SetTexture(5, presentDebugVolumeTexture);
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

        // --- 次フレームがこのフレームを「前フレーム」として参照するための状態を確定させる ---
        // 早期returnより後のここで行うことで、描画を行わなかったフレームでは前フレームの状態が
        // そのまま保たれ、履歴テクスチャの中身と行列の対応が1フレームずれない
        m_TAAPrevViewProj = constants.ViewProj;
        m_TAAPrevJitterUv = jitterUv;
        m_TAAPrevViewProjValid = true;
        m_TAAPrevEffectiveExposureEV100 = m_EffectiveExposureEV100;
        if (m_TAAEnabled)
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
