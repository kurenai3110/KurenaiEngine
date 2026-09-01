// ライトの距離減衰の**唯一の定義**。
//
// 【なぜ独立したヘッダーなのか】この式は以前 4つのファイル(PunctualLighting.hlsli /
// ProbeShading.hlsli / Transparent.hlsl / PlanarReflection.hlsl)に別々に書かれていた。
// どれも字面まで同一だったが、**新しいライトの種類を1本にだけ足すと残り3本が
// それを素のポイントライトとして評価する**。減衰の分母は max(distSq, 0.0001f) で
// 下限が入っているだけなので、光源の位置にサーフェスがある場合(面光源をプロキシ化すると起きる)
// には 1e4 倍の照度になり、しかも**絵は出る**。
//
// とくに ProbeShading.hlsli は DDGI のプローブ焼き込みと反射プローブが通る道で、
// DDGI はヒステリシスで時間収束するため「起動直後は正常で、数秒かけて白く飽和していく」
// という追いにくい壊れ方をする。式を1か所にまとめて構造的に防ぐ。
//
// 【構造体にもレジスタにも依存させないこと】ProbeShading.hlsli と PunctualLighting.hlsli は
// どちらも同じ struct GPULight を宣言しており、同時にインクルードできない
// (PunctualLighting.hlsli 冒頭の注意を参照)。そのため**このヘッダーは GPULight を受け取らず、
// スカラだけを受け取る**。これが両方から読める唯一の形になっている。
//
// このヘッダーは他の何もインクルードしない(組み込みの max / saturate しか使わない)。

#ifndef KURENAI_LIGHT_ATTENUATION_HLSLI
#define KURENAI_LIGHT_ATTENUATION_HLSLI

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

#endif // KURENAI_LIGHT_ATTENUATION_HLSLI
