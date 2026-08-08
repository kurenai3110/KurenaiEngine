// 空の天頂輝度スケールを、上半球の余弦重み積分が目標照度に一致するよう正規化して求める
// コンピュートシェーダー。θ64分割×φ256分割=16,384サンプルの積分を行い、その結果
// (天頂輝度・ティント4本)をGPUSkyParametersとして構造化バッファへ書く。
// **この式のCPUミラーを置いてはいけない**(二重実装にすると「片方を直したら必ずもう片方も
// 直す」という規約でしか整合が保てない)。
//
// 【なぜ正規化が必要か】zenith_luminance = 空光の照度[lx] をそのまま天頂輝度として
// 使ってはいけない。照度E[lx]と輝度L[cd/m^2]は E = ∫L・cosθ dω の関係にあるので、
// 実際に届く照度は「積分値の分だけ」ずれる。しかもPerez分布の形は太陽高度で変わるため、
// そのずれ自体が時刻とともに動く(空光の照度が1.8倍も勝手に変動する)。
// 正規化すると常に目標値ちょうどになり、時刻による空の明るさは薄明係数のように
// 意図した係数だけで制御できる。
//
// 補足: 「一様な空なら L = E/π なのでπ倍明るい」という説明は誤り。
// 積分にはティントの輝度成分(Rec.709)も入るため、単位球の積分はπ(3.14)には遠く
// 及ばない。正午での補正は数%〜十数%の範囲にとどまる。
//
// 【GPU実装の注意】HLSLのInterlockedAddはSM6.6未満では整数専用でfloatを加算できないため、
// groupshared配列へ各スレッドの部分和を書き、GroupMemoryBarrierWithGroupSync()を挟んだ
// ツリー加算(ペアワイズ和、256→128→…→1)で集約する。HLSLにdoubleの実用的な手段はないため
// floatのツリー加算で行う(16,384個の正の項に対する相対誤差はオーダーとしては小さい見込みだが、
// これは見込みであって測定値ではない)。
//
// スレッド構成は1グループ×256スレッド、Dispatch(1,1,1)固定。スレッドi(i=0..255)がφ
// インデックスiを担当し、θを64ステップぶん積む(1スレッドあたり64サンプル、合計16,384)
//
// 【積分の重みはSkyColorUpperUnitのRec.709輝度】「Perez相対輝度 × ティントのRec.709輝度成分」
// ではない。日中の色はPreetham xyYモデルで輝度(Y)と色度(x,y)が分離するため、
// 「ティントの輝度成分」という重みは意味を持たない。「実際に画面へ出る色
// (SkyColorUpperUnitの結果)のRec.709輝度」を積分すれば、昼(Preetham)・夜(ティント)・
// その間のクロスフェードのどの領域でも定義が同じになり、場合分けが要らない
// SkyView LUT。日中の空はこのLUTを引く。**定義しないと日中の空が黒くなる**ので、
// SkyColorUpperUnitを呼ぶシェーダーは全員定義すること(Sky.hlsliのSkyViewセクション参照)
#define KURENAI_SKYVIEW_REGISTER t0
#include "Sky.hlsli"

cbuffer SkyIntegrateConstants : register(b0)
{
    // xyz=太陽が「ある」向き(正規化済み)、w=未使用
    float4 SunDirection;
    // x=目標照度[lx](SunLighting::SkyIlluminanceLux)、y=実効プリ露出(effectiveExposure)、
    // z=タービディティ(KurenaiEngine3D::m_SkyTurbidity)、
    // w=空の彩度(アート指定。KurenaiEngine3D::m_SkySaturation。Sky.hlsliのSkySaturation参照)
    float4 IntegrateParams;

    // --- 以下はP18の第2段(雲込みの空の照度)専用。FrameConstantsの同名の枠と
    //     **完全に同じ内容**であること。食い違うと「背景に見えている雲」と
    //     「大気遠近が想定している雲」が別物になる ---
    float4 CloudParams0;  // x=積雲の被覆率、y=雲底高度[m]、z=UVスケール、w=消散係数
    float4 CloudParams1;  // xy=スクロール量、z=前方散乱g、w=厚み[m](0ならレイマーチしない)
    float4 CloudParams2;  // x=巻雲の被覆率、y=高度[m]、z=UVスケール、w=消散係数
    float4 CloudParams3;  // xy=スクロール量、z=異方スケール、w=雲の種類の偏り
    float4 FogParams0;    // x=消散係数[1/m]、y=スケールハイト[m]、z=基準高度[m]、w=有効フラグ
    // xyz=視点のワールド座標(レイの起点)、w=太陽照度/空照度比(SunToSkyIlluminanceRatio)
    float4 ViewerAndSunRatio;
};

