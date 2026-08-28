# -*- coding: utf-8 -*-
"""Project PLATEAU の .kmodel 群から .kscene を生成する。

671タイルぶんの [Model] を手で書くのは現実的でないため機械生成する。
先頭コメントには、このリポジトリの慣習どおり変換コマンドと判断の根拠を書く。

使い方:
    python Tools/plateau_scene.py <Assets/Packed/Plateau のパス> <出力する .kscene のパス>
"""
import glob
import os
import struct
import sys


def read_kmodel_bounds(path):
    """.kmodel の PackageHeader から (version, boundsMin, boundsMax) を返す"""
    with open(path, 'rb') as f:
        buf = f.read(64)
    if len(buf) < 64 or buf[:4] != b'KMDL':
        return None
    (_, version, _, _,
     x0, y0, z0, x1, y1, z1,
     _, _, _, _, _, _) = struct.unpack('<4sIII3f3fIIIIII', buf)
    return version, (x0, y0, z0), (x1, y1, z1)


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 1

    packed_dir, out_path = sys.argv[1], sys.argv[2]
    models = sorted(glob.glob(os.path.join(packed_dir, '*.kmodel')))
    if not models:
        print('[ERROR] .kmodel が見つからない: %s' % packed_dir, file=sys.stderr)
        return 1

    # 全タイルの AABB を合成する。カメラの初期位置と、シーンの規模から決まる
    # farZ・カスケード分割の見積もりに使う
    lo = [float('inf')] * 3
    hi = [float('-inf')] * 3
    bad_version = []
    for m in models:
        r = read_kmodel_bounds(m)
        if r is None:
            print('[ERROR] .kmodel として読めない: %s' % m, file=sys.stderr)
            return 1
        version, bmin, bmax = r
        if version != 9:
            bad_version.append((os.path.basename(m), version))
        for i in range(3):
            lo[i] = min(lo[i], bmin[i])
            hi[i] = max(hi[i], bmax[i])

    if bad_version:
        print('[ERROR] v9 でない .kmodel がある(再パックが要る):', file=sys.stderr)
        for name, v in bad_version[:5]:
            print('    %s (v%d)' % (name, v), file=sys.stderr)
        return 1

    size = [hi[i] - lo[i] for i in range(3)]
    diagonal = (size[0] ** 2 + size[1] ** 2 + size[2] ** 2) ** 0.5
    far_z = max(100.0, diagonal * 4.0)

    # カメラ。街を俯瞰しつつ、建物の立体感が分かる高さに置く。
    # 西新宿(53394525)の位置は --origin 適用後で X -5123〜-3939 / Z 946〜1996。
    # そこを south-west 側から見上げる形にする
    cam = (-6500.0, 900.0, -800.0)
    yaw, pitch = 35.0, -22.0

    lines = []
    w = lines.append
    w('# KurenaiEngine シーンファイル - Project PLATEAU 東京都23区(建築物 LOD1)')
    w('#')
    w('# 東京23区の全域(%d タイル、約 %.1f km x %.1f km)の建築物を、3次メッシュ(約1km四方)ごとの'
      % (len(models), size[0] / 1000.0, size[2] / 1000.0))
    w('# .kmodel として並べたもの。LOD1 なので建物は「航空レーザ点群の一律高さを与えた箱」で、')
    w('# テクスチャは持たない。')
    w('#')
    w('# 出典: 国土交通省 Project PLATEAU「3D都市モデル(Project PLATEAU)東京都23区」')
    w('#       https://www.geospatial.jp/ckan/dataset/plateau-tokyo23ku')
    w('#       ライセンス: 公共データ利用規約(PDL1.0) / CC BY 4.0 互換。商用利用可・要出典表示。')
    w('#       著作権は各地方公共団体に帰属する。')
    w('#')
    w('# 【このファイルは機械生成されたもの】 Tools\\plateau_scene.py が出力する。')
    w('# 取得から配布までは Tools\\import_plateau.ps1 が一括で行う:')
    w('#   Tools\\import_plateau.ps1')
    w('#   Sample3D.exe -scene PlateauTokyo23ku')
    w('#')
    w('# --- 変換コマンド ---')
    w('#   KurenaiPacker.exe <展開先>\\bldg\\lod1\\<メッシュコード>_bldg_6677.fbx ^')
    w('#     -o Assets\\Packed\\Plateau\\<メッシュコード>.kmodel --origin -8096,0,-36118')
    w('#')
    w('# 【--origin が要る理由】PLATEAU の FBX は EPSG:6677(平面直角座標系 第9系)の絶対座標で、')
    w('# 系原点から北へ最大52km離れている。頂点は float32 で、.kmodel の AABB もそのまま巨大になり、')
    w('# シーンAABBの対角から自動決定される遠クリップ面(farZ = max(100, 対角x4))が桁で狂う。')
    w('#')
    w('# 【全タイルで同じ値を引くこと】タイルごとに「自分のAABBの中心」で寄せると、')
    w('# タイル同士の相対位置が壊れて街が崩れる。値は Tools\\plateau_mesh.py origin が')
    w('# 全タイルのメッシュコードから算出する(671タイルの中心 = 東 -8096m / 北 -36118m)。')
    w('#')
    w('# 【軸の対応は実測で確定させた】FBX は Z-up で「X=東 / Y=北 / Z=標高」だが、assimp が')
    w('# Z-up→Y-up へ変換し aiProcess_ConvertToLeftHanded も入るため、パッカーが扱う時点では')
    w('# 「X=東 / Y=標高 / Z=北」になる。西新宿タイル(53394525)を --inspect した実測値')
    w('#   X -13218.99〜-12034.59 / Y 27.34〜267.02 / Z -35171.98〜-34122.55')
    w('# が、メッシュコードから計算した期待範囲(東 -13186〜-12055 / 北 -35192〜-34266)と')
    w('# 符号ごと一致することを確認している。街が鏡像になっていたら一見して気づけないため、')
    w('# 画像ではなく座標の数値で確定させた。Y は T.P.(東京湾平均海面)基準の絶対標高で、')
    w('# 都庁の243mを含む。')
    w('#')
    w('# 【dem(地形)と tran(道路)を入れていない理由】これらは6桁=2次メッシュ(約10km四方)で、')
    w('# bldg の3次メッシュとは分割単位が違う。1枚入れるだけでシーンAABBの対角が14km級になり、')
    w('# farZ が56kmまで伸びてカスケードシャドウが破綻する。')
    w('# 【LOD2 を入れていない理由】LOD2整備済み80タイルのメッシュコードは全て LOD1 側にも')
    w('# 存在するため、同時に読むと同じ建物が二重になり Z ファイティングを起こす。')
    w('#')
    w('# --- シーンの規模と、そこから決まるもの ---')
    w('#   AABB  X %.0f 〜 %.0f / Y %.0f 〜 %.0f / Z %.0f 〜 %.0f'
      % (lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]))
    w('#   大きさ %.0f x %.0f x %.0f m / 対角 %.0f m' % (size[0], size[1], size[2], diagonal))
    w('#   遠クリップ面 farZ = max(100, 対角x4) = %.0f m' % far_z)
    w('#')
    w('# 【ShadowDistance を指定している理由】カスケードシャドウの分割範囲は既定で')
    w('# 近クリップ面〜farZ そのもので、シャドウ専用の打ち切りが無い。farZ が %.0fm のままだと' % far_z)
    w('# 第1カスケードが数kmを2048x2048の1枚で覆うことになり、近景の影が事実上消える。')
    w('# 500m で打ち切ると第1カスケードは約31mを覆う(遠景の描画距離は farZ のまま変わらない)。')
    w('')
    w('[Scene]')
    w('Name = PLATEAU 東京23区 (建築物 LOD1)')
    w('ShadowDistance = 500')
    w('')

    for m in models:
        code = os.path.splitext(os.path.basename(m))[0]
        w('[Model]')
        w('Path = Plateau/%s.kmodel' % code)
        w('')

    w('# 街区の広がりが分かる俯瞰。西新宿(--origin適用後で X -5123〜-3939 / Z 946〜1996)を')
    w('# 北東方向に見込む位置に置いている。')
    w('# 【自動配置に任せられない】カメラを書かないとバウンズの20%内側へ置かれ、')
    w('# 33km四方のシーンでは建物の中に埋まる(BistroExterior.kscene と同じ理由)。')
    w('[Camera]')
    w('Position = %.1f, %.1f, %.1f' % cam)
    w('Yaw = %.1f' % yaw)
    w('Pitch = %.1f' % pitch)
    w('')
    w('[Sun]')
    w('TimeOfDay = 10.0')
    w('AzimuthDegrees = 150.0')
    w('Shadow = true')

    text = '\r\n'.join(lines) + '\r\n'
    with open(out_path, 'w', encoding='utf-8', newline='') as f:
        f.write(text)

    print('生成: %s' % out_path)
    print('  [Model] %d 件 / 対角 %.0f m / farZ %.0f m' % (len(models), diagonal, far_z))
    return 0


if __name__ == '__main__':
    sys.exit(main())
