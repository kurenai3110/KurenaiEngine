// 最終合成パス。DirectLightingパスで計算済みの直接光(拡散+鏡面反射、シャドウ適用済み)を
// サンプルし、IBL(拡散イラディアンス+プリフィルタ済み鏡面、AO/SSILの遮蔽率を適用)・
// 間接拡散光(SSIL使用時)を加算する。PBRのライティング計算自体はDirectLighting.hlsl側/
// このパスのEvaluateIBLで行うため、SceneColorへの書き込みはバッファの合成として行う。
// 出力はHDR(SceneColor、1.0を超える輝度を保持)のままで、トーンマッピングは行わない。
// SSRパスがこのHDR値を反射元として参照するため、ここでLDRへ落とすとSSRの反射色が
// 1.0を超えられずエネルギー保存が破れる。トーンマッピングはPresent直前のTonemap.hlslで行う
#include "NormalEncoding.hlsli"
// スペキュラのマルチスキャッタリング・エネルギー補正(14.9節)
#include "SpecularEnergy.hlsli"
// 空モデル(Perez分布)の共有ヘッダー(P3)。背景画素をSkyGenerate.hlslのキューブマップと
// 同じ関数・同じパラメータで画面解像度評価するために使う。PIを定義しないため、
// このファイルのPI定義(直後)より前でも後でもインクルード順は問題ない
#include "Sky.hlsli"

static const float PI = 3.14159265359f;

