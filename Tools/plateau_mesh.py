# -*- coding: utf-8 -*-
"""Project PLATEAU のFBXタイルを取り込むための補助スクリプト。

メッシュコード(ファイル名の先頭)から、そのタイルが平面直角座標系のどこに来るかを
計算する。用途は3つ:

  1. タイル全体の中心を求めて、KurenaiPacker の --origin へ渡す共通オフセットを決める
  2. 取り込んだ .kmodel の AABB が期待した位置に来ているかを検算する
     (assimp の軸変換と aiProcess_ConvertToLeftHanded を通ると、東西・南北がどちらの軸に
      なるか・符号がどちらかが自明でない。街が鏡像になっても一見気づけないため必ず突き合わせる)
  3. FBX を KurenaiPacker --inspect に掛けて、実AABBを期待矩形と機械的に突き合わせる
     (verify サブコマンド)

【コードの桁数は種別で違う】bldg と brid は8桁=3次メッシュ(約1km四方)、
dem と tran は6桁=2次メッシュ(約10km四方)。どちらも扱えるようにしてある。

【ファイル名の系番号(_6677 / _6697)は信用しない】brid の68件のうち13件は _6697 を
名乗るが、**中身は他と同じ第9系のメートル値である**(全68件を実測して確認済み)。
名前を根拠にせず verify サブコマンドの実測で確かめること。
なお「_6697 は元CityGMLのCRSを指す」という説明はPLATEAUの命名規約についての伝聞で、
このスクリプトで測って言えるのは「中身が系9のメートルである」ところまで。

【verify では --origin の適用は確かめられない】KurenaiPacker の --inspect は
--origin を無視して常に適用前の絶対座標を印字する(Main.cpp の inspect 分岐は
InspectModel を呼んで return するため、OriginOffset が渡らない)。
検算にはむしろ好都合だが、「--origin が効いたか」は .kmodel のヘッダAABBで見ること。

PLATEAU の FBX は EPSG:6677(JGD2011 平面直角座標系 第9系)の絶対座標で、単位はメートル。
系9の原点は 北緯36度・東経139度50分。

使い方:
    python Tools/plateau_mesh.py origin <FBXディレクトリ> [<FBXディレクトリ>...]
    python Tools/plateau_mesh.py rect <メッシュコード>
    python Tools/plateau_mesh.py verify <FBXディレクトリ> --packer <KurenaiPacker.exe>
"""
import glob
import math
import os
import re
import subprocess
import sys

# 平面直角座標系 第9系の原点(測量法施行令)
ORIGIN_LAT = 36.0
ORIGIN_LON = 139.0 + 50.0 / 60.0

# 判定のしきい値。「実AABBの中心が期待矩形の外へ出た量」を、そのタイル自身の半分の幅と比べる。
#
# 【なぜ中心と矩形の距離で測るのか】最初は「タイル中心と実AABB中心のずれ」で測ったが、
# それでは brid の68件中34件が超えた。ずれは 300〜609m で、これはタイル1辺(約1km)の
# 半分に相当する ―― 橋はタイルの一部にしか無いので、中身の中心がタイル中心から
# 半タイルぶんずれるのは正常である。「中心どうしのずれ」は中身が疎なデータでは意味を持たない。
# 中心が期待矩形の内側にあれば0、外に出た分だけ増える指標に変えた。
#
# 【なぜ絶対値[m]ではなくタイル幅に対する比なのか】次に絶対値300mで判定したところ、
# **既に出荷済みで正しく描けている bldg/lod1 の671タイル**が1件落ちた
# (`53392641`、中心外れ 363.9m)。同じコードの LOD2 も brid も水平方向のAABBが同一で、
# 「そのタイルの中身が自分の矩形より北へ820mはみ出している」という配布物の性質である。
# **既知の正しいデータで落ちる判定はゲートとして使えない。**
# またタイルの大きさは種別で10倍違う(bldg/brid は3次メッシュ約1km、dem/tran は2次メッシュ
# 約10km)ので、絶対値では両方に合う値が無い。
#
# 【しきい値2.0の根拠と、この判定で捕まえられないもの】
# 「中心の外れ量 ÷ そのタイルの半分の幅」を比として測り、実測で次の3点が分かっている:
#
#   | 対象                                          | 半タイル比 |
#   |-----------------------------------------------|-----------:|
#   | 既に出荷済みで正しく描けている bldg/lod1 671件 |  最大 0.79 |
#   | 対照実験: 実AABBを隣のタイルのコードと突合     |       1.01 |
#   | 対照実験: 実AABBを遠くのタイルのコードと突合   |      35.18 |
#
# **0.79(正しいデータ)と 1.01(1タイルずれ)の間に余裕が無い。**
# したがって「1タイル(約1km)だけずれている」は、誤検出なしには捕まえられない。
# この判定が確実に落とせるのは**座標系そのものの取り違え**で、その場合は
# 別の系なら数km〜数十km、緯度経度なら値が度のオーダー(139前後)になり、比は2桁になる。
# 2.0 は「正しいデータの最悪値 0.79 の2.5倍」かつ「取り違えの 35 の1/17」で、両側に余裕がある。
#
# 【1タイルずれを捕まえたいなら】このスクリプトではなく、隣接タイル同士の
# AABBが連続しているか(隙間や重なりが無いか)を見る別の検査が要る。今は必要としていない。
DEFAULT_LIMIT_RATIO = 2.0


