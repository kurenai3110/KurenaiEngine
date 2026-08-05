// 大気遠近(height fog / aerial perspective)。高度で指数減衰する消散係数を持つ
// 均一媒質を光線に沿って解析的に積分した透過率を返す、純粋な計算だけのヘッダー。
//
// 【モデル】 sigma(h) = sigma0 * exp(-(h - refHeight) / scaleHeight)
// (h=ワールドY、sigma0=refHeightでの消散係数、scaleHeightは大きいほど霞が高くまで及ぶ)。
// 光線ABに沿ったこの積分は閉形式で解けて(Frostbiteのexponential height fogと同型)、
//   tau = sigma0 * exp(-(A.y - refHeight)/scaleHeight) * |AB| * (1 - exp(-t)) / t,  t = (B.y-A.y)/scaleHeight
//   透過率 = exp(-tau)
// になる。tは光線の高度変化を無次元化したもので、t→0(ほぼ水平な光線)で(1-exp(-t))/t→1に収束する。
//
// 【cbuffer/レジスタに一切依存しない】呼び出し側ごとにcbufferのレイアウトが異なるため
// (AerialPerspective.hlslはFrameConstantsのFogParams0/1、PlanarReflection.hlslも同様だが
// レジスタ番号やテクスチャ構成は別)、必要な値はすべて引数で受け取る(Sky.hlsli冒頭の
// 「cbufferに依存しない」という作法にならう)
#ifndef KURENAI_HEIGHT_FOG_HLSLI
#define KURENAI_HEIGHT_FOG_HLSLI

// rayStart→rayEndの光線に沿った大気遠近の透過率(0〜1、1で無霧)。
// sigma0: refHeightでの消散係数[1/m]、scaleHeight: スケールハイト[m]、refHeight: 基準高度[m](ワールドY)
float HeightFogTransmittance(float3 rayStart, float3 rayEnd, float sigma0, float scaleHeight, float refHeight)
{
    // 0除算対策。UIの下限より充分小さい値からしか呼ばれない想定だが、念のため下限を設ける
    const float safeScaleHeight = max(scaleHeight, 1.0f);

    const float3 rayVec = rayEnd - rayStart;
    const float d = length(rayVec);
    const float dy = rayVec.y;

    // expの引数はカメラが基準高度から極端に離れている(異常なシーン設定)場合に大きく振れる。
    // クランプしないとexpがinfを返し、以降の掛け算でNaNへ伝播する
    const float heightRatio = clamp((rayStart.y - refHeight) / safeScaleHeight, -40.0f, 40.0f);
    const float baseSigma = sigma0 * exp(-heightRatio);

    const float t = dy / safeScaleHeight;
    // (1 - exp(-t)) / t は t→0 で 1 へ収束する。ほぼ水平な光線(dy≒0)では0/0になるため
    // 明示的に分岐する(閾値はfloatの実用精度から見て十分小さい値)
    const float falloff = (abs(t) > 1e-4f) ? ((1.0f - exp(-t)) / t) : 1.0f;

    const float tau = baseSigma * d * falloff;
    return exp(-max(tau, 0.0f));
}

#endif // KURENAI_HEIGHT_FOG_HLSLI