RWStructuredBuffer<GPUSkyParameters> SkyParametersOut : register(u0);

static const uint kThetaSteps = 64;
static const uint kPhiSteps = 256;
static const float kSkyIntegratePI = 3.14159265359f;

// 各スレッド(=φインデックス)の部分和をここへ書き、ツリー加算で集約する
groupshared float s_Partial[256];

// ============================================================================
// P18: 雲込みの空の照度(第2段)
//
// 【何を求めるか】CloudSkyLight = (雲込みの空の照度) / (晴天の空の照度) をRGBで求める。
// 大気遠近のin-scatter(airlight)を照らしているのは視線の先の空ではなく、その空間を
// 照らしている光=空全体の照度である。掛け算1つで大気遠近が曇り空へ追従する。
// 意味と、なぜZenithLuminanceを暗くする形にしないのかはSky.hlsliの
// SkyParameters::CloudSkyLightのコメントに書いてある。
//
// 【循環しない理由】雲込みの放射輝度は clearColor * T + S で、clearColor も S も
// (S の基準になる sunIlluminance = 比 × SkyIlluminanceOverZenith × 天頂輝度 を通じて)
// **天頂輝度に線形**である。したがって比を取れば天頂輝度は約分され、単位天頂輝度
// (ZenithLuminance = 1)で計算してよい。一方 SkyIlluminanceOverZenith は第1段の積分値
// そのものなので、第1段のツリー加算が終わってバリアを抜けた後でしか使えない。
// だから第2段は第1段の**後**に置く。
//
// 【被覆率0で厳密に(1,1,1)になる作り】分母を第1段の細かい格子(16,384方向)ではなく、
// 第2段と**同じ256方向**で積む。こうすると被覆率0では分子と分母が方向ごとに同一の
// 値になり、求積の刻みの違いが比へ漏れない。さらに積雲・巻雲とも被覆率0なら第2段
// ごと飛ばして(1,1,1)を書くので、雲を持たないシーンでは1命令も増えない。
//
// 【方向数】1スレッド1方向。θ16×φ16=256。半球平均という積分量なので、方向ごとの
// 高周波(雲の切れ目)は打ち消し合う。ただし求積誤差はベイクごとに揺れうるので、
// 遠景の霞がちらつかないかは実測で確かめること
// ============================================================================
static const uint kCloudThetaSteps = 16;
static const uint kCloudPhiSteps = 16;

// 雲込み・晴天それぞれの部分和。RGBなのでfloat3の配列2本
groupshared float3 s_CloudySum[256];
groupshared float3 s_ClearSum[256];

