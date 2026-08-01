// TAA(Temporal Anti-Aliasing)パス。SSRの後、自動露出/ブルーム/トーンマップの前に置く。
//
// 【原理】1ピクセルにつき1サンプルしか撮らないと斜めのエッジは「物があるか無いか」の2値になり
// 階段状のジャギーになる。そこで毎フレーム投影行列を1ピクセル未満だけずらし(ジッター)、
// 同じ画素が毎回わずかに違う位置をサンプルするようにしたうえで、このパスが過去の結果を
// 蓄積する。静止していれば十数フレームで収束し、実質的なスーパーサンプリングになる。
//
// 【難しいのは「過去のどこを見るか」】カメラや物体が動くと、今フレームの画素(x,y)と
// 前フレームの画素(x,y)は違う場所を映している。そのまま平均すると尾を引く(ゴースト)。
// そこでG-Bufferパスが書いたモーションベクター(速度)で前フレームの位置を引き当てる。
// さらに、速度で表現しきれないもの(遮蔽の変化・影や反射の移動)に備えて、
// 引いてきた履歴が「今このあたりにあり得る色」の範囲に収まるようクリップする。
// このクリップの品質がTAAの品質をほぼ決める。
//
// 実装上の勘所と、それぞれの理由は各処理のコメントに書いてある。

#include "Samplers.hlsli"

// このパスはb0(FrameConstants)を使わず、必要な行列も含めて専用のb1へ全部詰めている。
// FrameConstantsは末尾追加を重ねて700バイトを超えており、途中のフィールドを飛ばせない
// cbufferの規約上、ここで使いたい末尾の2つを読むためだけに全フィールドを宣言する羽目になるため
cbuffer TAAConstants : register(b1)
{
    // 今フレームのジッター済み逆ビュー射影行列(空の速度を補うのに使う)
    float4x4 InvViewProj;
    // 前フレームのジッター済みビュー射影行列
    float4x4 PrevViewProj;
    // ジッター量(UV単位)。xy=今フレーム、zw=前フレーム
    float4 JitterUv;
    // xy=レンダー解像度、zw=その逆数(1テクセルぶんのUV)
    float4 ScreenParams;
    // x=今フレームの色を混ぜる割合、y=シャープネス、
    // z=履歴が使えるか(0=使えない)、w=プリ露出の変化を打ち消す倍率
    float4 Params0;
};

Texture2D CurrentColor : register(t0);    // TAA前のHDR(SSR有効時はSSR適用後)
Texture2D HistoryColor : register(t1);    // 前フレームのTAA結果
Texture2D VelocityTexture : register(t2); // G-Bufferパスが書いたモーションベクター(UV単位)
Texture2D DepthTexture : register(t3);    // G-Buffer深度(Reverse-Z: 近=1.0、遠=0.0)

struct PSInput
{
    float4 Position : SV_POSITION;
    float2 UV : TEXCOORD0;
};

PSInput VSMain(uint vertexID : SV_VertexID)
{
    PSInput output;
    output.UV = float2((vertexID << 1) & 2, vertexID & 2);
    output.Position = float4(output.UV.x * 2.0f - 1.0f, 1.0f - output.UV.y * 2.0f, 0.0f, 1.0f);
    return output;
}

// 近傍クリップはRGBのまま行うと色相ごと動いてしまうため、輝度(Y)と色差(Co/Cg)へ分けて行う。
// 輝度と色を別軸として扱えるので、AABBが「明るさの範囲」と「色味の範囲」を素直に表せる
float3 RgbToYCoCg(float3 rgb)
{
    return float3(
        0.25f * rgb.r + 0.5f * rgb.g + 0.25f * rgb.b,
        0.5f * rgb.r - 0.5f * rgb.b,
        -0.25f * rgb.r + 0.5f * rgb.g - 0.25f * rgb.b);
}

float3 YCoCgToRgb(float3 ycocg)
{
    float y = ycocg.x;
    float co = ycocg.y;
    float cg = ycocg.z;
    return float3(y + co - cg, y + cg, y - co - cg);
}

float Luminance(float3 rgb)
{
    return dot(rgb, float3(0.2126f, 0.7152f, 0.0722f));
}

// 履歴の色をAABBの内側へ引き戻す。単純にclampで各軸を独立に切ると、
// 「AABBの角」へ寄った不自然な色になって色ずれが目立つ。中心から履歴へ向かう線分と
// AABBの交点まで縮める(=色の方向は保ったまま長さだけ詰める)ことでこれを避ける
float3 ClipToAABB(float3 history, float3 boxMin, float3 boxMax)
{
    float3 center = 0.5f * (boxMax + boxMin);
    float3 extent = 0.5f * (boxMax - boxMin) + 1e-5f;
    float3 offset = history - center;
    // 各軸で「何倍まで縮めればAABBに入るか」を求め、最も厳しい軸に合わせる
    float3 unit = abs(offset / extent);
    float maxUnit = max(unit.x, max(unit.y, unit.z));
    return (maxUnit > 1.0f) ? (center + offset / maxUnit) : history;
}

