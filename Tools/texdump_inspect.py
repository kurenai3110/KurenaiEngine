#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Sample3D の -dumptex が吐いた .bin (中間レンダーターゲットの生値) を読んで数値で調べる。

「コンパイルは通るが絵が違う」を、8bit・トーンマップ後のスクリーンショットではなく
**線形の生値**で切り分けるための道具。画面から採れるのは表示のために加工された姿だけで、
加工前を数えられないと「壊れている」と「そう見えるだけ」を区別できない。

使い方:
    python Tools/texdump_inspect.py header <dump.bin>
    python Tools/texdump_inspect.py stat   <dump.bin> [--rect x,y,w,h] [--channel rgba]
    python Tools/texdump_inspect.py px     <dump.bin> --at x,y [--at x,y ...] [--radius N]
    python Tools/texdump_inspect.py rect   <dump.bin> --rect x,y,w,h [--reduce mean|median|min|max]
    python Tools/texdump_inspect.py hist   <dump.bin> [--bins N] [--range lo,hi] [--log] [--channel g]
    python Tools/texdump_inspect.py row    <dump.bin> --y N [--x0 N --x1 N] [--step N]
    python Tools/texdump_inspect.py where  <dump.bin> --pred nan|inf|neg|zero|gt:V|lt:V [--limit N]
    python Tools/texdump_inspect.py diff   <a.bin> <b.bin> [--rel] [--rect x,y,w,h]
    python Tools/texdump_inspect.py noise  <連番*.bin> [--tile 16] [--offset Y,X]
                                           [--offset-sweep] [--channel rgba|r|g|b|a|luma]
                                           [--lit-threshold 1e-4]
    python Tools/texdump_inspect.py png    <dump.bin> -o out.png [--exposure F]
                                           [--channel rgb|r|g|b|a|len]
                                           [--mode linear|srgb|signed|falsecolor] [--range lo,hi]
    python Tools/texdump_inspect.py synth  <out.bin> --size WxH --ch N
                                           --pattern ramp|const:V|nan:N|checker
    python Tools/texdump_inspect.py selftest

**まず selftest を通してから使うこと。** 自分で作った物差しで自分の成果を測ると、
物差しの誤りに気づけない。synth が作る既知のパターンに当てて、期待どおりの値と順序を
返すことを先に確かめる。

出力の約束(どれも、記録された失敗の型に対応している):
  1. 全コマンドが先頭にヘッダ行を刷る。貼り付けた出力だけで「何をいつ測ったか」が分かる
  2. 件数には必ず母数を添える。「何本のうち何本か」を数えずに網羅性を語らない
  3. NaN/Inf は 0 でも刷る。「出ていない」と「見ていない」を区別する
  4. png は必ず stat の要約も刷る。「PNGを作って目視した」で終わる経路を残さない
