"""blender_msm_island.py --compare でレンダリングした画像と、参考写真(REF_DIR)を
突き合わせて、島のシルエット(スカイライン)の一致度を数値化するスクリプト。

標準ライブラリにはJPEGデコーダが無いため、Blenderの中で実行する(bpy.data.images.load()は
JPEG/PNGの両方を読める)。Tools/run_blender.ps1経由で
    Tools\\run_blender.ps1 -Script Tools\\compare_reference.py -ScriptArgs @(
        "--render-dir", <blender_msm_island.py --compare の出力ディレクトリ>,
        "--ref-dir", <参考写真フォルダ>,
        "--out-dir", <比較結果の出力先>)
のように呼び出す想定。

処理の概要(ビュー×参考写真の組ごと):
  (a) 参考写真(実写)は、画像上部のスカイ領域から輝度のしきい値を求め、各列で「空→島」の
      境界行(スカイライン)を抽出する(雲の内部の一瞬の暗部で誤検出しないよう、しきい値
      未満が一定行数連続して初めて境界と認める)。対岸の陸地・トンブレーヌ島・人物などの
      写り込みは、水平線(全列のスカイライン行の中央値)より十分上にあるスカイラインを
      持つ列だけをまず島の候補とし、建物の複雑なシルエットで生じる短い途切れを橋渡し
      してから、水平線からの高さの合計(面積)が最大の区間を島とみなすことで除外する
      (詳細はSKYLINE_MIN_RUN_*/ISLAND_MASK_GAP_CLOSE_FRACTION・_extract_island_skyline()の
      コメント参照。いずれも目視検証で判明した誤検出を修正した決め値)。
      自動検出が安定しない写真については、blender_msm_island.COMPARE_VIEWSの
      referencesエントリを文字列の代わりに辞書
      {"file": "xxx.jpg", "island_x_range": [x0, x1], "horizon_y": y}で書くことで、
      島の水平範囲(画像幅に対する比率、0=左端・1=右端)と水平線の位置(画像高さに対する
      比率、0=上端・1=下端)を手動指定できる(_extract_island_skyline_manual()参照。
      指定時は中央値推定・最長区間/面積最大区間の判定は行わず、指定範囲内だけで
      スカイラインを探す)。方位が他の写真と重複していて計測上の情報が増えない・
      構造的に自動検出が破綻する写真は、辞書に"metrics": Falseを足すことで並置画像・
      trace画像だけ作り指標(skyline_rms等)の算出をスキップできる(_parse_reference_entry()参照)
  (b-render) レンダ画像(blender_msm_island.py --compareの出力)は、地面を含まず背景が
      COMPARE_BACKGROUND_COLOR(マゼンタ)の単色であることを前提に、背景色からの色距離が
      閾値を超えるピクセル=島という厳密なマスクで列ごとの最上点を求める
      (_extract_island_render_mask()。輝度のしきい値・水平線の中央値推定は一切使わない。
      レンダに地面を混ぜると地面と空の境界(水平線)を島の輪郭と誤認する不具合があったため
      (詳細はblender_msm_island.COMPARE_BACKGROUND_COLORのコメント参照)、参考写真側の
      ロジックとは完全に分離した)
  (c) 島の幅を1.0・左端を0.0・水平線(レンダの場合は島の最下点)を高さ0とし、高さも
      島の幅で割って正規化した上で128点にリサンプリングする
  (d) 参考写真とレンダを並べた画像・スカイラインを重ねた画像・元画像に検出結果を
      焼き込んだ診断用トレース画像(<view>__<ref>__trace_ref.png /
      __trace_render.png。抽出が正しいかを人間が目視で判定するための出力)・
      指標をまとめたreport.txtを出力する(metrics=Falseの組は指標を計算せず、
      report.txtに対象外である旨だけ書く)

COMPARE_VIEWSの定義はTools/blender_msm_island.pyを流用する(import時にモデル生成が
走らないよう、blender_msm_island.py側は`if __name__ == "__main__":`でmain()を守っている)。
"""

import math
import os
import sys

import bpy
import numpy as np

# blender_msm_island.py(同じTools直下)をimportできるようにする
_TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
if _TOOLS_DIR not in sys.path:
    sys.path.insert(0, _TOOLS_DIR)

import blender_msm_island  # noqa: E402 - sys.path調整後のimportのため

# --- スカイライン抽出のパラメータ(承認済み計画の決め値) ---
SKY_BAND_FRACTION = 0.05          # 画像上部何%を「空」の輝度サンプルとして使うか
SKY_THRESHOLD_STD_MULT = 3.0      # しきい値 = sky_lum - max(3*sky_std, SKY_THRESHOLD_MIN_MARGIN)
SKY_THRESHOLD_MIN_MARGIN = 0.06
HORIZON_MARGIN_FRACTION = 0.03    # 水平線よりこの割合(画像高さ比)以上上のスカイラインだけを島とみなす
RESAMPLE_POINTS = 128

# 修正パス(コーディネーター指摘で置き換え): 以前はレンダ画像のスカイライン抽出にも
# 輝度+色距離のOR判定を使っていたが、根本原因は「--compareのレンダに地面が写り込んで
# 地面と空の境界(水平線)を島の輪郭と誤認していた」ことだったと判明した。地面を廃止し
# 背景をマゼンタ単色にしたことで、レンダ画像は_extract_island_render_mask()の厳密な
# 背景色距離マスクだけで判定できるようになったため、このOR判定(輝度しきい値方式の
# 補助)は不要になった。定数・ロジックは_extract_island_render_mask()側の
# RENDER_BACKGROUND_DIFF_THRESHOLDに置き換えた

# 修正パス(タスクA目視検証): south_mid__c_south_elevation_bluesky.jpg等、ゴシック建築の
# 細いピナクル(小尖塔)が林立する近距離写真では、隣り合う列でも「ピナクルの先端(高い)」と
# 「ピナクルの間から覗く奥の低い屋根(低い)」が交互に検出され、トレース画像で見ると
# 島の輪郭としてはあり得ない激しい垂直落下の連続に見える不具合があった。1列単位では
# 誤検出ではない(実際にその列の一番手前にある暗い物体を検出できている)が、低ポリゴンの
# レンダ側にはそこまで細かい凹凸が無く、128点への正規化・比較においては両者のスケールが
# 合わずノイズにしかならないため、島の区間内で水平方向(列方向)にメディアンフィルタを
# かけて、1〜数列だけの深い落ち込みを均す(区間幅に対する割合。出典なしの決め値)
SKYLINE_SMOOTH_FRACTION = 0.05
SKYLINE_SMOOTH_MIN_PX = 5

