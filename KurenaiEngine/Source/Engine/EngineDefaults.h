#pragma once

#include <cstdint>

namespace Kurenai::Defaults
{
    // KurenaiEngine3Dの各パラメータの既定値。
    //
    // 「KurenaiEngine3D.hのメンバ初期化子」と「UIの『既定値に戻す』」の2箇所に同じ数値を
    // 書くと必ずずれるため、ここを唯一の出所とし、両方がここを参照する。
    //
    // 注意: SSAO/SSILの半径・厚み・SSRの距離/厚みは、シーン読み込みのたびに
    // KurenaiEngine3D::ResetSceneDependentParams()がシーンの対角長から上書きするため、
    // ここの値は「シーンを読む前の初期値」でしかない。UI側はそれらだけ
    // 「既定値に戻す」ではなく「シーンから再計算」を提供する

    // --- AO / 間接光 ---
    inline constexpr bool AOEnabled = true;
    // マテリアルの遮蔽マップ(glTFのocclusionTexture。22章)を間接光へ掛けるか。
    // 上のAOEnabled(スクリーンスペースAO/GI)とは独立した別系統で、
    // 無効にするとObjectConstants.OcclusionStrengthへ0が渡り遮蔽マップの寄与が消える
    inline constexpr bool OcclusionMapEnabled = true;
    inline constexpr float SSAORadius = 0.5f;
    // 遮蔽率にかける指数。1.0は「求めた遮蔽率をそのまま使う」で、上げるほど遮蔽が濃くなる。
    // 素の遮蔽率を基準にしたいため既定は1.0
    inline constexpr float SSAOPower = 1.0f;
    // 1画素あたりのカーネルサンプル数(1〜16)。上限はSSAO.hlslのkSSAOKernelSizeMaxと
    // 定数バッファの配列長で決まっている。SSAOのコストはほぼこの数に比例する
    inline constexpr uint32_t SSAOKernelSize = 16;
    inline constexpr float SSILRadius = 0.5f;
    inline constexpr float SSILThickness = 0.01f;
    inline constexpr float SSILIntensity = 2.0f;
    // SSAOPowerと同じ意味・同じ理由で1.0
    inline constexpr float SSILPower = 1.0f;
    inline constexpr uint32_t SSILSliceCount = 4;
    inline constexpr uint32_t SSILStepCount = 6;

    // --- 深度プリパス(41.22節) ---
    // G-Bufferを描く前に不透明ジオメトリの深度だけを埋め、隠れる画素のピクセルシェーダーを
    // 早期Zで省く。ジオメトリを1周ぶん余計に描くコストと引き換えなので、
    // オーバードローが小さいシーンでは損になる
    inline constexpr bool DepthPrepassEnabled = true;

    // --- フラスタムカリング ---
    // メッシュ単位のフラスタムカリング(モデル単位の判定を通ったあとの、もう一段)。
    //
    // 【切れるようにしてある理由は対照実験のため】カリングは「効いていない」と
    // 「間引きすぎて物が消えた」のどちらも絵からは判別しにくい。同じ起動の中で
    // ON/OFFを切り替えて絵と間引き数を比べられないと、「差分ゼロ」が
    // 「変わらないのが正しい」なのか「そもそも実行されていない」なのかを区別できない。
    // OFFにすると判定を1回も呼ばないので、統計は「判定なし」になる。
    // モデル単位のカリングは常に有効(こちらは切れない)
    inline constexpr bool MeshCullingEnabled = true;

    // --- シャドウ ---
    inline constexpr bool ShadowEnabled = true;
    inline constexpr float ShadowLightSize = 0.02f;

    // --- IBL / 環境光 / スペキュラ ---
    inline constexpr bool IBLEnabled = true;
    inline constexpr float IBLIntensity = 0.5f;
    inline constexpr bool IBLUseDedicatedIrradiance = false;
    // 拡散イラディアンスの球面調和関数(SH L2)経路。CSIrradiance(総当たり積分)の高速な代替。
    // 既定はfalse。IBLUseDedicatedIrradianceが有効な場面でのみ意味を持つ
    inline constexpr bool IBLUseSHIrradiance = false;
    // SHのウィンドウ関数(Sloan)の強さ。0=無効(既定)。リンギングが実測で出た場合のつまみ
    inline constexpr float SHWindowLambda = 0.0f;

