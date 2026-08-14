// SSAO(Screen Space Ambient Occlusion)パス。
// PSMain: G-BufferのNormal/Depthからサンプリングカーネルを使って遮蔽率を計算する(Texture0=World Normal, Texture1=Depth)
// PSMainBlur: PSMainの出力(タイル状ノイズを含む)を均すための5x5分離可能ブラー(Texture0=AO Raw)。
// SSAOとSSIL(Visibility Bitmask)は同じRGBAフォーマット(rgb=間接拡散光, a=遮蔽率)を出力するため、
// このブラーはSSIL_VisibilityBitmask.hlslのブラーパスとしても共用する
#include "NormalEncoding.hlsli"
#include "Samplers.hlsli"

static const float PI = 3.14159265359f;
// 定数バッファに確保するカーネルの最大数。実際に回す段数はParams.wで実行時に渡す
// (品質プリセットから振れるようにするため。C++側のkSSAOKernelSizeMaxと一致させること)
static const int kSSAOKernelSizeMax = 16;

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
    // 【宣言はここで止めている】このシェーダーが読むのはProjまでで、それより後ろは使わない。
    // C++側のFrameConstantsはこの後ろにTimeParams・Sky*・Cloud*・PlanarReflectionPlane・
    // Fog*・WaterBodyColorを持つが、cbufferは宣言順レイアウトなので、途中を飛ばして末尾だけを
    // 宣言すると誤ったオフセットを読む。しかもコンパイルは通り絵も「それらしく」出るため気付けない。
    // これらが必要になったら、C++の並びどおりに間のフィールドをすべて宣言すること
};

cbuffer SSAOConstants : register(b1)
{
    float4 Samples[kSSAOKernelSizeMax]; // タンジェント空間の半球カーネル(xyz)。原点付近に偏らせてある
    float4 Params;                      // x: 半径, y: バイアス, z: 強さ(べき乗), w: 実際に使うサンプル数
};

// PSMainではNormal(t0)/Depth(t1)、PSMainBlurではSSAO Raw(t0)をバインドして使い回す
Texture2D Texture0 : register(t0);
Texture2D Texture1 : register(t1);

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

// ピクセル座標から[0,1)の疑似乱数を得るハッシュ関数(Dave Hoskinsのhash12)。
// タイル状のノイズテクスチャを用意する代わりに、画面全体で高周波なランダム回転を安価に生成する
float Hash12(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031f);
    p3 += dot(p3, p3.yzx + 33.33f);
    return frac((p3.x + p3.y) * p3.z);
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float depth = Texture1.Sample(DataSampler, input.UV).r;
    if (depth <= 0.0f)
    {
        // 背景(スカイ)は遮蔽なし・間接光なし(SSAOは間接光を計算しないのでrgbは常に0)
        // Reverse-Zのため遠平面(=背景)はNDC z=0.0付近になる
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    }

    float3 worldPos = ReconstructWorldPos(input.UV, depth);
    float3 normalWorld = OctDecode(Texture0.Sample(DataSampler, input.UV).xy);

    float3 viewPos = mul(float4(worldPos, 1.0f), View).xyz;
    float3 viewNormal = normalize(mul(normalWorld, (float3x3)View));

    // ピクセル座標のハッシュから毎ピクセル異なる回転を作り、カーネルサンプルの向きをランダム化する
    // (バンディングを高周波ノイズに変換し、後段のブラーパスで均す)
    float randomAngle = Hash12(input.Position.xy) * 2.0f * PI;
    float2 randomVec = float2(cos(randomAngle), sin(randomAngle));

    float3 tangent = normalize(float3(randomVec, 0.0f) - viewNormal * dot(float3(randomVec, 0.0f), viewNormal));
    float3 bitangent = cross(viewNormal, tangent);
    float3x3 tbn = float3x3(tangent, bitangent, viewNormal);

    const float radius = Params.x;
    const float bias = Params.y;
    const float power = Params.z;

    // 段数は実行時に変わる(品質プリセット)。C++側が範囲内の値しか書かないが、
    // 定数バッファの中身は保証できるものではないのでシェーダ側でも必ず丸める
    const int sampleCount = clamp((int)Params.w, 1, kSSAOKernelSizeMax);

    float occlusion = 0.0f;
    // 【[unroll]ではなく[loop]】可変回数のループは展開できない。
    // 段数を固定していた頃は[unroll]で16回展開していた
    [loop]
    for (int i = 0; i < sampleCount; ++i)
    {
        float3 sampleVec = mul(Samples[i].xyz, tbn);
        float3 samplePos = viewPos + sampleVec * radius;

        float4 offset = mul(float4(samplePos, 1.0f), Proj);
        offset.xyz /= offset.w;
        float2 sampleUV = float2(offset.x * 0.5f + 0.5f, 1.0f - (offset.y * 0.5f + 0.5f));

        if (sampleUV.x < 0.0f || sampleUV.x > 1.0f || sampleUV.y < 0.0f || sampleUV.y > 1.0f)
        {
            continue;
        }

        float sampleDepth = Texture1.Sample(DataSampler, sampleUV).r;
        if (sampleDepth <= 0.0f)
        {
            continue;
        }

        // サンプル位置のワールド座標をView空間へ変換し、Z(カメラからの距離)だけを比較に使う
        float3 sampleWorldPos = ReconstructWorldPos(sampleUV, sampleDepth);
        float sampleViewZ = mul(float4(sampleWorldPos, 1.0f), View).z;

        // 遮蔽物がカーネルサンプル位置より手前(視距離が近い)にあれば遮蔽としてカウントする。
        // ただし遠く離れた無関係なジオメトリまで遮蔽扱いしないよう半径ベースで減衰させる(range check)
        float rangeCheck = smoothstep(0.0f, 1.0f, radius / max(abs(viewPos.z - sampleViewZ), 1e-4f));
        occlusion += (sampleViewZ <= samplePos.z - bias ? 1.0f : 0.0f) * rangeCheck;
    }

    float ao = saturate(1.0f - occlusion / float(sampleCount));
    ao = pow(ao, power);
    // SSAOは間接光を計算しないため、rgb(間接拡散光)は常に0、a(遮蔽率)のみを書き込む
    return float4(0.0f, 0.0f, 0.0f, ao);
}

