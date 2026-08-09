// GBuffer.hlsl/Water.hlslが共有する頂点シェーダー・構造体・cbuffer宣言。
//
// 両者は「頂点変換・G-Bufferへの書き込み方の枠組み」が完全に同一で、違いはPSMain内の
// 法線の求め方とMaterial.aへ書くマテリアルIDだけ(水面マテリアル基盤)。
// そのため頂点シェーダーVSMain・入出力構造体・cbuffer宣言をこのヘッダーへ括り出し、
// GBuffer.hlsl/Water.hlslはそれぞれのPSMainだけを持つようにしている。
#ifndef KURENAI_GBUFFER_COMMON_HLSLI
#define KURENAI_GBUFFER_COMMON_HLSLI

#include "Meshlet.hlsli"
#include "NormalEncoding.hlsli"
#include "Samplers.hlsli"

// G-BufferのMaterial.aに書き込むマテリアル種別ID。0=通常マテリアル(GBuffer.hlslのPSMainが
// そのまま0.0fを書く)、1=水面。将来SSR.hlsl側で水面の反射統合を行う際にもこの値を
// 参照する予定のため、値を変える場合は参照側(SSR.hlsl)も必ず同時に直すこと
static const float kMaterialIDWater = 1.0f;

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
    // ここから下はこのシェーダでは PrevViewProj / TAAParams / TimeParams しか使わない。
    // cbufferのレイアウトは宣言順で決まり途中のフィールドを飛ばせないため、末尾のオフセットを
    // 合わせる目的で間のフィールドも宣言だけしている(C++側 KurenaiEngine3D.cpp の
    // FrameConstantsと並びを一致させること)
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
    // DDGI用(22章)。このシェーダでは未使用だが、C++側でTimeParamsより手前に置かれているため
    // オフセット合わせのためだけに宣言する
    float4 DDGIParams0;
    float4 DDGIParams1;
    float4 DDGIParams2;
    float4 DDGIParams3;
    float4 DDGIParams4;
    // bent normalによる遮蔽(34章)。GBuffer.hlsl/Water.hlslのどちらのPSMainも読まないが、
    // C++側 KurenaiEngine3D.cpp の FrameConstants ではDDGIParams4の直後にあるため、
    // 後続のTimeParams(Water.hlslが読む)のオフセットを合わせるために宣言する
    float4 OcclusionParams;
    // 水面用(末尾に追加)。x=水面法線マップのスクロールオフセット(0〜1、CPU側で
    // 既にfmod済み)、y=波のスケール倍率(m_WaterWaveScale、層ごとのUVスケールに掛ける)、
    // z=波の強さ(m_WaterWaveStrength、0〜1、距離減衰のweightに掛ける)、w=未使用。
    // GBuffer.hlslのPSMainは使わないが、Water.hlslのPSMainが読む
    float4 TimeParams;
    // 空の解析評価用・雲・巻雲・平面反射。GBuffer.hlsl/Water.hlslのどちらの
    // PSMainも使わないが、C++側 KurenaiEngine3D.cpp の FrameConstants と並びを一致させる
    // 目的だけで宣言する(他シェーダーの同名フィールドと同じ扱い)
    float4 SkySunDirection;
    float4 SkyParams;
    float4 CloudParams0;
    float4 CloudParams1;
    float4 CloudParams2;
    float4 CloudParams3;
    float4 PlanarReflectionPlane;
    // 大気遠近(末尾に追加)。GBuffer.hlsl/Water.hlslのどちらのPSMainも使わない
    // (オフセット合わせのためだけに宣言する)
    float4 FogParams0;
    float4 FogParams1;
    // 水中項。xyz=水体の色(リニア)、w=未使用。GBuffer.hlslのPSMainは使わないが、
    // Water.hlslのPSMainが「メッシュ自身のBaseColorFactorではなくこの色を出力Albedoに使う」ために読む
    // (干潟の水の色はシーン側で調整したいパラメータであり、.kmodelを焼き直さずに変えられるようにするため)
    float4 WaterBodyColor;
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
    // 調整する
    float OcclusionStrength;
    // glTFのpbrMetallicRoughness.baseColorFactor(既定[1,1,1,1])。glTF仕様では
    // baseColor = baseColorTexture * baseColorFactor と定義されており、テクスチャの有無に
    // 関わらず常に掛ける。半透明パス(Transparent.hlsl)・プローブ焼き込み(ProbeCapture.hlsl)・
    // レイトレーシング(RaytracingScene.hlsli)も同じく掛ける。**どれか1つでも落としてはいけない**
    // ―― 同じメッシュでも「直接見たとき」と「反射プローブ/RT反射に映ったとき」で
    // 色が食い違う(14章参照)
    float4 BaseColorFactor;
    // マテリアル種別ID(末尾に追加)。0=通常マテリアル、1=水面(kMaterialIDWater、上記参照)。
    // C++側 KurenaiEngine3D::MakeObjectConstants が instance.IsWater に応じて設定する
    float MaterialID;

    // --- メッシュシェーダー経路(GBufferMeshlet.hlsl)専用 -------------------------------
    //
    // メッシュシェーダーには入力アセンブラが無く、頂点もメッシュレットも自分でバッファから
    // 読むしかない。読む先はbindlessディスクリプタ番号でここから受け取る
    // (IRHIDevice::RegisterBindlessが払い出した番号。Bindless.hlsli参照)。
    //
    // 【末尾に足してあるので既存シェーダーへの影響は無い】Shadow.hlslのように
    // 先頭までしか宣言していないシェーダーがあっても、定数バッファのオフセットは1バイトも
    // 動かない(上のMaterialID・BaseColorFactorのコメントと同じ理由)。
    // 頂点シェーダー経路ではどれも読まれないため、C++側は0のままでも構わない
    uint VertexBufferIndex;          // StructuredBuffer<MeshVertex>(Assets::Vertexと同じ並び)
    uint MeshletBufferIndex;         // StructuredBuffer<Meshlet>
    uint MeshletVertexBufferIndex;   // StructuredBuffer<uint>(頂点バッファへのインデックス)
    uint MeshletTriangleBufferIndex; // StructuredBuffer<uint>(ローカル頂点番号3つを詰めたもの)
    uint MeshletCount;               // このメッシュのメッシュレット数(増幅シェーダーの範囲外判定用)
};

