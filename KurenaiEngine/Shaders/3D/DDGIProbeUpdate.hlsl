// DDGI(Dynamic Diffuse Global Illumination、22章)のプローブ更新パス。
//
// 入力は「プローブ1個ぶんのキャプチャキューブ」2本(放射輝度と距離)。反射プローブとまったく同じ
// キャプチャ経路(ProbeCapture.hlslの6面MRT → IBLConvolve.hlslのCSCopyCaptureToCubeFace)で
// 組み上げたもので、解像度だけkDDGICaptureSize(16)へ落としてある。
// 6面×16×16 = 1536テクセルが、そのままDDGIの「1536本のレイ」になる。
//
// 出力はオクタヘドラル投影の2Dアトラス2枚。
//   イラディアンス側 … 各テクセルが表す方向dを法線とみなした面が受ける光(E(d)/π)
//   距離側           … 各テクセルが表す方向dの、面までの平均距離と平均二乗距離
//
// 【なぜオクタヘドラルなのか】キューブマップは方向を6枚の正方形で表すが、オクタヘドラル投影は
// 1枚で表せる。DDGIはプローブを数百個置くので、この違いが決定的になる:
//   - 拡散イラディアンスは低周波なので6x6テクセルで足りる。キューブは最低6面必要で、
//     面あたり1x1では成立しない
//   - 数百個を1枚の2Dアトラスへ敷き詰められる。TextureCubeArrayで数百枚は非現実的
//
// 【なぜ距離だけ重みが違うのか】イラディアンスは半球全体をコサインで平均するのが物理的に正しいが、
// 距離を同じように平均してはいけない。「d方向の面までの距離」が欲しいのに90度横の面まで
// 混ざったら、そこから求める可視性判定が破綻するため。指数を大きく取った鋭い重みで、
// dのほぼ真正面だけを拾う。
#include "Samplers.hlsli"

static const float PI = 3.14159265359f;

cbuffer DDGIUpdateConstants : register(b0)
{
    // x=いま焼いているプローブの通し番号、y=ヒステリシス、z=距離モーメントのクランプ上限、
    // w=キャプチャキューブの1面の解像度
    float4 Params0;
    // x=イラディアンスの1辺のテクセル数(境界を含まない)、y=距離モーメントの1辺のテクセル数、
    // z=境界の幅、w=履歴を無視して上書きするフラグ
    float4 Params1;
    // xyz=各軸のプローブ数、w=このフレームの実効プリ露出
    float4 Params2;
};

TextureCube CaptureRadiance : register(t0);
TextureCube CaptureDistance : register(t1);

// 【R32系である必要がある】ヒステリシスのために前の値を読んでから書くが、型付きUAV読み出しは
// R32系しか保証されていない(それ以外はTypedUAVLoadAdditionalFormatsが要る)。
// AutoExposure.hlslが同じ理由でR32_Floatを2テクセル並べる構成を選んでいるのと同じ判断
RWTexture2D<float4> IrradianceAtlas : register(u0);
RWTexture2D<float2> DistanceAtlas : register(u1);

// 距離モーメントを求めるときの重みの鋭さ。実効半角は acos(0.5^(1/50)) ≒ 9.5度で、
// 立体角にすると球全体の約0.688%。1536本のレイのうち約10本がこの円錐に入る計算になる。
// 余裕は薄いので、遮蔽の輪郭がざらつく場合はここを下げる(下げるほど多くのレイを拾うが、
// 「d方向の距離」という意味からは遠ざかる)
static const float kDistanceSharpness = 50.0f;

// --- オクタヘドラル投影 ---
// 方向ベクトルを正方形[-1,1]^2へ写す。八面体 |x|+|y|+|z|=1 の表面へ射影し、
// 上半球(y>=0)はそのまま(x,z)を、下半球は正方形の四隅へ折り返す。
// キューブマップより歪みが均等で、面の選択も要らない
float2 DirectionToOctahedral(float3 direction)
{
    const float3 d = direction / max(abs(direction.x) + abs(direction.y) + abs(direction.z), 1e-8f);
    if (d.y >= 0.0f)
    {
        return float2(d.x, d.z);
    }
    // 下半球: |x|+|z| = 1-|y| の菱形を、四隅へ開くように折り返す
    return float2(
        (1.0f - abs(d.z)) * (d.x >= 0.0f ? 1.0f : -1.0f),
        (1.0f - abs(d.x)) * (d.z >= 0.0f ? 1.0f : -1.0f));
}

