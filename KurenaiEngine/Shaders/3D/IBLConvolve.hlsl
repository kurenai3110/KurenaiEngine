// IBL(Image Based Lighting)のオフスクリーン畳み込みパス。実行はエンジン起動時に一度だけ
// (スカイボックスは静的なため)。出力は本物のTextureCube(CreateUAVTextureCube/
// CreateMippedUAVTextureCubeで作成、面ごとに個別のUAV(Texture2DArray、要素数1)を持つ)で、
// HLSLがリソースを動的にスライス選択できないため、C++側(KurenaiEngine3D::Render)が
// 面ごとに1回ずつこのシェーダーをディスパッチする(カスケードシャドウマップのテクスチャ分岐と
// 同種の制約)。どの面を処理するかはIBLFaceConstants.Faceで受け取る。
//
// CSIrradiance: 拡散イラディアンス。出力テクセルが表す法線方向Nに対し、接空間の半球を
// (phi, theta)の等間隔グリッドで離散化してcosθ*sinθ重み付き積分する(Lambertian BRDFの1/πと
// 積分のπが相殺されるよう正規化済み。DeferredLighting.hlsl側は追加のπ処理なしでそのまま
// albedoに乗算できる)。出力テクセル1つあたり252×63=15,876サンプル、32²×6面で約9,750万サンプル。
//
// CSProjectSH / CSProjectSHFinal / CSEvaluateSH: 上と同じ拡散イラディアンスを、
// 球面調和関数(SH)L2(9項)で求める高速な経路(M11 Stage 4a)。CSIrradianceとの選択は
// C++側のトグルで行い、出力(IrradianceOut)の形・規約は完全に同じにしてある。詳細は
// このファイル後半のコメントを参照
//
// CSPrefilter: プリフィルタ済み鏡面。ミップレベルごとに異なるラフネスでGGXインポータンスサンプリングし、
// 出力テクセルが表す反射方向R周りのラディアンスをNdotL重み付き平均する。リアルタイムIBLの定番近似
// (Karis 2013, "Real Shading in Unreal Engine 4")に従いV=N=Rと仮定する(視線依存の歪みが出るが
// 実用上十分な近似として広く使われている)

static const float PI = 3.14159265359f;

#include "Samplers.hlsli"

TextureCube SourceSkybox : register(t0);

// IBLConvolve.hlsl側のこの宣言とC++側 KurenaiEngine3D.cpp の IBLFaceConstants を一致させる必要がある
cbuffer IBLFaceConstants : register(b0)
{
    uint Face;         // 処理対象の面(D3Dのキューブマップ標準順: +X=0,-X=1,+Y=2,-Y=3,+Z=4,-Z=5)
    float Roughness;   // CSPrefilterのみ使用。ミップが表すラフネス値
    // SHのウィンドウ関数(Sloan)の強さ。CSEvaluateSHのみ使用。0=無効(既定)。
    // 大きくするほど高次バンド(l=1,2)を減衰させ、打ち切り誤差によるリンギング
    // (小さく明るい光源の周りで暗部が負にオーバーシュートする現象)を、ボケと引き換えに抑える
    float SHWindowLambda;
    // CSProjectSH/CSProjectSHFinalが使う射影の離散化解像度(1面あたりの1辺のテクセル数)。
    // 【SourceSkyboxの実解像度とは無関係】SourceSkyboxはスカイボックスDDS(シーンごとに
    // 任意の解像度)や手続き空(256)など実行時に解像度が変わりうる一方、SampleLevelによる
    // 方向ベクトルでの参照は解像度に依存しないため、射影側の離散化密度は独立に選べる。
    // C++側はこの値でSHPartialSums/SHCoefficientsのディスパッチ数・バッファ容量を決めるため、
    // KurenaiEngine3D::kSHProjectionSizeと必ず一致させること(大きすぎるとバッファをはみ出す)
    float SHProjectionSize;
};

// キューブマップの1面上のUV([0,1]^2)から、その面・そのテクセルが表す方向を求める
// (D3Dの標準的な面→方向マッピング。KurenaiEngine3D.cpp/generate_sky_cubemap.pyの
// face_direction_gridと同じ規約に揃えている)
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