# レンダ画像専用: 背景色(blender_msm_island.COMPARE_BACKGROUND_COLOR、マゼンタ)からの
# 色距離がこの値を超えるピクセルを「島」とみなす。背景は単色でノイズがほぼ無いため、
# 島の石材・植生・空(この構図では映らない)がマゼンタにここまで近づくことは無いという
# 前提の決め値(RGB各成分0..1でユークリッド距離。背景との差0.15はどのマテリアルの色からも
# 十分離れている)
RENDER_BACKGROUND_DIFF_THRESHOLD = 0.15

# 修正パス(目視検証): 雲が多い空(sky_stdが大きい)では、単純に「しきい値を下回った最初の行」
# を境界とすると、雲の内部の暗い部分で一瞬しきい値を下回っただけの箇所を誤って島の輪郭として
# 拾ってしまい(pexels1.jpgで確認)、スカイラインがギザギザに壊れる不具合があった。
# しきい値を下回った状態がこの行数だけ連続して初めて境界と認めることでノイズを除去する
# (画像高さの0.5%、最低4px。出典なしの決め値)
SKYLINE_MIN_RUN_FRACTION = 0.005
SKYLINE_MIN_RUN_MIN_PX = 4

# 修正パス(目視検証): 複雑な建物のシルエット(尖塔は高いが低い城壁部分は水平線に近い)では、
# 「水平線より画像高さの3%以上上」という基準を一部の列だけ僅かに満たさず、本来ひとつづきの
# 島が細切れの区間に分断されてしまい、遠景の陸地・トンブレーヌ島など無関係な小さな高まりの方が
# 長い連続区間として誤検出される不具合があった(c_aerial_sw_daylight.jpg/aerial8.jpgで確認)。
# 島マスクの短い途切れ(この幅以下のFalseの区間)を埋めてから最長区間を探すことで、
# 遠く離れた無関係な陸地までは繋がず、同じ建造物内の僅かな途切れだけを橋渡しする
# (画像幅の2%。出典なしの決め値)
ISLAND_MASK_GAP_CLOSE_FRACTION = 0.02

# --- 出力画像のパラメータ ---
SKYLINE_IMAGE_WIDTH = 800
SKYLINE_IMAGE_HEIGHT = 500
SIDE_BY_SIDE_TARGET_HEIGHT = 600   # 並置画像で揃える高さ(px)
SIDE_BY_SIDE_SEPARATOR_WIDTH = 4   # 参考写真とレンダの間の区切り線の幅(px)


def _parse_args():
    """`--`より後ろの引数から --render-dir / --ref-dir / --out-dir を読む。"""
    argv = sys.argv
    if "--" in argv:
        argv = argv[argv.index("--") + 1:]
    else:
        argv = []

    render_dir = None
    ref_dir = None
    out_dir = None
    i = 0
    while i < len(argv):
        token = argv[i]
        if token == "--render-dir" and i + 1 < len(argv):
            render_dir = argv[i + 1]
            i += 2
        elif token == "--ref-dir" and i + 1 < len(argv):
            ref_dir = argv[i + 1]
            i += 2
        elif token == "--out-dir" and i + 1 < len(argv):
            out_dir = argv[i + 1]
            i += 2
        else:
            print(f"[WARNING] 未知の引数を無視しました: {token}", file=sys.stderr)
            i += 1

    return render_dir, ref_dir, out_dir


def _load_image_rgba(path):
    """画像を読み込み、arr[0]が画像の一番上の行になるようにした(H, W, 4)のfloat32配列を返す。

    Blenderのimage.pixelsは画像の一番下の行が先頭に来る並び(row0=底辺)のため、
    ここで上下反転してarr[0]=画像の一番上の行になるように揃える(スカイライン抽出・
    出力画像の生成のどちらも「上から下へ」の直感的な向きで扱えるようにするため)。
    """
    if not os.path.isfile(path):
        raise FileNotFoundError(f"画像が見つかりません: {path}")

    image = bpy.data.images.load(path, check_existing=False)
    try:
        width, height = image.size
        channels = image.channels
        if width <= 0 or height <= 0:
            raise ValueError(f"画像のサイズが不正です: {path} (size={image.size})")

        # 注意: Blender 2.82のImage.pixelsはforeach_get()を持たないため、スライスで
        # 一括取得する(要素ごとのPythonループより十分速い)
        buffer = np.array(image.pixels[:], dtype=np.float32)
        arr = buffer.reshape(height, width, channels)
        if channels == 3:
            alpha = np.ones((height, width, 1), dtype=np.float32)
            arr = np.concatenate([arr, alpha], axis=2)
        elif channels != 4:
            raise ValueError(f"想定外のチャンネル数です: {path} (channels={channels})")

        arr = arr[::-1, :, :].copy()  # 上下反転してarr[0]=一番上の行にする
        return arr
    finally:
        bpy.data.images.remove(image)


def _longest_true_run(mask):
    """1次元bool配列maskの中で、最長のTrue連続区間を(start, end_exclusive)で返す。

    Trueが1つも無ければNoneを返す。
    """
    best_start = None
    best_len = 0
    cur_start = None
    cur_len = 0
    for i, value in enumerate(mask):
        if value:
            if cur_start is None:
                cur_start = i
            cur_len += 1
            if cur_len > best_len:
                best_len = cur_len
                best_start = cur_start
        else:
            cur_start = None
            cur_len = 0

    if best_start is None:
        return None
    return best_start, best_start + best_len


def _all_true_runs(mask):
    """1次元bool配列maskの中の、すべてのTrue連続区間を(start, end_exclusive)のリストで返す。"""
    runs = []
    cur_start = None
    for i, value in enumerate(mask):
        if value:
            if cur_start is None:
                cur_start = i
        else:
            if cur_start is not None:
                runs.append((cur_start, i))
                cur_start = None
    if cur_start is not None:
        runs.append((cur_start, len(mask)))
    return runs


