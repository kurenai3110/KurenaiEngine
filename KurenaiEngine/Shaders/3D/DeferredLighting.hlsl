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
// 空モデル(Perez分布)の共有ヘッダー。背景画素をSkyGenerate.hlslのキューブマップと
// 同じ関数・同じパラメータで画面解像度評価するために使う。PIを定義しないため、
// このファイルのPI定義(直後)より前でも後でもインクルード順は問題ない
// SkyView LUT。日中の空はこのLUTを引く。**定義しないと日中の空が黒くなる**ので、
// SkyColorUpperUnitを呼ぶシェーダーは全員定義すること(Sky.hlsliのSkyViewセクション参照)
#define KURENAI_SKYVIEW_REGISTER t20
// 【雲の3Dノイズ(KURENAI_CLOUD_SHAPE/DETAIL_REGISTER)とウェザーマップをここで定義しない理由】
// このシェーダーは雲を自分で評価しない。雲はSkyCloud.hlsl(低解像度の専用パス)が評価し、
// 結果を「透過率 + 事前乗算済み散乱光」としてSkyCloudTexture(t18)から、
// 霞の補正に使うfogInFrontをSkyCloudFogTexture(t22)から受け取る(PSMain参照)。
// マクロを定義しないことでSky.hlsli側の3Dテクスチャ宣言が消え、t18/t19が空く。
// このシェーダーはt0〜t22を使い切っている(RHIのkTextureSlotCount=23)ため、
// この2枠の解放がそのままSkyCloudTextureの置き場所になっている

#include "Sky.hlsli"

static const float PI = 3.14159265359f;