def mesh_to_latlon_rect(code):
    """メッシュコード -> (lat_min, lat_max, lon_min, lon_max)

    8桁は3次メッシュ(約1km四方、bldg/brid)、6桁は2次メッシュ(約10km四方、dem/tran)。
    """
    if not code.isdigit() or len(code) not in (6, 8):
        raise ValueError('メッシュコードは6桁または8桁の数字: %r' % code)

    p, q = int(code[0:2]), int(code[2:4])
    r, s = int(code[4]), int(code[5])
    if len(code) == 8:
        t, u = int(code[6]), int(code[7])
        lat_span = (2.0 / 3.0) / 80.0
        lon_span = 1.0 / 80.0
    else:
        t, u = 0, 0
        lat_span = (2.0 / 3.0) / 8.0
        lon_span = 1.0 / 8.0

    lat = (p + r / 8.0 + t / 80.0) * 2.0 / 3.0
    lon = (q + 100.0) + s / 8.0 + u / 80.0
    return lat, lat + lat_span, lon, lon + lon_span


# 旧名。8桁専用だった頃の呼び出しが残っていても動くように残す
def mesh3_to_latlon_rect(code):
    if len(code) != 8:
        raise ValueError('3次メッシュコードは8桁の数字: %r' % code)
    return mesh_to_latlon_rect(code)


def latlon_to_plane9(lat, lon):
    """簡易な平面直角座標(第9系)。戻り値は (北[m], 東[m])。

    厳密なガウス・クリューゲル投影ではなく、原点周りの局所的な線形近似。
    ここでの用途は「タイル群の中心を数百m精度で決める」「取り込み後の位置と桁・符号を
    突き合わせる」ことなので、この精度で足りる。
    厳密な値が要るなら pyproj 等で EPSG:6677 へ変換すること。

    【誤差を実測した】国土地理院の標準式(GRS80のガウス・クリューゲル順変換、級数展開)を
    別に実装して、23区の全685メッシュコードの四隅1370点で比べた結果:

        北の誤差: -129.4 〜 -31.7 m (平均 -65.9)   タイル間の相対ずれ 97.7 m
        東の誤差:  -15.7 〜 +31.4 m (平均  +9.1)   タイル間の相対ずれ 47.1 m

    原因は2つで、どちらも定量できている:
      (1) 35.65°N での子午線1度は約 110949m なのに 111132.0 を使っている(1度あたり +183m)
      (2) 縮尺係数 m0 = 0.9999 を掛けていない

    **一定のオフセットではなく、緯度によって変わる**ことに注意(北で最大97.7mの相対差)。

    【それでも直していない理由】この関数の出力から `--origin -8096,0,-36118` が決まっており、
    直すと原点が数十mずれる。原点は**全タイルで同じ値であること**だけが要件で、
    絶対値が真の系9とずれていても街は1mも歪まない(FBXの頂点は真の系9のまま。
    近似が効くのは「期待矩形」の側だけで、街のジオメトリには一切触れない)。
    一方で原点を変えると、既にパック済みの .kmodel と新しくパックしたものが混ざらなくなる。
    **この誤差を承知のうえで固定する。**メートル精度の物差しとして使わないこと。

    verify の判定への影響: 期待矩形が最大98m動くが、しきい値は半タイル幅の2倍
    (3次メッシュで約1.85km)なので、判定を覆すには桁が足りない。
    """
    north = (lat - ORIGIN_LAT) * 111132.0
    east = (lon - ORIGIN_LON) * 111320.0 * math.cos(math.radians(lat))
    return north, east


