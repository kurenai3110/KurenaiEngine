// MegaLights の出力をフレーム方向に足し込むだけのパス。**計測のためだけにある。**
//
// 【何のためにあるか】確率的サンプリングの正しさは「ノイズが少ないか」ではなく
// 「**平均が真値に一致するか**」で決まる。ところが画面キャプチャで得られるのは
// トーンマップ後の8bitで、トーンマップは凹関数なので
//   平均(トーンマップ(x)) < トーンマップ(平均(x))
// となり、**偏りがまったく無くてもノイズがあるだけで平均が低く出る**。
// つまりスクリーンショットをN枚平均しても収束の検証にはならない。
// 線形空間で足し込む場所がどうしても要る。
//
// 【なぜテクスチャではなく構造化バッファか】読み書きを同じ資源へ行う(累積)ため、
// 型付きUAVの読み出しが要る。このRHIで型付きUAVの読み書きが保証されているのはR32系だけで、
// RGBA32Fのテクスチャでは成り立たない(AutoExposure.hlsl冒頭のコメント参照)。
// 構造化バッファにはその制約が無く、ping-pongも要らない。
//
// 【fp32で足すこと】fp16に落とすと足し込みの丸めが片側へ寄る。参照実装の出力を
// fp16に置いたときに実測で片側だけに寄った差が出ており、平均を測る器で同じことをすると
// 「偏りを測る道具そのものが偏る」ことになる。
//
// レイを撃たないので3バリアントすべてでコンパイルされる。

cbuffer MegaLightsAccumConstants : register(b0)
{
    // x=出力幅, y=出力高, z=足す前に0で始めるか(1でリセット), w=未使用
    uint4 Params0;
};

Texture2D MegaLightsTexture : register(t0);

// 1画素につきfloat4。index = y * 幅 + x。C++側の確保と添字の作り方を一致させること
RWStructuredBuffer<float4> MegaLightsAccum : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadID.xy;
    const uint2 outputSize = Params0.xy;
    if (pixel.x >= outputSize.x || pixel.y >= outputSize.y)
    {
        return;
    }

    const uint index = pixel.y * outputSize.x + pixel.x;
    const float4 current = MegaLightsTexture[pixel];

    // 【リセットのときは代入する】RHIにバッファのクリアが無いので、
    // 「0クリアしてから足す」ではなく「最初のフレームだけ代入する」形にする
    MegaLightsAccum[index] = (Params0.z != 0u) ? current : (MegaLightsAccum[index] + current);
}