// 反射プローブの環境ソースと鏡面IBLの重み。SSR.hlslが同じ定義を共有する(20章)。
// ReflectionProbe.hlsliはSamplers.hlsliのMaterialSamplerとFrameConstantsの
// ProbeParams/ShadowParams/AmbientColorを参照するため、それらの宣言より後でインクルードする
#define KURENAI_GLOBAL_IRRADIANCE_REGISTER t8
#define KURENAI_GLOBAL_PREFILTERED_REGISTER t9
// 反射プローブは鏡面専任なので拡散イラディアンス用のスロットは持たない
// (ReflectionProbe.hlsli冒頭のコメント参照)
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
    // **昼度をIBLの減衰に使ってはいけない**。手続き空(SkyGenerate.hlsl)は太陽高度に応じて
    // 空自体が暗くなるため、ここで掛けると二重に暗くなる(21.4節)。
    // Enable IBL無効時はrgbをそのまま定数色アンビエントとして使う(PSMain参照)
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
    // DDGIのクリップマップLOD(31.4.2節)。**要素数はC++側のkDDGIMaxLODCountと一致させること。**
    // 読むのはDDGI.hlsliだけだが、cbufferは宣言順でオフセットが決まるため、
    // DDGIParams4の後ろのフィールドを読むシェーダーはすべてここへ同じ宣言が要る
    // (飛ばすと以降のフィールドが64バイトずれ、コンパイルは通るのに別の値を読む)
    float4 DDGILODOrigin[4];
    float4 DDGILODBase[4];
    // bent normalによる遮蔽(34章)。x=ディフューズAOの出所、y=スペキュラ遮蔽の方式、
    // z=multi-bounce AO、w=未使用。
    // 【この位置を動かさないこと】C++側のFrameConstantsではDDGIParams4の直後に置いてある。
    // cbufferは宣言順レイアウトなので、以降のフィールドのオフセットがすべてずれる
    float4 OcclusionParams;
    // 水面用。このシェーダでは未使用だが、C++側でSkySunDirection等より手前に置かれているため
    // オフセット合わせのためだけに宣言する
    float4 TimeParams;
    // 空の解析評価用(末尾に追加)。背景(深度が無い画素)を、キューブマップのサンプルではなく
    // Sky.hlsliのSkyColorを画面解像度で直接評価するために使う。キューブマップは256px/面・
    // ミップ無しで、3840px・水平画角68度のカメラでは約20倍に拡大表示されるため、画面解像度で
    // 評価したほうが背景の輪郭がシャープになる(IBLは畳み込むため低解像度のままで正しい)。
    // 値はSkyGenerate.hlslが焼くキューブマップに使ったものと同一
    // (m_SkyParametersBuffer参照。ティント・天頂輝度はこのcbufferではなく
    // StructuredBuffer<GPUSkyParameters>(t17)にある)。
    // xyz=太陽が「ある」向き(未正規化のまま渡ってくる。PSMain側でnormalizeする。
    // SkyGenerate.hlsl側の慣習=呼び出し側でnormalizeする、に合わせてある)、w=未使用
    float4 SkySunDirection;
    // x=未使用(天頂輝度はSkyParametersBufferにある)、y=背景を解析評価するかのフラグ
    // (1=解析、0=キューブマップをサンプル。手続き空が無効なときは常に0)、
    // z=太陽照度/空照度比(SunToSkyIlluminanceRatio。MakeSkyParametersが読み、
    // Sky.hlsliのEvaluateCloudLayerが雲の明るさを太陽照度基準にするために使う)、w=未使用
    float4 SkyParams;
    // 雲(さらに末尾に追加)。SSR.hlslの同名フィールドと完全に同じ順・同じ型であること
    // (C++側 KurenaiEngine3D.cpp の FrameConstants::CloudParams0/1 と揃える。ずれると
    // 背景に見える雲と水面に映る雲が食い違う)。
    // CloudParams0: x=被覆率(0で雲なし。Sky.hlsliのSkyColorが早期脱出する)、
    //               y=雲底の高度[m](カメラ基準)、z=UVスケール[ノイズ空間/m]、w=消散係数
    float4 CloudParams0;
    // CloudParams1: xy=風によるノイズ空間の移動量(CPU側でSky.hlsliのkCloudNoisePeriodと
    //               同じ周期でwrap済み)、z=Henyey-Greensteinの非対称パラメータ、w=未使用
    float4 CloudParams1;
    // 巻雲(さらに末尾に追加)。SSR.hlsl/PlanarReflection.hlslの同名フィールドと完全に
    // 同じ順・同じ型であること(C++側 KurenaiEngine3D.cpp の FrameConstants::CloudParams2/3 と揃える)。
    // CloudParams2: x=巻雲の被覆率(0で巻雲なし)、y=雲底の高度[m](カメラ基準)、
    //               z=UVスケール[ノイズ空間/m]、w=消散係数
    float4 CloudParams2;
    // CloudParams3: xy=風によるノイズ空間の移動量(積雲と同じくkCloudNoisePeriodでwrap済み)、
    //               z=fBmのUV(U方向)を伸ばす異方性スケール、w=未使用
    float4 CloudParams3;
    // 平面反射。このシェーダでは未使用(オフセット合わせのためだけに宣言する)
    float4 PlanarReflectionPlane;
    // 大気遠近(末尾に追加)。このシェーダでは未使用だが、C++側 KurenaiEngine3D.cpp の
    // FrameConstantsと並びを一致させる目的だけで宣言する
    // (AerialPerspective.hlsl/PlanarReflection.hlslが読む)
    float4 FogParams0;
    float4 FogParams1;
    // 水中項。このシェーダでは未使用(オフセット合わせのためだけに宣言する)。Water.hlslが読む
    float4 WaterBodyColor;
    // 星空(末尾に追加)。x=強度(0で無効。昼はCPU側が0にする)、y=密度、z=またたき、
    // w=1画素が張る角度[rad]。C++側 KurenaiEngine3D.cpp の FrameConstants::StarsParams と揃えること
    float4 StarsParams;
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
// bent normal(GBuffer.hlslがSV_TARGET5へ書いたワールド空間のbRaw)。
// t12〜t14は反射プローブ、t15・t16はDDGI(22章)が使用中のためt17(34章)
Texture2D BentNormalTexture : register(t17);
// split-sum近似の第2項、BRDF積分LUT(x=NdotV, y=ラフネス。BRDFLUT.hlslで生成、方向性を持たない
// (NdotV, ラフネス)の2Dルックアップテーブルのため、これだけは通常のTexture2Dのまま)
Texture2D BRDFLUTTexture : register(t10);
// SkyIntegrate.hlslが書いた空パラメータ。ティント4本と正規化済みの天頂輝度が入る。
// t11を使う(t17はbent normal(34章)、反射プローブは鏡面専任で拡散側のスロットを持たない)
StructuredBuffer<GPUSkyParameters> SkyParametersBuffer : register(t11);
// SkyCloud.hlsl(低解像度の雲パス)の出力。rgb=事前乗算済みの散乱光、a=透過率。
// 雲の3Dノイズを自前で引かなくなったことで空いたt18に置いている(このファイル冒頭のコメント参照)
Texture2D SkyCloudTexture : register(t18);
// DDGIResolve.hlsl(低解像度のDDGIパス)の出力。rgb=イラディアンス、a=insideWeight。
// DDGIParams4.yが1のときだけ読み、0のときはこのファイル内でSampleDDGIIrradianceを直接呼ぶ
// (既定は0=直接呼ぶ。低解像度化は深度をまたぐ滲みを伴う近似のため。DDGIResolve.hlsl冒頭参照)。
// t19はスカイクラウド分離でt18/t19が空いたうちの残り1枠(このファイル冒頭のコメント参照)
Texture2D DDGIResolveTexture : register(t19);
// DDGIResolve.hlslがSV_TARGET1へ書いた「そのテクセルが代表している全解像度の深度」(41.24節)。
// 【なぜ全解像度の深度(t3)から引き直さないのか】引き直しても同じ値になる ―― 向こうも
// DataSampler(ポイント)で同じUVを引いている ―― が、4テクセルぶんとなると全解像度側は
// 2テクセルおきの位置になり1回のGatherにまとまらない。低解像度で持っておけば
// 隣り合う4テクセルなのでGatherRed 1回で済む(UpsampleDDGI参照)
Texture2D DDGIResolveDepthTexture : register(t21);
// SkyCloud.hlslがSV_TARGET1へ書いた fogInFront(雲に最初に当たった位置の霞の透過率)。
// 雲の手前の霞の色を直す補正(P18b。Sky.hlsliのCloudAirlightCorrection参照)にだけ使う。
// 【なぜSkyCloudTextureのaに同居できないのか】aには既に雲の透過率が入っている。
// 補正は clearColor * (CloudSkyLight - 1) * (1 - fogInFront) で、CloudSkyLightが
// float3のため画素ごとに (透過率, fogInFront) の2スカラが要り、RGBA1枚に収まらない。
// 雲が無い画素には1.0が入る(補正が厳密に0になる中立元)
Texture2D SkyCloudFogTexture : register(t22);

