// スクリーンスペースシャドウ(SSS / 接触影)の共通処理。
// ポイント/スポットライトの影を「シャドウマップを1枚も増やさずに」出すためのもので、
// G-Bufferの深度バッファを受光点からライトへ向かってレイマーチし、途中に手前のサーフェスが
// あればそのライトを遮る。NormalEncoding.hlsli・SpecularEnergy.hlsli・Samplers.hlsli・
// ShadowSampling.hlsliに次ぐ5つ目の共有ヘッダー。
//
// 【インクルードの前提】以下が宣言済みであることを前提にしているため、それらの後ろで
// #includeすること(既存の共有ヘッダーと同様、インクルードガードは持たない):
//   - FrameConstants の ViewProj
//   - LightingConstants(b1)の SSSParams0 / SSSParams1
//   - Samplers.hlsli の DataSampler(s2、Point+Clamp)
//   - Texture2D DepthTexture(G-Bufferの深度。DirectLighting.hlslではt3)
//
// 【この手法の限界】深度バッファに写っている表面しか遮蔽物として扱えない。したがって
//   - 画面外にある遮蔽物は影を落とさない
//   - 手前の面に隠れて深度バッファに残っていない遮蔽物は影を落とさない
//   - 深度バッファには厚みの情報が無いため、遮蔽と判定する深度差に上限(thickness)が要る
// 得られるのは「壁でランプが完全に遮られる」ような影ではなく、接触影・中距離の遮蔽である。
// これは手法の性質であってバグではない(docs/Architecture.html 18章)。
//
// 【なぜHi-Zを使わないか】このエンジンのHi-Zチェーン(HiZ.hlsl CSDownsample)はmin縮約であり、
// Reverse-Zではmin=「ブロック内で最も遠い可視サーフェス」を意味する。これはオクルージョン
// カリング(「候補は確実に遮蔽されている」の判定)向きの向きであって、レイマーチの空き空間
// スキップが必要とする「ブロック内に遮蔽物は確実に無い」の判定にはmax=最近が要る。
// 向きが逆なので流用できず、全解像度の深度を線形マーチしている(18章)。

// レイマーチのステップ数の上限。SSSParams0.xで実際のステップ数を指定するが、
// 定数上限を置くことでコンパイラがループを有界と扱えるようにする
static const uint kSSSMaxStepCount = 64u;

// 遮蔽と判定するための最小深度差。受光点自身や、その連続面をレイが「遮蔽物」と誤検出する
// (シャドウアクネ)のを防ぐ。View空間深度への相対値にしているのは、深度バッファの精度も
// 1ピクセルが占める世界距離も距離に比例して粗くなるため、絶対値のバイアスでは
// 近距離で過剰・遠距離で不足になるから
static const float kSSSRelativeDepthBias = 0.002f;

// 最大レイ長で打ち切られた場合に、レイ終端付近のヒットをフェードさせ始める位置(レイ長に対する比)。
// 打ち切りの境界で影が唐突に消えて空間に不自然な縁ができるのを緩和する
static const float kSSSDistanceFadeStart = 0.75f;

// Reverse-Zの深度値からView空間Z(カメラからの距離。値が大きいほど遠い)を復元する。
// Camera::GetProjectionMatrixの射影行列(行ベクトル規約)は clip.z = viewZ * a + b、clip.w = viewZ
// なので depth = clip.z / clip.w = a + b / viewZ、逆に解いて viewZ = b / (depth - a) になる。
// a / b はCPU側(KurenaiEngine3D::Render)が射影行列の要素から求めてSSSParams1.x / .yへ渡す。
// SSR.hlslのSampleSceneViewZはReconstructWorldPos + mul(View)で行列積2回だが、
// SSSはライト数×ステップ数だけこれを回すため、除算1回のこの形にしている
float SSSViewZFromDepth(float depth)
{
    return SSSParams1.y / (depth - SSSParams1.x);
}

// インターリーブド・グラディエント・ノイズ(Jimenez, "Next Generation Post Processing in
// Call of Duty: Advanced Warfare", 2014)。ステップ位置をピクセルごとにずらし、等間隔サンプリングで
// 出る縞状のバンディングを高周波のディザへ変換する。
//
// 【フレーム番号を混ぜてはいけない】このエンジンにはTAAが無く、時間的な平均化を行う後段が存在しない。
// ピクセル座標だけの関数にしておけばカメラが止まっている限りノイズも完全に静止し、
// 「わずかにディザが乗った静止画」で済む。フレームごとに変えると平均化されないまま
// 画面全体がちらつき続けることになる(SSIL_VisibilityBitmask.hlslのstepJitterも同じ理由で
// ピクセル座標のみの関数にしている)
float SSSJitter(float2 pixelCoord)
{
    return frac(52.9829189f * frac(dot(pixelCoord, float2(0.06711056f, 0.00583715f))));
}