def tile_rect(code):
    """メッシュコード -> {'north': (min, max), 'east': (min, max)}"""
    a, b, c, d = mesh_to_latlon_rect(code)
    n0, e0 = latlon_to_plane9(a, c)
    n1, e1 = latlon_to_plane9(b, d)
    return {
        'north': (min(n0, n1), max(n0, n1)),
        'east': (min(e0, e1), max(e0, e1)),
    }


def scan_tiles(directories):
    """FBXを走査して、メッシュコードと平面直角座標の範囲を返す。

    directories は文字列でもリストでもよい。複数種別(bldg/dem/tran/brid)を
    まとめて渡せば、全体を覆う共通原点が求まる。
    """
    if isinstance(directories, str):
        directories = [directories]

    tiles = []
    for directory in directories:
        paths = sorted(glob.glob(os.path.join(directory, '*.fbx')))
        if not paths:
            print('  [WARN] FBXが1つも無いディレクトリ: %s' % directory, file=sys.stderr)
        for path in paths:
            name = os.path.basename(path)
            code = name.split('_')[0]
            try:
                rect = tile_rect(code)
            except ValueError:
                print('  [SKIP] メッシュコードとして解釈できない: %s' % name, file=sys.stderr)
                continue
            tiles.append({'code': code, 'path': path,
                          'north': rect['north'], 'east': rect['east']})
    return tiles


# --inspect の「バウンズ」ブロックから min/max を拾う。
# 出力例:
#     min = (-8130.487, 9.799, -37944.723)
#     max = (-8106.179, 15.796, -37915.453)
_BOUNDS_RE = re.compile(
    r'^\s*(min|max)\s*=\s*\(\s*([-\d.eE+]+)\s*,\s*([-\d.eE+]+)\s*,\s*([-\d.eE+]+)\s*\)\s*$')


def inspect_bounds(packer, fbx_path):
    """KurenaiPacker --inspect を回して (min[3], max[3]) を返す。取れなければ None。

    軸は「X=東 / Y=標高 / Z=北」(下の origin のコメント参照)。
    """
    try:
        completed = subprocess.run(
            [packer, fbx_path, '--inspect'],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            encoding='utf-8', errors='replace')
    except OSError as error:
        print('  [ERROR] KurenaiPackerを起動できない: %s (%s)' % (packer, error), file=sys.stderr)
        return None

    if completed.returncode != 0:
        print('  [ERROR] --inspect が終了コード %d で失敗: %s'
              % (completed.returncode, os.path.basename(fbx_path)), file=sys.stderr)
        return None

    found = {}
    for line in completed.stdout.splitlines():
        matched = _BOUNDS_RE.match(line)
        if matched:
            found[matched.group(1)] = [float(matched.group(i)) for i in (2, 3, 4)]
        if 'min' in found and 'max' in found:
            return found['min'], found['max']

    print('  [ERROR] --inspect の出力からバウンズを読めない: %s'
          % os.path.basename(fbx_path), file=sys.stderr)
    return None


