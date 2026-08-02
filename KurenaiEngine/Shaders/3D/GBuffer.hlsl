#include "NormalEncoding.hlsli"
#include "Samplers.hlsli"

cbuffer FrameConstants : register(b0)
{
    float4x4 ViewProj;
    float4x4 InvViewProj;
    // カスケードシャドウマップ用(このシェーダでは未使用。CameraPosition等のオフセットを
    // C++側のFrameConstantsに合わせるためだけに同じ配列サイズで宣言する)
    float4x4 CascadeViewProj[4];
    float4 CameraPosition;
    float4 LightDirection;
    float4 LightColor;
    // ここから下はこのシェーダでは PrevViewProj / TAAParams しか使わない。cbufferのレイアウトは
    // 宣言順で決まり途中のフィールドを飛ばせないため、末尾の2つのオフセットを合わせる目的で
    // 間のフィールドも宣言だけしている(C++側 KurenaiEngine3D.cpp の FrameConstants と並びを一致させること)
    float4x4 View;
    float4x4 Proj;
    float4 AmbientColor;
    float4 CascadeSplits;
    float4 ShadowParams;
    float4 ActiveLightCount;
    float4 IBLParams;
    float4 ProbeParams;
    float4 ProbeParams2;
    // 前フレームのビュー射影行列(ジッターを含んだまま)。モーションベクターの算出に使う
    float4x4 PrevViewProj;
    // TAAのサブピクセルジッター量(UV単位)。xy=今フレーム、zw=前フレーム
    float4 TAAParams;
};

// メッシュ単位(将来的にはシーン上のモデルインスタンス単位)の情報。
// DX12のルートシグネチャがCBVをb0/b1の2枠しか持たないため、モデル行列もここへ同居させている
cbuffer ObjectConstants : register(b1)
{
    float4x4 World;
    // Worldの3x3部分の逆転置(4x4に格納)。回転+非一様スケールで法線が歪むのを防ぐため、
    // 位置と同じWorldではなくこちらを法線の変換に使う(Architecture.html「法線マッピングの
    // 接線ベクトル計算」参照)
    float4x4 NormalMatrix;
    float MetallicFactor;
    float RoughnessFactor;
    // Worldの行列式が負(ミラーリングを含む非一様スケール)の場合は-1。従法線の向きが
    // 反転するため、頂点接線のw成分(従法線の向き)に掛け合わせて補正する
    float TangentSignFlip;
    // 0以下ならアルファカットアウト無効(常に不透明として扱う)。glTFのalphaMode=MASKの
    // マテリアルのみalphaCutoff(既定0.5)が設定される
    float AlphaCutoff;
    float3 EmissiveFactor;
    // glTFのocclusionTexture.strength(既定1.0)。遮蔽マップの効き具合をlerp(1, ao, strength)で
    // 調整する。かつて純粋な詰め物(ObjectPadding)だった枠をそのまま使っているため、
    // 定数バッファのサイズ・オフセットは変わっていない
    float OcclusionStrength;
    // glTFのpbrMetallicRoughness.baseColorFactor(既定[1,1,1,1])。glTF仕様では
    // baseColor = baseColorTexture * baseColorFactor と定義されており、テクスチャの有無に
    // 関わらず常に掛ける。半透明パス(Transparent.hlsl)・プローブ焼き込み(ProbeCapture.hlsl)・
    // レイトレーシング(RaytracingScene.hlsli)は以前から掛けていたが、この不透明パスだけが
    // 宣言しておらず落としていた。そのため同じメッシュでも「直接見たとき」と
    // 「反射プローブ/RT反射に映ったとき」で色が食い違っていた(14章参照)
    float4 BaseColorFactor;
};

Texture2D BaseColorTexture : register(t0);
Texture2D NormalTexture : register(t1);
Texture2D MetallicRoughnessTexture : register(t2);
Texture2D EmissiveTexture : register(t3);
// ベイク済みアンビエントオクルージョン(遮蔽マップ)。赤チャンネルが遮蔽率(1=遮蔽なし)。
// t4はTransparent.hlsl/ProbeCapture.hlslがカスケードシャドウマップ配列に使っているため、
// マテリアルテクスチャを読む3パスで共通して空いている最初のスロットがt5になる
Texture2D OcclusionTexture : register(t5);
// bent normal(RGBA16F)。遮蔽マップと同じライトマップUV空間へ焼かれている。
// t4はカスケードシャドウ配列、t5は遮蔽マップが使っているためt6を割り当てる
Texture2D BentNormalTexture : register(t6);

struct VSInput
{
    float3 Position : POSITION;
    float3 Normal : NORMAL;
    float2 UV : TEXCOORD0;
    float4 Tangent : TANGENT;
    // ライトマップUV(Assets::Vertex::UV1)。遮蔽マップ専用で、重なりが無く[0,1]に収まる
    float2 LightmapUV : TEXCOORD1;
};

