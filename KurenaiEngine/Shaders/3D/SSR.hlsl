// スクリーンスペースリフレクション(SSR)パス。
// Lightingパスで完成したSceneColor(HDR、トーンマップ前)を「反射先の環境色」として
// 簡易的に再利用し、G-Buffer(Normal/Material/Depth)を使ってワールド空間でレイマーチングする。
// HDRのまま扱うため、1.0を超える輝度(明るい光源の反射など)も正しく合成できる。
// トーンマッピングはこのパスより後段のTonemap.hlsl(Present直前)でまとめて行う。
//
// このパスは反射色を「加算」しない(20章)。Lightingパスは既に鏡面IBL
//   鏡面IBL = 環境の放射輝度(プローブ+グローバルIBLの合成) * SpecularIBLWeight(...)
// をSceneColorへ書き込んでいるため、SSRの結果をそのまま足すと同じ反射を二重に計上してしまう
// (14.9.5節。White Furnace TestがSSRを切っているのはこれが目に見える形で出るため)。
// 代わりにSSRは「環境の放射輝度だけを差し替える」:
//   出力 = SceneColor + (SSRが得た放射輝度 - Lightingが使った放射輝度) * SpecularIBLWeight(...) * 確信度
// 確信度が0なら出力はSceneColorと厳密に一致し、1ならSSRの放射輝度が鏡面IBLを完全に置き換える。
// 係数SpecularIBLWeightと環境の放射輝度SampleEnvironmentはReflectionProbe.hlsliで
// DeferredLighting.hlslと共有しており、「足した覚えのない値を引く」ことが起きないようにしている。
//
// レイが画面外に外れた場合や最大距離まで判定がつかなかった場合は確信度0とし、Lightingパスが
// 適用したプローブ/グローバルIBLをそのまま残す。プローブが画面外の情報を持っているため、
// 「何もしない=プローブに任せる」が正しい答えになる。
//
// このエンジンにはPSOのブレンドステートが無いため、既存のSSAO/SSILと同じ
// フルスクリーン三角形+ピクセルシェーダーのパターンで実装し、合成もこのシェーダー内で直接行う。
#include "NormalEncoding.hlsli"
#include "Samplers.hlsli"
// 水面の解析空フォールバック用。DeferredLighting.hlslが背景の解析評価に使っている
// のと同じ空モデル定義を共有する。cbufferに依存しないヘッダーで、PIも定義しないため
// (Sky.hlsli冒頭のコメント参照)、このファイルがPIを定義していない現状と衝突しない
// ボリュメトリック積雲が引く3Dノイズのレジスタ。Sky.hlsliはcbufferにもレジスタにも
// 依存しない方針なので、DDGI.hlsliと同じくインクルードする側がマクロで指定する。
// 定義しないシェーダー(SkyGenerate/AerialPerspective/PlanarReflection)ではボリュームの
// 経路がコンパイルされず、従来の平面の経路だけが残る
// SkyView LUT。日中の空はこのLUTを引く。**定義しないと日中の空が黒くなる**ので、
// SkyColorUpperUnitを呼ぶシェーダーは全員定義すること(Sky.hlsliのSkyViewセクション参照)
#define KURENAI_SKYVIEW_REGISTER t15
#define KURENAI_CLOUD_SHAPE_REGISTER t13
#define KURENAI_CLOUD_DETAIL_REGISTER t14
#include "Sky.hlsli"

static const int kSSRStepCount = 32;
static const int kSSRBinaryStepCount = 6;
static const float kSSREdgeFadeDistance = 0.1f;
// 水面のマテリアルID(G-BufferのMaterial.a)。GBufferCommon.hlsliのkMaterialIDWaterと
// **同じ値でなければならない**。GBufferCommon.hlsliはcbuffer/テクスチャの宣言を含み
// このパスへはインクルードできないため、値をここに複製している
static const float kSSRWaterMaterialID = 1.0f;

