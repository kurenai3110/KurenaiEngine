// 直接光パス。G-Buffer(Albedo/Normal/Material/Depth)とシャドウマップから
// Cook-Torrance(GGX)のPBRで直接光(拡散+鏡面反射、シャドウ適用済み)だけを計算し、
// 専用のレンダーターゲットへ書き出す(環境光・間接光は含まない)。
// 太陽(平行光、b0、カスケードシャドウ付き)に加え、t8の構造化バッファに詰めたポイント/スポットライトを
// ループ加算する。ポイント/スポットの影はシャドウマップを増やさず、深度バッファをライト方向へ
// レイマーチするスクリーンスペースシャドウ(ScreenSpaceShadow.hlsli)で求める
// (詳細はdocs/Architecture.htmlの「複数ライト」「カスケードシャドウマップ」
// 「スクリーンスペースシャドウとタイルライトカリング」章を参照)。
// この結果はDeferredLightingパス(最終合成)とSSIL_VisibilityBitmask.hlsl(間接光サンプルの
// 簡易直接光の代わりに実際の直接光を使うことでシャドウも含めて正確にする)の両方からサンプルされる。
// レンダー解像度と同じ内部解像度で、HDR(トーンマップ前)の値をR32G32B32A32_Floatへ書き込む。
#include "NormalEncoding.hlsli"
// Smith可視性項とスペキュラのエネルギー補正。BRDF積分LUT(BRDFLUT.hlsl)と同じ可視性項を
// 使うことがエネルギー補正の前提になるため、定義を共有する
#include "SpecularEnergy.hlsli"

static const float PI = 3.14159265359f;

cbuffer FrameConstants : register(b0)
{
    float4x4 ViewProj;
    float4x4 InvViewProj;
    // カスケードシャドウマップ(CSM)のカスケードごとのライト視点ビュー・プロジェクション行列
    float4x4 CascadeViewProj[4];
    float4 CameraPosition;
    float4 LightDirection;
    float4 LightColor;
    float4x4 View;
    float4x4 Proj;
    float4 AmbientColor;
    // 各カスケードのView空間far距離(x=カスケード0, y=1, z=2, w=3)。ピクセルのView空間深度を
    // これと比較してどのカスケードのシャドウマップを使うか選ぶ
    float4 CascadeSplits;
    // x: PCSS(Percentage Closer Soft Shadows)のライトサイズ(シャドウマップUV空間の係数)。
    // ブロッカーサーチ・半影の広さの基準になる
    float4 ShadowParams;
    // 【宣言はここで止めている】このシェーダーが読むのはShadowParamsまでで、それより後ろは使わない。
    // C++側のFrameConstantsはこの後ろにTimeParams・Sky*・Cloud*・PlanarReflectionPlane・
    // Fog*・WaterBodyColorを持つが、cbufferは宣言順レイアウトなので、途中を飛ばして末尾だけを
    // 宣言すると誤ったオフセットを読む。しかもコンパイルは通り絵も「それらしく」出るため気付けない。
    // これらが必要になったら、C++の並びどおりに間のフィールドをすべて宣言すること
};

// ポイント/スポットライト1灯ぶんのデータ。C++側 KurenaiEngine3D.cpp の GPULight と
// 並び・ストライド(64バイト)を一致させる必要がある。既存の SSAOConstants/SSILConstants と同様、
// パッキング規則の解釈揺れを避けるためメンバはすべて float4 単位で宣言する
struct GPULight
{
    float4 PositionType;   // xyz=ワールド座標, w=LightType(0=Directional, 1=Point, 2=Spot)
    // rgb = Color * Intensity[cd] * exposure(EV100)。カンデラ→露出済みの最終放射輝度で、
    // CPU側(MakeGPULight)で計算してあるためシェーダ側はそのまま乗算するだけでよい
    float4 ColorRange;     // rgb=露出済み放射輝度, w=Range
    float4 DirectionAngle; // xyz=向き(正規化済み), w=spotAngleScale
    // x=spotAngleOffset, y=CastShadow(1でスクリーンスペースシャドウを撃つ / 0で撃たない),
    // zw=未使用(エリアライト用に予約)
    float4 Params;
};
StructuredBuffer<GPULight> Lights : register(t8);

