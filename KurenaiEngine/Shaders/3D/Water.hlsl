// 水面(Water.kmodel)専用のG-Bufferピクセルシェーダー(水面マテリアル基盤)。
// 頂点シェーダー(VSMain)・入出力構造体・cbuffer宣言はGBuffer.hlslと共有するため
// GBufferCommon.hlsliにある。ここでは「波打つ低ラフネス水面」に見せるための法線合成と、
// 水面であることを示すマテリアルID(kMaterialIDWater)の書き込みだけを担当する。
// 反射(SSR統合)はこのシェーダーでは行わない(SSR.hlslの水面分岐が扱う)。影はShadow.hlslの通常の深度のみパスに乗せる
// (他の不透明メッシュと同じ扱いで、このファイルでは何もしない)
#include "GBufferCommon.hlsli"

// 水面法線マップ(タイル可能な接線空間法線、Assets/Packed/MontSaintMichelStudy/WaterNormal.png)。
// t4はTransparent.hlsl/ProbeCapture.hlslがカスケードシャドウマップに、t5はOcclusionTextureに、
// **t6はGBuffer.hlslのbent normal(34章)** に使っているため、次に空いているt7を使う。
// [Water]NormalMapが空文字列のシーンではC++側(KurenaiEngine3D)が1x1のフラット法線
// (128,128,255,255)をここへバインドする
Texture2D WaterNormalTexture : register(t7);

// UDN(Unity風のDerivative Normal)ブレンド用、2層のUVスケール・スクロール方向・速度倍率。
// Scene::WaterWaveScale/WaterWaveStrengthはFrameConstants.TimeParams.y/zとして渡り、下の
// PSMainでこれらの定数へ掛け合わされる(UVスケールへの倍率・波振幅への倍率)。
// 層ごとの相対的なスケール比・スクロール方向・速度比そのものは実測で選んだ固定値のまま
// (将来ObjectConstants/FrameConstantsへ層ごとに個別のパラメータを追加する際は、
// この定数をそちらへ差し替えること)
static const float2 kWaterLayerAUvScale = float2(1.0f, 1.0f);
static const float2 kWaterLayerAScrollDir = float2(1.0f, 0.4f);
static const float kWaterLayerASpeedScale = 1.0f;

static const float2 kWaterLayerBUvScale = float2(2.37f, 2.37f);
static const float2 kWaterLayerBScrollDir = float2(-0.6f, 1.0f);
static const float kWaterLayerBSpeedScale = 1.7f;

// 波の振幅(法線の揺らぎの強さ)をカメラからの距離に応じてexp(-dist/k)で減衰させ、
// 遠方ではジオメトリ法線(タンジェント空間で(0,0,1)、合成前の状態)へ寄せる。
// 高周波の法線をそのまま地平線際まで残すとちらつき(エイリアシング)が目立つための対策。
// kは地平線際のエイリアシング対策の効き具合を決める値で、実測で調整可能
static const float kWaterDistanceFalloff = 200.0f;

