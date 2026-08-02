import os
import struct

import numpy as np

# 物理ベースのHDRスカイボックス生成(太陽本体は描かず、青空のグラデーションのみ)。以前は
# 地平線色→天頂色を単純に補間するだけのLDR(R8G8B8A8_UNorm、[0,1]にクランプ済み)キューブマップ
# だったため、IBLのプリフィルタ済み鏡面(M3、docs/Architecture.html 14章)が畳み込む入力に
# 十分なダイナミックレンジが無かった。
#
# 空の輝度分布はPerez et al., "All-Weather Model for Sky Luminance Distribution"(1993)/
# Preetham, Shirley, Smits, "A Practical Analytic Model for Daylight"(SIGGRAPH 1999)の
# CIE快晴空係数(a=-1, b=-0.32, c=10, d=-3, e=0.45)を使う。太陽の方向(sun_direction)自体は、
# この係数が表す「太陽に近い方向ほど散乱で明るくなる(circumsolar)」という空自体の輝度分布の
# 形を決めるためだけに使い、太陽本体の可視円盤は描かない(ユーザー指示により削除。以前は
# 実際の角直径から求めた円盤をエネルギー保存しつつ焼いていたが、可視の太陽自体が不要になった
# ため、その処理一式ごと削除した)。
#
# 絶対輝度のスケールはKurenaiEngine3D.cpp(ComputeSunLighting)と同じ出典(Lagarde & de Rousiers
# 2014の照度参照テーブル、空光20,000lx)にComputeExposureと同じ露出式を適用して求めており、
# 太陽本体の平行光・ポイント/スポットライトと同じHDRスケールに揃えている。
#
# 太陽・天頂角の計算に使う時刻・方位角はKurenaiEngine3D.cpp ComputeSunLightingと同じ式・同じ
# 既定値(Time of Day=12時, Sun Azimuth=126.87度)を使っているが、このスカイボックス自体は
# 起動時に一度だけ焼くオフラインアセットのため、実行中にImGuiの太陽スライダーを動かしても
# 空の輝度分布の形自体は追従しない(既知の制約。IBL側の「夜間はAmbientColor.aで全体を
# 減光する近似」と同じ性質)
#
# 計算は512x512x6面ぶんをnumpyで一括ベクトル化している(Pythonの素朴なピクセルループでは
# 数秒〜十秒程度かかっていたため)。半精度浮動小数点への変換もnumpyのfloat16キャストに
# まかせる(自前のビット演算より検証済みで確実)
#
# 【雲(P5)は意図的に実装していない】Shaders/3D/Sky.hlsliへ積雲1層のレイヤーモデルを追加したが、
# このスクリプトへは移植していない。理由は2つ:
#   (1) このスクリプトは「手続き空を無効にしたとき」のフォールバック用オフライン参照実装であり、
#       手続き空自体が無効な場面で雲だけ動くのは前提が矛盾する
#   (2) 手続き空側もIBL用キューブマップには雲を焼き込まない設計にした(判断A。Sky.hlsli
#       雲セクションのコメント参照)。SkyGenerate.hlslは雲を無効(CloudCoverage=0)にして
#       Sky.hlsliのSkyColorを呼ぶため、キューブマップの中身自体はこのスクリプトが焼くDDSと
#       同じく雲の無い晴天のまま。したがって「Sky.hlsliと同期すべき対象」から雲だけは外れる
# 以降、このファイルは従来どおりPerez分布(青空のグラデーションのみ)の移植を維持すればよい
#
# 【P7: 日中の色をPreetham xyYモデルへ置き換え】Shaders/3D/Sky.hlsliのSkyColorUpperUnitと
# 同じ変更をこのスクリプトへも反映する。日中(太陽仰角5度以上)の色度(x, y)はPreetham et al. 1999の
# xyYモデルから求め、輝度(Y)はPerez比+RELATIVE_LUMINANCE_FLOORのままタービディティ依存の係数へ
# 差し替える。夜・薄明(太陽仰角5度未満)は引き続き従来のティント補間(compute_sky_tint/sky_tint)を
# 使い、その間をクロスフェードする(sky_color_upper_unit参照)。