// キューブ面上の座標(u,v)∈[-1,1]^2が張る立体角(定数倍を除く、形状だけの項)。CSProjectSHが使う。
// キューブマップのテクセルは平らな面の上では等間隔でも、球面へ投影すると面積が等しくない。
// テクセルは「原点から見た小さな四角い窓」で、その立体角は
//   (1/距離²)×cos(窓の傾き)、距離²=u²+v²+1、cos傾き=1/sqrt(u²+v²+1)
// なので(u²+v²+1)^(-3/2)になる。面の中心で1、面の隅(1,1)では3^(-3/2)≈0.192と約1/5.2しかない。
// 等重みで足すとキューブの対角線方向が5倍過剰に効いてしまう。
// (DDGIProbeUpdate.hlslのCubeTexelSolidAngleWeightと同じ式)
//
// 【注意: DDGIProbeUpdate.hlslとは違い、ここでは絶対値の立体角(ステラジアン)が要る】
// DDGI側はレイの重み付き平均(分母で正規化して比にする)なので欠けている定数倍は相殺されて
// 消えるが、CSProjectSHは正規化しない絶対積分(∫L·Y dω)を求めるため、テクセル1個が
// 実際に張る立体角[sr]まで正しくスケールする必要がある。1テクセルのUV面積は
// (2/captureSize)² であり、呼び出し側でこの関数の戻り値に 4/(captureSize²) を掛けて使うこと
// (掛け忘れるとWhite Furnace Testの入出力が一致しない形で表面化する)
float CubeTexelSolidAngleWeight(float2 uv)
{
    const float2 ndc = uv * 2.0f - 1.0f;
    const float lengthSq = ndc.x * ndc.x + ndc.y * ndc.y + 1.0f;
    return 1.0f / (lengthSq * sqrt(lengthSq));
}

// 反射プローブのキャプチャ結果(ProbeCapture.hlslが2Dレンダーターゲットへ描いた1面ぶん)を、
// キューブマップの該当面へ書き写す。キューブマップへ直接描画する仕組み(面ごとのRTV)を
// RHIが持たないため、この経路でキューブマップを組み立てる。
// このシェーダーがIBLConvolve.hlsl側にあるのは、面→方向の対応(CubeFaceDirection)を
// 畳み込み側と1文字も違わず共有する必要があるため(ここがずれると焼いた面の向きが食い違う)。
//
// ジオメトリが描かれなかった(深度が書き込まれなかった)ピクセルは、その面・そのテクセルが
// 表す方向でスカイボックスを引いて埋める。夜間の減衰はここでは掛けない
// (プローブを使う側のEvaluateIBLが実行時に改めて掛けるため、焼き込み時にも掛けると二重になる)
// 同時に、キャプチャの2枚目のレンダーターゲット(プローブからの距離)を距離キューブ配列の
// 該当面へも書き写す(19.12節)。こちらは畳み込まないのでスクラッチのキューブマップを経由せず、
// プローブごとのスライスへ直接書く。ジオメトリが無かった方向にはkProbeSkyDistanceを入れ、
// 「その向きには何も無い」ことを表す
Texture2D CaptureColor : register(t1);
Texture2D CaptureDepth : register(t2);
Texture2D CaptureDistance : register(t3);
RWTexture2DArray<float4> ProbeRadianceOut : register(u0);
RWTexture2DArray<float> ProbeDistanceOut : register(u1);

// 空(ジオメトリ無し)を表す距離。ReflectionProbe.hlsli側はこの値そのものを判定に使わず、
// 「十分に遠いので交差もしないし遮蔽もしない」として自然に扱えるだけの大きさがあればよい
static const float kProbeSkyDistance = 1.0e6f;