def verify_tile(code, bounds_min, bounds_max):
    """実AABBと期待矩形の食い違いを返す。

    戻り値は dict:
      outside_east / outside_north : 実AABBの中心が期待矩形の外へ出た量[m](内側なら0)
      overhang_east / overhang_north : 実AABBが期待矩形からはみ出した量[m](参考値)
      deviation : 中心の外れ量[m](東西・南北の合成)
      ratio     : 判定に使う値。外れ量をそのタイルの半分の幅で割ったもの(1.0でしきい値)

    判定の考え方は DEFAULT_LIMIT_RATIO のコメント参照。
    はみ出し(overhang)は、タイルをまたぐ長い橋では正常に数百m出るので判定には使わない。
    """
    rect = tile_rect(code)
    # X=東 / Y=標高 / Z=北
    actual = {'east': (bounds_min[0], bounds_max[0]),
              'north': (bounds_min[2], bounds_max[2])}

    result = {}
    ratio = 0.0
    for axis in ('east', 'north'):
        lo, hi = rect[axis]
        alo, ahi = actual[axis]
        center = (alo + ahi) / 2.0
        half_width = (hi - lo) / 2.0
        outside = max(0.0, lo - center, center - hi)
        result['overhang_' + axis] = max(0.0, lo - alo, ahi - hi)
        result['outside_' + axis] = outside
        if half_width > 0.0:
            ratio = max(ratio, outside / half_width)
    result['deviation'] = math.hypot(result['outside_east'], result['outside_north'])
    result['ratio'] = ratio
    result['rect'] = rect
    result['actual'] = actual
    return result


def cmd_rect(argv):
    code = argv[0]
    a, b, c, d = mesh_to_latlon_rect(code)
    n0, e0 = latlon_to_plane9(a, c)
    n1, e1 = latlon_to_plane9(b, d)
    print('メッシュ %s (%d桁 = %s次メッシュ)' % (code, len(code), '3' if len(code) == 8 else '2'))
    print('  緯度 %.5f 〜 %.5f / 経度 %.5f 〜 %.5f' % (a, b, c, d))
    print('  系9  北 %+.0f 〜 %+.0f m / 東 %+.0f 〜 %+.0f m' % (n0, n1, e0, e1))
    return 0


def cmd_origin(argv):
    tiles = scan_tiles(argv)
    if not tiles:
        print('[ERROR] FBXが1つも見つからない: %s' % ' '.join(argv), file=sys.stderr)
        return 1
    north_min = min(t['north'][0] for t in tiles)
    north_max = max(t['north'][1] for t in tiles)
    east_min = min(t['east'][0] for t in tiles)
    east_max = max(t['east'][1] for t in tiles)
    cn = (north_min + north_max) / 2.0
    ce = (east_min + east_max) / 2.0
    print('タイル数: %d' % len(tiles))
    print('  北: %+.0f 〜 %+.0f m (幅 %.1f km)' % (north_min, north_max, (north_max - north_min) / 1000.0))
    print('  東: %+.0f 〜 %+.0f m (幅 %.1f km)' % (east_min, east_max, (east_max - east_min) / 1000.0))
    print('  中心: 北 %+.0f / 東 %+.0f' % (cn, ce))
    # 原点を引いたあとの水平方向の広がり(farZ はシーンAABBの対角から決まるので参考値)
    half_e = (east_max - east_min) / 2.0
    half_n = (north_max - north_min) / 2.0
    print('  原点を引いた後の水平対角: %.0f m' % (2.0 * math.hypot(half_e, half_n)))
    # 【軸の対応は実測で確定させた】FBX自体はZ-up(UpAxis=2)で
    # 「FBX X = 東 / FBX Y = 北 / FBX Z = 標高」だが、assimpがZ-up->Y-upへ変換し、
    # さらに aiProcess_ConvertToLeftHanded が入るため、KurenaiPackerが扱う時点では
    #   X = 東 / Y = 標高 / Z = 北
    # になる。--origin はこの変換後の座標から引かれるので、東・標高・北の順に渡す。
    #
    # 西新宿タイル(53394525)を --inspect した実測値:
    #   X -13218.99〜-12034.59 (期待した東 -13186〜-12055 と一致)
    #   Y     27.34〜  267.02  (T.P.標高。都庁243mを含む)
    #   Z -35171.98〜-34122.55 (期待した北 -35192〜-34266 と一致)
    # 符号は反転していない。街が鏡像になっていないことをここで確定させている。
    #
    # 標高(Y)は引かない ―― T.P.基準の絶対標高で23区は概ね0〜60mと小さく、
    # 引くと地面が原点より下へ潜って扱いにくくなるため
    print()
    print('  --origin %.0f,0,%.0f' % (ce, cn))
    return 0