// 反射プローブの環境ソースと鏡面IBLの重み(DeferredLighting.hlslと共有)。
// 拡散イラディアンスは使わないため、拡散側のレジスタは定義しない
#define KURENAI_GLOBAL_PREFILTERED_REGISTER t7
#define KURENAI_PROBE_PREFILTERED_REGISTER t8
#define KURENAI_PROBE_BUFFER_REGISTER t9
// 距離キューブ(19.12節)。DeferredLighting.hlslと同じ条件でコンパイルしないと、
// SSRが「Lightingが使ったのとは違う放射輝度」を引き算することになるため必ず定義する
#define KURENAI_PROBE_DISTANCE_REGISTER t10

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
    // このシェーダでは未使用(オフセット合わせのためだけに宣言する)。
    // a=昼度を鏡面IBLの重みに含めてはいけない。手続き空は空自体が暗くなるため(21.4節)
    float4 AmbientColor;
    // このシェーダでは未使用(オフセット合わせのためだけに宣言する)
    float4 CascadeSplits;
    // y: プリフィルタ済み鏡面マップの最大ミップレベル、z: IBL強度倍率、
    // w: スペキュラのマルチスキャッタリング・エネルギー補正のトグル
    float4 ShadowParams;
    // このシェーダでは未使用(オフセット合わせのためだけに宣言する)
    float4 ActiveLightCount;
    // 拡散イラディアンスの取得元切り替え。このシェーダは鏡面しか扱わないため未使用だが、
    // 後続のProbeParamsのオフセットを合わせるために宣言だけしている
    float4 IBLParams;
    // 反射プローブ用。ReflectionProbe.hlsliのプローブ選択・ブレンドが読む
    float4 ProbeParams;
    // 距離キューブ用(19.12節)。同じくReflectionProbe.hlsliが読む
    float4 ProbeParams2;
    // ここから下、TAA(23章)・DDGI(22章)・水面の波用の8本はこのシェーダでは未使用。
    // cbufferのレイアウトは宣言順で決まり途中のフィールドを飛ばせないため、末尾のSky*
    // フィールドのオフセットをC++側 KurenaiEngine3D.cpp の FrameConstants と合わせる
    // 目的だけで宣言している(DeferredLighting.hlslの同名フィールドと同じ扱い)
    float4x4 PrevViewProj;
    float4 TAAParams;
    float4 DDGIParams0;
    float4 DDGIParams1;
    float4 DDGIParams2;
    float4 DDGIParams3;
    float4 DDGIParams4;
    // bent normalによる遮蔽(34章)。DeferredLighting.hlslと必ず同じ値を読むこと。
    //
    // 【masterではここでDDGIParams0〜4の5本が抜けていた】cbufferは宣言順レイアウトなので、
    // 5本(80バイト)飛ばした位置を OcclusionParams として読んでいた——実体は DDGIParams0
    // (GIボリュームの最小コーナーのワールド座標)で、その y をスペキュラ遮蔽の方式番号として
    // 解釈していた。つまり**GIボリュームの高さでSSRの遮蔽方式が変わっていた**。
    // landscape-water-skyのマージで、この5本を宣言することで直した
    float4 OcclusionParams;
    float4 TimeParams;
    // 空の解析評価用。水面の解析空フォールバック(下記MakeSkyParameters参照)が
    // 読む。DeferredLighting.hlslのFrameConstants宣言と同じ意味を持つ値なのでそちらのコメントも
    // 参照。xyz=太陽が「ある」向き(未正規化のまま渡ってくる。呼び出し側でnormalizeする。
    // SkyGenerate.hlsl側の慣習に合わせてある)、w=未使用
    float4 SkySunDirection;
    // x=未使用(天頂輝度はSkyParametersBufferにある)。y=背景(深度なし画素)を解析評価するか
    // のフラグだが、このシェーダは背景を描かないため未使用(水面フォールバックの有効/無効は
    // DeferredLighting.hlslと共有せず、SSRConstants.Params0.wで別途持つ。C++側Render()が
    // 手続き空の有効/無効を含めて一本化して決める。KurenaiEngine3D.cppのExecute内コメント参照)。
    // z=太陽照度/空照度比(SunToSkyIlluminanceRatio。MakeSkyParametersが読み、
    // Sky.hlsliのEvaluateCloudLayerが雲の明るさを太陽照度基準にするために使う)、w=未使用
    float4 SkyParams;
    // 雲(さらに末尾に追加)。DeferredLighting.hlslの同名フィールドと完全に同じ順・同じ型
    // であること(C++側 KurenaiEngine3D.cpp の FrameConstants::CloudParams0/1 と揃える。
    // ずれると背景に見える雲と水面に映る雲が食い違う)。
    // CloudParams0: x=被覆率(0で雲なし。Sky.hlsliのSkyColorが早期脱出する)、
    //               y=雲底の高度[m](カメラ基準)、z=UVスケール[ノイズ空間/m]、w=消散係数
    float4 CloudParams0;
    // CloudParams1: xy=風によるノイズ空間の移動量(CPU側でSky.hlsliのkCloudNoisePeriodと
    //               同じ周期でwrap済み)、z=Henyey-Greensteinの非対称パラメータ、w=未使用
    float4 CloudParams1;
    // 巻雲(さらに末尾に追加)。DeferredLighting.hlsl/PlanarReflection.hlslの同名フィールドと
    // 完全に同じ順・同じ型であること(C++側 KurenaiEngine3D.cpp の FrameConstants::CloudParams2/3 と揃える)。
    // CloudParams2: x=巻雲の被覆率(0で巻雲なし)、y=雲底の高度[m](カメラ基準)、
    //               z=UVスケール[ノイズ空間/m]、w=消散係数
    float4 CloudParams2;
    // CloudParams3: xy=風によるノイズ空間の移動量(積雲と同じくkCloudNoisePeriodでwrap済み)、
    //               z=fBmのUV(U方向)を伸ばす異方性スケール、w=未使用
    float4 CloudParams3;
    // 平面反射。このシェーダでは未使用(オフセット合わせのためだけに宣言する)。
    // 実際の値はSSRConstants.Params1として別途受け取っている(下記cbuffer SSRConstants参照)
    float4 PlanarReflectionPlane;
    // 大気遠近(末尾に追加)。このシェーダでは未使用だが、C++側 KurenaiEngine3D.cpp の
    // FrameConstantsと並びを一致させる目的だけで宣言する
    // (AerialPerspective.hlsl/PlanarReflection.hlslが読む)
    float4 FogParams0;
    float4 FogParams1;
    // 水中項。このシェーダでは未使用(オフセット合わせのためだけに宣言する)。Water.hlslが読む
    float4 WaterBodyColor;
    // 星空(末尾に追加)。水面に映る空にも星を出すために読む。
    // C++側 KurenaiEngine3D.cpp の FrameConstants::StarsParams と揃えること
    float4 StarsParams;
};