// DirectionToOctahedralの逆。正方形[-1,1]^2の座標から方向を復元する
float3 OctahedralToDirection(float2 oct)
{
    float3 direction = float3(oct.x, 1.0f - abs(oct.x) - abs(oct.y), oct.y);
    if (direction.y < 0.0f)
    {
        // 折り返しを戻す
        direction.xz = float2(
            (1.0f - abs(direction.z)) * (direction.x >= 0.0f ? 1.0f : -1.0f),
            (1.0f - abs(direction.x)) * (direction.z >= 0.0f ? 1.0f : -1.0f));
    }
    return normalize(direction);
}

// --- キャプチャキューブのレイ列挙 ---
// IBLConvolve.hlslのCubeFaceDirectionと同じ対応でなければならない。ここがずれると
// 「焼いた面の向き」と「読む向き」が食い違い、間接光が見当違いの方向から来る
float3 CubeFaceDirection(uint face, float2 uv)
{
    const float2 ndc = uv * 2.0f - 1.0f;
    const float u = ndc.x;
    const float v = ndc.y;

    float3 dir;
    if (face == 0)      dir = float3(1.0f, -v, -u);   // +X
    else if (face == 1) dir = float3(-1.0f, -v, u);   // -X
    else if (face == 2) dir = float3(u, 1.0f, v);     // +Y
    else if (face == 3) dir = float3(u, -1.0f, -v);   // -Y
    else if (face == 4) dir = float3(u, -v, 1.0f);    // +Z
    else                dir = float3(-u, -v, -1.0f);  // -Z

    return normalize(dir);
}

// キューブ面上の座標(u,v)∈[-1,1]^2 が張る立体角(定数倍を除く)。
//
// キューブマップのテクセルは平らな面の上では等間隔でも、球面へ投影すると面積が等しくない。
// テクセルは「プローブから見た小さな四角い窓」で、その立体角は
//   (1 / 距離²) × cos(窓の傾き),  距離² = u²+v²+1,  cos傾き = 1 / sqrt(u²+v²+1)
// なので (u²+v²+1)^(-3/2) になる。面の中心で1、面の隅(1,1)では 3^(-3/2) = 0.192 と
// 約1/5.2しかない。等重みで足すとキューブの対角線方向が5倍過剰に効いてしまう。
//
// 積分は最終的に重みの総和で正規化する(比になる)ので、定数倍は不要
float CubeTexelSolidAngleWeight(float2 uv)
{
    const float2 ndc = uv * 2.0f - 1.0f;
    const float lengthSq = ndc.x * ndc.x + ndc.y * ndc.y + 1.0f;
    return 1.0f / (lengthSq * sqrt(lengthSq));
}

// --- アトラス上の位置 ---
// 並びは 列 = ProbeCounts.x * ProbeCounts.y、行 = ProbeCounts.z。
// XY平面のスライスを横に並べ、Zを行にする。プローブ番号との対応は
// index = x + y*Cx + z*Cx*Cy で、C++側のRecreateDDGIAtlasesと一致させること
uint2 ProbeAtlasCell(uint probeIndex, uint3 probeCounts)
{
    const uint slice = probeCounts.x * probeCounts.y;
    const uint z = probeIndex / slice;
    const uint remainder = probeIndex - z * slice;
    return uint2(remainder, z);
}

// プローブ1個ぶんのセルの左上テクセル(境界を含む)
uint2 ProbeAtlasOrigin(uint probeIndex, uint3 probeCounts, uint texels, uint border)
{
    const uint cellSize = texels + border * 2u;
    return ProbeAtlasCell(probeIndex, probeCounts) * cellSize;
}

