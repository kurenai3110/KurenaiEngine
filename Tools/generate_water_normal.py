"""モン・サン=ミシェル検証シーン用の、水面用タイル可能な接線空間法線マップを生成する。

512x512のRGBA8 PNG。複数オクターブのvalue noiseから高さ場を作り、中央差分で
接線空間法線を求めて (n*0.5+0.5) をRGBへ格納する(Aは常に255)。

【タイル可能性について(周期境界)】
ノイズの格子インデックスを `% N` で巻き戻す(周期境界)ことで、テクスチャの右端と左端
(上端と下端も同様)がちょうど1周分ズレて同じ値になるようにしてある。これが無いと、
格子座標がテクスチャ端で不連続に切れて右端と左端の値が噛み合わず、水面にこのテクスチャを
敷き詰めたときにタイルの継ぎ目が1本の線として見えてしまう(振幅の不連続そのものが
輝度/法線の急変として現れるため)。今回選んだ格子解像度(8, 16, 32)はいずれも512の約数
なので、1セルの一辺(512/N px)が整数になり継ぎ目の位置で補間の丸め誤差も出ない。

【シードについて】
NOISE_SEEDを固定しているため、このスクリプトは毎回まったく同じ法線マップを生成する
(A/B比較の再現性のため。generate_tidal_flat.pyと同じ理由)。

【RGBチャンネルの向き】
KurenaiEngine3D/Shaders/3D/GBuffer.hlslのPSMainは、法線マップのXY(RG)チャンネルだけを
`* 2.0 - 1.0` して読み、Zは `sqrt(1 - x^2 - y^2)` で再構成する(BC5(2ch)圧縮を主対象にした
設計のため、Bチャンネルの値そのものは実行時には使われない)。Rは接線(TANGENT、多くの場合
ワールド+X寄り)方向、Gは従法線(BITANGENT)方向のタンジェント空間X/Yに対応する。
このスクリプトはR=X, G=Y, B=Zの通常の並びでPNGへ書き出す(Bはビューア等で見たときに
違和感が出ないよう正しい値を入れているだけで、エンジン側の描画結果には影響しない)。

生成物: Assets/Source/MontSaintMichelStudy/WaterNormal.png
このフェーズではWater.gltf(generate_water_plane.py)のマテリアルからは参照しない
(このスクリプトはテクスチャアセットの生成だけを扱う)。そのため
KurenaiPacker.exeによる.ktexへの変換は、Water.gltfのマテリアルへnormalTextureとして
組み込んでモデルごとパックするタイミングまで不要(KurenaiPackerはモデルが参照する
テクスチャだけを処理するため、参照されていないPNG単体を変換するコマンドは無い)。
"""

import math
import os
import struct
import sys
import zlib

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(SCRIPT_DIR, "..", "Assets", "Source", "MontSaintMichelStudy")
PNG_NAME = "WaterNormal.png"

MAP_SIZE = 512

# 再現性のため固定(モジュールdocstring参照)
NOISE_SEED = 20260802

# (格子解像度[セル数], 振幅) のオクターブを3枚重ねる。いずれもMAP_SIZE(512)の約数にして
# タイルの継ぎ目で補間が割り切れるようにしてある
NOISE_OCTAVES = [
    (8, 1.00),
    (16, 0.50),
    (32, 0.22),
]

# 高さ場の勾配を法線の傾きへ増幅する係数。振幅が小さい(合計1.72程度)ノイズをそのまま
# 勾配に使うと傾きがほとんど付かず法線マップが真っ青(ほぼ(0,0,1))になってしまうため、
# 見た目のさざ波として十分な強さになるよう経験的に増幅する
NORMAL_STRENGTH = 5.0


def _hash01(ix, iz, seed):
    """周期境界を前提に、格子座標(ix, iz)とシードから[0, 1)の擬似乱数を1つ返す。

    呼び出し側で ix, iz を `% N` 済みにしておくことがタイル可能性の前提
    (このハッシュ自体は周期性を持たないので、境界の巻き戻しは呼び出し側の責務)。
    """
    h = (ix * 374761393 + iz * 668265263 + seed * 2246822519) & 0xFFFFFFFF
    h = (h ^ (h >> 13)) & 0xFFFFFFFF
    h = (h * 1274126177) & 0xFFFFFFFF
    h = (h ^ (h >> 16)) & 0xFFFFFFFF
    return h / 4294967295.0


def _build_periodic_grid(resolution, seed):
    """N x N の格子点にハッシュ由来の値を敷き詰めた2次元配列を作る(周期境界前提)。"""
    return [
        [_hash01(ix, iz, seed) for ix in range(resolution)]
        for iz in range(resolution)
    ]


def _smoothstep(t):
    return t * t * (3.0 - 2.0 * t)