cbuffer SSRConstants : register(b1)
{
    // w: 水面の解析空フォールバックを使うか(1=使う)。C++側で
    // m_WaterAnalyticSkyReflection && usingProceduralSky の両方が立っているときだけ1になる
    // (手続き空が無効=.ksceneがDDSスカイボックスを明示するシーンでは、このトグルの値に
    // 関わらず必ず0にする。DDSは任意の絵でPerezモデルとは無関係なため解析評価できない)
    float4 Params0; // x: 最大レイ距離(ワールド単位), y: ヒット判定の厚み, z: ラフネスカットオフ, w: 水面の解析空フォールバック
    // 平面反射(末尾に追加)。x: このフレームで平面反射パスを実行したか(1=有効。
    // C++側でm_PlanarReflectionEnabled && 水面インスタンスが存在するときのみ1になる)、
    // y: 波の法線による画面UVのずらし量(m_PlanarReflectionDistortion)、zw: 未使用
    float4 Params1;
};

Texture2D SceneColorTexture : register(t0);
Texture2D NormalTexture : register(t1);
Texture2D MaterialTexture : register(t2);
Texture2D DepthTexture : register(t3);
Texture2D AlbedoTexture : register(t4);
// SSAO/SSILのAO/GIバッファ。a=遮蔽率。スペキュラオクルージョンに使う
// (Lightingパスが適用した鏡面IBLの重みを再現するために必要)
Texture2D AOTexture : register(t5);
// split-sum近似の第2項、BRDF積分LUT
Texture2D BRDFLUTTexture : register(t6);
// bent normal(GBuffer.hlslがSV_TARGET5へ書いたワールド空間のbRaw)。
// t11は平面反射が使うためt16に置く(34章)
Texture2D BentNormalTexture : register(t16);

// プリフィルタ済み鏡面(t7)・プローブのキューブマップ配列(t8)・プローブの影響範囲バッファ(t9)・
// プローブの距離キューブ(t10)の宣言と、プローブの選択・視差補正・ブレンド・鏡面IBLの重みは
// ReflectionProbe.hlsliが持つ
#include "ReflectionProbe.hlsli"

// 平面反射。KurenaiEngine3D::Renderが鏡映カメラで描いたPlanarReflection.hlslの結果
// (m_PlanarReflectionColor)。t0〜t10は上ですべて埋まっているためt11を使う
Texture2D PlanarReflectionTexture : register(t11);
// SkyIntegrate.hlslが書いた空パラメータ。ティント4本と正規化済みの天頂輝度が入る。
// t0〜t11が既に使用済みのためt12を使う
StructuredBuffer<GPUSkyParameters> SkyParametersBuffer : register(t12);