    // bent normalによる遮蔽(34章)。
    // BentNormalAOSource: ディフューズAOを aoN = dot(N, bRaw) から取るか(false = 従来のベイクAO)。
    //   既定でbent normal側を使う。同じ積分の別推定量なので見た目は大きく変わらない。
    // SpecularOcclusionMode: スペキュラ遮蔽の方式。KurenaiEngine3D::SpecularOcclusionMode と
    //   HLSLのComposeSpecularOcclusionのsoModeに対応する(0=Frostbite近似 / 1=球冠交差 /
    //   2=球面ガウス)。SpecularCompensationModeと同じ理由でintで持つ。
    //   既定は2(SG) ―― 球冠交差(1)は d >= av+as で厳密に0になり、金属の凹部が純黒へ
    //   潰れる(34.10節)。SGは常に正なので方向性を保ったまま潰れない(34.11節)
    // MultiBounceAOEnabled: multi-bounce AO(Jimenez 2016)。アルベドが明るいほどAOを弱める補正で、
    //   見た目を大きく変えるためbent normal自体の検証を汚さないよう既定は無効
    inline constexpr bool BentNormalAOSource = true;
    inline constexpr int SpecularOcclusionMode = 2;
    inline constexpr bool MultiBounceAOEnabled = false;
    inline constexpr float AmbientScale = 0.2f;
    // 環境光(間接光)の拡散・鏡面それぞれに掛かる倍率。既定の1.0は「何も変えない」値で、
    // IBL強度(IBLIntensity)が拡散と鏡面へ一様に掛かるのに対し、こちらは両者の比率を崩す
    // ための画作り用のつまみ。IBLの有効/無効に関わらず効く(無効時の定数色アンビエントにも
    // 同じ倍率が掛かる)ので、切り替えても意味が変わらない
    inline constexpr float AmbientDiffuseScale = 1.0f;
    inline constexpr float AmbientSpecularScale = 1.0f;
    // スペキュラのマルチスキャッタリング・エネルギー補正の方式。
    // KurenaiEngine3D::SpecularCompensationMode と HLSL の KURENAI_SPEC_COMP_* に対応する
    // (0=Off / 1=Linear / 2=Series / 3=Kulla-Conty)。ここを型付きにするには enum を
    // このヘッダーへ持ち込む必要があるが、EngineDefaults.hは値だけを置く方針なのでintで持つ。
    // 既定のLinearは、実使用域で3方式のうち最も真値に近いことを実測で確認した結果(14.9.8節)
    inline constexpr int SpecularCompensationMode = 1;

    // --- SSR ---
    // レイトレーシング反射に非対応な環境で、SSRを反射の既定の手法にするか。
    // SSRは画面に映っているものしか反射に映せず、画面端で反射が途切れる破綻が目立つため既定は無効
    // (=「反射なし」で起動する)。DXR Tier 1.1対応環境では従来どおりRT反射が既定になる
    // (手法の決定はKurenaiEngine3D::ApplyLoadedScene。EngineDefaults.h冒頭の注意も参照)
    inline constexpr bool SSREnabled = false;
    inline constexpr float SSRMaxDistance = 5.0f;
    inline constexpr float SSRThickness = 0.1f;
    inline constexpr float SSRRoughnessCutoff = 0.6f;

    // --- 水面 ---
    // .ksceneの[Water]セクションが無い場合の既定値。[Water]がある場合はScene::WaterWaveScale等が
    // これらの値をリテラル複製した既定値で初期化され、そちらがKurenaiEngine3D::m_WaterWaveScale等を
    // 上書きする(Scene.hのコメント参照)。ここの値自体は.kscene側のコメントに残した暫定値の流用
    inline constexpr float WaterWaveScale = 12.0f;
    inline constexpr float WaterWaveSpeed = 0.03f;
    inline constexpr float WaterWaveStrength = 0.25f;
    // シーンに依存しないUIつまみ(Scene::WaterWaveScale等と違い.ksceneのキーを持たない)
    inline constexpr bool WaterTimeFrozen = false;
    // 水面のSSR反射で、レイが画面外へ抜けた・最大距離まで判定がつかなかったときに解析空
    // (Sky.hlsli)へフォールバックするか(SSRの水面分岐)。既定ON。
    // 【現状の効果は小さい】空が滑らかな勾配しか持たない間は、プリフィルタ済み鏡面を
    // 低ミップで引いた値と解析評価した値がほぼ一致する(実測: 8bitで最大1、平均0.67)。
    // 差が大きくなるのは空に高周波の要素(雲)が入ってからで、そのとき128pxベースの
    // プリフィルタでは雲の輪郭が色斑に潰れる。既定でONにしてあるのは、
    // 解析評価のほうが情報を落とさない上位互換だから。
    // 手続き空が無効なシーンでは実際には効かない
    // (KurenaiEngine3D::m_WaterAnalyticSkyReflectionのコメント参照)
    inline constexpr bool WaterAnalyticSkyReflection = true;

    // --- 平面反射(水面への鏡像描画) ---
    inline constexpr bool PlanarReflectionEnabled = true;
    // 反射解像度の倍率(1/2)。水面はラフネスが低いとはいえ波の法線で画面UVを歪ませて引くため
    // 等倍の解像度は要らず、フォワードパス(不透明メッシュ全体)をもう1回走らせるコストの方が
    // 支配的なため、半分に落として負荷を抑える
    inline constexpr float PlanarReflectionResolutionScale = 0.5f;
    // 波の法線による画面UVのずらし量(SSR.hlsl参照)。UV空間の小さな値から始め、実測で調整可能
    inline constexpr float PlanarReflectionDistortion = 0.02f;