struct PSInput
{
    float4 Position : SV_POSITION;
    float3 Normal : NORMAL;
    float3 WorldPos : TEXCOORD1;
    float2 UV : TEXCOORD0;
    float4 Tangent : TANGENT;
    float2 LightmapUV : TEXCOORD2;
    // モーションベクター用のクリップ空間座標。SV_POSITIONはラスタライザがw除算と
    // ビューポート変換を済ませた値になってしまい元のwが取れないため、別途そのまま渡す。
    // w除算は必ずPS側で行うこと(VS側で割ってから補間すると、遠近補正が効かず
    // 三角形の内側でずれる)
    float4 CurClip : TEXCOORD3;
    float4 PrevClip : TEXCOORD4;
};

struct PSOutput
{
    float4 Albedo : SV_TARGET0;
    float2 Normal : SV_TARGET1;
    float4 Material : SV_TARGET2;
    float4 Emissive : SV_TARGET3;
    // モーションベクター(この画素の中身が前フレームから今フレームまでに動いた量、UV単位)
    float2 Velocity : SV_TARGET4;
    // bent normal(正規化しない可視方向の平均、ワールド空間)。.rgb = bRaw、.a = 有効フラグ。
    // R11G11B10_Floatは使えない ―― 符号なしのため負の成分が落ちる(34章)
    float4 BentNormal : SV_TARGET5;
};

// クリップ空間座標を画面UV([0,1]、左上原点)へ変換する。
// NDCのyは上が+1・下が-1なのに対しUVのvは上が0・下が1なので、yだけ符号を反転する
float2 ClipToUv(float4 clipPos)
{
    return (clipPos.xy / clipPos.w) * float2(0.5f, -0.5f) + 0.5f;
}

PSInput VSMain(VSInput input)
{
    PSInput output;
    float3 worldPos = mul(float4(input.Position, 1.0f), World).xyz;
    output.Position = mul(float4(worldPos, 1.0f), ViewProj);
    output.Normal = mul(input.Normal, (float3x3)NormalMatrix);
    output.WorldPos = worldPos;
    output.UV = input.UV;
    output.LightmapUV = input.LightmapUV;
    // 接線は面上の方向ベクトルなので、法線と異なりinverse-transposeではなく
    // Worldの3x3部分そのままで変換する(位置と同じ変換)
    output.Tangent = float4(mul(input.Tangent.xyz, (float3x3)World), input.Tangent.w * TangentSignFlip);

    output.CurClip = output.Position;
    // 前フレームの投影位置。現在のシーンは全インスタンスが静的(ModelInstance::Worldは
    // 読み込み時に確定し以降変わらない)なので、前フレームのワールド座標は今と同じでよく、
    // 違うのはカメラ由来のビュー射影行列だけになる。動的オブジェクトを入れる場合は
    // ObjectConstantsへPrevWorldを追加し、input.Positionをそちらで変換してからここへ渡すこと
    output.PrevClip = mul(float4(worldPos, 1.0f), PrevViewProj);
    return output;
}

// 頂点接線(xyz)と従法線の向き(w = +1/-1)からTBN行列を構築する。
// UV/位置の画面空間微分(ddx/ddy)から近似する手法は、UV継ぎ目(シームがある円筒状展開の
// グラス類など)でピクセルクアッドがトポロジー的に不連続になり法線が破綻するため使用しない
float3x3 ComputeTangentFrame(float3 N, float4 tangent)
{
    // 頂点補間でTとNの直交性が崩れるため、ピクセル単位でGram-Schmidt再直交化する
    float3 T = normalize(tangent.xyz - N * dot(N, tangent.xyz));
    float3 B = cross(N, T) * tangent.w;
    return float3x3(T, B, N);
}

