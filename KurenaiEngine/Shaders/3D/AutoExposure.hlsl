// 自動露出(eye adaptation)。SceneColor(HDR)の輝度ヒストグラムをGPU上で作り、
// 低/高パーセンタイルを除外した加重平均から目標EV100を求め、時間方向に指数的に追従させる。
// 結果は1x1相当のテクスチャへ書き、Tonemapパスがそれを読んで露出倍率を掛ける。
//
// === プリ露出方式について ===
// このエンジンはEV100露出をCPU側でライト強度へ事前乗算している(ComputeSunLighting /
// MakeGPULight)。その値(m_SceneExposureEV100)を「プリ露出」と呼ぶ。
// HDRバッファには常にプリ露出済みの値が流れるため、
//   絶対輝度[cd/m^2] = バッファの値 / preExposure
// で元の測光量へ戻せる。自動露出はこの絶対輝度に対して働くので、プリ露出をどう設定しても
// 測定結果は変わらない(プリ露出はバッファの数値レンジをR16Fの範囲に収めるための係数でしかない)。
//
// この方式にしたのは、自動露出の結果を再びライト強度へ戻すとフィードバックループになり、
// かつCPUへのリードバック(GPU→CPUの同期待ち)が必要になるため。プリ露出を固定値に保ち、
// 露出の適用をTonemapパスの一箇所に閉じ込めることで、リードバックなしで成立させている
#include "Samplers.hlsli"

// ヒストグラムのビン数。CSClearHistogram/CSHistogramのスレッド数と一致させること
#define HISTOGRAM_BINS 256

cbuffer AutoExposureConstants : register(b1)
{
    uint2 InputSize;
    // 露出のクランプ範囲(EV100)。ヒストグラムのビン割りもこの範囲で行う
    float MinEV100;
    float MaxEV100;

    // CPU側でライト強度へ事前乗算済みのEV100(m_SceneExposureEV100)
    float PreExposureEV100;
    // 前フレームからの経過時間[秒]
    float DeltaTime;
    // 明順応(暗い→明るい)と暗順応(明るい→暗い)の速度。人間の目も両者で速度が違うため分けている
    float AdaptationSpeedUp;
    float AdaptationSpeedDown;

    // 加重平均から除外する下側/上側の累積割合(0〜1)。暗すぎる画素と明るすぎる画素に
    // 露出が引きずられるのを防ぐ
    float LowPercentile;
    float HighPercentile;
    // 露出補正(EV)。測定結果に対してユーザーが意図的に足すオフセット
    float ExposureCompensation;

    // --- 暗いシーンをわざと暗いまま写すための補正カーブ ---
    // 自動露出は「測ったものを中庸なグレーへ持ち上げる」ので、そのままだと夜が昼と同じ
    // 明るさで出てしまう(実測: 補正を入れる前は22時と12時の空の明度がほぼ一致していた)。
    // 実写でも夜景はわざと露出を切り詰めて撮るので、測定EV100が低いほど正のオフセットを
    // 足して(=表示を暗くして)「暗いシーンは暗いまま」にする。
    // Unreal Engine の Exposure Compensation Curve と同じ考え方の、2点線形版
    float NightRolloffEV;
    float NightRolloffDarkEV100;
    float NightRolloffBrightEV100;

    // --- 測光値の上側クランプ(構図依存を抑える) ---
    // 自動露出は画面に写っているものを測るので、空が画面に占める割合で結果が変わる。
    // 実測では同じ時刻・同じ壁面でも、空が画面の約40%を占める構図と空が入らない構図とで
    // 露出が2〜3.5段ずれた(夜だけでなく昼も同様)。測光範囲が50〜95パーセンタイル、つまり
    // ヒストグラムの明るい側の半分だけを平均しているため、空が測光を支配してしまう。
    //
    // そこでCPU側が持つキー照度(太陽・月・空の照度[lx]。画面に何が写っていようと変わらない)
    // から求めた基準EVを上限の足がかりにし、測光値がそこから KeyCeilingEV 段より上へ
    // 行かないようにする。上側だけを止めるのは、屋内のように実際の輝度が屋外のキー照度より
    // ずっと低いシーンでは測光値が下へ振れるのが正しいから(両側を締めると屋内が真っ暗になる)。
    // 下側は MinEV100 が絶対的な下限として効く
    float KeyReferenceEV100;
    float KeyCeilingEV;

    // --- シーン切り替え時の順応リセット ---
    // 順応の状態(前フレームのEVと初期化済みフラグ)はExposureOutputに入っているため、
    // CPU側からは「初回に戻す」手段が無い。シーンを切り替えても前のシーンの露出から
    // 順応が続いてしまうので、その1フレームだけこのフラグで順応を飛ばす。
    // 場面の切り替わりであって視点の移動ではないため、目の順応を模す理由が無い
    float ResetAdaptation;
    // 詰め物はfloat3で置くこと。HLSLのcbuffer内の配列は要素ごとに16バイト境界へ揃えられるため、
    // float ResetPadding[3] と書くと12バイトではなく48バイトを占め、C++側と大きさが食い違う
    float3 ResetPadding;
};