def _find_first_run_row(column_below_threshold, min_run):
    """1列分のbool配列(True=しきい値未満)の中で、min_run行以上連続してTrueが続く
    最初の開始行を返す。見つからなければ-1を返す。

    累積和で「開始行iからmin_run行分がすべてTrue」かどうかをO(1)判定できるようにし、
    先頭から順に判定する(列ごとにPythonループを回すこと自体はheight・widthの規模的に
    許容範囲だが、内側のmin_run行判定をnumpyの累積和で済ませて高速化する)。
    """
    height = column_below_threshold.shape[0]
    if min_run <= 1:
        first_index = int(np.argmax(column_below_threshold))
        return first_index if column_below_threshold[first_index] else -1

    if height < min_run:
        return -1

    cum = np.concatenate(([0], np.cumsum(column_below_threshold.astype(np.int32))))
    window_sums = cum[min_run:] - cum[:-min_run]  # window_sums[i] = column[i:i+min_run]の和
    qualifying = np.nonzero(window_sums == min_run)[0]
    if qualifying.size == 0:
        return -1
    return int(qualifying[0])


def _compute_non_sky_mask(arr):
    """参考写真(実写)専用: 画像配列(arr[0]=一番上の行)から「空でない」と判定する画素の
    bool配列(below_threshold)、および輝度・しきい値・空の輝度・輝度の標準偏差を求める。

    戻り値: (luminance(H,W), below_threshold(H,W) bool, threshold, sky_lum, sky_std)

    レンダ画像は背景が完全な単色(マゼンタ)のため、この輝度しきい値方式は使わず
    _extract_island_render_mask()の厳密な背景色距離マスクを使う(モジュールdocstring参照)。
    """
    height, width = arr.shape[:2]
    luminance = 0.2126 * arr[..., 0] + 0.7152 * arr[..., 1] + 0.0722 * arr[..., 2]

    top_rows = max(1, int(round(height * SKY_BAND_FRACTION)))
    top_band = luminance[:top_rows, :]
    sky_lum = float(np.median(top_band))
    sky_std = float(np.std(top_band))
    threshold = sky_lum - max(SKY_THRESHOLD_STD_MULT * sky_std, SKY_THRESHOLD_MIN_MARGIN)

    below_threshold = luminance < threshold

    return luminance, below_threshold, threshold, sky_lum, sky_std


def _median_smooth_1d(values, window):
    """1次元配列valuesへ、窓幅windowの移動中央値フィルタをかける(端は端値を延長してパディング)。

    windowが1以下ならそのまま返す。windowは奇数に丸める(中心を明確にするため)。
    """
    if window <= 1:
        return values.copy()
    if window % 2 == 0:
        window += 1

    n = len(values)
    half = window // 2
    padded = np.pad(values, (half, half), mode='edge')
    out = np.empty(n, dtype=np.float64)
    for i in range(n):
        out[i] = np.median(padded[i:i + window])
    return out


def _close_small_gaps(mask, max_gap):
    """1次元bool配列maskについて、True区間に挟まれた長さmax_gap以下のFalse区間をTrueに埋める。

    配列の端(片側だけTrueに接している、またはどちらもFalseに接している)区間は埋めない
    (無関係な陸地まで繋いでしまわないよう、両側がTrueの短い途切れだけを橋渡しする対象にする)。
    """
    mask = mask.copy()
    n = len(mask)
    i = 0
    while i < n:
        if mask[i]:
            i += 1
            continue
        j = i
        while j < n and not mask[j]:
            j += 1
        gap_len = j - i
        if i > 0 and j < n and gap_len <= max_gap:
            mask[i:j] = True
        i = j
    return mask


def _extract_island_skyline(arr):
    """参考写真(実写)専用: 画像配列(arr[0]=一番上の行)から島のスカイラインを抽出する。

    レンダ画像には_extract_island_render_mask()を使う(輝度しきい値方式ではなく
    背景色との厳密な色距離マスクを使う。モジュールdocstring参照)。
    戻り値: (left_col, right_col_exclusive, skyline_rows(長さW, 見つからない列は-1),
             horizon_row, threshold, sky_lum, sky_std)
    抽出できなかった場合はValueErrorを送出する。
    """
    height, width = arr.shape[:2]
    _luminance, below_threshold, threshold, sky_lum, sky_std = _compute_non_sky_mask(arr)

    # 修正パス: 単発のノイズ(雲の内部の暗い斑点など)で誤ってスカイラインと判定しないよう、
    # しきい値未満の状態が一定行数以上連続して初めて境界と認める
    min_run = max(SKYLINE_MIN_RUN_MIN_PX, int(round(height * SKYLINE_MIN_RUN_FRACTION)))

    skyline = np.full(width, -1, dtype=np.int64)
    for x in range(width):
        skyline[x] = _find_first_run_row(below_threshold[:, x], min_run)

    valid = skyline >= 0
    if not np.any(valid):
        raise ValueError(
            f"スカイラインが1本も検出できませんでした"
            f"(threshold={threshold:.4f}, sky_lum={sky_lum:.4f}, sky_std={sky_std:.4f}, min_run={min_run})"
        )

    horizon_row = float(np.median(skyline[valid]))
    island_mask = valid & (skyline.astype(np.float64) <= horizon_row - HORIZON_MARGIN_FRACTION * height)

    # 修正パス: 複雑な建物シルエットの一部の列だけが「水平線より十分上」の基準を僅かに
    # 満たさず本来ひとつづきの島が分断される問題への対処。短い途切れ(画像幅の一定割合以下)を
    # 埋めてから最長区間を探す
    gap_close_px = max(1, int(round(width * ISLAND_MASK_GAP_CLOSE_FRACTION)))
    closed_mask = _close_small_gaps(island_mask, gap_close_px)

    runs = _all_true_runs(closed_mask)
    if not runs:
        raise ValueError(
            f"水平線(row={horizon_row:.1f})より画像高さの{HORIZON_MARGIN_FRACTION * 100:.0f}%以上"
            f"上にあるスカイライン区間が見つかりませんでした"
        )

    # 修正パス(目視検証): 「最長の連続区間」だけを基準にすると、対岸の小さな陸地の方が
    # 建物の複雑なシルエットより連続区間が長くなり誤検出することがあった
    # (aerial8.jpg/pexels1.jpgで確認)。さらに「区間内の最高点(スカイライン行が最小の列)を
    # 含む区間を優先する」方式も試したが、画像端のJPEG圧縮ノイズ等で1列だけ極端に高い値
    # (row=0)を誤検出した場合に引きずられる不具合があった(aerial8.jpgで確認)。
    # そこで「水平線からの高さ(horizon_row - skyline_row)を区間内の全列で合計した面積」が
    # 最大の区間を選ぶ方式にした。これは「幅」と「高さ」の両方を考慮するため、1列だけの
    # ノイズにも、対岸の低くて幅だけがある陸地にも強い(モン・サン=ミシェル本体は他のどの
    # 写り込みよりも幅・高さの両方で優越しているという前提に基づく決め値)
    def _run_area(run_start, run_end):
        segment_valid = valid[run_start:run_end]
        segment_skyline = skyline[run_start:run_end]
        heights = np.where(segment_valid, horizon_row - segment_skyline, 0.0)
        return float(np.sum(np.clip(heights, 0.0, None)))

    run = max(runs, key=lambda r: _run_area(r[0], r[1]))

    left, right = run

    # _close_small_gapsで橋渡しした列は、しきい値未満の連続run自体は検出できていない
    # (skyline==-1のまま)ことがあるため、区間内にそうした列が残っていれば、区間内外の
    # 検出済み(valid)な列から線形補間して埋める(正規化・描画時に不正な値(-1)を
    # 使わないようにするため)
    skyline_filled = skyline.astype(np.float64)
    run_indices = np.arange(left, right)
    run_valid = valid[left:right]
    if not np.all(run_valid):
        valid_global_indices = np.nonzero(valid)[0]
        interpolated = np.interp(
            run_indices, valid_global_indices, skyline[valid_global_indices].astype(np.float64))
        local_values = skyline_filled[run_indices]
        local_values[~run_valid] = interpolated[~run_valid]
        skyline_filled[run_indices] = local_values

    # 修正パス(タスクA目視検証): ゴシック建築の細いピナクル群のような、レンダ側には無い
    # 高周波の凹凸で生じる1〜数列だけの深い落ち込みを、区間内でメディアンフィルタして均す
    smooth_window = max(SKYLINE_SMOOTH_MIN_PX, int(round((right - left) * SKYLINE_SMOOTH_FRACTION)))
    skyline_filled[left:right] = _median_smooth_1d(skyline_filled[left:right], smooth_window)

    return left, right, skyline_filled, horizon_row, threshold, sky_lum, sky_std


