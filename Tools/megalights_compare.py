#!/usr/bin/env python3
"""MegaLights の蓄積ダンプ(`.kmla`)を2つ読み、収束と不偏性の指標を出す。

【なぜツールにするのか】これまで「総和の相対差」「|相対誤差|の中央値」「死んだ画素の枚数」は
その都度アドホックに計算していて、再利用できる形で残っていなかった。段階2は 2a〜2g で
同じ指標を繰り返し取るので、物差しが段ごとにぶれると**数字どうしを比べられなくなる**。
リポジトリに `.kmla` の読み手が1本も無かったのを、ここで1本にする。

【書式】KurenaiEngine3D が `-megalightsdump` で書く生データ。
    'K','M','L','A'              4バイト
    uint32 × 4                   幅 / 高さ / 足したフレーム数 / 予約
    float32 × 4 × (幅 × 高さ)    RGBA の合計(**フレーム数で割ると平均**)

【何を出すか】3つの指標を分けて出す。混ぜると別のことを測ってしまう。

  ・総和の相対差      … 不偏性。N を増やすと0へ寄るべき量
  ・|相対誤差| の中央値 … 収束の速さ。**RMSE は使わない** ―― 上位10画素が二乗和の半分を
                          占めることがあり(ファイアフライ)、収束ではなく外れ値を測る
  ・死んだ画素の枚数    … 参照が正なのに測定側が厳密に0の画素。提案分布が特定の灯を
                          一度も引けていない画素で、いくら蓄積しても直らない

【領域マスクを受け取る理由】総和は「自明に一致する画素」が支配する。半影帯だけ・発光体の
近傍だけといった部分集合で同じ指標を出せないと、「変えた領域が正しく変わったか」を測れない。

使い方:
    python Tools/megalights_compare.py <参照.kmla> <測定.kmla>
    python Tools/megalights_compare.py ref.kmla test.kmla --rect 300,200,700,500
    python Tools/megalights_compare.py ref.kmla test.kmla --mask-min-luminance 1e-3
    python Tools/megalights_compare.py ref.kmla test.kmla --json out.json
"""

import argparse
import json
import os
import struct
import sys

MAGIC = b"KMLA"
HEADER_SIZE = 4 + 4 * 4
# rec.709。参照の明るさで画素を選ぶときに使う
LUMA = (0.2126, 0.7152, 0.0722)


class Dump:
    """1つの .kmla。値は「フレーム数で割った平均」で保持する"""

    def __init__(self, path, width, height, frames, pixels):
        self.path = path
        self.width = width
        self.height = height
        self.frames = frames
        # pixels: [(r, g, b, a), ...] を行優先で width*height 個
        self.pixels = pixels

    def __repr__(self):
        return "%s (%dx%d, %dフレーム)" % (
            os.path.basename(self.path), self.width, self.height, self.frames)


def load_dump(path):
    """.kmla を読み、フレーム数で割った平均にして返す。

    【フレーム数で割るのをここでやる理由】呼び出し側で割り忘れると、蓄積枚数の違う
    2つを比べたときに「N倍の差」が出る。参照と測定で枚数を揃える運用にしていても、
    揃っていることを確かめずに比べられる形にしてはいけない
    """
    with open(path, "rb") as fp:
        head = fp.read(HEADER_SIZE)
        if len(head) < HEADER_SIZE:
            raise ValueError("%s: ヘッダーが短い(%dバイトしか無い)" % (path, len(head)))
        if head[:4] != MAGIC:
            raise ValueError("%s: 先頭が 'KMLA' ではない(%r)。MegaLightsのダンプではない"
                             % (path, head[:4]))
        width, height, frames, _reserved = struct.unpack("<4I", head[4:])
        if width == 0 or height == 0:
            raise ValueError("%s: 解像度が 0 (%dx%d)" % (path, width, height))
        if frames == 0:
            raise ValueError("%s: 足したフレーム数が 0。蓄積が走る前に書き出されている" % path)

        count = width * height * 4
        expect = count * 4
        body = fp.read(expect)
        if len(body) != expect:
            raise ValueError("%s: 本体が %dバイト足りない(解像度と食い違う)"
                             % (path, expect - len(body)))
        # 末尾に余りが無いことも見る。あればヘッダーの解釈が違う
        if fp.read(1):
            raise ValueError("%s: 本体の後ろにデータが残っている(書式の解釈が違う)" % path)

    values = struct.unpack("<%df" % count, body)
    inv = 1.0 / float(frames)
    pixels = [
        (values[i] * inv, values[i + 1] * inv, values[i + 2] * inv, values[i + 3] * inv)
        for i in range(0, count, 4)
    ]
    return Dump(path, width, height, frames, pixels)