    // --- 雲(積雲1層のレイヤーモデル) ---
    inline constexpr bool CloudEnabled = true;
    // 被覆率。0.40は「晴れ間と雲がおよそ半々」という写真の見た目に寄せた値であり、
    // 物理的な導出ではない(実測で調整可能)。実測では、雲を流しながら8枚撮った
    // 画角内の青空の割合が中央値45.7%になる(下のCloudUvScaleのコメント参照)。
    //
    // 【この目盛りは線形ではない】被覆率Cはシェーダー側で remap(fBm, 1-C, 1) のしきい値として
    // 使われる。fBmは4オクターブの平均なので[0,1]に一様ではなく平均0.499・標準偏差0.132に
    // 集中しており(実測: ハッシュ単体は平均0.497・標準偏差0.289とほぼ完全な一様分布なので、
    // 集中はオクターブ平均によるもの)、Cと「実際に雲になる空の割合」は大きく食い違う。
    //   C=0.30 ->  6.5%   C=0.35 -> 13.7%   C=0.40 -> 23.8%
    //   C=0.45 -> 36.1%   C=0.50 -> 49.5%   C=0.60 -> 75.7%
    // 0.4〜0.5の狭い範囲に見た目が集中しているので、ここを動かすときは小刻みに動かすこと。
    // (この換算表はfBmの実装に依存する。CloudFbmのオクターブ数や合成方法を変えたら測り直す)
    inline constexpr float CloudCoverage = 0.40f;
    // 雲底の高度[m]。積雲の雲底高度として一般に言われる目安(だいたい1,000〜2,000m)の
    // 中間を採った値(精密な気象観測値ではなく目安からの採用)
    inline constexpr float CloudAltitude = 1500.0f;
    // ノイズ空間のUVスケール[ノイズ空間の距離/m]。1セル=1,000m。
    //
    // 【1セルの大きさではなく「画角に何セル入るか」で決める】「積雲1個がおよそ2km」だから
    // 1/2000、という決め方をしてはいけない。この値が本当に効くのは雲そのものの寸法ではなく、
    // 画角の中に雲と隙間が何回交代して現れるかである。雲底1,500mの平面を仰角θで見ると交点までの
    // 水平距離は1500/tanθで、画角の水平半角36.4度(FovY=45度・16:9)から画面が覆う横幅が決まる:
    //   仰角37度(カメラを15度上げたときの画面上端) -> 水平距離1,990m -> 画面の横幅は約2,940m
    // 1セル2kmだとここに1.5セルしか入らない。つまり画面上端は「1個の雲」か「1個の隙間」の
    // どちらかで埋まり、どちらになるかはノイズの引きだけで決まる。実測でもこの運任せがそのまま出た
    // (雲を流しながら8枚撮った画角内の青空の割合。カメラを15度上げた構図):
    //   1セル2,000m … 最小20.4% / 中央38.0% / 最大58.9%  標準偏差12.3pt
    //   1セル1,000m … 最小33.9% / 中央44.3% / 最大53.6%  標準偏差 5.5pt
    // 中央値はほとんど動かない(=被覆率の設定だけでは決まらない)。効くのは
    // 「悪い引き」が無くなることで、標準偏差は半分以下になる。
    //
    // 【1,000mという値】上の2,940mに雲と隙間の交代が3回以上入る条件から 2940/3 = 980m。
    // 参考に1セル800mも測ったが、画面上端の標準偏差が10.3ptへ戻り改善しなかったため採らない。
    //
    // 【なぜ効きが大きいか】風速の既定5m/sとこのUVスケールでは、ノイズ空間は毎秒
    // 0.005セルしか進まない(1セル進むのに3分以上かかる)。起動時のスクロール量は0なので、
    // 実用上ほぼ常に「スクロール量0の引き」だけを見ることになる。1セル2,000mでは
    // その引きが極端に悪く、カメラを15度上げると青空が1.9%しか残らない
    // (同じ引きで1セル1,000mにすると41.2%)。分布の中央値ではなくこの1枚が
    // 「空が全面曇っている」という体感を作る
    inline constexpr float CloudUvScale = 1.0f / 1000.0f;
    // 消散係数。既定の光路長(地平線際でクランプ後の最大約20倍)でも雲の芯が十分不透明に見え、
    // かつ薄い箇所では下の空が透けるバランスを見た目で探った調整値(実測で調整可能)
    inline constexpr float CloudDensity = 8.0f;
    // 風速[m/s]。そよ風〜軟風程度(ビューフォート風力階級2〜3相当)の値を感覚的に採用した
    // 調整値であり、実測値ではない
    inline constexpr float CloudWindSpeed = 5.0f;
    // 風向き(度)。太陽方位角の既定値と特に関係を持たせる理由が無いため、東(0度)を既定にした
    inline constexpr float CloudWindDirectionDegrees = 0.0f;
    // Henyey-Greensteinの非対称パラメータ。前方散乱が強すぎると太陽周辺だけが不自然に
    // 明るい点になるため、縁が仄かに光る程度に留めた調整値(実測で調整可能)
    inline constexpr float CloudForwardG = 0.6f;
    // 積雲をボリューム(スラブのレイマーチ)として描くか。無効にすると従来の
    // 厚みゼロの平面レイヤーに戻る。負荷と見た目を直接比べるためのA/Bトグルでもある
    inline constexpr bool CloudVolumetric = true;
    // 雲底から雲頂までの厚み[m]。1,000mは並雲(cumulus mediocris)の目安で、
    // 既定の雲底1,500mと合わせると雲頂は2,500mになる。晴天時によく見る「もこもこした綿雲」の
    // 縦横比(1セル=1,000mに対して縦1,000m)に相当する。精密な気象観測値ではなく目安からの採用
    inline constexpr float CloudThickness = 400.0f;
    // シーンに依存しないUIつまみ(m_WaterTimeFrozenと同じ位置づけ)。
    // 積雲・巻雲の両方に効く(片方だけ凍結できるとA/B比較の対照が取れなくなるため)
    inline constexpr bool CloudTimeFrozen = false;
    // 積雲のボリュームレイマーチの段数(1〜32)。上限はSky.hlsliの
    // kCumulusRaymarchStepsMaxと一致させること。雲パスのコストの主なつまみで、
    // 1画素あたりのウェザーマップ評価18回(視線 + 自己影5 + 基底1)の大半がこのループになる
    inline constexpr uint32_t CloudRaymarchSteps = 12;