FACE_SIZE = 512
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
# Sample3DのPostBuildEventがAssets\Packed\の中身をそのままOutDir\Assets\へコピーするため、
# ここ(リポジトリ側のAssets\Packed\Skybox\)に置いたファイルが実行時はOutDir\Assets\Skybox\Sky.dds
# (KurenaiEngine3D.cpp: dataRoot + "Assets\\Skybox\\Sky.dds")として読み込まれる。
# スカイボックスはKurenaiPacker(.ktex変換)を経由しないため、Packed\配下に直接DDSを置く
OUT_DIR = os.path.join(SCRIPT_DIR, "..", "Assets", "Packed", "Skybox")
OUT_PATH = os.path.join(OUT_DIR, "Sky.dds")

# --- KurenaiEngine3D.cpp ComputeSunLighting/ComputeExposureと出典・数値を揃える ---
SKY_ILLUMINANCE_LUX = 20000.0    # 空光(曇天相当値、直射日光に対する比率としても妥当)。同テーブル
DEFAULT_EV100 = 15.0             # KurenaiEngine3D::m_SceneExposureEV100の既定値
DEFAULT_TIME_OF_DAY_HOURS = 12.0
DEFAULT_SUN_AZIMUTH_DEGREES = 126.87

# 空の色味。物理的な分光計算(Rayleigh散乱の波長依存性を積分するなど)はせず、Perez分布が
# 与える輝度の大きさ(スケール)はそのままに、色味だけを太陽高度で補間する簡略化
# (アート的な近似であることを明記する)。
# 実際の快晴の空は天頂から中程度の高度まで彩度の高い青を保ち、本当の水平線ぎわ(最後の
# 20〜30度程度)でようやく白っぽくなる。天頂→水平線を単純に線形補間すると、ゲームカメラが
# 見る典型的な低めの仰角(建物越しに覗く空など)でもすでに大きく白側へ寄ってしまい、
# 「青空に見えない」結果になる(実際に一度この問題が起きた)。そのためTINT自体を水平線側でも
# はっきり青みが残る値にし、かつ後述のブレンド係数も水平線ぎわに寄せてある。
#
# KurenaiEngine3D.cpp の ComputeSkyTint と同じ値・同じ補間であること。
# 一方だけ変えるとオフラインで焼いたDDSと手続き空の色が食い違う
DAY_ZENITH_TINT = np.array([0.22, 0.45, 1.0])
DAY_HORIZON_TINT = np.array([0.55, 0.74, 1.0])
DAY_GROUND_TINT = np.array([0.10, 0.09, 0.08])
# 薄明(太陽仰角0度)。天頂は青を残したまま暗く、水平線は夕焼けの橙へ
DUSK_ZENITH_TINT = np.array([0.13, 0.22, 0.60])
DUSK_HORIZON_TINT = np.array([0.95, 0.50, 0.28])
DUSK_GROUND_TINT = np.array([0.06, 0.05, 0.05])
# 夜(太陽仰角-15度以下)。月光は分光的にはほぼ太陽光そのもので、夜空が青く見えるのは
# 暗所視のプルキンエ現象による知覚的なもの。青へ寄せるのは正しいが寄せすぎると
# ネオンブルーになるため、昼空と同程度の彩度に留める
NIGHT_ZENITH_TINT = np.array([0.09, 0.15, 0.40])
NIGHT_HORIZON_TINT = np.array([0.16, 0.24, 0.50])
NIGHT_GROUND_TINT = np.array([0.02, 0.02, 0.03])
# 太陽方向に乗せる夕焼け・朝焼けの暖色
SUN_GLOW_TINT = np.array([1.0, 0.38, 0.12])

# 地平線よりさらに下(地面方向)は空のモデルの適用範囲外のため、水平線のプラトー色から
# この暗い接地色へフェードさせる(実際の地面反射を計算しているわけではないアート的な近似。
# ゼロにはせずIBLの拡散イラディアンス積分が下半球で完全な暗黒にならないようにする)
GROUND_FADE_START_Y = -0.02
GROUND_FADE_END_Y = -0.6

# CIE快晴空係数(circumsolar項 c=10, d=-3)は反太陽側の水平線で輝度が天頂の0.2倍程度まで落ちる。
# 実際の大気は多重散乱で暗部が持ち上がるためゼロにはしないが、0.45まで底上げしていたときは
# 輝度の勾配がほぼ消えて空全体が一様なスレートグレーになっていた(実測: 彩度0.26で時刻不変)。
# 勾配が残る値まで下げてある
# (sky_color_upper_unit と compute_zenith_scale の両方から参照するのでモジュール定数にしてある。
#  Sky.hlsli の kRelativeLuminanceFloor と一致させること)
RELATIVE_LUMINANCE_FLOOR = 0.12