"""

import argparse
import glob
import math
import os
import struct
import sys

import numpy as np

# === ファイル形式 (KurenaiEngine3D::WriteTextureDumpFile と一致させること) ===
#   off  size  内容
#     0    4   マジック 'K','T','X','D'
#     4    4   uint32 Version (=2。v1はBackend欄が無く、名前の位置も違うため拒否する)
#     8    4   uint32 HeaderBytes (=128。ピクセルデータはここから)
#    12    4   uint32 Width
#    16    4   uint32 Height
#    20    4   uint32 ChannelCount (1..4)
#    24    4   uint32 ElementType (1=UNorm8, 2=Float16, 3=Float32)
#    28    4   uint32 BytesPerElement (1/2/4)
#    32    4   uint32 FrameIndex
#    36    4   uint32 MipLevel
#    40    4   uint32 ArraySlice
#    44    4   uint32 Backend (1=DX11, 2=DX12)
#    48   64   char   SourceName[64] (NUL終端UTF-8)
#   112   16   予約(0)
#   128  ...   ピクセルデータ。行パディング無し、上から下・左から右、リトルエンディアン
MAGIC = b"KTXD"
HEADER_FIXED = struct.Struct("<4sIIIIIIIIIII")  # magic + uint32 x 11 = 48バイト
NAME_BYTES = 64
HEADER_BYTES = 128

# Backend の値 -> 表示名。0(未記録)も明示する。「分からない」を「DX11」と読ませない
BACKEND_NAMES = {0: "(未記録)", 1: "DX11", 2: "DX12"}

# ElementType -> (ファイル上のdtype, 表示名, 量子化の刻み幅の説明)
# 4 = R11G11B10_Float。**ファイル上は1テクセル uint32 1個**で、3成分への展開はこちらで行う
# (エンジン側で展開しないのは、手元のどのシーンでもこのフォーマットが全画素0で、
#  C++側に置くと一度も動かせないデコーダになるため。ここなら selftest で全ビットパターンを回せる)
ELEMENT_TYPES = {
    1: ("u1", "UNorm8", "1/255 = 0.003922"),
    2: ("<f2", "Float16", "半精度(相対 約 2^-11)"),
    3: ("<f4", "Float32", "単精度(相対 約 2^-24)"),
    4: ("<u4", "R11G11B10_Float", "R/Gは6bit仮数(相対 約2^-7)、Bは5bit仮数(相対 約2^-6)"),
}

PACKED_11_11_10 = 4

CHANNEL_LABELS = "RGBA"


def decode_float_bits(exponent, mantissa, mantissa_bits):
    """符号なし浮動小数(5bit指数 + N bit仮数、指数バイアス15)をfloat64へ展開する。

    R11G11B10_Float の3成分はどれもこの形(R/GはN=6、BはN=5)。
    指数バイアスがhalfと同じ15なので、halfから仮数のビット数だけを変えた式になる。
    **Inf/NaNを0で潰さない** —— NaNが出ていること自体が探している症状でありうる。

    exponent / mantissa は同じ形の numpy 配列。戻り値は float64 の配列
    """
    exponent = np.asarray(exponent, dtype=np.int64)
    mantissa = np.asarray(mantissa, dtype=np.int64)
    scale = float(1 << mantissa_bits)

    # 正規化数: 2^(e-15) * (1 + m / 2^N)
    normal = np.ldexp(1.0 + mantissa / scale, exponent - 15)
    # 非正規化数(e==0): 2^-14 * (m / 2^N)
    subnormal = np.ldexp(mantissa / scale, -14)
    # e==31: 仮数0ならInf、それ以外はNaN
    special = np.where(mantissa == 0, np.inf, np.nan)

    result = np.where(exponent == 0, subnormal, normal)
    return np.where(exponent == 31, special, result)


def unpack_r11g11b10(packed):
    """uint32 の配列を (…, 3) の float64 配列へ展開する。

    ビット配置(DXGI_FORMAT_R11G11B10_FLOAT): [31:22]=B(10bit) [21:11]=G(11bit) [10:0]=R(11bit)
    R/G は 5bit指数 + 6bit仮数、B は 5bit指数 + 5bit仮数。符号ビットは無い
    """
    packed = np.asarray(packed, dtype=np.uint32)
    r = (packed & np.uint32(0x7FF)).astype(np.int64)
    g = ((packed >> np.uint32(11)) & np.uint32(0x7FF)).astype(np.int64)
    b = ((packed >> np.uint32(22)) & np.uint32(0x3FF)).astype(np.int64)
    return np.stack(
        [
            decode_float_bits(r >> 6, r & 0x3F, 6),
            decode_float_bits(g >> 6, g & 0x3F, 6),
            decode_float_bits(b >> 5, b & 0x1F, 5),
        ],
        axis=-1,
    )


class Dump:
    """読み込んだダンプ1枚。data は (H, W, C) の numpy 配列"""

    def __init__(self, path, header, data):
        self.path = path
        self.name = header["name"]
        self.width = header["width"]
        self.height = header["height"]
        self.channels = header["channels"]
        self.element_type = header["element_type"]
        self.bytes_per_element = header["bytes_per_element"]
        self.frame_index = header["frame_index"]
        self.mip_level = header["mip_level"]
        self.array_slice = header["array_slice"]
        self.backend = header["backend"]
        self.data = data

    @property
    def backend_name(self):
        return BACKEND_NAMES.get(self.backend, "(不明:{})".format(self.backend))

    @property
    def type_name(self):
        return ELEMENT_TYPES[self.element_type][1]

    @property
    def quantization(self):
        return ELEMENT_TYPES[self.element_type][2]

    def as_float(self):
        """計算用に float64 へ広げる。UNorm8 は 0〜1 へ正規化する。

        **UNorm8 を 0〜255 のまま扱わない。** 他のフォーマットと同じ尺度に載せないと、
        diff の閾値やヒストグラムの範囲を型ごとに読み替えることになり、必ずどこかで間違える。
        """
        if self.element_type == 1:
            return self.data.astype(np.float64) / 255.0
        if self.element_type == PACKED_11_11_10:
            # ファイル上は1テクセル uint32 1個。ここで3成分へ展開する
            return unpack_r11g11b10(self.data[:, :, 0])
        return self.data.astype(np.float64)

    def header_text(self):
        # ElementType=4 は「1テクセル uint32 1個」で、他は「1成分あたりのバイト数」。単位を取り違えない
        unit = "バイト/テクセル" if self.element_type == PACKED_11_11_10 else "バイト/成分"
        return (
            "source : {name}  frame={frame}  {w}x{h}  ch={ch}  mip={mip}  slice={slice}\n"
            "format : {type} ({bpe}{unit})  量子化の刻み {quant}\n"
            "backend: {backend}\n"
            "file   : {path}"
        ).format(
            unit=unit,
            backend=self.backend_name,
            name=self.name or "(名前なし)",
            frame=self.frame_index,
            w=self.width,
            h=self.height,
            ch=self.channels,
            mip=self.mip_level,
            slice=self.array_slice,
            type=self.type_name,
            bpe=self.bytes_per_element,
            quant=self.quantization,
            path=self.path,
        )


def load(path):
    """ダンプを1枚読む。形式が違えば止める(黙って別物として読まない)"""
    if not os.path.isfile(path):
        raise SystemExit("ファイルがありません: {}".format(path))

    size = os.path.getsize(path)
    if size < HEADER_BYTES:
        raise SystemExit(
            "ヘッダより短いファイルです({}バイト。最低{}バイト必要): {}".format(size, HEADER_BYTES, path)
        )

    with open(path, "rb") as handle:
        raw = handle.read(HEADER_BYTES)

    (
        magic,
        version,
        header_bytes,
        width,
        height,
        channels,
        element_type,
        bytes_per_element,
        frame_index,
        mip_level,
        array_slice,
        backend,
    ) = HEADER_FIXED.unpack_from(raw, 0)

    if magic != MAGIC:
        raise SystemExit("マジックが一致しません(期待 {}, 実際 {}): {}".format(MAGIC, magic, path))
    if version == 1:
        # v1 は Backend 欄が無く、SourceName の位置が4バイト手前。そのまま読むと
        # 名前の先頭4文字がBackendとして解釈され、名前も4バイトずれる。
        # **黙って読まない** —— ずれた名前と意味のないBackendで判断させないため
        raise SystemExit(
            "v1 のダンプです。Backend欄が入る前の形式なので読めません(採り直してください): {}".format(path)
        )
    if version != 2:
        raise SystemExit("未知のバージョンです(v{}): {}".format(version, path))
    if element_type not in ELEMENT_TYPES:
        raise SystemExit("未知のElementTypeです({}): {}".format(element_type, path))
    if channels < 1 or channels > 4:
        raise SystemExit("チャンネル数が範囲外です({}): {}".format(channels, path))

    name = raw[HEADER_FIXED.size : HEADER_FIXED.size + NAME_BYTES]
    name = name.split(b"\0", 1)[0].decode("utf-8", errors="replace")

    dtype = ELEMENT_TYPES[element_type][0]
    # ElementType=4 だけは「1テクセル uint32 1個」で、ChannelCountは展開後の成分数を表す。
    # 長さの計算だけこの1点で分岐させ、それ以外はすべて共通の道を通す
    stored_channels = 1 if element_type == PACKED_11_11_10 else channels
    expected = width * height * stored_channels * bytes_per_element
    actual = size - header_bytes
    if actual != expected:
        # 途中で切れたファイルを黙って読むと、末尾だけ欠けた配列で統計を出してしまう
        raise SystemExit(
            "ピクセルデータの長さが合いません(期待 {}バイト, 実際 {}バイト): {}".format(
                expected, actual, path
            )
        )

    data = np.fromfile(path, dtype=dtype, offset=header_bytes).reshape(height, width, stored_channels)

    header = {
        "name": name,
        "width": width,
        "height": height,
        "channels": channels,
        "element_type": element_type,
        "bytes_per_element": bytes_per_element,
        "frame_index": frame_index,
        "mip_level": mip_level,
        "array_slice": array_slice,
        "backend": backend,
    }
    return Dump(path, header, data)


def parse_rect(text):
    parts = text.split(",")
    if len(parts) != 4:
        raise SystemExit("--rect は x,y,w,h の形で指定します: {}".format(text))
    try:
        x, y, w, h = (int(part) for part in parts)
    except ValueError:
        raise SystemExit("--rect の値が整数ではありません: {}".format(text))
    if w <= 0 or h <= 0:
        raise SystemExit("--rect の幅と高さは1以上です: {}".format(text))
    return x, y, w, h


def parse_range(text):
    parts = text.split(",")
    if len(parts) != 2:
        raise SystemExit("--range は lo,hi の形で指定します: {}".format(text))
    try:
        lo, hi = (float(part) for part in parts)
    except ValueError:
        raise SystemExit("--range の値が数値ではありません: {}".format(text))
    if not hi > lo:
        raise SystemExit("--range は lo < hi である必要があります: {}".format(text))
    return lo, hi


def crop(values, rect, width, height):
    """(H, W, C) を矩形で切る。画像内へ丸め、丸めたあとの矩形も返す"""
    if rect is None:
        return values, None
    x, y, w, h = rect
    x0 = max(0, min(x, width))
    y0 = max(0, min(y, height))
    x1 = max(x0, min(x + w, width))
    y1 = max(y0, min(y + h, height))
    if x1 == x0 or y1 == y0:
        raise SystemExit("--rect が画像の外です (画像は {}x{})".format(width, height))
    return values[y0:y1, x0:x1, :], (x0, y0, x1 - x0, y1 - y0)


def select_channels(dump, spec):
    """--channel の指定をチャンネル番号のリストへ直す"""
    if spec is None:
        return list(range(dump.channels))
    indices = []
    for ch in spec.lower():
        pos = "rgba".find(ch)
        if pos < 0:
            raise SystemExit("--channel に使えるのは r/g/b/a です: {}".format(spec))
        if pos >= dump.channels:
            raise SystemExit(
                "チャンネル {} はこのダンプにありません (ch={})".format(ch.upper(), dump.channels)
            )
        indices.append(pos)
    return indices


def count_line(count, total):
    """件数には必ず母数を添える。母数を数えずに網羅性を語らないため"""
    ratio = (100.0 * count / total) if total else 0.0
    return "{:,} / {:,} ({:.2f}%)".format(int(count), int(total), ratio)


def format_float(value):
    value = float(value)
    if np.isnan(value):
        return "NaN"
    if np.isinf(value):
        return "+Inf" if value > 0 else "-Inf"
    return "{:.6f}".format(value)


def finite_percentiles(values, percents):
    """非有限を除いた分位点。全部非有限なら NaN を返す(0で埋めない)"""
    finite = values[np.isfinite(values)]
    if finite.size == 0:
        return [float("nan")] * len(percents)
    return list(np.percentile(finite, percents))


# =============================================================================
# noise (連番ダンプから、時間ノイズの空間スケールを分けて測る)
# =============================================================================


def parse_offset(text, tile):
    parts = text.split(",")
    if len(parts) != 2:
        raise SystemExit("--offset は Y,X の形で指定します: {}".format(text))
    try:
        y, x = (int(part) for part in parts)
    except ValueError:
        raise SystemExit("--offset の値が整数ではありません: {}".format(text))
    if y < 0 or x < 0 or y >= tile or x >= tile:
        raise SystemExit("--offset は 0 以上 tile 未満です(tile={}): {}".format(tile, text))
    return y, x


def noise_channel(values, spec):
    """(H,W,C) を時間統計用の1成分へ選ぶ。戻り値は必ずfloat64。"""
    if spec == "luma":
        # RGBがあるものはRec.709で輝度化する。2ch以下をRGBと見なすと架空の成分を混ぜるため先頭だけ使う。
        if values.shape[2] >= 3:
            return np.tensordot(values[:, :, :3], np.array([0.2126, 0.7152, 0.0722]), axes=([2], [0]))
        return values[:, :, 0]
    if spec == "rgba":
        # rgba指定は、存在する全成分の算術平均で1本の尺度にする。RGB未満のダンプにも同じ指定を使える。
        return np.mean(values, axis=2, dtype=np.float64)
    index = "rgba".find(spec)
    if index < 0 or index >= values.shape[2]:
        raise SystemExit("--channel {} はこのダンプにありません (ch={})".format(spec, values.shape[2]))
    return values[:, :, index]


def summarize_noise(values, tile, offset):
    """タイル分解を計算する。表示と分離し、selftestも同じ測り方を通す。"""
    frames, height, width = values.shape
    offset_y, offset_x = offset
    tiles_y = (height - offset_y) // tile
    tiles_x = (width - offset_x) // tile
    if tiles_y == 0 or tiles_x == 0:
        raise SystemExit("--tile {} と --offset {},{} で完全なタイルがありません ({}x{})".format(
            tile, offset_y, offset_x, width, height))

    used_height = tiles_y * tile
    used_width = tiles_x * tile
    cropped = values[:, offset_y:offset_y + used_height, offset_x:offset_x + used_width]
    # x_p=m_T+r_p と明示して同じ配列から両成分を作る。別経路の丸め誤差を統計差と誤読させない。
    blocks = cropped.reshape(frames, tiles_y, tile, tiles_x, tile).transpose(0, 1, 3, 2, 4)
    tile_mean = np.mean(blocks, axis=(3, 4), dtype=np.float64)
    residual = blocks - tile_mean[:, :, :, None, None]
    between_var = np.var(tile_mean, axis=0, ddof=1, dtype=np.float64)
    residual_var = np.var(residual, axis=0, ddof=1, dtype=np.float64)
    inner_var = np.mean(residual_var, axis=(2, 3), dtype=np.float64)
    pixel_var = np.var(blocks, axis=0, ddof=1, dtype=np.float64)
    pixel_var_mean = float(np.mean(pixel_var, dtype=np.float64))
    decomposed_var = float(np.mean(between_var + inner_var, dtype=np.float64))
    identity_relative = abs(pixel_var_mean - decomposed_var) / max(abs(pixel_var_mean), 1e-300)

    # lag-1は画素ごとの時系列で測る。定数列は相関が定義できないのでNaNのまま報告する。
    before = blocks[:-1].reshape(frames - 1, -1)
    after = blocks[1:].reshape(frames - 1, -1)
    before = before - np.mean(before, axis=0, dtype=np.float64)
    after = after - np.mean(after, axis=0, dtype=np.float64)
    denominator = np.sqrt(np.sum(before * before, axis=0) * np.sum(after * after, axis=0))
    lag1 = np.full(denominator.shape, np.nan, dtype=np.float64)
    usable = denominator > 0.0
    lag1[usable] = np.sum(before[:, usable] * after[:, usable], axis=0) / denominator[usable]

    return {
        "tiles_y": tiles_y, "tiles_x": tiles_x,
        "used_pixels": tiles_y * tiles_x * tile * tile,
        "total_pixels": height * width,
        "between_std": np.sqrt(between_var),
        # 【タイル内は画素で集約する】確立した定義(61.7m.1)は
        # 「画素からそのフレームのタイル平均を引いた残差の時間std」の**全画素の中央値**。
        # タイル内でいったん分散を平均してからタイルの中央値を取ると別の統計量になり、
        # 実測で系統的に高く出た(1280x720/K=32 で 46.98 のところ 58.59)。
        # 恒等式の検算だけは平均どうしで行うので inner_var(タイル平均)も残す
        "residual_std": np.sqrt(residual_var),
        "inner_std": np.sqrt(inner_var),
        "pixel_std": np.sqrt(pixel_var).reshape(-1),
        "lag1": lag1,
        "tile_time_mean": np.mean(tile_mean, axis=0, dtype=np.float64),
        "pixel_var_mean": pixel_var_mean,
        "decomposed_var": decomposed_var,
        "identity_relative": identity_relative,
    }


def noise_percentiles(values):
    return finite_percentiles(np.asarray(values, dtype=np.float64), [50, 90])


def print_noise_summary(summary, lit_threshold):
    total_tiles = summary["between_std"].size
    unlit = summary["tile_time_mean"] <= lit_threshold
    print("タイル   : {} x {} = {:,}  完全タイルの画素 {:,} / {:,} (除外 {})".format(
        summary["tiles_x"], summary["tiles_y"], total_tiles, summary["used_pixels"], summary["total_pixels"],
        count_line(summary["total_pixels"] - summary["used_pixels"], summary["total_pixels"])))
    print("未点灯   : 時間平均 <= {:.6g} のタイル {}".format(lit_threshold, count_line(np.count_nonzero(unlit), total_tiles)))
    # タイル間はタイルで、タイル内は画素で集約する(61.7m.1 の定義)。
    # 未点灯の除外は**タイル単位**で行う ―― 画素を間引くとタイルが欠けて恒等式が破れる
    between_all = noise_percentiles(summary["between_std"])
    between_lit = noise_percentiles(summary["between_std"][~unlit])
    residual = summary["residual_std"]
    inner_all = noise_percentiles(residual)
    inner_lit = noise_percentiles(residual[~unlit])
    print("{:<8}: 全タイル median={} p90={}  未点灯除外 median={} p90={}".format(
        "タイル間", format_float(between_all[0]), format_float(between_all[1]),
        format_float(between_lit[0]), format_float(between_lit[1])))
    print("{:<8}: 全画素   median={} p90={}  未点灯除外 median={} p90={}".format(
        "タイル内", format_float(inner_all[0]), format_float(inner_all[1]),
        format_float(inner_lit[0]), format_float(inner_lit[1])))
    # 合成は「報告した2つの中央値」から作る(61.7m の表の作り方。√(4.03²+8.30²)=9.22 と一致する)
    print("{:<8}: 全体 median={}  未点灯除外 median={}   ← √(タイル間²+タイル内²)".format(
        "合成", format_float(math.hypot(between_all[0], inner_all[0])),
        format_float(math.hypot(between_lit[0], inner_lit[0]))))
    pixel_p50, pixel_p90 = noise_percentiles(summary["pixel_std"])
    lag_p50, _ = noise_percentiles(summary["lag1"])
    print("画素時間std: median={} p90={}".format(format_float(pixel_p50), format_float(pixel_p90)))
    print("lag-1自己相関: 画素 {:,} 中 median={}".format(summary["lag1"].size, format_float(lag_p50)))
    print("恒等式   : mean_p Var(x_p)={:.12g}  Var(m_T)+mean_p Var(r_p)={:.12g}  相対偏差={:.3e}".format(
        summary["pixel_var_mean"], summary["decomposed_var"], summary["identity_relative"]))
    if not np.isfinite(summary["identity_relative"]) or summary["identity_relative"] > 1e-9:
        print("!! 恒等式の相対偏差が 1e-9 を超えました。実装または非有限値の扱いを疑ってください")


def cmd_noise(args):
    print("=== noise ===")
    if args.tile <= 0:
        raise SystemExit("--tile は1以上です: {}".format(args.tile))
    paths = []
    for pattern in args.paths:
        matches = sorted(glob.glob(pattern))
        if not matches:
            raise SystemExit("一致するファイルがありません: {}".format(pattern))
        paths.extend(matches)
    # 同じ引数を二度渡しても二重に平均しない。実体は絶対パスで比較する。
    unique_paths = []
    seen_paths = set()
    for path in paths:
        absolute = os.path.abspath(path)
        if absolute not in seen_paths:
            seen_paths.add(absolute)
            unique_paths.append(path)
    if len(unique_paths) < 3:
        raise SystemExit("noise は3枚以上必要です (実際 {} 枚)".format(len(unique_paths)))
    dumps = [load(path) for path in unique_paths]
    dumps.sort(key=lambda dump: dump.frame_index)
    first = dumps[0]
    fields = (("width", "幅"), ("height", "高さ"), ("channels", "チャンネル数"),
              ("element_type", "要素型"), ("name", "SourceName"))
    mismatches = []
    for dump in dumps[1:]:
        for field, label in fields:
            if getattr(dump, field) != getattr(first, field):
                mismatches.append("{}: {} ({}) != {} ({})".format(
                    label, dump.path, getattr(dump, field), first.path, getattr(first, field)))
    if mismatches:
        raise SystemExit("連番に一致しないヘッダがあります:\n  " + "\n  ".join(mismatches))

    print("source : {}  {}枚  {}x{} ch={} {}".format(
        first.name or "(名前なし)", len(dumps), first.width, first.height, first.channels, first.type_name))
    print("単位     : 線形の生値（階調・8bit値ではない）")
    frame_indices = [dump.frame_index for dump in dumps]
    intervals = np.diff(frame_indices)
    print("FrameIndex: " + ", ".join(str(value) for value in frame_indices))
    if np.all(intervals == intervals[0]):
        print("撮影間隔 : {} フレーム（隣接 {:,} 組、一定）".format(int(intervals[0]), intervals.size))
    else:
        print("撮影間隔 : 一定ではない（隣接差分: {}）".format(", ".join(str(int(value)) for value in intervals)))

    identical = []
    payloads = [dump.data.tobytes() for dump in dumps]
    for right in range(1, len(dumps)):
        for left in range(right):
            if payloads[left] == payloads[right]:
                identical.append((dumps[left].path, dumps[right].path))
    print("ビット同一: {}".format(count_line(len(identical), len(dumps) * (len(dumps) - 1) // 2)))
    if identical:
        print("!! 同じフレームを重複捕獲した可能性があります")
        for left, right in identical:
            print("  {} == {}".format(left, right))

    values = np.stack([noise_channel(dump.as_float(), args.channel) for dump in dumps], axis=0).astype(np.float64)
    print_nonfinite(values, "入力非有限")
    offset = parse_offset(args.offset, args.tile)
    summary = summarize_noise(values, args.tile, offset)
    print("channel  : {}  tile={}  offset={},{}".format(args.channel, args.tile, offset[0], offset[1]))
    print_noise_summary(summary, args.lit_threshold)
    if args.offset_sweep:
        # 【最大値が主役】格子をフレームごとにずらす手法を評価するとき、固定オフセットの
        # タイル間は**必ず**下がる ―― 測る格子が相手の格子と合わなくなるだけで、
        # 中身は何も良くなっていない。**全オフセットでの最大**なら、単なる位相のずれは
        # どこかのオフセットで再整列するので動かず、ブロック構造が本当に壊れたときだけ下がる。
        # 合成データで確認済み(格子固定 0.9866@(0,0) / 格子ジッタ 0.6994@(12,10)、画素stdは同等)。
        #
        # 刻みを粗くすると最大を取り逃がす(上の例の最大は t/4 刻みの格子に載っていない)ので、
        # 既定では全オフセットを見る。--sweep-step で粗くできるが、そのときは最大が
        # 下振れしうることを承知して使うこと。
        step = max(1, args.sweep_step)
        offsets = list(range(0, args.tile, step))
        sweep = []
        best = None
        for y in offsets:
            for x in offsets:
                result = summarize_noise(values, args.tile, (y, x))
                value = noise_percentiles(result["between_std"])[0]
                sweep.append(value)
                if best is None or value > best[0]:
                    best = (value, (y, x))
        print("offset-sweep（タイル間median、{}通り / 刻み{}）: min={} median={} max={} at offset {},{}".format(
            len(sweep), step, format_float(np.nanmin(sweep)), format_float(np.nanmedian(sweep)),
            format_float(best[0]), best[1][0], best[1][1]))
        print("  ← 格子をずらす手法の評価には **max** を使うこと(固定オフセットの値は位相で下がる)")
    return 0


# =============================================================================
# header
# =============================================================================


def cmd_header(args):
    dump = load(args.path)
    print("=== header ===")
    print(dump.header_text())
    print(
        "bytes  : ヘッダ {} + データ {:,} = {:,}".format(
            HEADER_BYTES, dump.data.nbytes, HEADER_BYTES + dump.data.nbytes
        )
    )
    return 0


# =============================================================================
# stat
# =============================================================================


def print_nonfinite(values, label="非有限"):
    """NaN/Inf は 0 でも刷る。「出ていない」と「見ていない」を区別するため"""
    nan_count = int(np.count_nonzero(np.isnan(values)))
    pinf_count = int(np.count_nonzero(np.isposinf(values)))
    ninf_count = int(np.count_nonzero(np.isneginf(values)))
    print(
        "{:<7}: NaN={:,}  +Inf={:,}  -Inf={:,}    (要素 {:,} 中)".format(
            label, nan_count, pinf_count, ninf_count, values.size
        )
    )
    return nan_count + pinf_count + ninf_count


def print_stat_table(values, channels, labels):
    """チャンネル別の統計表。==0 と <0 の列は常に出す。

    **値の受け渡しが切れる型のバグは、典型的に全画素0か全画素定数になる。**
    ゼロ数と min==max の一致だけで判定できるので、オプションにせず毎回出す。
    """
    print(
        "{:<5} {:<10} {:<10} {:<10} {:<10} {:<10} {:<10} {:<12} {:<10}".format(
            "ch", "min", "p1", "median", "mean", "p99", "max", "==0", "<0"
        )
    )
    for index, ch in enumerate(channels):
        column = values[:, :, index].ravel()
        finite = column[np.isfinite(column)]
        if finite.size == 0:
            lo = hi = mean = med = p1 = p99 = float("nan")
        else:
            lo = float(np.min(finite))
            hi = float(np.max(finite))
            mean = float(np.mean(finite))
            p1, med, p99 = finite_percentiles(column, [1, 50, 99])
        zero_count = int(np.count_nonzero(column == 0.0))
        neg_count = int(np.count_nonzero(column < 0.0))
        print(
            "{:<5} {:<10} {:<10} {:<10} {:<10} {:<10} {:<10} {:<12,} {:<10,}".format(
                labels[ch],
                format_float(lo),
                format_float(p1),
                format_float(med),
                format_float(mean),
                format_float(p99),
                format_float(hi),
                zero_count,
                neg_count,
            )
        )


def report_constant(values, channels, labels):
    """全画素が同じ値なら強く知らせる。単発で最も価値の高い自動所見"""
    flat = values.reshape(-1, values.shape[2])
    finite_mask = np.isfinite(flat).all(axis=1)
    if not finite_mask.any():
        return
    finite = flat[finite_mask]
    first = finite[0]
    if not np.all(finite == first):
        return
    parts = " ".join(
        "{}={}".format(labels[ch], format_float(first[i])) for i, ch in enumerate(channels)
    )
    print("")
    print("!! 定数化: 全画素が同じ値 ({})".format(parts))
    print("   上流が書いていない / バインドが違う / 定数を焼き込んだままの可能性。")
    print("   まず「そのパスが実際に走ったか」をログで確かめること")


def cmd_stat(args):
    dump = load(args.path)
    channels = select_channels(dump, args.channel)
    labels = CHANNEL_LABELS

    values = dump.as_float()[:, :, channels]
    rect = parse_rect(args.rect) if args.rect else None
    values, clamped = crop(values, rect, dump.width, dump.height)

    print("=== stat ===")
    print(dump.header_text())
    if clamped:
        print(
            "rect   : x={} y={} w={} h={}  (画素 {:,})".format(
                clamped[0], clamped[1], clamped[2], clamped[3], clamped[2] * clamped[3]
            )
        )
    else:
        print("rect   : 全画面  (画素 {:,})".format(dump.width * dump.height))
    print_nonfinite(values)
    print("")
    print_stat_table(values, channels, labels)

    # 法線や色は「長さが1か」を毎回見たいので、3成分以上あるときは自動で足す
    if len(channels) >= 3:
        rgb = values[:, :, 0:3]
        length = np.sqrt(np.sum(np.square(rgb), axis=2)).ravel()
        finite = length[np.isfinite(length)]
        if finite.size:
            p1, med, p99 = finite_percentiles(length, [1, 50, 99])
            print(
                "{:<5} {:<10} {:<10} {:<10} {:<10} {:<10} {:<10}".format(
                    "len3",
                    format_float(np.min(finite)),
                    format_float(p1),
                    format_float(med),
                    format_float(np.mean(finite)),
                    format_float(p99),
                    format_float(np.max(finite)),
                )
            )

    report_constant(values, channels, labels)
    return 0


# =============================================================================
# px / rect / row
# =============================================================================


def cmd_px(args):
    dump = load(args.path)
    values = dump.as_float()
    labels = CHANNEL_LABELS

    print("=== px ===")
    print(dump.header_text())
    radius = max(0, args.radius)
    if radius > 0:
        # 1画素だとノイズやディザに引っ張られる。半径を取って平均する
        print("radius : {} ({}x{} の平均も出す)".format(radius, radius * 2 + 1, radius * 2 + 1))
    print("")

    for spec in args.at:
        parts = spec.split(",")
        if len(parts) != 2:
            raise SystemExit("--at は x,y の形で指定します: {}".format(spec))
        try:
            x, y = (int(part) for part in parts)
        except ValueError:
            raise SystemExit("--at の値が整数ではありません: {}".format(spec))
        if not (0 <= x < dump.width and 0 <= y < dump.height):
            print("({},{}) 範囲外 (画像は {}x{})".format(x, y, dump.width, dump.height))
            continue

        texel = values[y, x, :]
        parts_text = "  ".join(
            "{}={}".format(labels[ch], format_float(texel[ch])) for ch in range(dump.channels)
        )
        line = "({},{}) r=0  {}".format(x, y, parts_text)
        if dump.channels >= 3:
            length = float(np.sqrt(np.sum(np.square(texel[0:3]))))
            line += "  |rgb|={}".format(format_float(length))
        print(line)

        if radius > 0:
            x0 = max(0, x - radius)
            y0 = max(0, y - radius)
            x1 = min(dump.width, x + radius + 1)
            y1 = min(dump.height, y + radius + 1)
            block = values[y0:y1, x0:x1, :]
            finite_mask = np.isfinite(block).all(axis=2)
            total = block.shape[0] * block.shape[1]
            if not finite_mask.any():
                print("      r={} 平均は取れません(有限な画素が0/{})".format(radius, total))
                continue
            mean = block[finite_mask].mean(axis=0)
            parts_text = "  ".join(
                "{}={}".format(labels[ch], format_float(mean[ch])) for ch in range(dump.channels)
            )
            print(
                "      r={} ({}x{}平均)  {}   (有限 {}/{})".format(
                    radius,
                    block.shape[1],
                    block.shape[0],
                    parts_text,
                    int(np.count_nonzero(finite_mask)),
                    total,
                )
            )
    return 0


def cmd_rect(args):
    dump = load(args.path)
    channels = select_channels(dump, args.channel)
    labels = CHANNEL_LABELS

    values = dump.as_float()[:, :, channels]
    rect = parse_rect(args.rect)
    values, clamped = crop(values, rect, dump.width, dump.height)

    print("=== rect ===")
    print(dump.header_text())
    print(
        "rect   : x={} y={} w={} h={}  (画素 {:,})".format(
            clamped[0], clamped[1], clamped[2], clamped[3], clamped[2] * clamped[3]
        )
    )
    print_nonfinite(values)
    print("")

    reducers = {
        "mean": np.mean,
        "median": np.median,
        "min": np.min,
        "max": np.max,
        "sum": np.sum,
    }
    reduce_fn = reducers[args.reduce]
    for index, ch in enumerate(channels):
        column = values[:, :, index].ravel()
        finite = column[np.isfinite(column)]
        if finite.size == 0:
            print("{} {} = 取れません(有限な画素が0)".format(labels[ch], args.reduce))
            continue
        print(
            "{} {:<7} = {}   (有限 {})".format(
                labels[ch], args.reduce, format_float(reduce_fn(finite)), count_line(finite.size, column.size)
            )
        )
    return 0


def cmd_row(args):
    """断面プロファイル。領域平均では原理的に見えない量(ハイライト中心と裾の配分など)がある"""
    dump = load(args.path)
    channels = select_channels(dump, args.channel)
    labels = CHANNEL_LABELS
    values = dump.as_float()

    if not (0 <= args.y < dump.height):
        raise SystemExit("--y が範囲外です ({} / 高さ {})".format(args.y, dump.height))

    x0 = max(0, args.x0)
    x1 = dump.width if args.x1 < 0 else min(dump.width, args.x1)
    if x1 <= x0:
        raise SystemExit("--x0 / --x1 の範囲が空です ({} 〜 {})".format(x0, x1))
    step = max(1, args.step)

    print("=== row ===")
    print(dump.header_text())
    print("row    : y={}  x={}〜{}  step={}  ({} 点)".format(args.y, x0, x1 - 1, step, len(range(x0, x1, step))))
    print("")
    print("{:<8} {}".format("x", "  ".join("{:<10}".format(labels[ch]) for ch in channels)))
    for x in range(x0, x1, step):
        texel = values[args.y, x, :]
        print(
            "{:<8} {}".format(
                x, "  ".join("{:<10}".format(format_float(texel[ch])) for ch in channels)
            )
        )
    return 0


# =============================================================================
# hist
# =============================================================================


def cmd_hist(args):
    dump = load(args.path)
    channels = select_channels(dump, args.channel)
    labels = CHANNEL_LABELS

    values = dump.as_float()[:, :, channels]
    rect = parse_rect(args.rect) if args.rect else None
    values, clamped = crop(values, rect, dump.width, dump.height)

    print("=== hist ===")
    print(dump.header_text())
    if clamped:
        print("rect   : x={} y={} w={} h={}".format(clamped[0], clamped[1], clamped[2], clamped[3]))
    print_nonfinite(values)

    flat = values.ravel()
    finite = flat[np.isfinite(flat)]
    if finite.size == 0:
        print("有限な値がありません")
        return 0

    if args.range:
        lo, hi = parse_range(args.range)
    else:
        lo = float(np.min(finite))
        hi = float(np.max(finite))
        if hi <= lo:
            hi = lo + 1.0
    print("range  : [{}, {}]  bins={}".format(format_float(lo), format_float(hi), args.bins))
    print("")

    counts, edges = np.histogram(finite, bins=args.bins, range=(lo, hi))
    peak = int(counts.max()) if counts.size else 0
    for i in range(len(counts)):
        count = int(counts[i])
        if args.log:
            width = 0 if count == 0 else int(40.0 * np.log10(count + 1) / np.log10(peak + 1))
        else:
            width = 0 if peak == 0 else int(40.0 * count / peak)
        print(
            "[{:>10} , {:>10}) {:<24} {}".format(
                format_float(edges[i]), format_float(edges[i + 1]), "#" * width, count_line(count, finite.size)
            )
        )

    below = int(np.count_nonzero(finite < lo))
    above = int(np.count_nonzero(finite > hi))
    print("")
    print("範囲より下 : {}".format(count_line(below, finite.size)))
    print("範囲より上 : {}".format(count_line(above, finite.size)))
    return 0


# =============================================================================
# where
# =============================================================================


def cmd_where(args):
    """壊れている「場所の座標」を返す。

    stat で「NaNが143個」まで分かっても座標へ行けなければ手が止まる。
    目視で場所を探す作業を機械へ置き換えるための中核。
    """
    dump = load(args.path)
    channels = select_channels(dump, args.channel)
    labels = CHANNEL_LABELS
    values = dump.as_float()[:, :, channels]

    pred = args.pred.lower()
    if pred == "nan":
        mask = np.isnan(values)
        description = "NaN"
    elif pred == "inf":
        mask = np.isinf(values)
        description = "Inf"
    elif pred == "neg":
        mask = values < 0.0
        description = "< 0"
    elif pred == "zero":
        mask = values == 0.0
        description = "== 0"
    elif pred.startswith("gt:") or pred.startswith("lt:"):
        try:
            threshold = float(pred[3:])
        except ValueError:
            raise SystemExit("--pred のしきい値が数値ではありません: {}".format(args.pred))
        if pred.startswith("gt:"):
            mask = values > threshold
            description = "> {}".format(threshold)
        else:
            mask = values < threshold
            description = "< {}".format(threshold)
    else:
        raise SystemExit("--pred は nan/inf/neg/zero/gt:V/lt:V のいずれかです: {}".format(args.pred))

    print("=== where ===")
    print(dump.header_text())
    print("pred   : {}  (対象チャンネル {})".format(description, "".join(labels[ch] for ch in channels)))

    # チャンネルのどれかが該当する画素
    pixel_mask = mask.any(axis=2)
    total_pixels = dump.width * dump.height
    print("該当画素 : {}".format(count_line(np.count_nonzero(pixel_mask), total_pixels)))
    for index, ch in enumerate(channels):
        print(
            "  {} : {}".format(
                labels[ch], count_line(np.count_nonzero(mask[:, :, index]), total_pixels)
            )
        )

    ys, xs = np.nonzero(pixel_mask)
    if ys.size == 0:
        print("")
        print("該当なし。**「見ていない」ではなく「出ていない」ことの確認になっている**")
        return 0

    print("")
    print(
        "位置の範囲 : x={}〜{}  y={}〜{}".format(int(xs.min()), int(xs.max()), int(ys.min()), int(ys.max()))
    )
    limit = min(args.limit, ys.size)
    print("先頭 {} 件:".format(limit))
    full = dump.as_float()
    for i in range(limit):
        x = int(xs[i])
        y = int(ys[i])
        texel = full[y, x, :]
        parts_text = "  ".join(
            "{}={}".format(labels[ch], format_float(texel[ch])) for ch in range(dump.channels)
        )
        print("  ({},{})  {}".format(x, y, parts_text))
    if ys.size > limit:
        print("  ... 残り {:,} 件".format(int(ys.size - limit)))
    return 0


# =============================================================================
# diff
# =============================================================================


def cmd_diff(args):
    left = load(args.a)
    right = load(args.b)

    print("=== diff ===")
    print("A: {}".format(left.path))
    print(
        "   {} {}x{} ch={} frame={} {} mip={} slice={} backend={}".format(
            left.name, left.width, left.height, left.channels, left.frame_index,
            left.type_name, left.mip_level, left.array_slice, left.backend_name,
        )
    )
    print("B: {}".format(right.path))
    print(
        "   {} {}x{} ch={} frame={} {} mip={} slice={} backend={}".format(
            right.name, right.width, right.height, right.channels, right.frame_index,
            right.type_name, right.mip_level, right.array_slice, right.backend_name,
        )
    )
    # 【バックエンドをはっきり書く】DX11とDX12のダンプは一致していればヘッダまでバイト一致し、
    # ファイルからは出所が分からなくなる。「別々のバックエンドで採った」ことを
    # ファイル自身に自己申告させ、同一バックエンド同士の比較を片肺の検証と誤読させない
    if left.backend == 0 or right.backend == 0:
        # RenderDoc経由の書き出し(renderdoc_probe.py export)もここに来る。
        # **「分からない」を「片方はDX11だろう」と読ませない**
        print("→ **どちらかにバックエンドが記録されていません** ({} vs {})。".format(
            left.backend_name, right.backend_name))
        print("   DX11とDX12の一致を示す根拠にはこの組み合わせを使わないこと")
    elif left.backend != right.backend:
        print("→ 別バックエンド同士の比較です ({} vs {})".format(left.backend_name, right.backend_name))
    else:
        print("→ 同じバックエンド({})同士の比較です".format(left.backend_name))

    mismatches = []
    if (left.width, left.height) != (right.width, right.height):
        mismatches.append("寸法")
    if left.channels != right.channels:
        mismatches.append("チャンネル数")
    if left.element_type != right.element_type:
        mismatches.append("要素型")
    if left.name != right.name:
        mismatches.append("source名")

    if mismatches:
        # 形の違うものを黙って比較させない
        print("")
        print("!! 一致しない項目: {}".format(" / ".join(mismatches)))
        if not args.force:
            print("   --force を付けない限り比較しません(別物どうしの差分は意味を持たないため)")
            return 2
        print("   --force が指定されたので続行します")
        if (left.width, left.height) != (right.width, right.height) or left.channels != right.channels:
            print("   ただし寸法かチャンネル数が違うため、要素ごとの比較はできません")
            return 2
    else:
        print("形状・チャンネル・要素型・source名: 一致")

    a_values = left.as_float()
    b_values = right.as_float()
    rect = parse_rect(args.rect) if args.rect else None
    a_values, clamped = crop(a_values, rect, left.width, left.height)
    b_values, _ = crop(b_values, rect, right.width, right.height)
    if clamped:
        print("rect   : x={} y={} w={} h={}".format(clamped[0], clamped[1], clamped[2], clamped[3]))

    print("")
    a_nonfinite = print_nonfinite(a_values, "A非有限")
    b_nonfinite = print_nonfinite(b_values, "B非有限")

    delta = b_values - a_values
    # 非有限どうしの引き算はNaNになる。**それを「差あり」と数えない**
    # (両方NaNの画素は「同じ」だが、片方だけNaNなら本物の差なので別に数える)
    both_nan = np.isnan(a_values) & np.isnan(b_values)
    delta = np.where(both_nan, 0.0, delta)
    one_side_nonfinite = int(
        np.count_nonzero(np.isfinite(a_values) != np.isfinite(b_values))
    )

    total = delta.size
    finite_delta = delta[np.isfinite(delta)]
    abs_delta = np.abs(finite_delta)

    # 相対的な刻み幅。「量子化で消えた差」と「本物の差」を分けるためのしきい値
    quantum = {1: 1.0 / 255.0, 2: 2.0 ** -11, 3: 2.0 ** -24, PACKED_11_11_10: 2.0 ** -6}[
        left.element_type
    ]
    print("")
    print("片側だけ非有限 : {}".format(count_line(one_side_nonfinite, total)))
    print("|d| > 0        : {}".format(count_line(np.count_nonzero(abs_delta > 0.0), total)))
    print("|d| > 1e-6     : {}".format(count_line(np.count_nonzero(abs_delta > 1e-6), total)))
    print("|d| > 1e-3     : {}".format(count_line(np.count_nonzero(abs_delta > 1e-3), total)))
    print(
        "|d| > 量子化刻み : {}   (刻み {})".format(
            count_line(np.count_nonzero(abs_delta > quantum), total), format_float(quantum)
        )
    )

    changed = finite_delta[abs_delta > 0.0]
    if changed.size:
        greater = int(np.count_nonzero(changed > 0))
        less = int(np.count_nonzero(changed < 0))
        # 【符号の内訳】偏りの符号で候補を排除する型を、毎回自動で出す。
        # 「暗くする向きにしか働けない候補」は、明るい側へ偏った差の主因ではありえない
        bias = ""
        if greater == 0 or less == 0:
            bias = "   <- 完全に片側(系統差)"
        elif max(greater, less) / float(changed.size) > 0.9:
            bias = "   <- 偏りあり"
        print(
            "符号の内訳(差のある要素のみ): B>A {:,} ({:.1f}%) / B<A {:,} ({:.1f}%){}".format(
                greater,
                100.0 * greater / changed.size,
                less,
                100.0 * less / changed.size,
                bias,
            )
        )
        p50, p90, p99 = np.percentile(np.abs(changed), [50, 90, 99])
        print(
            "|d| 分位点 p50/p90/p99/max : {} / {} / {} / {}".format(
                format_float(p50), format_float(p90), format_float(p99), format_float(np.max(np.abs(changed)))
            )
        )

        flat_index = int(np.argmax(np.abs(np.where(np.isfinite(delta), delta, 0.0))))
        y, x, ch = np.unravel_index(flat_index, delta.shape)
        offset_x = clamped[0] if clamped else 0
        offset_y = clamped[1] if clamped else 0
        a_text = " ".join(format_float(v) for v in a_values[y, x, :])
        b_text = " ".join(format_float(v) for v in b_values[y, x, :])
        print(
            "最大差の位置 : (x={}, y={}, ch={})  A=({})  B=({})".format(
                int(x) + offset_x, int(y) + offset_y, CHANNEL_LABELS[int(ch)], a_text, b_text
            )
        )

    print("")
    print("ch別 |d| 平均:")
    for ch in range(left.channels):
        column = delta[:, :, ch]
        finite = np.abs(column[np.isfinite(column)])
        mean = float(np.mean(finite)) if finite.size else float("nan")
        note = "   <- 1ビットも動いていない" if finite.size and np.max(finite) == 0.0 else ""
        print("  {} : {}{}".format(CHANNEL_LABELS[ch], format_float(mean), note))

    if args.rel:
        denom = np.maximum(np.abs(a_values), args.eps)
        rel = np.abs(np.where(np.isfinite(delta), delta, 0.0)) / denom
        rel_finite = rel[np.isfinite(rel)]
        if rel_finite.size:
            p50, p90, p99 = np.percentile(rel_finite, [50, 90, 99])
            print("")
            print(
                "相対差 p50/p90/p99/max : {} / {} / {} / {}   (分母は max(|A|, {}))".format(
                    format_float(p50),
                    format_float(p90),
                    format_float(p99),
                    format_float(np.max(rel_finite)),
                    args.eps,
                )
            )

    if changed.size == 0 and one_side_nonfinite == 0:
        print("")
        print("差はゼロ(1ビットも違わない)。")
        print("**「片方が実行されていない」可能性を先に潰すこと。**")
        print("  - シェーダを変えたなら、ビルドログで .kshader の焼き直しが走ったかを確認する")
        print("  - わざと大きく壊した版で差が出ることを先に確かめる(逆向きの対照)")
    return 0


# =============================================================================
# png
# =============================================================================


def cmd_png(args):
    try:
        from PIL import Image
    except ImportError:
        raise SystemExit("png には Pillow が要ります: pip install pillow")

    dump = load(args.path)
    values = dump.as_float()

    if args.channel == "len":
        if dump.channels < 3:
            raise SystemExit("--channel len は3チャンネル以上のダンプにだけ使えます")
        gray = np.sqrt(np.sum(np.square(values[:, :, 0:3]), axis=2))
        rgb = np.dstack([gray, gray, gray])
    elif args.channel in ("r", "g", "b", "a"):
        index = "rgba".find(args.channel)
        if index >= dump.channels:
            raise SystemExit(
                "チャンネル {} はこのダンプにありません (ch={})".format(args.channel.upper(), dump.channels)
            )
        gray = values[:, :, index]
        rgb = np.dstack([gray, gray, gray])
    else:
        if dump.channels >= 3:
            rgb = values[:, :, 0:3]
        else:
            gray = values[:, :, 0]
            rgb = np.dstack([gray, gray, gray])

    # 【非有限は色に混ぜない】NaN/Infをそのままuint8へキャストすると値が未定義になり、
    # 「たまたまそれらしい色」で塗られて見逃す。このリポジトリの表示規約に合わせてマゼンタで塗り、
    # 件数も刷る(Present.hlslがライトタイルの容量超過やbent normal欠損をマゼンタにするのと同じ)
    nonfinite_mask = ~np.isfinite(rgb).all(axis=2)
    nonfinite_count = int(np.count_nonzero(nonfinite_mask))

    mode = args.mode
    if mode == "signed":
        # 0 を中間灰、正=赤、負=青。符号・座標系の間違いを1枚で見えるようにする唯一のモード
        scale = args.exposure if args.exposure > 0.0 else 1.0
        # 非有限はこのあとマゼンタで上書きするので、ここでは0にしてキャストの未定義を避ける
        normalized = np.clip(np.nan_to_num(rgb * scale, nan=0.0, posinf=1.0, neginf=-1.0), -1.0, 1.0)
        out = np.zeros(rgb.shape, dtype=np.float64)
        out[:, :, 0] = 0.5 + 0.5 * np.clip(normalized[:, :, 0], 0.0, 1.0)
        out[:, :, 2] = 0.5 + 0.5 * np.clip(-normalized[:, :, 0], 0.0, 1.0)
        out[:, :, 1] = 0.5
        rgb8 = np.clip(out * 255.0, 0, 255).astype(np.uint8)
    elif mode == "falsecolor":
        if not args.range:
            raise SystemExit(
                "--mode falsecolor には --range lo,hi が要ります。"
                "自動レンジだと2枚のPNGが別の尺度になり、比較そのものが壊れるため"
            )
        lo, hi = parse_range(args.range)
        gray = rgb[:, :, 0]
        t = np.clip(np.nan_to_num((gray - lo) / (hi - lo), nan=0.0, posinf=1.0, neginf=0.0), 0.0, 1.0)
        # 青 -> 緑 -> 赤 の3点補間。ライブラリを増やさずに済ませる
        red = np.clip(2.0 * t - 1.0, 0.0, 1.0)
        blue = np.clip(1.0 - 2.0 * t, 0.0, 1.0)
        green = 1.0 - red - blue
        rgb8 = np.clip(np.dstack([red, green, blue]) * 255.0, 0, 255).astype(np.uint8)
        print("falsecolor range : [{}, {}]".format(format_float(lo), format_float(hi)))
    else:
        exposed = rgb * args.exposure
        if mode == "srgb":
            exposed = np.clip(exposed, 0.0, 1.0)
            exposed = np.where(
                exposed <= 0.0031308, exposed * 12.92, 1.055 * np.power(exposed, 1.0 / 2.4) - 0.055
            )
        rgb8 = np.clip(np.nan_to_num(exposed, nan=0.0, posinf=1.0, neginf=0.0) * 255.0, 0, 255).astype(
            np.uint8
        )

    if nonfinite_count:
        rgb8[nonfinite_mask] = np.array([255, 0, 255], dtype=np.uint8)

    Image.fromarray(rgb8, mode="RGB").save(args.out)

    # 【必ず統計も刷る】「PNGを作って目視した」で終わる経路を残さない
    print("=== png ===")
    print(dump.header_text())
    print("mode   : {}  channel={}  exposure={}".format(mode, args.channel, args.exposure))
    print("out    : {}".format(args.out))
    print(
        "非有限の画素 : {}   ({})".format(
            count_line(nonfinite_count, values.shape[0] * values.shape[1]),
            "マゼンタ(255,0,255)で塗ってある" if nonfinite_count else "塗り分けは無し",
        )
    )
    print("")
    print_nonfinite(values)
    print("")
    print_stat_table(values, list(range(dump.channels)), CHANNEL_LABELS)
    report_constant(values, list(range(dump.channels)), CHANNEL_LABELS)
    print("")
    print("**このPNGは目で見るためのもので、判定の根拠にしてはいけない。**")
    print("  判定は stat / where / diff / row の数値で行うこと")
    return 0


# =============================================================================
# synth (物差しを検算するための、既知の中身を持つダンプを作る)
# =============================================================================


def write_dump(path, array, name, element_type, frame_index=0, mip_level=0, array_slice=0, backend=0):
    height, width, stored_channels = array.shape
    dtype = ELEMENT_TYPES[element_type][0]
    bytes_per_element = np.dtype(dtype).itemsize
    # ElementType=4 は「1テクセル uint32 1個」で格納し、ChannelCountには展開後の3を書く
    channels = 3 if element_type == PACKED_11_11_10 else stored_channels
    payload = array.astype(dtype)

    header = HEADER_FIXED.pack(
        MAGIC,
        2,  # Version。KurenaiEngine3D::WriteTextureDumpFile と一致させること
        HEADER_BYTES,
        width,
        height,
        channels,
        element_type,
        bytes_per_element,
        frame_index,
        mip_level,
        array_slice,
        backend,
    )
    name_bytes = name.encode("utf-8")[: NAME_BYTES - 1]
    name_bytes = name_bytes + b"\0" * (NAME_BYTES - len(name_bytes))
    reserved = b"\0" * (HEADER_BYTES - len(header) - NAME_BYTES)

    with open(path, "wb") as handle:
        handle.write(header)
        handle.write(name_bytes)
        handle.write(reserved)
        handle.write(payload.tobytes())


def build_pattern(pattern, width, height, channels):
    if pattern.startswith("const:"):
        value = float(pattern[len("const:") :])
        return np.full((height, width, channels), value, dtype=np.float32)
    if pattern.startswith("nan:"):
        count = int(pattern[len("nan:") :])
        array = np.zeros((height, width, channels), dtype=np.float32)
        # 全チャンネルではなく先頭チャンネルにだけ入れる(「該当画素数 = count」を検算できる形にする)
        for i in range(count):
            array[(i // width) % height, i % width, 0] = np.float32("nan")
        return array
    if pattern == "ramp":
        xs = np.linspace(0.0, 1.0, width, dtype=np.float32)
        array = np.tile(xs.reshape(1, width, 1), (height, 1, channels))
        return array.astype(np.float32)
    if pattern == "checker":
        ys, xs = np.mgrid[0:height, 0:width]
        checker = ((xs // 8 + ys // 8) % 2).astype(np.float32)
        return np.repeat(checker[:, :, None], channels, axis=2)
    raise SystemExit("--pattern は ramp / const:V / nan:N / checker のいずれかです: {}".format(pattern))


def cmd_synth(args):
    parts = args.size.lower().split("x")
    if len(parts) != 2:
        raise SystemExit("--size は WxH の形で指定します: {}".format(args.size))
    width, height = (int(part) for part in parts)
    array = build_pattern(args.pattern, width, height, args.ch)
    write_dump(args.out, array, args.name, 3)
    print("=== synth ===")
    print(
        "作成しました: {}  ({}x{} ch={} pattern={} name={})".format(
            args.out, width, height, args.ch, args.pattern, args.name
        )
    )
    return 0


# =============================================================================
# selftest (物差しが機能することを、エンジン抜きで先に示す)
# =============================================================================


def cmd_selftest(args):
    """既知の中身に当てて、期待どおりの値と順序を返すことを確かめる。

    **新しい・変えた測り方は、既知の例に当てて期待どおりになるか確かめてから使う。**
    自分で作った物差しで自分の成果を測ると、物差しの誤りに気づけない。
    """
    import tempfile

    failures = []
    checks = 0

    def check(label, condition, detail=""):
        nonlocal checks
        checks += 1
        if condition:
            print("  PASS  {}".format(label))
        else:
            print("  FAIL  {}  {}".format(label, detail))
            failures.append(label)

    print("=== selftest ===")
    with tempfile.TemporaryDirectory() as workdir:
        # --- 1. const: 統計が全部同じ値になり、定数化として検出される ---
        print("1. const:0.25")
        const_path = os.path.join(workdir, "const.bin")
        write_dump(const_path, build_pattern("const:0.25", 16, 8, 4), "SelfTestConst", 3)
        dump = load(const_path)
        values = dump.as_float()
        check("寸法とチャンネルが往復できる", (dump.width, dump.height, dump.channels) == (16, 8, 4),
              "実際 {}x{} ch={}".format(dump.width, dump.height, dump.channels))
        check("min/median/mean/max が全部 0.25",
              np.allclose([values.min(), np.median(values), values.mean(), values.max()], 0.25),
              "実際 {}".format([values.min(), np.median(values), values.mean(), values.max()]))
        check("非有限は0件", int(np.count_nonzero(~np.isfinite(values))) == 0)

        # --- 2. nan: where が件数と座標を返す ---
        print("2. nan:10")
        nan_path = os.path.join(workdir, "nan.bin")
        write_dump(nan_path, build_pattern("nan:10", 16, 8, 4), "SelfTestNaN", 3)
        dump = load(nan_path)
        values = dump.as_float()
        nan_count = int(np.count_nonzero(np.isnan(values)))
        check("NaNがちょうど10件", nan_count == 10, "実際 {}".format(nan_count))
        nan_pixels = int(np.count_nonzero(np.isnan(values).any(axis=2)))
        check("NaNを含む画素も10件", nan_pixels == 10, "実際 {}".format(nan_pixels))

        # --- 3. ramp: 単調増加で、中央値が理論値 ---
        print("3. ramp")
        ramp_path = os.path.join(workdir, "ramp.bin")
        write_dump(ramp_path, build_pattern("ramp", 17, 4, 1), "SelfTestRamp", 3)
        dump = load(ramp_path)
        values = dump.as_float()
        row = values[0, :, 0]
        check("行が単調増加", bool(np.all(np.diff(row) > 0)))
        check("中央値が0.5", abs(float(np.median(values)) - 0.5) < 1e-6, "実際 {}".format(np.median(values)))
        check("両端が0と1", abs(float(row[0])) < 1e-6 and abs(float(row[-1]) - 1.0) < 1e-6)

        # --- 4. diff: 同一なら差ゼロ、片側へずらせば符号が100%片側 ---
        print("4. diff")
        base = build_pattern("ramp", 16, 8, 3)
        a_path = os.path.join(workdir, "a.bin")
        b_path = os.path.join(workdir, "b.bin")
        write_dump(a_path, base, "SelfTestDiff", 3)
        write_dump(b_path, base, "SelfTestDiff", 3)
        left = load(a_path).as_float()
        right = load(b_path).as_float()
        check("同一ファイルどうしの差は0", float(np.max(np.abs(right - left))) == 0.0)

        shifted = base + np.float32(0.01)
        write_dump(b_path, shifted, "SelfTestDiff", 3)
        right = load(b_path).as_float()
        delta = right - left
        changed = delta[np.abs(delta) > 0]
        check("ずらした側が全部プラス", changed.size > 0 and int(np.count_nonzero(changed < 0)) == 0,
              "マイナス {} 件".format(int(np.count_nonzero(changed < 0))))
        check("差の中央値が0.01", abs(float(np.median(changed)) - 0.01) < 1e-6,
              "実際 {}".format(np.median(changed)))

        # --- 5. ヘッダ不一致は比較を断る ---
        print("5. ヘッダ不一致")
        other_path = os.path.join(workdir, "other.bin")
        write_dump(other_path, build_pattern("ramp", 8, 8, 3), "SelfTestOther", 3)
        other = load(other_path)
        first = load(a_path)
        check("寸法が違うことを検出できる", (other.width, other.height) != (first.width, first.height))
        check("source名が違うことを検出できる", other.name != first.name)

        # --- 6. 壊れたファイルは黙って読まない ---
        print("6. 壊れたファイル")
        broken_path = os.path.join(workdir, "broken.bin")
        with open(a_path, "rb") as handle:
            raw = handle.read()
        with open(broken_path, "wb") as handle:
            handle.write(raw[: len(raw) - 8])  # 末尾を8バイト削る
        try:
            load(broken_path)
            check("途中で切れたファイルを拒否する", False, "読めてしまった")
        except SystemExit:
            check("途中で切れたファイルを拒否する", True)

        bad_magic_path = os.path.join(workdir, "badmagic.bin")
        with open(bad_magic_path, "wb") as handle:
            handle.write(b"XXXX" + raw[4:])
        try:
            load(bad_magic_path)
            check("マジックが違うファイルを拒否する", False, "読めてしまった")
        except SystemExit:
            check("マジックが違うファイルを拒否する", True)

        # --- 6.5 UNorm8 の 0〜1 正規化 ---
        #
        # 【この検査が抜けていた】1〜5はすべてFloat32で作っており、UNorm8の往復が一度も
        # 走っていなかった。as_float()の "/ 255.0" を消しても27件すべてPASSしてしまい、
        # アルベド(ElementType=1)の数値が255倍ずれても検査を素通りする状態だった。
        # **「全件PASS」は、通っていない経路については何も言っていない。**
        print("6.5 UNorm8 の正規化")
        unorm_path = os.path.join(workdir, "unorm.bin")
        # 0 / 51 / 204 / 255 → 0 / 0.2 / 0.8 / 1.0
        unorm = np.array([[[0, 51, 204, 255]], [[255, 204, 51, 0]]], dtype=np.uint8)
        write_dump(unorm_path, unorm, "GBufferAlbedo", 1, backend=1)
        unorm_dump = load(unorm_path)
        values = unorm_dump.as_float()
        check("UNorm8が0〜1へ正規化される", bool(np.allclose(values[0, 0], [0.0, 0.2, 0.8, 1.0])),
              "実際 {}".format(values[0, 0]))
        check("最大値が1.0(255ではない)", abs(float(values.max()) - 1.0) < 1e-9,
              "実際 {}".format(values.max()))
        check("生データは0〜255のまま保たれている", int(unorm_dump.data.max()) == 255,
              "実際 {}".format(unorm_dump.data.max()))
        check("Backendが往復する", unorm_dump.backend == 1 and unorm_dump.backend_name == "DX11",
              "実際 {} / {}".format(unorm_dump.backend, unorm_dump.backend_name))

        # Float16 も同様に、生ビットのまま読めることを確かめる(展開してしまっていないか)
        half_path = os.path.join(workdir, "half.bin")
        half = np.array([[[0.5, -0.25]], [[1.0, 2.0]]], dtype=np.float16)
        write_dump(half_path, half, "GBufferNormal", 2, backend=2)
        half_dump = load(half_path)
        half_values = half_dump.as_float()
        check("Float16がそのまま読める", bool(np.allclose(half_values[0, 0], [0.5, -0.25])),
              "実際 {}".format(half_values[0, 0]))
        check("Float16は正規化されない", abs(float(half_values[1, 0, 1]) - 2.0) < 1e-9,
              "実際 {}".format(half_values[1, 0, 1]))
        check("BackendがDX12として往復する", half_dump.backend_name == "DX12",
              "実際 {}".format(half_dump.backend_name))

        # --- 7. R11G11B10 のデコーダを、全ビットパターンで独立に検算する ---
        #
        # このフォーマットのバッファ(G-Bufferのエミッシブ)は手元のどのシーンでも全画素0で、
        # 実データでは一度も動かせない。**動かせないものを「たぶん合っている」で通さない**ため、
        # 11bit/10bitの取りうる値をすべて列挙し、独立に組み立てた期待値と突き合わせる。
        # 期待値は「Cで書くならこう」という素直なループで、本体のベクトル化した式とは別物にしてある
        # (同じ式を2回書いても、写し間違いは見つかっても考え違いは見つからない)
        print("7. R11G11B10 デコーダ(全ビットパターン)")

        def reference_decode(bits, mantissa_bits):
            exponent = bits >> mantissa_bits
            mantissa = bits & ((1 << mantissa_bits) - 1)
            if exponent == 0:
                return (mantissa / float(1 << mantissa_bits)) * (2.0 ** -14)
            if exponent == 31:
                return float("inf") if mantissa == 0 else float("nan")
            return (1.0 + mantissa / float(1 << mantissa_bits)) * (2.0 ** (exponent - 15))

        for label, bit_count, mantissa_bits, shift in (("R", 11, 6, 0), ("G", 11, 6, 11), ("B", 10, 5, 22)):
            count = 1 << bit_count
            packed = np.arange(count, dtype=np.uint64) << np.uint64(shift)
            decoded = unpack_r11g11b10(packed.astype(np.uint32))
            column = decoded[:, "RGB".index(label)]
            expected = np.array([reference_decode(v, mantissa_bits) for v in range(count)], dtype=np.float64)

            nan_match = np.array_equal(np.isnan(column), np.isnan(expected))
            finite_mask = np.isfinite(expected) & np.isfinite(column)
            value_match = bool(np.array_equal(column[finite_mask], expected[finite_mask]))
            inf_match = np.array_equal(np.isinf(column), np.isinf(expected))
            check(
                "{}成分 {}通りすべてが期待値と一致".format(label, count),
                nan_match and inf_match and value_match,
                "NaN一致={} Inf一致={} 値一致={}".format(nan_match, inf_match, value_match),
            )

        # 他の成分のビットに影響されないこと(シフト・マスクの取り違えを潰す)
        packed_all = np.array([0xFFFFFFFF], dtype=np.uint32)
        decoded_all = unpack_r11g11b10(packed_all)[0]
        check(
            "全ビット1なら3成分ともNaN",
            bool(np.all(np.isnan(decoded_all))),
            "実際 {}".format(decoded_all),
        )
        # 1.0 のビット列: 指数15(=0x0F)、仮数0
        one_r = (15 << 6)
        one_g = (15 << 6) << 11
        one_b = (15 << 5) << 22
        decoded_one = unpack_r11g11b10(np.array([one_r | one_g | one_b], dtype=np.uint32))[0]
        check(
            "指数15・仮数0 が3成分とも 1.0",
            bool(np.allclose(decoded_one, [1.0, 1.0, 1.0])),
            "実際 {}".format(decoded_one),
        )
        # 0 は0のまま(非正規化数の経路)
        decoded_zero = unpack_r11g11b10(np.array([0], dtype=np.uint32))[0]
        check("全ビット0なら3成分とも0.0", bool(np.all(decoded_zero == 0.0)), "実際 {}".format(decoded_zero))

        # --- 8. ElementType=4 のファイルを往復できる ---
        print("8. R11G11B10 のファイル往復")
        packed_path = os.path.join(workdir, "packed.bin")
        pattern = np.array([[[0], [one_r | one_g | one_b]], [[0xFFFFFFFF], [one_r]]], dtype=np.uint32)
        write_dump(packed_path, pattern, "GBufferEmissive", PACKED_11_11_10)
        packed_dump = load(packed_path)
        check("ChannelCountが3として読める", packed_dump.channels == 3, "実際 {}".format(packed_dump.channels))
        values = packed_dump.as_float()
        check("形が (2,2,3) になる", values.shape == (2, 2, 3), "実際 {}".format(values.shape))
        check("(0,0) が 0", bool(np.all(values[0, 0] == 0.0)))
        check("(1,0) が 1.0", bool(np.allclose(values[0, 1], [1.0, 1.0, 1.0])))
        check("(0,1) が全部NaN", bool(np.all(np.isnan(values[1, 0]))))
        check(
            "(1,1) は R だけ1.0で G/B は0",
            bool(np.allclose(values[1, 1], [1.0, 0.0, 0.0])),
            "実際 {}".format(values[1, 1]),
        )

        # --- 9. 連番ノイズ: 既知のタイル共通成分と画素固有成分を分解できる ---
        print("9. 連番ノイズのタイル分解")
        tile = 16
        width = height = 64
        frames = 256
        stride = 3
        planted_between = 0.04
        planted_inner = 0.02
        rng = np.random.default_rng(20260905)
        noise_paths = []
        for frame in range(frames):
            # タイル共通オフセットはタイル間、画素ごとの独立ノイズはタイル内にだけ現れる。
            common = rng.normal(0.0, planted_between, size=(height // tile, width // tile))
            common = np.repeat(np.repeat(common, tile, axis=0), tile, axis=1)
            independent = rng.normal(0.0, planted_inner, size=(height, width))
            path = os.path.join(workdir, "noise_{:04d}.bin".format(frame))
            write_dump(path, (common + independent)[:, :, None].astype(np.float32), "SelfTestNoise", 3,
                       frame_index=frame * stride)
            noise_paths.append(path)
        noise_dumps = [load(path) for path in noise_paths]
        noise_values = np.stack([noise_channel(dump.as_float(), "luma") for dump in noise_dumps], axis=0)
        noise_summary = summarize_noise(noise_values, tile, (0, 0))
        between_recovered = noise_percentiles(noise_summary["between_std"])[0]
        # タイル内は**画素で**集約する(確立した定義。print_noise_summary と同じ量を検算する)
        inner_recovered = noise_percentiles(noise_summary["residual_std"])[0]
        relative_tolerance = 0.15
        check("タイル間が植え込みstdを復元する（許容相対誤差15%）",
              abs(between_recovered - planted_between) / planted_between <= relative_tolerance,
              "実際 {:.6g} / 植込み {:.6g}".format(between_recovered, planted_between))
        check("タイル内が植え込みstdを復元する（許容相対誤差15%）",
              abs(inner_recovered - planted_inner) / planted_inner <= relative_tolerance,
              "実際 {:.6g} / 植込み {:.6g}".format(inner_recovered, planted_inner))
        check("タイル分解の恒等式の相対偏差が1e-9未満", noise_summary["identity_relative"] < 1e-9,
              "実際 {:.3e}".format(noise_summary["identity_relative"]))
        frame_steps = np.diff([dump.frame_index for dump in noise_dumps])
        check("FrameIndexの間隔が書いたstrideと一致", bool(np.all(frame_steps == stride)),
              "実際 {}".format(frame_steps.tolist()))

        # 対照はタイル共通成分だけを0にする。タイル間の漏れ込みがタイル内より十分小さいことを確認する。
        control_values = []
        for frame in range(frames):
            control_values.append(rng.normal(0.0, planted_inner, size=(height, width)))
        control_summary = summarize_noise(np.asarray(control_values, dtype=np.float64), tile, (0, 0))
        control_between = noise_percentiles(control_summary["between_std"])[0]
        control_inner = noise_percentiles(control_summary["residual_std"])[0]
        check("タイル共通成分0ならタイル間はタイル内より十分小さい",
              control_between < control_inner * 0.2,
              "タイル間 {:.6g} / タイル内 {:.6g}".format(control_between, control_inner))

        # --- 10. 全オフセットの最大は「位相のずれ」で動かず、「格子を壊した」ときだけ下がる ---
        #
        # 【なぜこの対照が要るか】タイル格子をフレームごとにずらす手法を、固定オフセットの
        # タイル間で評価すると**必ず改善して見える**。測る格子が相手と合わなくなるだけで、
        # 中身は何も良くなっていない。評価に使う統計量が、位相のずれでは動かないことを
        # 先に示しておく。示さずに掃引すると、物差しの側で外す。
        print("10. 全オフセット最大は位相不変で、格子を壊したときだけ下がる")
        # 【厳密な不変ではなく、端の欠けぶんだけずれる】オフセットを付けると完全なタイルの数が
        # 減る(例: 64画素幅・タイル16なら 4個 → 3個)ため、中央値を取る母集団が変わる。
        # 画像が小さいとこの影響が大きく出るので、端の欠けが相対的に小さい寸法で確かめる
        tile10 = 16
        size10 = 128
        frames10 = 32
        rng10 = np.random.default_rng(20260906)

        def planted_series(jitter):
            """タイル共通の成分を持つ系列。jitter=Trueならフレームごとに格子をずらす"""
            out = []
            for _ in range(frames10):
                coarse = rng10.normal(0.0, 0.05, size=(size10 // tile10 + 2, size10 // tile10 + 2))
                big = np.repeat(np.repeat(coarse, tile10, axis=0), tile10, axis=1)
                if jitter:
                    oy, ox = int(rng10.integers(0, tile10)), int(rng10.integers(0, tile10))
                else:
                    oy, ox = 0, 0
                out.append(big[oy:oy + size10, ox:ox + size10]
                           + rng10.normal(0.0, 0.01, size=(size10, size10)))
            return np.asarray(out, dtype=np.float64)

        def sweep_max(series):
            best = -1.0
            for oy in range(tile10):
                for ox in range(tile10):
                    value = noise_percentiles(summarize_noise(series, tile10, (oy, ox))["between_std"])[0]
                    if value > best:
                        best = value
            return best

        fixed_series = planted_series(False)
        fixed_max = sweep_max(fixed_series)
        # 全フレームを同じだけずらす = 純粋な位相のずれ。最大は動いてはいけない
        shifted_max = sweep_max(np.roll(fixed_series, (5, 7), axis=(1, 2)))
        jitter_series = planted_series(True)
        jitter_max = sweep_max(jitter_series)

        # 端のタイルが欠けるぶんだけ母集団が変わるので、厳密な不変にはならない。
        # ジッタの効き(下の -15%以上)とは桁が違うことが要点
        check("純粋な位相のずれでは全オフセット最大がほぼ動かない(相対3%以内)",
              abs(shifted_max - fixed_max) / fixed_max < 0.03,
              "固定 {:.6g} / 平行移動後 {:.6g}".format(fixed_max, shifted_max))
        check("フレームごとに格子を振ると全オフセット最大が下がる",
              jitter_max < fixed_max * 0.85,
              "固定 {:.6g} / ジッタ {:.6g}".format(fixed_max, jitter_max))
        # ジッタは「1枚あたりのノイズ量」を減らす手法ではない。総量が変わっていないことも見る
        fixed_pixel = np.median(np.std(fixed_series, axis=0, ddof=1))
        jitter_pixel = np.median(np.std(jitter_series, axis=0, ddof=1))
        check("ジッタは画素ごとの時間stdの総量を変えない(相対20%以内)",
              abs(jitter_pixel - fixed_pixel) / fixed_pixel < 0.2,
              "固定 {:.6g} / ジッタ {:.6g}".format(fixed_pixel, jitter_pixel))

    print("")
    if failures:
        print("selftest: {} 件中 {} 件が失敗".format(checks, len(failures)))
        for label in failures:
            print("  - {}".format(label))
        return 1
    print("selftest: {} 件すべて PASS".format(checks))
    return 0


# =============================================================================


def main(argv):
    parser = argparse.ArgumentParser(
        description="-dumptex が吐いた中間レンダーターゲットの生値を数値で調べる",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    sub = parser.add_subparsers(dest="command")

    p = sub.add_parser("header", help="ヘッダだけ表示する")
    p.add_argument("path")
    p.set_defaults(func=cmd_header)

    p = sub.add_parser("stat", help="チャンネル別の統計と、定数化・非有限の検出")
    p.add_argument("path")
    p.add_argument("--rect", help="x,y,w,h で範囲を絞る")
    p.add_argument("--channel", help="rgba のうち見たいものだけ")
    p.set_defaults(func=cmd_stat)

    p = sub.add_parser("px", help="指定座標の値")
    p.add_argument("path")
    p.add_argument("--at", action="append", required=True, help="x,y (複数指定可)")
    p.add_argument("--radius", type=int, default=0, help="この半径の平均も出す(既定0)")
    p.set_defaults(func=cmd_px)

    p = sub.add_parser("rect", help="矩形の集約値")
    p.add_argument("path")
    p.add_argument("--rect", required=True, help="x,y,w,h")
    p.add_argument("--channel", help="rgba のうち見たいものだけ")
    p.add_argument("--reduce", default="mean", choices=["mean", "median", "min", "max", "sum"])
    p.set_defaults(func=cmd_rect)

    p = sub.add_parser("hist", help="ヒストグラム")
    p.add_argument("path")
    p.add_argument("--bins", type=int, default=16)
    p.add_argument("--range", help="lo,hi")
    p.add_argument("--rect", help="x,y,w,h で範囲を絞る")
    p.add_argument("--channel", help="rgba のうち見たいものだけ")
    p.add_argument("--log", action="store_true", help="バーを対数目盛にする")
    p.set_defaults(func=cmd_hist)

    p = sub.add_parser("row", help="1行の断面プロファイル")
    p.add_argument("path")
    p.add_argument("--y", type=int, required=True)
    p.add_argument("--x0", type=int, default=0)
    p.add_argument("--x1", type=int, default=-1)
    p.add_argument("--step", type=int, default=1)
    p.add_argument("--channel", help="rgba のうち見たいものだけ")
    p.set_defaults(func=cmd_row)

    p = sub.add_parser("where", help="条件に合う画素の座標")
    p.add_argument("path")
    p.add_argument("--pred", required=True, help="nan / inf / neg / zero / gt:V / lt:V")
    p.add_argument("--limit", type=int, default=20)
    p.add_argument("--channel", help="rgba のうち見たいものだけ")
    p.set_defaults(func=cmd_where)

    p = sub.add_parser("diff", help="2つのダンプの差")
    p.add_argument("a")
    p.add_argument("b")
    p.add_argument("--rel", action="store_true", help="相対差も出す")
    p.add_argument("--eps", type=float, default=1e-6, help="相対差の分母の下限")
    p.add_argument("--rect", help="x,y,w,h で範囲を絞る")
    p.add_argument("--force", action="store_true", help="ヘッダが食い違っていても比較する")
    p.set_defaults(func=cmd_diff)

    p = sub.add_parser("noise", help="連番ダンプの時間ノイズをタイル間・タイル内へ分解する")
    p.add_argument("paths", nargs="+", help="連番のパスまたはglob（3枚以上）")
    p.add_argument("--tile", type=int, default=16, help="完全タイルの一辺（既定16）")
    p.add_argument("--offset", default="0,0", help="Y,X でタイル格子をずらす（既定0,0）")
    p.add_argument("--offset-sweep", action="store_true",
                   help="格子オフセットを全通り振り、タイル間の min/median/max を出す(評価に使うのは max)")
    p.add_argument("--sweep-step", type=int, default=1,
                   help="--offset-sweep の刻み(既定1=全オフセット)。粗くすると最大を取り逃がす")
    p.add_argument("--channel", default="luma", choices=["rgba", "r", "g", "b", "a", "luma"])
    p.add_argument("--lit-threshold", type=float, default=1e-4, help="時間平均がこれ以下のタイルを別集計する")
    p.set_defaults(func=cmd_noise)

    p = sub.add_parser("png", help="PNGへ書き出す(判定の根拠にはしないこと)")
    p.add_argument("path")
    p.add_argument("-o", "--out", required=True)
    p.add_argument("--exposure", type=float, default=1.0)
    p.add_argument("--channel", default="rgb", choices=["rgb", "r", "g", "b", "a", "len"])
    p.add_argument("--mode", default="linear", choices=["linear", "srgb", "signed", "falsecolor"])
    p.add_argument("--range", help="falsecolor のレンジ lo,hi")
    p.set_defaults(func=cmd_png)

    p = sub.add_parser("synth", help="既知の中身を持つダンプを作る(物差しの検算用)")
    p.add_argument("out")
    p.add_argument("--size", default="16x8", help="WxH")
    p.add_argument("--ch", type=int, default=4, choices=[1, 2, 3, 4])
    p.add_argument("--pattern", default="ramp", help="ramp / const:V / nan:N / checker")
    p.add_argument("--name", default="Synth")
    p.set_defaults(func=cmd_synth)

    p = sub.add_parser("selftest", help="物差しが機能することを先に示す")
    p.set_defaults(func=cmd_selftest)

    args = parser.parse_args(argv)
    if not getattr(args, "func", None):
        parser.print_help()
        return 1
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
