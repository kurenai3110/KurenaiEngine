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

# 空の色味(天頂は青みが強く、地平線付近は白っぽくなる)。物理的な分光計算(Rayleigh散乱の
# 波長依存性を積分するなど)はせず、Perez分布が与える輝度の大きさ(スケール)はそのままに、
# 色味だけをこの範囲で補間する簡略化(アート的な近似であることを明記する)。
# 実際の快晴の空は天頂から中程度の高度まで彩度の高い青を保ち、本当の水平線ぎわ(最後の
# 20〜30度程度)でようやく白っぽくなる。天頂→水平線を単純に線形補間すると、ゲームカメラが
# 見る典型的な低めの仰角(建物越しに覗く空など)でもすでに大きく白側へ寄ってしまい、
# 「青空に見えない」結果になる(実際に一度この問題が起きた)。そのためTINT自体を水平線側でも
# はっきり青みが残る値にし、かつ後述のブレンド係数も水平線ぎわに寄せてある
ZENITH_TINT = np.array([0.30, 0.55, 0.95])
HORIZON_TINT = np.array([0.65, 0.80, 1.0])

# 地平線よりさらに下(地面方向)は空のモデルの適用範囲外のため、水平線のプラトー色から
# この暗い接地色へフェードさせる(実際の地面反射を計算しているわけではないアート的な近似。
# ゼロにはせずIBLの拡散イラディアンス積分が下半球で完全な暗黒にならないようにする)
GROUND_TINT = np.array([0.10, 0.09, 0.08])
GROUND_FADE_START_Y = -0.02
GROUND_FADE_END_Y = -0.6


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
    # CIE快晴空の標準係数(Perez et al. 1993 / Preetham et al. 1999, Table 1)
    a, b, c, d, e = -1.0, -0.32, 10.0, -3.0, 0.45
    numerator = perez_f(cos_theta, gamma, a, b, c, d, e)
    denominator = perez_f(cos_theta_sun, theta_sun, a, b, c, d, e)
    return numerator / denominator


def sky_color_upper(dirs, sun_dir, zenith_luminance):
    # dirsは(...,3)の方向配列。水平線以上(GROUND_FADE_START_Y以上)を仮定した空モデルの色を返す
    # (呼び出し側でground_fadeと合成する)
    dir_y = dirs[..., 1]

    # Perez分布は水平線(cosθ→0)で数式が不安定になるため、天頂角を89.5度までにクランプする
    clamped_y = np.maximum(dir_y, np.cos(np.radians(89.5)))
    cos_theta = np.clip(clamped_y, 1e-3, 1.0)

    theta_sun = np.arccos(np.clip(sun_dir[1], -1.0, 1.0))
    cos_theta_sun = max(np.cos(theta_sun), 1e-3)

    cos_gamma = np.clip(np.tensordot(dirs, sun_dir, axes=([-1], [0])), -1.0, 1.0)
    gamma = np.arccos(cos_gamma)

    relative = perez_relative_luminance(cos_theta, gamma, cos_theta_sun, theta_sun)
    relative = np.maximum(relative, 0.0)
    # CIE快晴空係数(circumsolar項、c=10, d=-3)は太陽から45度も離れると輝度が天頂の1/4程度まで
    # 急激に落ち込む(単一散乱のみを仮定した理想的な快晴空のモデルのため)。実際の大気は多重散乱・
    # エアロゾルにより太陽から離れた領域もある程度明るく保たれるため、そのままだと典型的な
    # カメラ視点(太陽の真下ではない方向)で「くすんだ暗い空」に見えてしまう(実機で指摘された
    # 見た目の問題)。RELATIVE_LUMINANCE_FLOORで最低輝度を底上げし、circumsolarのハイライトは
    # 保ったまま全体の見た目を明るくする(多重散乱を簡略化して表現するアート的な近似)
    RELATIVE_LUMINANCE_FLOOR = 0.45
    relative = RELATIVE_LUMINANCE_FLOOR + (1.0 - RELATIVE_LUMINANCE_FLOOR) * relative

    # 水平線側への寄せを3乗カーブにし、高度がある程度あるうちは天頂色をほぼ保ったまま、
    # 水平線ぎわ(仰角の低い最後の範囲)だけで急速に白側へブレンドする
    horizon_blend = (1.0 - np.clip(cos_theta, 0.0, 1.0)) ** 3
    tint = ZENITH_TINT[None, None, :] + (HORIZON_TINT - ZENITH_TINT)[None, None, :] * horizon_blend[..., None]
    luminance = relative * zenith_luminance
    return luminance[..., None] * tint


def build_face_array(face, sun_dir, zenith_luminance):
    dirs = face_direction_grid(face)  # (FACE_SIZE, FACE_SIZE, 3)
    dir_y = dirs[..., 1]

    upper_color = sky_color_upper(dirs, sun_dir, zenith_luminance)

    # 水平線より下: プラトー色(GROUND_FADE_START_Yの高さに射影した方向の空色)から
    # 暗い接地色へフェード(地面の物理モデルは持たないアート的近似)
    plateau_dirs = dirs.copy()
    plateau_dirs[..., 1] = GROUND_FADE_START_Y
    plateau_dirs = plateau_dirs / np.linalg.norm(plateau_dirs, axis=-1, keepdims=True)
    plateau_color = sky_color_upper(plateau_dirs, sun_dir, zenith_luminance)

    ground_color = zenith_luminance * GROUND_TINT
    ground_t = np.clip((dir_y - GROUND_FADE_START_Y) / (GROUND_FADE_END_Y - GROUND_FADE_START_Y), 0.0, 1.0)
    below_color = plateau_color * (1.0 - ground_t[..., None]) + ground_color[None, None, :] * ground_t[..., None]

    is_above = (dir_y >= GROUND_FADE_START_Y)[..., None]
    color = np.where(is_above, upper_color, below_color)

    rgba = np.concatenate([color, np.ones(color.shape[:-1] + (1,))], axis=-1)
    return rgba.astype(np.float16)


def main():
    os.makedirs(OUT_DIR, exist_ok=True)

    exposure = compute_exposure(DEFAULT_EV100)
    zenith_luminance = SKY_ILLUMINANCE_LUX * exposure
    sun_dir = sun_direction(DEFAULT_TIME_OF_DAY_HOURS, DEFAULT_SUN_AZIMUTH_DEGREES)

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
            arr = build_face_array(face, sun_dir, zenith_luminance)
            f.write(arr.tobytes())

    print(f"wrote {OUT_PATH} ({FACE_SIZE}x{FACE_SIZE} x6 faces, R16G16B16A16_Float)")
    print(f"zenith luminance (post-exposure) = {zenith_luminance:.4f}")


if __name__ == "__main__":
    main()
