// SkyView LUT(P14b)のパラメータ化。焼く側(AtmosphereLUT.hlsl CSSkyView)と
// 引く側(Sky.hlsli SkyColorUpperUnit)の両方が使う共有ヘッダー。
//
// 【なぜ別ファイルにするのか】この写像は「方向 → UV」と「UV → 方向」が厳密に逆でなければ
// ならない。片方だけ直すと空全体が歪むが、絵として破綻せずズレるだけなので気づきにくい。
// AtmosphereLUT.hlslはUAVを持つコンピュート専用でSky.hlsliからincludeできないため、
// 共通部分だけをここへ括り出して単一定義点にする(NormalEncoding.hlsli等と同じ作法)。
//
// 【単位はkm】AtmosphereLUT.hlslと揃える。

#ifndef KURENAI_ATMOSPHERE_COMMON_HLSLI
#define KURENAI_ATMOSPHERE_COMMON_HLSLI

static const float kAtmospherePI = 3.14159265359f;

// 地球の半径と大気圏上端。Hillaire (2020) / Bruneton-Neyret (2008) の標準値
static const float kBottomRadiusKm = 6360.0f;
static const float kTopRadiusKm = 6460.0f;

// SkyView LUTを焼く高度。地表から1m。
//
// 【なぜカメラの実際の高度を使わないのか】3つ理由がある。
//  1. このエンジンの空と地面の切り分けは既に「dir.y の符号」つまり高度0の地平線を前提にしている
//     (SkyColorUpperは「水平線以上」と定義され、呼び出し側が地面と合成する)。LUTの地平線も
//     ちょうどそこへ来るのが最も素直で、地平線の位置がカメラの上下移動でずれない
//  2. 大気の厚さ100kmに対してこのエンジンのカメラ高度(数m〜数百m)は無視できる。高度100mでも
//     地平線の天頂角の変化は0.32度しかなく、空の色そのものはさらに変わらない
//  3. SkyGenerate.hlsl(IBLキューブ)とSkyIntegrate.hlsl(照度積分)はカメラ位置を持たない。
//     カメラ高度を要求すると、この2つへカメラ位置を新たに配線することになる
//
// ちょうど地表(6360.0)にしないのは、レイと地球の交差判定が接する退化ケースを避けるため。
// 1m浮かせておけば上向きのレイは確実に地面と交わらない
static const float kSkyViewHeightKm = kBottomRadiusKm + 0.001f;

// LUTの解像度。KurenaiEngine3D.h の kSkyViewLUTWidth / kSkyViewLUTHeight と一致させること
// (UVの半テクセル補正に解像度が要るため、シェーダ側にも同じ値が必要になる)
static const float kSkyViewLUTWidthF = 192.0f;
static const float kSkyViewLUTHeightF = 108.0f;

// UVの端を「テクセルの中心」に合わせる補正(Hillaireの fromUnitToSubUvs / fromSubUvsToUnit)。
// これが無いとu=0(太陽の子午線)とv=0(天頂)が最初のテクセルの中心から半テクセルずれる。
// 天頂は輝度の正規化基準そのものなので、ここは合わせておきたい
float AtmosphereUnitToSubUv(float u, float resolution)
{
    return (u + 0.5f / resolution) * (resolution / (resolution + 1.0f));
}

float AtmosphereSubUvToUnit(float u, float resolution)
{
    return (u - 0.5f / resolution) * (resolution / (resolution - 1.0f));
}

// 太陽・視線の水平成分(XZ平面へ落として正規化)。真上を向いていると退化するので、
// そのときは+X軸を返す。どの方位も等価なので何を選んでもよいが、
// **焼く側と引く側で必ず同じ選択でなければならない**のでここに1つだけ置く
float3 AtmosphereHorizontalDirection(float3 v)
{
    const float2 h = v.xz;
    const float lengthSq = dot(h, h);
    if (lengthSq < 1e-12f)
    {
        return float3(1.0f, 0.0f, 0.0f);
    }
    const float invLength = rsqrt(lengthSq);
    return float3(h.x * invLength, 0.0f, h.y * invLength);
}

// 地平線の天頂角[ラジアン]と、地平線より下に残る角度幅。
// 高度kSkyViewHeightKmではzenithHorizonAngleはほぼ90度になる
void SkyViewHorizonAngles(out float zenithHorizonAngle, out float beta)
{
    const float horizonDistance =
        sqrt(max(kSkyViewHeightKm * kSkyViewHeightKm - kBottomRadiusKm * kBottomRadiusKm, 0.0f));
    const float cosBeta = horizonDistance / max(kSkyViewHeightKm, 1e-6f);
    beta = acos(clamp(cosBeta, -1.0f, 1.0f));
    zenithHorizonAngle = kAtmospherePI - beta;
}