def _sample_periodic(grid, resolution, px, pz, map_size):
    """周期的な格子(resolution x resolution)を512x512ピクセル空間へ双一次補間で拡大する。

    格子インデックスを`% resolution`で巻き戻しているのがタイル可能性の要(モジュールdocstring参照)。
    """
    cell_size = map_size / resolution
    gx = px / cell_size
    gz = pz / cell_size
    ix0 = int(math.floor(gx))
    iz0 = int(math.floor(gz))
    fx = gx - ix0
    fz = gz - iz0

    ix0m = ix0 % resolution
    iz0m = iz0 % resolution
    ix1m = (ix0 + 1) % resolution
    iz1m = (iz0 + 1) % resolution

    v00 = grid[iz0m][ix0m]
    v10 = grid[iz0m][ix1m]
    v01 = grid[iz1m][ix0m]
    v11 = grid[iz1m][ix1m]

    sx = _smoothstep(fx)
    sz = _smoothstep(fz)
    top = v00 + (v10 - v00) * sx
    bottom = v01 + (v11 - v01) * sx
    return top + (bottom - top) * sz


def build_height_field(map_size):
    """複数オクターブの周期value noiseを合成した高さ場(map_size x map_size)を作る。"""
    grids = []
    for octave_index, (resolution, _amplitude) in enumerate(NOISE_OCTAVES):
        grids.append(_build_periodic_grid(resolution, NOISE_SEED + octave_index * 97))

    height = [[0.0] * map_size for _ in range(map_size)]
    for pz in range(map_size):
        for px in range(map_size):
            value = 0.0
            for (resolution, amplitude), grid in zip(NOISE_OCTAVES, grids):
                n = _sample_periodic(grid, resolution, float(px), float(pz), map_size)
                value += (n * 2.0 - 1.0) * amplitude
            height[pz][px] = value
    return height


def height_field_to_normal_rgba(height, map_size, strength):
    """高さ場から接線空間法線を求め、RGBA8のバイト列(行ごと)を返す。

    中央差分・法線ともにmap_sizeで`%`を取って隣接ピクセルを参照するため、端でも不連続が出ない
    (タイル可能性の要。モジュールdocstring参照)。
    """
    rows = []
    for z in range(map_size):
        row = bytearray()
        z_prev = (z - 1) % map_size
        z_next = (z + 1) % map_size
        for x in range(map_size):
            x_prev = (x - 1) % map_size
            x_next = (x + 1) % map_size

            dhdx = (height[z][x_next] - height[z][x_prev]) * 0.5 * strength
            dhdz = (height[z_next][x] - height[z_prev][x]) * 0.5 * strength

            normal_vec = (-dhdx, -dhdz, 1.0)
            length = math.sqrt(sum(c * c for c in normal_vec))
            nx, ny, nz = (c / length for c in normal_vec)

            row.append(int(round((nx * 0.5 + 0.5) * 255.0)))
            row.append(int(round((ny * 0.5 + 0.5) * 255.0)))
            row.append(int(round((nz * 0.5 + 0.5) * 255.0)))
            row.append(255)
        rows.append(bytes(row))
    return rows


def write_rgba_png(path, width, height_px, row_bytes):
    """任意のRGBA8ピクセル配列(行ごとのbytes)からPNGを書き出す。

    Tools/generate_material_test.pyのwrite_solid_rgba_pngと同じ方式(zlib + struct、
    フィルタバイト0、標準ライブラリのみ)を、任意ピクセル配列を渡せるように一般化したもの。
    """

    def chunk(kind: bytes, data: bytes) -> bytes:
        return (
            struct.pack(">I", len(data))
            + kind
            + data
            + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
        )

    ihdr = struct.pack(">IIBBBBB", width, height_px, 8, 6, 0, 0, 0)

    # 各スキャンラインの先頭にフィルタタイプ(0=None)を1バイト置くのがPNGの規定
    raw = b"".join(b"\x00" + row for row in row_bytes)

    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", ihdr)
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )

    with open(path, "wb") as f:
        f.write(png)


def main():
    try:
        os.makedirs(OUT_DIR, exist_ok=True)
    except OSError as error:
        print(f"[ERROR] 出力ディレクトリの作成に失敗しました: {OUT_DIR} ({error})", file=sys.stderr)
        raise

    height = build_height_field(MAP_SIZE)
    row_bytes = height_field_to_normal_rgba(height, MAP_SIZE, NORMAL_STRENGTH)

    png_path = os.path.join(OUT_DIR, PNG_NAME)
    try:
        write_rgba_png(png_path, MAP_SIZE, MAP_SIZE, row_bytes)
    except OSError as error:
        print(f"[ERROR] PNGの書き込みに失敗しました: {png_path} ({error})", file=sys.stderr)
        raise

    file_size = os.path.getsize(png_path)
    print(f"size={MAP_SIZE}x{MAP_SIZE} octaves={NOISE_OCTAVES} strength={NORMAL_STRENGTH} noise_seed={NOISE_SEED}")
    print(f"file_bytes={file_size}")
    print(f"wrote {png_path}")


if __name__ == "__main__":
    main()
