// 大気遠近(height fog / aerial perspective)パス。
//
// 反射パス(SSR/RT反射)の後、TAAパスの直前に置くフルスクリーン三角形+ピクセルシェーダー。
// Lightingパスの中に入れなかったのは次の2点が実コードの制約として存在するため:
//   1. SSR(Shaders/3D/SSR.hlsl)はRenderTargets::SceneColorを「反射先の環境色」としてそのまま読む(t0)。
//      Lightingの中でフォグを掛けてしまうとSSRがフォグ済みの色を反射に使い、
//      画面上でフォグが二重に(直接見えている分+反射に映った分)乗ってしまう
//   2. 半透明パス(Transparent.hlsl)はLightingパスの後にRenderTargets::SceneColorへ直接描き足す。
//      Lighting内でフォグを掛けるとその後に描かれる半透明サーフェスだけフォグを免れてしまう
// TAAより前に置くのは、フォグが深度から決まる純関数で時間方向に揺れないため
// (TAA自身が時間方向のノイズを均す側に回れる。逆にTAAの後ろへ置くと、フォグが作る
// 勾配の強い縁がジッターで解決されないままTAAをすり抜けてエイリアシングを残す)。
//
// 頂点シェーダー(フルスクリーン三角形)とReconstructWorldPosはSSR.hlslのものと完全に同一の内容を
// 複製している(SSR.hlslはcbuffer/テクスチャの宣言と一体になっており、この2つだけを
// 抜き出してヘッダー化すると余計な結合が増えるため、複製のほうを選んだ)。
#include "Samplers.hlsli"
// 大気遠近の透過率(cbufferに依存しない純粋関数)。PlanarReflection.hlslと共有する
#include "HeightFog.hlsli"
// 空モデル(Perez分布)の共有ヘッダー。in-scatter項(フォグの合成先の色)に、背景と同じ
// SkyColorをそのまま使うことで、遠方の地物が無限遠で背景の空色へ厳密に収束するようにする
// (詳細はSky.hlsli冒頭のコメント、および本ファイルPSMain末尾のコメント参照)
// SkyView LUT。日中の空はこのLUTを引く。**定義しないと日中の空が黒くなる**ので、
// SkyColorUpperUnitを呼ぶシェーダーは全員定義すること(Sky.hlsliのSkyViewセクション参照)
#define KURENAI_SKYVIEW_REGISTER t3
#include "Sky.hlsli"

#include "ShaderInterop/FrameConstants.hlsli"

// t0=入力のSceneColor(反射パスの出力。GetActiveReflectionOutput())、t1=深度、
// t2=SkyIntegrate.hlslが書いた空パラメータ(SSR.hlsl等と同じStructuredBuffer)
Texture2D SceneColorTexture : register(t0);
Texture2D DepthTexture : register(t1);
StructuredBuffer<GPUSkyParameters> SkyParametersBuffer : register(t2);

#include "ShaderInterop/FullscreenTriangle.hlsli"

#include "ShaderInterop/Common.hlsli"

#include "ShaderInterop/SkyFrameParameters.hlsli"

