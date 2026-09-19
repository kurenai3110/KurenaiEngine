# -*- coding: utf-8 -*-
"""MegaLights が使う 64x64 のブルーノイズマスクを作り、HLSL のヘッダーへ焼く。

なぜ要るか:
  MegaLights の画素ごとの乱数位相は Interleaved Gradient Noise だった。IGN は
  「隣接画素の値が離れる」ことは満たすが**等方ではない**。等値線が直線なので、
  同じ位相を持つ画素が直線状に並び、候補スロットの選び方まで直線状に揃う。
  結果、デノイザを切ると細い斜めの筋として見える。
  ここで作るのは Ulichney の void-and-cluster によるマスクで、こちらは
  どの方向にも偏らない(等方な高周波)。

使い方:
  python Tools/make_blue_noise_mask.py
  python Tools/make_blue_noise_mask.py --check   # 生成物が最新かだけ調べる(書かない)

出力:
  KurenaiEngine/Shaders/3D/MegaLightsBlueNoise.hlsli
"""

import argparse
import os
import sys

import numpy as np

# マスクの1辺。2の冪にしておくと、シェーダ側の折り返しが & (N-1) で済む
MASK_SIZE = 64
# void-and-cluster のガウスフィルタの幅。Ulichney の原論文が 1.5 を挙げている
SIGMA = 1.5
# 初期の二値パターンで 1 にする割合。1割前後が安定するとされる
INITIAL_RATIO = 0.1
# 乱数の種。生成を決定的にするために固定する(同じ入力なら同じマスクが出る)
SEED = 20260916

OUTPUT_PATH = os.path.join("KurenaiEngine", "Shaders", "3D", "MegaLightsBlueNoise.hlsli")


def build_kernel(size: int, sigma: float) -> np.ndarray:
    """トーラス上のガウス核。距離は必ず折り返した最短距離で測る。"""
    half = size // 2
    d = np.arange(size)
    d = np.minimum(d, size - d)  # 折り返した距離
    dx = d[None, :].astype(np.float64)
    dy = d[:, None].astype(np.float64)
    k = np.exp(-(dx * dx + dy * dy) / (2.0 * sigma * sigma))
    if half <= 0:
        raise ValueError("マスクの1辺が小さすぎる: %d" % size)
    return k


def make_initial_pattern(size: int, ratio: float, rng: np.random.Generator,
                         kernel: np.ndarray) -> np.ndarray:
    """初期二値パターン。密集を崩し空隙を埋めて、動かなくなるまで繰り返す。"""
    count = max(1, int(round(size * size * ratio)))
    flat = rng.permutation(size * size)[:count]
    pattern = np.zeros((size, size), dtype=bool)
    pattern.flat[flat] = True

    energy = filter_energy(pattern, kernel)
    # 無限ループの保険。1要素も動かなくなれば途中で抜ける
    for _ in range(size * size * 4):
        ty, tx = tightest_cluster(pattern, energy)
        pattern[ty, tx] = False
        energy = add_kernel(energy, kernel, ty, tx, -1.0)

        vy, vx = largest_void(pattern, energy)
        if (vy, vx) == (ty, tx):
            # 取り除いた場所がそのまま最大の空隙 = 収束
            pattern[ty, tx] = True
            energy = add_kernel(energy, kernel, ty, tx, +1.0)
            return pattern
        pattern[vy, vx] = True
        energy = add_kernel(energy, kernel, vy, vx, +1.0)
    print("警告: 初期パターンが収束しなかった。そのまま続行する", file=sys.stderr)
    return pattern


def filter_energy(pattern: np.ndarray, kernel: np.ndarray) -> np.ndarray:
    """パターン全体の巡回畳み込み。初期化のときだけ使う(以降は差分更新)。"""
    return np.real(np.fft.ifft2(np.fft.fft2(pattern.astype(np.float64)) * np.fft.fft2(kernel)))


def add_kernel(energy: np.ndarray, kernel: np.ndarray, y: int, x: int, sign: float) -> np.ndarray:
    """1点の増減ぶんだけエネルギーを更新する。毎回畳み込み直すと数千倍遅い。"""
    return energy + sign * np.roll(np.roll(kernel, y, axis=0), x, axis=1)