struct PSInput
{
    float4 Position : SV_POSITION;
    float2 UV : TEXCOORD0;
};

// 頂点バッファなしで画面全体を覆う三角形を1枚だけ生成する定番のテクニック
PSInput VSMain(uint vertexID : SV_VertexID)
{
    PSInput output;
    output.UV = float2((vertexID << 1) & 2, vertexID & 2);
    output.Position = float4(output.UV.x * 2.0f - 1.0f, 1.0f - output.UV.y * 2.0f, 0.0f, 1.0f);
    return output;
}

float3 ReconstructWorldPos(float2 uv, float depth)
{
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 clipPos = float4(ndc, depth, 1.0f);
    float4 worldPos = mul(clipPos, InvViewProj);
    return worldPos.xyz / worldPos.w;
}

// FrameConstantsのSky*フィールドからSky.hlsliのSkyParametersを組み立てる。
// DeferredLighting.hlsl/AerialPerspective.hlsl/PlanarReflection.hlslのMakeSkyParametersと
// 完全に同一の内容であること(正規化の扱いを含む)。4つのシェーダーはcbufferをそれぞれ別に
// 宣言しているため関数そのものは共有できず複製しているが、中身がずれると「背景の空」
// 「水面に映る空」「フォグの合成先の色」が互いに食い違ってしまうため、
// 中身を変える場合は必ず4つとも同時に直すこと
SkyParameters MakeSkyParameters()
{
    SkyParameters params;
    params.SunDirection = normalize(SkySunDirection.xyz);
    // ティント4本と天頂輝度はSkyParametersBuffer(t12)にある(SkyIntegrate.hlslが書く)
    params = ApplySkyParametersFromBuffer(params, SkyParametersBuffer[0]);
    // 太陽照度/空照度比(SkyParams.zに詰めてある。KurenaiEngine3D.cppのSkyParams.zコメント参照)。
    // EvaluateCloudLayerが雲の明るさを太陽照度基準にするために使う
    params.SunToSkyIlluminanceRatio = SkyParams.z;
    // 雲。DeferredLighting.hlslのMakeSkyParametersと完全に同一の内容であること
    // (このファイル冒頭のコメントと同じ理由。背景に見える雲と水面に映る雲が食い違ってはいけない)
    params.CloudCoverage = CloudParams0.x;
    params.CloudAltitude = CloudParams0.y;
    params.CloudUvScale = CloudParams0.z;
    params.CloudDensity = CloudParams0.w;
    params.CloudScrollOffset = CloudParams1.xy;
    params.CloudForwardG = CloudParams1.z;
    // 積雲の厚み[m](CloudParams1.wの枠に詰めてある)。
    // 0ならレイマーチせず平面として扱う
    params.CloudThickness = CloudParams1.w;
    // 巻雲。DeferredLighting.hlslのMakeSkyParametersと完全に同一の内容であること
    params.CirrusCoverage = CloudParams2.x;
    params.CirrusAltitude = CloudParams2.y;
    params.CirrusUvScale = CloudParams2.z;
    params.CirrusDensity = CloudParams2.w;
    params.CirrusScrollOffset = CloudParams3.xy;
    params.CirrusAnisotropy = CloudParams3.z;
    // 雲層へ掛ける大気遠近(Sky.hlsliのEvaluateCloudLayer (f)節)。
    // 雲はAerialPerspective.hlslの早期脱出でフォグを受けないため、雲側で自前に掛ける
    params = ApplyCloudFogParameters(params, FogParams0, CameraPosition.y);
    // 星空。水面に映る空にも背景と同じ星を出す(ApplyCloudFogParametersが0で潰した後に上書きする)。
    // 背景側(DeferredLighting.hlsl)と同じ値を入れること——食い違うと
    // 「空には出ているのに水面には映らない星」ができる
    params.StarsIntensity = StarsParams.x;
    params.StarsDensity = StarsParams.y;
    params.StarsTwinkle = StarsParams.z;
    params.StarsPixelAngle = StarsParams.w;
    params.StarsTime = TimeParams.x;
    return params;
}