def _extract_island_skyline_manual(arr, island_x_range, horizon_y):
    """島の水平範囲・水平線をコーディネーターが実測した値で手動指定してスカイラインを抽出する。

    island_x_range: (x0, x1)。画像幅に対する比率(0=左端, 1=右端)
    horizon_y: 画像高さに対する比率(0=上端, 1=下端)
    自動検出(_extract_island_skyline)と異なり、中央値による水平線推定・
    最長区間/面積最大区間の判定は一切行わず、指定範囲内だけでスカイラインを探す。

    戻り値の形は_extract_island_skyline()と同じ(呼び出し側で分岐せずに使えるようにするため)。
    """
    height, width = arr.shape[:2]
    if len(island_x_range) != 2:
        raise ValueError(f"island_x_rangeは[x0, x1]の2要素である必要があります: {island_x_range}")

    x0, x1 = float(island_x_range[0]), float(island_x_range[1])
    if not (0.0 <= x0 < x1 <= 1.0):
        raise ValueError(f"island_x_rangeが不正です(0<=x0<x1<=1である必要があります): {island_x_range}")

    left = int(round(x0 * width))
    right = int(round(x1 * width))
    left = max(0, min(left, width - 1))
    right = max(left + 1, min(right, width))

    horizon_row = float(horizon_y) * height

    # 手動指定は参考写真専用のため、_compute_non_sky_mask()(輝度しきい値方式)を使う
    _luminance, below_threshold, threshold, sky_lum, sky_std = _compute_non_sky_mask(arr)
    min_run = max(SKYLINE_MIN_RUN_MIN_PX, int(round(height * SKYLINE_MIN_RUN_FRACTION)))
    skyline = np.full(width, -1, dtype=np.int64)
    for x in range(left, right):
        skyline[x] = _find_first_run_row(below_threshold[:, x], min_run)

    valid = skyline >= 0
    run_valid = valid[left:right]
    if not np.any(run_valid):
        raise ValueError(
            f"手動指定範囲[{left},{right})内でスカイラインが1本も検出できませんでした"
            f"(threshold={threshold:.4f}, sky_lum={sky_lum:.4f}, sky_std={sky_std:.4f}, min_run={min_run})"
        )

    # 指定範囲内で検出できなかった列は、範囲内の検出済みの列から線形補間して埋める
    skyline_filled = skyline.astype(np.float64)
    run_indices = np.arange(left, right)
    if not np.all(run_valid):
        valid_local_indices = run_indices[run_valid]
        interpolated = np.interp(
            run_indices, valid_local_indices, skyline[valid_local_indices].astype(np.float64))
        local_values = skyline_filled[run_indices]
        local_values[~run_valid] = interpolated[~run_valid]
        skyline_filled[run_indices] = local_values

    smooth_window = max(SKYLINE_SMOOTH_MIN_PX, int(round((right - left) * SKYLINE_SMOOTH_FRACTION)))
    skyline_filled[left:right] = _median_smooth_1d(skyline_filled[left:right], smooth_window)

    return left, right, skyline_filled, horizon_row, threshold, sky_lum, sky_std