PSOutput PSMain(PSInput input)
{
    // baseColor/AlphaCutoffの扱いはGBuffer.hlslのPSMainと同じ
    float4 baseColorSample = BaseColorTexture.Sample(MaterialSampler, input.UV) * BaseColorFactor;
    clip(baseColorSample.a - AlphaCutoff);

    float3 geometricNormal = normalize(input.Normal);
    float3x3 tbn = ComputeTangentFrame(geometricNormal, input.Tangent);

    // 2層の法線マップを異なるUVスケール・スクロール方向でサンプルする。TimeParams.xは
    // CPU側で既に[0,1)へfmod済みのため、ここで層ごとの速度倍率を掛けてfracし直しても
    // (どちらも有界な値どうしの掛け算・剰余なので)精度が失われず安全。
    // TimeParams.y(m_WaterWaveScale、既定12.0)をUVスケールに掛けているのは、水面メッシュの
    // UVが「ワールド20mあたり1タイル」で焼かれているため(Tools/generate_water_plane.py参照)、
    // その上でさらに何回波紋を繰り返すかを決める倍率として使うため
    float2 uvA = input.UV * kWaterLayerAUvScale * TimeParams.y + frac(TimeParams.x * kWaterLayerASpeedScale) * kWaterLayerAScrollDir;
    float2 uvB = input.UV * kWaterLayerBUvScale * TimeParams.y + frac(TimeParams.x * kWaterLayerBSpeedScale) * kWaterLayerBScrollDir;

    float3 normalSampleA = WaterNormalTexture.Sample(MaterialSampler, uvA).xyz * 2.0f - 1.0f;
    float3 normalSampleB = WaterNormalTexture.Sample(MaterialSampler, uvB).xyz * 2.0f - 1.0f;

    // UDN(Derivative Normal)ブレンド。2つの接線空間法線のxy(傾き成分)だけを足し合わせ、
    // zは正規化で再構成する。normalize(lerp(n1, n2, 0.5))で単純に混ぜる方式と違い、
    // 高周波の凹凸が打ち消し合って潰れにくいため、細かい波紋の重なりの表現に向く
    float3 blendedTangentNormal = normalize(float3(normalSampleA.xy + normalSampleB.xy, normalSampleA.z));

    // カメラからの距離が遠いほど波の振幅を弱め、ジオメトリ法線(タンジェント空間で(0,0,1))へ寄せる。
    // TimeParams.z(m_WaterWaveStrength、既定0.25)をsaturateして掛けているのは、下のlerpが
    // 「ジオメトリ法線と波の法線を0〜1の範囲で混ぜる」式であり、UIのスライダー自体は0〜1だが
    // 万一1を超える値が渡ってきてもweightが0〜1の範囲を超えないようにするため
    float distanceToCamera = length(CameraPosition.xyz - input.WorldPos);
    float waveWeight = exp(-distanceToCamera / kWaterDistanceFalloff) * saturate(TimeParams.z);
    float3 tangentNormal = normalize(lerp(float3(0.0f, 0.0f, 1.0f), blendedTangentNormal, waveWeight));

    float3 N = normalize(mul(tangentNormal, tbn));

    // roughness/metallicはメッシュ自身のRoughnessFactor/MetallicFactor(+テクスチャ)を
    // そのまま使う。水面専用の上書きはしない(GBuffer.hlslのPSMainと同じ式)
    float3 metallicRoughnessSample = MetallicRoughnessTexture.Sample(MaterialSampler, input.UV).rgb;
    float metallic = saturate(MetallicFactor * metallicRoughnessSample.b);
    float roughnessFactor = (RoughnessFactor < 0.0f) ? 1.0f : RoughnessFactor;
    float roughness = clamp(roughnessFactor * metallicRoughnessSample.g, 0.045f, 1.0f);

    float3 emissive = EmissiveTexture.Sample(MaterialSampler, input.UV).rgb * EmissiveFactor;

    // モーションベクターの求め方はGBuffer.hlslのPSMainと同じ(GBufferCommon.hlsliのコメント参照)
    float2 currentUv = ClipToUv(input.CurClip) - TAAParams.xy;
    float2 previousUv = ClipToUv(input.PrevClip) - TAAParams.zw;

    float occlusionSample = OcclusionTexture.Sample(MaterialSampler, input.LightmapUV).r;
    float ao = lerp(1.0f, occlusionSample, OcclusionStrength);

    PSOutput output;
    // 水中項。メッシュ自身のbaseColorSample.rgb(誘電体でほぼ黒に焼かれている)は使わず、
    // WaterBodyColor.rgb(FrameConstants、UIから調整可能)を出力Albedoに使う。
    // これは水体で拡散的に後方散乱して戻ってくる光の粗い近似であり、屈折・水深依存の減衰は
    // 含んでいない(G-Bufferが水深の情報を持たないため一定色にしている)。
    // 拡散項として書くことで、Fresnelによる「見下ろすと水の色/すれすれだと鏡」の切り替わりは
    // 既存のDeferredLighting.hlslの式がそのまま担当する(水面専用の分岐は増やさない)。
    // baseColorSample.aによるアルファカットアウトはこの置き換えとは無関係にそのまま残す(直前のclip参照)
    // aチャンネルは透過率(GBuffer.hlslと共有するPSOutputの規約)。
    // 水面は葉のような薄い透過体ではなく専用の経路で扱うため、0(透過なし)を書く。
    // **書き残してはいけない** ―― この構造体はGBuffer.hlslと共有しており、
    // 書かないとそのチャンネルの内容が未定義になる
    output.Albedo = float4(WaterBodyColor.rgb, 0.0f);
    output.Normal = OctEncode(N);
    // aチャンネルに水面のマテリアルIDを書く(GBuffer.hlslの通常マテリアルは0.0fのまま)。
    // DebugView::WaterMask(Present.hlsl Mode 17)がこの値をそのままグレースケール表示する
    output.Material = float4(metallic, roughness, ao, kMaterialIDWater);
    output.Emissive = float4(emissive, 1.0f);
    output.Velocity = currentUv - previousUv;
    // bent normal(34章)。水面はオフラインで遮蔽を焼いていないため「データ無し」を意味する
    // 有効フラグ0を書く。消費側(DeferredLighting.hlsl/SSR.hlslのDecodeBentOcclusion)は
    // このとき従来の遮蔽の経路へ落ちる。
    // 【書き残してはいけない】PSOutputはGBuffer.hlslと共有しており、書かないと
    // そのレンダーターゲットの内容が未定義になる(前フレームの残骸が読まれうる)
    output.BentNormal = float4(0.0f, 0.0f, 0.0f, 0.0f);
    return output;
}
