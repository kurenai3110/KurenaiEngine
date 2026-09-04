#!/usr/bin/env python3
# MegaLights の画質・ちらつきを数値で比べるためのスクリプト。
#
# 【なぜ要るのか】docs/ImplementationDetail.md 61.7f〜61.7i の実測はすべて使い捨ての
# スクリプトで取られており、リポジトリには1本も残っていなかった。数値の再現も反証も
# できない状態だったので、確立した定義をここへ機械化する。
#
# 物差しの選び方には理由がある(勝手に変えないこと):
#   - **RMSE は使わない。** 1/p の重みが作る裾(ファイアフライ)に二乗和が支配され、
#     実測で誤差二乗和の73.6%を上位10画素が占める。|相対誤差| の中央値で見る(61.7g)
#   - **分母は必ず参照実装。** 900枚の蓄積平均を「期待値」として分母に使うと、
#     それ自体に残ったノイズが小さい差を潰し、a-trous の段数比較では順序が入れ替わる(61.7g)
#   - **ちらつきは隣接フレーム差で測らない。** 連写の間隔が起動ごとに揃わないため、
#     同じ構成の2回で3〜4倍動く。連写全体の時間標準偏差を使う(61.7i)
#   - **画面キャプチャで収束は測れない。** 8bit かつトーンマップ後で、丸めだけで
#     RMSE に 0.29 階調の下限が生まれる。線形の蓄積ダンプを使う(61.5)
#
# 使い方:
#   python Tools/megalights_metrics.py dump   <参照.bin> <比較.bin> [--edge-threshold 0.25]
#   python Tools/megalights_metrics.py burst  <連写*.png>            [--title-bar 48]
#   python Tools/megalights_metrics.py strafe <ストレイフ*.png>      [--title-bar 48]
#   python Tools/megalights_metrics.py perf   <perfdump.csv> [...]

import argparse
import glob
import hashlib
import struct
import sys
from pathlib import Path

import numpy as np

# 蓄積ダンプのマジック。KurenaiEngine3D.cpp の書き出しと一致させること
DUMP_MAGIC = b"KMLA"
# ヘッダは magic 4B + uint32 x4 (幅 / 高さ / 足したフレーム数 / 予約)
DUMP_HEADER_SIZE = 4 + 4 * 4

# Rec.709 の輝度係数。エンジン側 Luminance() と同じ
LUMA_WEIGHTS = np.array([0.2126, 0.7152, 0.0722], dtype=np.float64)

# 【スクリーンショットのタイトルバーは除く】描画と無関係に変わることがあり、
# 実測で1回だけ全画素の0.16%が動いて「シーンが動いている」ように見えた(61.7i)
DEFAULT_TITLE_BAR_ROWS = 48


def read_dump(path):
    """蓄積ダンプを読んで「1フレームあたりの平均」の float32 配列 (H, W, 4) を返す。

    ダンプは**総和**なので枚数で割る。割り忘れると比が枚数比になって静かに間違える。
    """
    raw = Path(path).read_bytes()
    if len(raw) < DUMP_HEADER_SIZE:
        raise ValueError(f"{path}: ファイルが短すぎます({len(raw)}バイト)。ダンプが途中で終わっている可能性があります")
    magic = raw[:4]
    if magic != DUMP_MAGIC:
        raise ValueError(f"{path}: マジックが {magic!r} で 'KMLA' ではありません。蓄積ダンプではないファイルです")
    width, height, frames, _reserved = struct.unpack_from("<4I", raw, 4)
    if frames == 0:
        raise ValueError(f"{path}: 足したフレーム数が0です。蓄積が走る前に書き出されています")
    expected = DUMP_HEADER_SIZE + width * height * 4 * 4
    if len(raw) != expected:
        raise ValueError(
            f"{path}: サイズが合いません(期待 {expected} バイト / 実際 {len(raw)} バイト)。"
            f"{width}x{height} と食い違っています"
        )
    data = np.frombuffer(raw, dtype=np.float32, count=width * height * 4, offset=DUMP_HEADER_SIZE)
    data = data.reshape(height, width, 4).astype(np.float64) / float(frames)
    return data, width, height, frames


def luminance(rgb):
    return rgb[..., :3] @ LUMA_WEIGHTS