// 受光点からライトへ向かってレイマーチし、遮蔽率を返す(1=完全に光が当たる、0=完全に影)。
//   worldPos          : 受光点のワールド座標
//   N                 : 受光点の法線(正規化済み)
//   L                 : 受光点からライトへ向かう単位ベクトル
//   distanceToLight   : 受光点からライトまでの距離。平行光の場合は非常に大きい値を渡す
//   pixelCoord        : SV_POSITION.xy(ジッタ用)
float ComputeScreenSpaceShadow(
    float3 worldPos, float3 N, float3 L, float distanceToLight, float2 pixelCoord)
{
    // SSSParams0: x=ステップ数, y=最大レイ長(ワールド単位), z=thickness, w=有効フラグ
    // SSSParams1: x=深度リニアライズ定数a, y=同b, z=法線方向の押し出し量, w=画面端フェード幅(UV)
    if (SSSParams0.w <= 0.0f)
    {
        return 1.0f;
    }

    const uint stepCount = min((uint)SSSParams0.x, kSSSMaxStepCount);
    if (stepCount == 0u)
    {
        return 1.0f;
    }

    const float maxRayLength = SSSParams0.y;
    const float thickness = SSSParams0.z;

    // ライトより先へは進まない。レイ長が最大レイ長で打ち切られたかどうかは、
    // 終端付近のヒットをフェードさせるかの判断に使う
    const float rayLength = min(distanceToLight, maxRayLength);
    if (rayLength <= 0.0f)
    {
        return 1.0f;
    }
    const bool truncatedByMaxLength = (maxRayLength < distanceToLight);

    // 始点を法線方向へ押し出して、受光点自身とその連続面による自己遮蔽を避ける。
    // clipStart.wがそのままView空間Zになるので、深度比例のスケールは下で掛ける
    const float4 clipUnbiased = mul(float4(worldPos, 1.0f), ViewProj);
    const float startViewZ = max(clipUnbiased.w, 1.0f);
    const float3 rayStart = worldPos + N * (SSSParams1.z * startViewZ);

    const float4 clipStart = mul(float4(rayStart, 1.0f), ViewProj);
    float4 clipEnd = mul(float4(rayStart + L * rayLength, 1.0f), ViewProj);

    // ライトがカメラの手前(または背後)にあるとレイの終端がView空間Z<=0へ回り込み、
    // clip.wでの除算が破綻して画面上を逆走する。そうなる前にレイを切り詰める。
    // clipStart.wは可視ピクセルなので必ず正であり、以下のtClipは(0,1)に収まる
    const float kMinViewZ = 1e-3f;
    if (clipEnd.w < kMinViewZ)
    {
        const float tClip = saturate((kMinViewZ - clipStart.w) / (clipEnd.w - clipStart.w));
        clipEnd = lerp(clipStart, clipEnd, tClip);
    }

    const float jitter = SSSJitter(pixelCoord);
    const float invStepCount = 1.0f / float(stepCount);

    float occlusion = 0.0f;
    float hitT = 0.0f;
    float2 hitUV = float2(0.0f, 0.0f);

    [loop]
    for (uint step = 0u; step < stepCount; ++step)
    {
        // t は(0, 1]。t=0(受光点そのもの)を踏まないよう1始まりにしている
        const float t = (float(step + 1u) - jitter) * invStepCount;

        // 射影は同次座標に対して線形なので、クリップ空間での線形補間は
        // ワールド空間で等間隔に進むことと厳密に等価。SSR.hlslのProjectToScreenのように
        // ステップごとにmulを2回行う必要が無く、ライト数ぶん回るSSSではこの差が効く
        const float4 clip = lerp(clipStart, clipEnd, t);
        const float rayViewZ = clip.w;
        if (rayViewZ < kMinViewZ)
        {
            break;
        }

        const float2 ndc = clip.xy / rayViewZ;
        if (abs(ndc.x) > 1.0f || abs(ndc.y) > 1.0f)
        {
            // 画面外へ外れた: この先に遮蔽物があるかどうか深度バッファからは分からない。
            // SSR.hlslが「その先に何があるか分からないので反射を足さない」のと同じ理由で、
            // 分からないものを影にはしない
            break;
        }

        const float2 uv = float2(ndc.x * 0.5f + 0.5f, 1.0f - (ndc.y * 0.5f + 0.5f));
        const float sceneDepth = DepthTexture.SampleLevel(DataSampler, uv, 0).r;
        if (sceneDepth <= 0.0f)
        {
            // 背景(スカイ)。Reverse-Zのため遠平面はNDC z=0.0付近になる。遮蔽物は無い
            continue;
        }

        const float sceneViewZ = SSSViewZFromDepth(sceneDepth);
        const float delta = rayViewZ - sceneViewZ;

        // deltaが正 = レイがそのピクセルのサーフェスより奥に潜った(=遮蔽された)。
        // 上限(thickness)が必要なのは、深度バッファがサーフェスの厚みを持たないため。
        // 上限を外すと、レイの後ろにある遠景すべてが「無限に厚い遮蔽物」として影を作ってしまう
        if (delta > sceneViewZ * kSSSRelativeDepthBias && delta < thickness)
        {
            occlusion = 1.0f;
            hitT = t;
            hitUV = uv;
            break;
        }
    }

    if (occlusion <= 0.0f)
    {
        return 1.0f;
    }

    // 画面端に近いヒットほど影を弱める。カメラを振って遮蔽物が画面外へ出た瞬間に
    // 影が消えるのを、境界での急変から緩やかな消え方に変える(SSR.hlslのedgeFadeと同じ考え方)
    const float2 edgeDistance = min(hitUV, float2(1.0f, 1.0f) - hitUV);
    occlusion *= saturate(min(edgeDistance.x, edgeDistance.y) / max(SSSParams1.w, 1e-4f));

    // 最大レイ長で打ち切られている場合のみ、終端付近のヒットをフェードさせる。
    // 打ち切られていない(ライトまで到達している)場合は終端も正当な遮蔽なのでフェードしない
    if (truncatedByMaxLength)
    {
        occlusion *= 1.0f - smoothstep(kSSSDistanceFadeStart, 1.0f, hitT);
    }

    return saturate(1.0f - occlusion);
}
