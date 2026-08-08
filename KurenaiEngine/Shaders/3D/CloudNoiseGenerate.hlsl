// ボリュメトリック雲の3Dノイズを焼くコンピュートシェーダー(P13a)。
//
// 起動後に一度だけディスパッチして2枚の3Dテクスチャを作る(BRDF積分LUT・IBLの畳み込みと
// まったく同じ「最初のフレームで一度だけ焼く」作法。KurenaiEngine3D::Render の m_*Baked 参照)。
//   CSGenerateShape  … 128^3 RGBA8。雲の大まかな塊の形を決める
//   CSGenerateDetail …  32^3 RGBA8。塊の縁を削って房状にする
//
// 【なぜ手続きfBmではなくテクスチャに焼くのか】現在の平面レイヤー(P5〜P12)は
// Sky.hlsli の CloudFbm を画素ごとに評価している。これは4オクターブ×格子4隅=16回の
// ハッシュで、1画素につき1回なら十分安いがレイマーチでは1画素につき何十回も評価することになる。
// 焼いておけばトライリニアフェッチ1回で済み、しかも3次元の形をそのまま持てる。
//
// 【タイル可能でなければならない】雲のUVWはワールド座標から作るため無限に大きくなる。
// 有限のテクスチャを繰り返して使うので、格子セルの番号を必ず周期で巻き戻す
// (下の WrapCell)。ここを外すとタイル境界に格子状の筋が出る。
// 巻き戻しに floor ベースの剰余を使うのは Sky.hlsli の CloudPeriodicHash と同じ理由で、
// 素朴な fmod だと負のセル座標で負の値が返り隣接セルの参照がずれる。
//
// 【サンプリング側の注意】読む側は必ず Samplers.hlsli の s3 VolumeSampler(Linear + Wrap)で
// 引くこと。Clamp で引くと周期の境界でトライリニア補間のタップが端のテクセルに張り付き、
// 一定間隔で継ぎ目が出る(シェーダー側で frac() しても補間そのものが端を跨げないため消せない)。

// ============================================================================
// ハッシュと格子
// ============================================================================

// 3D→3D のハッシュ。Worleyノイズの特徴点をセル内のどこに置くかを決めるのに使う。
// Sky.hlsli の CloudHash12 と同系統(Dave Hoskins の hash 群)で、次元だけ 3→3 にしたもの
float3 CloudNoiseHash33(float3 p)
{
    float3 p3 = frac(p * float3(0.1031f, 0.1030f, 0.0973f));
    p3 += dot(p3, p3.yxz + 33.33f);
    return frac((p3.xxy + p3.yxx) * p3.zyx);
}

// 3D→1D のハッシュ。値ノイズ(Perlin代わり)の格子点の値を決めるのに使う
float CloudNoiseHash13(float3 p)
{
    float3 p3 = frac(p * 0.1031f);
    p3 += dot(p3, p3.zyx + 31.32f);
    return frac((p3.x + p3.y) * p3.z);
}

// セル番号を周期 period で巻き戻す。floor ベースなので負のセル座標でも常に [0, period) に収まる
float3 WrapCell(float3 cell, float period)
{
    return cell - period * floor(cell / period);
}

// ============================================================================
// ノイズ
// ============================================================================