[numthreads(8, 8, 1)]
void CSCopyCaptureToCubeFace(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint width, height, elements;
    ProbeRadianceOut.GetDimensions(width, height, elements);
    if (dispatchThreadID.x >= width || dispatchThreadID.y >= height)
    {
        return;
    }

    // キャプチャ用レンダーターゲットとキューブ面は同じ解像度で確保しているため、
    // テクセルは1対1で対応する(補間を挟まないようLoadで読む)
    const int3 texel = int3(int2(dispatchThreadID.xy), 0);
    const float depth = CaptureDepth.Load(texel).r;

    float3 radiance;
    float distance;
    if (depth <= 0.0f)
    {
        // Reverse-Zのため、何も描かれなかった背景はNDC z=0.0のまま
        const float2 uv = (float2(dispatchThreadID.xy) + 0.5f) / float2(width, height);
        const float3 direction = CubeFaceDirection(Face, uv);
        radiance = SourceSkybox.SampleLevel(MaterialSampler, direction, 0.0f).rgb;
        distance = kProbeSkyDistance;
    }
    else
    {
        radiance = CaptureColor.Load(texel).rgb;
        distance = CaptureDistance.Load(texel).r;
    }

    ProbeRadianceOut[uint3(dispatchThreadID.xy, 0)] = float4(radiance, 1.0f);
    ProbeDistanceOut[uint3(dispatchThreadID.xy, 0)] = distance;
}

RWTexture2DArray<float4> IrradianceOut : register(u0);

[numthreads(8, 8, 1)]
void CSIrradiance(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint width, height, elements;
    IrradianceOut.GetDimensions(width, height, elements);
    if (dispatchThreadID.x >= width || dispatchThreadID.y >= height)
    {
        return;
    }

    const float2 uv = (float2(dispatchThreadID.xy) + 0.5f) / float2(width, height);
    const float3 N = CubeFaceDirection(Face, uv);
    const float3 up = abs(N.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
    const float3 tangentX = normalize(cross(up, N));
    const float3 tangentY = cross(N, tangentX);

    float3 irradiance = float3(0.0f, 0.0f, 0.0f);
    uint sampleCount = 0;

    const float kPhiStep = 0.025f;
    const float kThetaStep = 0.025f;

    [loop]
    for (float phi = 0.0f; phi < 2.0f * PI; phi += kPhiStep)
    {
        [loop]
        for (float theta = 0.0f; theta < 0.5f * PI; theta += kThetaStep)
        {
            const float3 tangentSample = float3(sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta));
            const float3 sampleDir = tangentX * tangentSample.x + tangentY * tangentSample.y + N * tangentSample.z;

            irradiance += SourceSkybox.SampleLevel(MaterialSampler, sampleDir, 0.0f).rgb * cos(theta) * sin(theta);
            sampleCount += 1;
        }
    }

    irradiance = PI * irradiance / max(float(sampleCount), 1.0f);
    IrradianceOut[uint3(dispatchThreadID.xy, 0)] = float4(irradiance, 1.0f);
}

// ============================================================================
// M11 Stage 4a: 拡散イラディアンスの球面調和関数(SH L2)化
// ============================================================================
//
// CSIrradianceは出力テクセル1つあたり半球を総当たりで積分するため、ソースの1テクセルを
// 平均992回読み直している(docs/Architecture.html 19.10節/M11ロードマップの実測見積もり)。
// 拡散イラディアンス E(N) = ∫ L(ω)max(0,cosθ)dω はコサインローブによる畳み込みであり、
// この畳み込みはSHのL2(9項)へほぼ完全に収まる(Ramamoorthi & Hanrahan 2001)。
// そこでソースを1回だけ読んで9個の係数(RGBなので実数27個)へ射影し、出力テクセルでは
// その9個の係数を評価するだけにする。
//
// 3つのエントリポイントに分かれている。
//   CSProjectSH      ソースキューブ全体(6面×captureSize²)を1回だけ読み、
//                    L(ω)·Y_k(ω)·ΔΩ(ω) の総和(k=0..8)をグループ単位の部分和として書き出す。
//                    98,304テクセル(128²×6面)を1グループには収められないため、
//                    グループごとにgroupshared配列で64→1のツリー還元を行い、
//                    その結果を構造化バッファ(SHPartialSums)へ1グループ1エントリで積む
//   CSProjectSHFinal 全グループぶんの部分和(最大1,536個)を1ディスパッチで合算し、
//                    最終的な9個の係数(SHCoefficients)を書く。同じツリー還元を64スレッドで行う
//   CSEvaluateSH     9個の係数から出力テクセルのirradiance = E(N)/πを評価する。
//                    IrradianceOutへ書く形はCSIrradianceと完全に同じなので、呼び出し側は
//                    パイプラインを差し替えるだけでよい
//
// 精度: 「Â_l(コサインローブのSH係数)がレベル3以降をほぼ完全に消す」性質により、
// L2はどんな照明でも数%以内の誤差に収まる。ただしエミッシブ帯のような小さく明るい光源では
// 打ち切り誤差がリンギング(暗部の負のオーバーシュート)として出ることがある。SHWindowLambda
// (既定0=無効)はこれを抑えるウィンドウ関数の強さで、実測でリンギングが出た場合につまみとして使う。
//
// 【White Furnace Testでの検算】一様な環境 L(ω)=c は l=0成分にしか射影されない
// (coeff0 = c·Y0·4π、他は0)。評価側は irradiance = coeff0·Y0·(Â0/π) で、
// Y0=sqrt(1/4π)なので Y0²·4π=1、つまり irradiance = c·1.0 と厳密に一致する。
// ここが合わない場合は規約(1/πの相殺)・立体角重み・畳み込み係数のいずれかが誤っている

