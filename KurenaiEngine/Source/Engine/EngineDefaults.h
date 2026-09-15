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
    // SpecularOcclusionMode: スペキュラ遮蔽の方式。Kurenai::SpecularOcclusionMode と
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
    // Kurenai::SpecularCompensationMode と HLSL の KURENAI_SPEC_COMP_* に対応する
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
    // これらの値をリテラル複製した既定値で初期化され、そちらがKurenaiEngine3D::m_Settings.Water.WaveScale等を
    // 上書きする(Scene.hのコメント参照)。ここの値自体は.kscene側のコメントに残した暫定値の流用
    inline constexpr float WaterWaveScale = 12.0f;
    inline constexpr float WaterWaveSpeed = 0.03f;
    inline constexpr float WaterWaveStrength = 0.25f;
    // シーンに依存しないUIつまみ(Scene::WaterWaveScale等と違い.ksceneのキーを持たない)
    inline constexpr bool WaterTimeFrozen = false;
    // 水面のSSR反射で、レイが画面外へ抜けた・最大距離まで判定がつかなかったときに解析空
    // (Sky.hlsli)へフォールバックするか(SSRの水面分岐)。既定ON。
    // **現状の効果は小さい**が、解析評価のほうが情報を落とさない上位互換なのでONにする。
    // 手続き空が無効なシーンでは実際には効かない。実測は docs/ImplementationDetail.md 65.2
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
    // 物理的な導出ではない。
    // 【この目盛りは線形ではない】被覆率Cはシェーダー側で remap(fBm, 1-C, 1) の
    // しきい値として使われ、fBmは[0,1]に一様ではない。Cと「実際に雲になる空の割合」は
    // 大きく食い違う。0.4〜0.5の狭い範囲に見た目が集中しているので、
    // ここを動かすときは小刻みに動かすこと。換算表は docs/ImplementationDetail.md 35.10.3
    inline constexpr float CloudCoverage = 0.40f;
    // 雲底の高度[m]。積雲の雲底高度として一般に言われる目安(だいたい1,000〜2,000m)の
    // 中間を採った値(精密な気象観測値ではなく目安からの採用)
    inline constexpr float CloudAltitude = 1500.0f;
    // ノイズ空間のUVスケール[ノイズ空間の距離/m]。1セル=1,000m。
    //
    // 【1セルの大きさではなく「画角に何セル入るか」で決める】「積雲1個がおよそ2km」
    // だから1/2000、という決め方をしてはいけない。効くのは雲そのものの寸法ではなく、
    // 画角の中に雲と隙間が何回交代して現れるかである。1セル2kmでは画面上端に
    // 1.5セルしか入らず、雲か隙間かがノイズの引きだけで決まる。
    // 【動きが遅いので「引き」がそのまま体感になる】風速の既定5m/sではノイズ空間は
    // 毎秒0.005セルしか進まない(1セル進むのに3分以上)。実用上ほぼ常に
    // 起動時の引きだけを見ることになる。
    // 導出と実測は docs/ImplementationDetail.md 35.10
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
    // 雲の種類の偏り(C4)。0=層雲寄り / 0.5=中立 / 1=雄大積雲寄り。
    // 場所ごとの縦プロファイル(層雲・積雲・雄大積雲)の選択を空全体でどちらへ寄せるかを決める。
    // 0.5は「値ノイズの平均が0.5なので、そのまま3種が均等に散らばる」という中立点
    inline constexpr float CloudTypeBias = 0.5f;
    // シーンに依存しないUIつまみ(m_Settings.Water.TimeFrozenと同じ位置づけ)。
    // 積雲・巻雲の両方に効く(片方だけ凍結できるとA/B比較の対照が取れなくなるため)
    inline constexpr bool CloudTimeFrozen = false;
    // 積雲のボリュームレイマーチの段数(1〜32)。上限の意味は
    // KurenaiEngine3D::kCloudRaymarchStepsMax のコメントを見ること。雲パスのコストの主なつまみで、
    // 1画素あたりのウェザーマップ評価18回(視線 + 自己影5 + 基底1)の大半がこのループになる
    inline constexpr uint32_t CloudRaymarchSteps = 12;

    // --- 巻雲(積雲の上に重ねる2層目) ---
    //
    // 【この5つの値の決め方】いずれも見た目からの調整値で物理的な導出ではない。
    // 有効/無効の切り替えで空領域の画素が動くことを実測で確かめてある。
    // **弱い側へまとめて振ると巻雲がほぼ見えなくなる**(効くのは主にUVスケール。
    // 実測は docs/ImplementationDetail.md 35.10.4)
    inline constexpr bool CirrusEnabled = true;
    // 被覆率。remap(fBm, 1-被覆率, 1)で塊に整形するため、この値が低いとfBmの上位だけが
    // 残ってまばらな筋になる。積雲と同程度にして空の広い範囲へ薄く掛ける
    inline constexpr float CirrusCoverage = 0.5f;
    // 雲底の高度[m]。巻雲の高度帯として一般に言われる目安は5,000〜13,000mで、その中ほどを
    // 採った値(精密な気象観測値ではなく目安からの採用。積雲のCloudAltitudeと同じ性格)
    inline constexpr float CirrusAltitude = 8000.0f;
    // ノイズ空間のUVスケール[ノイズ空間の距離/m]。1セル=2,000m。
    // 積雲(1セル1,000m)と値が違ってよい理由は docs/ImplementationDetail.md 35.10.4。
    // **1/6000のような大きなセルにしてはいけない** ―― 画角内に1〜2セルしか入らず、
    // 模様が大きすぎて筋として読めない
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
    // (親セッション側の実機確認で調整可能。KurenaiEngine3D::m_Settings.Fog.Density等のコメント参照)
    inline constexpr bool FogEnabled = true;
    // 基準高度(FogRefHeight)での消散係数[1/m]。
    // 気象学的視程Vと Koschmieder の関係 sigma = 3.912 / V で結び付く。
    // 0.0004 は V ≒ 10km 相当。動かすときは「どのくらいの視程を想定するか」で
    // 決めるのが分かりやすい(根拠は docs/ImplementationDetail.md 37章)
    inline constexpr float FogDensity = 0.0004f;
    // スケールハイト[m]。大きいほど霞が高くまで及ぶ。エアロゾルのスケールハイトとして
    // 一般に言われる目安(おおむね1〜1.5km)の下端を採った(精密な観測値ではなく目安からの採用)。
    // このシーンは地物が最も高い尖塔でも165mしかないため、この値の違いは絵にほとんど出ない
    inline constexpr float FogScaleHeight = 1000.0f;
    // 基準高度[m](ワールドY)。水面の高さに合わせている
    inline constexpr float FogRefHeight = 0.0f;
    inline constexpr float FogMaxOpacity = 1.0f;
    // 水体の色(リニア)。水中で拡散的に後方散乱して戻ってくる光の粗い近似で、
    // 見下ろしたときに水面が何色に見えるかをほぼ一手に決める(見下ろす角度では
    // Fresnelが約0.03まで下がるため、見えているもののほぼ全部がこの色になる)。
    // **B÷R比を1へ近づけると灰色になる。**
    // 値の決め方は docs/ImplementationDetail.md 65.1(物理量の実測ではなく見た目からの調整値)
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
    // 【1本では1灯あたり1標本しかない】可視率は0か1に振れ、半影ではなく黒い斑点
    // として出る。真値は蓄積(-megalightsrays / -megalightsaccum)で寄せる前提で、
    // **1枚だけ見ると斑点は残る**。コストは本数にほぼ線形。
    // 実測と、なぜ「答えが変わらない」ではないのかは docs/ImplementationDetail.md 61.7f
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
    // 【時間再利用が前提】切ると2回目が未検証のサンプルを重ねて数えて明るくなるため、
    // KurenaiEngine3D.cpp 側で「時間再利用が無ければ1回」へ落としてある。
    // 実測は docs/ImplementationDetail.md 61.7f
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
    // 【上げるほど良いわけではない】Mが大きいと勝者の交代率(≒8/(8+M))が下がって
    // フレーム間は静かになるが、各画素の当選灯が凍結し、**点描状の空間ノイズが
    // 影の縁に固定される**。64は「参照との差がほぼ底に達する」点として選んだ。
    // 【ちらつきの数値を根拠にしないこと】隣接フレーム差は連写の間隔に依存し、
    // 同じ構成の2回で3倍以上動く(docs/ImplementationDetail.md 61.7i)。
    // 掃引と、一度640へ上げて戻した経緯は docs/ImplementationDetail.md 61.7b.1〜61.7b.2 と 61.7f
    inline constexpr int MegaLightsTemporalMClamp = 64;
    // デノイザ(時間累積 + エッジ停止付き à-trous)。
    // 時空間再利用が「どの灯を選ぶか」を改善するのに対し、こちらは出た色をならす。
    // **TAAの手前で落とすこと** ―― TAAはノイズを信号の広がりと解釈して履歴を棄却するので、
    // ノイズを残したまま渡すとノイズもAAも両方失う
    inline constexpr bool MegaLightsDenoiseEnabled = true;
    // a-trous の段数。段ごとにステップ幅が倍になるので、4段で半径16画素ぶんに届く。
    // 【3段が底】3段と4段は区別できず、5段で悪化に転じる。損失の小さい3段を採る。
    // 【分母は参照実装にすること】900枚の蓄積平均を分母にすると順序が入れ替わって
    // 見えるが、**900枚平均自体にまだ画素ごとのノイズが残っている**。
    // 掃引の実測は docs/ImplementationDetail.md 61.7g.2
    inline constexpr int MegaLightsDenoiseAtrousPasses = 3;
    // 時間累積の上限フレーム数。**TAAより短くすること** ―― 長いとTAAのゴーストと重なって
    // 二重に尾を引き、どちらが原因か切り分けられなくなる
    inline constexpr int MegaLightsDenoiseMaxFrames = 32;
    // クアッド共有(手法3)での時間累積の上限。**手法2より長くしてある。**
    //
    // 【なぜ手法ごとに分けるのか】手法2は時間方向の記憶をリザーバの履歴と
    // デノイザの時間累積の2か所に分けて持つが、手法3はリザーバを持ち回らないので
    // **デノイザだけが時間方向の記憶**であり、同じ32では手法2より粗くなる。
    // a-trous の段数では代わりにならない(ちらつきは時間方向の分散のため)。
    // 引き換えはゴーストで、品質は単調に良くなりゴーストは単調に悪くなる ――
    // **測定だけでは決まらない値**である。64 は「手法2と同じちらつきに並ぶ最小の値」。
    // 掃引と残光の数値は docs/ImplementationDetail.md 61.7j.6
    inline constexpr int MegaLightsQuadDenoiseMaxFrames = 64;
    // 輝度のエッジ停止の強さ(SVGFのσ_l)。|中心-タップ| を σ・√分散 で割って exp に入れる。
    // 大きいほど広く混ぜる = ノイズは減るが本物の明暗差も混ざる。
    // 本家SVGFの慣例値は4.0。根拠は docs/ImplementationDetail.md 61.7f
    inline constexpr float MegaLightsDenoiseSigmaLuminance = 1.5f;
    // ファイアフライの近傍クランプ。時間累積へ入れる前に、5x5近傍の刈り込み平均の
    // k 倍で上側だけ頭打ちにする(0で無効)。
    // 【既定は無効 ―― 測ったが割に合わなかった】外れ値が空間的に固まっていて
    // 近傍の基準ごと押し上げるため切れず、エネルギーだけを失う。
    // 別のシーンで本物のファイアフライが出たときのために経路だけ残してある。
    // 実測は docs/ImplementationDetail.md 61.7g.4
    inline constexpr float MegaLightsDenoiseFireflyClamp = 0.0f;
    // デノイザの時間累積が履歴の色を引くときの再サンプリング。
    // false = バイリニア(従来) / true = Catmull-Rom。
    //
    // 【なぜ切り替えを足したか】バイリニアで引くと毎フレーム「補間した結果をまた補間する」
    // ことになり、**移動中の鮮鋭さが累積的に失われる**。同じリポジトリの TAA は
    // まさにこの理由で Catmull-Rom を使っており、TAA.hlsl にそう明記されている。
    // デノイザ側だけがバイリニアのままだった。
    //
    // 実測(BistroExteriorNight / 決定的カメラ経路 Strafe / 2560x1440 / DX12 / Release、
    // 分母は同じ経路の参照実装 rays=64。S = 候補自身の高周波エネルギー ÷ 真値のそれ):
    //   a-trous 0段に固定して累積上限だけを振ると
    //     上限2  S=0.987 / 上限4  S=0.744 / 上限16 S=0.553 / 上限64 S=0.516
    //   a-trous の段数では S はほとんど動かない(0段 0.516 → 5段 0.481)ので、
    //   なまりを作っているのは空間フィルタではなく履歴の再サンプリング。
    //
    // 【一度落として、原因を見つけて戻した】最初の実測では S が +19〜24% 上がる代わりに
    // 総和比が 0.927 → 0.910 と暗くなり、誤差が正の画素が 49% → 30% へ偏った。原因は
    // 5タップ化で落とした角4タップのぶん重みの和が 1 にならず(f=0.5 で 0.984)、上限64の
    // 帰還ループで損失が育っていたこと。重みの和で割ったあとの実測(同じ経路):
    //            総和比   |相対誤差|中央   p90      S      誤差>0
    //   バイリニア  0.9273   0.0600        0.6513   0.480   49.2%
    //   Catmull-Rom 0.9375   0.0514        0.5483   0.579   49.3%
    //   4タップ + CR   0.9492   0.0473        0.4737   0.689   48.7%
    // **すべての指標でバイリニアを上回り、符号の偏りも消えた。** 無効時の出力は変更前と
    // ビット同一。4タップ判定とは「4タップが全部通ったときだけ Catmull-Rom」の形で併用でき、
    // 両方 ON が全指標で最良(S は既定の 0.480 → 0.689)。
    //
    // **既定は目視のあとに決める。** 数値と経緯は docs/ImplementationDetail.md 61.7q
    inline constexpr bool MegaLightsDenoiseHistoryCatmullRom = false;
    // デノイザの時間累積が履歴の妥当性を何タップで判定するか。
    // false = 最近傍1タップ(従来) / true = バイリニア2x2の4タップ。
    //
    // 【なぜ切り替えを足したか】履歴の**色**は4タップ混ぜているのに、その4タップが
    // 妥当かどうかを1点でしか見ていなかった。1点だけがシルエットの向こう側だと
    // 履歴全体を棄却し(本当は妥当なのに捨てる)、逆に1点が通れば別の面の色が
    // 3/4の重みで入る。時間再利用(MegaLightsTemporal)は元から2x2を走査しており、
    // デノイザだけが片肺だった。
    //
    // 【既定は false のまま。実測では良くなるが、目で見て確かめられなかった】
    //
    // 下表のとおり指標はすべて改善する。にもかかわらず既定にしていないのは、
    // **実際に動かして見比べても差が分からなかった**ため。
    // これは測定と矛盾しない ―― 効くのは履歴が棄却された画素だけで、
    // それは画面の 4.8% から 2.6% へという範囲であり、9割以上の画素は元から変わらない。
    // 「指標が動いたこと」と「見て分かること」は別で、既定を決めるのは後者である。
    //
    // 有効にするには `-megalightsdenoise4tap 1`。実測は下表
    // BistroExteriorNight / 決定的カメラ経路 Strafe / 2560x1440 / DX12 / Release / 16フレーム。
    // 分母は同じ経路を参照実装(1灯64本の影レイ)で走らせた1フレームごとの真値。
    // S は候補自身の高周波エネルギー ÷ 真値のそれ(1が真値どおり、1未満はなまっている):
    //
    //            棄却率    |相対誤差|中央  p90      S       総和比   誤差>0
    //   1タップ  4.843%   0.05827      0.5698   0.4800  0.9269  48.6%
    //   4タップ  2.627%   0.04767      0.4579   0.6132  0.9385  48.0%
    //
    // **すべて同じ向きに改善し、符号のバランス(誤差>0)は動かない** ―― つまり
    // 新しい系統誤差を入れずに、ノイズと鮮鋭さの両方が良くなっている。
    // 棄却率は決定的な経路の上では決定的に出る(ノイズ下限が無い)ので、主指標はそれ。
    //
    // コストは正規化 MegaLights 合計で 1タップ 3.1252 / 3.1526(同一構成2回)に対し
    // 4タップ 3.1233。**対照2回だけで 0.88% 開いており、差はそのばらつきに埋もれている。**
    // 「ほぼ0」とは書けるが「速くなった」と読んではいけない。
    //
    // 経緯と検算は docs/ImplementationDetail.md 61.7o
    inline constexpr bool MegaLightsDenoiseHistory4Tap = false;
    // デノイザの時間累積に、残差駆動のアンチラグを掛けるか。
    //
    // 【何を直すのか】従来の累積は「その画素の信号が変化したか」を一切見ていない。
    // だから遅れ(灯を消したあとの残光)は上限が全画素へ一律に決めており、
    // 上限64なら (1-1/64)^t で尾を引いて10%まで2.5秒かかる(61.7j.6、理論値と3桁一致)。
    // 「現フレームの7x7平均を数フレームならした値」と「履歴の7x7平均」の相対変化で変化を
    // 検出して、変化した画素だけ上限を4まで落とす。静穏な画素は長く累積したまま ――
    // つまり**上限を伸ばしてノイズを下げても遅れが増えない**形にする。
    // アンチラグ単独でも上限の引き上げ単独でも意味が無く、対で入れて初めて成立する。
    //
    // 【2つの設計を測って落とした】MegaLights の生標本は裾が重く右に歪んでいる
    // (相対 std が画素ごとに 0.3〜1.0、中央値 < 平均)。
    //   (1) 平均の差を時間stdで正規化 … σを振っても「静止 <0.1% / 消灯 >90%」を両立しない
    //       (σ=0.25: 29.2%/97.8%、1.0: 0.98%/73.6%、2.0: 0.086%/34.2%)
    //   (2) タップごとの符号検定 … 歪みで静止でも約75%のタップが「生 < 平均」になり 30.8% 誤発火
    //   (3) 平均どうしの相対変化 … 消灯は通るが静止で 3.0% が毎フレーム発火。現フレーム側の
    //       49タップを中心画素の復調係数で割っていたため、アルベドの縁で履歴(画素ごとに復調済み)と
    //       恒常的にずれていた。タップごとに復調して 0.24〜0.28% へ(MegaLightsDenoise.hlsl の AntiLagCap)
    //
    // 【実測(61.7p、BistroExteriorNight / 手法3 / 2560x1440 / 上限64)】
    //   消灯の残光が 10% まで: OFF 約147フレーム → ON 約14フレーム(EMA 長4のぶん2フレーム遅れて落ちる)
    //   静止の画素時間std: OFF/64 0.524 / ON/64 0.535 / 同じ遅れの OFF/8 4.06
    //   静止900フレームの総和比 ON/OFF 0.9994、Strafe 移動中の N1 は3構成とも 4.3〜4.5 で差なし
    //   コスト: 時間累積パスが約2倍(+1.2)、全体 +11%(49タップ×4テクスチャを毎フレーム読む)
    //
    // 【効かないもの】カメラ移動中のなまりには効かない。移動中の誤差の主成分は遅れではなく
    // なまりで(61.7o.6)、再投影が効いている面では残差が出ず発火しない。
    // 効くのは灯や影が変わったときの残像。
    //
    // 【既定は無効。目視で決めるまで変えない。ON にするなら累積上限の引き上げと対で】
    // T0/T1 は相対変化 |fast − hist| / max(fast, hist) に対する smoothstep の両端
    // (消灯なら EMA の長さぶん遅れて 1.0 へ向かう)。FastFrames は短い EMA の長さで、
    // ファイアフライ1個で動く単フレームの7x7平均をならす。長いほど誤発火は減り、検出は遅れる
    // (4/8/16 で静止の誤発火は 3.0/2.9/2.9% と動かなかった ―― 上の (3) の切り分けの根拠)
    inline constexpr bool MegaLightsDenoiseAntiLag = false;
    inline constexpr float MegaLightsDenoiseAntiLagT0 = 0.35f;
    inline constexpr float MegaLightsDenoiseAntiLagT1 = 0.6f;
    inline constexpr int MegaLightsDenoiseAntiLagFastFrames = 4;

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
    // 1本では足りない ―― クアッド共有は2x2の4本を平均するので実効4標本だが、
    // カメラが動いている間はデノイザの時間累積が効かず、生の推定量がそのまま見える。
    // 1/2/4の掃引と、4を超えても効きが鈍る理由は docs/ImplementationDetail.md 61.7l
    inline constexpr int MegaLightsQuadSamplesPerPixel = 4;
    // デノイザに棄却される画素へ追加するブースト標本数と、その対象を選ぶモード。
    // 1=デノイザの予測棄却画素、2=それに加えて前フレームの履歴長がしきい値未満、
    // 3=全画素(検算専用)。
    // 【既定は無効。効き代の実測は docs 61.7q.1(棄却画素は最悪画素の 7%)】
    // 実測(61.7q.5、BistroExteriorNight / Strafe / 2560x1440、分母は参照実装):
    //   棄却画素の N1 中央値  B=0 14.98 / B=4 13.68 / B=8 13.32 (4タップ判定 ON: 11.20 / 10.08)
    //   全画素の N1 中央値    B=0 4.46  / B=4 4.42  / B=8 4.40
    //   Initial パス          B=0 2.88〜2.91 / B=4 3.28〜3.43 / B=8 3.82 [ms]
    //   不偏性: mode 3 の静止 900 フレーム総和比 1.0004、Strafe の生出力の総和比 0.9997
    //   予測ゲートとデノイザの実際の棄却の一致率 100.00%(16 フレーム、食い違い ≤ 6 画素/フレーム)
    inline constexpr int MegaLightsQuadBoostSamples = 0;
    inline constexpr int MegaLightsQuadBoostMode = 1;
    // 候補プールが1タイル(16x16画素)あたりに抽出する灯の数(K)。
    //
    // 【1画素あたりの標本数では消えないノイズがここで決まる】プールはタイルに1つで、
    // タイル内の全画素が同じK個のスロットから引く。プールの引き方のばらつきは
    // タイル内で共通のオフセットとして乗り、画素あたりの標本を増やしても平均されない。
    // **Kはレイの本数を増やさない**のでコストはほぼ無料。
    // 掃引の実測と、128より上を測っていない理由は docs/ImplementationDetail.md 61.7m
    inline constexpr int MegaLightsTilePoolCapacity = 128;
    // 候補プールの16x16格子をフレームごとにずらし、タイル内で共通する抽出誤差を
    // 時間累積後の同じ画面位置へ固定しない。1フレームのノイズ量を減らす機能ではない。
    // 無効時は従来のタイル添字・乱数の種・ディスパッチ数を保つため既定は無効
    inline constexpr bool MegaLightsTileJitterEnabled = false;
    // 候補プールの参照を、自分のタイル固定から「最も近い4タイルの確率的バイリニア参照」へ
    // 変えるか。0=自分のタイル固定(従来)、1=2x2クアッドごとに1タイル、2=画素ごとに1タイル。
    //
    // 【何を直すためのものか】プールの抽選はタイルごとで、当たり外れがそのタイルの256画素に
    // 共有される。これがデノイズ後まで残る16x16のタイル形のムラの正体で、タイル内の画素が
    // 同じ誤差を共有している以上、**空間フィルタでは原理的に取れない**(実測はデノイズ後の
    // タイル誤差の時間相関0.985、a-trousでタイルを跨げる重みは幾何的上限0.859に対し0.020)。
    // 格子ジッター(MegaLightsTileJitterEnabled)は境界の位置を動かすだけで、
    // 「1画素は1タイルに属する」割り当てが残るため効かない ―― これも実測済み。
    //
    // 【既定を1(クアッドごと)にしてある根拠】隣接タイル段差が約31%下がり、タイル内の
    // ばらつき・なまり・総和比は悪化しない(ぼかして稼いだのではない)。タイル格子の
    // オフセットを全256通りに振った最大値でも同じだけ下がるので、物差しの位相のずれではない。
    // 長時間蓄積では参照実装に対する総和比が無効時と 0.01% 未満しか違わず、不偏性も保たれる。
    // 数値と測定条件は docs/ImplementationDetail.md 61.7t。
    //
    // 【2(画素ごと)にしない理由】指標は1と有意に違わないのに、クアッド層化
    // (MegaLightsQuadStratify)はクアッドの4画素が同じプールを引く前提で組まれている。
    // 画素ごとに選ぶと層化がクアッドを跨いで壊れる。同じ効果なら壊れないほうを採る。
    //
    // 【コストは上がる】4タイルぶんのヘッダ読み出しと w_j(y) の再計算が候補ごとに増え、
    // MegaLightsInitial が重くなる。**近似で軽くしてはいけない** ―― 提案分布を歪めて
    // 静かにバイアスを入れる。釣り合いを取るなら候補数 M(MegaLightsSampleCount)を
    // 下げる方向で取ること(61.7t.8)
    inline constexpr int MegaLightsTilePoolBilinearMode = 1;

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

    // --- メッシュライト(段階2: 発光面を三角形のまま積分する) ---
    // 段階1のプロキシが発光クラスタを重心1点へ潰すのに対し、こちらは同じクラスタを
    // 三角形の束のまま面積分する。遠方では両者は一致しなければならず、それが検証になる。
    //
    // 【MegaLights 経路だけが切り替わる】プロキシは m_SceneGPUResources.LightBuffer に積んだままで、
    // MegaLights の参照実装と候補プールだけが型3を読み飛ばす。DDGI・反射プローブ・
    // RT反射・半透明・平面反射は面光源を扱えないのでプロキシが要る ―― 消すと
    // それらから発光体の照明だけが消え、しかもそれらしく見える。
    // DX11 / 非DXR は ShouldRunMegaLights() が偽なので自動的にプロキシへ落ちる。
    //
    // 【既定で無効】まだ参照実装(全三角形総当たり)しか無く、実シーンでは回らない
    inline constexpr bool MeshLightsEnabled = false;

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
    // 上限はタイルライトカリングの1タイル容量(Passes::kLightTileCapacity = 64)で、
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