// 256ビンの輝度ヒストグラム
RWStructuredBuffer<uint> Histogram : register(u0);
// 露出の保存先。2x1のR32_Floatで、texel(0,0)=平滑化後のEV100、texel(1,0)=初期化済みフラグ。
// RWTexture2Dからの型付き読み出しはR32系しか保証されていない(それ以外は
// TypedUAVLoadAdditionalFormatsが必要)ため、float4を1テクセルに詰めるのではなく
// R32_Floatを2テクセル並べる構成にしている
RWTexture2D<float> ExposureOutput : register(u1);

Texture2D SceneColorTexture : register(t0);
// G-Bufferの深度。空(=何も描かれなかった背景)を測光から外すために読む。
// Reverse-Zのため遠平面(=背景)はNDC z=0.0付近になる
Texture2D DepthTexture : register(t1);

groupshared uint gHistogram[HISTOGRAM_BINS];

float Luminance(float3 color)
{
    return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

// プリ露出の係数。ComputeExposure(ev) = 1/(1.2 * 2^ev) のHLSL版
float PreExposureScale()
{
    return 1.0f / (1.2f * exp2(PreExposureEV100));
}

// 絶対輝度L[cd/m^2] から EV100 へ。反射光式露出計の定数K=12.5を使う標準式
//   EV100 = log2(L * 100 / K) = log2(L * 8)
float LuminanceToEV100(float luminance)
{
    return log2(luminance * 8.0f);
}

uint EV100ToBin(float ev)
{
    const float t = saturate((ev - MinEV100) / max(MaxEV100 - MinEV100, 1e-6f));
    return (uint)(t * (float)(HISTOGRAM_BINS - 1) + 0.5f);
}

float BinToEV100(uint bin)
{
    const float t = (float)bin / (float)(HISTOGRAM_BINS - 1);
    return MinEV100 + t * (MaxEV100 - MinEV100);
}

// --- ヒストグラムのクリア(1グループ・256スレッド) ---
[numthreads(HISTOGRAM_BINS, 1, 1)]
void CSClearHistogram(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    Histogram[dispatchThreadID.x] = 0;
}

// --- ヒストグラムの構築 ---
// 16x16=256スレッド/グループ。SV_GroupIndexが0〜255になるのでビン番号と1対1に対応させ、
// グループ共有メモリで集計してからグローバルへ1回だけ加算する
// (全画素が直接グローバルへInterlockedAddすると原子操作が集中して遅くなるため)
[numthreads(16, 16, 1)]
void CSHistogram(uint3 dispatchThreadID : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex)
{
    gHistogram[groupIndex] = 0;
    GroupMemoryBarrierWithGroupSync();

    if (dispatchThreadID.x < InputSize.x && dispatchThreadID.y < InputSize.y)
    {
        // === 空は測光から外す ===
        // 空は「被写体」ではなく「光源」である。写真でも露出は被写体に合わせるもので、
        // 空にカメラを向けたぶんだけ地上が暗く写るのは逆光の露出失敗にあたる。
        //
        // これを数えていたために、同じ時刻・同じ壁面でも空が画面に占める割合で
        // 露出が2〜3.5段ずれていた(21.9.6節)。空を外せば測光値は画面に写る
        // **地物だけ**で決まるので、構図依存はその場で消える。
        //
        // Reverse-Zなので背景(何も描かれなかった画素)は深度0付近になる
        const float depth = DepthTexture[dispatchThreadID.xy].r;
        if (depth > 0.0f)
        {
            const float3 preExposed = max(SceneColorTexture[dispatchThreadID.xy].rgb, 0.0f);
            // プリ露出を外して絶対輝度へ戻す(冒頭のコメント参照)
            const float luminance = Luminance(preExposed) / PreExposureScale();
            // 完全な黒はlog2が-infになるうえ露出を不当に下へ引くので数えない
            if (luminance > 1e-6f)
            {
                InterlockedAdd(gHistogram[EV100ToBin(LuminanceToEV100(luminance))], 1);
            }
        }
    }

    GroupMemoryBarrierWithGroupSync();
    InterlockedAdd(Histogram[groupIndex], gHistogram[groupIndex]);
}

// --- ヒストグラムの縮約と時間方向の順応(1スレッド) ---
// 256回のループを1スレッドで回すだけなので、並列縮約を書くより単純で、
// 1フレームに1回しか走らないためコストも問題にならない
[numthreads(1, 1, 1)]
void CSResolve(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint total = 0;
    for (uint i = 0; i < HISTOGRAM_BINS; ++i)
    {
        total += Histogram[i];
    }

    // 空以外が1画素も写っていない(カメラが空だけを向いている)ときの退避値。
    // レンジの中点では昼夜を問わず的外れな露出になるので、シーンの基準EVを使う
    const float fallbackEV = KeyReferenceEV100;
    float targetEV = fallbackEV;

    if (total > 0)
    {
        // 累積個数ベースでパーセンタイル範囲[low, high)に入る画素だけを加重平均する
        const uint lowCount = (uint)((float)total * saturate(LowPercentile));
        const uint highCount = (uint)((float)total * saturate(HighPercentile));

        uint seen = 0;
        float weightedSum = 0.0f;
        float weightTotal = 0.0f;
        for (uint bin = 0; bin < HISTOGRAM_BINS; ++bin)
        {
            const uint count = Histogram[bin];
            if (count == 0)
            {
                continue;
            }

            const uint binStart = seen;
            const uint binEnd = seen + count;
            seen = binEnd;

            const uint rangeStart = max(binStart, lowCount);
            const uint rangeEnd = min(binEnd, highCount);
            if (rangeEnd > rangeStart)
            {
                const float n = (float)(rangeEnd - rangeStart);
                weightedSum += BinToEV100(bin) * n;
                weightTotal += n;
            }
        }

        targetEV = (weightTotal > 0.0f) ? (weightedSum / weightTotal) : fallbackEV;
    }

    // 測光値の上側だけをキー照度の基準EVでクランプする(構図依存を抑える。宣言部のコメント参照)。
    // **夜の露出切り詰めより前に行うこと**。切り詰めは意図した演出のオフセットなので、
    // クランプの対象は「測った値」だけにする
    targetEV = min(targetEV, KeyReferenceEV100 + KeyCeilingEV);

    // 測定値が暗いほど正のオフセットを足す(EV100が大きい=表示が暗い)。
    // 補正はクランプ後の測定値から決めること。切り詰め後の値から決めると自己参照になって
    // 明るさが振動する
    const float rolloffT = saturate(
        (targetEV - NightRolloffDarkEV100) / max(NightRolloffBrightEV100 - NightRolloffDarkEV100, 1e-3f));
    const float nightRolloff = lerp(NightRolloffEV, 0.0f, rolloffT);

    targetEV = clamp(targetEV + ExposureCompensation + nightRolloff, MinEV100, MaxEV100);

    const float previousEV = ExposureOutput[uint2(0, 0)];
    const float initialized = ExposureOutput[uint2(1, 0)];

    float ev;
    if (initialized < 0.5f || ResetAdaptation > 0.5f)
    {
        // 起動直後(UAVテクスチャはゼロ初期化されるので、initializedで初回を判定できる)と
        // シーン切り替え直後(CPU側がResetAdaptationを立てる)は順応させずいきなり合わせる
        ev = targetEV;
    }
    else
    {
        const float speed = (targetEV > previousEV) ? AdaptationSpeedUp : AdaptationSpeedDown;
        ev = lerp(previousEV, targetEV, saturate(1.0f - exp(-DeltaTime * speed)));
    }

    ExposureOutput[uint2(0, 0)] = ev;
    ExposureOutput[uint2(1, 0)] = 1.0f;
}