// 周期化した3DのWorleyノイズ。返すのは「最も近い特徴点までの距離」を反転した値なので、
// 1に近いほど特徴点の近く(=雲の芯)になる。cellCount がテクスチャ1周ぶんのセル数
float WorleyNoise(float3 uvw, float cellCount)
{
    const float3 scaled = uvw * cellCount;
    const float3 baseCell = floor(scaled);
    const float3 local = scaled - baseCell;

    float minDistanceSq = 1e9f;
    // 隣接27セルの特徴点を調べる。セル内のどこに点があるかで最近傍が隣のセルに来るため、
    // 3x3x3 を見ないと距離が飛ぶ。
    //
    // 【[unroll]しない】このループは呼び出し元(WorleyFbm 3オクターブ × CSGenerateShapeで4回)を
    // 通すと1シェーダーあたり324回ぶんの展開になり、fxcのコンパイルが実測4.4秒(Shape 2.7秒 +
    // Detail 1.7秒)かかっていた。起動のたびに払うには重すぎる。このパスは起動後に一度だけ
    // 走るベイクなので、動的ループにして実行時にわずかに遅くなることは問題にならない
    [loop]
    for (int z = -1; z <= 1; ++z)
    {
        [loop]
        for (int y = -1; y <= 1; ++y)
        {
            [loop]
            for (int x = -1; x <= 1; ++x)
            {
                const float3 offset = float3(x, y, z);
                // 特徴点の位置はセル番号だけで決まる。ここで巻き戻すことで
                // テクスチャの端を跨いだセルが反対側の端のセルと同じ点を共有し、タイル可能になる。
                // 変数名を featurePoint にしているのは point がHLSLの予約語(ジオメトリシェーダーの
                // プリミティブ種別)のため。const float3 point と書くと構文エラーになる
                const float3 featurePoint = CloudNoiseHash33(WrapCell(baseCell + offset, cellCount));
                const float3 diff = offset + featurePoint - local;
                minDistanceSq = min(minDistanceSq, dot(diff, diff));
            }
        }
    }

    // 距離を [0,1] へ。最近傍距離は最大でも 1.0 程度(セル1個ぶん)に収まる
    return saturate(1.0f - sqrt(minDistanceSq));
}

// Worleyノイズを3オクターブ重ねたfBm。周波数と一緒に周期(セル数)も倍にしないと
// オクターブごとにタイルの継ぎ目の位置がずれ、周期性そのものが壊れる
float WorleyFbm(float3 uvw, float cellCount)
{
    float amplitude = 0.625f;
    float sum = 0.0f;
    float amplitudeSum = 0.0f;
    float cells = cellCount;
    [unroll]
    for (int octave = 0; octave < 3; ++octave)
    {
        sum += amplitude * WorleyNoise(uvw, cells);
        amplitudeSum += amplitude;
        amplitude *= 0.5f;
        cells *= 2.0f;
    }
    return sum / amplitudeSum;
}

// 周期化した3Dの値ノイズ。Perlin代わりに使う(格子点の値をsmoothstepで補間する標準的なもので、
// Sky.hlsli の CloudValueNoise を3次元へ拡張したのと同じ形)
float ValueNoise3D(float3 uvw, float cellCount)
{
    const float3 scaled = uvw * cellCount;
    const float3 cell = floor(scaled);
    const float3 f = scaled - cell;
    const float3 w = f * f * (3.0f - 2.0f * f);

    float corners[8];
    [unroll]
    for (int i = 0; i < 8; ++i)
    {
        const float3 offset = float3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        corners[i] = CloudNoiseHash13(WrapCell(cell + offset, cellCount));
    }

    const float x00 = lerp(corners[0], corners[1], w.x);
    const float x10 = lerp(corners[2], corners[3], w.x);
    const float x01 = lerp(corners[4], corners[5], w.x);
    const float x11 = lerp(corners[6], corners[7], w.x);
    return lerp(lerp(x00, x10, w.y), lerp(x01, x11, w.y), w.z);
}

// 値ノイズを4オクターブ重ねたfBm。周期の扱いはWorleyFbmと同じ理由で周波数と揃える
float ValueFbm3D(float3 uvw, float cellCount)
{
    float amplitude = 0.5f;
    float sum = 0.0f;
    float amplitudeSum = 0.0f;
    float cells = cellCount;
    [unroll]
    for (int octave = 0; octave < 4; ++octave)
    {
        sum += amplitude * ValueNoise3D(uvw, cells);
        amplitudeSum += amplitude;
        amplitude *= 0.5f;
        cells *= 2.0f;
    }
    return sum / amplitudeSum;
}

float NoiseRemap(float x, float lo, float hi)
{
    return saturate((x - lo) / max(hi - lo, 1e-5f));
}

// ============================================================================
// 形状テクスチャ(128^3)
// ============================================================================