// 1536本のレイを走査して、方向dのイラディアンスと距離モーメントを求める。
// キャプチャキューブはDataSampler(Point)で引く。方向をテクセル中心から作っているので
// ちょうどそのテクセルが返る(補間させると存在しない中間の距離が作られてしまう)
void IntegrateRays(float3 d, float captureSize, float maxRayDistance,
                   out float3 irradiance, out float2 distanceMoments)
{
    float3 radianceSum = float3(0.0f, 0.0f, 0.0f);
    float cosineWeightSum = 0.0f;
    float2 momentSum = float2(0.0f, 0.0f);
    float sharpWeightSum = 0.0f;

    const uint size = (uint)captureSize;

    [loop]
    for (uint face = 0; face < 6; ++face)
    {
        [loop]
        for (uint y = 0; y < size; ++y)
        {
            [loop]
            for (uint x = 0; x < size; ++x)
            {
                const float2 uv = (float2(x, y) + 0.5f) / captureSize;
                const float3 rayDirection = CubeFaceDirection(face, uv);
                const float solidAngle = CubeTexelSolidAngleWeight(uv);

                const float cosTheta = dot(d, rayDirection);
                if (cosTheta <= 0.0f)
                {
                    continue;
                }

                // (1) イラディアンス: ランバートの余弦則そのもの。分母で正規化すると
                //     ∫max(0,cos)dω = π なので、得られる値はちょうど E(d)/π になる。
                //     このエンジンのイラディアンステクスチャは既に「1/πと積分のπを相殺済み」の
                //     規約(DeferredLighting.hlsl参照)なので、この形がそのまま使える
                const float cosineWeight = cosTheta * solidAngle;
                radianceSum += CaptureRadiance.SampleLevel(DataSampler, rayDirection, 0.0f).rgb * cosineWeight;
                cosineWeightSum += cosineWeight;

                // (2) 距離モーメント: dのほぼ真正面だけを拾う鋭い重み。
                //     クランプが要る理由はScene.hのGIVolume::MaxRayDistance参照
                //     (空に当たったレイの1e6をそのまま平均すると、分散が桁落ちで潰れる)
                // saturateは値としては何もしない(cosThetaは正規化ベクトル同士の内積で、
                // ここへ来る時点で(0,1]に収まっている)。コンパイラへ「負は来ない」と伝えて
                // pow()の警告X3571を消すために書いている
                const float sharpWeight = pow(saturate(cosTheta), kDistanceSharpness) * solidAngle;
                const float rayDistance = min(CaptureDistance.SampleLevel(DataSampler, rayDirection, 0.0f).r, maxRayDistance);
                momentSum += float2(rayDistance, rayDistance * rayDistance) * sharpWeight;
                sharpWeightSum += sharpWeight;
            }
        }
    }

    irradiance = radianceSum / max(cosineWeightSum, 1e-8f);
    distanceMoments = momentSum / max(sharpWeightSum, 1e-8f);
}

// プローブ1個ぶんのイラディアンスと距離モーメントを焼き直す。
// ディスパッチは1プローブにつき1回で、スレッドはイラディアンス側と距離側の広いほうに合わせて
// 起動し、それぞれの範囲外は自分で弾く(2つの解像度が違うため)
[numthreads(8, 8, 1)]
void CSUpdateProbe(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const uint probeIndex = (uint)Params0.x;
    const float hysteresis = Params0.y;
    const float maxRayDistance = Params0.z;
    const float captureSize = Params0.w;

    const uint irradianceTexels = (uint)Params1.x;
    const uint distanceTexels = (uint)Params1.y;
    const uint border = (uint)Params1.z;
    // 初回は履歴が無い。未初期化のアトラスと混ぜてはいけないのでヒステリシスを無視して上書きする
    const bool overwrite = Params1.w > 0.5f;

    const uint3 probeCounts = uint3((uint)Params2.x, (uint)Params2.y, (uint)Params2.z);
    const float preExposure = Params2.w;

    const uint2 texel = dispatchThreadID.xy;

    if (texel.x < irradianceTexels && texel.y < irradianceTexels)
    {
        // テクセル中心を[-1,1]へ写してから方向へ戻す
        const float2 oct = ((float2(texel) + 0.5f) / (float)irradianceTexels) * 2.0f - 1.0f;
        const float3 direction = OctahedralToDirection(oct);

        float3 irradiance;
        float2 unusedMoments;
        IntegrateRays(direction, captureSize, maxRayDistance, irradiance, unusedMoments);

        // 【露出非依存の単位で格納する】キャプチャした放射輝度にはCPU側で実効プリ露出が
        // 事前乗算されている(21.5節)。その倍率は時刻に連動して最大18段(約26万倍)動くため、
        // 掛かったまま溜めると「古い露出で焼かれた数値」を新しい露出の値として読むことになる。
        // DDGIは多重バウンスで自分自身へフィードバックするのでこのズレが増幅され、
        // 夜を挟んで昼に戻すと画面が数倍明るいまま戻らなくなる。
        // ここで割っておけば、ヒステリシスのブレンドも露出をまたいで整合する
        irradiance /= max(preExposure, 1e-12f);

        const uint2 writeAt = ProbeAtlasOrigin(probeIndex, probeCounts, irradianceTexels, border) + border + texel;
        const float3 previous = IrradianceAtlas[writeAt].rgb;
        const float3 blended = overwrite ? irradiance : lerp(irradiance, previous, hysteresis);
        IrradianceAtlas[writeAt] = float4(blended, 1.0f);
    }

    if (texel.x < distanceTexels && texel.y < distanceTexels)
    {
        const float2 oct = ((float2(texel) + 0.5f) / (float)distanceTexels) * 2.0f - 1.0f;
        const float3 direction = OctahedralToDirection(oct);

        float3 unusedIrradiance;
        float2 moments;
        IntegrateRays(direction, captureSize, maxRayDistance, unusedIrradiance, moments);

        const uint2 writeAt = ProbeAtlasOrigin(probeIndex, probeCounts, distanceTexels, border) + border + texel;
        const float2 previous = DistanceAtlas[writeAt];
        const float2 blended = overwrite ? moments : lerp(moments, previous, hysteresis);
        DistanceAtlas[writeAt] = blended;
    }
}

