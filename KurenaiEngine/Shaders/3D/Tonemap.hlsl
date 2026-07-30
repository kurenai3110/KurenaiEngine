// トーンマップパス。HDRのSceneColor(SSR有効時はSSR適用後のSceneColor)を読み、
// トーンマッピングカーブ+sRGBエンコードでLDR(0〜1)へ変換してPresentパスへ渡す。
// 以前はDeferredLighting.hlsl(ライティングパス自体)がこの変換まで行っていたが、
// それだとSSRがトーンマップ済みLDRを反射元として読むことになり、反射色が1.0を超えられず
// エネルギー保存が破れていた。SceneColorをHDRのまま保持しトーンマップをPresent直前の
// この独立パスへ切り出すことで、SSR・ブルーム/露出制御が物理的に正しいHDR値の
// 上に成立できるようにする
#include "Samplers.hlsli"

Texture2D SceneColorTexture : register(t0);
// AutoExposure.hlslが書いた露出。texel(0,0)=平滑化後のEV100、texel(1,0)=初期化済みフラグ。
// 自動露出が無効のときも常に有効なテクスチャがバインドされる(UseAutoExposure=0で無視される)
Texture2D<float> ExposureTexture : register(t1);
// Bloom.hlslが作ったブルームのピラミッド最上段(半解像度)。無効時も常に有効なテクスチャが
// バインドされる(BloomStrength=0で寄与しない)
Texture2D BloomTexture : register(t2);

cbuffer TonemapConstants : register(b1)
{
    // トーンマッピングカーブの選択(KurenaiEngine3D.h の TonemapCurve と一致させること)。
    // 0=Reinhard, 1=ACES, 2=AgX
    int Curve;
    // 手動露出時に使う固定の露出倍率(常に1.0。露出はCPU側でライト強度へ事前乗算済み)
    float ExposureScale;
    // 出力8bit量子化の直前に加えるディザの強さ(0=無効、1=±1LSB)。
    // 暗部の滑らかなグラデーションが数十コードにしか乗らないことによるバンディングは、
    // 中間バッファの精度ではなくこの最終8bit量子化が主因であることを実測で確認している
    // (Bistro Interiorで走査線上に同一色が24px連続。バッファ精度をHDR化しても変わらなかった)
    float DitherStrength;
    // 1.0=自動露出を使う(ExposureTextureのEV100から露出倍率を求める)、0.0=手動(ExposureScale)
    float UseAutoExposure;
    // CPU側でライト強度へ事前乗算済みのEV100(プリ露出)。自動露出のEV100との差が
    // そのまま適用すべき露出倍率になる(PSMain参照)
    float PreExposureEV100;
    // ブルームの合成比(0で無効)。加算ではなくlerpで混ぜることでエネルギーを保存する
    // (加算だと画面全体が明るくなり、露出を上げたのと区別がつかなくなる)
    float BloomStrength;
    float2 TonemapPadding;
};

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

// --- Reinhard: c/(c+1)。実装が最も単純だが、ハイライトが彩度を失って灰色へ寄り、
//     暗部のコントラストも寝る。比較用のリファレンスとして残している
float3 TonemapReinhard(float3 color)
{
    return color / (color + 1.0f);
}

// --- ACES: Narkowicz 2015 の曲線フィット近似("ACES Filmic Tone Mapping Curve")。
//     フィルミックなコントラストが得られるが、飽和した明るい色の色相がシフトする
//     (特に赤がオレンジへ寄る)ことが知られている
float3 TonemapACES(float3 color)
{
    const float a = 2.51f;
    const float b = 0.03f;
    const float c = 2.43f;
    const float d = 0.59f;
    const float e = 0.14f;
    return saturate((color * (a * color + b)) / (color * (c * color + d) + e));
}

// --- AgX: Troy Sobotka の AgX を Filament / three.js が採用している形で実装したもの。
//     いったんRec.2020へ移し、インセット行列で色域を内側へ縮めてからlog2空間の
//     シグモイドを掛け、アウトセット行列で戻す。ハイライトが色相を保ったまま白へ
//     素直に脱色するため、飽和した色(このシーンだと赤い壁)でACESのような色相シフトが出ない。
//
//     行列はGLSL(列ベクトル、M*v)の定義をそのまま持ち込んでいる。HLSLのmul(v, M)は
//     Mの「行」に対して同じ線形結合になるため、GLSL側のvec3をそのまま行として並べれば等価になる
static const float3x3 kLinearSRGBToLinearRec2020 =
{
    0.6274f, 0.0691f, 0.0164f,
    0.3293f, 0.9195f, 0.0880f,
    0.0433f, 0.0113f, 0.8956f,
};

static const float3x3 kLinearRec2020ToLinearSRGB =
{
     1.6605f, -0.1246f, -0.0182f,
    -0.5876f,  1.1329f, -0.1006f,
    -0.0728f, -0.0083f,  1.1187f,
};

static const float3x3 kAgXInsetMatrix =
{
    0.856627153315983f,  0.137318972929847f,  0.11189821299995f,
    0.0951212405381588f, 0.761241990602591f,  0.0767994186031903f,
    0.0482516061458583f, 0.101439036467562f,  0.811302368396859f,
};