# 大気の濁り具合(P7: Preetham xyYモデルのタービディティ)。EngineDefaults.h::SkyTurbidityと
# 同じ既定値(2.5)。Preethamの定義域はおおむね1.7〜10で、実測値ではなく見た目からの選択
DEFAULT_TURBIDITY = 2.5

# Preethamの重みを求める仰角の遷移幅(度)。仰角0度で0(従来ティントのみ)、5度で1(Preethamのみ)。
# Sky.hlsli SkyColorUpperUnitと同じ閾値であること
PREETHAM_TRANSITION_END_DEGREES = 5.0


def smoothstep(edge0, edge1, x):
    t = np.clip((x - edge0) / (edge1 - edge0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def compute_sky_tint(sun_elevation_sin):
    """太陽高度のサインから空の色味を決める。KurenaiEngine3D.cpp の ComputeSkyTint と同じ式。

    ここで色味を暗くしても空が暗くなるわけではない点に注意。compute_zenith_scale が
    「色味の輝度成分込みで積分して目標照度に合わせる」ため、色味は最終的な明るさではなく
    色相・彩度だけを決める。
    """
    sin_15deg = np.sin(np.radians(15.0))
    day_blend = smoothstep(0.0, sin_15deg, sun_elevation_sin)
    night_blend = smoothstep(0.0, sin_15deg, -sun_elevation_sin)

    def blend(dusk, night, day):
        return (dusk + (night - dusk) * night_blend) * (1.0 - day_blend) + day * day_blend

    return {
        "zenith": blend(DUSK_ZENITH_TINT, NIGHT_ZENITH_TINT, DAY_ZENITH_TINT),
        "horizon": blend(DUSK_HORIZON_TINT, NIGHT_HORIZON_TINT, DAY_HORIZON_TINT),
        "ground": blend(DUSK_GROUND_TINT, NIGHT_GROUND_TINT, DAY_GROUND_TINT),
        "sun_glow": SUN_GLOW_TINT,
        # 暖色は仰角0度で最大、±15度で0になる三角窓
        "sun_glow_strength": (1.0 - day_blend) * (1.0 - night_blend),
    }


def sky_tint(cos_theta, cos_gamma, tint_set):
    """方向に対する空の色味。KurenaiEngine3D.cpp / SkyGenerate.hlsl の SkyTint と同じ式。"""
    # 水平線側への寄せを3乗カーブにし、高度がある程度あるうちは天頂色をほぼ保ったまま、
    # 水平線ぎわ(仰角の低い最後の範囲)だけで急速に水平線色へブレンドする
    horizon_blend = ((1.0 - np.clip(cos_theta, 0.0, 1.0)) ** 3)[..., None]
    base = tint_set["zenith"] + (tint_set["horizon"] - tint_set["zenith"]) * horizon_blend

    # 太陽から離れるほど急に落ちる4乗カーブ。太陽が地平線下にあっても、その方位の低空には
    # まだ暖色が残る(実際の夕焼けの残光と同じ構造)
    proximity = np.clip(cos_gamma, 0.0, 1.0)
    glow = np.clip(tint_set["sun_glow_strength"] * proximity ** 4, 0.0, 1.0)[..., None]
    return base + (tint_set["sun_glow"] - base) * glow


def compute_exposure(ev100):
    return 1.0 / (1.2 * (2.0 ** ev100))


def sun_direction(time_of_day_hours, sun_azimuth_degrees):
    # KurenaiEngine3D.cpp ComputeSunLightingと同じ式(太陽がある向き。光が進む向きの符号違いに注意)。
    # 太陽本体は描かないが、Perez分布のcircumsolar項(空自体の輝度分布の形)がこの方向を基準にする
    azimuth_radians = np.radians(sun_azimuth_degrees)
    sunrise_horizontal = np.array([np.cos(azimuth_radians), 0.0, np.sin(azimuth_radians)])
    hour_angle = (time_of_day_hours / 24.0) * 2.0 * np.pi - np.pi / 2.0
    sin_hour = np.sin(hour_angle)
    cos_hour = np.cos(hour_angle)
    d = np.array([sunrise_horizontal[0] * cos_hour, sin_hour, sunrise_horizontal[2] * cos_hour])
    return d / np.linalg.norm(d)


def face_direction_grid(face):
    # D3Dのキューブマップ標準の面->方向マッピング(u, vは-1..1)。戻り値はいずれも(FACE_SIZE,FACE_SIZE)
    idx = np.arange(FACE_SIZE)
    v = (2.0 * (idx + 0.5) / FACE_SIZE) - 1.0
    u = (2.0 * (idx + 0.5) / FACE_SIZE) - 1.0
    uu, vv = np.meshgrid(u, v)  # uu/vv shape (FACE_SIZE, FACE_SIZE), row=y, col=x

    if face == 0:    # +X
        d = np.stack([np.ones_like(uu), -vv, -uu], axis=-1)
    elif face == 1:  # -X
        d = np.stack([-np.ones_like(uu), -vv, uu], axis=-1)
    elif face == 2:  # +Y
        d = np.stack([uu, np.ones_like(uu), vv], axis=-1)
    elif face == 3:  # -Y
        d = np.stack([uu, -np.ones_like(uu), -vv], axis=-1)
    elif face == 4:  # +Z
        d = np.stack([uu, -vv, np.ones_like(uu)], axis=-1)
    else:            # -Z
        d = np.stack([-uu, -vv, -np.ones_like(uu)], axis=-1)

    norm = np.linalg.norm(d, axis=-1, keepdims=True)
    return d / norm


def perez_f(cos_theta, gamma, a, b, c, d, e):
    # cos_thetaは0付近(水平線)で発散しないよう呼び出し側でクランプ済みの前提
    return (1.0 + a * np.exp(b / cos_theta)) * (1.0 + c * np.exp(d * gamma) + e * np.cos(gamma) ** 2)


def perez_relative_luminance(cos_theta, gamma, cos_theta_sun, theta_sun):
    # 【既知の不備、意図的に据え置き】この関数の分母はperez_f(cos_theta_sun, theta_sun, ...)だが、
    # Preethamの定義(Table 1〜2)に照らすと本来はperez_f(1.0, theta_sun, ...)(天頂方向=cosθ=0での
    # 評価。gammaのほうはtheta_sunで正しい。天頂は太陽からtheta_sunだけ離れているため)であるべき。
    # sky_color_upper_unitのPreetham側は正しくperez_f(1.0, theta_sun, ...)へ修正済みだが、この
    # 関数自体はP7以前(P9まで)から存在する従来のティント方式の輝度分布であり、ここを直すと
    # 夜・薄明の見た目が変わってSky.hlsli側のP7合格条件(夜の厳密一致)と食い違うため、現状のまま
    # 残している。これは正しいという意味ではなく意図的な保留である
    #
    # CIE快晴空の標準係数(Perez et al. 1993 / Preetham et al. 1999, Table 1)
    a, b, c, d, e = -1.0, -0.32, 10.0, -3.0, 0.45
    numerator = perez_f(cos_theta, gamma, a, b, c, d, e)
    denominator = perez_f(cos_theta_sun, theta_sun, a, b, c, d, e)
    return numerator / denominator


def preetham_luminance_coeffs(turbidity):
    # Y(輝度)の5係数。従来の固定CIE快晴空係数(a=-1,b=-0.32,c=10,d=-3,e=0.45)を
    # タービディティ依存(Preetham et al. 1999, Table 1)へ差し替える
    a = 0.1787 * turbidity - 1.4630
    b = -0.3554 * turbidity + 0.4275
    c = -0.0227 * turbidity + 5.3251
    d = 0.1206 * turbidity - 2.5771
    e = -0.0670 * turbidity + 0.3703
    return a, b, c, d, e


def preetham_chroma_x_coeffs(turbidity):
    a = -0.0193 * turbidity - 0.2592
    b = -0.0665 * turbidity + 0.0008
    c = -0.0004 * turbidity + 0.2125
    d = -0.0641 * turbidity - 0.8989
    e = -0.0033 * turbidity + 0.0452
    return a, b, c, d, e


def preetham_chroma_y_coeffs(turbidity):
    a = -0.0167 * turbidity - 0.2608
    b = -0.0950 * turbidity + 0.0092
    c = -0.0079 * turbidity + 0.2102
    d = -0.0441 * turbidity - 1.6537
    e = -0.0109 * turbidity + 0.0529
    return a, b, c, d, e


def preetham_zenith_chromaticity(turbidity, theta_sun):
    # 天頂の色度(x_z, y_z)。太陽天頂角theta_sun[ラジアン]の3次式(Preetham et al. 1999, Table 2)
    t2 = turbidity * turbidity
    ts2 = theta_sun * theta_sun
    ts3 = ts2 * theta_sun
    x_z = (t2 * (0.00166 * ts3 - 0.00375 * ts2 + 0.00209 * theta_sun + 0.0)
           + turbidity * (-0.02903 * ts3 + 0.06377 * ts2 - 0.03202 * theta_sun + 0.00394)
           + (0.11693 * ts3 - 0.21196 * ts2 + 0.06052 * theta_sun + 0.25885))
    y_z = (t2 * (0.00275 * ts3 - 0.00610 * ts2 + 0.00317 * theta_sun + 0.0)
           + turbidity * (-0.04214 * ts3 + 0.08970 * ts2 - 0.04153 * theta_sun + 0.00516)
           + (0.15346 * ts3 - 0.26756 * ts2 + 0.06669 * theta_sun + 0.26688))
    return x_z, y_z


def xyy_to_linear_srgb(x, y, Y):
    """xyY色空間から線形sRGB(Rec.709/D65)へ変換する。Sky.hlsliのXyYToLinearSRGBと同じ式。

    負成分ぶんだけ全チャンネルへ白を足すデサチュレーションを入れてあるが、これは保険であって
    常用される経路ではない。実測(タービディティ1.7〜8.0 × 太陽仰角5〜60度で上半球を32×64方向に
    走査)では負値が出た方向は0.0%だった。Preethamの色度はCIE図の中央付近に収まり、そこは
    sRGBの三角形の内側だからである。単純クランプにしていないのは、万一発動したときに彩度が
    飛ぶため。デサチュレーションは輝度を持ち上げるので、操作後に元のYへ戻るよう再スケールする。
    """
    # yが0近傍だとX,Zがゼロ除算で発散するため下限をクランプする
    safe_y = np.maximum(y, 1e-4)
    X = (x / safe_y) * Y
    Z = ((1.0 - x - y) / safe_y) * Y

    r = 3.2406 * X - 1.5372 * Y - 0.4986 * Z
    g = -0.9689 * X + 1.8758 * Y + 0.0415 * Z
    b = 0.0557 * X - 0.2040 * Y + 1.0570 * Z
    rgb = np.stack([r, g, b], axis=-1)

    # 色域外(負値)を白へ寄せるデサチュレーション。全成分に同じ量を足す = 白を混ぜる
    min_component = np.min(rgb, axis=-1, keepdims=True)
    rgb = np.where(min_component < 0.0, rgb - min_component, rgb)

    # デサチュレーションで持ち上がった輝度を元のYへ戻す再スケール(Yが0近傍のときのゼロ除算に注意)
    rescaled_luminance = (rgb[..., 0] * 0.2126 + rgb[..., 1] * 0.7152 + rgb[..., 2] * 0.0722)
    scale = np.where(rescaled_luminance > 1e-6, Y / np.maximum(rescaled_luminance, 1e-6), 1.0)
    rgb = rgb * scale[..., None]

    return rgb


def sky_color_upper_unit(dirs, sun_dir, tint_set, turbidity):
    """天頂輝度を1としたときの空の色(水平線以上)。Sky.hlsliのSkyColorUpperUnitと同じ式。

    日中(太陽仰角5度以上)はPreetham xyYモデルから色度を求める。夜・薄明(太陽仰角5度未満)は
    従来のティント補間(compute_sky_tint/sky_tint)を使い、その間(0〜5度)をクロスフェードする。
    zenith_luminanceを一切参照しない(compute_zenith_scaleがこの関数の結果を積分して
    zenith_luminanceを逆算するため、参照すると循環定義になる)。
    """
    dir_y = dirs[..., 1]

    # Perez分布は水平線(cosθ→0)で数式が不安定になるため、天頂角を89.5度までにクランプする
    clamped_y = np.maximum(dir_y, np.cos(np.radians(89.5)))
    cos_theta = np.clip(clamped_y, 1e-3, 1.0)

    theta_sun = np.arccos(np.clip(sun_dir[1], -1.0, 1.0))
    cos_theta_sun = max(np.cos(theta_sun), 1e-3)

    cos_gamma = np.clip(np.tensordot(dirs, sun_dir, axes=([-1], [0])), -1.0, 1.0)
    gamma = np.arccos(cos_gamma)

    def legacy_color():
        relative = perez_relative_luminance(cos_theta, gamma, cos_theta_sun, theta_sun)
        relative = np.maximum(relative, 0.0)
        relative = RELATIVE_LUMINANCE_FLOOR + (1.0 - RELATIVE_LUMINANCE_FLOOR) * relative
        tint = sky_tint(cos_theta, cos_gamma, tint_set)
        return relative[..., None] * tint

    # Preethamの重み。仰角0度で0(従来ティントのみ)、仰角5度で1(Preethamのみ)
    preetham_weight = smoothstep(0.0, np.sin(np.radians(PREETHAM_TRANSITION_END_DEGREES)), sun_dir[1])
    if preetham_weight <= 0.0:
        # 【夜の厳密一致】太陽が地平線下ではPreethamの計算を一切行わず、従来のティント色を返す
        return legacy_color()

    # --- Preetham et al. 1999のxyYモデル ---
    # 【分母はperez_f(1, theta_sun)、cos_theta_sunではない】Preethamの正規化はF(θ,γ)/F(0,θs)で、
    # 分母は「天頂方向(θ=0、cosθ=1)での評価」。天頂は太陽からtheta_sunだけ離れているためgammaの
    # ほうはtheta_sunで正しいが、第1引数(cosθの位置)にcos(theta_sun)を渡すと天頂での評価(θ=0)
    # ではなく太陽方向での評価になってしまう。これを入れると天頂の値がy_a/x_z/y_zに一致しなくなり、
    # 太陽が低いほど誤差が拡大する(実測: タービディティ2.5・仰角5度でxの誤差-22%)
    y_a, y_b, y_c, y_d, y_e = preetham_luminance_coeffs(turbidity)
    preetham_relative = perez_f(cos_theta, gamma, y_a, y_b, y_c, y_d, y_e) / perez_f(
        1.0, theta_sun, y_a, y_b, y_c, y_d, y_e)
    preetham_relative = np.maximum(preetham_relative, 0.0)
    preetham_relative = RELATIVE_LUMINANCE_FLOOR + (1.0 - RELATIVE_LUMINANCE_FLOOR) * preetham_relative

    x_a, x_b, x_c, x_d, x_e = preetham_chroma_x_coeffs(turbidity)
    cy_a, cy_b, cy_c, cy_d, cy_e = preetham_chroma_y_coeffs(turbidity)
    x_z, y_z = preetham_zenith_chromaticity(turbidity, theta_sun)

    # 分母は輝度と同じ理由でperez_f(1, theta_sun)(天頂方向での評価)。これにより天頂方向
    # (theta=0)を評価するとchroma_x==x_z・chroma_y==y_zに厳密に一致する(定義上そうあるべき値)
    chroma_x = x_z * (perez_f(cos_theta, gamma, x_a, x_b, x_c, x_d, x_e)
                       / perez_f(1.0, theta_sun, x_a, x_b, x_c, x_d, x_e))
    chroma_y = y_z * (perez_f(cos_theta, gamma, cy_a, cy_b, cy_c, cy_d, cy_e)
                       / perez_f(1.0, theta_sun, cy_a, cy_b, cy_c, cy_d, cy_e))

    preetham_color = xyy_to_linear_srgb(chroma_x, chroma_y, preetham_relative)

    if preetham_weight >= 1.0:
        return preetham_color

    return legacy_color() * (1.0 - preetham_weight) + preetham_color * preetham_weight


def sky_color_upper(dirs, sun_dir, zenith_luminance, tint_set, turbidity):
    # 水平線以上を仮定した空の色(呼び出し側でground_fadeと合成する)
    return zenith_luminance * sky_color_upper_unit(dirs, sun_dir, tint_set, turbidity)


def build_face_array(face, sun_dir, zenith_luminance, tint_set, turbidity):
    dirs = face_direction_grid(face)  # (FACE_SIZE, FACE_SIZE, 3)
    dir_y = dirs[..., 1]

    upper_color = sky_color_upper(dirs, sun_dir, zenith_luminance, tint_set, turbidity)

    # 水平線より下: プラトー色(GROUND_FADE_START_Yの高さに射影した方向の空色)から
    # 暗い接地色へフェード(地面の物理モデルは持たないアート的近似)
    plateau_dirs = dirs.copy()
    plateau_dirs[..., 1] = GROUND_FADE_START_Y
    plateau_dirs = plateau_dirs / np.linalg.norm(plateau_dirs, axis=-1, keepdims=True)
    plateau_color = sky_color_upper(plateau_dirs, sun_dir, zenith_luminance, tint_set, turbidity)

    ground_color = zenith_luminance * tint_set["ground"]
    ground_t = np.clip((dir_y - GROUND_FADE_START_Y) / (GROUND_FADE_END_Y - GROUND_FADE_START_Y), 0.0, 1.0)
    below_color = plateau_color * (1.0 - ground_t[..., None]) + ground_color[None, None, :] * ground_t[..., None]

    is_above = (dir_y >= GROUND_FADE_START_Y)[..., None]
    color = np.where(is_above, upper_color, below_color)

    rgba = np.concatenate([color, np.ones(color.shape[:-1] + (1,))], axis=-1)
    return rgba.astype(np.float16)


def compute_zenith_scale(sun_dir, target_illuminance_lux, tint_set, turbidity):
    # 天頂輝度スケールを、上半球の余弦重み積分が目標照度に一致するよう正規化して求める。
    #
    # 照度E[lx]と輝度L[cd/m^2]は E = ∫L·cosθ dω の関係にあるので、SKY_ILLUMINANCE_LUXを
    # そのまま天頂輝度として使うと、実際に届く照度は積分値の分だけずれる。しかもPerez分布の
    # 形は太陽高度で変わるため、そのずれ自体が時刻とともに動く。
    #   太陽高度90度 → 積分1.080 → 届く照度 21,600 lx
    #   太陽高度45度 → 積分1.898 → 届く照度 37,960 lx
    # (輝度フロア0.45・旧ティントでの実測値。フロアとティントを変えれば積分値も変わるが、
    #  正規化しているので最終的な照度は変わらない)
    # 正規化すると常に目標値ちょうどになる。
    #
    # 補足: 「一様な空ならL=E/πなので従来はπ倍明るかった」という説明は誤り。積分には
    # 実際に画面へ出る色のRec.709輝度成分も入るため、単位球の積分はπ(3.14)には遠く及ばず、
    # 正午での補正は数%〜十数%にすぎない。
    #
    # KurenaiEngine3D.cpp から移った SkyIntegrate.hlsl の CSIntegrateSky と同じ結果になること。
    # 一方だけ変えると、オフラインで焼いたDDSと手続き空の明るさが食い違う
    #
    # 【P7】被積分関数は「sky_color_upper_unit(=SkyColorUpperUnit)の結果のRec.709輝度」。
    # 以前は「Perez相対輝度 × ティントの輝度成分」だったが、Preethamで輝度(Y)と色度(x,y)が
    # 分離したためこの形に変えた(Sky.hlsli SkyIntegrate.hlsl冒頭のP7コメントと同じ理由)
    theta_steps = 64
    phi_steps = 256

    d_theta = (np.pi / 2.0) / theta_steps
    d_phi = (2.0 * np.pi) / phi_steps

    # 中点則。thetaは行、phiは列
    theta = (np.arange(theta_steps) + 0.5) * d_theta
    phi = (np.arange(phi_steps) + 0.5) * d_phi

    cos_theta_raw = np.cos(theta)[:, None]
    sin_theta = np.sin(theta)[:, None]

    dirs_x = sin_theta * np.cos(phi)[None, :]
    dirs_y = np.broadcast_to(cos_theta_raw, (theta_steps, phi_steps))
    dirs_z = sin_theta * np.sin(phi)[None, :]
    dirs = np.stack([dirs_x, dirs_y, dirs_z], axis=-1)  # (theta_steps, phi_steps, 3)

    unit_color = sky_color_upper_unit(dirs, sun_dir, tint_set, turbidity)
    weight = unit_color[..., 0] * 0.2126 + unit_color[..., 1] * 0.7152 + unit_color[..., 2] * 0.0722

    # dω = sinθ dθ dφ、余弦重みは cosθ
    integral = np.sum(weight * cos_theta_raw * sin_theta) * d_theta * d_phi
    if integral < 1e-6:
        return target_illuminance_lux
    return target_illuminance_lux / integral


def main():
    os.makedirs(OUT_DIR, exist_ok=True)

    exposure = compute_exposure(DEFAULT_EV100)
    sun_dir = sun_direction(DEFAULT_TIME_OF_DAY_HOURS, DEFAULT_SUN_AZIMUTH_DEGREES)
    turbidity = DEFAULT_TURBIDITY
    # 空の色味は太陽高度で決まる。生成と照度正規化の両方が同じセットを使う必要がある
    tint_set = compute_sky_tint(sun_dir[1])
    zenith_luminance = compute_zenith_scale(sun_dir, SKY_ILLUMINANCE_LUX, tint_set, turbidity) * exposure

    DDSD_CAPS = 0x1
    DDSD_HEIGHT = 0x2
    DDSD_WIDTH = 0x4
    DDSD_PITCH = 0x8
    DDSD_PIXELFORMAT = 0x1000
    header_flags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT

    DDPF_FOURCC = 0x4

    DDSCAPS_TEXTURE = 0x1000
    DDSCAPS_COMPLEX = 0x8
    caps = DDSCAPS_TEXTURE | DDSCAPS_COMPLEX

    DDSCAPS2_CUBEMAP = 0x200
    DDSCAPS2_CUBEMAP_POSITIVEX = 0x400
    DDSCAPS2_CUBEMAP_NEGATIVEX = 0x800
    DDSCAPS2_CUBEMAP_POSITIVEY = 0x1000
    DDSCAPS2_CUBEMAP_NEGATIVEY = 0x2000
    DDSCAPS2_CUBEMAP_POSITIVEZ = 0x4000
    DDSCAPS2_CUBEMAP_NEGATIVEZ = 0x8000
    caps2 = (DDSCAPS2_CUBEMAP | DDSCAPS2_CUBEMAP_POSITIVEX | DDSCAPS2_CUBEMAP_NEGATIVEX |
             DDSCAPS2_CUBEMAP_POSITIVEY | DDSCAPS2_CUBEMAP_NEGATIVEY |
             DDSCAPS2_CUBEMAP_POSITIVEZ | DDSCAPS2_CUBEMAP_NEGATIVEZ)

    bytes_per_pixel = 8  # R16G16B16A16_Float = 4channel x 2byte
    pitch = FACE_SIZE * bytes_per_pixel

    header = bytearray()
    header += struct.pack("<I", 124)              # dwSize
    header += struct.pack("<I", header_flags)      # dwFlags
    header += struct.pack("<I", FACE_SIZE)         # dwHeight
    header += struct.pack("<I", FACE_SIZE)         # dwWidth
    header += struct.pack("<I", pitch)             # dwPitchOrLinearSize
    header += struct.pack("<I", 0)                 # dwDepth
    header += struct.pack("<I", 1)                 # dwMipMapCount
    header += b"\x00" * 44                          # dwReserved1[11]
    # DDS_PIXELFORMAT
    header += struct.pack("<I", 32)                # ddspf.dwSize
    header += struct.pack("<I", DDPF_FOURCC)       # ddspf.dwFlags
    header += b"DX10"                                # ddspf.dwFourCC
    header += struct.pack("<I", 0)                 # ddspf.dwRGBBitCount
    header += struct.pack("<I", 0)                 # ddspf.dwRBitMask
    header += struct.pack("<I", 0)                 # ddspf.dwGBitMask
    header += struct.pack("<I", 0)                 # ddspf.dwBBitMask
    header += struct.pack("<I", 0)                 # ddspf.dwABitMask
    header += struct.pack("<I", caps)              # dwCaps
    header += struct.pack("<I", caps2)             # dwCaps2
    header += struct.pack("<I", 0)                 # dwCaps3
    header += struct.pack("<I", 0)                 # dwCaps4
    header += struct.pack("<I", 0)                 # dwReserved2
    assert len(header) == 124, len(header)

    DXGI_FORMAT_R16G16B16A16_FLOAT = 10
    D3D10_RESOURCE_DIMENSION_TEXTURE2D = 3
    DDS_RESOURCE_MISC_TEXTURECUBE = 0x4

    header_dxt10 = bytearray()
    header_dxt10 += struct.pack("<I", DXGI_FORMAT_R16G16B16A16_FLOAT)
    header_dxt10 += struct.pack("<I", D3D10_RESOURCE_DIMENSION_TEXTURE2D)
    header_dxt10 += struct.pack("<I", DDS_RESOURCE_MISC_TEXTURECUBE)
    header_dxt10 += struct.pack("<I", 1)  # arraySize (キューブ数)
    header_dxt10 += struct.pack("<I", 0)  # miscFlags2
    assert len(header_dxt10) == 20

    with open(OUT_PATH, "wb") as f:
        f.write(b"DDS ")
        f.write(header)
        f.write(header_dxt10)
        # D3Dキューブマップの面順: +X, -X, +Y, -Y, +Z, -Z
        for face in range(6):
            arr = build_face_array(face, sun_dir, zenith_luminance, tint_set, turbidity)
            f.write(arr.tobytes())

    print(f"wrote {OUT_PATH} ({FACE_SIZE}x{FACE_SIZE} x6 faces, R16G16B16A16_Float)")
    print(f"zenith luminance (post-exposure) = {zenith_luminance:.4f}")


if __name__ == "__main__":
    main()