// 履歴のサンプリング。バイリニアで引くと、毎フレーム「補間した結果をまた補間する」ことになり
// ボケが際限なく累積する。Catmull-Romは負のローブを持つ補間カーネルで、この累積を打ち消して
// 輪郭を保てる。本来16タップ要るところを、双一次補間のハードウェアを使って5タップへ削る
// 定番の最適化(Filmic SMAA/UE等と同じ形)
float3 SampleHistoryCatmullRom(float2 uv)
{
    float2 texelSize = ScreenParams.zw;
    float2 samplePos = uv * ScreenParams.xy;
    float2 texPos1 = floor(samplePos - 0.5f) + 0.5f;
    float2 f = samplePos - texPos1;

    // Catmull-Rom(B=0, C=0.5)の重み
    float2 w0 = f * (-0.5f + f * (1.0f - 0.5f * f));
    float2 w1 = 1.0f + f * f * (-2.5f + 1.5f * f);
    float2 w2 = f * (0.5f + f * (2.0f - 1.5f * f));
    float2 w3 = f * f * (-0.5f + 0.5f * f);

    // w1とw2を1タップのバイリニアへまとめる
    float2 w12 = w1 + w2;
    float2 offset12 = w2 / max(w12, 1e-5f);

    float2 texPos0 = (texPos1 - 1.0f) * texelSize;
    float2 texPos3 = (texPos1 + 2.0f) * texelSize;
    float2 texPos12 = (texPos1 + offset12) * texelSize;

    float3 result = 0.0f;
    result += HistoryColor.SampleLevel(ColorSampler, float2(texPos12.x, texPos0.y), 0).rgb * w12.x * w0.y;
    result += HistoryColor.SampleLevel(ColorSampler, float2(texPos0.x, texPos12.y), 0).rgb * w0.x * w12.y;
    result += HistoryColor.SampleLevel(ColorSampler, float2(texPos12.x, texPos12.y), 0).rgb * w12.x * w12.y;
    result += HistoryColor.SampleLevel(ColorSampler, float2(texPos3.x, texPos12.y), 0).rgb * w3.x * w12.y;
    result += HistoryColor.SampleLevel(ColorSampler, float2(texPos12.x, texPos3.y), 0).rgb * w12.x * w3.y;

    // 負のローブがあるため、HDRの明暗差が大きい場所では結果が負に振れることがある。
    // 負の色が履歴へ入ると次フレーム以降も残り続けるのでここで潰す
    return max(result, 0.0f);
}