def _extract_island_render_mask(arr, background_color):
    """レンダ画像(blender_msm_island.py --compareの出力)専用のスカイライン抽出。

    修正パス(コーディネーター指摘): 以前は参考写真と同じ輝度しきい値方式をレンダにも
    使っていたが、--compareのレンダに干潟の地面(PreviewGround)が写り込んでいたため、
    地面と空の境界(水平線)を島の輪郭と誤認する不具合があった(south_high/sw_high/
    se_highでwidth_px_renderが極端に小さくなる形で顕在化)。blender_msm_island.py側で
    --compareは地面を作らず背景をCOMPARE_BACKGROUND_COLOR(マゼンタ)単色にしたため、
    ここでは「背景色からの色距離が閾値を超えるピクセル=島」という厳密なマスクだけで
    判定する(輝度のしきい値・水平線の中央値推定・最長区間/面積最大区間の判定は一切不要)。

    戻り値の形は_extract_island_skyline()と同じ(呼び出し側で分岐せずに使えるようにするため)。
    horizon_row(正規化の基準となる高さ0の行)は、レンダには参考写真のような水平線が
    存在しないため、島のシルエット自身の最下点(裾野の一番下の行)を代用する。
    threshold/sky_lum/sky_stdはレンダには意味を持たないため、report.txtとの互換のため
    RENDER_BACKGROUND_DIFF_THRESHOLDとNaNを詰めて返す。
    """
    height, width = arr.shape[:2]
    background = np.array(background_color, dtype=np.float64)
    color_diff = np.linalg.norm(arr[..., :3].astype(np.float64) - background.reshape(1, 1, 3), axis=-1)
    is_island = color_diff > RENDER_BACKGROUND_DIFF_THRESHOLD

    skyline = np.full(width, -1, dtype=np.int64)
    for x in range(width):
        column = is_island[:, x]
        first_index = int(np.argmax(column))
        skyline[x] = first_index if column[first_index] else -1

    valid = skyline >= 0
    if not np.any(valid):
        raise ValueError(
            f"レンダ画像から島のピクセルが1つも検出できませんでした"
            f"(background_color={background_color}, threshold={RENDER_BACKGROUND_DIFF_THRESHOLD})"
        )

    valid_indices = np.nonzero(valid)[0]
    left = int(valid_indices[0])
    right = int(valid_indices[-1]) + 1

    # 区間内で検出できなかった列(背景に紛れて薄い等)は、区間内の検出済みの列から
    # 線形補間して埋める
    skyline_filled = skyline.astype(np.float64)
    run_indices = np.arange(left, right)
    run_valid = valid[left:right]
    if not np.all(run_valid):
        valid_local_indices = run_indices[run_valid]
        interpolated = np.interp(
            run_indices, valid_local_indices, skyline[valid_local_indices].astype(np.float64))
        local_values = skyline_filled[run_indices]
        local_values[~run_valid] = interpolated[~run_valid]
        skyline_filled[run_indices] = local_values

    # 水平線が存在しないため、島のシルエット自身の最下点(裾野)を高さ0の基準にする
    horizon_row = float(np.max(skyline_filled[left:right]))

    return left, right, skyline_filled, horizon_row, RENDER_BACKGROUND_DIFF_THRESHOLD, math.nan, math.nan


def _parse_reference_entry(ref_entry):
    """COMPARE_VIEWSのreferences要素(文字列 or 辞書)を正規化する。

    戻り値: {"file": ファイル名, "island_x_range": (x0,x1) or None, "horizon_y": float or None,
             "metrics": bool}
    辞書形式はisland_x_range/horizon_yを両方同時に指定する必要がある(片方だけの指定は
    仕様が曖昧になるためエラーにする)。"metrics": Falseを指定すると、並置画像・trace画像は
    作るが指標(skyline_rms等)の算出をスキップする(方位が他の写真と重複していて計測上の
    情報が増えない・自動検出が構造的に破綻する写真向け。省略時はTrue扱い)。
    """
    if isinstance(ref_entry, str):
        return {"file": ref_entry, "island_x_range": None, "horizon_y": None, "metrics": True}

    if isinstance(ref_entry, dict):
        if "file" not in ref_entry:
            raise ValueError(f"参照エントリに'file'キーがありません: {ref_entry!r}")
        island_x_range = ref_entry.get("island_x_range")
        horizon_y = ref_entry.get("horizon_y")
        if (island_x_range is None) != (horizon_y is None):
            raise ValueError(
                f"island_x_rangeとhorizon_yは両方同時に指定してください(片方だけは不可): {ref_entry!r}"
            )
        metrics = ref_entry.get("metrics", True)
        return {
            "file": ref_entry["file"], "island_x_range": island_x_range, "horizon_y": horizon_y,
            "metrics": metrics,
        }

    raise ValueError(f"参照エントリは文字列か辞書である必要があります: {ref_entry!r}")


def _normalize_and_resample(left, right, skyline, horizon_row):
    """島の区間[left, right)のスカイラインを、幅1.0・左端0.0・水平線=高さ0・
    高さも島の幅で割って正規化した上で、RESAMPLE_POINTS点に等間隔リサンプリングする。

    戻り値: (x_new(長さRESAMPLE_POINTS, 0..1), y_new(同じ長さ, 正規化済み高さ), width_px)
    """
    width_px = right - left
    if width_px <= 0:
        raise ValueError(f"島の区間の幅が0以下です(left={left}, right={right})")

    sub_skyline = skyline[left:right].astype(np.float64)
    y_orig = (horizon_row - sub_skyline) / width_px  # 高さも幅で正規化(縦横比を保つため)

    if width_px == 1:
        x_orig = np.array([0.0])
    else:
        x_orig = np.linspace(0.0, 1.0, width_px)

    x_new = np.linspace(0.0, 1.0, RESAMPLE_POINTS)
    y_new = np.interp(x_new, x_orig, y_orig)
    return x_new, y_new, width_px


