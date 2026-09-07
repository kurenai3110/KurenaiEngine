#pragma once

#include <cstddef>
#include <cstdint>

#include <DirectXMath.h>

// 描画パス全体が register(b0) で共有する定数バッファ。
//
// 【HLSL側の宣言はここが唯一の出所】同じレイアウトを
// KurenaiEngine/Shaders/3D/ShaderInterop/FrameConstants.hlsli が宣言しており、
// .hlsl / .hlsli はそれを include するだけで自前の cbuffer を書かない。
// 以前は27本のHLSLが「先頭からの前方一致」として手で再宣言しており、
// 途中へフィールドを挿すと後ろを宣言している十数本が黙ってオフセットずれを起こし、
// **コンパイルは通るのに絵だけが壊れた**(経緯は docs/ImplementationHistory.md)。
//
// 【フィールドを足すときの手順】
//   1. ここへ足す(原則は末尾。途中へ挿すなら下の static_assert がすべて動くので、
//      期待値も一緒に更新して「どのフィールドが何バイト動くか」を明示すること)
//   2. FrameConstants.hlsli の同じ位置へ同じ型で足す
//   3. 下の static_assert に1行足す
// HLSL側は全フィールドを常に宣言するため、「使わないから宣言を省く」ことはもう起きない。
namespace Kurenai::ShaderInterop
{
    // カスケードシャドウマップの段数。
    //
    // 【KurenaiEngine3D::kCascadeCount と同じ値でなければならない】この構造体は
    // KurenaiEngine3D に依存しない(逆に KurenaiEngine3D.h がこれを取り込む)ため、
    // 独立に定義している。食い違いは KurenaiEngine3D.cpp の static_assert が止める
    constexpr uint32_t kCascadeCount = 4;

    // DDGIのクリップマップLOD配列の要素数。
    //
    // 【KurenaiEngine3D::kDDGIMaxLODCount と同じ値でなければならない】理由は
    // kCascadeCount と同じ。食い違いは KurenaiEngine3D.cpp の static_assert が止める
    constexpr uint32_t kDDGILODCount = 4;

