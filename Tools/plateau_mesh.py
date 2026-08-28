# -*- coding: utf-8 -*-
"""Project PLATEAU のFBXタイルを取り込むための補助スクリプト。

3次メッシュコード(ファイル名の先頭8桁)から、そのタイルが平面直角座標系のどこに来るかを
計算する。用途は2つ:

  1. 671タイル全体の中心を求めて、KurenaiPacker の --origin へ渡す共通オフセットを決める
  2. 取り込んだ .kmodel の AABB が期待した位置に来ているかを検算する
     (assimp の軸変換と aiProcess_ConvertToLeftHanded を通ると、東西・南北がどちらの軸に
      なるか・符号がどちらかが自明でない。街が鏡像になっても一見気づけないため必ず突き合わせる)

PLATEAU の FBX は EPSG:6677(JGD2011 平面直角座標系 第9系)の絶対座標で、単位はメートル。
系9の原点は 北緯36度・東経139度50分。

使い方:
    python Tools/plateau_mesh.py origin <展開先のbldg/lod1ディレクトリ>
    python Tools/plateau_mesh.py rect <メッシュコード>
"""
import math
import os
import sys
import glob

# 平面直角座標系 第9系の原点(測量法施行令)
ORIGIN_LAT = 36.0
ORIGIN_LON = 139.0 + 50.0 / 60.0


def mesh3_to_latlon_rect(code):
    """8桁の3次メッシュコード -> (lat_min, lat_max, lon_min, lon_max)"""
    if len(code) != 8 or not code.isdigit():
        raise ValueError('3次メッシュコードは8桁の数字: %r' % code)
    p, q = int(code[0:2]), int(code[2:4])
    r, s = int(code[4]), int(code[5])
    t, u = int(code[6]), int(code[7])
    lat = (p + r / 8.0 + t / 80.0) * 2.0 / 3.0
    lon = (q + 100.0) + s / 8.0 + u / 80.0
    return lat, lat + (2.0 / 3.0) / 80.0, lon, lon + 1.0 / 80.0


def latlon_to_plane9(lat, lon):
    """簡易な平面直角座標(第9系)。戻り値は (北[m], 東[m])。

    厳密なガウス・クリューゲル投影ではなく、原点周りの局所的な線形近似。
    ここでの用途は「タイル群の中心を数百m精度で決める」「取り込み後の位置と桁・符号を
    突き合わせる」ことなので、この精度で足りる。
    厳密な値が要るなら pyproj 等で EPSG:6677 へ変換すること。
    """
    north = (lat - ORIGIN_LAT) * 111132.0
    east = (lon - ORIGIN_LON) * 111320.0 * math.cos(math.radians(lat))
    return north, east


def scan_tiles(directory):
    """bldg/lod1 のFBXを走査して、メッシュコードと平面直角座標の範囲を返す"""
    tiles = []
    for path in sorted(glob.glob(os.path.join(directory, '*.fbx'))):
        name = os.path.basename(path)
        code = name.split('_')[0]
        if len(code) != 8 or not code.isdigit():
            print('  [SKIP] メッシュコードとして解釈できない: %s' % name, file=sys.stderr)
            continue
        a, b, c, d = mesh3_to_latlon_rect(code)
        n0, e0 = latlon_to_plane9(a, c)
        n1, e1 = latlon_to_plane9(b, d)
        tiles.append({
            'code': code, 'path': path,
            'north': (min(n0, n1), max(n0, n1)),
            'east': (min(e0, e1), max(e0, e1)),
        })
    return tiles


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1

    if sys.argv[1] == 'rect':
        code = sys.argv[2]
        a, b, c, d = mesh3_to_latlon_rect(code)
        n0, e0 = latlon_to_plane9(a, c)
        n1, e1 = latlon_to_plane9(b, d)
        print('メッシュ %s' % code)
        print('  緯度 %.5f 〜 %.5f / 経度 %.5f 〜 %.5f' % (a, b, c, d))
        print('  系9  北 %+.0f 〜 %+.0f m / 東 %+.0f 〜 %+.0f m' % (n0, n1, e0, e1))
        return 0

    if sys.argv[1] == 'origin':
        directory = sys.argv[2]
        tiles = scan_tiles(directory)
        if not tiles:
            print('[ERROR] FBXが1つも見つからない: %s' % directory, file=sys.stderr)
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

    print(__doc__)
    return 1


if __name__ == '__main__':
    sys.exit(main())