float4 PSMain(PSInput input) : SV_TARGET
{
    float3 sceneColor = SceneColorTexture.Sample(ColorSampler, input.UV).rgb;

    float depth = DepthTexture.Sample(DataSampler, input.UV).r;
    if (depth <= 0.0f)
    {
        // Reverse-Zのため背景(スカイ)の深度は0.0。背景は既にDeferredLighting.hlslの解析評価
        // (またはスカイボックス)で正しい値になっており、経路長も定義できないためフォグを掛けない。
        // ここで1ビットも変えずに素通しすることが、フォグ無効時との差分ゼロを担保する
        return float4(sceneColor, 1.0f);
    }

    if (FogParams0.w <= 0.5f)
    {
        // 無効(UIでオフ、またはシーンが手続き空を使っていない。FogParams0.wのコメント参照)。
        // 恒等関数として振る舞い、以降の計算は一切行わない
        return float4(sceneColor, 1.0f);
    }

    float3 worldPos = ReconstructWorldPos(input.UV, depth);
    const float transmittance =
        HeightFogTransmittance(CameraPosition.xyz, worldPos, FogParams0.x, FogParams0.y, FogParams0.z);
    const float alpha = saturate(1.0f - transmittance) * saturate(FogParams1.x);

    // in-scatterの方向は視線方向のyを0以上へクランプしたものを使う。水平線より下を向いた画素で
    // SkyColorをそのまま評価すると下半球のGroundTint(暗い接地色)が返り、遠景が霞むのではなく
    // 黒ずんでしまう。水平線(viewDir.y==0)では両者が一致するため、クランプしても背景との
    // 連続性(depth<=0の早期脱出との継ぎ目)は保たれる
    const float3 viewDir = normalize(worldPos - CameraPosition.xyz);
    const float3 clampedDir = float3(viewDir.x, max(viewDir.y, 0.0f), viewDir.z);
    const float clampedLength = length(clampedDir);
    // 真下をちょうど向いた画素ではclampedDirが零ベクトルになり、normalizeがNaNを返す。
    // NaNはこの後のTAA・ブルーム・自動露出を経由して画面全体へ伝播するため必ず退避させる。
    // 該当するのは視線が厳密に真下の1画素だけで、その周囲の画素は既に水平方向を向いている
    // (x,zがどれだけ小さくても比だけで方向が決まる)ため、退避先も水平方向の任意の1方向でよい
    const float3 fogDir =
        (clampedLength > 1e-5f) ? (clampedDir / clampedLength) : float3(0.0f, 0.0f, 1.0f);

    // 【Mie位相関数は掛けない】SkyColorUpperが返す値には既にPerez分布のgamma項(太陽角距離の項)と
    // SunGlowTintが入っており、太陽方向で明るくなる角度依存性を既に持っている。その上に
    // 位相関数をさらに掛けると前方散乱を二重に計上することになるため、ここでは掛けない。
    //
    // 【雲を含むSkyColorではなく、晴天のSkyColorUpperを使う】
    // ここで必要なのは「カメラと着目点の間にある空気」が散乱して視線へ入れてくる光(airlight)で
    // あって、無限遠から届く空の放射輝度そのものではない。雲は高度1,000m以上のレイヤーで、
    // 視線が雲底平面と交わるのは水平距離で数kmから数十km先——カメラと数百m先の地物の間には
    // 存在しない。にもかかわらずSkyColorを使うと、地物の手前に無いはずの雲の輝度が
    // in-scatterに乗る。雲は青空の3倍以上明るく、しかもfBmで空間的に激しく変動するため、
    // 暗い地物ほど「雲の模様が透けて見える」形で破綻する(実測: 雲が流れる6秒間で塔の画素が
    // 最大72・平均5.7動き、これは同じ2フレーム間の空そのものの変化量(最大86・平均6.3)と
    // ほぼ同じ=雲がほぼ素通しで地物へ焼き付く)。
    //
    // 【地平線での背景との連続性は保たれる】無限遠へ収束するのは視線が水平(fogDir.y==0)の
    // ときだけで、Sky.hlsliの雲の地平線フェードはsmoothstep(kCloudHorizonFadeEndY=0,
    // kCloudHorizonFadeStartY=0.2, dir.y)——すなわちdir.y==0でフェードが厳密に0になり、
    // そこではSkyColorとSkyColorUpperの値が一致する。よってこのパスの設計目標である
    // 「遠方の地物が背景の空色へ厳密に収束し水平線に継ぎ目が出ない」は成立したままになる。
    // なおfogDirはyを0以上へクランプ済みなのでSkyColorの地面フェード分岐
    // (dir.y < kGroundFadeStartY = -0.02)には決して入らず、SkyColorをSkyColorUpperへ
    // 置き換えることは「雲の合成を外す」ことと厳密に等価になる。
    //
    // 【雲による減光と無彩色化はここで掛ける】(P18) 雲に覆われた空の下では airlight を
    // 照らす光そのものが弱まり、色も無彩色に寄る。SkyColorUpperは雲を通さない晴天の空
    // なので、被覆率1.0で空が灰色一色でも遠方の地物には青い散乱光が掛かり続けていた
    // (実測: 被覆率1.0での水平線際の空はB-R 42・輝度81だが、掛かっていたのは被覆率0の
    // B-R 86・輝度110)。
    //
    // 【視線の先の雲でSkyColorへ替える案は却下した】(a)地物の画素ごとにレイマーチが走って
    // コストが跳ねる。(b)地物までの散乱光を決めるのは「その空間を照らしている光」であって
    // 視線の先の雲ではないので、意味的にもずれる。正しい量は空全体の照度の半球平均であり、
    // それをSkyIntegrate.hlslが1本のRGBとして求めている(Sky.hlsli CloudSkyLight参照)。
    // 被覆率0では厳密に(1,1,1)なので、雲を持たないシーンの画素は1ビットも動かない
    const SkyParameters skyParams = MakeSkyParameters(input.Position.xy);
    const float3 inScatter = SkyColorUpper(fogDir, skyParams) * skyParams.CloudSkyLight;

    const float3 outColor = sceneColor * (1.0f - alpha) + inScatter * alpha;
    return float4(outColor, 1.0f);
}