    struct alignas(16) FrameConstants
    {
        DirectX::XMFLOAT4X4 ViewProj;
        DirectX::XMFLOAT4X4 InvViewProj;
        // カスケードシャドウマップ(CSM)用、カスケードごとのライト視点ビュー・プロジェクション行列。
        // 要素数を変えるとこれより後ろのフィールドが全部動くので、kCascadeCountと
        // FrameConstants.hlsliの[4]を同時に直すこと(下のoffsetofのstatic_assertが検出する)
        DirectX::XMFLOAT4X4 CascadeViewProj[kCascadeCount];
        DirectX::XMFLOAT4 CameraPosition;
        DirectX::XMFLOAT4 LightDirection;
        DirectX::XMFLOAT4 LightColor;
        // SSAOパスがView空間でのサンプリングに使う(末尾に追加し、既存シェーダのオフセットは変えない)
        DirectX::XMFLOAT4X4 View;
        DirectX::XMFLOAT4X4 Proj;
        // 昼夜サイクル用(末尾に追加し、既存シェーダのオフセットは変えない)。rgb=環境光の色
        // (m_IBLSettings.AmbientScale乗算済み、Render()側のconstants.AmbientColor代入部を参照)、
        // a=昼度(0=夜,1=昼。m_IBLSettings.AmbientScaleは掛けない)
        DirectX::XMFLOAT4 AmbientColor;
        // M2: カスケード選択・PCSS用(末尾に追加)。xyzw = 各カスケードのView空間far距離
        DirectX::XMFLOAT4 CascadeSplits;
        // x: PCSSのライトサイズ(m_ShadowSettings.LightSize)。y: IBLプリフィルタ済み鏡面マップの
        // 最大ミップレベル(kIBLPrefilterMipLevels-1、DeferredLighting.hlslがラフネス→ミップの
        // 変換に使う)。z: IBL強度倍率(m_IBLSettings.Enabled=falseの場合は0.0fを渡し、シェーダ側で
        // EvaluateIBLの代わりに定数色アンビエント(AmbientColor.rgb)へフォールバックする)。
        // w: スペキュラのマルチスキャッタリング・エネルギー補正の方式
        // (m_ReflectionSettings.SpecularCompensation。0=Off / 1=Linear / 2=Series / 3=Kulla-Conty。
        // 共有ヘッダーSpecularEnergy.hlsliのKURENAI_SPEC_COMP_*と一致させること。14.9節)
        DirectX::XMFLOAT4 ShadowParams;
        // 半透明パス(Transparent.hlsl)専用。x=t8のライトリストの有効数。DirectLighting.hlslは
        // 専用のLightingConstants(b1)で受け取るためこのフィールドを使わない(末尾に追加のため
        // 既存シェーダのオフセットは変わらない)
        DirectX::XMFLOAT4 ActiveLightCount;
        // 拡散IBLの取得元切り替え(末尾に追加のため既存シェーダのオフセットは変わらない)。
        // x: 0(既定)=プリフィルタ済み鏡面の最終ミップ(roughness=1)、1=従来の専用
        // イラディアンスマップ(t8。検証用に残している経路)。CSPrefilterはV=R=Nを仮定して
        // いるため、roughness=1(α=1)ではGGXインポータンスサンプリングの実効カーネルが
        // コサイン畳み込みへ厳密に退化し、格納値もCSIrradianceと同じE(N)/πになる(14.10節)。
        // 反射プローブの拡散イラディアンスにもまったく同じ規則を適用する(19.7節)。
        // y: 環境光の拡散倍率(m_IBLSettings.AmbientDiffuseScale)、z: 同じく鏡面倍率
        // (m_IBLSettings.AmbientSpecularScale)。どちらもIBLの有効/無効に関わらず効く。w: 未使用
        DirectX::XMFLOAT4 IBLParams;
        // 反射プローブ用(末尾に追加)。x=有効プローブ数(0ならプローブを使わずグローバルIBLのみ)、
        // y=影響範囲のデバッグ表示フラグ、z=視差補正の有効フラグ、w=プローブ間ブレンドの有効フラグ。
        // DeferredLighting.hlslとSSR.hlslが読む
        DirectX::XMFLOAT4 ProbeParams;
        // 反射プローブの距離キューブ用(末尾に追加、19.12節)。x=視差補正に距離キューブを使うフラグ、
        // y=距離キューブによる遮蔽判定(光漏れ抑制)の有効フラグ、z=距離キューブの1面の解像度
        // (テクセル。ReflectionProbe.hlsliのProbeDistanceBiasが1テクセル幅の見積もりに使う。
        // ハードコードせずここから渡すのは、kProbeCaptureSizeを変えたときに黙ってずれないため)、
        // w=焼いた時点の実効プリ露出から現在の実効プリ露出への換算倍率(19.14節。
        // m_ProbeBakedExposureEV100のコメントに理由がある)
        DirectX::XMFLOAT4 ProbeParams2;
        // TAA用(末尾に追加のため既存シェーダのオフセットは変わらない)。前フレームの
        // ビュー射影行列(TAAのジッターを含んだままのもの)。GBuffer.hlslが頂点をこの行列でも
        // 変換し、今フレームの投影位置との差からモーションベクター(速度)を求める。
        // 初回フレームとTAAの履歴リセット時は今フレームのViewProjと同じ値を入れる
        // (未定義値が速度バッファへ焼き込まれるのを防ぐため)
        DirectX::XMFLOAT4X4 PrevViewProj;
        // TAAのサブピクセルジッター量(末尾に追加)。xy=今フレーム、zw=前フレーム。
        // 単位はUV(=ピクセルオフセット / レンダー解像度)。
        //
        // 【なぜ速度からジッターを引くのか】ジッターは投影行列に入れてあるので、ViewProjと
        // PrevViewProjで投影した位置の差にはジッターの差も混ざる。しかしジッターは
        // 「同じ面のどこをサンプルしたか」の違いであって「ものが動いた量」ではない。
        // 引いておかないとTAAが履歴を引く位置が毎フレーム±0.5px揺れ、いつまでも収束しない
        DirectX::XMFLOAT4 TAAParams;
        // DDGI用(さらに末尾に追加、22章)。サンプリング側(DeferredLighting.hlsl)が必要とする値だけを
        // 置く。ヒステリシスや最大レイ距離は焼く側にしか要らないのでDDGIUpdateConstantsが持つ。
        //   DDGIParams0: xyz=ボリュームの最小コーナー(ワールド)、w=有効フラグ(0なら従来のIBLのまま)
        //   DDGIParams1: xyz=プローブ間隔、w=法線バイアス(遮蔽判定の照会点を面から浮かせる量)
        //   DDGIParams2: xyz=各軸のプローブ数、w=視線バイアス
        //   DDGIParams3: x=イラディアンスの1辺のテクセル数(境界を含まない)、
        //                y=距離モーメントの1辺のテクセル数(同)、z=拡散間接光の強度倍率、w=未使用
        // テクセル数をハードコードせずここから渡すのは、ProbeParams2.zと同じ理由
        // (C++側の定数を変えたときにシェーダーとの対応が黙ってずれないため)
        DirectX::XMFLOAT4 DDGIParams0;
        DirectX::XMFLOAT4 DDGIParams1;
        DirectX::XMFLOAT4 DDGIParams2;
        //                y=距離モーメントの1辺のテクセル数(同)、z=拡散間接光の強度倍率、
        //                w=境界の幅(テクセル)
        DirectX::XMFLOAT4 DDGIParams3;
        // DDGIParams4: x=このフレームの実効プリ露出(m_EffectiveExposureEV100の線形倍率)、
        //             y=1/2解像度で評価するか、z=未使用、
        //             w=プローブ分類のしきい値(裏面ヒット率がこれを超えたら
        //               そのプローブを信用しない。0以下なら分類を無効にする)。
        //
        // 【アトラスは露出非依存の単位で持つ】ライトの色にはCPU側で実効プリ露出が
        // 事前乗算されており(21.5節)、その倍率は時刻に連動して最大18段(約26万倍)動く。
        // アトラスへプリ露出済みの値をそのまま溜めると、時刻が変わった瞬間に
        // 「古い露出で焼かれた数値」を新しい露出の値として読むことになる。
        // DDGIは多重バウンスで自分自身へフィードバックするため、このズレが増幅され続け、
        // 夜を挟んで昼に戻すと画面が数倍明るいまま戻らなくなる(実測で12時の平均輝度が
        // 45.6→132.9)。そこで書き込み時にこの倍率で割り、読み出し時に掛け直して、
        // アトラスの中身を露出に依存しない物理量に保つ。
        // R32で確保してある(22.6節)ので、夜の小さな値でもfp32の範囲に余裕がある
        DirectX::XMFLOAT4 DDGIParams4;
        // DDGIのクリップマップLOD(31.4.2節)。LOD k は間隔が ProbeSpacing * 2^k。
        //
        // DDGILODOrigin[k].xyz … LOD k の格子の原点(ワールド)。k=0はDDGIParams0.xyzと同じ値
        // DDGILODBase[k].xyz   … その原点に対応する格子の整数座標。
        //                        トロイダル(剰余)addressingの基準。
        //
        // 【どちらもCPUで求めて渡す】原点÷間隔をシェーダー側でも計算すると、丸めが
        // 食い違ったときにプローブのワールド位置とアトラスのセルがずれる。
        // ずれても絵は出るので気づけない。「同じ量を2か所で導出しない」ための冗長さである。
        //
        // 【この位置にあるのは経緯による】宣言が27本に散っていた頃、末尾へ足すと
        // DDGIを読む6本が間の14フィールドをオフセット合わせのためだけに宣言する
        // 羽目になるので、ここへ入れた。いまは宣言が1本なので、この配慮は要らない
        DirectX::XMFLOAT4 DDGILODOrigin[kDDGILODCount];
        DirectX::XMFLOAT4 DDGILODBase[kDDGILODCount];
        // bent normalによる遮蔽用(34章)。
        // x=ディフューズAOの出所   0=従来のベイクAO(Material.b) / 1=aoN = dot(N, bRaw)
        // y=スペキュラ遮蔽の方式   0=Frostbite近似(従来)      / 1=bent normalの錐体交差
        // z=multi-bounce AO       0=無効(既定)                / 1=有効
        // w=未使用
        //
        // xとyは同じ積分の別推定量どうしの切り替えなので、0と1で見た目がほぼ変わらないことが
        // そのまま検証になる。zだけは見た目を大きく変えるため既定を無効にしてある。
        //
        // 【この位置を動かしてはいけない】ここまでのオフセットは他ブランチと共有している。
        // 新しいフィールドは下のTimeParams以降と同じく末尾へ足すこと
        DirectX::XMFLOAT4 OcclusionParams;
        // 水面用(さらに末尾に追加)。x=水面法線マップのスクロール
        // オフセット(0〜1、CPU側で既にfmod済み)、y=波のスケール倍率(m_WaterSettings.WaveScale)、
        // z=波の強さ(m_WaterSettings.WaveStrength、0〜1)、w=未使用。Water.hlslのPSMainが読む。
        DirectX::XMFLOAT4 TimeParams;
        // 空の解析評価用(さらに末尾に追加)。DeferredLighting.hlslが背景画素で
        // Sky.hlsliのSkyColorを画面解像度で評価するために使う。太陽方向以外の値
        // (ティント4本・天頂輝度)は手続き空のベイクと同じタイミングでSkyIntegrate.hlslが
        // m_SkyResources.ParametersBufferへ書き、両者が同じ空を描くことを保証する。
        // SkySunDirection: xyz=太陽が「ある」向き、w=未使用。
        //   【正規化はシェーダ側で行う】sunLighting.SunPositionは解析的にはほぼ単位長だが、
        //   SkyGenerate.hlsl側の慣習(呼び出し側=SkyParameters組み立て時にnormalizeする)に
        //   合わせ、C++側では正規化せずそのまま渡す(DeferredLighting.hlslのMakeSkyParameters参照)。
        //   **LightDirectionでは代用できない**——あちらは支配ライトの向きで、月が支配的な
        //   夜には月の向きになる。Perez分布のcircumsolar項は常に太陽を基準にする
        DirectX::XMFLOAT4 SkySunDirection;
        // SkyParams: x=未使用(天頂輝度はSkyParametersBufferにある)、
        //   y=背景を解析評価するかのフラグ(1=解析、0=キューブマップをサンプル)、
        //   z=太陽照度/空照度比(SunToSkyIlluminanceRatio。sunLighting.KeyIlluminanceLux /
        //   sunLighting.SkyIlluminanceLuxから求める。Sky.hlsliのEvaluateCloudLayerが雲の
        //   明るさの基準を太陽の照度にするために使う。雲を照らしているのは空ではなく
        //   太陽であるため、天頂輝度基準では雲が原理的に空より暗くしかならなかった
        //   問題への対処)、w=未使用。
        //   yは手続き空が無効(.ksceneのDDSスカイボックス使用時)は常に0にする
        //   (DDSは任意の絵でPerezモデルとは無関係なため、解析評価してはいけない)。
        //   ティント4本(SkyZenithTint/SkyHorizonTint/SkyGroundTint/SkySunGlowTint)は
        //   m_SkyResources.ParametersBuffer(GPUSkyParameters、SkyIntegrate.hlslが書く)にあり、
        //   このFrameConstantsには持たない)。
        DirectX::XMFLOAT4 SkyParams;
        // 雲(さらに末尾に追加)。背景と水面反射が同じ雲を描くための値
        // (Sky.hlsli冒頭のコメント・各シェーダーのMakeSkyParametersのコメント参照)。
        // CloudParams0: x=被覆率(0で雲なし。Sky.hlsliのSkyColorが早期脱出する)、
        //               y=雲底の高度[m](**ワールドYの絶対高度**。P17より前はカメラ相対だった)、
        //               z=UVスケール[ノイズ空間の距離/m]、w=消散係数
        DirectX::XMFLOAT4 CloudParams0;
        // CloudParams1: xy=風によるノイズ空間の移動量(CPU側でSky.hlsliのkCloudNoisePeriodと
        //               同じ周期でstd::fmod済み。m_CloudScrollOffset参照)、
        //               z=Henyey-Greensteinの非対称パラメータ、w=未使用
        DirectX::XMFLOAT4 CloudParams1;
        // 巻雲(さらに末尾に追加)。
        // CloudParams2: x=巻雲の被覆率(0で巻雲なし。Sky.hlsliのSkyColorが早期脱出する)、
        //               y=雲底の高度[m](**ワールドYの絶対高度**。P17より前はカメラ相対だった)、
        //               z=UVスケール[ノイズ空間の距離/m]、w=消散係数
        DirectX::XMFLOAT4 CloudParams2;
        // CloudParams3: xy=風によるノイズ空間の移動量(CPU側でSky.hlsliのkCloudNoisePeriodと
        //               同じ周期でstd::fmod済み。m_CirrusScrollOffset参照)、
        //               z=fBmのUV(U方向)を伸ばす異方性スケール(m_CloudSettings.CirrusAnisotropy)、
        //               w=積雲の種類の偏り(m_CloudSettings.TypeBias、C4)。C4より前は未使用だった枠なので
        //               FrameConstantsは1バイトも増えていない
        DirectX::XMFLOAT4 CloudParams3;
        // 平面反射(さらに末尾に追加)。xyz=水面平面の法線(現状は常に(0,1,0))、
        // w=平面の距離項。PlanarReflection.hlslのVSMainが
        // SV_ClipDistance0 = dot(worldPos, xyz) + w として使い、水面より上で正になるようにする
        // (水面より下のジオメトリを反射に映さないため)。このシェーダー以外は参照しない
        DirectX::XMFLOAT4 PlanarReflectionPlane;
        // 大気遠近(さらに末尾に追加)。AerialPerspective.hlsl/PlanarReflection.hlslが読む。
        // x=基準高度での消散係数[1/m]、y=スケールハイト[m]、z=基準高度[m](ワールドY)、
        // w=有効フラグ(0で無効。UIでオフ、またはシーンが手続き空を使っていない場合に0にする。
        // 手続き空が無効なシーンでは大気遠近のin-scatter項(SkyColorの解析評価)が意味を持たない
        // ため、SSR.hlslのwaterAnalyticSkyFlagと同じ判断をRender()側で行う)
        DirectX::XMFLOAT4 FogParams0;
        // x=不透明度の上限(1.0で完全に空の色まで行く)、yzw=未使用
        DirectX::XMFLOAT4 FogParams1;
        // 水中項(さらに末尾に追加)。xyz=水体の色(リニア)、w=未使用。Water.hlslのPSMainが
        // メッシュ自身のBaseColorFactorの代わりにこの色を出力Albedoに使う
        // (見下ろした水面がFresnel最小(約0.02)でほぼ真っ黒になる問題への対処。詳細はWater.hlsl参照)
        DirectX::XMFLOAT4 WaterBodyColor;
        // 星空(さらに末尾に追加)。Sky.hlsliのEvaluateStarfieldが読む。
        // x=強度(0で完全に無効。昼はCPU側で0にする)、y=密度(天球を割るセルの細かさ)、
        // z=またたきの強さ、w=1画素が張る角度[rad](星がこれを下回らないようにする)。
        //
        // 【読むのはDeferredLighting.hlslとSSR.hlslだけ】背景と水面の映り込みにしか
        // 星を出さないため。AerialPerspective.hlsl(フォグのin-scatter)と
        // SkyGenerate.hlsl(IBLキューブ)は自分のMakeSkyParametersで0を入れる
        DirectX::XMFLOAT4 StarsParams;
        // 雲の品質(さらに末尾に追加)。x=積雲のボリュームレイマーチの段数、yzwは予備。
        //
        // 【読むのはSkyCloud.hlslだけ】ボリューム経路を持つのがこのシェーダーだけだからである。
        // 他のシェーダー(SSR/PlanarReflection/AerialPerspective)は厚みゼロの平面経路を通り、
        // レイマーチそのものを行わないのでこの値を必要としない。
        //
        // 【オクターブ数と自己影の段数はここへ入れない】あれらはfBmの値そのものを変えるため、
        // 実行時に動かすとボリューム経路と平面経路で雲の形が食い違い、
        // 背景の雲と水面に映る雲が別物になる(Sky.hlsliのCloudRaymarchStepsのコメント参照)
        DirectX::XMFLOAT4 CloudQualityParams;
        // Hi-Zオクルージョンカリング(さらに末尾に追加、Stage 5-2)。
        // 読むのはGBufferMeshlet.hlslの増幅シェーダーだけ。
        //
        // x=有効フラグ(0で判定そのものを行わない)、y=バウンディング球の半径倍率
        // (m_GeometrySettings.OcclusionCullRadiusScale)、z=前フレームからのカメラ移動距離[m]、
        // w=Hi-Zのミップ段数(m_HiZMipLevels)。
        //
        // 【xを明示的なフラグにする理由】判定はPrevViewProjで投影するが、プローブ
        // キャプチャや平面反射のパスはPrevViewProjへViewProjを入れて潰している
        // (前フレームという意味を持たない)。そこで判定が動くと、Hi-Zの中身とは
        // 別の視点の行列で投影して見えているものを消す。行列の中身から推し量るのではなく、
        // 「メインカメラのG-Bufferパスか」をCPU側で決めてここへ渡す
        DirectX::XMFLOAT4 OcclusionCullParams;
        // 同じくHi-Zオクルージョンカリング用。xy=Hi-Zのミップ0の解像度[画素]、zw=その逆数。
        // NDC→UV→テクセル座標の変換に要る(FrameConstantsはレンダー解像度を持っていない)
        DirectX::XMFLOAT4 HiZScreenParams;
        // メッシュレットカリングの統計(さらに末尾に追加、Stage 5-2)。
        // x=有効フラグ、y=カウンタバッファのbindless番号(UAVの側)、zw=未使用。
        //
        // 【なぜbindlessで渡すのか】メッシュシェーダー用ルートシグネチャはSRVテーブルと
        // サンプラーテーブルしか持たず、UAVレンジが無い。増幅シェーダーから書き込むには
        // ResourceDescriptorHeap経由しかない(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXEDは立っている)
        DirectX::XMFLOAT4 MeshletCullStatsParams;

    };