def edge_band_mask(ref_lum, threshold):
    """参照実装の3x3近傍の輝度レンジが大きい画素 = 影の縁の帯。

    【縁の帯を別に数える理由】クアッド共有の偏りは硬い影の縁を最大1画素ぼかすが、
    箱フィルタは積分を保存するので**総和比には出ない**。縁の帯でだけ見える。
    """
    padded = np.pad(ref_lum, 1, mode="edge")
    stack = np.stack(
        [padded[dy : dy + ref_lum.shape[0], dx : dx + ref_lum.shape[1]] for dy in range(3) for dx in range(3)],
        axis=0,
    )
    local_range = stack.max(axis=0) - stack.min(axis=0)
    # 相対レンジ。暗部の絶対レンジは小さいので、局所平均で正規化する
    local_mean = stack.mean(axis=0)
    with np.errstate(divide="ignore", invalid="ignore"):
        relative = np.where(local_mean > 1e-6, local_range / local_mean, 0.0)
    return relative > threshold


def cmd_dump(args):
    ref, rw, rh, rframes = read_dump(args.reference)
    cmp_, cw, ch, cframes = read_dump(args.compare)
    if (rw, rh) != (cw, ch):
        raise ValueError(f"解像度が違います: 参照 {rw}x{rh} / 比較 {cw}x{ch}。-renderres を揃えること")

    print(f"参照 : {args.reference}  {rw}x{rh}  {rframes}フレーム")
    print(f"比較 : {args.compare}  {cw}x{ch}  {cframes}フレーム")

    if args.hash:
        # ビット同一の判定(回帰対照用)。平均ではなく生バイトで比べる
        h_ref = hashlib.sha256(Path(args.reference).read_bytes()).hexdigest()
        h_cmp = hashlib.sha256(Path(args.compare).read_bytes()).hexdigest()
        print(f"\nsha256 参照 : {h_ref}")
        print(f"sha256 比較 : {h_cmp}")
        print(f"ビット同一 : {'はい' if h_ref == h_cmp else 'いいえ'}")

    ref_lum = luminance(ref)
    cmp_lum = luminance(cmp_)

    # 光の届いている画素だけを見る(61.7g: 真値の輝度 > 1e-4)
    lit = ref_lum > args.lit_threshold
    lit_count = int(lit.sum())
    total = ref_lum.size
    if lit_count == 0:
        raise ValueError("参照実装に点灯画素がありません。シーン・露出・-megalights の指定を確認すること")

    # 総和比。デノイザの損失と推定量の偏りが乗る指標
    sum_ref = float(ref[..., :3].sum())
    sum_cmp = float(cmp_[..., :3].sum())
    sum_ratio = sum_cmp / sum_ref if sum_ref > 0 else float("nan")

    with np.errstate(divide="ignore", invalid="ignore"):
        signed = np.where(lit, (cmp_lum - ref_lum) / np.maximum(ref_lum, 1e-12), 0.0)
    signed_lit = signed[lit]
    abs_lit = np.abs(signed_lit)

    # 死んだ画素 = 参照は光っているのに比較側が厳密に0(61.7f)
    dead = int(np.count_nonzero(lit & (cmp_lum == 0.0)))

    print(f"\n点灯画素 : {lit_count} / {total} ({100.0 * lit_count / total:.1f}%)")
    print(f"総和比 (比較/参照)          : {sum_ratio:.5f}")
    print(f"|相対誤差| 中央値           : {np.median(abs_lit):.5f}")
    print(f"|相対誤差| p90              : {np.percentile(abs_lit, 90):.5f}")
    print(f"|相対誤差| p99              : {np.percentile(abs_lit, 99):.5f}")
    print(f"符号つき相対誤差の中央値    : {np.median(signed_lit):+.5f}")
    print(f"誤差が負側の画素の割合      : {100.0 * np.count_nonzero(signed_lit < 0) / lit_count:.1f}%")
    print(f"死んだ画素                  : {dead}")

    # 【平均は載せるが指標にしない】ファイアフライに支配される(61.7h)
    print(f"(参考)相対誤差の平均      : {signed_lit.mean():+.5f}  ← 指標にしないこと")

    edge = edge_band_mask(ref_lum, args.edge_threshold) & lit
    edge_count = int(edge.sum())
    if edge_count > 0:
        edge_abs = np.abs(signed[edge])
        print(f"\n影の縁の帯 : {edge_count} 画素 ({100.0 * edge_count / lit_count:.1f}% の点灯画素)")
        print(f"  |相対誤差| 中央値 : {np.median(edge_abs):.5f}")
        print(f"  |相対誤差| p90    : {np.percentile(edge_abs, 90):.5f}")
        flat = lit & ~edge
        if flat.sum() > 0:
            print(f"  平坦部の中央値    : {np.median(np.abs(signed[flat])):.5f}")
    else:
        print("\n影の縁の帯 : 0 画素(--edge-threshold を下げること)")


def load_gray(path, title_bar):
    from PIL import Image

    img = np.asarray(Image.open(path).convert("RGB"), dtype=np.float64)
    if title_bar > 0:
        if img.shape[0] <= title_bar:
            raise ValueError(f"{path}: 高さ {img.shape[0]} がタイトルバー {title_bar} 行以下です")
        img = img[title_bar:, :, :]
    return img @ LUMA_WEIGHTS