    // --- 巻雲(積雲の上に重ねる2層目) ---
    //
    // 【この5つの値の決め方】いずれも見た目からの調整値で物理的な導出ではない。
    // 有効/無効の切り替えで空領域の画素が23.8%動き(平均差2.20/255)、高層に筋雲が読み取れる
    // ことを実測で確かめてある。**被覆率0.3・UVスケール1/6000・消散係数0.8のような
    // 組み合わせにしてはいけない** ―― 同じ比較で4.5%(平均差0.25/255)しか動かず、
    // 巻雲がほぼ見えなくなる(効くのは主にUVスケール。下のCirrusUvScaleのコメント参照)
    inline constexpr bool CirrusEnabled = true;
    // 被覆率。remap(fBm, 1-被覆率, 1)で塊に整形するため、この値が低いとfBmの上位だけが
    // 残ってまばらな筋になる。積雲と同程度にして空の広い範囲へ薄く掛ける
    inline constexpr float CirrusCoverage = 0.5f;
    // 雲底の高度[m]。巻雲の高度帯として一般に言われる目安は5,000〜13,000mで、その中ほどを
    // 採った値(精密な気象観測値ではなく目安からの採用。積雲のCloudAltitudeと同じ性格)
    inline constexpr float CirrusAltitude = 8000.0f;
    // ノイズ空間のUVスケール[ノイズ空間の距離/m]。1セル=2,000m。
    // 【積雲(1セル1,000m)と値が違ってよい理由】視線と雲底平面の交点までの距離は高度に比例する
    // ため、同じUVスケールでも高度が高い層ほど画角内に入るセル数が増える。CloudUvScaleの
    // コメントで使ったのと同じ計算を高度8,000mでやると、カメラを15度上げたときの画面上端
    // (仰角37度)で水平距離10,600m・画面の横幅は約15,700m、1セル2,000mなら7.8セル入る。
    // 積雲が1.5セルしか入らず運任せになっていたのに対し、巻雲は元から十分な数が入っていたので
    // ここは動かさない。巻雲の細い筋・積雲の大きな塊という見え方の違いは、この高度差から自然に出る。
    // これを1/6000のような大きなセルにすると画角内に1〜2セルしか入らず、模様が大きすぎて筋として読めない
    inline constexpr float CirrusUvScale = 1.0f / 2000.0f;
    // 消散係数。巻雲は光学的に薄く下の青空が透けるのが特徴なので積雲(8.0)より大幅に小さい。
    // 0.8まで下げると上の被覆率・UVスケールと相まってほぼ見えなくなったため、透けは保ちつつ
    // 存在は分かる値まで戻してある
    inline constexpr float CirrusDensity = 2.0f;
    // 風速[m/s]。高層ほど風が速いため積雲(5.0)より大きくしてある。感覚的に採用した
    // 調整値であり実測値ではない。風向きは積雲と共有する(CloudWindDirectionDegrees)
    inline constexpr float CirrusWindSpeed = 15.0f;
    // fBmのUV(U方向)を伸ばして筋状にする倍率。巻雲の「刷毛で掃いたような筋」を作るための
    // 見た目からの調整値。1.0で積雲と同じ等方的な塊になる
    inline constexpr float CirrusAnisotropy = 3.0f;

    // --- 大気遠近(height fog / aerial perspective) ---
    // 以下の数値はいずれも見た目からの調整値であり、物理的な導出や実測値ではない
    // (親セッション側の実機確認で調整可能。KurenaiEngine3D::m_FogDensity等のコメント参照)
    inline constexpr bool FogEnabled = true;
    // 基準高度(FogRefHeight)での消散係数[1/m]。
    // 【この値の根拠】消散係数は気象学的視程Vと Koschmieder の関係 sigma = 3.912 / V で結び付く
    // (コントラスト閾値2%での定義)。0.0004 は V ≒ 10km に相当し、「晴れているが遠景がわずかに
    // 霞む」日に当たる。最初に置いていた 0.0015 は V ≒ 2.6km(もや)に相当し、実測すると
    // 600m先の島でも透過率が0.5を切って絵全体が灰色に潰れた。ここは調整可能なつまみだが、
    // 動かすときは「どのくらいの視程を想定するか」で決めるのが分かりやすい
    inline constexpr float FogDensity = 0.0004f;
    // スケールハイト[m]。大きいほど霞が高くまで及ぶ。エアロゾルのスケールハイトとして
    // 一般に言われる目安(おおむね1〜1.5km)の下端を採った(精密な観測値ではなく目安からの採用)。
    // このシーンは地物が最も高い尖塔でも165mしかないため、この値の違いは絵にほとんど出ない
    inline constexpr float FogScaleHeight = 1000.0f;
    // 基準高度[m](ワールドY)。水面の高さに合わせている
    inline constexpr float FogRefHeight = 0.0f;
    inline constexpr float FogMaxOpacity = 1.0f;
    // 水体の色(リニア)。水中で拡散的に後方散乱して戻ってくる光の粗い近似で、
    // 見下ろしたときに水面が何色に見えるかをほぼ一手に決める(見下ろす角度ではFresnelが
    // 約0.03まで下がるため、見えているもののほぼ全部がこの色になる)。
    //
    // 【値の決め方】メッシュのbaseColorFactor程度の暗さ((0.02, 0.03, 0.04)など)では、
    // 見下ろしたときの近距離の水面が輝度15/255程度にしかならず「海が見えない」状態になる
    // (Fresnelが約0.028まで下がるため、見えているもののほぼ全部がこの拡散色になる)。
    // モン・サン=ミシェル湾は土砂を多く含む濁った海なので、澄んだ海水(反射率数%)ではなく
    // 濁水の反射率(おおむね8〜15%と言われる帯域)の下寄りを採る。
    //
    // 【B÷R比を1へ近づけると灰色になる】無彩色に寄せると、Fresnelが下がる見下ろす角度では
    // 海が曇り空のような灰色に見える。輝度をほぼ保ったまま青緑へ寄せてあり、
    // 物理量の実測ではなく見た目からの調整値である
    inline constexpr float WaterBodyColorR = 0.040f;
    inline constexpr float WaterBodyColorG = 0.082f;
    inline constexpr float WaterBodyColorB = 0.085f;