static const float3x3 kAgXOutsetMatrix =
{
     1.1271005818144368f,  -0.1413297634984383f,  -0.14132976349843826f,
    -0.11060664309660323f,  1.157823702216272f,   -0.11060664309660294f,
    -0.016493938717834573f, -0.016493938717834257f, 1.2519364065950405f,
};

// AgXが扱う露出レンジ(log2)。この範囲外は潰れる
static const float kAgXMinEv = -12.47393f;
static const float kAgXMaxEv = 4.026069f;

// AgXのシグモイドを7次多項式で近似したもの(Filament/three.jsと同じ係数)
float3 AgXContrastApprox(float3 x)
{
    float3 x2 = x * x;
    float3 x4 = x2 * x2;
    return 15.5f * x4 * x2
         - 40.14f * x4 * x
         + 31.96f * x4
         - 6.868f * x2 * x
         + 0.4298f * x2
         + 0.1191f * x
         - 0.00232f;
}

float3 TonemapAgX(float3 color)
{
    color = mul(color, kLinearSRGBToLinearRec2020);
    color = max(color, 0.0f);
    color = mul(color, kAgXInsetMatrix);
    // log2(0)=-infを避けるための下限
    color = max(color, 1e-10f);
    color = log2(color);
    color = (color - kAgXMinEv) / (kAgXMaxEv - kAgXMinEv);
    color = saturate(color);
    color = AgXContrastApprox(color);
    color = mul(color, kAgXOutsetMatrix);
    // アウトセット行列は出力が2.2ガンマであることを前提にしているため、
    // ここでリニアへ戻す(このあと下でsRGB OETFを掛けるのが本パスの担当)
    color = pow(max(color, 0.0f), 2.2f);
    color = mul(color, kLinearRec2020ToLinearSRGB);
    return saturate(color);
}

// リニア値をsRGBのOETF(区分関数)でエンコードする。従来は pow(color, 1/2.2) の近似だったが、
// テクスチャ側のデコードはハードウェアのsRGB(区分関数)で行われているため、
// エンコードだけ2.2の冪にすると暗部が系統的にずれる。両者を揃える
float3 LinearToSRGB(float3 color)
{
    color = saturate(color);
    const float3 lo = color * 12.92f;
    const float3 hi = 1.055f * pow(color, 1.0f / 2.4f) - 0.055f;
    return lerp(lo, hi, step(0.0031308f, color));
}

// Interleaved Gradient Noise (Jimenez 2014)。1テクセルあたりの計算が軽く、
// 空間的に十分に散るのでディザ用途に向く
float InterleavedGradientNoise(float2 position)
{
    return frac(52.9829189f * frac(dot(position, float2(0.06711056f, 0.00583715f))));
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float3 color = SceneColorTexture.Sample(ColorSampler, input.UV).rgb;

    // 露出。SceneColorにはプリ露出(PreExposureEV100)が既に乗っているので、
    // 自動露出が求めたEV100との「差」だけを掛け直せばよい:
    //   exposure(ev) = 1/(1.2 * 2^ev) より
    //   exposure(auto) / exposure(pre) = 2^(pre - auto)
    // 自動露出が無効なら手動のExposureScale(=1.0)をそのまま使う
    float exposureScale = ExposureScale;
    if (UseAutoExposure > 0.0f)
    {
        const float autoEV100 = ExposureTexture.Load(int3(0, 0, 0));
        exposureScale = exp2(PreExposureEV100 - autoEV100);
    }
    color *= exposureScale;
    // NaN/負値がここまで来ると以降の多項式で破綻するので落としておく
    color = max(color, 0.0f);

    // ブルームの合成。ブルームのピラミッドは入力段で既に同じ露出を掛けてあるので
    // (Bloom.hlsl の ExposureScale())、ここでは露出適用後のcolorとそのまま混ぜられる。
    // 加算ではなくlerpなのはエネルギーを保存するため(加算だと画面全体が明るくなり、
    // 露出を上げたのと区別がつかなくなる)
    if (BloomStrength > 0.0f)
    {
        const float3 bloom = BloomTexture.Sample(ColorSampler, input.UV).rgb;
        color = lerp(color, bloom, saturate(BloomStrength));
    }

    if (Curve == 1)
    {
        color = TonemapACES(color);
    }
    else if (Curve == 2)
    {
        color = TonemapAgX(color);
    }
    else
    {
        color = TonemapReinhard(color);
    }

    color = LinearToSRGB(color);

    // 8bit量子化の直前に三角分布ノイズ(±1LSB)を加える。一様分布より三角分布のほうが
    // 量子化誤差と入力値の相関が切れ、バンドの縁が残りにくい。
    // 2つの独立した一様乱数の和-1で三角分布を作る
    if (DitherStrength > 0.0f)
    {
        const float r1 = InterleavedGradientNoise(input.Position.xy);
        const float r2 = InterleavedGradientNoise(input.Position.xy + float2(11.0f, 17.0f));
        const float triangular = r1 + r2 - 1.0f;
        color += (triangular * DitherStrength) / 255.0f;
    }

    return float4(color, 1.0f);
}