def expand_inputs(patterns):
    files = []
    for pattern in patterns:
        matched = sorted(glob.glob(pattern))
        if not matched:
            # glob が効かない環境でも直接指定なら通す
            if Path(pattern).exists():
                matched = [pattern]
            else:
                raise ValueError(f"{pattern}: ファイルが見つかりません")
        files.extend(matched)
    return files


def cmd_burst(args):
    """連写のちらつき。**時間標準偏差で測る**(隣接フレーム差は物差しにならない。61.7i)"""
    files = expand_inputs(args.images)
    if len(files) < 3:
        raise ValueError(f"連写が {len(files)} 枚しかありません。時間stdを取るには最低3枚(実測は16枚)要ります")
    frames = np.stack([load_gray(f, args.title_bar) for f in files], axis=0)
    print(f"連写 {len(files)} 枚  {frames.shape[2]}x{frames.shape[1]} (タイトルバー {args.title_bar} 行を除去済み)")

    # ビット同一の組を数える。参照実装は全組同一になるはずで、これがノイズ下限0の証明になる
    identical = sum(
        1 for i in range(len(files) - 1) if np.array_equal(frames[i], frames[i + 1])
    )
    print(f"隣接組でビット同一 : {identical} / {len(files) - 1}")

    std = frames.std(axis=0, ddof=1)
    print(f"\n時間std 中央値      : {np.median(std):.3f} 階調")
    print(f"時間std p90         : {np.percentile(std, 90):.3f} 階調")
    print(f"時間std > 2階調     : {100.0 * np.count_nonzero(std > 2.0) / std.size:.2f}%")

    # 【参考】隣接フレーム差。撮影の運を測るので**判断に使わない**(61.7i)
    adjacent = np.abs(np.diff(frames, axis=0))
    print(f"(参考)隣接差>4階調 : {100.0 * np.count_nonzero(adjacent > 4.0) / adjacent.size:.3f}%  ← 判断に使わないこと")

    # 明るさで層別。暗部ほど静かで明部ほど揺れる(61.7i.3)
    mean = frames.mean(axis=0)
    print("\n輝度で層別:")
    edges = [4, 20, 49, 88, 145, 256]
    for lo, hi in zip(edges[:-1], edges[1:]):
        band = (mean >= lo) & (mean < hi)
        if band.sum() > 0:
            print(f"  {lo:3d}〜{hi:3d} : std中央値 {np.median(std[band]):.3f}  画素 {100.0 * band.sum() / mean.size:4.1f}%")


def cmd_strafe(args):
    """移動中の粒。局所中央値からの上振れで測る(カメラ位置が揃わなくても比べられる。61.7g.7)

    【ノイズ下限は34%】同一構成の2回でこれだけ動く。この幅より小さい差を改善と呼ばないこと。
    """
    files = expand_inputs(args.images)
    if len(files) < 2:
        raise ValueError(f"{len(files)} 枚しかありません。1枚は分布ではないので複数枚(実測は10枚)要ります")

    over8 = 0
    over16 = 0
    total = 0
    deviations = []
    for path in files:
        gray = load_gray(path, args.title_bar)
        padded = np.pad(gray, 1, mode="edge")
        # 自分を除く8近傍の中央値
        neighbors = np.stack(
            [
                padded[dy : dy + gray.shape[0], dx : dx + gray.shape[1]]
                for dy in range(3)
                for dx in range(3)
                if not (dy == 1 and dx == 1)
            ],
            axis=0,
        )
        deviation = gray - np.median(neighbors, axis=0)
        deviations.append(deviation.ravel())
        over8 += int(np.count_nonzero(deviation >= 8.0))
        over16 += int(np.count_nonzero(deviation >= 16.0))
        total += deviation.size

    all_dev = np.concatenate(deviations)
    print(f"移動中 {len(files)} 枚 (タイトルバー {args.title_bar} 行を除去済み)")
    print(f"+8階調以上  : {100.0 * over8 / total:.4f}%")
    print(f"+16階調以上 : {100.0 * over16 / total:.4f}%")
    print(f"p99.9       : {np.percentile(all_dev, 99.9):+.1f} 階調")
    print("\n【ノイズ下限34%】同一構成の2回でこれだけ動く。この幅より小さい差を改善と呼ばないこと(61.7g.7)")