def _resize_nearest(arr, new_height, new_width):
    """最近傍法でarr(H, W, C)をnew_height×new_widthへリサイズする。

    標準ライブラリ・bpy以外の画像処理ライブラリを追加しないための最小実装。
    """
    height, width = arr.shape[:2]
    row_idx = np.clip((np.arange(new_height) * height // max(new_height, 1)), 0, height - 1)
    col_idx = np.clip((np.arange(new_width) * width // max(new_width, 1)), 0, width - 1)
    return arr[row_idx][:, col_idx]


def _scale_to_height(arr, target_height):
    height, width = arr.shape[:2]
    target_width = max(1, round(width * target_height / height))
    return _resize_nearest(arr, target_height, target_width)


def _save_image_rgba(arr_top_first, path):
    """arr[0]=一番上の行、という向きの配列をPNGとして保存する。

    Blenderのimage.pixelsは一番下の行が先頭に来る並びのため、書き込み直前に上下反転する。
    """
    out_dir = os.path.dirname(path)
    try:
        os.makedirs(out_dir, exist_ok=True)
    except OSError as error:
        print(f"[ERROR] 出力ディレクトリの作成に失敗しました: {out_dir} ({error})", file=sys.stderr)
        raise

    height, width = arr_top_first.shape[:2]
    arr_bottom_first = arr_top_first[::-1, :, :]
    image_name = f"CompareOut_{os.path.basename(path)}"
    # 同名の既存データブロックが残っていると新規作成時に".001"等の連番が付き、
    # 保存先ファイル名とは無関係になるだけなので実害は無いが、念のため既存を消してから作る
    existing = bpy.data.images.get(image_name)
    if existing is not None:
        bpy.data.images.remove(existing)

    image = bpy.data.images.new(image_name, width=width, height=height, alpha=True)
    try:
        # 注意: Image.pixelsはforeach_set()を持たないため、スライス代入で一括設定する
        flat = np.clip(arr_bottom_first, 0.0, 1.0).astype(np.float32).reshape(-1)
        image.pixels[:] = flat.tolist()
        image.filepath_raw = path
        image.file_format = 'PNG'
        image.save()
    except Exception as error:  # noqa: BLE001
        print(f"[ERROR] 画像の保存に失敗しました: {path} ({error})", file=sys.stderr)
        raise
    finally:
        bpy.data.images.remove(image)

    print(f"wrote {path}")


def _draw_line(canvas, x_values, y_values, color, thickness=2):
    """canvas(H, W, 4)へ、既にピクセル座標へ変換済みの(col, row)の折れ線を描く。

    thickness: 太さ(px)。中心線から下方向へthickness-1px分太らせる簡易実装。
    """
    cols, rows = x_values, y_values
    height, width = canvas.shape[:2]
    thickness = max(1, int(thickness))
    for i in range(len(cols) - 1):
        x0, y0 = cols[i], rows[i]
        x1, y1 = cols[i + 1], rows[i + 1]
        steps = max(int(round(max(abs(x1 - x0), abs(y1 - y0)))), 1)
        for t in range(steps + 1):
            px = int(round(x0 + (x1 - x0) * t / steps))
            py = int(round(y0 + (y1 - y0) * t / steps))
            for dy in range(thickness):
                yy = py + dy
                if 0 <= yy < height and 0 <= px < width:
                    canvas[yy, px, 0:3] = color
                    canvas[yy, px, 3] = 1.0


def _make_skyline_image(x_ref, y_ref, x_render, y_render):
    """正規化後の座標系で、参考写真のスカイラインを赤、レンダのスカイラインを緑で
    重ねて描いた画像(黒背景、SKYLINE_IMAGE_WIDTH×SKYLINE_IMAGE_HEIGHT)を返す。
    """
    canvas = np.zeros((SKYLINE_IMAGE_HEIGHT, SKYLINE_IMAGE_WIDTH, 4), dtype=np.float32)
    canvas[..., 3] = 1.0

    all_y = np.concatenate([y_ref, y_render])
    y_max_plot = max(float(np.max(all_y)) * 1.15, 0.05)
    y_min_plot = min(0.0, float(np.min(all_y)))
    if y_max_plot - y_min_plot < 1.0e-6:
        y_max_plot = y_min_plot + 1.0

    def to_px(x_values, y_values):
        cols = x_values * (SKYLINE_IMAGE_WIDTH - 1)
        rows = (y_max_plot - y_values) / (y_max_plot - y_min_plot) * (SKYLINE_IMAGE_HEIGHT - 1)
        return cols, rows

    ref_cols, ref_rows = to_px(x_ref, y_ref)
    render_cols, render_rows = to_px(x_render, y_render)

    _draw_line(canvas, ref_cols, ref_rows, (1.0, 0.15, 0.15))
    _draw_line(canvas, render_cols, render_rows, (0.15, 1.0, 0.15))

    return canvas


def _make_side_by_side_image(ref_arr, render_arr):
    """参考写真とレンダを同じ高さに揃えて左右に並べた画像を返す(参考写真が左、レンダが右)。"""
    ref_scaled = _scale_to_height(ref_arr, SIDE_BY_SIDE_TARGET_HEIGHT)
    render_scaled = _scale_to_height(render_arr, SIDE_BY_SIDE_TARGET_HEIGHT)

    separator = np.zeros((SIDE_BY_SIDE_TARGET_HEIGHT, SIDE_BY_SIDE_SEPARATOR_WIDTH, 4), dtype=np.float32)
    separator[..., 0] = 1.0  # 目立つよう赤い区切り線にする
    separator[..., 3] = 1.0

    return np.concatenate([ref_scaled, separator, render_scaled], axis=1)


def _make_trace_image(arr, left, right, skyline, horizon_row, skyline_color):
    """元画像(arr[0]=一番上の行)の上に、検出したスカイライン・島の左右端(縦線)・
    水平線とみなした行(横線)を焼き込んだ診断用画像を返す。

    「抽出ロジックが正しく効いているか」を線だけの画像ではなく元の写真の上で目視確認できる
    ようにするための出力(タスクAの完了条件)。縦線・横線は半透明の青、スカイライン自体は
    引数skyline_colorの不透明な太線(2〜3px)で描く。
    """
    height, width = arr.shape[:2]
    canvas = arr.copy()
    canvas[..., 3] = 1.0

    overlay_color = np.array([0.2, 0.4, 1.0], dtype=np.float32)
    overlay_alpha = 0.45

    def blend_row(row_idx):
        row_idx = int(round(row_idx))
        if 0 <= row_idx < height:
            canvas[row_idx, :, 0:3] = (
                canvas[row_idx, :, 0:3] * (1.0 - overlay_alpha) + overlay_color * overlay_alpha
            )

    def blend_col(col_idx):
        col_idx = int(round(col_idx))
        if 0 <= col_idx < width:
            canvas[:, col_idx, 0:3] = (
                canvas[:, col_idx, 0:3] * (1.0 - overlay_alpha) + overlay_color * overlay_alpha
            )

    # 水平線とみなした行(横線)・島と判定した左右端(縦線)
    blend_row(horizon_row)
    blend_col(left)
    blend_col(max(left, right - 1))

    # 検出したスカイライン自体(区間[left, right)内)を太い線で描く
    cols = np.arange(left, right, dtype=np.float64)
    rows = skyline[left:right].astype(np.float64)
    if len(cols) >= 2:
        _draw_line(canvas, cols, rows, skyline_color, thickness=3)
    elif len(cols) == 1:
        _draw_line(canvas, np.array([cols[0], cols[0]]), np.array([rows[0], rows[0]]), skyline_color, thickness=3)

    return canvas


def _process_pair(view, ref_entry_info, render_arr_look, render_arr_mask, render_skyline_info,
                   ref_dir, out_dir, report_lines):
    """1つの(ビュー, 参考写真)の組を処理する。失敗時はreport_linesへ理由を書いてNoneを返す。

    ref_entry_infoは_parse_reference_entry()の戻り値(file/island_x_range/horizon_y)。
    island_x_range/horizon_yが両方指定されていれば手動指定モード(_extract_island_skyline_manual)、
    どちらもNoneなら自動検出モード(_extract_island_skyline)を使う。

    修正パス(タスクA): --compareがビューごとに見た目版(<name>.png)とマスク版
    (<name>_mask.png)の2枚を出力するようになったため、並置画像は見た目版
    (render_arr_look)、スカイライン抽出・指標・trace_render.pngはマスク版
    (render_arr_mask、render_skyline_infoも同じくマスク版から抽出済み)を使い分ける。
    """
    view_name = view["name"]
    ref_name = ref_entry_info["file"]
    is_manual = ref_entry_info["island_x_range"] is not None
    pair_id = f"{view_name}__{os.path.splitext(ref_name)[0]}"
    ref_path = os.path.join(ref_dir, ref_name)

    try:
        ref_arr = _load_image_rgba(ref_path)
    except Exception as error:  # noqa: BLE001
        message = f"[ERROR] 参考写真の読み込みに失敗したためスキップします: {ref_path} ({error})"
        print(message, file=sys.stderr)
        report_lines.append(f"{pair_id}: SKIPPED ({message})")
        return None

    try:
        if is_manual:
            ref_left, ref_right, ref_skyline, ref_horizon, ref_threshold, ref_sky_lum, ref_sky_std = (
                _extract_island_skyline_manual(
                    ref_arr, ref_entry_info["island_x_range"], ref_entry_info["horizon_y"])
            )
        else:
            ref_left, ref_right, ref_skyline, ref_horizon, ref_threshold, ref_sky_lum, ref_sky_std = (
                _extract_island_skyline(ref_arr)
            )
    except Exception as error:  # noqa: BLE001
        mode = "手動指定" if is_manual else "自動検出"
        message = f"[ERROR] 参考写真のスカイライン抽出({mode})に失敗したためスキップします: {ref_path} ({error})"
        print(message, file=sys.stderr)
        report_lines.append(f"{pair_id}: SKIPPED ({message})")
        return None

    render_left, render_right, render_skyline, render_horizon, _rt, _rl, _rs = render_skyline_info

    # (c-1) 並置画像(metrics=Falseでも作る)。見た目版(青空+地面)を使う
    try:
        side_by_side = _make_side_by_side_image(ref_arr, render_arr_look)
        _save_image_rgba(side_by_side, os.path.join(out_dir, f"{pair_id}__side_by_side.png"))
    except Exception as error:  # noqa: BLE001
        message = f"[ERROR] 並置画像の生成に失敗しました: {pair_id} ({error})"
        print(message, file=sys.stderr)
        report_lines.append(f"{pair_id}: SKIPPED ({message})")
        return None

    # (c-2) 診断用トレース画像(タスクA。metrics=Falseでも作る): 元画像の上に検出結果を
    # 焼き込み、抽出が正しく効いているかを人間が目視で判定できるようにする。
    # レンダ側はマスク版(render_arr_mask)を使う(スカイライン抽出もマスク版から行っているため)
    try:
        ref_trace = _make_trace_image(ref_arr, ref_left, ref_right, ref_skyline, ref_horizon, (1.0, 0.0, 0.0))
        _save_image_rgba(ref_trace, os.path.join(out_dir, f"{pair_id}__trace_ref.png"))
        render_trace = _make_trace_image(
            render_arr_mask, render_left, render_right, render_skyline, render_horizon, (0.0, 1.0, 0.0))
        _save_image_rgba(render_trace, os.path.join(out_dir, f"{pair_id}__trace_render.png"))
    except Exception as error:  # noqa: BLE001
        message = f"[ERROR] トレース画像の生成に失敗しました: {pair_id} ({error})"
        print(message, file=sys.stderr)
        report_lines.append(f"{pair_id}: SKIPPED ({message})")
        return None

    # 修正パス(コーディネーター指摘): pexels1.jpg/aerial8.jpgのように、島以外の遠景の
    # 陸地を拾ってしまう構造的な破綻がある・他の写真と方位が重複していて計測上の情報が
    # 増えない写真は、metrics=Falseにして並置画像・trace画像だけ作り指標は計算しない
    if not ref_entry_info["metrics"]:
        report_lines.append(f"{pair_id}: (指標対象外: 島以外の陸地を拾うため)")
        return None

    x_ref, y_ref, width_px_ref = _normalize_and_resample(ref_left, ref_right, ref_skyline, ref_horizon)
    x_render, y_render, width_px_render = _normalize_and_resample(
        render_left, render_right, render_skyline, render_horizon)

    skyline_rms = float(np.sqrt(np.mean((y_ref - y_render) ** 2)))

    peak_idx_ref = int(np.argmax(y_ref))
    peak_idx_render = int(np.argmax(y_render))
    peak_position_ref = float(x_ref[peak_idx_ref])
    peak_position_render = float(x_render[peak_idx_render])
    peak_height_ref = float(y_ref[peak_idx_ref])
    peak_height_render = float(y_render[peak_idx_render])

    peak_position_diff = abs(peak_position_ref - peak_position_render)
    peak_height_ratio = (
        peak_height_render / peak_height_ref if abs(peak_height_ref) > 1.0e-9 else math.nan
    )

    # (c-3) スカイライン重ね描画画像(metrics=Trueの組のみ)
    try:
        skyline_image = _make_skyline_image(x_ref, y_ref, x_render, y_render)
        _save_image_rgba(skyline_image, os.path.join(out_dir, f"{pair_id}__skyline.png"))
    except Exception as error:  # noqa: BLE001
        message = f"[ERROR] スカイライン画像の生成に失敗しました: {pair_id} ({error})"
        print(message, file=sys.stderr)
        report_lines.append(f"{pair_id}: SKIPPED ({message})")
        return None

    mode_note = "manual" if is_manual else "auto"
    report_lines.append(
        f"{pair_id}: (ref_mode={mode_note})\n"
        f"  width_px_ref={width_px_ref} width_px_render={width_px_render}\n"
        f"  peak_position: ref={peak_position_ref:.4f} render={peak_position_render:.4f} "
        f"diff={peak_position_diff:.4f}\n"
        f"  peak_height_ratio: ref={peak_height_ref:.4f} render={peak_height_render:.4f} "
        f"render/ref={peak_height_ratio:.4f}\n"
        f"  skyline_rms={skyline_rms:.4f}\n"
        f"  (ref: threshold={ref_threshold:.4f} sky_lum={ref_sky_lum:.4f} sky_std={ref_sky_std:.4f} "
        f"island_cols=[{ref_left},{ref_right}))"
    )
    return skyline_rms


def main():
    render_dir, ref_dir, out_dir = _parse_args()

    if render_dir is None or ref_dir is None or out_dir is None:
        print(
            "[ERROR] --render-dir / --ref-dir / --out-dir はすべて必須です"
            f"(render_dir={render_dir}, ref_dir={ref_dir}, out_dir={out_dir})",
            file=sys.stderr,
        )
        raise SystemExit(1)

    render_dir = os.path.abspath(render_dir)
    ref_dir = os.path.abspath(ref_dir)
    out_dir = os.path.abspath(out_dir)

    try:
        os.makedirs(out_dir, exist_ok=True)
    except OSError as error:
        print(f"[ERROR] 比較結果の出力ディレクトリの作成に失敗しました: {out_dir} ({error})", file=sys.stderr)
        raise

    report_lines = []
    all_rms = []

    for view in blender_msm_island.COMPARE_VIEWS:
        view_name = view["name"]
        # 修正パス(タスクA): --compareがビューごとに見た目版(<name>.png、青空+地面)と
        # マスク版(<name>_mask.png、マゼンタ背景+地面無し)の2枚を出力するようになった。
        # 並置画像には見た目版、スカイライン抽出・指標・trace_render.pngにはマスク版を使う
        render_look_path = os.path.join(render_dir, f"{view_name}.png")
        render_mask_path = os.path.join(render_dir, f"{view_name}_mask.png")

        if not os.path.isfile(render_look_path) or not os.path.isfile(render_mask_path):
            message = (
                f"[ERROR] レンダ画像(見た目版/マスク版)が見つからないためビュー全体をスキップします: "
                f"look={render_look_path} mask={render_mask_path}"
            )
            print(message, file=sys.stderr)
            report_lines.append(f"{view_name}: SKIPPED ({message})")
            continue

        try:
            render_arr_look = _load_image_rgba(render_look_path)
            render_arr_mask = _load_image_rgba(render_mask_path)
        except Exception as error:  # noqa: BLE001
            message = (
                f"[ERROR] レンダ画像の読み込みに失敗したためビュー全体をスキップします: "
                f"look={render_look_path} mask={render_mask_path} ({error})"
            )
            print(message, file=sys.stderr)
            report_lines.append(f"{view_name}: SKIPPED ({message})")
            continue

        try:
            # レンダ画像(マスク版)は背景がCOMPARE_BACKGROUND_COLOR(マゼンタ)単色・
            # 地面無しのため、参考写真とは別の厳密な背景色距離マスクで抽出する
            # (モジュールdocstring参照)
            render_skyline_info = _extract_island_render_mask(
                render_arr_mask, blender_msm_island.COMPARE_BACKGROUND_COLOR)
        except Exception as error:  # noqa: BLE001
            message = (
                f"[ERROR] レンダ画像のスカイライン抽出に失敗したためビュー全体をスキップします: "
                f"{render_mask_path} ({error})"
            )
            print(message, file=sys.stderr)
            report_lines.append(f"{view_name}: SKIPPED ({message})")
            continue

        for ref_entry in view["references"]:
            try:
                ref_entry_info = _parse_reference_entry(ref_entry)
            except Exception as error:  # noqa: BLE001
                message = f"[ERROR] 参照エントリの解釈に失敗したためスキップします: {ref_entry!r} ({error})"
                print(message, file=sys.stderr)
                report_lines.append(f"{view_name}__(不明な参照): SKIPPED ({message})")
                continue

            ref_name = ref_entry_info["file"]
            if not os.path.isfile(os.path.join(ref_dir, ref_name)):
                message = f"[ERROR] 参考写真が見つからないためスキップします: {os.path.join(ref_dir, ref_name)}"
                print(message, file=sys.stderr)
                report_lines.append(f"{view_name}__{os.path.splitext(ref_name)[0]}: SKIPPED ({message})")
                continue

            rms = _process_pair(
                view, ref_entry_info, render_arr_look, render_arr_mask, render_skyline_info,
                ref_dir, out_dir, report_lines)
            if rms is not None:
                all_rms.append(rms)

    report_path = os.path.join(out_dir, "report.txt")
    try:
        with open(report_path, "w", encoding="utf-8") as f:
            f.write("KurenaiEngine MSM島 参考写真突き合わせレポート\n")
            f.write(f"render_dir={render_dir}\n")
            f.write(f"ref_dir={ref_dir}\n")
            f.write("=" * 60 + "\n\n")
            f.write("\n\n".join(report_lines))
            f.write("\n\n" + "=" * 60 + "\n")
            if all_rms:
                f.write(
                    f"skyline_rms 平均(成功しmetrics=Trueの組のみ, {len(all_rms)}組): "
                    f"{sum(all_rms) / len(all_rms):.4f}\n"
                )
            else:
                f.write("成功した組がありませんでした\n")
    except OSError as error:
        print(f"[ERROR] report.txtの書き込みに失敗しました: {report_path} ({error})", file=sys.stderr)
        raise

    print(f"wrote {report_path}")


if __name__ == "__main__":
    main()