// AO/GIバッファ(rgb=間接拡散光, a=遮蔽率)を4チャンネルまとめて均す汎用ブラー。
//
// 【このパスは完全にサンプラー律速である】Intel UHD Graphics 620 / 1280x720 / DX11 / Release の
// 実測で2.21ms。1280x720の全画素×16タップ=14.7Mタップを、Gen9がRGBA16F(64bpp)のバイリニアを
// 半レート(約6.6 Gtexel/s)で回すと2.23msになり、実測とほぼ一致する。演算でも帯域でもなく
// 「タップ数×フォーマットのフィルタレート」だけで決まっているので、削るならタップ数を減らす。
//
// 【元の16タップが実際に作っていたカーネル】オフセットが{-1.5,-0.5,0.5,1.5}テクセル、つまり
// すべて半テクセルずれた位置=テクセルの角に落ちる。バイリニアはそこで周囲2テクセル(1軸あたり)を
// 1/2ずつ混ぜるので、1軸の重みを積み上げると{0.5, 1, 1, 1, 0.5}/4 という**5テクセル幅**の
// 分離可能カーネルになる。名前は「4x4ボックス」だが実体は5x5のテントである。
//
// 【9タップでまったく同じカーネルを作る】同じ重みは、隣接する2テクセルを1回のバイリニアで
// 拾う定番の畳み込みで1軸3タップに詰められる:
//   ・オフセット -4/3 テクセル … テクセル(-2,-1)を(1/3, 2/3)で混ぜる。タップ重み1.5/4
//   ・オフセット   0 テクセル … テクセル(0)ちょうど。タップ重み1.0/4
//   ・オフセット +4/3 テクセル … テクセル(+1,+2)を(2/3, 1/3)で混ぜる。タップ重み1.5/4
// 積み上げると1軸あたり{0.125, 0.25, 0.25, 0.25, 0.125}(合計1)で、元の{0.5,1,1,1,0.5}/4と
// **完全に一致する**。2軸とも分離可能なので3x3=9タップで済み、タップ数は16→9(-44%)になる。
//
// 【厳密には一致しない部分】元のオフセットは重みがちょうど1/2で、これは補間器の固定小数で
// 誤差なく表せる。新しい±4/3は1/3と2/3を要求するため、D3Dが保証するサブテクセル精度
// (小数8bit=1/256刻み)で最大0.0013ずれる。タップ重み0.375を掛けるとテクセルあたりの重み誤差は
// 0.0005以下で、[0,1]の遮蔽率に対して0.13/255相当。最終8bit出力の1LSBより小さい
static const float kAOBlurOffsets[3] = { -4.0f / 3.0f, 0.0f, 4.0f / 3.0f };
static const float kAOBlurWeights[3] = { 1.5f / 4.0f, 1.0f / 4.0f, 1.5f / 4.0f };

float4 PSMainBlur(PSInput input) : SV_TARGET
{
    uint width, height;
    Texture0.GetDimensions(width, height);
    float2 texelSize = 1.0f / float2(width, height);

    float4 sum = float4(0.0f, 0.0f, 0.0f, 0.0f);
    [unroll]
    for (int x = 0; x < 3; ++x)
    {
        [unroll]
        for (int y = 0; y < 3; ++y)
        {
            // offsetUVは画面端で[0,1]をはみ出すが、ColorSamplerはClampなので端のテクセルが
            // 引き伸ばされるだけで済む(Wrapのサンプラーで引くと反対側の端のAO/GIが混ざり、
            // 画面の四辺2px幅に無関係な遮蔽が滲む)
            float2 offsetUV = input.UV + float2(kAOBlurOffsets[x], kAOBlurOffsets[y]) * texelSize;
            sum += (kAOBlurWeights[x] * kAOBlurWeights[y]) * Texture0.Sample(ColorSampler, offsetUV);
        }
    }

    // 重みの総和が1になるよう作ってあるので、ここでの割り算は要らない
    return sum;
}