// 反射プローブの環境ソースと鏡面IBLの重み。SSR.hlslが同じ定義を共有する(20章)。
// ReflectionProbe.hlsliはSamplers.hlsliのMaterialSamplerとFrameConstantsの
// ProbeParams/ShadowParams/AmbientColorを参照するため、それらの宣言より後でインクルードする
#define KURENAI_GLOBAL_IRRADIANCE_REGISTER t8
#define KURENAI_GLOBAL_PREFILTERED_REGISTER t9
#define KURENAI_PROBE_IRRADIANCE_REGISTER t11
#define KURENAI_PROBE_PREFILTERED_REGISTER t12
#define KURENAI_PROBE_BUFFER_REGISTER t13
#define KURENAI_PROBE_DISTANCE_REGISTER t14
// DDGI(22章)のオクタヘドラルアトラス。拡散イラディアンスだけを差し替えるため、
// 鏡面を担うReflectionProbe.hlsli側とはレジスタも役割も完全に分かれている
#define KURENAI_DDGI_IRRADIANCE_REGISTER t15
#define KURENAI_DDGI_DISTANCE_REGISTER t16

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
    // 昼夜サイクル用。rgb=環境光の色(m_AmbientScale乗算済み、KurenaiEngine3D::Render側の
    // constants.AmbientColor代入部を参照)、a=昼度(0=夜,1=昼)。
    // **昼度はIBLの減衰にはもう使わない**。かつては空が昼固定のスカイボックスから焼かれていた
    // ためこれが唯一の減光手段だったが、手続き空(SkyGenerate.hlsl)の導入で空自体が
    // 太陽高度に応じて暗くなるようになり不要になった(21.4節。掛けると二重に暗くなる)。
    // Enable IBL無効時はIBL導入以前と同じ、rgbをそのまま定数色アンビエントとして使う(PSMain参照)
    float4 AmbientColor;
    // このシェーダでは未使用(オフセット合わせのためだけに宣言する)
    float4 CascadeSplits;
    // y: プリフィルタ済み鏡面マップの最大ミップレベル(ミップ数-1)。ラフネス[0,1]をミップ番号へ
    // 変換するのに使う(EvaluateIBL参照)。z: IBL強度倍率(m_IBLEnabled=falseなら0.0f。
    // PSMain側でこれが0以下の場合はEvaluateIBLの代わりにAmbientColor.rgbの定数色アンビエントへ
    // フォールバックする)。x/wはこのシェーダでは未使用
    float4 ShadowParams;
    // 半透明パス(Transparent.hlsl)専用のフィールドで、このシェーダでは使わない。cbufferのレイアウトは
    // 宣言順で決まり途中のフィールドを飛ばせないため、後続のIBLParams/ProbeParamsのオフセットを
    // 合わせる目的で宣言だけしている(C++側 KurenaiEngine3D.cpp の FrameConstants と並びを一致させること)
    float4 ActiveLightCount;
    // x: 拡散イラディアンスの取得元(0=プリフィルタ済み鏡面の最終ミップ、1=専用イラディアンスマップ)。
    // EvaluateIBL参照。yzwは未使用
    float4 IBLParams;
    // 反射プローブ用(19章)。x=有効プローブ数(0ならプローブは一切使わずグローバルIBLのみ)、
    // y=影響範囲のデバッグ表示フラグ(1以上でプローブごとの色分け表示に切り替える)、
    // z=視差補正(box projection)の有効フラグ、w=プローブ間ブレンドの有効フラグ
    float4 ProbeParams;
    // 反射プローブの距離キューブ用(19.12節)。x=視差補正に距離キューブを使うフラグ、
    // y=距離キューブによる遮蔽判定(光漏れ抑制)の有効フラグ、z=距離キューブの1面の解像度、w=未使用
    float4 ProbeParams2;
    // TAA(23章)用。このシェーダーでは未使用だが、C++側でDDGIParamsより手前に置かれているため
    // オフセット合わせのためだけに宣言する
    float4x4 PrevViewProj;
    float4 TAAParams;
    // DDGI用(22章)。レイアウトはC++側 KurenaiEngine3D.cpp の FrameConstants のコメント参照。
    // DDGI.hlsliがこの4本を読む
    float4 DDGIParams0;
    float4 DDGIParams1;
    float4 DDGIParams2;
    float4 DDGIParams3;
    // x=このフレームの実効プリ露出(アトラスは露出非依存で持つため読み出し時に掛け戻す)
    float4 DDGIParams4;
    // 水面(P2)用。このシェーダでは未使用だが、C++側でSkySunDirection等より手前に置かれているため
    // オフセット合わせのためだけに宣言する
    float4 TimeParams;
    // 空の解析評価用(P3、末尾に追加)。背景(深度が無い画素)を、キューブマップのサンプルではなく
    // Sky.hlsliのSkyColorを画面解像度で直接評価するために使う。キューブマップは256px/面・
    // ミップ無しで、3840px・水平画角68度のカメラでは約20倍に拡大表示されるため、画面解像度で
    // 評価したほうが背景の輪郭がシャープになる(IBLは畳み込むため低解像度のままで正しい)。
    // 値はSkyGenerate.hlslが焼くキューブマップに使ったものと同一
    // (m_SkyParametersBuffer参照。ティント・天頂輝度はP9でこのcbufferから
    // StructuredBuffer<GPUSkyParameters>(t17)へ移った)。
    // xyz=太陽が「ある」向き(未正規化のまま渡ってくる。PSMain側でnormalizeする。
    // SkyGenerate.hlsl側の慣習=呼び出し側でnormalizeする、に合わせてある)、w=未使用
    float4 SkySunDirection;
    // x=未使用(P9で天頂輝度はSkyParametersBufferへ移動)、y=背景を解析評価するかのフラグ
    // (1=解析、0=キューブマップをサンプル。手続き空が無効なときは常に0)、zw=未使用
    float4 SkyParams;
    // 雲(P5、さらに末尾に追加)。SSR.hlslの同名フィールドと完全に同じ順・同じ型であること
    // (C++側 KurenaiEngine3D.cpp の FrameConstants::CloudParams0/1 と揃える。ずれると
    // 背景に見える雲と水面に映る雲が食い違う)。
    // CloudParams0: x=被覆率(0で雲なし。Sky.hlsliのSkyColorが早期脱出する)、
    //               y=雲底の高度[m](カメラ基準)、z=UVスケール[ノイズ空間/m]、w=消散係数
    float4 CloudParams0;
    // CloudParams1: xy=風によるノイズ空間の移動量(CPU側でSky.hlsliのkCloudNoisePeriodと
    //               同じ周期でwrap済み)、z=Henyey-Greensteinの非対称パラメータ、w=未使用
    float4 CloudParams1;
    // 巻雲(P11、さらに末尾に追加)。SSR.hlsl/PlanarReflection.hlslの同名フィールドと完全に
    // 同じ順・同じ型であること(C++側 KurenaiEngine3D.cpp の FrameConstants::CloudParams2/3 と揃える)。
    // CloudParams2: x=巻雲の被覆率(0で巻雲なし)、y=雲底の高度[m](カメラ基準)、
    //               z=UVスケール[ノイズ空間/m]、w=消散係数
    float4 CloudParams2;
    // CloudParams3: xy=風によるノイズ空間の移動量(積雲と同じくkCloudNoisePeriodでwrap済み)、
    //               z=fBmのUV(U方向)を伸ばす異方性スケール、w=未使用
    float4 CloudParams3;
};

