// 大気遠近(height fog / aerial perspective)パス。
//
// 反射パス(SSR/RT反射)の後、TAAパスの直前に置くフルスクリーン三角形+ピクセルシェーダー。
// Lightingパスの中に入れなかったのは次の2点が実コードの制約として存在するため:
//   1. SSR(Shaders/3D/SSR.hlsl)はm_SceneColorを「反射先の環境色」としてそのまま読む(t0)。
//      Lightingの中でフォグを掛けてしまうとSSRがフォグ済みの色を反射に使い、
//      画面上でフォグが二重に(直接見えている分+反射に映った分)乗ってしまう
//   2. 半透明パス(Transparent.hlsl)はLightingパスの後にm_SceneColorへ直接描き足す。
//      Lighting内でフォグを掛けるとその後に描かれる半透明サーフェスだけフォグを免れてしまう
// TAAより前に置くのは、フォグが深度から決まる純関数で時間方向に揺れないため
// (TAA自身が時間方向のノイズを均す側に回れる。逆にTAAの後ろへ置くと、フォグが作る
// 勾配の強い縁がジッターで解決されないままTAAをすり抜けてエイリアシングを残す)。
//
// 頂点シェーダー(フルスクリーン三角形)とReconstructWorldPosはSSR.hlslのものと完全に同一の内容を
// 複製している(SSR.hlslはcbuffer/テクスチャの宣言と一体になっており、この2つだけを
// 抜き出してヘッダー化すると余計な結合が増えるため、複製のほうを選んだ)。
#include "Samplers.hlsli"
// 大気遠近の透過率(cbufferに依存しない純粋関数)。PlanarReflection.hlslと共有する
#include "HeightFog.hlsli"
// 空モデル(Perez分布)の共有ヘッダー。in-scatter項(フォグの合成先の色)に、背景と同じ
// SkyColorをそのまま使うことで、遠方の地物が無限遠で背景の空色へ厳密に収束するようにする
// (詳細はSky.hlsli冒頭のコメント、および本ファイルPSMain末尾のコメント参照)
// SkyView LUT。日中の空はこのLUTを引く。**定義しないと日中の空が黒くなる**ので、
// SkyColorUpperUnitを呼ぶシェーダーは全員定義すること(Sky.hlsliのSkyViewセクション参照)
#define KURENAI_SKYVIEW_REGISTER t3
#include "Sky.hlsli"

