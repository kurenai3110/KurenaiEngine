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

    // インスタンシング(同じモデルを指すインスタンスを1回のDrawIndexedへまとめる)。
    //
    // 【切れるようにしてある理由はメッシュ単位カリングと同じ】まとめても絵は変わらないのが
    // 正しいので、絵だけを見ても効いたかどうかが分からない。同じ起動の中でON/OFFを
    // 切り替え、ドローコール数が減ることと絵が一致することの両方を確かめられるようにする。
    //
    // 効くのは同じ.kmodelを複数配置しているシーンだけ(Scenes/InstancingTest.kscene、
    // MultiModelTest.kscene)。PLATEAU・Sponza・Bistroは全モデルがユニークなので
    // ONにしてもバッチが1つも作られず、発行されるコマンドは従来とまったく同じになる
    inline constexpr bool InstancingEnabled = true;

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

    // --- MegaLights(DX12かつDXR Tier 1.1対応時のみ選択できる) ---
    // ポイント/スポットライトの直接光を専用パスで求め、1灯ごとにTLASへ影レイを撃つ。
    // 既定で無効なのは、既存のライトループ(タイルカリング)からの切り替えを
    // シーン側の判断に委ねているため。有効にすると MegaLightsMode::Reference
    //(全灯総当たり。ノイズは無いがライト数に比例して重い)で始まる。
    // 確率的サンプリングは実装済みで、UI か `-megalights 2` で選ぶ
    inline constexpr bool MegaLightsEnabled = false;
    // 1灯あたりに撃つ影レイの本数。**0にすると影を撃たず可視率1で評価する**。
    // その状態の出力は、スクリーンスペースシャドウを切った既存のライトループと
    // 数値的に一致するはずで、移植の取り違えを一度に洗い出せる(恒等テスト)。
    //
    // 【1本のままなのは「答えが変わらないから」ではない ―― かつてそう書いていた】
    // 光源に半径が入った段階で、影レイは球面上の1点へ撃つ確率的な推定になった。
    // 1本では**1灯あたり1標本**しかないので、可視率は0か1に振れる。BistroExteriorNight
    // の街灯は半径4cmで、しかも器具の中から出しきれていない灯があり、球面の一部が
    // 自分の器具に遮られる。そこが「本物の半影」ではなく画面いっぱいの黒い斑点として出る。
    //
    // 【では何が答えを決めるか】参照実装は蓄積(-megalightsaccum)で真値へ寄せる前提で、
    // そのために種へフレーム番号を混ぜてある(MegaLightsReference.hlsl)。128枚で
    // 32本相当に対する|相対誤差|のp90は0.070、50%以上ずれる画素は0.40%まで落ちる。
    // **1枚だけ見ると斑点は残る。** 消すには本数を上げるしかないが、コストは本数に
    // ほぼ線形で、2560x1440・107灯で 1本 11.0ms に対し 8本 91.8ms(RTX 4070 Ti / Release)。
    // 止め絵の真値が要るときだけ -megalightsrays を上げること
    inline constexpr int MegaLightsShadowRayCount = 1;
    // 確率的サンプリングが1ピクセルあたりに候補プールから引く数(RISのM)。
    // 大きいほど「寄与の大きい灯を選べる」確率が上がってノイズが減るが、
    // 候補ごとにBRDFを1回評価するぶん重くなる。影レイの本数はこれとは独立で常に1本。
    // 既定値の根拠はまだ実測していない ―― 段階2の誤差カーブを見てから決める
    inline constexpr int MegaLightsSampleCount = 8;
    // 空間再利用: 近傍の画素が選んだ灯を借りて自分の面で評価し直す。
    // 候補プールの重みは設計上、法線を見られない。そのため法線が候補集合と噛み合わない面では
    // 提案が外れる。それを選んだあとで埋め合わせるのがこの段。
    // 【既定は有効】初期可視レイと組にして初めて効く(片方だけでは実測でほぼ何も
    // 変わらない)。両方入れると、1画素1本の影レイの当たり外れが支配する分散を
    // 「近傍の可視な当たりを借りる」形で削れる。BistroInteriorLit の実測
    // (RTX 4070 Ti / 1920x1080 / 256フレーム蓄積 / |相対誤差|の中央値)で、
    // 再利用なし 0.0926 → 空間+時間 0.0259。コストは約1.1ms(バイアス補正レイ込み)。
    // かつて既定を無効にしていた根拠は「初期可視レイ無しの空間再利用」の測定で、
    // 効く組み合わせを測っていなかった。数値と経緯は
    // docs/ImplementationDetail.md 61.7f と docs/ImplementationHistory.md 67章
    inline constexpr bool MegaLightsSpatialEnabled = true;
    // 借りる近傍の数。増やすほどノイズは減るが、候補ごとにBRDFを1回評価するぶん重くなる
    inline constexpr int MegaLightsSpatialNeighborCount = 5;
    // 空間再利用を何回繰り返すか。2回目は1回目の出力を入力にするので、実効的な近傍は
    // k から k^2 へ広がる(近傍の近傍まで届く)。回すたびにパス1本ぶんのコストが増える。
    // 【近傍の型板は反復ごとに変える】同じ型板を2回使うと同じ近傍から借り直すだけになる。
    // 実測(1280x720 / _MLCheck / 参照実装に対する|相対誤差|の中央値):
    //   静止      1回 0.02861 → 2回 0.02437(15%改善)
    //   遮蔽解除  1回 0.06627 → 2回 0.05217(21%改善)
    // 900枚の蓄積平均は総和比 0.99965 で不偏。
    // 【時間再利用が前提】切ると2回目が未検証のサンプルを重ねて数え、+22.4% 明るくなる。
    // KurenaiEngine3D.cpp 側で「時間再利用が無ければ1回」へ落としてある。
    // 根拠は docs/ImplementationDetail.md 61.7f
    inline constexpr int MegaLightsSpatialIterations = 2;
    // 近傍を探す半径(ピクセル)。広げると遠くの良いサンプルを拾えるが、
    // 深度・法線・材質の一致条件で弾かれる割合も増える
    inline constexpr int MegaLightsSpatialRadius = 16;
    // 空間再利用の結合を不偏化(Z。Bitterli 2020 Alg.6)にするか。
    // false は confidence(M)で重み付ける単純な結合で、近傍が自分と違う候補集合から
    // 引いている可能性を無視するため不偏にならない(実測で総和の相対差 -8.0%)。
    // 使う理由が無く、切り替えは「両者の長時間平均に差が出ること」の検証用にだけ残している
    inline constexpr bool MegaLightsSpatialMIS = true;
    // 初期サンプルへ可視レイを1本撃ち、遮蔽されていたらリザーバごと殺すか。
    // 【既定は有効。空間再利用と組にする】殺しは「遮蔽で0になるサンプルを近傍へ配らない」
    // ためのもので、空間再利用が無いと実測で絵が1bitも変わらない(殺されるサンプルは
    // シェード側のレイでもどうせ0)。逆に空間再利用は殺しが無いと効かない(借りた灯が
    // 自分の位置で遮蔽されるとレイが無駄になる)。
    // 【かつて -3.6% 暗く偏った件は解消済み】原因は、殺された画素のストリームが
    // 「可視な灯しか配れない」形に変わるのに、不偏化の分母(Z)が可視性を見ずに M を
    // 数えていたこと。殺した灯の番号をリザーバへ残し、Z 側で確定情報を使い、
    // 不明な近傍にだけバイアス補正レイを撃つ形にした(MegaLightsSpatial.hlsl)。
    // 修正後は不偏(総和の相対差 -0.03%)。数値は docs/ImplementationDetail.md 61.7f
    inline constexpr bool MegaLightsInitialVisibility = true;
    // 時間再利用。前フレームのリザーバを速度ベクトルで再投影して結合する。
    // 空間再利用と違い、実効サンプル数がフレーム方向に積み上がるので収束が速くなる。
    // レイは1本も増えない(借りるのは「どの灯か」だけ)
    inline constexpr bool MegaLightsTemporalEnabled = true;
    // 履歴のM(これまでに何個の候補から絞ったか)の上限。
    // 【上げるほど良いわけではない ―― 凍結した空間ノイズと引き換えになる】
    // Mが大きいと勝者の交代率(≒8/(8+M))が下がってフレーム間は静かになるが、
    // 各画素の当選灯が数分単位で凍結し、隣どうしが別々の灯の寄与を出し続ける
    // **点描状の空間ノイズが影の縁に固定される**。逆にMが小さいと再抽選が速く、
    // 時間方向に混ざって滑らかになる ―― そのぶんのちらつきはデノイザの時間累積が吸う。
    // 実測(1920x1080 / 定常の実機、参照実装との差で16階調を超える画素の割合と、
    // 連写の隣接フレーム差で4階調を超える画素の割合):
    //   M640: 3.40% / 1.4%   M160: 1.84% / 2.1%   M64: 1.06% / 2.1%   M32: 0.81% / 2.6%
    // 64は「参照との差がほぼ底に達し、ちらつきの増分が出ない」点として選んだ。
    // 【右側(ちらつき)の数値は使わないこと】隣接フレーム差は連写の間隔に依存し、
    // 同じ構成の2回で3倍以上動く(docs/ImplementationDetail.md 61.7i)。
    // 上の 1.4〜2.6% の違いはその幅の内側にあり、何も言っていない。
    // 64を選んだ根拠として残るのは左側(参照との差)だけである。
    // 【一度640へ上げて戻した】あの判断はデノイザの時間累積が動いていない状態
    // (履歴バッファ未書き込みのバグ)での測定に基づいていた。累積が動けば
    // ちらつきは吸えるので、Mで抑える必要は無い。
    // 根拠と測り方は docs/ImplementationDetail.md 61.7b.1〜61.7b.2 と 61.7f
    inline constexpr int MegaLightsTemporalMClamp = 64;
    // デノイザ(時間累積 + エッジ停止付き à-trous)。
    // 時空間再利用が「どの灯を選ぶか」を改善するのに対し、こちらは出た色をならす。
    // **TAAの手前で落とすこと** ―― TAAはノイズを信号の広がりと解釈して履歴を棄却するので、
    // ノイズを残したまま渡すとノイズもAAも両方失う
    inline constexpr bool MegaLightsDenoiseEnabled = true;
    // a-trous の段数。段ごとにステップ幅が倍になるので、4段で半径16画素ぶんに届く。
    // 【3段が底】分散を段ごとに畳んで次段へ渡すようにするまでは、段を増やすほど
    // 誤差もエネルギー損失も悪化していた(未フィルタの時間分散を全段で使い回すため、
    // 後段ほど輝度の門番が効かずただのぼかしになっていた)。
    // 分散伝搬と空間再利用2回を入れた後の実測(1280x720 / _MLCheck /
    // **参照実装(真値)** に対する|相対誤差|の中央値, 影の縁での中央値, 総和比):
    //   1段 0.02858/0.0627/0.9958  2段 0.02509/0.0569/0.9943
    //   3段 0.02437/0.0554/0.9936  4段 0.02433/0.0553/0.9933  5段 0.02442/0.0558/0.9931
    // 3段と4段は区別できず、5段で悪化に転じる。損失の小さい3段を採る。
    // 【分母は参照実装にすること】900枚の蓄積平均を分母にすると 2段 0.03047 /
    // 3段 0.03050 と順序が入れ替わって見えるが、**900枚平均自体にまだ画素ごとの
    // ノイズが残っている**(フレーム間が相関しているので実効枚数は900より遥かに少ない)。
    // 無ノイズなのは参照実装だけ
    inline constexpr int MegaLightsDenoiseAtrousPasses = 3;
    // 時間累積の上限フレーム数。**TAAより短くすること** ―― 長いとTAAのゴーストと重なって
    // 二重に尾を引き、どちらが原因か切り分けられなくなる
    inline constexpr int MegaLightsDenoiseMaxFrames = 32;
    // クアッド共有(手法3)での時間累積の上限。**手法2より長くしてある。**
    //
    // 【なぜ手法ごとに分けるのか】手法2は時間方向の記憶を2か所に分けて持っている ――
    // リザーバの履歴(Mの上限64。当選灯の交代率を 8/(8+M) に抑える)と、
    // このデノイザの時間累積。手法3はリザーバを持ち回らないので**デノイザだけが
    // 時間方向の記憶**であり、同じ32では手法2より粗くなる。
    //
    // 実測(BistroExteriorNight / 1280x720 / RTX 4070 Ti / 連写16枚の時間std。
    // 参照実装は全16枚がビット同一で下限は厳密に0):
    //   上限 32: 中央値 0.773 / p90 2.233 / >2階調 11.92%
    //   上限 64: 中央値 0.451 / p90 1.223 / >2階調  4.30%   ← 手法2(0.448)と同水準
    //   上限128: 中央値 0.299 / p90 0.667 / >2階調  1.21%
    // a-trous の段数では代わりにならない(3段0.773→5段0.719で7%しか動かず、
    // 1段あたり約0.4ms かかる)。ちらつきは時間方向の分散なので、空間フィルタでは取れない。
    //
    // 【引き換えはゴースト ―― 測って決まる値ではない】消灯直後32フレームの残光は
    // 上限32で61.8%、64で77.8%、128で88.0%(いずれも指数移動平均の理論値と3桁一致)。
    // 残光が10%まで落ちるのは 2.30*N フレームで、60Hzなら 32→1.2秒 / 64→2.5秒 / 128→4.9秒。
    // 品質は単調に良くなりゴーストは単調に悪くなるので、**測定だけでは決まらない**。
    // 64 は「手法2と同じちらつきに並ぶ最小の値」として選んだ判断である
    inline constexpr int MegaLightsQuadDenoiseMaxFrames = 64;
    // 輝度のエッジ停止の強さ(SVGFのσ_l)。|中心-タップ| を σ・√分散 で割って exp に入れる。
    // 大きいほど広く混ぜる = ノイズは減るが本物の明暗差も混ざる。
    // 本家SVGFの慣例値は4.0。根拠は docs/ImplementationDetail.md 61.7f
    inline constexpr float MegaLightsDenoiseSigmaLuminance = 1.5f;
    // ファイアフライの近傍クランプ。時間累積へ入れる前に、5x5近傍の刈り込み平均の
    // k 倍で上側だけ頭打ちにする(0で無効)。
    // 【既定は無効 ―― 測ったが割に合わなかった】画面上で最も目につくのは白い粒なので
    // 入れてみたが、実測(1280x720 / _MLCheck)で効果がほとんど無い一方、エネルギーは
    // 確実に失う:
    //   静止      k=0: 期待値の10倍超が132画素 / 総和比0.99416
    //             k=8: 同133画素(改善なし)   / 総和比0.98967(-0.45%)
    //   遮蔽解除  k=0: 417画素 / 0.97733    k=8: 413画素 / 0.97394
    // 切れないのは、外れ値が空間的に固まっていて近傍の基準ごと押し上げるため
    //(平均+k・標準偏差では標準偏差が、刈り込み平均では平均が押し上げられる)。
    // 別のシーンで本物のファイアフライが出たときのために経路だけ残してある。
    // 根拠は docs/ImplementationDetail.md 61.7f
    inline constexpr float MegaLightsDenoiseFireflyClamp = 0.0f;

    // --- MegaLights クアッド共有(手法3) ---
    // 2x2クアッドの4画素がそれぞれ別の灯へ影レイを1本ずつ撃ち、**4本の可視性を
    // クアッド内で共有して平均する**。追加のレイは1本も撃たない。
    //
    // 【なぜ別の手法を足したのか】手法2(ReSTIR DI)は厳密な不偏性を保つために
    // 再利用のたびに可視レイと不偏化の分母のための補正レイを撃つ。実測
    // (BistroExteriorNight 107灯 / 1280x720 / RTX 4070 Ti / Release)で
    // MegaLights合計 4.26ms、うち MegaLightsSpatial が 2.64ms を占め、
    // **全灯総当たりの参照実装(4.21ms)と同じコスト**になっていた。
    // 参照実装はノイズもちらつきも無いので、その時点で手法2が勝っている軸が1つも無い。
    // UE5 の MegaLights が ReSTIR を採らなかった理由(候補ごとに可視性レイが要る)と
    // 同じ問題である。
    //
    // 【受け入れている偏り】仲間のレイの結果を借りるので、影の境界がクアッドを横切る
    // 画素で可視性が食い違う。硬い影の縁が最大1画素(対角 sqrt(2))ぼける2x2の箱フィルタ
    // 相当で、**箱フィルタは積分を保存するので総和比には出ず、影の縁の帯の
    // |相対誤差| にだけ出る**。UE の DownsampleFactor=2 と同じ種類の近似。
    // 既定を有効にしているのは、切ると1画素1標本になって手法2の再利用なし相当まで
    // ノイズが戻るため。**切り替えは陽性対照に要る**(切った状態で手法2の
    // 時間・空間再利用を外した構成と画素単位で一致することを確かめる)
    inline constexpr bool MegaLightsQuadShareEnabled = true;
    // クアッドの4画素へ候補プールのスロットを分けて引かせるか(層化)。
    // 【周辺分布は変わらない】プールのK個のスロットは混合分布からの i.i.d. 抽出なので、
    // スロットの選び方を変えても引かれる灯の分布は変わらず、Initial の割り戻しの式
    // (p = 0.25/届いた灯数 + 0.75・w/SumW)はそのまま厳密。
    // MegaLightsCommon.hlsli が禁じている「(m + phase)/M の等間隔層化」は
    // **1つのスロット列の中で層化する**場合の話で、これは該当しない。
    // 【既定はまだ測っていない】クアッドで重複した灯を引く確率が下がるので4標本の
    // 多様性が上がるはずだが、効果を測ってから決める
    inline constexpr bool MegaLightsQuadStratify = true;
    // 遮蔽が確定した灯のキャッシュ(BlockedLights)を手法3でも使うか。
    // 手法3は時間再利用パスを持たないが、キャッシュ自体は Initial が維持しており、
    // 「影の縁で支配光を毎フレーム選んでは殺される」ことによる暗黒点を防ぐ。
    // 【陽性対照では切る】履歴に依存すると手法2との画素単位の一致が崩れる
    inline constexpr bool MegaLightsBlockedCacheEnabled = true;
    // クアッド共有(手法3)が1画素あたりに引く標本の数。**影レイの本数がそのままこれになる。**
    //
    // 【1本では足りなかった】クアッド共有は2x2の4本を平均するので1画素1本でも実効4標本だが、
    // カメラが動いている間はデノイザの時間累積が効かず、生の推定量がそのまま見える。
    // ノイズの内訳は「どの灯を選ぶか」と「選んだ灯の可視性」の2つで、後者が大きい ――
    // 参照実装を影レイ1本と32本で比べると|相対誤差|のp90が0.37あった
    // (BistroExteriorNight・1280x720。docs/ImplementationDetail.md)。
    // どちらも標本数を増やす以外に減らす手が無い。
    //
    // 【UE5も1本ではない】r.MegaLights.NumSamplesPerPixel は 2 / 4 / 16 から選ぶ形で、
    // **最小でも2**である。1本という予算はこちらが最初に決めたもので、手法の限界ではない。
    //
    // 【既定値の根拠 ―― 1/2/4を掃引した】BistroExteriorNight・RTX 4070 Ti・Release。
    // ノイズはデノイザを切った静止カメラの連写16枚の時間std(中央値、階調)、
    // コストは2560x1440でのMegaLights全パスの合計:
    //
    //   N=1  std 17.95   5.589 ms
    //   N=2  std 14.54   6.071 ms
    //   N=4  std 12.57   7.517 ms   (参照実装は 11.325 ms)
    //
    // 4本でも参照実装より1.5倍速い。デノイザが4.1msを占めていて本体が小さいので、
    // 本数を上げても合計はあまり動かない ―― ここが上げどころだった。
    // 【4を超えても意味が薄い】理論どおりなら 1/sqrt(N) で 0.50 まで落ちるはずが
    // 実測は 0.70 で止まる。クアッド層化が4層であること、および
    // **1タイルの候補プールがK=32で固定**でそこから引く以上プール自体のばらつきは
    // 標本数で減らないことが効いていると考えられる(後者は未検証)
    inline constexpr int MegaLightsQuadSamplesPerPixel = 4;
    // 候補プールが1タイル(16x16画素)あたりに抽出する灯の数(K)。
    //
    // 【1画素あたりの標本数では消えないノイズがここで決まる】プールはタイルに1つで、
    // タイル内の全画素が同じK個のスロットから引く。したがってプールの引き方のばらつきは
    // タイル内で共通のオフセットとして乗り、画素あたりの標本を増やしても平均されない。
    // 静止カメラの連写16枚(デノイザ切・BistroExteriorNight・1280x720)で1フレームの誤差を
    // 分解すると、標本数を1→4にしたとき **タイル内は 18.03 → 11.33 と下がるのに
    // タイル間は 7.41 → 7.24 と2.3%しか動かない**(8bit階調・中央値)。
    // 標本数4では分散の29%がタイル間成分で、しかも16画素の格子に揃っているぶん
    // 同じ大きさの白色ノイズより目につく。
    //
    // 【この分解はウィンドウキャプチャから取っており、切り取り方に敏感】
    // 内部解像度を引き伸ばして表示したものなので、格子が半タイルずれるだけで
    // タイル間成分は3割動く。**空はMegaLightsで照らされず揺れないので上端は検出できない** ――
    // 左右と下端から幅を取り、高さは縦横比で決めること。比そのもの(何倍になったか)は
    // 切り取り方を変えても安定するが、絶対値は条件を揃えたときだけ比べられる。
    //
    // 【既定値の根拠 ―― 32/64/128を掃引した】同じ条件(標本数4)での1フレームの誤差:
    //
    //   K= 32  タイル間 7.81  タイル内 10.27  MegaLights合計 7.517 ms
    //   K= 64  タイル間 5.53  タイル内  8.77  同 7.529 ms
    //   K=128  タイル間 4.03  タイル内  8.30  同 7.613 ms
    //
    // **タイル間が半分になる(-48%)。** これは切り取り方を変えても -48〜-50% で安定する。
    // タイル内も下がる(プールが良くなるとRISの提案分布が真の寄与に近づくため)が、
    // こちらは切り取り方で -10% から -19% まで動くので、**幅を持たせて読むこと**。
    // コストは 2560x1440・107灯で候補プールのパスが 0.273 → 0.321 ms、
    // 全体で +1.3% しか増えない(RTX 4070 Ti / Release)。
    // 1画素あたりの標本数と違い、**Kはレイの本数を増やさない**のでほぼ無料である。
    //
    // 【128を上限にした理由】このシーンは107灯で、Kが灯数を超えると
    // 「届いているのにスロットに入らなかった灯」はほぼ無くなる。それ以上は
    // バッファが太るだけだと考えているが、**128より上は測っていない**
    // (バッファの確保がコンパイル時の kMegaLightsTilePoolCapacity で決まるため、
    // 振るにはビルドが要る)
    inline constexpr int MegaLightsTilePoolCapacity = 128;

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
    // 実行時に「システム」パネルから変更できる。
    // 既定はウィンドウの初期サイズ(1280x720)より大きい1920x1080で、縮小して表示される。
    // 画素数は720pの2.25倍になるため、非力なGPUではここを下げるのが最初のつまみになる
    inline constexpr uint32_t RenderWidth = 1920;
    inline constexpr uint32_t RenderHeight = 1080;

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

    // --- エミッシブ光源(自発光メッシュを光源として扱う) ---
    // 自発光面はG-Bufferへ書いて加算されるだけで周囲を照らしていない。読み込み時に
    // 自発光メッシュから「光源のかたまり」を起こし、GPULight(LightType 3)として
    // 従来のライトループにもMegaLightsにも流す。
    // 【既定で無効】既存シーンの絵を変えないため。有効にすると明るさが増える
    inline constexpr bool EmissiveLightsEnabled = false;
    // 打ち切り照度τ[表示空間]。この照度まで落ちる距離をRangeにする。
    //
    // 【根拠】windowed inverse-square が持ち込む絶対誤差は、u=d/Range とおくと
    // err(u) = τ(2u^2 - u^6) で、u^4=2/3(u=0.9036)で最大 1.089τ。
    // **τの1.09倍を決して超えない**ので、安全率を掛けずτひとつで縛れる。
    // τ=1e-3 なら、反射率0.5の拡散面が返す表示放射輝度は 0.5/π*1e-3 ≒ 1.6e-4 で、
    // トーンマップ後の8bit量子化ステップ(中間調で約3.9e-3)の1/20以下になる
    inline constexpr float EmissiveLightsCutoffIrradiance = 1e-3f;
    // 採用するプロキシ数の上限。手置きライトを押し出さないよう別枠で管理する。
    //
    // 【kMaxLights と同じ値にしてある】切り捨てはエネルギーを捨てる。EmeraldSquare の実測で、
    // 面積の大きい順に上位256個を残しても総面積の46.7%(上位1024個でも84.9%)にしかならない。
    // 併合(段C)で 3370個 → 651個 まで下げてあり、kMaxLights の枠に収まる以上、
    // ここで更に切る理由が無い。**切り捨てが起きたら、まず併合の長さ尺度を疑うこと。**
    //
    // 【残る危険はタイルの容量】1タイル64灯なので、看板が密集した街区では
    // 1タイルへ集中して欠落しうる(タイル境界の硬い縦横の継ぎ目として出る)。
    // 実際に出たらここを下げるより、そのシーンで打ち切り照度τを上げてRangeを縮めるほうが
    // エネルギーを捨てずに済む
    inline constexpr int EmissiveLightsMaxCount = 1024;
    // DDGI にも自発光を加算したままにするか(= 二重に数えるか)。
    //
    // 【既定は抑止する】プロキシを光源として直接光へ流したうえで、DDGI のプローブが
    // 同じ発光面を「明るい面」として焼き込むと、同じ発光が2回入る。
    // 抑止するのは**DDGIだけ**で、反射プローブ・RT反射・G-Bufferの自発光はそのまま。
    // 鏡面が光源を直接見ているのは二重計上ではなく、消すと光る看板が鏡に映らなくなる。
    //
    // 【つまみとして残す理由】どちらが正しいかではなく、**どれだけ二重に入っていたかを
    // 測るための対照**が要る。差分がゼロなら「抑止が効いていない」を先に疑うこと
    inline constexpr bool EmissiveLightsDoubleCountGI = false;

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
    // 機体を「周囲を照らす光源」としても送るか。
    // 既定はfalse。有効にしたシーンだけ絵が変わる(Enabledと同じ扱い)。
    // 光度はショーのBrightnessとRadiusから導くので、明るさのつまみはここには無い
    // (DroneShow::BuildLightSamplesの導出を参照)
    inline constexpr bool DroneShowCastLight = false;
    // 光源として送る灯の数。全機ぶんは送れないので間引く(理由はBuildLightSamplesのコメント)。
    // 【シーンにもショーにも持たせない】これは描画側の容量で決まる数で、ショーの表現ではない。
    // 上限はタイルライトカリングの1タイル容量(KurenaiEngine3D::kLightTileCapacity = 64)で、
    // 超えると溢れた灯が静かに欠落する。手置きライトと同居する余地を残して48にしてある。
    // 精度は1500機の厳密な逆二乗和に対し、島と水面で平均+3%(最大+9%)。
    // 灯数を倍にしても最大誤差は+7%までしか縮まらない(docs/ImplementationDetail.md 38.12)
    inline constexpr int DroneShowLightSampleCount = 48;
    // 灯の影響半径Rangeを逆算するための打ち切り照度[lx]。R = sqrt(I / この値)。
    // 満月の地表照度0.25lxの1%で、夜のキー照度(月0.25 + 夜空0.05)に対して2桁下。
    // 減衰は窓付き逆二乗なので打ち切り境界にハードエッジは出ない(LightAttenuation.hlsli)
    inline constexpr float DroneShowLightCutoffLux = 2.5e-3f;
    // 灯の明るさの倍率。**1.0がスプライトから導いた物理的な値**で、既定はそこから動かさない。
    //
    // 【なぜ倍率が要るのか】1.0だと絵として見えない。夜の島の明るさは空由来の間接光が
    // 支配していて、機体の光はその0.6%にしかならない(実測。docs/ImplementationDetail.md 38.13)。
    // 実物のドローンショーも1.3km先の山を照らしはしないので1.0が正しい振る舞いではあるが、
    // それでは「機体が周囲を照らす」という機能が絵に出ない。
    //
    // 【1.0を既定に残す理由】ここを大きい値にすると、物理的な値がどれだったのかが
    // 分からなくなる。**演出として上げたいシーンが[DroneShow]CastLightScaleで明示的に上げる** ――
    // 実際、唯一のサンプルシーン(Scenes/DroneShow.kscene)は8.0を指定している。
    // 既定とサンプルが食い違って見えるのは意図したもので、「エンジンの既定は物理的な値、
    // 絵作りはシーンの責任」という分担をそのまま表している
    inline constexpr float DroneShowCastLightScale = 1.0f;

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
    // Hi-Zを深度プリパスの深度から作るか。
    //
    // 【これが有効だとG-Bufferの判定に1フレーム遅れが無くなる】プリパスが書いた
    // 今フレームの深度から作るため、投影に前フレームの行列を使う必要も、
    // 視差ぶんを膨らませる必要も無くなり、カメラが動いてもポップしない。
    // 深度プリパスが走らないフレームでは、この値によらず従来どおり
    // G-Bufferの後で作り、次フレームに前フレームのものとして読む
    inline constexpr bool HiZFromDepthPrepass = true;
    // カリング結果で実際に描画発行(ExecuteIndirect)まで行うか。
    //
    // 【切っても判定と計数は動く】falseなら描くのは従来のCPUループのままで、
    // GPU側の判定はカウンタに残る。「判定が正しいか」と「間接描画が速いか」は
    // 別々に確かめたいので、トグルを分けてある。
    // DX11とメッシュシェーダー非対応環境では、この値によらず従来のCPUループへ縮退する
    inline constexpr bool ModelCullIndirectEnabled = true;

    // --- メッシュレットLOD(離散LOD。Stage 6) ---------------------------------------------
    //
    // 増幅シェーダーがモデルのバウンディング球の投影サイズから段を1つ選ぶ。
    // KurenaiPackerが焼いた段が無いモデル(段が1つだけ)では何も起きない
    inline constexpr bool MeshletLODEnabled = true;
    // しきい値の倍率。段を落とす投影直径は
    // MeshletLODQuality * sqrt(4 * LOD0の三角形数 / π) [画素]。
    //
    // 【1.0の根拠】倍率1.0は「原寸の三角形の平均面積が1画素を切ったところで1段落とす」
    // にちょうど対応する。そこから先は、原寸を保っても画面に出せる情報が増えない。
    // 大きくすると原寸を長く保ち(安全側)、小さくすると早く粗くする
    inline constexpr float MeshletLODQuality = 1.0f;
    // 段を固定する番号。負なら自動選択。
    // 【対照実験用】自動のまま数値が動かないとき、「段の選択が効いていない」のか
    // 「効いた上で変わらない」のかは、段を固定して初めて切り分けられる
    inline constexpr int32_t MeshletLODForcedLevel = -1;
}