Texture2D AlbedoTexture : register(t0);
Texture2D DirectLightTexture : register(t1);
Texture2D MaterialTexture : register(t2);
Texture2D DepthTexture : register(t3);
TextureCube SkyboxTexture : register(t4);
// SSAO/SSIL(Visibility Bitmask)共通のAO/GIバッファ。rgb=間接拡散光(加算)、a=遮蔽率(乗算)
Texture2D AOTexture : register(t5);
// G-Bufferのエミッシブ(自発光)バッファ。AO/シャドウの影響を受けず常に加算する
Texture2D EmissiveTexture : register(t6);
// G-Bufferの法線(オクタヘドラルエンコード)。IBLの方向依存項(拡散イラディアンスのサンプル方向、
// 鏡面の反射方向)を求めるのに必要
Texture2D NormalTexture : register(t7);
// split-sum近似の第2項、BRDF積分LUT(x=NdotV, y=ラフネス。BRDFLUT.hlslで生成、方向性を持たない
// (NdotV, ラフネス)の2Dルックアップテーブルのため、これだけは通常のTexture2Dのまま)
Texture2D BRDFLUTTexture : register(t10);
// SkyIntegrate.hlslが書いた空パラメータ(P9)。ティント4本と正規化済みの天頂輝度が入る。
// t0〜t16が既に使用済みのためt17を使う
StructuredBuffer<GPUSkyParameters> SkyParametersBuffer : register(t17);

// 拡散イラディアンス(t8)・プリフィルタ済み鏡面(t9)・プローブのキューブマップ配列(t11/t12)・
// プローブの影響範囲バッファ(t13)の宣言と、プローブの選択・視差補正・ブレンド・鏡面IBLの重みは
// ReflectionProbe.hlsliが持つ(SSR.hlslと共有するため。レジスタ番号は上のマクロで与えている)
#include "ReflectionProbe.hlsli"
// DDGI(22章)。拡散イラディアンスだけを差し替える。鏡面には一切触れないため、
// SSRとの「足した量と引く量が一致する」不変条件(20章)には影響しない
#include "DDGI.hlsli"