    // --- レイトレーシング反射(DX12かつDXR Tier 1.1対応時のみ選択できる) ---
    // 最大レイ距離はシーン読み込み時に対角長から決め直す(SSRのMaxDistanceと同じ扱い)。
    // SSRより長いのは、画面外まで追えるRTでは短く切ると反射が途中で空へ抜けてしまうため
    inline constexpr float RTReflectionMaxDistance = 50.0f;
    // SSRと同じく1本の鏡面レイしか撃たないため、粗い面ではプローブ/グローバルIBLへ戻す。
    // SSRより高めなのは、RTには「画面外に外れて打ち切り」という破綻要因が無く、
    // 中程度の粗さでも結果が安定しているため
    inline constexpr float RTReflectionRoughnessCutoff = 0.8f;
    // ヒット面から太陽へ影レイを撃つか。切ると反射に映る面の影が消えるが、その分速い
    inline constexpr bool RTReflectionShadowRayEnabled = true;

    // --- レイトレーシングシャドウ(DX12かつDXR Tier 1.1対応時のみ選択できる) ---
    // 1ピクセルあたりに撃つ影レイの本数。デノイザ(時間方向の蓄積)を持たないため、
    // 太陽を大きくする(角半径を上げる)ほどここを増やさないと半影にノイズが出る
    inline constexpr int RTShadowSampleCount = 4;
    // 太陽の見かけの半径(度)。実際の太陽は視直径約0.53度なので既定値はその半分。
    // 大きくすると半影が広く柔らかくなる(が、同じサンプル数ならノイズも増える)
    inline constexpr float RTShadowSunAngularRadiusDegrees = 0.27f;

    // --- レイトレーシングAO/GI(DX12かつDXR Tier 1.1対応時のみ選択できる) ---
    // 半球へ余弦重みで撃つレイの本数。デノイザを持たずAOBlurのボックスブラーだけで均すため、
    // 少なすぎるとブラー後もノイズが残る
    inline constexpr int RTAOSampleCount = 8;
    // レイの最大距離はシーン読み込み時に対角長から決め直す(SSAO/SSILの半径と同じ扱い)。
    // スクリーンスペース手法より長く取れる(画面外の遮蔽物も追えるため)
    inline constexpr float RTAOMaxDistance = 2.0f;
    // 遮蔽率にかける指数。SSAO/SSILと同じ意味・同じ既定値
    inline constexpr float RTAOPower = 1.5f;
    // 間接拡散光の強さ。物理的に正しい値が1.0になるためSSILの2.0より小さい
    // (SSILの重み付けはヒューリスティックで、1.0では暗すぎた)
    inline constexpr float RTAOIntensity = 1.0f;
    // バウンス面から太陽へ影レイを撃つか。切ると間接光に日陰が反映されなくなるが、その分速い
    inline constexpr bool RTAOBounceShadowRayEnabled = true;

    // --- シャドウ(スクリーンスペース) ---
    // ポイント/スポットライトの影。深度バッファに写っている面しか遮蔽物にできず、
    // 得られるのは接触影・中距離の遮蔽に限られる(画面外の物は影を落とさない)。
    // 効果の範囲が限定的な割に全ライトぶんのレイマーチを毎フレーム走らせるため既定は無効
    inline constexpr bool ScreenSpaceShadowEnabled = false;
    inline constexpr int ScreenSpaceShadowStepCount = 16;
    inline constexpr float ScreenSpaceShadowMaxRayLength = 1.5f;
    inline constexpr float ScreenSpaceShadowThickness = 0.5f;
    inline constexpr float ScreenSpaceShadowNormalBias = 0.002f;
    inline constexpr float ScreenSpaceShadowEdgeFade = 0.1f;
    inline constexpr int ScreenSpaceShadowMaxLightsPerPixel = 4;

    // --- タイルドライトカリング ---
    inline constexpr bool LightCullingEnabled = true;

    // 自前ソフトウェアラスタライザ(46章)を実行するか。既定は無効。
    // 比較・検証用の経路で通常の描画には寄与しないため、必要なときだけUIから有効にする
    // (DX12かつSM 6.6 + Int64ShaderOps + bindlessの環境でのみ選択できる)
    inline constexpr bool SoftwareRasterEnabled = false;
    inline constexpr int LightTileHeatmapMax = 8;

    // --- 反射プローブ ---
    inline constexpr bool ReflectionProbeEnabled = true;
    inline constexpr bool ProbeParallaxCorrectionEnabled = true;
    inline constexpr bool ProbeBlendingEnabled = true;
    // 距離キューブを使う2つの機能(19.12節)。どちらも実装・検証は済んでいるが、プローブが疎な
    // 現状では副作用のほうが大きいため既定は無効。理由はKurenaiEngine3D.hの各メンバのコメント参照
    inline constexpr bool ProbeDepthParallaxEnabled = false;
    inline constexpr bool ProbeOcclusionEnabled = false;
    // 距離キューブのデバッグ表示で白飽和する距離。ProbeTestのホール(24×12)が収まる程度
    inline constexpr float ProbeDistanceDebugRange = 20.0f;