// ワールド座標を画面UVとView空間Z(カメラからの距離。値が大きいほど遠い)へ投影する。
// カメラ背後、または画面外に出た場合はfalseを返す
bool ProjectToScreen(float3 worldPos, out float2 uv, out float viewZ)
{
    float4 clipPos = mul(float4(worldPos, 1.0f), ViewProj);
    if (clipPos.w <= 0.0f)
    {
        uv = float2(0.0f, 0.0f);
        viewZ = 0.0f;
        return false;
    }

    float3 ndc = clipPos.xyz / clipPos.w;
    uv = float2(ndc.x * 0.5f + 0.5f, 1.0f - (ndc.y * 0.5f + 0.5f));
    viewZ = mul(float4(worldPos, 1.0f), View).z;
    return (uv.x >= 0.0f && uv.x <= 1.0f && uv.y >= 0.0f && uv.y <= 1.0f);
}

// 平面反射を解析空フォールバックより優先して使う。呼び出し側(useWaterAnalyticSky
// が立っている水面画素)からのみ呼ばれる想定。
//
// 平面反射はSSRのレイマーチとは完全に別経路――鏡映カメラで景色を描き直したPlanarReflection.hlsl
// の結果(m_PlanarReflectionColor)を、反射ベクトルを再投影せず同じ画面UV(input.UV)でそのまま
// サンプルするだけでよい(平面鏡の反射は鏡映カメラで撮り直すことと数学的に等価なため。
// 詳細はPlanarReflection.hlsl冒頭のコメント参照)。波の法線でその画面UVを少しだけずらすことで、
// 波打つ水面らしい歪みを付ける。
//
// 【画面端の扱い】reflUVが画面端に近いほど平面反射の信頼度を落とすが、confidence
// (roughnessFade)ではなくここで解析空とのlerpとして表現する。confidenceを落とすと
// Lightingパスが適用したプローブ/グローバルIBLへ戻ってしまい、
// 「水面にIBLしか映らない」状態が画面端で出るため
//
// 【ジオメトリが無い方向の扱い】平面反射パスは不透明メッシュしか描かないため、
// 反射先に何も無い方向(=島の鏡像以外のほとんどの向き)のテクセルはクリア値のまま残る。
// **ここを区別せずlerpしてはいけない** ―― 水面のほぼ全面がクリア色(黒)で塗り潰され、
// 20.6節の解析空が見えなくなる(SSRを有効にすると水面が一様な暗色になる)。
// レンダーターゲットはアルファ0でクリアされ、PlanarReflection.hlslのPSMainは
// float4(color, 1.0f)を返すので、アルファがそのまま「ジオメトリが描かれたか」の
// カバレッジになる(ブレンドはOpaqueなのでアルファは加工されずに書き込まれる)。
// これを使って、描かれていない方向は解析空へ戻す
float3 ApplyPlanarReflection(float3 analyticSky, float2 screenUV, float3 N)
{
    // Params1.x <= 0.5fは「このフレームで平面反射パスを実行していない」ケース
    // (m_PlanarReflectionEnabled=falseか、シーンに水面インスタンスが無い)。
    // この分岐に入ると解析空のみの経路を通る
    if (Params1.x <= 0.5f)
    {
        return analyticSky;
    }

    const float2 reflUV = screenUV + N.xz * Params1.y;
    if (reflUV.x < 0.0f || reflUV.x > 1.0f || reflUV.y < 0.0f || reflUV.y > 1.0f)
    {
        // 画面外に出た場合は平面反射を使わず解析空のみにする
        return analyticSky;
    }

    const float4 planarSample = PlanarReflectionTexture.Sample(ColorSampler, reflUV);
    const float2 edgeDist = min(reflUV, float2(1.0f, 1.0f) - reflUV);
    const float edgeFade = saturate(min(edgeDist.x, edgeDist.y) / kSSREdgeFadeDistance);

    // 事前乗算済みアルファのover合成。
    // 【なぜlerp(analyticSky, planarSample.rgb, edgeFade * planarSample.a)ではないのか】
    // クリア値が(0,0,0,0)でジオメトリが(color, 1)を書くため、バイリニア補間された
    // テクセルのrgbは既にアルファが掛かった値(a * color)になっている。素のlerpだと
    // ジオメトリの輪郭でアルファがもう一度掛かって二重に暗くなる。
    // この形なら a=0 で解析空そのもの、a=1 かつ edgeFade=1 で平面反射そのものになり、
    // 中間でも輪郭に暗い縁が出ない
    return analyticSky * (1.0f - edgeFade * planarSample.a) + planarSample.rgb * edgeFade;
}