// 環境ソース(プローブとグローバルIBLの重み付き合成)はSampleEnvironmentが返す。プローブと
// グローバルIBLはどちらも同じ手順で焼かれており(IBLConvolve.hlslを共有している)、解像度・
// ミップ構成も揃えてあるため、式そのものは同一で引くキューブマップだけが変わる
float3 EvaluateIBL(float3 N, float3 V, float3 worldPos, float3 albedo, float metallic, float roughness, float ao)
{
    const float NdotV = saturate(dot(N, V));
    const float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);

    const float3 R = reflect(-V, N);
    // ShadowParams.y = プリフィルタ済み鏡面マップの最大ミップレベル(ミップ数-1、KurenaiEngine3D側で設定)
    const float mipLevel = roughness * ShadowParams.y;

    float3 irradiance;
    float3 prefiltered;
    SampleEnvironment(worldPos, N, R, mipLevel, irradiance, prefiltered);

    // DDGI(22章)が有効なら、拡散の環境ソースだけをプローブ格子由来のものへ差し替える。
    // 【加算ではなく差し替えである】DDGIのイラディアンスは「その位置に来ている光」そのもので、
    // グローバルIBL/反射プローブのイラディアンスと同じ量の別の推定値である。足すと二重計上になる。
    // 鏡面(prefiltered)は差し替えない——反射プローブのほうが方向解像度が桁違いに高く、
    // DDGIのオクタヘドラル6x6では鏡面の映り込みを表現できないため
    if (DDGIParams0.w > 0.5f)
    {
        irradiance = SampleDDGIIrradiance(worldPos, N, V);
    }

    // --- 拡散IBL ---
    // irradianceの取得元(専用マップ or プリフィルタ済み鏡面の最終ミップ)の切り替えは
    // SampleEnvironmentの中で行う。プローブ側にもまったく同じ規則を適用するため、
    // IBLParams.xの判定はReflectionProbe.hlsliに1か所だけ置いている(14.10節・19.7節)
    // ラフネスを考慮したFresnel-Schlick(Lagarde, "Moving Frostbite to PBR")。粗い面ほど
    // 視線に対するフレネルの立ち上がりが緩やかになる近似で、鏡面に回らない分をkdへ反映する
    const float3 fresnelRoughness = F0 + (max(float3(1.0f - roughness, 1.0f - roughness, 1.0f - roughness), F0) - F0) * pow(saturate(1.0f - NdotV), 5.0f);
    const float3 kd = (1.0f - fresnelRoughness) * (1.0f - metallic);
    const float3 diffuseIBL = kd * albedo * irradiance;

    // --- 鏡面IBL(split-sum近似) ---
    // 「環境の放射輝度 × 係数」の形に分解しておく。SSRはこの放射輝度だけを差し替えるため、
    // 係数の定義はReflectionProbe.hlsliのSpecularIBLWeightに1か所だけ置いている(20章)。
    // LUTの第3成分(Eavg)はKulla-Conty方式だけが使うため.rgbで引く(14.9.2.1節)
    const float3 brdf = BRDFLUTTexture.Sample(ColorSampler, float2(NdotV, roughness)).rgb;
    const float3 specularWeight =
        SpecularIBLWeight(F0, NdotV, roughness, ao, brdf, ShadowParams.w, ShadowParams.z);
    // Kulla-Conty方式(ShadowParams.w = 3)が足す加算ローブ。プリフィルタ済み鏡面ではなく
    // 拡散イラディアンスに掛かるため、上の「放射輝度 × 係数」とは別の項として持つ。
    // 乗算型(1・2)と無効(0)ではこの係数が0になり、項ごと消える
    const float3 multiScatterWeight =
        SpecularIBLMultiScatterWeight(F0, NdotV, roughness, ao, brdf, ShadowParams.w, ShadowParams.z);

    // 【昼度(AmbientColor.a)による減衰はしない】かつては空が昼固定のスカイボックスから
    // 焼かれていたため、夜を表現する手段が「IBL全体を昼度で0倍する」ことしか無かった。
    // その結果、IBLを有効にすると夜の環境光が厳密にゼロになり、建物が真っ黒な影絵になっていた。
    // 現在は手続き空(SkyGenerate.hlsl)が太陽高度に応じて自分で暗くなり、夜は月明かりの
    // 空になるため、ここで追加の減衰を掛ける必要がない(掛けると二重に暗くなる。21.4節)。
    // 鏡面側のShadowParams.z(IBL強度倍率)はspecularWeightに含まれている
    return diffuseIBL * ao * ShadowParams.z
         + prefiltered * specularWeight
         + irradiance * multiScatterWeight;
}

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