    // --- DDGI(22章) ---
    // .ksceneに[GIVolume]が無いシーンでは、このフラグに関わらず何も起きない
    inline constexpr bool DDGIEnabled = true;
    inline constexpr float DDGIIntensity = 1.0f;
    // 1フレームに焼き直すプローブ数。DDGIはヒステリシスで時間収束させる手法なので、
    // 全プローブを毎フレーム焼く必要はない(455個ならこの値で約29フレームで一巡する)
    inline constexpr int DDGIProbesPerFrame = 16;
    // DDGIの拡散イラディアンスを内部レンダー解像度の1/2で評価し、深度を見てアップサンプルするか。
    // 【既定は無効】雲の低解像度化(SkyCloud.hlsl)と違い、DDGIは面の位置と法線の関数なので
    // 数学的に等価ではなく、ジオメトリの輪郭で滲みが出る近似である。品質プリセットの低/中が有効にする
    inline constexpr bool DDGIHalfResolution = false;

    // --- トーンマップ / ディザ ---
    // 8bit出力時のバンディングを散らすディザ。最終出力へノイズを載せる処理であり、
    // スクリーンショットの画素差を取るA/B比較では差分の下限を押し上げてしまうため既定は無効
    inline constexpr bool DitherEnabled = false;
    inline constexpr float MesopicStrength = 0.0f;

    // --- TAA(Temporal Anti-Aliasing) ---
    // 時間方向に蓄積するため、フレームレートの揺れがそのまま画素差になりA/B比較の妨げになる。
    // 残像・半透明メッシュのゴーストといった副作用もあるため既定は無効
    inline constexpr bool TAAEnabled = false;
    // 今フレームの色を履歴へ混ぜる割合。0.1なら毎フレーム1割ずつ入れ替わるので、
    // 静止していれば十数フレームで収束する。上げるとゴーストに強くなる代わりにちらつきが残る
    inline constexpr float TAABlendWeight = 0.1f;
    // ジッターの振れ幅の倍率。1.0でピクセル内いっぱい(±0.5px)に散らす
    inline constexpr float TAAJitterScale = 1.0f;
    // トーンマップ後の黒の締め(ブラックポイント)。0で恒等=既定の見た目を変えない。
    // 屋外の遠景では大気遠近が最暗部へ空の輝度を加算して黒が浮くため、シーン側で
    // [Scene]TonemapBlackPoint を指定して締められるようにしてある
    inline constexpr float TonemapBlackPoint = 0.0f;

    // 蓄積によるボケを補う量。0で無効。TAAの中ではなくTonemapパスで最終出力にのみ掛ける
    inline constexpr float TAASharpness = 0.35f;
    // 近傍クリップのボックス幅(近傍の標準偏差の何倍まで履歴を許容するか)。
    // 小さいほどゴーストに強いがちらつきが増える
    inline constexpr float TAAClipGamma = 1.25f;
    // 静止している画素のちらつきを抑える量。0で無効。動いている画素の挙動は変わらない
    inline constexpr float TAAAntiFlicker = 1.0f;

    // --- ブルーム ---
    // 明部の滲みを足す画作りの処理で、素の輝度分布を確認したいときには邪魔になるため既定は無効
    inline constexpr bool BloomEnabled = false;
    inline constexpr float BloomStrength = 0.06f;
    inline constexpr float BloomThreshold = 1.0f;
    inline constexpr float BloomSoftKnee = 0.5f;

    // --- 自動露出 ---
    // 画面の内容に応じて露出が動くため、カメラを動かすだけで明るさが変わりA/B比較の基準にならない。
    // 無効時はSceneExposureEV100(シーン全体の露出)がそのまま効く固定露出になるため既定は無効
    inline constexpr bool AutoExposureEnabled = false;
    inline constexpr float AutoExposureMinEV100 = -6.0f;
    inline constexpr float AutoExposureMaxEV100 = 18.0f;
    inline constexpr float AutoExposureSpeedUp = 3.0f;
    inline constexpr float AutoExposureSpeedDown = 1.0f;
    inline constexpr float AutoExposureLowPercentile = 0.5f;
    inline constexpr float AutoExposureHighPercentile = 0.95f;
    inline constexpr float AutoExposureCompensation = 0.0f;
    inline constexpr float AutoExposureNightRolloffEV = 4.5f;
    inline constexpr float AutoExposureNightRolloffDarkEV100 = -2.0f;
    inline constexpr float AutoExposureNightRolloffBrightEV100 = 10.0f;
    inline constexpr float AutoExposureKeyCeilingEV = 2.0f;

    // --- 内部レンダー解像度 ---
    // G-Buffer以降すべての中間バッファの解像度。ウィンドウサイズとは独立しており、
    // Presentパスでアスペクト比を保ったままウィンドウへ拡大縮小する(レターボックス/ピラーボックス)。
    // 実行時に「システム」パネルから変更できる
    inline constexpr uint32_t RenderWidth = 1280;
    inline constexpr uint32_t RenderHeight = 720;

