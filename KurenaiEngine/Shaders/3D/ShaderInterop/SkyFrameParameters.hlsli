// FrameConstants の Sky* / Cloud* / Fog* / Stars* から Sky.hlsli の SkyParameters を
// 組み立てる。**このリポジトリで唯一の定義。**
//
// 背景・水面の映り込み・平面反射・大気遠近・雲パスが同じ空を描くための同期義務を、
// かつてはコメントで持たせていた。違いは下の2つのマクロだけなので定義を1本へ寄せてある
// (経緯は docs/ImplementationHistory.md 82章)。
//
// 【インクルードする側の責務】このヘッダーより前に次を用意しておくこと。
//   - #include "Sky.hlsli"
//   - #include "ShaderInterop/FrameConstants.hlsli"
//   - StructuredBuffer<GPUSkyParameters> SkyParametersBuffer
//     (レジスタ番号はシェーダーごとに違うので、宣言は各シェーダーが持つ)
//
// 【任意のマクロ】どちらも定義しないのが既定で、定義しない側が安全側に倒れる
//   KURENAI_SKY_WITH_STARS
//       定義すると星空のパラメータを渡す。星を描くのは背景(DeferredLighting.hlsl)・
//       水面の映り込み(SSR.hlsl)・雲パス(SkyCloud.hlsl)だけ。定義しなければ
//       ApplyCloudFogParameters が入れた 0 のままになる(Sky.hlsli の同関数のコメント参照)
//   KURENAI_SKY_RAYMARCH_STEPS
//       ボリューム経路のレイマーチ段数を上書きする式。ボリューム経路を持つ
//       SkyCloud.hlsl だけが CloudQualityParams.x を渡す。定義しなければ 0 のままで、
//       Sky.hlsli のコンパイル時の既定へ落ちる

#ifndef KURENAI_SHADERINTEROP_SKYFRAMEPARAMETERS_HLSLI
#define KURENAI_SHADERINTEROP_SKYFRAMEPARAMETERS_HLSLI

SkyParameters MakeSkyParameters(float2 pixelPosition)
{
    SkyParameters params;
    // 【正規化はここで行う】C++側は SkySunDirection を正規化せずに渡す
    // (FrameConstants.h の同フィールドのコメント参照)
    params.SunDirection = normalize(SkySunDirection.xyz);
    // ティント4本と天頂輝度は SkyParametersBuffer にある(SkyIntegrate.hlsl が書く)
    params = ApplySkyParametersFromBuffer(params, SkyParametersBuffer[0]);
    // 太陽照度/空照度比(SkyParams.z に詰めてある)。EvaluateCloudLayer が雲の明るさを
    // 太陽照度基準にするために使う
    params.SunToSkyIlluminanceRatio = SkyParams.z;

    params.CloudCoverage = CloudParams0.x;
    params.CloudAltitude = CloudParams0.y;
    params.CloudUvScale = CloudParams0.z;
    params.CloudDensity = CloudParams0.w;
    params.CloudScrollOffset = CloudParams1.xy;
    params.CloudForwardG = CloudParams1.z;
    // 積雲の厚み[m](CloudParams1.w の枠に詰めてある)。0 ならレイマーチせず平面として扱う
    params.CloudThickness = CloudParams1.w;

    params.CirrusCoverage = CloudParams2.x;
    params.CirrusAltitude = CloudParams2.y;
    params.CirrusUvScale = CloudParams2.z;
    params.CirrusDensity = CloudParams2.w;
    params.CirrusScrollOffset = CloudParams3.xy;
    params.CirrusAnisotropy = CloudParams3.z;
    params.CloudTypeBias = CloudParams3.w;

#ifdef KURENAI_SKY_RAYMARCH_STEPS
    // 【ApplyCloudFogParameters より前で上書きする】あちらは段数に触れないが、
    // 元の SkyCloud.hlsl がこの位置で入れていた並びをそのまま保つ
    params.CloudRaymarchSteps = (int)(KURENAI_SKY_RAYMARCH_STEPS);
#endif

    // 雲層へ掛ける大気遠近(P12。Sky.hlsli の EvaluateCloudLayer (f)節)。
    // 雲は AerialPerspective.hlsl の早期脱出でフォグを受けないため、雲側で自前に掛ける。
    //
    // 【第3引数はレイの起点そのもの(P17)】雲層はワールド座標に固定されており、XZ まで要る。
    // PlanarReflection.hlsl はここへ鏡映後のカメラ位置(y が負)を渡すが、SkyColorUpper しか
    // 呼ばず EvaluateCloudLayer へ到達しないため影響しない。あちらを SkyColor /
    // SkyColorWithRay へ変えるなら、鏡映前のカメラ位置を渡し直すこと
    params = ApplyCloudFogParameters(params, FogParams0, CameraPosition.xyz);
    // レイマーチの開始位置を画素ごとにずらす量(C2)。スライスの縞をディザへ変える
    params.RaymarchJitter = CloudRaymarchDither(pixelPosition);

#ifdef KURENAI_SKY_WITH_STARS
    // 【ApplyCloudFogParameters の後で上書きする】あちらが 0 で潰すため。
    // 昼は CPU 側が StarsParams.x へ 0 を入れるので、Sky.hlsli 側が最初の if で抜ける
    params.StarsIntensity = StarsParams.x;
    params.StarsDensity = StarsParams.y;
    params.StarsTwinkle = StarsParams.z;
    params.StarsPixelAngle = StarsParams.w;
    params.StarsTime = TimeParams.x;
#endif

    return params;
}

#endif // KURENAI_SHADERINTEROP_SKYFRAMEPARAMETERS_HLSLI