PSOutput PSMain(PSInput input)
{
    // baseColor = baseColorTexture * baseColorFactor(glTF仕様)。BaseColorTextureIndexが
    // 無いマテリアルは白1x1のプレースホルダーが差さるため、この乗算だけで
    // 「テクスチャのみ」「係数のみ」「両方」のすべてを正しく扱える
    float4 baseColorSample = BaseColorTexture.Sample(MaterialSampler, input.UV) * BaseColorFactor;

    // AlphaCutoff<=0(アルファカットアウト無効)の場合、alpha(0〜1)は常にAlphaCutoff以上になるため
    // clipは発火しない。AlphaCutoff>0の場合のみ、alphaがそれを下回るピクセルを破棄する
    clip(baseColorSample.a - AlphaCutoff);

    float3 geometricNormal = normalize(input.Normal);

    // BC5(2チャンネル、X/Yのみ)圧縮された法線マップはB/Aチャンネルにデータを持たず、
    // サンプリング時にハードウェアがB=0を返すため、Bをそのまま使うとタンジェント空間Zが
    // 常に-1(裏向き)になってしまう。単位ベクトルである前提でX/YからZを再構成する
    // (通常の3チャンネル法線マップに対しても正しく機能する)
    float2 normalXY = NormalTexture.Sample(MaterialSampler, input.UV).xy * 2.0f - 1.0f;
    float normalZ = sqrt(saturate(1.0f - dot(normalXY, normalXY)));
    float3 normalSample = float3(normalXY, normalZ);
    float3x3 tbn = ComputeTangentFrame(geometricNormal, input.Tangent);
    float3 N = normalize(mul(normalSample, tbn));

    float3 metallicRoughnessSample = MetallicRoughnessTexture.Sample(MaterialSampler, input.UV).rgb;
    float metallic = saturate(MetallicFactor * metallicRoughnessSample.b);
    // RoughnessFactorが負の場合はソースデータにラフネス係数が無かったことを表す
    // (Assets::kInvalidMaterialFactor)。パッカーが勝手な既定値を埋めない方針のため、
    // ここで係数1.0=テクスチャの値をそのまま使う、と解釈する
    float roughnessFactor = (RoughnessFactor < 0.0f) ? 1.0f : RoughnessFactor;
    float roughness = clamp(roughnessFactor * metallicRoughnessSample.g, 0.045f, 1.0f);

    float3 emissive = EmissiveTexture.Sample(MaterialSampler, input.UV).rgb * EmissiveFactor;

    // モーションベクター。今フレームと前フレームの投影位置をどちらも画面UVへ直し、その差を取る。
    //
    // 【ジッターを引く理由】投影行列にはTAAのサブピクセルジッターが入っている。単純に差を取ると
    // 「ジッターが前フレームからどれだけ変わったか」まで混ざるが、ジッターは同じ面のどこを
    // サンプルしたかの違いであって、ものが動いた量ではない。両フレームぶんを引いて純粋な移動量に
    // 戻しておかないと、TAAが履歴を引く位置が毎フレーム±0.5px揺れて永久に収束しない
    float2 currentUv = ClipToUv(input.CurClip) - TAAParams.xy;
    float2 previousUv = ClipToUv(input.PrevClip) - TAAParams.zw;

    // ベイク済みアンビエントオクルージョン。glTF仕様どおり赤チャンネルを遮蔽率として読み、
    // occlusionTexture.strengthをここで適用してしまう(下流のライティングパスは単に乗算するだけで
    // 済み、strengthの解釈が1か所に閉じる)。遮蔽マップを持たないマテリアルは白1x1が
    // バインドされるためao=1となり、strengthに関わらず見た目は変わらない。
    //
    // 【引くUVがLightmapUV(TEXCOORD1)である理由】遮蔽マップはKurenaiPackerがxatlasで生成した
    // 重なりの無い専用UV空間へ焼かれている。マテリアル用のUV(TEXCOORD0)はタイリング前提で
    // 面ごとに固有の場所を持たないため、そちらで引くと別の場所の遮蔽を読んでしまう(22章)。
    // glTFのocclusionTextureのように元から遮蔽マップを持つアセットもこのUV1側へ寄せてある
    // (パッカーが未ベイク時はUV1をUVと同じ値で埋める)ので、シェーダー側の分岐は不要
    float occlusionSample = OcclusionTexture.Sample(MaterialSampler, input.LightmapUV).r;
    float ao = lerp(1.0f, occlusionSample, OcclusionStrength);

    PSOutput output;
    output.Albedo = float4(baseColorSample.rgb, 1.0f);
    output.Normal = OctEncode(N);
    // bチャンネルはマテリアルの遮蔽率。DeferredLighting.hlslとSSR.hlslがSSAO/SSILの遮蔽と
    // 乗算して使う(専用のG-Bufferを増やさずに済むよう、未使用だった枠を使っている)
    output.Material = float4(metallic, roughness, ao, 0.0f);
    output.Emissive = float4(emissive, 1.0f);
    output.Velocity = currentUv - previousUv;

    // bent normalも遮蔽マップと同じLightmapUVで引く(焼かれている空間が同じ)。
    //
    // 【接空間で焼かれている】ワールド(モデル)空間で焼くと「遮蔽なし = N」になるため、
    // 曲面では遮蔽が無くても隣り合うテクセルの向きが違い、ミップ生成やバイリニア補間で
    // 平均したときに打ち消し合って長さが縮む。消費側はその長さをaoB(遮蔽率)として
    // 読むので、縮小するほど暗くなり細かい黒い点になる。接空間なら遮蔽なしは曲率に
    // よらず常に(0,0,1)なので、平均しても長さ1のまま保たれる(34章)。
    //
    // ベイカーが使う基底は上のComputeTangentFrameとまったく同じ手順で組まれている。
    // ここでmul(bentTS, tbn)と書けるのは、tbnの行が順にT/B/Nだから。
    // 直交行列なので長さ(=aoB)は変換で保たれる ―― 遮蔽の強さが座標変換で変わってはいけない
    const float4 bentSample = BentNormalTexture.Sample(MaterialSampler, input.LightmapUV);
    output.BentNormal = float4(mul(bentSample.xyz, tbn), bentSample.a);

    return output;
}
