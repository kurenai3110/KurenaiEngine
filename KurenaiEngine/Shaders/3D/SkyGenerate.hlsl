// 空のキューブマップをGPUで手続き生成する。このファイルはIBL専用キューブマップ
// (256px/面、ミップ無し)のベイクに専念し、空モデル本体(Perez et al. "All-Weather Model
// for Sky Luminance Distribution" (1993) / Preetham, Shirley, Smits (SIGGRAPH 1999) の
// CIE快晴空モデル)はSky.hlsliへ移した(P3)。背景の解析評価(DeferredLighting.hlsl)も
// 同じSky.hlsliの関数を使うため、キューブマップと背景は常に同じ空を見る。
//
// === なぜオフラインDDSではなくGPU生成なのか ===
// Perez分布は太陽の位置に依存するため、時刻が変わると空の輝度分布の「形」そのものが変わる
// (circumsolarの明るい領域が太陽と一緒に動く)。これはオフラインで焼いた2枚のキューブマップの
// 線形補間では表現できない。
// 逆に言えば「昼夜2枚をブレンドするだけ」なら畳み込みが線形演算である以上
// prefilter(lerp(昼,夜,t)) == lerp(prefilter(昼),prefilter(夜),t) が厳密に成立するので、
// 動的な再ベイクは純粋な無駄になる。再ベイクが意味を持つのは、この非線形な変化を追うときだけ。
//
// このシェーダー(とSky.hlsli)は Tools/generate_sky_cubemap.py の移植であり、両者は同じ絵を
// 出す必要がある(あちらはオフラインの参照実装 兼 手続き空を無効にしたときのフォールバック用)。
// 係数・定数を変える場合は必ず両方(と KurenaiEngine3D.cpp のCPUミラー)を同時に直すこと。
#include "Samplers.hlsli"
#include "Sky.hlsli"

cbuffer SkyBakeConstants : register(b0)
{
    // 処理対象の面(D3Dのキューブマップ標準順: +X=0,-X=1,+Y=2,-Y=3,+Z=4,-Z=5)
    uint Face;
    // 天頂輝度のスケール。Perezの相対輝度にこれを掛けたものが最終的な輝度になる
    float ZenithLuminance;
    float2 SkyPadding0;
    // 太陽が「ある」向き(正規化済み)。光が進む向きとは符号が逆なので注意
    // (KurenaiEngine3D.cpp ComputeSunLighting の sunDirection と同じ向き)
    float4 SunDirection;
    // --- 空の色味。太陽高度に応じてCPU側(ComputeSkyTint)が決めた値。xyzのみ使う ---
    // 【なぜCPUで決めるのか】この色味は、ここでの描画と CPU 側の照度正規化
    // (ComputeSkyZenithScale。積分の重みに色味の輝度成分が入る)の両方で完全に一致して
    // いなければならない。CPUで1度決めて配れば、両者がずれることが構造的に起きなくなる
    float4 ZenithTint;
    float4 HorizonTint;
    float4 GroundTint;
    // xyz=夕焼け・朝焼けの暖色、w=その強さ(太陽の仰角0度で1、±15度で0)
    float4 SunGlowTint;
};

RWTexture2DArray<float4> SkyOut : register(u0);

// キューブマップの1面上のUV([0,1]^2)から方向を求める。
// IBLConvolve.hlsl の CubeFaceDirection および generate_sky_cubemap.py の face_direction_grid と
// 完全に同一の規約でなければならない(ずれると空とIBLで方向が食い違う)
float3 CubeFaceDirection(uint face, float2 uv)
{
    float2 ndc = uv * 2.0f - 1.0f;
    float u = ndc.x;
    float v = ndc.y;

    float3 dir;
    if (face == 0)      dir = float3(1.0f, -v, -u);   // +X
    else if (face == 1) dir = float3(-1.0f, -v, u);   // -X
    else if (face == 2) dir = float3(u, 1.0f, v);     // +Y
    else if (face == 3) dir = float3(u, -1.0f, -v);   // -Y
    else if (face == 4) dir = float3(u, -v, 1.0f);    // +Z
    else                dir = float3(-u, -v, -1.0f);  // -Z

    return normalize(dir);
}

[numthreads(8, 8, 1)]
void CSGenerateSky(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint width, height, elements;
    SkyOut.GetDimensions(width, height, elements);
    if (dispatchThreadID.x >= width || dispatchThreadID.y >= height)
    {
        return;
    }

    // テクセル中心のUVから方向を求める
    const float2 uv = (float2(dispatchThreadID.xy) + 0.5f) / float2(width, height);
    const float3 dir = CubeFaceDirection(Face, uv);

    // SkyBakeConstantsからSky.hlsliのSkyParametersを組み立てる。
    // ここでの正規化(normalize)は元の実装(SkyColor呼び出し側でのnormalize(SunDirection.xyz))を
    // そのまま踏襲したもので、Sky.hlsli側では正規化しない(呼び出し側の責務)
    SkyParameters params;
    params.SunDirection = normalize(SunDirection.xyz);
    params.ZenithLuminance = ZenithLuminance;
    params.ZenithTint = ZenithTint.rgb;
    params.HorizonTint = HorizonTint.rgb;
    params.GroundTint = GroundTint.rgb;
    params.SunGlowTint = SunGlowTint.rgb;
    params.SunGlowStrength = SunGlowTint.w;

    // 雲(P5)は明示的に無効(CloudCoverage=0)で埋める。IBL用キューブマップには雲を焼き込まない
    // (判断A、詳細はSky.hlsliの雲セクションのコメント参照)。雲が風で動くたびにキューブの
    // 焼き直し(空生成6回+プリフィルタ36回のディスパッチ)が必要になるのを避けるためで、
    // 被覆率による減光(判断B)はキューブへ焼く直前にCPU側(KurenaiEngine3D.cpp)が
    // ZenithLuminanceへ平均透過率を掛けることで表現する。CloudCoverage=0ならSky.hlsli側の
    // 早期脱出でこの下の残りのフィールドは一切参照されないが、意図を読めるようにするため
    // 他のフィールドも明示的に0で埋めておく
    params.CloudCoverage = 0.0f;
    params.CloudAltitude = 0.0f;
    params.CloudUvScale = 0.0f;
    params.CloudDensity = 0.0f;
    params.CloudScrollOffset = float2(0.0f, 0.0f);
    params.CloudForwardG = 0.0f;

    const float3 color = SkyColor(dir, params);

    // 面ごとに要素数1のUAVを張るためスライスは常に0
    SkyOut[uint3(dispatchThreadID.xy, 0)] = float4(max(color, 0.0f), 1.0f);
}