// グローバルIBLの拡散イラディアンス(t8)・プリフィルタ済み鏡面(t9)・プローブのプリフィルタ済み
// 鏡面キューブマップ配列(t12)・プローブの影響範囲バッファ(t13)の宣言と、プローブの選択・
// 視差補正・ブレンド・鏡面IBLの重みはReflectionProbe.hlsliが持つ(SSR.hlslと共有するため。
// レジスタ番号は上のマクロで与えている)。反射プローブは鏡面専任なので、
// プローブ側の拡散イラディアンスは無い
#include "ReflectionProbe.hlsli"
// DDGI(22章)。拡散イラディアンスだけを差し替える。鏡面には一切触れないため、
// SSRとの「足した量と引く量が一致する」不変条件(20章)には影響しない
#include "DDGI.hlsli"

// DDGIResolveTexture(低解像度)を深度を見ながら引き伸ばす。
//
// 【なぜバイリニアでは駄目か】雲(SkyCloudTexture)は視線方向だけの関数で深度に依存しないため
// 素直なバイリニアで数学的に正しかった。DDGIのイラディアンスは面の位置と法線の関数なので、
// ジオメトリの輪郭をまたいで補間すると手前の面の間接光が奥の面へ滲む。
// そこで4テクセルを個別に引き、**中心画素と深度が近いテクセルだけ**を採用する。
//
// centerDepth はこの画素のG-Buffer深度(Reverse-Z、生値)。
// Reverse-Zの生値は概ね 1/z に比例するので、相対差をそのまま近さの尺度に使える
// (輪郭の検出が目的で、正確な線形深度は要らない)
float4 UpsampleDDGI(float2 uv, float centerDepth)
{
    uint width, height;
    DDGIResolveTexture.GetDimensions(width, height);
    const float2 resolution = float2(width, height);
    const float2 texelSize = 1.0f / resolution;

    // このUVを囲む4テクセルの中心。DDGIResolve.hlslが各テクセルで使ったUVと同じ式になるため、
    // 「そのテクセルが代表している位置」の深度をここで正しく引き直せる
    const float2 baseTexel = floor(uv * resolution - 0.5f);
    const float2 fractional = uv * resolution - 0.5f - baseTexel;

    // 4テクセルぶんの深度を1回で取る(41.24節)。GatherRedが返す並びは
    // .x=(u0,v1) .y=(u1,v1) .z=(u1,v0) .w=(u0,v0) なので、以降で使う
    // (0,0)(1,0)(0,1)(1,1) の順へ並べ替えると w, z, x, y になる。
    //
    // 【ループにも配列にもしないこと】ここを[unroll]ループ + 添字アクセスで書くと、
    // ローカル配列でもfloat4の添字でもfxcのコンパイル時間が爆発する
    // (41.20節と同じ罠。実際にこのシェーダーのコンパイルが数十秒延びた)。
    // 4本しかないので4成分のベクタ演算として素直に展開する ―― 重みの計算も
    // 4本まとめて1命令ずつになるため、展開したほうが実行時も速い
    const float4 tapDepth = DDGIResolveDepthTexture.GatherRed(DataSampler, uv).wzxy;

    // バイリニアの重み。上の並び順に合わせて (1-fx)(1-fy), fx(1-fy), (1-fx)fy, fx*fy
    const float2 wx = float2(1.0f - fractional.x, fractional.x);
    const float2 wy = float2(1.0f - fractional.y, fractional.y);
    float4 weight = float4(wx.x * wy.x, wx.y * wy.x, wx.x * wy.y, wx.y * wy.y);

    // 深度の近さ。相対差2%で概ね1/e。輪郭以外ではほぼ1になり、通常のバイリニアと一致する
    const float4 relativeDifference = abs(tapDepth - centerDepth) / max(centerDepth, 1e-6f);
    weight *= exp(-relativeDifference * 50.0f);
    // 背景(depth<=0)のテクセルはDDGIを持たないので必ず落とす。
    // 重み0で足すのは「足さない」と厳密に同じ(0倍して加えても和は変わらない)
    weight = (tapDepth > 0.0f) ? weight : 0.0f;

    const float2 uv00 = (baseTexel + float2(0.0f, 0.0f) + 0.5f) * texelSize;
    const float2 uv10 = (baseTexel + float2(1.0f, 0.0f) + 0.5f) * texelSize;
    const float2 uv01 = (baseTexel + float2(0.0f, 1.0f) + 0.5f) * texelSize;
    const float2 uv11 = (baseTexel + float2(1.0f, 1.0f) + 0.5f) * texelSize;

    // 加算の順序は展開前のループ(i=0..3)と同じにしてある。浮動小数の和は順序で結果が変わるため
    float4 accumulated = DDGIResolveTexture.SampleLevel(DataSampler, uv00, 0.0f) * weight.x;
    accumulated += DDGIResolveTexture.SampleLevel(DataSampler, uv10, 0.0f) * weight.y;
    accumulated += DDGIResolveTexture.SampleLevel(DataSampler, uv01, 0.0f) * weight.z;
    accumulated += DDGIResolveTexture.SampleLevel(DataSampler, uv11, 0.0f) * weight.w;
    const float totalWeight = weight.x + weight.y + weight.z + weight.w;

    // 4テクセルすべてが深度的に遠い(細い輪郭の内側など)。ここでDDGIを0にすると
    // 輪郭に沿って間接光が抜けた黒い縁が出るため、最寄り1テクセルをそのまま採る
    if (totalWeight <= 1e-6f)
    {
        return DDGIResolveTexture.SampleLevel(DataSampler, uv, 0.0f);
    }
    return accumulated / totalWeight;
}