    // 【cbufferのレイアウトを固定する本体】HLSL側は宣言順でオフセットが決まる。
    // C++側で並べ替え・挿入・型変更が起きるとここが即座に落ちるので、
    // FrameConstants.hlsli を直し忘れたまま黙って絵が壊れることはない。
    //
    // 【期待値を「通すために」書き換えないこと】オフセットが動いたのなら、
    // HLSL側の宣言も同じだけ動いていなければならない。値を合わせて済ませると
    // この仕掛けそのものが無意味になる(Assets/ModelPackage.h と同じ規約)。
    //
    // 【float4 が16バイト境界に載ることが前提】HLSLのcbufferは float4 を
    // またがせないため、16の倍数から外れた時点でHLSL側と食い違う
    static_assert(offsetof(FrameConstants, ViewProj) == 0, "FrameConstants.hlsli の ViewProj と位置が食い違っている");
    static_assert(offsetof(FrameConstants, InvViewProj) == 64, "FrameConstants.hlsli の InvViewProj と位置が食い違っている");
    static_assert(offsetof(FrameConstants, CascadeViewProj) == 128, "FrameConstants.hlsli の CascadeViewProj と位置が食い違っている");
    static_assert(offsetof(FrameConstants, CameraPosition) == 384, "FrameConstants.hlsli の CameraPosition と位置が食い違っている");
    static_assert(offsetof(FrameConstants, LightDirection) == 400, "FrameConstants.hlsli の LightDirection と位置が食い違っている");
    static_assert(offsetof(FrameConstants, LightColor) == 416, "FrameConstants.hlsli の LightColor と位置が食い違っている");
    static_assert(offsetof(FrameConstants, View) == 432, "FrameConstants.hlsli の View と位置が食い違っている");
    static_assert(offsetof(FrameConstants, Proj) == 496, "FrameConstants.hlsli の Proj と位置が食い違っている");
    static_assert(offsetof(FrameConstants, AmbientColor) == 560, "FrameConstants.hlsli の AmbientColor と位置が食い違っている");
    static_assert(offsetof(FrameConstants, CascadeSplits) == 576, "FrameConstants.hlsli の CascadeSplits と位置が食い違っている");
    static_assert(offsetof(FrameConstants, ShadowParams) == 592, "FrameConstants.hlsli の ShadowParams と位置が食い違っている");
    static_assert(offsetof(FrameConstants, ActiveLightCount) == 608, "FrameConstants.hlsli の ActiveLightCount と位置が食い違っている");
    static_assert(offsetof(FrameConstants, IBLParams) == 624, "FrameConstants.hlsli の IBLParams と位置が食い違っている");
    static_assert(offsetof(FrameConstants, ProbeParams) == 640, "FrameConstants.hlsli の ProbeParams と位置が食い違っている");
    static_assert(offsetof(FrameConstants, ProbeParams2) == 656, "FrameConstants.hlsli の ProbeParams2 と位置が食い違っている");
    static_assert(offsetof(FrameConstants, PrevViewProj) == 672, "FrameConstants.hlsli の PrevViewProj と位置が食い違っている");
    static_assert(offsetof(FrameConstants, TAAParams) == 736, "FrameConstants.hlsli の TAAParams と位置が食い違っている");
    static_assert(offsetof(FrameConstants, DDGIParams0) == 752, "FrameConstants.hlsli の DDGIParams0 と位置が食い違っている");
    static_assert(offsetof(FrameConstants, DDGIParams1) == 768, "FrameConstants.hlsli の DDGIParams1 と位置が食い違っている");
    static_assert(offsetof(FrameConstants, DDGIParams2) == 784, "FrameConstants.hlsli の DDGIParams2 と位置が食い違っている");
    static_assert(offsetof(FrameConstants, DDGIParams3) == 800, "FrameConstants.hlsli の DDGIParams3 と位置が食い違っている");
    static_assert(offsetof(FrameConstants, DDGIParams4) == 816, "FrameConstants.hlsli の DDGIParams4 と位置が食い違っている");
    static_assert(offsetof(FrameConstants, DDGILODOrigin) == 832, "FrameConstants.hlsli の DDGILODOrigin と位置が食い違っている");
    static_assert(offsetof(FrameConstants, DDGILODBase) == 896, "FrameConstants.hlsli の DDGILODBase と位置が食い違っている");
    static_assert(offsetof(FrameConstants, OcclusionParams) == 960, "FrameConstants.hlsli の OcclusionParams と位置が食い違っている");
    static_assert(offsetof(FrameConstants, TimeParams) == 976, "FrameConstants.hlsli の TimeParams と位置が食い違っている");
    static_assert(offsetof(FrameConstants, SkySunDirection) == 992, "FrameConstants.hlsli の SkySunDirection と位置が食い違っている");
    static_assert(offsetof(FrameConstants, SkyParams) == 1008, "FrameConstants.hlsli の SkyParams と位置が食い違っている");
    static_assert(offsetof(FrameConstants, CloudParams0) == 1024, "FrameConstants.hlsli の CloudParams0 と位置が食い違っている");
    static_assert(offsetof(FrameConstants, CloudParams1) == 1040, "FrameConstants.hlsli の CloudParams1 と位置が食い違っている");
    static_assert(offsetof(FrameConstants, CloudParams2) == 1056, "FrameConstants.hlsli の CloudParams2 と位置が食い違っている");
    static_assert(offsetof(FrameConstants, CloudParams3) == 1072, "FrameConstants.hlsli の CloudParams3 と位置が食い違っている");
    static_assert(offsetof(FrameConstants, PlanarReflectionPlane) == 1088, "FrameConstants.hlsli の PlanarReflectionPlane と位置が食い違っている");
    static_assert(offsetof(FrameConstants, FogParams0) == 1104, "FrameConstants.hlsli の FogParams0 と位置が食い違っている");
    static_assert(offsetof(FrameConstants, FogParams1) == 1120, "FrameConstants.hlsli の FogParams1 と位置が食い違っている");
    static_assert(offsetof(FrameConstants, WaterBodyColor) == 1136, "FrameConstants.hlsli の WaterBodyColor と位置が食い違っている");
    static_assert(offsetof(FrameConstants, StarsParams) == 1152, "FrameConstants.hlsli の StarsParams と位置が食い違っている");
    static_assert(offsetof(FrameConstants, CloudQualityParams) == 1168, "FrameConstants.hlsli の CloudQualityParams と位置が食い違っている");
    static_assert(offsetof(FrameConstants, OcclusionCullParams) == 1184, "FrameConstants.hlsli の OcclusionCullParams と位置が食い違っている");
    static_assert(offsetof(FrameConstants, HiZScreenParams) == 1200, "FrameConstants.hlsli の HiZScreenParams と位置が食い違っている");
    static_assert(offsetof(FrameConstants, MeshletCullStatsParams) == 1216, "FrameConstants.hlsli の MeshletCullStatsParams と位置が食い違っている");
    static_assert(sizeof(FrameConstants) == 1232, "FrameConstants の総サイズが変わっている(定数バッファの確保サイズもHLSL側の末尾も動く)");
    static_assert(alignof(FrameConstants) == 16, "cbufferは16バイト境界で扱う");
}