cbuffer FrameConstants : register(b0)
{
    float4x4 ViewProj;
    float4x4 InvViewProj;
    // カスケードシャドウマップ用(このシェーダでは未使用。オフセット合わせのためだけに宣言する)
    float4x4 CascadeViewProj[4];
    float4 CameraPosition;
    float4 LightDirection;
    float4 LightColor;
    float4x4 View;
    float4x4 Proj;
    // このシェーダでは未使用(オフセット合わせのためだけに宣言する)
    float4 AmbientColor;
    // このシェーダでは未使用(オフセット合わせのためだけに宣言する)
    float4 CascadeSplits;
    // このシェーダでは未使用(オフセット合わせのためだけに宣言する)
    float4 ShadowParams;
    // このシェーダでは未使用(オフセット合わせのためだけに宣言する)
    float4 ActiveLightCount;
    // このシェーダでは未使用(オフセット合わせのためだけに宣言する)
    float4 IBLParams;
    // このシェーダでは未使用(オフセット合わせのためだけに宣言する)
    float4 ProbeParams;
    // このシェーダでは未使用(オフセット合わせのためだけに宣言する)
    float4 ProbeParams2;
    // ここから下、TAA(23章)・DDGI(22章)・水面の波用の8本はこのシェーダでは未使用。
    // cbufferのレイアウトは宣言順で決まり途中のフィールドを飛ばせないため、末尾のSky*・Fog*
    // フィールドのオフセットをC++側 KurenaiEngine3D.cpp の FrameConstants と合わせる
    // 目的だけで宣言している(SSR.hlsl/DeferredLighting.hlslの同名フィールドと同じ扱い)
    float4x4 PrevViewProj;
    float4 TAAParams;
    float4 DDGIParams0;
    float4 DDGIParams1;
    float4 DDGIParams2;
    float4 DDGIParams3;
    float4 DDGIParams4;
    // DDGIのクリップマップLOD(31.4.2節)。**要素数はC++側のkDDGIMaxLODCountと一致させること。**
    // 読むのはDDGI.hlsliだけだが、cbufferは宣言順でオフセットが決まるため、
    // DDGIParams4の後ろのフィールドを読むシェーダーはすべてここへ同じ宣言が要る
    // (飛ばすと以降のフィールドが64バイトずれ、コンパイルは通るのに別の値を読む)
    float4 DDGILODOrigin[4];
    float4 DDGILODBase[4];
    // bent normalによる遮蔽(34章)。このシェーダーでは読まないが、C++側のFrameConstantsでは
    // DDGIParams4の直後にあるため、**宣言しないと以降のフィールドが16バイトずれる**
    float4 OcclusionParams;
    float4 TimeParams;
    // 空の解析評価用。MakeSkyParametersが読む。xyz=太陽が「ある」向き
    // (未正規化のまま渡ってくる。呼び出し側でnormalizeする)、w=未使用
    float4 SkySunDirection;
    // x=未使用、y=このシェーダでは未使用(背景の解析評価トグルはDeferredLighting.hlsl専用)、
    // z=太陽照度/空照度比(SunToSkyIlluminanceRatio。MakeSkyParametersが読み、
    // Sky.hlsliのEvaluateCloudLayerが雲の明るさを太陽照度基準にするために使う)、w=未使用
    float4 SkyParams;
    // 雲(さらに末尾に追加)。DeferredLighting.hlsl/SSR.hlsl/PlanarReflection.hlslの同名フィールドと
    // 完全に同じ順・同じ型であること(C++側 KurenaiEngine3D.cpp の FrameConstants::CloudParams0/1 と
    // 揃える。ずれるとフォグのin-scatterに使う空の色が背景・水面反射と食い違う)。
    // CloudParams0: x=被覆率(0で雲なし。Sky.hlsliのSkyColorが早期脱出する)、
    //               y=雲底の高度[m](カメラ基準)、z=UVスケール[ノイズ空間/m]、w=消散係数
    float4 CloudParams0;
    // CloudParams1: xy=風によるノイズ空間の移動量(CPU側でSky.hlsliのkCloudNoisePeriodと
    //               同じ周期でwrap済み)、z=Henyey-Greensteinの非対称パラメータ、w=未使用
    float4 CloudParams1;
    // 巻雲(さらに末尾に追加)。他シェーダーの同名フィールドと完全に同じ順・同じ型であること
    // (C++側 KurenaiEngine3D.cpp の FrameConstants::CloudParams2/3 と揃える)。
    // CloudParams2: x=巻雲の被覆率(0で巻雲なし)、y=雲底の高度[m](カメラ基準)、
    //               z=UVスケール[ノイズ空間/m]、w=消散係数
    float4 CloudParams2;
    // CloudParams3: xy=風によるノイズ空間の移動量(積雲と同じくkCloudNoisePeriodでwrap済み)、
    //               z=fBmのUV(U方向)を伸ばす異方性スケール、w=未使用
    float4 CloudParams3;
    // 平面反射。このシェーダでは未使用(オフセット合わせのためだけに宣言する)
    float4 PlanarReflectionPlane;
    // 大気遠近(末尾に追加)。x=基準高度での消散係数[1/m]、y=スケールハイト[m]、
    // z=基準高度[m](ワールドY)、w=有効フラグ(0で無効。UIでオフ、またはシーンが手続き空を
    // 使っていない場合に0になる。手続き空が無効なシーンでは下のSkyColorによる解析評価が
    // 意味を持たないため、SSR.hlslのwaterAnalyticSkyFlagと同じ判断をC++側で行う)
    float4 FogParams0;
    // x=不透明度の上限(1.0で完全に空の色まで行く)、yzw=未使用
    float4 FogParams1;
    // このシェーダでは未使用(オフセット合わせのためだけに宣言する)。Water.hlslのPSMainが読む
    float4 WaterBodyColor;
};

// t0=入力のSceneColor(反射パスの出力。GetActiveReflectionOutput())、t1=深度、
// t2=SkyIntegrate.hlslが書いた空パラメータ(SSR.hlsl等と同じStructuredBuffer)
Texture2D SceneColorTexture : register(t0);
Texture2D DepthTexture : register(t1);
StructuredBuffer<GPUSkyParameters> SkyParametersBuffer : register(t2);

struct PSInput
{
    float4 Position : SV_POSITION;
    float2 UV : TEXCOORD0;
};

// 頂点バッファなしで画面全体を覆う三角形を1枚だけ生成する定番のテクニック(SSR.hlslのVSMainと同一)
PSInput VSMain(uint vertexID : SV_VertexID)
{
    PSInput output;
    output.UV = float2((vertexID << 1) & 2, vertexID & 2);
    output.Position = float4(output.UV.x * 2.0f - 1.0f, 1.0f - output.UV.y * 2.0f, 0.0f, 1.0f);
    return output;
}

