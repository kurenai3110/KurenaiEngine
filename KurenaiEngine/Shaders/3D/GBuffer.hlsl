// G-Buffer書き込み(不透明ジオメトリ用)のピクセルシェーダー。頂点シェーダー(VSMain)・
// 入出力構造体・cbuffer宣言はGBuffer.hlsl/Water.hlslで共有するためGBufferCommon.hlsliへ
// 括り出してある(P2: 水面マテリアル基盤)
#include "GBufferCommon.hlsli"

// bent normal(接空間で焼かれている。遮蔽マップと同じライトマップUV空間、34章)。
// t0〜t3・t5はGBufferCommon.hlsliのマテリアルテクスチャが使用中、t4はTransparent.hlsl/
// ProbeCapture.hlslがカスケードシャドウマップ配列に使っているためt6。
// **Water.hlslはこのテクスチャを読まない**(水面はbent normalを焼いていない)ため、
// 水面法線マップはt7に置いてある
Texture2D BentNormalTexture : register(t6);

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