// タイルライトカリング(LightCulling.hlsl)が書いたライトグリッド。レイアウトは
//   base = tileIndex * (1 + kMaxLightsPerTile)
//   [base + 0]     = そのタイルに届いたライト数(容量超過時はそのままの数が入るのでここで打ち切る)
//   [base + 1 + n] = n番目のライトの、Lights(t8)側でのインデックス
// LightCulling.hlsl側のkMaxLightsPerTile・C++側のkLightTileCapacityと必ず同じ値にすること
static const uint kMaxLightsPerTile = 64u;
StructuredBuffer<uint> LightTiles : register(t5);

// DirectLighting.hlsl側のこの宣言とC++側 KurenaiEngine3D.cpp の LightingConstants を一致させる必要がある。
// b0はFrameConstantsが使っており、RHIの定数バッファスロットは2本(DX12Sの kConstantBufferSlotCount = 2)
// しか無いため、このパス固有のパラメータはすべてこのb1へ足していく
cbuffer LightingConstants : register(b1)
{
    // x=有効ライト数, y=ピクセルあたりに撃つスクリーンスペースシャドウのレイ数の上限,
    // z=太陽の影の手法(KurenaiEngine3D::ShadowMode。0=なし, 1=カスケードシャドウマップ,
    //   2=レイトレーシング。2のときだけRTShadowTexture(t6)を読む), w=未使用
    uint4 LightCount;
    // スクリーンスペースシャドウ(ScreenSpaceShadow.hlsli)のパラメータ。
    // x=レイマーチのステップ数, y=最大レイ長(ワールド単位), z=遮蔽とみなす深度差の上限(thickness),
    // w=有効フラグ(0で無効)
    float4 SSSParams0;
    // x=深度リニアライズ定数a, y=同b(viewZ = b / (depth - a))、
    // z=レイ始点の法線方向への押し出し量(View空間深度に比例させる係数)、w=画面端フェード幅(UV)
    float4 SSSParams1;
    // タイルライトカリング(LightCulling.hlsl)のパラメータ。
    // x=タイル数X, y=タイルの1辺のピクセル数, z=1タイルあたりの容量, w=カリング有効フラグ(0で無効)
    uint4 TileParams;
};

Texture2D AlbedoTexture : register(t0);
Texture2D NormalTexture : register(t1);
Texture2D MaterialTexture : register(t2);
Texture2D DepthTexture : register(t3);
// カスケードシャドウマップ(t4のTexture2DArray)とそのPCSSサンプリング。
// FrameConstants(CascadeViewProj/CascadeSplits/ShadowParams)とDataSamplerを参照するため、
// それらの宣言より後でインクルードする必要がある
#include "ShadowSampling.hlsli"
// レイトレーシングシャドウ(RTShadow.hlsl)が書いた太陽の可視率(0=完全に影, 1=完全に光が当たる)。
// このパスと同じ解像度・同じピクセル格子なのでフィルタは不要で、常にLoadで1テクセルだけ読む。
// LightCount.zが2(Raytraced)のときだけ読まれるが、DX12はSetPipelineStateのたびに
// ルート引数が無効化されるため、シェーダが宣言しているリソースはモードによらず必ず
// バインドしなければならない(C++側は非対応環境では代わりに深度テクスチャを張る)
Texture2D RTShadowTexture : register(t6);
// ポイント/スポットライトのスクリーンスペースシャドウ。FrameConstants(ViewProj)・
// LightingConstants(SSSParams0/1)・DataSampler・DepthTextureを参照するため、それらより後でインクルードする
#include "ScreenSpaceShadow.hlsli"
// split-sum近似の第2項、BRDF積分LUT(x=NdotV, y=ラフネス。BRDFLUT.hlslで生成)。
// このパスはIBLを計算しないが、スペキュラのエネルギー補正(SpecularEnergy.hlsli、14.9節)で
// Ess = brdf.x + brdf.y を必要とするためバインドしている。
// t8はライトリスト(StructuredBuffer<GPULight>)が占有しているためt9に置く
Texture2D BRDFLUTTexture : register(t9);

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