def cmd_verify(argv):
    """FBXの実AABBを期待矩形と突き合わせる。

    パックする前に走らせること。1件でも系が違えば、そのタイルだけ数十km飛んだ街になる。
    """
    directories = []
    packer = None
    limit_ratio = DEFAULT_LIMIT_RATIO
    i = 0
    while i < len(argv):
        if argv[i] == '--packer':
            i += 1
            if i >= len(argv):
                print('[ERROR] --packer に値がありません', file=sys.stderr)
                return 1
            packer = argv[i]
        elif argv[i] == '--limit-ratio':
            i += 1
            if i >= len(argv):
                print('[ERROR] --limit-ratio に値がありません', file=sys.stderr)
                return 1
            limit_ratio = float(argv[i])
        else:
            directories.append(argv[i])
        i += 1

    if not directories or packer is None:
        print('[ERROR] 使い方: verify <FBXディレクトリ>... --packer <KurenaiPacker.exe>', file=sys.stderr)
        return 1
    if not os.path.isfile(packer):
        print('[ERROR] KurenaiPacker.exe が見つかりません: %s' % packer, file=sys.stderr)
        return 1

    paths = []
    for directory in directories:
        paths.extend(sorted(glob.glob(os.path.join(directory, '*.fbx'))))
    if not paths:
        print('[ERROR] FBXが1つも見つからない: %s' % ' '.join(directories), file=sys.stderr)
        return 1

    print('%d件を検証します(しきい値: 中心外れ / 半タイル幅 <= %.2f)' % (len(paths), limit_ratio))
    print('%-10s %-6s %10s %8s %10s %10s  %s'
          % ('コード', '系番号', '中心外れm', '半タイル比', 'はみ出し東', 'はみ出し北', '判定'))

    failures = []
    unreadable = []
    worst = 0.0
    by_suffix = {}
    for path in paths:
        name = os.path.basename(path)
        parts = os.path.splitext(name)[0].split('_')
        code = parts[0]
        suffix = parts[-1] if len(parts) >= 3 else '-'
        try:
            tile_rect(code)
        except ValueError:
            print('  [SKIP] メッシュコードとして解釈できない: %s' % name, file=sys.stderr)
            continue

        bounds = inspect_bounds(packer, path)
        if bounds is None:
            unreadable.append(name)
            continue

        r = verify_tile(code, bounds[0], bounds[1])
        worst = max(worst, r['ratio'])
        ok = r['ratio'] <= limit_ratio
        if not ok:
            failures.append((name, r['deviation'], r['ratio']))
        by_suffix.setdefault(suffix, []).append((r['deviation'], r['ratio']))
        print('%-10s %-6s %10.1f %8.2f %10.1f %10.1f  %s'
              % (code, suffix, r['deviation'], r['ratio'],
                 r['overhang_east'], r['overhang_north'], 'OK' if ok else '**NG**'))

    print()
    # ファイル名の系番号ごとに分けて出す。_6697 を名乗る13件が本当に他と同じ系なのかは、
    # 「全体が通った」ではなく「系番号で分けても差が無い」で示す必要がある
    for suffix in sorted(by_suffix):
        values = by_suffix[suffix]
        metres = [v[0] for v in values]
        print('  系番号 _%s: %d件 / 中心外れ 最大 %.1f m 平均 %.1f m'
              % (suffix, len(values), max(metres), sum(metres) / len(metres)))
    print()
    print('最大の半タイル比: %.2f / しきい値 %.2f' % (worst, limit_ratio))
    if unreadable:
        print('[ERROR] --inspect で読めなかったファイル %d件: %s'
              % (len(unreadable), ', '.join(unreadable)), file=sys.stderr)
    if failures:
        print('[ERROR] 期待矩形から外れたタイル %d件:' % len(failures), file=sys.stderr)
        for name, deviation, ratio in failures:
            print('  %s (中心外れ %.1f m / 半タイル比 %.2f)' % (name, deviation, ratio),
                  file=sys.stderr)
        return 1
    if unreadable:
        return 1
    print('全件が期待矩形の中に収まっています。同じ --origin で混ぜて構いません')
    return 0


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1

    command = sys.argv[1]
    if command == 'rect':
        return cmd_rect(sys.argv[2:])
    if command == 'origin':
        return cmd_origin(sys.argv[2:])
    if command == 'verify':
        return cmd_verify(sys.argv[2:])

    print(__doc__)
    return 1


if __name__ == '__main__':
    sys.exit(main())