def luminance(rgb):
    return LUMA[0] * rgb[0] + LUMA[1] * rgb[1] + LUMA[2] * rgb[2]


def build_mask(ref, rect, min_luminance):
    """比較に含める画素の添字を返す。

    rect は (x0, y0, x1, y1) で x1/y1 は含まない。min_luminance は参照側の輝度でのふるい。
    **両方とも参照側だけで決める** ―― 測定側の値で選ぶと、暗く出た画素を自分で除外して
    「一致した」と言えてしまう
    """
    if rect is None:
        x0, y0, x1, y1 = 0, 0, ref.width, ref.height
    else:
        x0, y0, x1, y1 = rect
        x0 = max(0, min(x0, ref.width))
        x1 = max(0, min(x1, ref.width))
        y0 = max(0, min(y0, ref.height))
        y1 = max(0, min(y1, ref.height))
        if x0 >= x1 or y0 >= y1:
            raise ValueError("--rect が空の矩形になっている: %s" % (rect,))

    indices = []
    for y in range(y0, y1):
        row = y * ref.width
        for x in range(x0, x1):
            i = row + x
            if min_luminance is not None and luminance(ref.pixels[i]) < min_luminance:
                continue
            indices.append(i)
    return indices


def median(values):
    if not values:
        return float("nan")
    ordered = sorted(values)
    mid = len(ordered) // 2
    if len(ordered) % 2 == 1:
        return ordered[mid]
    return 0.5 * (ordered[mid - 1] + ordered[mid])


def percentile(values, q):
    """線形補間の分位点。q は 0〜100"""
    if not values:
        return float("nan")
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    pos = (len(ordered) - 1) * (q / 100.0)
    lo = int(pos)
    hi = min(lo + 1, len(ordered) - 1)
    frac = pos - lo
    return ordered[lo] * (1.0 - frac) + ordered[hi] * frac


def compare(ref, test, indices, dead_epsilon):
    """3つの指標を計算する。indices は比較に含める画素の添字"""
    ref_sum = 0.0
    test_sum = 0.0
    rel_errors = []
    dead = 0
    # 参照が0で測定が正の画素(逆向きの取りこぼし)も数える。片側だけ数えると
    # 「暗く出た」と「明るく出た」のどちらかを見落とす
    spurious = 0

    for i in indices:
        r = luminance(ref.pixels[i])
        t = luminance(test.pixels[i])
        ref_sum += r
        test_sum += t
        if r > dead_epsilon:
            rel_errors.append(abs(t - r) / r)
            if t <= 0.0:
                dead += 1
        elif t > dead_epsilon:
            spurious += 1

    sum_rel = float("nan")
    if ref_sum != 0.0:
        sum_rel = (test_sum - ref_sum) / ref_sum

    return {
        "pixels": len(indices),
        "ref_sum": ref_sum,
        "test_sum": test_sum,
        "sum_relative_diff": sum_rel,
        "abs_rel_error_median": median(rel_errors),
        "abs_rel_error_p90": percentile(rel_errors, 90.0),
        "abs_rel_error_p99": percentile(rel_errors, 99.0),
        "compared_pixels": len(rel_errors),
        "dead_pixels": dead,
        "spurious_pixels": spurious,
    }