// オクタヘドラルの縁は球面上で対辺へ折り返して繋がっている。バイリニア補間がセルの縁で
// 正しい値を拾えるよう、その繋がる先のテクセルを外周へ複製する。
//
// 本体の書き込みが全て終わっていないと読む値が確定しないため、CSUpdateProbeとは別パスになる。
//
// 折り返しの規則(1辺Nの本体に対し、境界1テクセル):
//   上下の辺 … 横方向を反転した同じ辺の内側
//   左右の辺 … 縦方向を反転した同じ辺の内側
//   四隅     … 対角のテクセル
// これは正方形の縁を八面体の稜線とみなしたときの、隣り合う面の対応そのものである
uint2 OctahedralBorderSource(int2 borderTexel, int texels)
{
    int2 source = borderTexel;

    const bool onLeft = (borderTexel.x < 0);
    const bool onRight = (borderTexel.x >= texels);
    const bool onTop = (borderTexel.y < 0);
    const bool onBottom = (borderTexel.y >= texels);

    if ((onLeft || onRight) && (onTop || onBottom))
    {
        // 四隅は対角のテクセルへ対応する
        source.x = onLeft ? 0 : (texels - 1);
        source.y = onTop ? 0 : (texels - 1);
        source = int2(texels - 1 - source.x, texels - 1 - source.y);
    }
    else if (onLeft || onRight)
    {
        source.x = onLeft ? 0 : (texels - 1);
        source.y = texels - 1 - borderTexel.y;
    }
    else // onTop || onBottom
    {
        source.y = onTop ? 0 : (texels - 1);
        source.x = texels - 1 - borderTexel.x;
    }

    return (uint2)source;
}

[numthreads(8, 8, 1)]
void CSCopyBorder(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const uint probeIndex = (uint)Params0.x;
    const uint irradianceTexels = (uint)Params1.x;
    const uint distanceTexels = (uint)Params1.y;
    const uint border = (uint)Params1.z;
    const uint3 probeCounts = uint3((uint)Params2.x, (uint)Params2.y, (uint)Params2.z);

    // セル全体(境界込み)を走査し、境界のテクセルだけを埋める
    {
        const uint cellSize = irradianceTexels + border * 2u;
        if (dispatchThreadID.x < cellSize && dispatchThreadID.y < cellSize)
        {
            const int2 local = int2(dispatchThreadID.xy) - (int)border;
            const bool isBorder =
                local.x < 0 || local.y < 0 || local.x >= (int)irradianceTexels || local.y >= (int)irradianceTexels;
            if (isBorder)
            {
                const uint2 cellOrigin = ProbeAtlasOrigin(probeIndex, probeCounts, irradianceTexels, border);
                const uint2 source = OctahedralBorderSource(local, (int)irradianceTexels);
                IrradianceAtlas[cellOrigin + dispatchThreadID.xy] = IrradianceAtlas[cellOrigin + border + source];
            }
        }
    }

    {
        const uint cellSize = distanceTexels + border * 2u;
        if (dispatchThreadID.x < cellSize && dispatchThreadID.y < cellSize)
        {
            const int2 local = int2(dispatchThreadID.xy) - (int)border;
            const bool isBorder =
                local.x < 0 || local.y < 0 || local.x >= (int)distanceTexels || local.y >= (int)distanceTexels;
            if (isBorder)
            {
                const uint2 cellOrigin = ProbeAtlasOrigin(probeIndex, probeCounts, distanceTexels, border);
                const uint2 source = OctahedralBorderSource(local, (int)distanceTexels);
                DistanceAtlas[cellOrigin + dispatchThreadID.xy] = DistanceAtlas[cellOrigin + border + source];
            }
        }
    }
}