Texture2D BaseColorTexture : register(t0);
Texture2D NormalTexture : register(t1);
Texture2D MetallicRoughnessTexture : register(t2);
Texture2D EmissiveTexture : register(t3);
// ベイク済みアンビエントオクルージョン(遮蔽マップ)。赤チャンネルが遮蔽率(1=遮蔽なし)。
// t4はTransparent.hlsl/ProbeCapture.hlslがカスケードシャドウマップ配列に使っているため、
// マテリアルテクスチャを読む3パスで共通して空いている最初のスロットがt5になる
Texture2D OcclusionTexture : register(t5);

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
    // このピクセルを出したメッシュレットの番号。メッシュシェーダー経路
    // (GBufferMeshlet.hlsl)でだけ実際の番号が入り、従来の頂点シェーダー経路では
    // kInvalidMeshletIndexになる。
    //
    // 三角形の中では一定なのでnointerpolation(補間すると意味を成さない整数が混ざる)。
    // 通常のPSMainは読まず、メッシュレットの分かれ方を目で確かめるデバッグ表示
    // (GBufferMeshlet.hlslのPSMainMeshletDebug)だけが使う
    nointerpolation uint MeshletIndex : TEXCOORD5;
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
    // R11G11B10_Floatは使えない ―― 符号なしのため負の成分が落ちる(34章)。
    // 【Water.hlslも必ず書くこと】この構造体はGBuffer.hlslとWater.hlslで共有しており、
    // 書き残すとそのターゲットの内容が未定義になる。水面はbent normalを焼いていないので
    // 有効フラグ0(=データ無し)を書き、消費側で従来の経路へ落とす
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
    // この経路はメッシュレットを経由していない(GBufferCommon.hlsliのPSInput参照)
    output.MeshletIndex = kInvalidMeshletIndex;
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

#endif // KURENAI_GBUFFER_COMMON_HLSLI