// FrameConstantsのSky*フィールドからSky.hlsliのSkyParametersを組み立てる(P3)。
// SunDirectionの正規化はここで行う(SkyGenerate.hlsl側の慣習=呼び出し側でnormalizeする、に揃える)
SkyParameters MakeSkyParameters()
{
    SkyParameters params;
    params.SunDirection = normalize(SkySunDirection.xyz);
    // ティント4本と天頂輝度はP9でSkyParametersBuffer(t17)へ移った(SkyIntegrate.hlslが書く)
    params = ApplySkyParametersFromBuffer(params, SkyParametersBuffer[0]);
    // 雲(P5)。SSR.hlslのMakeSkyParametersと完全に同一の内容であること(このファイル冒頭の
    // コメントと同じ理由。背景に見える雲と水面に映る雲が食い違ってはいけない)
    params.CloudCoverage = CloudParams0.x;
    params.CloudAltitude = CloudParams0.y;
    params.CloudUvScale = CloudParams0.z;
    params.CloudDensity = CloudParams0.w;
    params.CloudScrollOffset = CloudParams1.xy;
    params.CloudForwardG = CloudParams1.z;
    // 巻雲(P11)。SSR.hlslのMakeSkyParametersと完全に同一の内容であること
    params.CirrusCoverage = CloudParams2.x;
    params.CirrusAltitude = CloudParams2.y;
    params.CirrusUvScale = CloudParams2.z;
    params.CirrusDensity = CloudParams2.w;
    params.CirrusScrollOffset = CloudParams3.xy;
    params.CirrusAnisotropy = CloudParams3.z;
    return params;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float depth = DepthTexture.Sample(DataSampler, input.UV).r;
    if (depth <= 0.0f)
    {
        // 何も描かれなかった背景ピクセル: カメラからそのピクセル方向への視線ベクトルを求める
        // Reverse-Zのため遠平面(=背景)はNDC z=0.0付近になる
        float3 farPoint = ReconstructWorldPos(input.UV, 0.0f);
        float3 rayDir = normalize(farPoint - CameraPosition.xyz);

        // SkyParams.y > 0.5fなら、キューブマップをサンプルする代わりにSky.hlsliのSkyColorを
        // 画面解像度で直接評価する。キューブマップは256px/面・ミップ無しのため、
        // 3840px・水平画角68度のカメラでは約20倍に拡大表示され、背景としては輪郭がぼける
        // (IBLとして使うぶんには畳み込むため低解像度のままで正しく、この解析評価は背景専用)。
        // 手続き空が無効(.ksceneのDDSスカイボックス使用時)はC++側でSkyParams.yを0にしてあるため、
        // ここでは値を見るだけでよい
        float3 skyColor;
        if (SkyParams.y > 0.5f)
        {
            const SkyParameters skyParams = MakeSkyParameters();
            skyColor = SkyColor(rayDir, skyParams);
        }
        else
        {
            // 手続き空は太陽高度に応じた明るさで焼かれている(夜は月明かりの空になる)ため、
            // かつてここで行っていた「夜は暗い紺色へlerpする」補正は不要になった
            skyColor = SkyboxTexture.Sample(MaterialSampler, rayDir).rgb;
        }
        return float4(skyColor, 1.0f);
    }

    float3 albedo = AlbedoTexture.Sample(ColorSampler, input.UV).rgb;
    float3 material = MaterialTexture.Sample(DataSampler, input.UV).rgb;
    float metallic = material.r;
    float roughness = material.g;
    // b = マテリアルの遮蔽マップ(GBuffer.hlslでstrength適用済み。遮蔽マップを持たない
    // マテリアルは1.0)。ベイク済みAOはスクリーンスペース手法が拾えない細部の遮蔽を持つ
    float materialAO = material.b;
    float3 diffuseColor = albedo * (1.0f - metallic);

    float3 worldPos = ReconstructWorldPos(input.UV, depth);
    float3 N = OctDecode(NormalTexture.Sample(DataSampler, input.UV).xy);
    float3 V = normalize(CameraPosition.xyz - worldPos);

    float4 aoSample = AOTexture.Sample(ColorSampler, input.UV);
    // スクリーンスペースの遮蔽(SSAO/SSIL)とマテリアルの遮蔽マップを乗算して合成する。
    // 両者は由来が独立(前者は実行時の周辺ジオメトリ、後者はアセットに焼かれた細部)なので、
    // 片方だけを採用するmin合成ではなく素直に積を取る。
    // 【重要】SSR.hlslは「Lightingパスが適用した鏡面IBLをまったく同じ式で再現する」設計のため、
    // この合成式を変える場合はSSR.hlsl側も必ず同時に合わせること(ズレると鏡面が二重計上/引きすぎになる)
    float ao = aoSample.a * materialAO;
    float3 indirectLight = aoSample.rgb; // SSIL(Visibility Bitmask)使用時のみ非ゼロ。周囲のサーフェスからの間接拡散光
    float3 directLight = DirectLightTexture.Sample(ColorSampler, input.UV).rgb; // DirectLighting.hlslで計算済み(シャドウ適用済み)
    float3 emissive = EmissiveTexture.Sample(ColorSampler, input.UV).rgb;

    // ShadowParams.z = IBL強度倍率(0以下ならEnable IBL無効)。無効時はIBL導入以前と同じ、
    // 定数色(昼夜サイクルで変化するAmbientColor.rgb)による簡易アンビエントにフォールバックする
    // (何もライティングしない真っ暗な状態にはしない)。
    // EvaluateIBL内のirradianceはIBLConvolve.hlsl側で1/πと積分のπを相殺済みなのでそのままでよいが、
    // このフォールバックの定数色AmbientColorはその正規化を受けていないため、DirectLighting.hlslの
    // 拡散反射(kd*albedo/PI)とスケールを揃えるべくここで明示的に/PIする
    // (以前このフォールバックだけ/PIが抜けており、環境光がπ倍(意図の20%に対し実際は約65%)
    // 明るくなっていた)
    // ProbeParams.y = 影響範囲のデバッグ表示。どのプローブがどれだけ効いているかを色で
    // 塗り分けて返す(ライティングは行わない)。プローブの配置・形状・ブレンド幅の確認用
    if (ProbeParams.y > 0.0f)
    {
        return float4(ProbeInfluenceDebugColor(worldPos), 1.0f);
    }

    float3 ambient;
    if (ShadowParams.z > 0.0f)
    {
        // 反射プローブ(19章)はEvaluateIBL内のSampleEnvironmentで環境ソースへ合成される。
        // プローブが1つも効いていない位置では従来どおりスカイボックス由来のグローバルIBLになる。
        // IBL強度倍率(ShadowParams.z)はEvaluateIBLの中で拡散・鏡面それぞれに掛かっている
        ambient = EvaluateIBL(N, V, worldPos, albedo, metallic, roughness, ao);
    }
    else
    {
        // IBL無効時のフォールバック。AmbientColor.rgbを「方向依存を持たない一様な環境」とみなす。
        // 一様な放射輝度Lの環境から受けるイラディアンスはE = PI * Lなので、上のコメントどおり
        // AmbientColor.rgbをイラディアンスE相当として扱うなら、環境の放射輝度はL = E / PIになる。
        // 拡散側の/PIと同じ量であり、拡散項の値は従来と厳密に一致する
        const float3 ambientRadiance = AmbientColor.rgb / PI;

        // 【鏡面項を0にしてはいけない】diffuseColor = albedo * (1 - metallic)のため、
        // 拡散だけにすると金属(metallic=1)の環境光が厳密に0になり真っ黒な影絵になる。
        // 低ラフネスの誘電体も環境のハイライトを完全に失う。
        // そこで一様な環境放射輝度に対してsplit-sum近似の第2項(BRDF積分LUT。方向性を持たない
        // (NdotV, ラフネス)のテーブルなのでIBLの有効/無効に関わらず使える)を掛けた鏡面を足す。
        // 半透明パス(Transparent.hlsl)は以前からこの形のフォールバックを持っており、
        // 不透明パスとプローブ焼き込みパスにだけ無かったものを揃えたもの
        const float NdotV = saturate(dot(N, V));
        const float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
        // LUTの第3成分(Eavg)はKulla-Conty方式だけが使うため.rgbで引く(EvaluateIBLと同じ)
        const float3 brdf = BRDFLUTTexture.Sample(ColorSampler, float2(NdotV, roughness)).rgb;
        const SpecularEnergyContext energy = MakeSpecularEnergyContext(F0, brdf, roughness, ShadowParams.w);
        const float3 fallbackFssEss = F0 * brdf.x + brdf.y;
        const float fallbackEss = brdf.x + brdf.y;

        // 定数色アンビエントはプリフィルタ済み鏡面・拡散イラディアンスの両方の代わりを兼ねるため、
        // Kulla-Contyの加算ローブにも同じambientRadianceを掛ける
        ambient = diffuseColor * ambientRadiance * ao
            + ambientRadiance
                * (fallbackFssEss * energy.Compensation
                   + SpecularMultiScatterIBL(F0, fallbackFssEss, fallbackEss, energy.Mode))
                * SpecularOcclusion(NdotV, roughness, ao);
    }

    // エミッシブは自発光のためAO/シャドウの影響を受けず常に加算する。SSILの間接拡散光も
    // 受光面のランバート反射(diffuseColor/PI、非金属分)として正規化してから加算する。
    // SSILの間接拡散光にはマテリアルの遮蔽マップを掛ける(これも間接光のため)。SSIL自身の
    // 遮蔽はaoSample.rgbの算出時点で織り込み済みなので、ここで掛けるのはmaterialAOだけでよい。
    // directLightとemissiveには掛けない(遮蔽マップは間接光にのみ効かせる方針。glTF仕様も同様)
    float3 color = ambient + (diffuseColor / PI) * indirectLight * materialAO + directLight + emissive;

    return float4(color, 1.0f);
}