// --- 方向 → UV ---
//
// u: 太陽の子午線からの方位。**空は太陽の子午線について左右対称**なので、方位差の余弦だけを
//    使って[0,1]へ畳む(u=0が太陽側、u=1が反太陽側)。両端が対称面なので折り返しの継ぎ目が
//    構造的に出ず、アドレスモードはClamp(s1 ColorSampler)でよい。実効解像度も2倍になる。
//    sqrtを噛ませて太陽の近く(前方散乱で変化が急な側)へ分解能を寄せる
//
// v: 天頂角。地平線(v=0.5)の近くへ分解能を寄せる。1-sqrt(1-x)の形なのは、
//    地平線側で写像の傾きが大きくなるようにするため(単純なsqrtだと天頂側が密になり逆効果)
//
// 【dirは正規化されていなくてよい】この関数はdir.yを天頂角の余弦、dir.xzを方位として
// **独立に**読む。Sky.hlsliはyだけを地平線の下限でクランプした非正規化のベクトルを渡してくる
// (真下を向いた視線でも「地平線のすぐ上」を引きたいため。正規化してしまうと真下が天頂へ化ける)
float2 SkyViewDirectionToUv(float3 dir, float3 sunDir)
{
    float zenithHorizonAngle, beta;
    SkyViewHorizonAngles(zenithHorizonAngle, beta);

    const float viewZenithAngle = acos(clamp(dir.y, -1.0f, 1.0f));

    float v;
    if (viewZenithAngle < zenithHorizonAngle)
    {
        float coord = viewZenithAngle / max(zenithHorizonAngle, 1e-6f);
        coord = 1.0f - coord;
        coord = sqrt(max(coord, 0.0f));
        coord = 1.0f - coord;
        v = coord * 0.5f;
    }
    else
    {
        float coord = (viewZenithAngle - zenithHorizonAngle) / max(beta, 1e-6f);
        coord = sqrt(max(coord, 0.0f));
        v = coord * 0.5f + 0.5f;
    }

    const float3 dirHorizontal = AtmosphereHorizontalDirection(dir);
    const float3 sunHorizontal = AtmosphereHorizontalDirection(sunDir);
    const float lightViewCos = clamp(dot(dirHorizontal, sunHorizontal), -1.0f, 1.0f);
    const float u = sqrt(saturate(-lightViewCos * 0.5f + 0.5f));

    return float2(AtmosphereUnitToSubUv(u, kSkyViewLUTWidthF),
                  AtmosphereUnitToSubUv(v, kSkyViewLUTHeightF));
}

// --- UV → 方向 ---
// 上のSkyViewDirectionToUvの厳密な逆写像。sunDirを基準にした座標系で方向を組み立てる。
// 方位は対称に畳んであるため、太陽の左右どちらへ復元しても同じ空の色になる
float3 SkyViewUvToDirection(float2 uv, float3 sunDir)
{
    float zenithHorizonAngle, beta;
    SkyViewHorizonAngles(zenithHorizonAngle, beta);

    const float u = AtmosphereSubUvToUnit(uv.x, kSkyViewLUTWidthF);
    const float v = AtmosphereSubUvToUnit(uv.y, kSkyViewLUTHeightF);

    float viewZenithAngle;
    if (v < 0.5f)
    {
        float coord = 2.0f * v;
        coord = 1.0f - coord;
        coord = coord * coord;
        coord = 1.0f - coord;
        viewZenithAngle = coord * zenithHorizonAngle;
    }
    else
    {
        float coord = 2.0f * v - 1.0f;
        coord = coord * coord;
        viewZenithAngle = zenithHorizonAngle + coord * beta;
    }

    const float lightViewCos = -(u * u * 2.0f - 1.0f);

    const float cosViewZenith = cos(viewZenithAngle);
    const float sinViewZenith = sin(viewZenithAngle);
    const float sinAzimuth = sqrt(saturate(1.0f - lightViewCos * lightViewCos));

    const float3 sunHorizontal = AtmosphereHorizontalDirection(sunDir);
    // 太陽の子午線に直交する水平方向。upが+Yなので外積で求まる
    const float3 sunSide = float3(-sunHorizontal.z, 0.0f, sunHorizontal.x);

    return sunHorizontal * (lightViewCos * sinViewZenith) +
           sunSide * (sinAzimuth * sinViewZenith) +
           float3(0.0f, cosViewZenith, 0.0f);
}

#endif // KURENAI_ATMOSPHERE_COMMON_HLSLI