// 空(ジオメトリが無い画素)の速度。空はG-Bufferに描かれない(ライティングパスがキューブマップから
// 作る)ため速度バッファは0のままだが、カメラを回せば空も動く。0のままにすると空だけ収束せず、
// 地平線を境に画質が変わってしまうので、深度の遠平面上の点を前フレームへ再投影して補う。
//
// Camera::GetProjectionMatrixは有限の遠平面を持つ(a = n/(n-f) ≠ 0)ので、深度0の点は
// 「カメラからFarZだけ離れた点」として復元できる。無限遠のreverse-Zへ変更するとw=0で
// ゼロ除算になるため、その場合はここを方向ベクトルの再投影へ書き換えること
float2 ComputeSkyVelocity(float2 uv)
{
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 worldPos = mul(float4(ndc, 0.0f, 1.0f), InvViewProj);
    float4 prevClip = mul(float4(worldPos.xyz / worldPos.w, 1.0f), PrevViewProj);
    float2 prevUv = (prevClip.xy / prevClip.w) * float2(0.5f, -0.5f) + 0.5f;

    // ジオメトリ側(GBuffer.hlsl)とまったく同じ規約で、両フレームのジッターを取り除く。
    // ここを省くと空だけ履歴を引く位置が毎フレーム揺れて収束しない
    return (uv - JitterUv.xy) - (prevUv - JitterUv.zw);
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float2 texelSize = ScreenParams.zw;
    int3 pixelCoord = int3(input.Position.xy, 0);
    float3 current = CurrentColor.Load(pixelCoord).rgb;

    // --- 3x3近傍の統計を取る(色のAABBと、シャープネス用の低域) ---
    // 同じループで速度のディレートも行う。ディレートとは「近傍で最も手前にある画素の速度を使う」
    // ことで、物体のシルエットの縁で背景の速度を拾ってゴーストになるのを抑える。
    // Reverse-Zなので「最も手前」は深度値が最大の画素であることに注意(minにすると
    // 最も遠い=多くの場合は空の速度を拾ってしまい、かえって悪化する)
    float3 moment1 = 0.0f;
    float3 moment2 = 0.0f;
    float3 neighborMin = 1e30f;
    float3 neighborMax = -1e30f;
    float3 lowPass = 0.0f;
    float closestDepth = -1.0f;
    int2 closestOffset = int2(0, 0);

    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            int3 tapCoord = pixelCoord + int3(x, y, 0);
            float3 tap = RgbToYCoCg(CurrentColor.Load(tapCoord).rgb);
            moment1 += tap;
            moment2 += tap * tap;
            neighborMin = min(neighborMin, tap);
            neighborMax = max(neighborMax, tap);
            lowPass += CurrentColor.Load(tapCoord).rgb;

            float tapDepth = DepthTexture.Load(tapCoord).r;
            if (tapDepth > closestDepth)
            {
                closestDepth = tapDepth;
                closestOffset = int2(x, y);
            }
        }
    }
    lowPass *= (1.0f / 9.0f);

    // --- 速度を決める ---
    float2 velocity;
    if (closestDepth <= 0.0f)
    {
        // 近傍9画素すべてにジオメトリが無い=空。深度から再投影して補う
        velocity = ComputeSkyVelocity(input.UV);
    }
    else
    {
        velocity = VelocityTexture.Load(pixelCoord + int3(closestOffset, 0)).rg;
    }

    float2 historyUv = input.UV - velocity;

    // --- 履歴が使えないケースは、履歴を「サンプルすらせず」今フレームの色を返す ---
    // ブレンド率を0にするだけでは不十分。作りたての履歴バッファ(fp16)の中身は未定義で
    // NaNのことがあり、lerp(NaN, x, 1.0)もNaNのまま伝播する。一度でも履歴へ入ると
    // その画素は以後ずっとNaNのまま固着するため、読むこと自体を避ける必要がある
    bool historyOutside = any(historyUv < 0.0f) || any(historyUv > 1.0f);
    if (Params0.z < 0.5f || historyOutside)
    {
        return float4(max(current, 0.0f), 1.0f);
    }

    // --- 履歴を引く ---
    // プリ露出(m_EffectiveExposureEV100)は時間順応で毎フレーム変わるので、
    // 履歴は「前フレームの露出で焼かれた明るさ」のままになっている。倍率を掛けて今の露出へ揃えないと、
    // 露出が動いている間ずっと古い明るさを引きずって明るさの尾を引く
    float3 history = SampleHistoryCatmullRom(historyUv) * Params0.w;
    // fp16の履歴では稀にInf/NaNが紛れ込む。混入を1フレームで断ち切る保険
    if (!all(isfinite(history)))
    {
        history = current;
    }

    // --- 近傍クリップ ---
    // 近傍の平均±標準偏差からAABBを作る(分散クリッピング)。min/maxをそのまま使うより
    // 外れ値に強く、ちらつきとゴーストのバランスが取りやすい。
    // 最後に実際のmin/maxとの積集合を取り、AABBが近傍の実在範囲を超えないようにする
    const float kVarianceGamma = 1.25f;
    float3 mean = moment1 * (1.0f / 9.0f);
    float3 variance = max(moment2 * (1.0f / 9.0f) - mean * mean, 0.0f);
    float3 sigma = sqrt(variance) * kVarianceGamma;
    float3 boxMin = max(mean - sigma, neighborMin);
    float3 boxMax = min(mean + sigma, neighborMax);

    float3 historyYCoCg = ClipToAABB(RgbToYCoCg(history), boxMin, boxMax);
    history = max(YCoCgToRgb(historyYCoCg), 0.0f);

    // --- シャープネス ---
    // 蓄積とCatmull-Rom補間で失われる高域を戻す。必ず「今フレームの入力」側へ掛けること。
    // 出力はそのまま次フレームの履歴になるので、ブレンド後の結果へ掛けるとシャープが
    // 毎フレーム再適用されて累積し、輪郭が発振してリンギングになる
    float3 sharpened = current + (current - lowPass) * Params0.y;
    sharpened = max(sharpened, 0.0f);

    // --- ブレンド ---
    // 単純な重み付き平均だと、1画素だけ極端に明るい点(ファイアフライ)が平均を支配して
    // 尾を引く。輝度が高いサンプルほど重みを下げることで、HDRのまま安定して平均できる
    float weightCurrent = Params0.x / (1.0f + Luminance(sharpened));
    float weightHistory = (1.0f - Params0.x) / (1.0f + Luminance(history));
    float3 result = (sharpened * weightCurrent + history * weightHistory) / max(weightCurrent + weightHistory, 1e-5f);

    return float4(max(result, 0.0f), 1.0f);
}