float DistributionGGX(float NdotH, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0f) + 1.0f;
    return a2 / max(PI * d * d, 1e-6f);
}

// GeometrySchlickGGX / GeometrySmith はSpecularEnergy.hlsliの共有定義を使う
// (**Disneyのラフネス再マップ k=(roughness+1)^2/8 をここへ足してはいけない**。理由は同ヘッダー参照)

float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0f - F0) * pow(saturate(1.0f - cosTheta), 5.0f);
}

// SpecularEnergyContext(スペキュラのエネルギー補正のうちピクセル内で一定な量)は
// SpecularEnergy.hlsliの共有定義を使う。

// Cook-Torrance を1灯ぶん評価する(シャドウ・ライト色・減衰は呼び出し側で乗算する)。
// 太陽(b0)とポイント/スポットライト(t8)の両方から共通で呼ばれる。
float3 EvaluateDirectBRDF(
    float3 N, float3 V, float3 L, float NdotV, float3 albedo, float metallic, float roughness,
    SpecularEnergyContext energy)
{
    float3 H = normalize(V + L);
    float NdotL = saturate(dot(N, L));
    float NdotH = saturate(dot(N, H));
    float VdotH = saturate(dot(V, H));

    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
    float D = DistributionGGX(NdotH, roughness);
    float G = GeometrySmith(NdotV, NdotL, roughness);
    float3 F = FresnelSchlick(VdotH, F0);

    // 補正は鏡面項にのみ掛ける(拡散項kdは変更しない。理由は14.9節)
    float3 specular = (D * G * F) / max(4.0f * NdotV * NdotL, 1e-4f) * energy.Compensation;

    if (energy.Mode == KURENAI_SPEC_COMP_KULLACONTY)
    {
        // 加算ローブはE(NdotL)を要る。ライトのループ内から呼ばれるため、勾配に依存しない
        // SampleLevelを使う(Sampleは動的な分岐・ループ内で勾配が未定義になり得る)
        const float2 brdfL = BRDFLUTTexture.SampleLevel(ColorSampler, float2(NdotL, energy.Roughness), 0).rg;
        specular += SpecularMultiScatterLobe(F0, energy.EssV, brdfL.x + brdfL.y, energy.Eavg, energy.Mode);
    }

    float3 kd = (1.0f - F) * (1.0f - metallic);
    float3 diffuse = kd * albedo / PI;

    return (diffuse + specular) * NdotL;
}

// 透過項に掛ける遮蔽の下限を決める係数。透過率へ掛けたものが「完全な影でも残る割合」になる
// (透過率0.55なら下限0.33)。葉が重なった樹冠を単発のシャドウ問い合わせで表すための近似で、
// 本来は多重散乱で解くべきところ。上げるほど樹冠の内側が明るく平坦になる
static const float kTranslucencyShadowFloor = 0.6f;

// 薄いものの透過(translucency)を1灯ぶん評価する。シャドウ・ライト色は呼び出し側で乗算する。
//
// 【何のためにあるか】葉・花弁・紙のように薄いものは、裏から当たった光を透かして
// 表側が明るく見える。Cook-Torranceは NdotL<=0 の面を真っ黒にするため、そのままでは
// 逆光の樹冠が空の環境光だけで照らされ、青灰色に沈む(45章)。
//
// 【物理的な位置づけ】厚みを持つ媒質の散乱を解くのではなく、
// 「薄い両面の被写体」を近似する定番の形(いわゆる wrap / back-lit translucency)。
// 拡散のみで鏡面は持たない ―― 透過してきた光は媒質内で散乱しきっており、
// 表面反射のローブを作らないため。
float3 EvaluateTranslucency(float3 N, float3 V, float3 L, float3 albedo, float translucency)
{
    if (translucency <= 0.0f)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    // 裏面がどれだけ光を受けているか。表(N)の裏返し(-N)で測る
    float backNdotL = saturate(dot(-N, L));

    // 前方散乱。光の進行方向(-L)を見込む角度ほど強く透ける(葉を太陽にかざしたときの見え方)。
    // 全周に一定量を残すのは、散乱しきった成分がどの方向にも一様に抜けるため
    float forward = saturate(dot(V, -L));
    float lobe = 0.35f + 0.65f * (forward * forward * forward);

    // アルベドを掛けるのは、透けてくる光が媒質の色に染まるため(白い花弁は白く、葉は緑に透ける)。
    // 1/PIは拡散項と同じ正規化
    return albedo * (translucency * backNdotL * lobe / PI);
}