// SSR.hlslのReconstructWorldPosと同一の内容
float3 ReconstructWorldPos(float2 uv, float depth)
{
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 clipPos = float4(ndc, depth, 1.0f);
    float4 worldPos = mul(clipPos, InvViewProj);
    return worldPos.xyz / worldPos.w;
}

// FrameConstantsのSky*フィールドからSky.hlsliのSkyParametersを組み立てる。
// SSR.hlsl/DeferredLighting.hlsl/PlanarReflection.hlslのMakeSkyParametersと完全に同一の内容
// (正規化の扱いを含む)。4つのシェーダーはcbufferをそれぞれ別に宣言しているため関数そのものは
// 共有できず複製しているが、中身がずれると「背景の空」「水面に映る空」「フォグの合成先の色」が
// 互いに食い違ってしまうため、中身を変える場合は必ず4つとも同時に直すこと
SkyParameters MakeSkyParameters(float2 pixelPosition)
{
    SkyParameters params;
    params.SunDirection = normalize(SkySunDirection.xyz);
    params = ApplySkyParametersFromBuffer(params, SkyParametersBuffer[0]);
    // 太陽照度/空照度比(SkyParams.zに詰めてある。KurenaiEngine3D.cppのSkyParams.zコメント参照)。
    // EvaluateCloudLayerが雲の明るさを太陽照度基準にするために使う
    params.SunToSkyIlluminanceRatio = SkyParams.z;
    params.CloudCoverage = CloudParams0.x;
    params.CloudAltitude = CloudParams0.y;
    params.CloudUvScale = CloudParams0.z;
    params.CloudDensity = CloudParams0.w;
    params.CloudScrollOffset = CloudParams1.xy;
    params.CloudForwardG = CloudParams1.z;
    // 積雲の厚み[m](CloudParams1.wの枠に詰めてある)。
    // 0ならレイマーチせず平面として扱う
    params.CloudThickness = CloudParams1.w;
    params.CirrusCoverage = CloudParams2.x;
    params.CirrusAltitude = CloudParams2.y;
    params.CirrusUvScale = CloudParams2.z;
    params.CirrusDensity = CloudParams2.w;
    params.CirrusScrollOffset = CloudParams3.xy;
    params.CirrusAnisotropy = CloudParams3.z;
    // 雲の種類の偏り(C4)。CloudParams3.wはこれまで未使用だった枠なので、FrameConstantsは1バイトも増えない
    params.CloudTypeBias = CloudParams3.w;
    // 雲層へ掛ける大気遠近(P12。Sky.hlsliのEvaluateCloudLayer (f)節)。
    // 雲はAerialPerspective.hlslの早期脱出でフォグを受けないため、雲側で自前に掛ける
    params = ApplyCloudFogParameters(params, FogParams0, CameraPosition.xyz);
    // レイマーチの開始位置を画素ごとにずらす量(C2)。スライスの縞をディザへ変える
    params.RaymarchJitter = CloudRaymarchDither(pixelPosition);
    return params;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float3 sceneColor = SceneColorTexture.Sample(ColorSampler, input.UV).rgb;

    float depth = DepthTexture.Sample(DataSampler, input.UV).r;
    if (depth <= 0.0f)
    {
        // Reverse-Zのため背景(スカイ)の深度は0.0。背景は既にDeferredLighting.hlslの解析評価
        // (またはスカイボックス)で正しい値になっており、経路長も定義できないためフォグを掛けない。
        // ここで1ビットも変えずに素通しすることが、フォグ無効時との差分ゼロを担保する
        return float4(sceneColor, 1.0f);
    }

    if (FogParams0.w <= 0.5f)
    {
        // 無効(UIでオフ、またはシーンが手続き空を使っていない。FogParams0.wのコメント参照)。
        // 恒等関数として振る舞い、以降の計算は一切行わない
        return float4(sceneColor, 1.0f);
    }

    float3 worldPos = ReconstructWorldPos(input.UV, depth);
    const float transmittance =
        HeightFogTransmittance(CameraPosition.xyz, worldPos, FogParams0.x, FogParams0.y, FogParams0.z);
    const float alpha = saturate(1.0f - transmittance) * saturate(FogParams1.x);

    // in-scatterの方向は視線方向のyを0以上へクランプしたものを使う。水平線より下を向いた画素で
    // SkyColorをそのまま評価すると下半球のGroundTint(暗い接地色)が返り、遠景が霞むのではなく
    // 黒ずんでしまう。水平線(viewDir.y==0)では両者が一致するため、クランプしても背景との
    // 連続性(depth<=0の早期脱出との継ぎ目)は保たれる
    const float3 viewDir = normalize(worldPos - CameraPosition.xyz);
    const float3 clampedDir = float3(viewDir.x, max(viewDir.y, 0.0f), viewDir.z);
    const float clampedLength = length(clampedDir);
    // 真下をちょうど向いた画素ではclampedDirが零ベクトルになり、normalizeがNaNを返す。
    // NaNはこの後のTAA・ブルーム・自動露出を経由して画面全体へ伝播するため必ず退避させる。
    // 該当するのは視線が厳密に真下の1画素だけで、その周囲の画素は既に水平方向を向いている
    // (x,zがどれだけ小さくても比だけで方向が決まる)ため、退避先も水平方向の任意の1方向でよい
    const float3 fogDir =
        (clampedLength > 1e-5f) ? (clampedDir / clampedLength) : float3(0.0f, 0.0f, 1.0f);

    // 【Mie位相関数は掛けない】SkyColorUpperが返す値には既にPerez分布のgamma項(太陽角距離の項)と
    // SunGlowTintが入っており、太陽方向で明るくなる角度依存性を既に持っている。その上に
    // 位相関数をさらに掛けると前方散乱を二重に計上することになるため、ここでは掛けない。
    //
    // 【雲を含むSkyColorではなく、晴天のSkyColorUpperを使う】
    // ここで必要なのは「カメラと着目点の間にある空気」が散乱して視線へ入れてくる光(airlight)で
    // あって、無限遠から届く空の放射輝度そのものではない。雲は高度1,000m以上のレイヤーで、
    // 視線が雲底平面と交わるのは水平距離で数kmから数十km先——カメラと数百m先の地物の間には
    // 存在しない。にもかかわらずSkyColorを使うと、地物の手前に無いはずの雲の輝度が
    // in-scatterに乗る。雲は青空の3倍以上明るく、しかもfBmで空間的に激しく変動するため、
    // 暗い地物ほど「雲の模様が透けて見える」形で破綻する(実測: 雲が流れる6秒間で塔の画素が
    // 最大72・平均5.7動き、これは同じ2フレーム間の空そのものの変化量(最大86・平均6.3)と
    // ほぼ同じ=雲がほぼ素通しで地物へ焼き付く)。
    //
    // 【地平線での背景との連続性は保たれる】無限遠へ収束するのは視線が水平(fogDir.y==0)の
    // ときだけで、Sky.hlsliの雲の地平線フェードはsmoothstep(kCloudHorizonFadeEndY=0,
    // kCloudHorizonFadeStartY=0.2, dir.y)——すなわちdir.y==0でフェードが厳密に0になり、
    // そこではSkyColorとSkyColorUpperの値が一致する。よってこのパスの設計目標である
    // 「遠方の地物が背景の空色へ厳密に収束し水平線に継ぎ目が出ない」は成立したままになる。
    // なおfogDirはyを0以上へクランプ済みなのでSkyColorの地面フェード分岐
    // (dir.y < kGroundFadeStartY = -0.02)には決して入らず、SkyColorをSkyColorUpperへ
    // 置き換えることは「雲の合成を外す」ことと厳密に等価になる。
    //
    // 【雲による減光と無彩色化はここで掛ける】(P18) 雲に覆われた空の下では airlight を
    // 照らす光そのものが弱まり、色も無彩色に寄る。SkyColorUpperは雲を通さない晴天の空
    // なので、被覆率1.0で空が灰色一色でも遠方の地物には青い散乱光が掛かり続けていた
    // (実測: 被覆率1.0での水平線際の空はB-R 42・輝度81だが、掛かっていたのは被覆率0の
    // B-R 86・輝度110)。
    //
    // 【視線の先の雲でSkyColorへ替える案は却下した】(a)地物の画素ごとにレイマーチが走って
    // コストが跳ねる。(b)地物までの散乱光を決めるのは「その空間を照らしている光」であって
    // 視線の先の雲ではないので、意味的にもずれる。正しい量は空全体の照度の半球平均であり、
    // それをSkyIntegrate.hlslが1本のRGBとして求めている(Sky.hlsli CloudSkyLight参照)。
    // 被覆率0では厳密に(1,1,1)なので、雲を持たないシーンの画素は1ビットも動かない
    const SkyParameters skyParams = MakeSkyParameters(input.Position.xy);
    const float3 inScatter = SkyColorUpper(fogDir, skyParams) * skyParams.CloudSkyLight;

    const float3 outColor = sceneColor * (1.0f - alpha) + inScatter * alpha;
    return float4(outColor, 1.0f);
}