// 環境ソース(プローブとグローバルIBLの重み付き合成)はSampleEnvironmentが返す。プローブと
// グローバルIBLはどちらも同じ手順で焼かれており(IBLConvolve.hlslを共有している)、解像度・
// ミップ構成も揃えてあるため、式そのものは同一で引くキューブマップだけが変わる
// uv/depthはDDGIを低解像度パスから引くとき(DDGIParams4.y > 0.5)のアップサンプルにだけ使う
float3 EvaluateIBL(float3 N, float3 V, float3 worldPos, float3 albedo, float metallic, float roughness,
                   float materialAO, float ssao, float diffuseAO, BentOcclusion bent,
                   float2 uv, float depth)
{
    const float NdotV = saturate(dot(N, V));
    const float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);

    const float3 R = reflect(-V, N);
    // ShadowParams.y = プリフィルタ済み鏡面マップの最大ミップレベル(ミップ数-1、KurenaiEngine3D側で設定)
    const float mipLevel = roughness * ShadowParams.y;

    float3 irradiance;
    float3 prefiltered;
    // 【イラディアンスはNではなくbent normalの軸で評価する】遮蔽されている方向からは
    // どのみち光が来ないので、開いている方向の環境を引くほうが正しい。壁際・窓際で
    // 「壁の側の空の色まで平均してしまう」方向バイアスがこれで解消される(仕様書§5)。
    // 鏡面のRは変えない ―― あちらは反射方向そのものが物理的に決まっているため
    const float3 irradianceDir = (OcclusionParams.x > 0.5f) ? bent.axis : N;
    SampleEnvironment(worldPos, irradianceDir, R, mipLevel, irradiance, prefiltered);

    // DDGI(22章)が有効なら、拡散の環境ソースだけをプローブ格子由来のものへ差し替える。
    // 【加算ではなく差し替えである】DDGIのイラディアンスは「その位置に来ている光」そのもので、
    // グローバルIBL/反射プローブのイラディアンスと同じ量の別の推定値である。足すと二重計上になる。
    // 鏡面(prefiltered)は差し替えない——反射プローブのほうが方向解像度が桁違いに高く、
    // DDGIのオクタヘドラル6x6では鏡面の映り込みを表現できないため
    //
    // 【無条件に差し替えず、ボリューム内外で重み付けしてlerpする】
    // ボリュームは1個しか持てず(22.10節)、無条件に差し替えると外側でも境界のプローブを
    // 外挿し続けてボリュームの外が「1部屋ぶんの間接光」で照らされる。insideWeightが0の点
    // (ボリューム外)はirradianceがSampleEnvironmentの返した値のまま残る
    if (DDGIParams0.w > 0.5f)
    {
        float3 ddgiIrradiance;
        float ddgiInsideWeight;
        // DDGIParams4.y は「低解像度のDDGIResolveパスの結果を使うか」。
        // 定数バッファ由来のuniform分岐なので画素ごとに分かれることはない
        if (DDGIParams4.y > 0.5f)
        {
            const float4 resolved = UpsampleDDGI(uv, depth);
            ddgiIrradiance = resolved.rgb;
            ddgiInsideWeight = resolved.a;
        }
        else
        {
            ddgiIrradiance = SampleDDGIIrradiance(worldPos, N, V, ddgiInsideWeight);
        }
        irradiance = lerp(irradiance, ddgiIrradiance, ddgiInsideWeight);
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
    // 0 = Frostbite近似 / 1 = 球冠交差 / 2 = 球面ガウス(34.11節)
    const int soMode = (int)(OcclusionParams.y + 0.5f);
    const float3 specularWeight =
        SpecularIBLWeight(F0, NdotV, roughness, soMode, bent, N, R, materialAO, ssao, brdf,
                          ShadowParams.w, ShadowParams.z);
    // Kulla-Conty方式(ShadowParams.w = 3)が足す加算ローブ。プリフィルタ済み鏡面ではなく
    // 拡散イラディアンスに掛かるため、上の「放射輝度 × 係数」とは別の項として持つ。
    // 乗算型(1・2)と無効(0)ではこの係数が0になり、項ごと消える
    const float3 multiScatterWeight =
        SpecularIBLMultiScatterWeight(F0, NdotV, roughness, soMode, bent, N, R, materialAO, ssao, brdf,
                                      ShadowParams.w, ShadowParams.z);

    // 【昼度(AmbientColor.a)による減衰はしない】手続き空(SkyGenerate.hlsl)は太陽高度に
    // 応じて自分で暗くなり、夜は月明かりの空になるため、ここで追加の減衰を掛ける必要がない
    // (掛けると二重に暗くなる。21.4節)。「IBL全体を昼度で0倍する」と夜の環境光が厳密に
    // ゼロになり、建物が真っ黒な影絵になる。
    // 鏡面側のShadowParams.z(IBL強度倍率)はspecularWeightに含まれている。
    // 環境光の鏡面倍率(IBLParams.z)も同様に2つのWeightの中で掛かっているため、
    // ここで明示的に掛けるのは拡散倍率(IBLParams.y)だけでよい。
    //
    // multi-bounce AO(Jimenez 2016)。アルベドが明るいほどAOを弱める補正。
    // 見た目を大きく変えるためUIで独立して切り替えられるようにしてある(既定は無効)
    const float3 diffuseOcclusion = (OcclusionParams.z > 0.5f)
        ? GTAOMultiBounce(diffuseAO, albedo)
        : float3(diffuseAO, diffuseAO, diffuseAO);

    return diffuseIBL * diffuseOcclusion * ShadowParams.z * IBLParams.y
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

// FrameConstantsのSky*フィールドからSky.hlsliのSkyParametersを組み立てる。
// SunDirectionの正規化はここで行う(SkyGenerate.hlsl側の慣習=呼び出し側でnormalizeする、に揃える)。
// SSR.hlsl/AerialPerspective.hlsl/PlanarReflection.hlslのMakeSkyParametersと完全に同一の内容で
// あること。4つのシェーダーはcbufferをそれぞれ別に宣言しているため関数そのものは共有できず
// 複製しているが、中身がずれると「背景の空」「水面に映る空」「フォグの合成先の色」が
// 互いに食い違ってしまうため、中身を変える場合は必ず4つとも同時に直すこと
SkyParameters MakeSkyParameters(float2 pixelPosition)
{
    SkyParameters params;
    params.SunDirection = normalize(SkySunDirection.xyz);
    // ティント4本と天頂輝度はSkyParametersBuffer(t17)にある(SkyIntegrate.hlslが書く)
    params = ApplySkyParametersFromBuffer(params, SkyParametersBuffer[0]);
    // 太陽照度/空照度比(SkyParams.zに詰めてある。KurenaiEngine3D.cppのSkyParams.zコメント参照)。
    // EvaluateCloudLayerが雲の明るさを太陽照度基準にするために使う
    params.SunToSkyIlluminanceRatio = SkyParams.z;
    // 雲。SSR.hlslのMakeSkyParametersと完全に同一の内容であること(このファイル冒頭の
    // コメントと同じ理由。背景に見える雲と水面に映る雲が食い違ってはいけない)
    params.CloudCoverage = CloudParams0.x;
    params.CloudAltitude = CloudParams0.y;
    params.CloudUvScale = CloudParams0.z;
    params.CloudDensity = CloudParams0.w;
    params.CloudScrollOffset = CloudParams1.xy;
    params.CloudForwardG = CloudParams1.z;
    // 積雲の厚み[m](CloudParams1.wの枠に詰めてある)。
    // 0ならレイマーチせず平面として扱う
    params.CloudThickness = CloudParams1.w;
    // 巻雲。SSR.hlslのMakeSkyParametersと完全に同一の内容であること
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
    // 星空。背景(このシェーダ)と水面の映り込み(SSR.hlsl)だけが星を描く。
    // 昼はCPU側がStarsParams.xへ0を入れるので、Sky.hlsli側が最初のifで抜ける
    params.StarsIntensity = StarsParams.x;
    params.StarsDensity = StarsParams.y;
    params.StarsTwinkle = StarsParams.z;
    params.StarsPixelAngle = StarsParams.w;
    params.StarsTime = TimeParams.x;
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
            // 【jitterはこのパスでは使われない】雲を評価するのはSkyCloud.hlslであり、
            // ここが呼ぶのはSkyColorWithoutClouds(雲を踏まない)とCloudAirlightCorrection
            // (レイマーチを持たない)だけ。他の4つのMakeSkyParametersと中身を揃えるために
            // 引数と代入はそのまま残してある
            const SkyParameters skyParams = MakeSkyParameters(input.Position.xy);
            // 雲を含まない空(SkyView LUT + 星)はここでフル解像度のまま評価する。
            // 太陽・星のような高周波成分がこちら側にあるため、雲の低解像度化で
            // にじむことがない
            const float3 clearColor = SkyColorWithoutClouds(rayDir, skyParams);
            // 雲はSkyCloudパスが低解像度で評価済み。事前乗算のover合成なので、
            // バイリニアで引き伸ばしてもこの合成の形は変わらない(SkyCloud.hlsl冒頭参照)。
            // 雲が無い画素には(0,1)が入っており、clearColor*1.0+0.0はIEEE754で
            // 厳密にclearColorと一致する
            const float4 cloud = SkyCloudTexture.Sample(ColorSampler, input.UV);
            // 雲の手前の霞の色を晴天の空色から曇天の空色へ直す(P18b)。
            // 【なぜここでフル解像度で掛けるか】補正項はclearColorに比例するため、
            // 低解像度の雲パス側で畳み込むと補正項の中の太陽・星だけがぼける。
            // CloudSkyLightはフレーム定数なので、画素ごとに要るのはfogInFrontの1chだけ。
            // 雲が無い画素には1.0が入っており、(1 - 1.0) = 0 で補正は厳密に0になる
            const float fogInFront = SkyCloudFogTexture.Sample(ColorSampler, input.UV).r;
            skyColor = clearColor * cloud.a + cloud.rgb
                     + CloudAirlightCorrection(clearColor, fogInFront, skyParams);

        }
        else
        {
            // 手続き空は太陽高度に応じた明るさで焼かれている(夜は月明かりの空になる)ため、
            // 「夜は暗い紺色へlerpする」ような補正は要らない
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
    // bent normal(34章)。持たないマテリアルは黒1x1がバインドされ、
    // DecodeBentOcclusionがaxis = N・aoB = 1(遮蔽なし)へ落とす
    const BentOcclusion bent = DecodeBentOcclusion(BentNormalTexture.Sample(DataSampler, input.UV), N);
    // ディフューズAOの出所。従来のベイクAO(materialAO)と aoN = dot(N, bRaw) は
    // 同じ積分の別推定量なので、切り替えても見た目はほぼ変わらないはず(変わったらどちらかがバグ)
    const float diffuseAO = (OcclusionParams.x > 0.5f) ? (aoSample.a * bent.aoN) : ao;
    float3 indirectLight = aoSample.rgb; // SSIL(Visibility Bitmask)使用時のみ非ゼロ。周囲のサーフェスからの間接拡散光
    float3 directLight = DirectLightTexture.Sample(ColorSampler, input.UV).rgb; // DirectLighting.hlslで計算済み(シャドウ適用済み)
    float3 emissive = EmissiveTexture.Sample(ColorSampler, input.UV).rgb;

    // ShadowParams.z = IBL強度倍率(0以下ならEnable IBL無効)。無効時は
    // 定数色(昼夜サイクルで変化するAmbientColor.rgb)による簡易アンビエントにフォールバックする
    // (何もライティングしない真っ暗な状態にはしない)。
    // EvaluateIBL内のirradianceはIBLConvolve.hlsl側で1/πと積分のπを相殺済みなのでそのままでよいが、
    // このフォールバックの定数色AmbientColorはその正規化を受けていないため、DirectLighting.hlslの
    // 拡散反射(kd*albedo/PI)とスケールを揃えるべくここで明示的に/PIする
    // (**/PIを落とすと環境光がπ倍(意図の20%に対し実際は約65%)明るくなる**)
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
        ambient = EvaluateIBL(
            N, V, worldPos, albedo, metallic, roughness, materialAO, aoSample.a, diffuseAO, bent, input.UV, depth);
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
        // 半透明パス(Transparent.hlsl)・プローブ焼き込みパス(ProbeCapture.hlsl)も
        // 同じ形のフォールバックを持つ(3経路で揃っていること)
        const float NdotV = saturate(dot(N, V));
        const float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
        // LUTの第3成分(Eavg)はKulla-Conty方式だけが使うため.rgbで引く(EvaluateIBLと同じ)
        const float3 brdf = BRDFLUTTexture.Sample(ColorSampler, float2(NdotV, roughness)).rgb;
        const SpecularEnergyContext energy = MakeSpecularEnergyContext(F0, brdf, roughness, ShadowParams.w);
        const float3 fallbackFssEss = F0 * brdf.x + brdf.y;
        const float fallbackEss = brdf.x + brdf.y;

        // 定数色アンビエントはプリフィルタ済み鏡面・拡散イラディアンスの両方の代わりを兼ねるため、
        // Kulla-Contyの加算ローブにも同じambientRadianceを掛ける。
        // 拡散項の遮蔽はdiffuseAO(bent normal有効時はaoN)を使う。鏡面項はこのフォールバックが
        // 方向を持たない一様環境を仮定しているため、bent normalのコーン交差ではなく
        // 従来どおりの非方向性SpecularOcclusion(ao)のままでよい(半透明パスと同じ扱い)
        ambient = diffuseColor * ambientRadiance * diffuseAO
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