// R = Perlin-Worley(低周波の塊)、G/B/A = 周波数を上げたWorley(縁を削るのに使う)。
// この4チャンネル構成はSchneiderらのボリュメトリック雲(Nubis)で標準的に使われるもので、
// R単体では塊が丸すぎ、Worleyを重ねることで綿状の輪郭になる
//
// 【H1cで4から8へ上げた】被覆率を上げると**遠方まで一定間隔の繰り返し**が見えていた。
// 4だと**タイル1枚に大きな特徴が4x4=16個しか無い**。Sky.hlsli側はこのテクスチャを
// ワールドの2,096mごとに繰り返して敷いていたので、空が「同じ16種類の塊の反復」になっていた。
//
// 【この値だけを上げてはいけない】上げると特徴が小さくなるので、Sky.hlsliの
// kCloudShapeRepeats を同じ比率で下げて**ワールドでの特徴の大きさ(524m)を保つ**こと。
// 8にしたときの相棒は kCloudShapeRepeats = 86 / kCloudShapeVerticalPeriod = 1984。
//
// 【上限】128^3に対して1セルあたりのテクセル数が 128/8 = 16。WorleyFbmは3オクターブで
// セル数を倍々にするので最高で 8*4 = 32セル = 4テクセル/セルになる。
// これ以上増やすとベイクの時点でエイリアスするため、16へ上げるのは避ける。
// 【この値を変えたら測り直すもの】kCloudShapeContrastLow/High と
// kCloudVolumeDensityNormalize(どちらもSky.hlsli)
static const float kShapeBaseCells = 8.0f;

RWTexture3D<float4> ShapeOut : register(u0);

[numthreads(4, 4, 4)]
void CSGenerateShape(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint width, height, depth;
    ShapeOut.GetDimensions(width, height, depth);
    if (dispatchThreadID.x >= width || dispatchThreadID.y >= height || dispatchThreadID.z >= depth)
    {
        return;
    }

    // テクセルの中心を [0,1)^3 の座標にする。中心(+0.5)にするのは、格子の境界ちょうどを
    // 評価すると隣接テクセル間で値が対称になり模様が不自然に整列するため
    const float3 uvw = (float3(dispatchThreadID) + 0.5f) / float3(width, height, depth);

    const float perlin = ValueFbm3D(uvw, kShapeBaseCells);
    const float worley = WorleyFbm(uvw, kShapeBaseCells);

    // Perlin-Worley: Perlinの値をWorleyを下限として引き伸ばす。Perlin単体の「ふわっとした
    // 濃淡」にWorleyの「粒の塊」を掛け合わせた形になり、積雲の房らしい分布になる
    const float perlinWorley = NoiseRemap(perlin, worley - 1.0f, 1.0f);

    ShapeOut[dispatchThreadID] = float4(
        perlinWorley,
        WorleyFbm(uvw, kShapeBaseCells * 2.0f),
        WorleyFbm(uvw, kShapeBaseCells * 4.0f),
        WorleyFbm(uvw, kShapeBaseCells * 8.0f));
}

// ============================================================================
// ディテールテクスチャ(32^3)
// ============================================================================

// 形状テクスチャで作った塊の縁だけを削るための高周波ノイズ。3チャンネルとも
// Worleyのfbmで、周波数だけを変えてある(aは未使用だがRGBA8で焼くため0で埋める)
static const float kDetailBaseCells = 6.0f;

RWTexture3D<float4> DetailOut : register(u0);

[numthreads(4, 4, 4)]
void CSGenerateDetail(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint width, height, depth;
    DetailOut.GetDimensions(width, height, depth);
    if (dispatchThreadID.x >= width || dispatchThreadID.y >= height || dispatchThreadID.z >= depth)
    {
        return;
    }

    const float3 uvw = (float3(dispatchThreadID) + 0.5f) / float3(width, height, depth);

    DetailOut[dispatchThreadID] = float4(
        WorleyFbm(uvw, kDetailBaseCells),
        WorleyFbm(uvw, kDetailBaseCells * 2.0f),
        WorleyFbm(uvw, kDetailBaseCells * 4.0f),
        0.0f);
}