[numthreads(256, 1, 1)]
void CSIntegrateSky(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const uint phiIndex = dispatchThreadID.x;

    // 呼び出し側の慣習(SkyGenerate.hlsl等)に合わせ、受け取った向きはここで正規化する。
    // 太陽天頂角(thetaSun/cosThetaSun)はSkyColorUpperUnit内部で求めるため、
    // ここでは保持しない
    const float3 sunPosition = normalize(SunDirection.xyz);

    const float dTheta = (kSkyIntegratePI * 0.5f) / float(kThetaSteps);
    const float dPhi = (kSkyIntegratePI * 2.0f) / float(kPhiSteps);

    // 色味はθ・φに依存しないため1度だけ求める(CPU版ComputeSkyZenithScaleと同じく
    // 呼び出し元が1度だけ決めてループへ渡す構造)
    const SkyTintSet tintSet = ComputeSkyTintSet(sunPosition.y);

    // タービディティ。C++側からIntegrateParams.zで渡される。
    // SkyColorUpperUnitはこれを読まない(濁りはSkyView LUTへ焼き込み済み)が、
    // GPUSkyParametersへそのまま載せてログ・デバッグから見えるようにしている
    const float turbidity = IntegrateParams.z;
    const float skySaturation = IntegrateParams.w;
    // 物理モデル(Hillaire)の重み。仰角0度で0(従来ティントのみ)、
    // 仰角5度で1(物理モデルのみ)。Sky.hlsli SkyColorUpperUnitの早期脱出/クロスフェードと
    // 同じ閾値であること
    const float physicalSkyWeight = smoothstep(0.0f, sin(radians(5.0f)), sunPosition.y);

    // SkyColorUpperUnitを呼ぶためのSkyParameters。ZenithLuminanceと雲パラメータはこの関数が
    // 参照しないため0で埋める(SkyColorUpperUnitのコメント参照。ZenithLuminanceを参照すると
    // 循環定義になるため、この関数は絶対に参照しない設計になっている)
    SkyParameters unitParams = (SkyParameters)0;
    unitParams.SunDirection = sunPosition;
    unitParams.ZenithTint = tintSet.Zenith;
    unitParams.HorizonTint = tintSet.Horizon;
    unitParams.GroundTint = tintSet.Ground;
    unitParams.SunGlowTint = tintSet.SunGlow;
    unitParams.SunGlowStrength = tintSet.SunGlowStrength;
    unitParams.Turbidity = turbidity;
    unitParams.SkySaturation = skySaturation;
    unitParams.PhysicalSkyWeight = physicalSkyWeight;

    const float phi = (float(phiIndex) + 0.5f) * dPhi;

    // 中点則でθ64ステップぶん積む(1スレッドあたり64サンプル)
    float partialSum = 0.0f;
    [loop]
    for (uint ti = 0; ti < kThetaSteps; ++ti)
    {
        const float theta = (float(ti) + 0.5f) * dTheta;
        const float cosThetaRaw = cos(theta);
        const float sinTheta = sin(theta);

        const float3 dir = float3(sinTheta * cos(phi), cosThetaRaw, sinTheta * sin(phi));

        // 被積分関数は「SkyColorUpperUnitの結果のRec.709輝度」。SkyColorUpperUnit内部で
        // SkyColorUpperと同じ地平線のクランプが行われる。
        // 日中はSkyView LUTを引くため、このパスもLUTをSRVで読む。したがって
        // SkyViewBakeパスより後に実行されなければならない(レンダーグラフの依存で保証)
        const float3 unitColor = SkyColorUpperUnit(dir, unitParams);
        const float weight = dot(unitColor, float3(0.2126f, 0.7152f, 0.0722f));

        // dω = sinθ dθ dφ、余弦重みはcosθ(クランプ前のcosThetaRawを使う。CPU版と同じ)
        partialSum += weight * cosThetaRaw * sinTheta * dTheta * dPhi;
    }

    s_Partial[phiIndex] = partialSum;
    GroupMemoryBarrierWithGroupSync();

    // ツリー加算(ペアワイズ和)。256要素を128→64→…→1まで畳み込む
    [unroll]
    for (uint stride = 128; stride > 0; stride >>= 1)
    {
        if (phiIndex < stride)
        {
            s_Partial[phiIndex] += s_Partial[phiIndex + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    // ================= 第2段(P18): 雲込みの空の照度 =================
    // 【ここより前に置けない】被積分関数の中で SkyIlluminanceOverZenith(=第1段の積分値)が
    // 要るため、上のツリー加算が終わって全スレッドが s_Partial[0] を読めるようになってから
    // でないと始められない。ループ末尾のバリアで同期済み
    const float skyIlluminanceOverZenith = s_Partial[0];

    // 雲を評価するためのSkyParameters。ティント類は第1段の unitParams をそのまま使う。
    // 【ZenithLuminance = 1(単位天頂輝度)にする理由】求めるのは比なので天頂輝度は約分
    // される。1にしておけば「まだ確定していない天頂輝度」を参照せずに済み、循環しない
    SkyParameters cloudParams = unitParams;
    cloudParams.ZenithLuminance = 1.0f;
    cloudParams.SkyIlluminanceOverZenith = skyIlluminanceOverZenith;
    cloudParams.SunToSkyIlluminanceRatio = ViewerAndSunRatio.w;
    // 【ここは必ず1.0にする(P18b)】SkyColorWithRayは雲の手前の霞の色をCloudSkyLightで
    // 直す(CloudAirlightCorrection参照)。その値は**いまここで求めようとしているもの**
    // なので、参照すると循環する。1.0=補正なしにして、補正前の空の照度から比を求める。
    //
    // 【この1段だけで足りる理由】補正が効くのは霞に埋もれた雲、つまり (1 - 霞の透過率) が
    // 大きい低い仰角に限られる。この積分は余弦重みなので低い仰角の寄与そのものが小さく、
    // 反復しても比はほとんど動かない。出荷の霞(消散係数5e-5)では天頂方向の霞の透過率が
    // 0.93あり、補正の掛かる余地は7%しかない。
    // **(SkyParameters)0のまま放置してはいけない** —— 0だと補正項が
    // clearColor * (0-1) * (1-霞) となり、霞の濃い方向の空を丸ごと引き算してしまう
    cloudParams.CloudSkyLight = float3(1.0f, 1.0f, 1.0f);

    // 雲・巻雲。**FrameConstants側の詰め方(DeferredLighting.hlslのMakeSkyParameters)と
    // 完全に同じ順で読むこと**。ずれると背景と別の雲を積分することになる
    cloudParams.CloudCoverage = CloudParams0.x;
    cloudParams.CloudAltitude = CloudParams0.y;
    cloudParams.CloudUvScale = CloudParams0.z;
    cloudParams.CloudDensity = CloudParams0.w;
    cloudParams.CloudScrollOffset = CloudParams1.xy;
    cloudParams.CloudForwardG = CloudParams1.z;
    cloudParams.CloudThickness = CloudParams1.w;
    cloudParams.CirrusCoverage = CloudParams2.x;
    cloudParams.CirrusAltitude = CloudParams2.y;
    cloudParams.CirrusUvScale = CloudParams2.z;
    cloudParams.CirrusDensity = CloudParams2.w;
    cloudParams.CirrusScrollOffset = CloudParams3.xy;
    cloudParams.CirrusAnisotropy = CloudParams3.z;
    cloudParams.CloudTypeBias = CloudParams3.w;
    // 霞と視点位置。星空はこのヘルパが0で潰す(このパスは星を描かない)
    cloudParams = ApplyCloudFogParameters(cloudParams, FogParams0, ViewerAndSunRatio.xyz);
    // レイマーチの開始位置のずらし。画面座標が無いので方向インデックスから作る。
    // 【なぜ要るか】全方向が同じ位置からマーチを始めると、スライスの切れ目が方向間で
    // 揃って積分値に偏りとして残る。方向ごとにずらせば1歩の内側でばらけて相殺される
    const float2 ditherSeed = float2(float(phiIndex & 15u), float(phiIndex >> 4u));
    cloudParams.RaymarchJitter = CloudRaymarchDither(ditherSeed);

    // 1スレッド1方向。θ16×φ16=256
    const float dThetaCloud = (kSkyIntegratePI * 0.5f) / float(kCloudThetaSteps);
    const float dPhiCloud = (kSkyIntegratePI * 2.0f) / float(kCloudPhiSteps);
    const float thetaCloud = (float(phiIndex / kCloudPhiSteps) + 0.5f) * dThetaCloud;
    const float phiCloud = (float(phiIndex % kCloudPhiSteps) + 0.5f) * dPhiCloud;
    const float cosThetaCloud = cos(thetaCloud);
    const float sinThetaCloud = sin(thetaCloud);
    const float3 cloudDir =
        float3(sinThetaCloud * cos(phiCloud), cosThetaCloud, sinThetaCloud * sin(phiCloud));
    const float cloudWeight = cosThetaCloud * sinThetaCloud * dThetaCloud * dPhiCloud;

    // 【分母も同じ256方向で積む】第1段の16,384方向で割ると求積の刻みの違いが比へ漏れ、
    // 被覆率0でも1.000にならない。同じ格子・同じ方向で割れば、被覆率0のとき
    // SkyColorWithRayの早期脱出により cloudy と clear が方向ごとに同一の値になり、
    // 和も一致して比が厳密に1.0になる
    const float3 clearRadiance = SkyClearColor(cloudDir, cloudParams);
    const float3 cloudyRadiance = SkyColorWithRay(
        cloudParams.ViewerPosition, cloudDir, kCloudBackgroundRayDistance, cloudParams);

    s_ClearSum[phiIndex] = clearRadiance * cloudWeight;
    s_CloudySum[phiIndex] = cloudyRadiance * cloudWeight;
    GroupMemoryBarrierWithGroupSync();

    [unroll]
    for (uint cloudStride = 128; cloudStride > 0; cloudStride >>= 1)
    {
        if (phiIndex < cloudStride)
        {
            s_ClearSum[phiIndex] += s_ClearSum[phiIndex + cloudStride];
            s_CloudySum[phiIndex] += s_CloudySum[phiIndex + cloudStride];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    // スレッド0が最終値をまとめてバッファへ書く
    if (phiIndex == 0)
    {
        const float integral = s_Partial[0];
        const float targetIlluminance = IntegrateParams.x;
        const float effectiveExposure = IntegrateParams.y;

        // 積分がゼロ近傍になることは無い想定だが、ゼロ除算だけは防いでおく
        // (CPU版ComputeSkyZenithScaleの同じ判定。警告ログはHLSLから出せないため、
        // 検証は積分値そのもの(Luminance.y)をバッファへ残すことで行う)
        const float zenithLuminance =
            (integral < 1e-6f ? targetIlluminance : targetIlluminance / integral) * effectiveExposure;

        GPUSkyParameters result;
        result.ZenithTint = float4(tintSet.Zenith, 0.0f);
        result.HorizonTint = float4(tintSet.Horizon, 0.0f);
        result.GroundTint = float4(tintSet.Ground, 0.0f);
        result.SunGlowTint = float4(tintSet.SunGlow, tintSet.SunGlowStrength);
        result.Luminance = float4(zenithLuminance, integral, 0.0f, 0.0f);
        // タービディティとPreethamの重みもここで確定させて配る
        // (SkyGenerate.hlsl/DeferredLighting.hlsl/SSR.hlslはApplySkyParametersFromBufferで読むだけ)
        result.ModelParams = float4(turbidity, physicalSkyWeight, skySaturation, 0.0f);

        // P18: 雲込みの空の明かり。晴天の照度が0近傍(夜で従来ティントも0)のときだけ
        // ゼロ除算になりうるので、その成分は1.0(無変化)へ倒す。**0にしてはいけない**――
        // 大気遠近のin-scatterが丸ごと消えて遠景が黒く抜ける
        const float3 clearIlluminance = s_ClearSum[0];
        const float3 cloudyIlluminance = s_CloudySum[0];
        const float3 cloudSkyLight = float3(
            clearIlluminance.r > 1e-9f ? cloudyIlluminance.r / clearIlluminance.r : 1.0f,
            clearIlluminance.g > 1e-9f ? cloudyIlluminance.g / clearIlluminance.g : 1.0f,
            clearIlluminance.b > 1e-9f ? cloudyIlluminance.b / clearIlluminance.b : 1.0f);
        result.CloudSkyLight = float4(cloudSkyLight, 0.0f);

        SkyParametersOut[0] = result;
    }
}