// Karis 2013 / Frostbite の windowed inverse-square。Range を超えると厳密に0になり、
// 打ち切り境界でのハードエッジが出ない
float DistanceAttenuation(float distSq, float range)
{
    float factor = distSq / max(range * range, 1e-4f); // (d/r)^2
    float window = saturate(1.0f - factor * factor);   // 1 - (d/r)^4
    // 光源に極端に近づいたときの発散を抑える。定数1.0を足す実装はシーンスケール依存になるため、
    // 最小距離二乗でのクランプにする
    return (window * window) / max(distSq, 0.0001f);
}

// Frostbite の lightAngleScale / lightAngleOffset。CPU側(MakeGPULight)で事前計算した値を
// GPULight.DirectionAngle.w / Params.x として受け取る
//   scale  = 1 / max(0.001, cos(inner) - cos(outer))
//   offset = -cos(outer) * scale
float SpotAttenuation(float3 spotDirection, float3 L, float angleScale, float angleOffset)
{
    float t = saturate(dot(spotDirection, -L) * angleScale + angleOffset);
    return t * t;
}

// t8のライトリストを1灯ぶん評価する。early-outは効きの強い順(距離→減衰→スポット円錐→NdotL)に並べる。
// 影はシャドウマップではなくスクリーンスペースシャドウ(ScreenSpaceShadow.hlsli)で求める。
//
// shadowRayBudgetは「このピクセルで残り何本シャドウレイを撃ってよいか」。ライト数が増えても
// ピクセルあたりのレイマーチ回数が青天井にならないよう、呼び出し側(PSMain)で上限を渡して
// ここで消費していく。early-outをすべて通過した後にだけ消費するので、
// そのピクセルに実際に届いているライトから順に予算が使われる
float3 EvaluateLight(
    GPULight light, float3 worldPos, float3 N, float3 V, float NdotV, float3 albedo, float metallic, float roughness,
    float translucency, SpecularEnergyContext energy, float2 pixelCoord, inout uint shadowRayBudget)
{
    uint lightType = (uint)light.PositionType.w;
    float range = light.ColorRange.w;

    float3 L;
    float atten = 1.0f;
    // 平行光は光源が無限遠にあるため、レイ長は常に最大レイ長(SSSParams0.y)側で決まる
    float distanceToLight = 1e30f;

    if (lightType == 0u) // Directional
    {
        L = normalize(-light.DirectionAngle.xyz);
    }
    else
    {
        float3 toLight = light.PositionType.xyz - worldPos;
        float distSq = dot(toLight, toLight);
        if (distSq > range * range)
        {
            return float3(0.0f, 0.0f, 0.0f);
        }

        atten = DistanceAttenuation(distSq, range);
        if (atten <= 0.0f)
        {
            return float3(0.0f, 0.0f, 0.0f);
        }

        float dist = sqrt(max(distSq, 1e-16f));
        distanceToLight = dist;
        L = toLight / dist;

        if (lightType == 2u) // Spot
        {
            float spotAtten = SpotAttenuation(light.DirectionAngle.xyz, L, light.DirectionAngle.w, light.Params.x);
            if (spotAtten <= 0.0f)
            {
                return float3(0.0f, 0.0f, 0.0f);
            }
            atten *= spotAtten;
        }
    }

    // 【透過するマテリアルではNdotL<=0でも打ち切らない】薄いものは裏から当たった光を
    // 透かすため、その側にこそ寄与がある(EvaluateTranslucency参照)。
    // 透過しないマテリアル(translucency=0)は従来どおりここで打ち切る
    if (dot(N, L) <= 0.0f && translucency <= 0.0f)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    // ここまで来たライトだけが実際にこのピクセルを照らす。予算が残っていて、かつ
    // そのライトが影を落とす設定(Params.y)ならレイマーチする
    float shadow = 1.0f;
    if (shadowRayBudget > 0u && light.Params.y > 0.5f)
    {
        shadow = ComputeScreenSpaceShadow(worldPos, N, L, distanceToLight, pixelCoord);
        shadowRayBudget -= 1u;
    }

    const float3 reflected = EvaluateDirectBRDF(N, V, L, NdotV, albedo, metallic, roughness, energy);
    // 透過側の遮蔽は太陽と同じ扱い(遮蔽側も光を通すぶんを下限として残す)
    const float transmissionShadow = lerp(saturate(translucency * kTranslucencyShadowFloor), 1.0f, shadow);
    const float3 transmitted = EvaluateTranslucency(N, V, L, albedo, translucency) * transmissionShadow;
    return reflected * shadow * light.ColorRange.rgb * atten + transmitted * light.ColorRange.rgb * atten;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float depth = DepthTexture.Sample(DataSampler, input.UV).r;
    if (depth <= 0.0f)
    {
        // 背景(スカイ)には直接光はない(スカイボックス自体はDeferredLightingパス側で表示する)
        // Reverse-Zのため遠平面(=背景)はNDC z=0.0付近になる
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    float3 worldPos = ReconstructWorldPos(input.UV, depth);
    // aチャンネルは透過率(GBuffer.hlslが書く)。0なら従来どおりの不透明な陰影になる
    float4 albedoSample = AlbedoTexture.Sample(ColorSampler, input.UV);
    float3 albedo = albedoSample.rgb;
    float translucency = albedoSample.a;
    float3 N = OctDecode(NormalTexture.Sample(DataSampler, input.UV).xy);
    float2 material = MaterialTexture.Sample(DataSampler, input.UV).rg;
    float metallic = material.r;
    float roughness = material.g;

    float3 V = normalize(CameraPosition.xyz - worldPos);
    float NdotV = saturate(dot(N, V)) + 1e-5f;

    // スペキュラのエネルギー補正(SpecularEnergy.hlsli、14.9節)。Ess=(NdotV, ラフネス)だけの
    // 関数でピクセル内では一定なので、太陽・ライトリストのループへ入る前に1度だけ求める。
    // F0のlerpはEvaluateDirectBRDF内と同じ式(この式はコードベース内の複数箇所に登場するため
    // ここだけ引数化して特別扱いはしない)
    const float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
    const float3 brdf = BRDFLUTTexture.Sample(ColorSampler, float2(NdotV, roughness)).rgb;
    const SpecularEnergyContext energy = MakeSpecularEnergyContext(F0, brdf, roughness, ShadowParams.w);

    float3 directLight = float3(0.0f, 0.0f, 0.0f);

    // --- 太陽(b0、カスケードシャドウ付き) ---
    // ここでNdotL<=0のとき関数全体を打ち切ってはいけない。
    // 太陽の寄与だけをこのブロックに閉じ込め、その後は必ずライトリストのループへ進む
    float3 sunL = normalize(-LightDirection.xyz);
    float sunNdotL = saturate(dot(N, sunL));
    // 【透過はNdotL<=0の側で効く】葉や花弁は薄いので、裏から当たった光が透けて表側を光らせる。
    // 不透明体としての寄与(上のsunNdotL>0)とは排他なので、シャドウの取得だけ共有できるよう
    // ここで先に影の可視率を求めておく
    const bool needSunShadow = (sunNdotL > 0.0f) || (translucency > 0.0f);
    float sunShadow = 1.0f;
    if (needSunShadow)
    {
        if (LightCount.z == 2u) // Raytraced
        {
            // RTShadow.hlslが同じ解像度・同じピクセル格子へ書いた可視率をそのまま使う
            sunShadow = RTShadowTexture.Load(int3((int2)input.Position.xy, 0)).r;
        }
        else
        {
            // Off(0)のときもここを通る。シャドウパスがシャドウマップを最遠(深度1.0)へ
            // クリアしたまま何も描かないため、ComputeShadowFactorの深度比較が常に
            // 「影なし」と判定して1.0を返す(シャドウパスのClearDepth参照)
            float viewDepth = mul(float4(worldPos, 1.0f), View).z;
            // 【透過側ではNdotLを0で渡す】ComputeCascadedShadowFactorはNdotLを
            // 法線オフセット(シャドウアクネ対策)に使う。裏から照らされている面では
            // sunNdotLが0なのでオフセットは掛からず、面そのものの深度で比較される
            sunShadow = ComputeCascadedShadowFactor(worldPos, viewDepth, sunNdotL);
        }

        if (sunNdotL > 0.0f)
        {
            directLight += EvaluateDirectBRDF(N, V, sunL, NdotV, albedo, metallic, roughness, energy)
                * LightColor.rgb * sunShadow;
        }

        // 【透過には不透明体の影をそのまま掛けない】シャドウは「不透明な何かに遮られたか」しか
        // 答えないが、透過するものが重なっている場合は遮蔽側も光を通す。そのまま掛けると、
        // 密な樹冠では1枚重なった時点で透過が完全に消える
        // (実測: 樹冠の平均が (109, 109, 120) → (75, 82, 98) まで落ち、透過を入れた意味が無くなる)。
        // 遮蔽側の透過ぶんを下限として残す。下限をマテリアル自身の透過率から作るのは、
        // よく透けるものほど「隣も同じだけ透ける」ため。不透明(translucency=0)なら
        // 下限も0になり、従来どおり完全な影になる
        const float transmissionShadow = lerp(saturate(translucency * kTranslucencyShadowFloor), 1.0f, sunShadow);
        directLight += EvaluateTranslucency(N, V, sunL, albedo, translucency) * LightColor.rgb * transmissionShadow;
    }

    // --- t8のライトリスト(スクリーンスペースシャドウ付き) ---
    // ピクセルあたりのシャドウレイ数の上限。ライトを増やしてもレイマーチのコストが
    // 線形に伸び続けないようにするための予算で、EvaluateLightが消費する
    uint shadowRayBudget = LightCount.y;

    // タイルライトカリング有効時は、このピクセルが属するタイルのリストだけをループする。
    // 無効時は従来どおり全ライトを回す(カリングの有無で結果が変わらないことの検証に使う)
    if (TileParams.w != 0u)
    {
        const uint2 tileCoord = uint2(input.Position.xy) / TileParams.y;
        const uint tileBase = (tileCoord.y * TileParams.x + tileCoord.x) * (1u + TileParams.z);
        // カリング側は容量を超えた数もそのまま書く(デバッグ表示が超過を検出できるように)ため、
        // 読み手のここで実際に格納されている件数まで打ち切る
        const uint tileLightCount = min(LightTiles[tileBase], kMaxLightsPerTile);

        [loop]
        for (uint t = 0; t < tileLightCount; ++t)
        {
            directLight += EvaluateLight(
                Lights[LightTiles[tileBase + 1u + t]], worldPos, N, V, NdotV, albedo, metallic, roughness,
                translucency, energy, input.Position.xy, shadowRayBudget);
        }
    }
    else
    {
        [loop]
        for (uint i = 0; i < LightCount.x; ++i)
        {
            directLight += EvaluateLight(
                Lights[i], worldPos, N, V, NdotV, albedo, metallic, roughness, translucency, energy,
                input.Position.xy, shadowRayBudget);
        }
    }

    return float4(directLight, 1.0f);
}