def tightest_cluster(pattern: np.ndarray, energy: np.ndarray):
    """1 が最も密集している場所。0 の場所は候補から外す。"""
    masked = np.where(pattern, energy, -np.inf)
    return np.unravel_index(int(np.argmax(masked)), pattern.shape)


def largest_void(pattern: np.ndarray, energy: np.ndarray):
    """0 が最も広く空いている場所。1 の場所は候補から外す。"""
    masked = np.where(pattern, np.inf, energy)
    return np.unravel_index(int(np.argmin(masked)), pattern.shape)


def void_and_cluster(size: int) -> np.ndarray:
    """0..size*size-1 の順位を返す。順位を値とみなすとブルーノイズになる。"""
    rng = np.random.default_rng(SEED)
    kernel = build_kernel(size, SIGMA)
    pattern = make_initial_pattern(size, INITIAL_RATIO, rng, kernel)

    rank = np.full((size, size), -1, dtype=np.int64)
    total = size * size

    # 第1段: 初期パターンの 1 を、密集している順に外しながら下へ番号を振る
    work = pattern.copy()
    energy = filter_energy(work, kernel)
    ones = int(work.sum())
    for r in range(ones - 1, -1, -1):
        y, x = tightest_cluster(work, energy)
        work[y, x] = False
        energy = add_kernel(energy, kernel, y, x, -1.0)
        rank[y, x] = r

    # 第2段: 初期パターンへ戻し、空隙の広い順に埋めながら上へ番号を振る
    work = pattern.copy()
    energy = filter_energy(work, kernel)
    for r in range(ones, (total + 1) // 2):
        y, x = largest_void(work, energy)
        work[y, x] = True
        energy = add_kernel(energy, kernel, y, x, +1.0)
        rank[y, x] = r

    # 第3段: 以降は 0 と 1 を入れ替えて考える。
    # 「0 側の密集」を外していくのが、そのまま残りの順位になる
    energy = filter_energy(~work, kernel)
    inverted = ~work
    for r in range((total + 1) // 2, total):
        y, x = tightest_cluster(inverted, energy)
        inverted[y, x] = False
        energy = add_kernel(energy, kernel, y, x, -1.0)
        rank[y, x] = r

    if (rank < 0).any():
        raise RuntimeError("順位の割り当てに抜けがある: %d 画素" % int((rank < 0).sum()))
    if len(np.unique(rank)) != total:
        raise RuntimeError("順位が重複している")
    return rank


def anisotropy(values: np.ndarray) -> float:
    """周期4〜32画素の帯で、角度5度ビンの最大エネルギー比を返す。等方なら 1/36。"""
    v = values - values.mean()
    n = v.shape[0]
    p = np.abs(np.fft.fftshift(np.fft.fft2(v))) ** 2
    c = n // 2
    gy, gx = np.mgrid[0:n, 0:n]
    fy = (gy - c) / n
    fx = (gx - c) / n
    r = np.hypot(fx, fy)
    band = (r > 1.0 / 32.0) & (r < 0.25)
    if not band.any():
        raise ValueError("帯に周波数が1つも入らない。マスクが小さすぎる")
    ang = np.degrees(np.arctan2(fy, fx)) % 180.0
    hist = np.array([p[band & (np.abs(((ang - a + 90.0) % 180.0) - 90.0) < 5.0)].sum()
                     for a in range(0, 180, 5)])
    return float(hist.max() / hist.sum())


def render_header(rank: np.ndarray) -> str:
    """順位を 8bit へ落として uint へ4つずつ詰め、HLSL のヘッダー本文を返す。"""
    size = rank.shape[0]
    total = size * size
    # 値は 0..255。4096 順位を 16 個ずつ束ねる ―― 候補スロットの選択が使うのは
    # 上位6bit 程度なので、8bit あれば足りる(束ねられた画素どうしは十分離れている)
    values = (rank * 256 // total).astype(np.uint32)
    if values.max() > 255:
        raise RuntimeError("8bit へ収まっていない: 最大 %d" % int(values.max()))
    packed = (values.reshape(-1, 4) * np.array([1, 1 << 8, 1 << 16, 1 << 24], dtype=np.uint32)).sum(axis=1)

    aniso = anisotropy(values.astype(np.float64))
    lines = []
    lines.append("// このファイルは Tools/make_blue_noise_mask.py が生成している。手で編集しないこと。")
    lines.append("// 作り直すには: python Tools/make_blue_noise_mask.py")
    lines.append("//")
    lines.append("// %dx%d の void-and-cluster ブルーノイズ(Ulichney 1993)。" % (size, size))
    lines.append("// 値は順位を 8bit へ落としたもので、1 uint に4画素ぶんを詰めてある。")
    lines.append("// 角度の偏り(周期4〜32画素の帯・5度ビンの最大比) = %.4f(等方は %.4f)。" % (aniso, 1.0 / 36.0))
    lines.append("// 比較: 同じ検査で Interleaved Gradient Noise は 0.36 前後まで偏る。")
    lines.append("#ifndef KURENAI_MEGALIGHTS_BLUE_NOISE_HLSLI")
    lines.append("#define KURENAI_MEGALIGHTS_BLUE_NOISE_HLSLI")
    lines.append("")
    lines.append("static const uint kMegaLightsBlueNoiseSize = %du;" % size)
    lines.append("static const uint kMegaLightsBlueNoiseMask = %du;" % (size - 1))
    lines.append("")
    lines.append("static const uint kMegaLightsBlueNoise[%d] =" % len(packed))
    lines.append("{")
    for i in range(0, len(packed), 8):
        chunk = ", ".join("0x%08Xu" % int(v) for v in packed[i:i + 8])
        lines.append("    %s," % chunk)
    lines.append("};")
    lines.append("")
    lines.append("// 画素座標からマスクの値を [0,1) で引く。マスクはタイル状に敷き詰める")
    lines.append("float MegaLightsBlueNoiseValue(uint2 pixel)")
    lines.append("{")
    lines.append("    const uint2 p = pixel & kMegaLightsBlueNoiseMask;")
    lines.append("    const uint index = p.y * kMegaLightsBlueNoiseSize + p.x;")
    lines.append("    const uint packedValue = kMegaLightsBlueNoise[index >> 2u];")
    lines.append("    const uint byteValue = (packedValue >> ((index & 3u) * 8u)) & 0xFFu;")
    lines.append("    return (float(byteValue) + 0.5f) * (1.0f / 256.0f);")
    lines.append("}")
    lines.append("")
    lines.append("#endif // KURENAI_MEGALIGHTS_BLUE_NOISE_HLSLI")
    lines.append("")
    return "\r\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description="MegaLights 用ブルーノイズマスクの生成")
    parser.add_argument("--check", action="store_true",
                        help="生成物が最新かだけ調べる。書き換えない")
    args = parser.parse_args()

    try:
        rank = void_and_cluster(MASK_SIZE)
        text = render_header(rank)
    except Exception as exc:  # 生成に失敗したら、古い生成物を残したまま落とす
        print("エラー: マスクの生成に失敗した: %s" % exc, file=sys.stderr)
        return 1

    data = text.encode("utf-8")
    if args.check:
        if not os.path.exists(OUTPUT_PATH):
            print("NG 生成物が無い: %s" % OUTPUT_PATH, file=sys.stderr)
            return 1
        with open(OUTPUT_PATH, "rb") as f:
            current = f.read()
        if current != data:
            print("NG 生成物が最新でない: %s" % OUTPUT_PATH, file=sys.stderr)
            return 1
        print("OK 生成物は最新: %s" % OUTPUT_PATH)
        return 0

    try:
        with open(OUTPUT_PATH, "wb") as f:
            f.write(data)
    except OSError as exc:
        print("エラー: 書き出しに失敗した: %s" % exc, file=sys.stderr)
        return 1
    print("OK %s を生成した(%dx%d)" % (OUTPUT_PATH, MASK_SIZE, MASK_SIZE))
    return 0


if __name__ == "__main__":
    sys.exit(main())