def parse_rect(text):
    parts = text.split(",")
    if len(parts) != 4:
        raise argparse.ArgumentTypeError("--rect は x0,y0,x1,y1 の4つ")
    try:
        return tuple(int(p) for p in parts)
    except ValueError:
        raise argparse.ArgumentTypeError("--rect の値が整数ではない: %s" % text)


def main(argv):
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("reference", help="参照実装のダンプ(真値の側)")
    parser.add_argument("test", help="測定するダンプ(確率的サンプリングの側)")
    parser.add_argument("--rect", type=parse_rect, default=None,
                        help="比較する矩形 x0,y0,x1,y1(x1/y1は含まない)。既定は全画面")
    parser.add_argument("--mask-min-luminance", type=float, default=None,
                        help="参照側の輝度がこの値未満の画素を除く。背景を落とすときに使う")
    parser.add_argument("--dead-epsilon", type=float, default=0.0,
                        help="参照が「正」と見なす下限(既定0)。ここを上げると死んだ画素の判定が緩む")
    parser.add_argument("--allow-frame-mismatch", action="store_true",
                        help="蓄積フレーム数が違っても続行する(既定は止める)")
    parser.add_argument("--json", default=None, help="指標をJSONで書き出すパス")
    args = parser.parse_args(argv)

    try:
        ref = load_dump(args.reference)
        test = load_dump(args.test)
    except (OSError, ValueError) as error:
        print("読み込みに失敗しました: %s" % error, file=sys.stderr)
        return 1

    print("参照: %s" % ref)
    print("測定: %s" % test)

    if (ref.width, ref.height) != (test.width, test.height):
        print("解像度が違うので比較できません(%dx%d と %dx%d)。-renderres を揃えること"
              % (ref.width, ref.height, test.width, test.height), file=sys.stderr)
        return 1
    if ref.frames != test.frames and not args.allow_frame_mismatch:
        # 平均にしてあるので枚数が違っても計算はできるが、収束の速さを比べる意味が無くなる
        print("蓄積フレーム数が違います(%d と %d)。収束の比較にならないので止めます"
              "(承知のうえなら --allow-frame-mismatch)" % (ref.frames, test.frames),
              file=sys.stderr)
        return 1

    try:
        indices = build_mask(ref, args.rect, args.mask_min_luminance)
    except ValueError as error:
        print("マスクの指定が不正です: %s" % error, file=sys.stderr)
        return 1

    total = ref.width * ref.height
    if not indices:
        print("比較対象の画素が0個になりました(--rect と --mask-min-luminance を見直すこと)",
              file=sys.stderr)
        return 1

    result = compare(ref, test, indices, args.dead_epsilon)
    result["width"] = ref.width
    result["height"] = ref.height
    result["frames"] = ref.frames
    result["reference"] = os.path.abspath(args.reference)
    result["test"] = os.path.abspath(args.test)

    print("")
    print("対象画素: %d / %d (%.2f%%)" % (result["pixels"], total, 100.0 * result["pixels"] / total))
    print("総和(輝度): 参照 %.6g / 測定 %.6g" % (result["ref_sum"], result["test_sum"]))
    print("  総和の相対差 %+.4f%%   ← 不偏性。Nを増やして0へ寄るか"
          % (100.0 * result["sum_relative_diff"]))
    print("|相対誤差|: 中央 %.4f / p90 %.4f / p99 %.4f (%d画素)"
          % (result["abs_rel_error_median"], result["abs_rel_error_p90"],
             result["abs_rel_error_p99"], result["compared_pixels"]))
    print("  ← 収束の速さ。**RMSEは使わない**(ファイアフライに支配される)")
    print("死んだ画素: %d  (参照が正なのに測定が厳密に0)" % result["dead_pixels"])
    print("余分な画素: %d  (参照が0なのに測定が正)" % result["spurious_pixels"])

    if args.json:
        try:
            with open(args.json, "w", encoding="utf-8") as fp:
                json.dump(result, fp, ensure_ascii=False, indent=2)
            print("")
            print("JSONを書き出しました: %s" % args.json)
        except OSError as error:
            print("JSONを書き出せませんでした: %s" % error, file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