// 実数のSH基底関数(l<=2、直交正規化: ∫Y_i·Y_j dω = δ_ij)。方向dは単位ベクトル
void EvaluateSHBasis(float3 d, out float basis[9])
{
    basis[0] = 0.282095f;
    basis[1] = 0.488603f * d.y;
    basis[2] = 0.488603f * d.z;
    basis[3] = 0.488603f * d.x;
    basis[4] = 1.092548f * d.x * d.y;
    basis[5] = 1.092548f * d.y * d.z;
    basis[6] = 0.315392f * (3.0f * d.z * d.z - 1.0f);
    basis[7] = 1.092548f * d.x * d.z;
    basis[8] = 0.546274f * (d.x * d.x - d.y * d.y);
}

static const uint kSHCoeffCount = 9;
static const uint kSHGroupThreads = 64; // 8x8

RWStructuredBuffer<float4> SHPartialSums : register(u0);

// [k][threadIndex]。9係数×64スレッド×16バイト(float4) = 9,216バイト。TGSM上限(通常32KB)に十分収まる
groupshared float4 gsSHProject[kSHCoeffCount][kSHGroupThreads];

// Dispatch((SHProjectionSize+7)/8, (SHProjectionSize+7)/8, 6)で呼ぶこと。
// 1グループが1面の8x8タイルを担当する
[numthreads(8, 8, 1)]
void CSProjectSH(uint3 dispatchThreadID : SV_DispatchThreadID, uint3 groupID : SV_GroupID, uint groupIndex : SV_GroupIndex)
{
    // SourceSkyboxの実解像度ではなく、C++側が指定した射影の離散化解像度を使う
    // (cbuffer宣言のコメント参照。SourceSkyboxはSampleLevelで方向ベクトル参照するため、
    // 離散化解像度と実解像度が一致している必要はない)
    const uint width = (uint)SHProjectionSize;
    const uint height = (uint)SHProjectionSize;

    float4 partial[kSHCoeffCount];
    [unroll]
    for (uint k0 = 0; k0 < kSHCoeffCount; ++k0)
    {
        partial[k0] = float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    const uint face = groupID.z;
    const uint2 texel = dispatchThreadID.xy;
    if (texel.x < width && texel.y < height)
    {
        const float2 uv = (float2(texel) + 0.5f) / float2(width, height);
        const float3 dir = CubeFaceDirection(face, uv);
        // CubeTexelSolidAngleWeightは形状だけの項なので、1テクセルのUV面積ぶんの定数
        // 4/(width*height)を掛けて絶対立体角[sr]にする(このファイル冒頭のコメント参照)
        const float solidAngle = CubeTexelSolidAngleWeight(uv) * (4.0f / (float(width) * float(height)));
        const float3 weighted = SourceSkybox.SampleLevel(MaterialSampler, dir, 0.0f).rgb * solidAngle;

        float basis[9];
        EvaluateSHBasis(dir, basis);
        [unroll]
        for (uint k1 = 0; k1 < kSHCoeffCount; ++k1)
        {
            partial[k1] = float4(weighted * basis[k1], 0.0f);
        }
    }

    [unroll]
    for (uint k2 = 0; k2 < kSHCoeffCount; ++k2)
    {
        gsSHProject[k2][groupIndex] = partial[k2];
    }
    GroupMemoryBarrierWithGroupSync();

    // 64→1のツリー還元(kSHGroupThreadsはコンパイル時定数なのでunroll可能)
    [unroll]
    for (uint stride = kSHGroupThreads / 2; stride > 0; stride >>= 1)
    {
        if (groupIndex < stride)
        {
            [unroll]
            for (uint k3 = 0; k3 < kSHCoeffCount; ++k3)
            {
                gsSHProject[k3][groupIndex] += gsSHProject[k3][groupIndex + stride];
            }
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (groupIndex == 0)
    {
        // グループ番号の並びはCSProjectSHFinal側の走査順(x + y*groupsX + face*groupsX*groupsY)と
        // 一致させること
        const uint groupsX = (width + 7) / 8;
        const uint groupsY = (height + 7) / 8;
        const uint groupLinear = groupID.x + groupID.y * groupsX + face * groupsX * groupsY;
        [unroll]
        for (uint k4 = 0; k4 < kSHCoeffCount; ++k4)
        {
            SHPartialSums[groupLinear * kSHCoeffCount + k4] = gsSHProject[k4][0];
        }
    }
}

StructuredBuffer<float4> SHPartialSumsIn : register(t1);
RWStructuredBuffer<float4> SHCoefficientsOut : register(u0);

groupshared float4 gsSHFinal[kSHCoeffCount][kSHGroupThreads];

// Dispatch(1, 1, 1)で呼ぶこと。CSProjectSHが書いた全グループぶんの部分和を64スレッドで合算する
[numthreads(64, 1, 1)]
void CSProjectSHFinal(uint groupIndex : SV_GroupIndex)
{
    // CSProjectSHと同じ解像度でなければグループ番号の対応がずれる
    const uint width = (uint)SHProjectionSize;
    const uint height = (uint)SHProjectionSize;
    const uint groupsX = (width + 7) / 8;
    const uint groupsY = (height + 7) / 8;
    const uint totalGroups = groupsX * groupsY * 6;

    float4 partial[kSHCoeffCount];
    [unroll]
    for (uint k5 = 0; k5 < kSHCoeffCount; ++k5)
    {
        partial[k5] = float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    // totalGroupsは実行時値(ソースキューブの解像度依存)なのでunrollしない
    [loop]
    for (uint i = groupIndex; i < totalGroups; i += kSHGroupThreads)
    {
        [unroll]
        for (uint k6 = 0; k6 < kSHCoeffCount; ++k6)
        {
            partial[k6] += SHPartialSumsIn[i * kSHCoeffCount + k6];
        }
    }

    [unroll]
    for (uint k7 = 0; k7 < kSHCoeffCount; ++k7)
    {
        gsSHFinal[k7][groupIndex] = partial[k7];
    }
    GroupMemoryBarrierWithGroupSync();

    [unroll]
    for (uint stride2 = kSHGroupThreads / 2; stride2 > 0; stride2 >>= 1)
    {
        if (groupIndex < stride2)
        {
            [unroll]
            for (uint k8 = 0; k8 < kSHCoeffCount; ++k8)
            {
                gsSHFinal[k8][groupIndex] += gsSHFinal[k8][groupIndex + stride2];
            }
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (groupIndex == 0)
    {
        [unroll]
        for (uint k9 = 0; k9 < kSHCoeffCount; ++k9)
        {
            SHCoefficientsOut[k9] = gsSHFinal[k9][0];
        }
    }
}

StructuredBuffer<float4> SHCoefficientsFinal : register(t1);

// CSIrradianceと同じIrradianceOut(register u0)へ書く。呼び出し側の面ごとディスパッチ・
// 出力の意味はCSIrradianceと完全に同一
[numthreads(8, 8, 1)]
void CSEvaluateSH(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint width, height, elements;
    IrradianceOut.GetDimensions(width, height, elements);
    if (dispatchThreadID.x >= width || dispatchThreadID.y >= height)
    {
        return;
    }

    const float2 uv = (float2(dispatchThreadID.xy) + 0.5f) / float2(width, height);
    const float3 N = CubeFaceDirection(Face, uv);

    float basis[9];
    EvaluateSHBasis(N, basis);

    // Â_l/π(コサインローブの畳み込み係数に、このエンジンの「1/πと積分のπを相殺済み」規約の
    // /πを先に折り込んである): band0: Â0/π=1、band1: Â1/π=2/3、band2: Â2/π=1/4
    const float lambda = SHWindowLambda;
    const float w1 = 1.0f / (1.0f + lambda * 4.0f);  // band1: l(l+1)=1*2=2, l^2(l+1)^2=4
    const float w2 = 1.0f / (1.0f + lambda * 36.0f); // band2: l(l+1)=2*3=6, l^2(l+1)^2=36

    float3 irradiance = SHCoefficientsFinal[0].rgb * basis[0];
    irradiance += (SHCoefficientsFinal[1].rgb * basis[1]
                 + SHCoefficientsFinal[2].rgb * basis[2]
                 + SHCoefficientsFinal[3].rgb * basis[3]) * ((2.0f / 3.0f) * w1);
    irradiance += (SHCoefficientsFinal[4].rgb * basis[4]
                 + SHCoefficientsFinal[5].rgb * basis[5]
                 + SHCoefficientsFinal[6].rgb * basis[6]
                 + SHCoefficientsFinal[7].rgb * basis[7]
                 + SHCoefficientsFinal[8].rgb * basis[8]) * (0.25f * w2);

    IrradianceOut[uint3(dispatchThreadID.xy, 0)] = float4(irradiance, 1.0f);
}

RWTexture2DArray<float4> PrefilterOut : register(u0);

float RadicalInverseVdC_P(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10f;
}

float2 Hammersley_P(uint i, uint n)
{
    return float2(float(i) / float(n), RadicalInverseVdC_P(i));
}

float3 ImportanceSampleGGX_P(float2 xi, float3 N, float roughness)
{
    const float a = roughness * roughness;

    const float phi = 2.0f * PI * xi.x;
    const float cosTheta = sqrt((1.0f - xi.y) / (1.0f + (a * a - 1.0f) * xi.y));
    const float sinTheta = sqrt(1.0f - cosTheta * cosTheta);

    float3 H;
    H.x = sinTheta * cos(phi);
    H.y = sinTheta * sin(phi);
    H.z = cosTheta;

    const float3 up = abs(N.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
    const float3 tangentX = normalize(cross(up, N));
    const float3 tangentY = cross(N, tangentX);

    return tangentX * H.x + tangentY * H.y + N * H.z;
}

static const uint kPrefilterSampleCount = 256;

[numthreads(8, 8, 1)]
void CSPrefilter(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint width, height, elements;
    PrefilterOut.GetDimensions(width, height, elements);
    if (dispatchThreadID.x >= width || dispatchThreadID.y >= height)
    {
        return;
    }

    const float2 uv = (float2(dispatchThreadID.xy) + 0.5f) / float2(width, height);
    const float3 N = CubeFaceDirection(Face, uv);

    // リアルタイムIBLの定番近似(Karis 2013): V=R=Nと仮定し、視線依存の伸び(斜め視線での
    // 反射像の伸長)を無視する
    const float3 V = N;

    if (Roughness < 1e-3f)
    {
        // ラフネス0(ミップ0)は鏡面そのものなので畳み込み不要。そのままスカイボックスをコピーする
        PrefilterOut[uint3(dispatchThreadID.xy, 0)] = float4(SourceSkybox.SampleLevel(MaterialSampler, N, 0.0f).rgb, 1.0f);
        return;
    }

    float3 prefilteredColor = float3(0.0f, 0.0f, 0.0f);
    float totalWeight = 0.0f;

    [loop]
    for (uint i = 0; i < kPrefilterSampleCount; ++i)
    {
        const float2 xi = Hammersley_P(i, kPrefilterSampleCount);
        const float3 H = ImportanceSampleGGX_P(xi, N, Roughness);
        const float3 L = normalize(2.0f * dot(V, H) * H - V);

        const float NdotL = saturate(dot(N, L));
        if (NdotL > 0.0f)
        {
            prefilteredColor += SourceSkybox.SampleLevel(MaterialSampler, L, 0.0f).rgb * NdotL;
            totalWeight += NdotL;
        }
    }

    prefilteredColor = (totalWeight > 0.0f) ? (prefilteredColor / totalWeight) : SourceSkybox.SampleLevel(MaterialSampler, N, 0.0f).rgb;
    PrefilterOut[uint3(dispatchThreadID.xy, 0)] = float4(prefilteredColor, 1.0f);
}