def cmd_perf(args):
    """-perfdump の CSV を読み、MegaLights のパス合計を出す。

    【AccumとDumpは除く】名前が MegaLights で始まるので素朴に合計すると計測専用パスが混ざる。
    【同名パスは合計済み】空間再利用が2回走るとき CSV の1行は2ディスパッチの和(61.7g.3)。
    """
    for path in expand_inputs(args.csv):
        rows = {}
        frames = None
        for line in Path(path).read_text(encoding="utf-8").splitlines()[1:]:
            if not line.strip():
                continue
            name, value = line.rsplit(",", 1)
            if name == "__frames":
                frames = int(float(value))
                continue
            rows[name] = float(value)

        # 計測専用のパスは MegaLights のコストではない
        excluded = {"MegaLightsAccum", "MegaLightsDump"}
        mega = {k: v for k, v in rows.items() if k.startswith("MegaLights") and k not in excluded}
        mega_total = sum(mega.values())
        grand_total = sum(rows.values())

        # ライト数に依存しないパスの中央値で正規化する(起動ごとに絶対値が20〜24%ずれる。61.7e.2)
        stable_names = [
            "AO", "AOBlur", "AerialPerspective", "DepthPrepass", "HiZ",
            "Present", "SkyCloud", "Tonemap", "Shadow0", "Shadow1", "Shadow2", "Shadow3",
        ]
        # 【中央値ではなく合計で正規化する】中央値は Shadow0-3(夜のシーンでは 0.005ms 級)に
        # 引っ張られ、そこはタイマの量子化が支配するので基準そのものが暴れる。
        # 実測で、同じ機械の同じシーンでも中央値は 0.021ms と 0.037ms のあいだで動いた
        # (1.7倍)。合計なら大きいパスが効くので安定する
        stable = [rows[n] for n in stable_names if n in rows]
        normalizer = float(np.sum(stable)) if stable else float("nan")

        print(f"\n=== {path} ({frames} フレームの平均) ===")
        for name, value in sorted(mega.items(), key=lambda kv: -kv[1]):
            print(f"  {name:32s} {value:7.3f} ms")
        print(f"  {'MegaLights 合計':32s} {mega_total:7.3f} ms")
        print(f"  {'総GPU時間':32s} {grand_total:7.3f} ms")
        if stable:
            print(f"  {'正規化の基準(安定パスの合計)':32s} {normalizer:7.3f} ms  ({len(stable)}本)")
            print(f"  {'正規化した MegaLights 合計':32s} {mega_total / normalizer:7.3f}")
            print(f"  {'正規化した総GPU時間':32s} {grand_total / normalizer:7.3f}")
        else:
            print("  【警告】正規化に使える安定パスが1本もありません。絶対値は起動ごとに20〜24%ずれます")
        for name in excluded:
            if name in rows:
                print(f"  (除外) {name:26s} {rows[name]:7.3f} ms  ← 計測専用パス")


def main():
    parser = argparse.ArgumentParser(
        description="MegaLights の画質・ちらつき・コストを測る(定義は docs/ImplementationDetail.md 61章)"
    )
    sub = parser.add_subparsers(dest="command", required=True)

    p_dump = sub.add_parser("dump", help="蓄積ダンプ2本を比べる(分母は必ず参照実装)")
    p_dump.add_argument("reference", help="参照実装のダンプ(分母。-megalights 1)")
    p_dump.add_argument("compare", help="比較対象のダンプ")
    p_dump.add_argument("--lit-threshold", type=float, default=1e-4, help="点灯とみなす参照の輝度(既定 1e-4)")
    p_dump.add_argument("--edge-threshold", type=float, default=0.25, help="影の縁とみなす3x3の相対レンジ(既定 0.25)")
    p_dump.add_argument("--hash", action="store_true", help="ビット同一かも判定する(回帰対照用)")
    p_dump.set_defaults(func=cmd_dump)

    p_burst = sub.add_parser("burst", help="連写のちらつきを時間stdで測る")
    p_burst.add_argument("images", nargs="+", help="連写のPNG(グロブ可)")
    p_burst.add_argument("--title-bar", type=int, default=DEFAULT_TITLE_BAR_ROWS, help="除去するタイトルバーの行数")
    p_burst.set_defaults(func=cmd_burst)

    p_strafe = sub.add_parser("strafe", help="移動中の粒を局所中央値からの上振れで測る")
    p_strafe.add_argument("images", nargs="+", help="ストレイフ中のPNG(グロブ可)")
    p_strafe.add_argument("--title-bar", type=int, default=DEFAULT_TITLE_BAR_ROWS, help="除去するタイトルバーの行数")
    p_strafe.set_defaults(func=cmd_strafe)

    p_perf = sub.add_parser("perf", help="-perfdump の CSV を読む")
    p_perf.add_argument("csv", nargs="+", help="perfdump の CSV(グロブ可)")
    p_perf.set_defaults(func=cmd_perf)

    args = parser.parse_args()
    try:
        args.func(args)
    except (ValueError, OSError) as error:
        print(f"エラー: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