    // --- 超解像(FSR1相当のEASU+RCAS。41.23節) ---
    // 有効にすると、上の解像度は「出力解像度」の意味になり、内部レンダー解像度は
    // 品質モードの倍率で割った値が自動で設定される。トーンマップ後のLDR画像を
    // EASUで出力解像度へ再構成し、RCASでシャープ化してからPresentへ渡す。
    // 既定でOFFなのは、有効にすると内部解像度が変わって絵が変わるため。
    // 「速度と引き換えに絵を変える」判断はユーザーがするものであり、
    // 深度プリパス(絵が変わらないので既定ON)とはそこが違う
    inline constexpr bool UpscaleEnabled = false;
    // RCASのシャープネス(0〜1)。0で無効、1で参照実装の最大。
    // 内部で 2^(-2*(1-この値)) へ変換して渡す(FSR1のsharpnessは「ストップ数」で、
    // 0ストップ=最大、大きいほど弱い)。既定の0.25は、
    // TAAのシャープネス(Defaults::TAASharpness)と同程度の効き方になる値
    inline constexpr float UpscaleSharpness = 0.25f;

    // --- カメラ操作(WASD/E/Qの移動速度) ---
    //
    // 【この値は「シーンを読む前の初期値」でしかない】SSAO半径などと同じく、
    // シーン読み込みのたびにResetSceneDependentParams()がシーン対角から決め直す。
    // .ksceneが[Scene]CameraSpeedを持っていればそれが優先される。
    // UI側は「既定値に戻す」ではなく「シーンから再計算」を提供すること
    inline constexpr float CameraSpeed = 5.0f;
    // Shiftを押している間の倍率。20/5 = 4倍という従来の即値をそのまま保つ
    inline constexpr float CameraSpeedShiftMultiplier = 4.0f;
    // 自動決定の基準となるシーン対角[m]と、そのときの速度[m/s]。
    //
    // 【基準をEmeraldSquareにする理由】従来の5 m/sはこのシーンで手に馴染む値として選ばれていた。
    // 対角344.6mは Assets/Packed/EmeraldSquare/Day.kmodel のヘッダAABBから実測した値で、
    // .ksceneのコメント(「対角344.6m、farZ 1378m」)とも一致する。
    // この基準ならEmeraldSquareはちょうど従来どおりの5 m/sになる
    inline constexpr float CameraSpeedReferenceDiagonal = 344.6f;
    // 自動決定の下限[m/s]。
    //
    // 【比例させるだけでは小さいシーンが遅くなる】Sponza(対角37.1m)は比例式だと0.54 m/sになり、
    // 30mの中庭を横切るのに55秒かかる。従来の5 m/sで既に使いやすいシーンをわざわざ遅くする
    // 理由が無いため、基準対角より小さいシーンでは従来値をそのまま据え置く。
    // 上限は設けない ―― 東京23区(実測のシーン対角45,014m)は653.14 m/s、Shiftで2,612.55 m/sになり、
    // 端から端までが68.9秒/17.2秒になる。ここを頭打ちにすると、この機能を入れた目的そのものが消える
    inline constexpr float CameraSpeedMin = 5.0f;

    // --- 同期 ---
    inline constexpr bool VSyncEnabled = false;
    inline constexpr bool FixedFPSEnabled = true;
    inline constexpr float TargetFPS = 60.0f;

    // --- 性能ログ ---
    // FPS・CPU/GPUフレーム時間を一定間隔でログファイルへ出す。プロファイラパネルの表示は
    // その場で消えてしまい後から比較できないため、実行の記録として残すためのもの。
    // 出力は1秒に1行だけなのでフレーム時間への影響は無視できる
    inline constexpr bool FrameStatsLoggingEnabled = true;
    inline constexpr float FrameStatsLogIntervalSeconds = 1.0f;

    // --- デバッグ表示 ---
    inline constexpr float DebugViewGain = 1.0f;

    // --- 太陽 / シーン全体の露出 ---
    inline constexpr bool SunEnabled = true;
    inline constexpr float TimeOfDay = 12.0f;
    inline constexpr bool TimeAutoAdvance = false;
    inline constexpr float TimeAdvanceSpeed = 1.0f;
    inline constexpr float SunAzimuthDegrees = 126.87f;
    // 大気の濁り具合(Preetham xyYモデルのタービディティ)。Preethamの定義域はおおむね
    // 1.7〜10で、2.5は「澄んだ晴天」に相当する見た目からの選択であり、実測値ではない
    inline constexpr float SkyTurbidity = 2.5f;
    // 空の彩度。**物理量ではなく明示的なアート指定**で、既定の1.0はPreethamの色度そのまま
    // (=物理的に導かれた値をいじらない)。色度図上で白色点(D65)から遠ざける倍率なので、
    // 色相は変えずに鮮やかさだけが変わる。
    //
    // 【なぜ物理と分けて持つのか】参考写真の最も深い空はB/R=4.84だが、Preethamは
    // 論文の係数から独立に計算しても1.34〜1.74しか出さない(タービディティを1.8まで下げても
    // 改善しない)。実装はこの予測範囲の中にありモデルに忠実なので、差は実装の誤りではなく
    // モデルの性質であり、物理側をいじっても埋まらない。絵作りが要るシーンは
    // .ksceneの[Scene]SkySaturationで上げること
    inline constexpr float SkySaturation = 1.0f;
    // 月は時刻に連動しない独立した向き。ここを変えると夜空の目標照度が変わるため空の焼き直しが要る
    inline constexpr float MoonAzimuthDegrees = 306.87f;
    inline constexpr float MoonElevationDegrees = 45.0f;
    inline constexpr bool ProceduralSkyEnabled = true;
    // 背景(深度が無い画素)をキューブマップのサンプルではなく、Sky.hlsliのSkyColorを画面解像度で
    // 直接評価するか。キューブマップは256px/面しかなく背景としては拡大表示されるため、
    // 既定でON。手続き空が無効なときはこの設定に関わらずキューブマップが使われる
    inline constexpr bool SkyAnalyticBackground = true;
    inline constexpr float SceneExposureEV100 = 15.0f;
    inline constexpr float EmissiveIntensity = 1.0f;