// UV位置の実際のジオメトリのView空間Zを取得する。背景(深度なし)ならfalseを返す
bool SampleSceneViewZ(float2 uv, out float viewZ)
{
    float sceneDepth = DepthTexture.Sample(DataSampler, uv).r;
    if (sceneDepth <= 0.0f)
    {
        viewZ = 0.0f;
        return false;
    }
    float3 sceneWorldPos = ReconstructWorldPos(uv, sceneDepth);
    viewZ = mul(float4(sceneWorldPos, 1.0f), View).z;
    return true;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float3 baseColor = SceneColorTexture.Sample(ColorSampler, input.UV).rgb;

    float depth = DepthTexture.Sample(DataSampler, input.UV).r;
    if (depth <= 0.0f)
    {
        // 背景(スカイ)には反射元のサーフェスがない
        return float4(baseColor, 1.0f);
    }

    float3 albedo = AlbedoTexture.Sample(ColorSampler, input.UV).rgb;
    // .aに水面のマテリアルID(kMaterialIDWater)が入っているため、rgbとaを1回のサンプルで
    // まとめて読む
    float4 materialSample = MaterialTexture.Sample(DataSampler, input.UV);
    float3 material = materialSample.rgb;
    float metallic = material.r;
    float roughness = material.g;
    float materialAO = material.b; // マテリアルの遮蔽マップ(GBuffer.hlslでstrength適用済み)
    // DataSamplerはPoint+Clamp(Samplers.hlsli参照)なので、水面と通常マテリアルの境界でIDが
    // バイリニア補間により中間値化することは無い。それでも==ではなく閾値で比較しているのは、
    // 浮動小数点の等値比較そのものを避ける一般的な安全策のため(実際に出現する値は0.0か1.0のみ)。
    // kSSRWaterMaterialIDの半分をしきい値にしているのは、0.5fと決め打つよりIDの値と連動させておき、
    // 将来kMaterialIDWater/kSSRWaterMaterialIDが1.0f以外に変わってもここを直し忘れないようにするため
    const bool isWater = materialSample.a > kSSRWaterMaterialID * 0.5f;

    const float maxDistance = Params0.x;
    const float thickness = Params0.y;
    const float roughnessCutoff = Params0.z;
    // 水面の解析空フォールバックが有効か。水面タグ(isWater)とC++側のフラグの両方が
    // 立っているときだけtrueになる。非水面画素では常にfalseになるため、以降の分岐は
    // このフィールドが存在しなかったときとまったく同じコードパスを通る
    const bool useWaterAnalyticSky = isWater && (Params0.w > 0.5f);

    // スクリーンスペースのレイマーチはヒット色を1点サンプルするだけで、粗い面に必要な
    // 円錐状のぼかしを持たない。そのため粗い面ほどSSRの結果を信用しない
    float roughnessFade = 1.0f - smoothstep(0.0f, roughnessCutoff, roughness);
    if (roughnessFade <= 0.0f)
    {
        // SSRを信用しない=Lightingパスが適用したプローブ/グローバルIBLをそのまま残す。
        // このシーンの水面メッシュはroughnessFactor=0.03(Tools/generate_water_plane.pyの
        // WATER_ROUGHNESS)で焼かれており、Water.hlslのPSMainが下限0.045へクランプするため
        // G-Bufferには0.045が入る。既定のroughnessCutoff(0.6)より十分低いのでここを通過するが、
        // ユーザーがroughnessCutoffをそれ以下まで下げると水面もここで弾かれ、
        // 下の解析空フォールバックも一緒に無効になる。
        // 「粗すぎる面ではSSRを信用しない」という設計は水面かどうかで変えていない
        return float4(baseColor, 1.0f);
    }

    float3 worldPos = ReconstructWorldPos(input.UV, depth);
    float3 N = OctDecode(NormalTexture.Sample(DataSampler, input.UV).xy);
    float3 V = normalize(CameraPosition.xyz - worldPos);
    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
    float NdotV = saturate(dot(N, V));

    float3 reflectDir = normalize(reflect(-V, N));

    // --- Lightingパスが適用した鏡面IBLを、そのときとまったく同じ式で再現する ---
    // 環境の放射輝度と、それに掛かる係数。どちらもReflectionProbe.hlsliの定義を共有しているため、
    // ここで求めた値はLightingパスがSceneColorへ足したものと定義上一致する
    // aoの合成式はDeferredLighting.hlslのPSMainとまったく同じでなければならない
    // (スクリーンスペースの遮蔽 × マテリアルの遮蔽マップ)。ズレるとSSRが適用される領域と
    // されない領域の境界に段差が出る
    const float ssao = AOTexture.Sample(ColorSampler, input.UV).a;
    // bent normalもDeferredLighting.hlslとまったく同じ引き方をすること。
    // 反射ベクトルも同じものを渡す。あちらはreflect(-V, N)でnormalizeを挟まないが、
    // VとNが単位ベクトルならreflectは長さを保つので同じ向き・同じ長さになる
    const BentOcclusion bent = DecodeBentOcclusion(BentNormalTexture.Sample(DataSampler, input.UV), N);
    // 0 = Frostbite近似 / 1 = 球冠交差 / 2 = 球面ガウス(34.11節)。
    // DeferredLighting.hlslとまったく同じ読み方をすること(段差防止)
    const int soMode = (int)(OcclusionParams.y + 0.5f);
    const float3 brdf = BRDFLUTTexture.Sample(ColorSampler, float2(NdotV, roughness)).rgb;
    const float3 specularWeight =
        SpecularIBLWeight(F0, NdotV, roughness, soMode, bent, N, reflectDir, materialAO, ssao, brdf,
                          ShadowParams.w, ShadowParams.z);
    // Kulla-Conty方式の加算ローブ(SpecularIBLMultiScatterWeight)はここでは扱わない。
    // あれは拡散イラディアンスに掛かるほぼ拡散のローブで、鏡面反射として差し替える対象では
    // ないため、Lightingパスが足したまま残す(14.9節)

    const float mipLevel = roughness * ShadowParams.y;
    float3 unusedIrradiance;
    float3 envRadiance;
    SampleEnvironment(worldPos, N, reflectDir, mipLevel, unusedIrradiance, envRadiance);

    // 線形マーチ: レイに沿って一定間隔でサンプルし、G-Buffer深度より奥に入った地点(ヒット)を探す
    const float stepSize = maxDistance / float(kSSRStepCount);
    bool hit = false;
    bool skyHit = false;
    float2 hitUV = float2(0.0f, 0.0f);
    float tPrev = 0.0f;
    float tCurr = 0.0f;

    [loop]
    for (int i = 1; i <= kSSRStepCount; ++i)
    {
        tPrev = tCurr;
        tCurr = stepSize * float(i);

        float3 samplePos = worldPos + reflectDir * tCurr;
        float2 sampleUV;
        float rayViewZ;
        if (!ProjectToScreen(samplePos, sampleUV, rayViewZ))
        {
            // 画面外に外れた: この先に何があるか(スカイか別のジオメトリか)分からないため打ち切る
            break;
        }

        float sceneViewZ;
        if (!SampleSceneViewZ(sampleUV, sceneViewZ))
        {
            // 画面内で背景(スカイ)ピクセルに到達したことが確定したので、以降はスカイボックスへ
            // フォールバックしてよい
            skyHit = true;
            break;
        }

        if (rayViewZ >= sceneViewZ && rayViewZ - sceneViewZ < thickness)
        {
            hit = true;
            hitUV = sampleUV;
            break;
        }
    }

    // --- 環境の放射輝度を差し替える ---
    // newRadiance が envRadiance の代わりに使う放射輝度、confidence がその信用度
    float3 newRadiance = envRadiance;
    float confidence = 0.0f;

    if (hit)
    {
        // 2分探索でヒット区間[tPrev, tCurr]を精密化し、貫通による誤差を減らす
        float tLo = tPrev;
        float tHi = tCurr;
        [unroll]
        for (int j = 0; j < kSSRBinaryStepCount; ++j)
        {
            float tMid = (tLo + tHi) * 0.5f;
            float3 samplePos = worldPos + reflectDir * tMid;
            float2 sampleUV;
            float rayViewZ;
            float sceneViewZ;
            if (ProjectToScreen(samplePos, sampleUV, rayViewZ) && SampleSceneViewZ(sampleUV, sceneViewZ) && rayViewZ >= sceneViewZ)
            {
                hitUV = sampleUV;
                tHi = tMid;
            }
            else
            {
                tLo = tMid;
            }
        }

        // 画面内に実際に映っているサーフェスの色。プローブより新しく、視差も完全に正しい
        newRadiance = SceneColorTexture.Sample(ColorSampler, hitUV).rgb;

        // 反射先が画面の縁に近いほど信用を落とす(画面外へレイが抜ける際の急な打ち切りを緩和する)。
        // 縁で確信度が0へ落ちると、その分だけプローブ/グローバルIBLへ滑らかに戻る
        float2 edgeDist = min(hitUV, float2(1.0f, 1.0f) - hitUV);
        float edgeFade = saturate(min(edgeDist.x, edgeDist.y) / kSSREdgeFadeDistance);

        confidence = roughnessFade * edgeFade;
    }
    else if (skyHit)
    {
        // 画面内で実際にスカイへ到達したことが確定した場合。プローブは屋内の壁を返しうるが、
        // このレイは確かに外へ抜けているので、空のほうが正しい答えになる。
        if (useWaterAnalyticSky)
        {
            // 水面: プリフィルタ済み鏡面(128pxベースのキューブマップをラフネス由来の
            // ミップで引く)の代わりに、Perez分布を画面解像度でそのまま評価した解析空を使う。
            // 水面はroughnessが低く(このroughnessCutoffのゲートを通過している時点でそう)、
            // 低ミップの128pxを直接引くと空に映る太陽・地平線の勾配が色斑としてにじむため、
            // 解析評価のほうが実際の見え方に近い。
            // reflectDirが水平線より下を向く場合(強い波で反射ベクトルが下向きになったとき)は
            // SkyColorが持つ地平線下の接地色へのフェード(Sky.hlsli kGroundFadeStartY/EndY)で
            // そのまま処理でき、ここで別扱いする必要はない。
            // 平面反射はこの解析空よりさらに優先する(ApplyPlanarReflection参照)
            newRadiance = ApplyPlanarReflection(SkyColor(reflectDir, MakeSkyParameters()), input.UV, N);
        }
        else
        {
            // 生のスカイボックスではなくプリフィルタ済み鏡面をラフネス→ミップで引く
            // (生のスカイボックスにはミップ選択が無く、粗い面でも鮮鋭な鏡像が返ってしまう)
            newRadiance = PrefilteredEnvTexture.SampleLevel(MaterialSampler, reflectDir, mipLevel).rgb;
        }
        confidence = roughnessFade;
    }
    else if (useWaterAnalyticSky)
    {
        // 画面外に外れた、または最大距離まで判定がつかなかった場合。非水面はこの分岐が無く
        // confidence = 0 のまま(Lightingパスが適用したプローブ/グローバルIBLをそのまま残す。
        // プローブが画面外を知っているため、20.3節のとおりこれが正しい答え)。
        //
        // 水面だけここで解析空を使う理由: このシーンの水面は4000m四方あり、かつ
        // SSRMaxDistance(既定5.0m)に対して反射レイはほぼ確実に数ステップで画面外へ抜けるか
        // 最大距離まで判定がつかない。つまり水面ではこの分岐が「レアケース」ではなく
        // ほぼ常時通る経路になり、confidence=0のままだと水面の映り込みが実質いつも死んで
        // プリフィルタ済み鏡面IBL(低解像度でにじむ)しか見えなくなる。
        // 水平な水面を上から見たとき反射ベクトルは必ず上向き(空側)を向くため、
        // 空で埋めるのは常に妥当な近似になる(上のskyHit分岐と同じ理由)。
        // 屋根の下の水たまりのような反例は、.kscene側で[Model]Water=trueと明示的にタグ付けした
        // 面にしかこの経路が適用されない(オプトイン)ため、影響範囲がそこに閉じている。
        // 平面反射はこの解析空よりさらに優先する(ApplyPlanarReflection参照)
        newRadiance = ApplyPlanarReflection(SkyColor(reflectDir, MakeSkyParameters()), input.UV, N);
        confidence = roughnessFade;
    }
    // 非水面が画面外に外れた、または最大距離まで判定がつかなかった場合は confidence = 0 のまま。
    // Lightingパスが適用したプローブ/グローバルIBLをそのまま残す(プローブが画面外を知っている)

    const float3 composited = baseColor + (newRadiance - envRadiance) * specularWeight * confidence;

    // 半透明サーフェスのピクセルではG-Bufferが「ガラスの奥にある不透明面」の値を持つため、
    // ここで引く鏡面IBLがSceneColor(ガラスで上書き済み)に含まれておらず負へ振れうる。
    // 半透明パスがSSRの対象外である以上この不一致は避けられないので、負の輝度だけは止めておく
    return float4(max(composited, float3(0.0f, 0.0f, 0.0f)), 1.0f);
}