    // --- 星空 ---
    // 夜空に星を描くか。既定はtrueだが、昼は太陽の仰角で完全に0までフェードするため
    // 昼のシーンの絵は1画素も変わらない(Sky.hlsliのEvaluateStarfield参照)
    inline constexpr bool StarsEnabled = true;
    // 星の密度。空を分割するセルの1辺あたりの数で、大きいほど星が増える。
    // 肉眼で見える恒星は全天で約6,000個。既定値はその桁に合わせてある
    inline constexpr float StarsDensity = 48.0f;
    // 星の明るさ倍率。1.0で「実際の夜空を肉眼で見たときの印象」に寄せた既定
    inline constexpr float StarsBrightness = 1.0f;
    // またたきの強さ。**既定は0(無効)**。TAAと相性が悪くちらつきに見えるうえ、
    // A/B比較のスクリーンショットの再現性も落とすため、必要なときだけ上げる
    inline constexpr float StarsTwinkle = 0.0f;

    // --- ドローンショー ---
    // ショーの中身(点・機体数・保持/変形秒・明るさ・ビルボード半径・揺れ・再生速度・種)は
    // .kshowが持つため、ここには無い。残っているのは「シーンが決める配置」と
    // 「シーンにもショーにも決めさせない描画側の下限」だけ。
    //
    // 既定はfalse。専用シーン(Scenes/DroneShow.kscene)が[DroneShow]Enabled=trueで
    // 有効にする。既定で走らせると全シーンに無関係な描画パスが増えてしまう
    inline constexpr bool DroneShowEnabled = false;
    // 編隊の中心(ワールド座標)。水面より十分上に置くこと
    inline constexpr float DroneShowCenterX = 0.0f;
    inline constexpr float DroneShowCenterY = 220.0f;
    inline constexpr float DroneShowCenterZ = 260.0f;
    // 編隊の代表半径[m]。.kshowの点は代表半径1へ正規化されており、これを掛けて実寸にする
    inline constexpr float DroneShowScale = 130.0f;
    // 画面上の最小半径(NDC単位)。遠方の機体が1画素を割るとTAAのジッターで明滅するため、
    // これ以下にならないようシェーダ側で押し上げる。1280x720で約1.4画素に相当する。
    // 【シーンにもショーにも持たせない】ショーの表現ではなく描画側の下限で、
    // 「1画素を割るとちらつく」という事実はどのシーン・どのショーでも変わらない
    inline constexpr float DroneShowMinScreenRadius = 0.002f;

    // --- Hi-Zオクルージョンカリング(Stage 5-2) ---
    // 増幅シェーダーがメッシュレットのバウンディング球を前フレームのHi-Zへ投影し、
    // 「視界内だが手前の何かに完全に隠れている」塊を落とす。
    //
    // 既定は有効。メッシュシェーダー経路でしか動かないので、非対応環境
    // (DX11、および基準機のIntel UHD 620)ではこの値に関わらず一切走らない
    inline constexpr bool OcclusionCullingEnabled = true;
    // バウンディング球を膨らませる倍率。
    //
    // 【1.0が基準であることに根拠がある】判定に使うHi-Zは1フレーム古いが、その時間差から
    // 来る視差ずれは別項(前フレームからのカメラ移動距離をそのまま半径へ足す)が受け持っている。
    // この倍率が埋めるのはそれとは別の誤差 ―― バウンディング球がメッシュレットの実体より
    // 緩いこと、およびカメラ回転による見え方の変化。どちらもワールド半径に比例するとは
    // 限らないため、まず「膨らませない」1.0から始め、ポップが出たら実測で上げる。
    // 最初から余裕を持たせると、効いていないのか判定が緩いのか区別できなくなる
    inline constexpr float OcclusionCullRadiusScale = 1.0f;
    // メッシュレットカリングの間引き数を数え、Perfログへ出すか。
    //
    // 【既定は有効】この機能は「効いているか」を数値でしか確かめられない ―― 保守的な判定が
    // 正しく働いていれば絵は1画素も変わらないので、絵からは間引けているかどうかが分からない。
    // 既定で切っておくと「有効にしたのに何も起きない」の切り分けが毎回必要になる。
    // 増幅シェーダーのアトミックはグループ単位に集約してあり、切るのは実測して重いと分かってから
    inline constexpr bool MeshletCullStatsEnabled = true;
    // モデル単位のGPUカリング(Stage 5-3)を走らせるか。
    // メッシュレット経路とHi-Zが要るので、非対応環境では走らない
    inline constexpr bool ModelCullGpuEnabled = true;
    // カリング結果で実際に描画発行(ExecuteIndirect)まで行うか。
    //
    // 【切っても判定と計数は動く】falseなら描くのは従来のCPUループのままで、
    // GPU側の判定はカウンタに残る。「判定が正しいか」と「間接描画が速いか」は
    // 別々に確かめたいので、トグルを分けてある。
    // DX11とメッシュシェーダー非対応環境では、この値によらず従来のCPUループへ縮退する
    inline constexpr bool ModelCullIndirectEnabled = true;
}
