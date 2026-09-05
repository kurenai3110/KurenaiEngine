"""Project PLATEAU の dem(地形)だけを並べた .kscene を生成する。

メッシュレットLOD(Stage 6)の効きを測るための検証専用シーン。

【なぜ dem だけを切り出すか】メッシュレットLODが効くのは「三角形そのものが支配的コスト」の
モデルに限られる。PLATEAU の中でそれに当たるのは dem だけで、bldg LOD2 は1タイル
115,156三角形に対してテクスチャが2,165MBあり、支配するのはドローコールとテクスチャの側になる。
23区の全部入りシーンで測ると両者が混ざり、三角形が減っても総フレーム時間が動かないときに
「LODが効いていない」のか「効いた上でボトルネックが別にある」のかを切り分けられない。

【なぜ plateau_scene.py に相乗りしないか】あちらは bldg LOD1 の671タイル専用で、
先頭コメントも --origin の説明も bldg のものが埋め込まれている。加えて、あちらは
モデルLOD・ストリーミング(Stage 2)の側で作り直しが入る。両方が同じ関数を書き換えると
統合のたびに衝突するため、検証専用のこちらは独立させてある。

使い方:
    python Tools/plateau_dem_scene.py Assets/Packed/Plateau/Dem Scenes/PlateauDem.kscene
"""

import glob
import math
import os
import struct
import sys

CRLF = chr(13) + chr(10)
KMODEL_MAGIC = b'KMDL'
KMODEL_VERSION = 10

# カメラを置く距離を、シーンの外接球の半径の何倍にするか。
#
# 【1倍では球の内側】地形タイルは1枚で10km四方あり、外接球の半径は7km級になる。
# 等倍だとカメラが球の中に入り、投影サイズが「画面いっぱい」に振り切れて
# 段が一度も落ちない。2.1倍は、既定のFOV(45度)と720pで最初の段のしきい値を
# ちょうど越える位置(1タイルの実測で約15.3km)にあたる
CAMERA_DISTANCE_IN_RADII = 2.1
# 見下ろす角度。真横だと地形の起伏が読めず、真上だと段の切り替わりが分かりにくい
CAMERA_PITCH_DEGREES = -15.0
# 方位。南西から北東を見込む(plateau_scene.py の俯瞰と向きを揃えてある)
CAMERA_YAW_DEGREES = 45.0


def read_kmodel_bounds(path):
    """.kmodel のヘッダ64バイトからバージョンとAABBを読む。"""
    with open(path, 'rb') as f:
        header = f.read(64)
    if len(header) < 64 or header[:4] != KMODEL_MAGIC:
        return None
    version = struct.unpack_from('<I', header, 4)[0]
    bmin = struct.unpack_from('<3f', header, 16)
    bmax = struct.unpack_from('<3f', header, 28)
    return version, list(bmin), list(bmax)


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 1

    packed_dir, out_path = sys.argv[1], sys.argv[2]
    models = sorted(glob.glob(os.path.join(packed_dir, '*.kmodel')))
    if not models:
        print('[ERROR] .kmodel が見つからない: %s' % packed_dir, file=sys.stderr)
        return 1

    lo = [float('inf')] * 3
    hi = [float('-inf')] * 3
    bad_version = []
    for m in models:
        r = read_kmodel_bounds(m)
        if r is None:
            print('[ERROR] .kmodel として読めない: %s' % m, file=sys.stderr)
            return 1
        version, bmin, bmax = r
        if version != KMODEL_VERSION:
            bad_version.append((os.path.basename(m), version))
        for i in range(3):
            lo[i] = min(lo[i], bmin[i])
            hi[i] = max(hi[i], bmax[i])

    if bad_version:
        print('[ERROR] v%d でない .kmodel がある(再パックが要る):' % KMODEL_VERSION, file=sys.stderr)
        for name, v in bad_version[:5]:
            print('    %s (v%d)' % (name, v), file=sys.stderr)
        return 1

    size = [hi[i] - lo[i] for i in range(3)]
    diagonal = math.sqrt(sum(s * s for s in size))
    far_z = max(100.0, diagonal * 4.0)
    center = [(lo[i] + hi[i]) * 0.5 for i in range(3)]
    radius = diagonal * 0.5

    # カメラは中心から CAMERA_DISTANCE_IN_RADII 倍の距離に置き、
    # 指定した方位と俯角でそこを見込む
    distance = radius * CAMERA_DISTANCE_IN_RADII
    yaw = math.radians(CAMERA_YAW_DEGREES)
    pitch = math.radians(CAMERA_PITCH_DEGREES)
    forward = (
        math.cos(pitch) * math.sin(yaw),
        math.sin(pitch),
        math.cos(pitch) * math.cos(yaw),
    )
    cam = tuple(center[i] - forward[i] * distance for i in range(3))

    lines = []
    w = lines.append
    w('# KurenaiEngine シーンファイル - Project PLATEAU 東京都23区(地形 dem)')
    w('#')
    w('# 【メッシュレットLOD(Stage 6)の検証専用シーン】')
    w('# 三角形そのものが支配的コストになるモデルだけを並べてある。dem は1タイルが')
    w('# 2次メッシュ(約10km四方)の1メッシュで、三角形は100万の桁にのぼる一方、')
    w('# テクスチャを1枚も持たない。bldg を混ぜるとドローコールとテクスチャのコストが')
    w('# 上乗せされ、三角形を減らした効果が総フレーム時間から読めなくなる。')
    w('#')
    w('# 出典: 国土交通省 Project PLATEAU「3D都市モデル(Project PLATEAU)東京都23区」')
    w('#       https://www.geospatial.jp/ckan/dataset/plateau-tokyo23ku')
    w('#       ライセンス: 公共データ利用規約(PDL1.0) / CC BY 4.0 互換。商用利用可・要出典表示。')
    w('#       著作権は各地方公共団体に帰属する。')
    w('#')
    w(r'# 【このファイルは機械生成されたもの】 Tools\plateau_dem_scene.py が出力する:')
    w(r'#   python Tools\plateau_dem_scene.py Assets\Packed\Plateau\Dem Scenes\PlateauDem.kscene')
    w('#   Sample3D.exe -dx12 -scene PlateauDem')
    w('#')
    w('# --- 変換コマンド ---')
    w(r'#   KurenaiPacker.exe <展開先>\dem\<メッシュコード>_dem_6677.fbx ^')
    w(r'#     -o Assets\Packed\Plateau\Dem\<メッシュコード>.kmodel --origin -8096,0,-36118')
    w('#')
    w('# 【--origin は bldg と同じ値を引くこと】種別ごとに違う値で寄せると、')
    w('# 地形と建物の相対位置が壊れて建物が地面から浮く/沈む。')
    w('#')
    w('# --- シーンの規模と、そこから決まるもの ---')
    w('#   [Model] %d 件' % len(models))
    w('#   AABB  X %.0f 〜 %.0f / Y %.0f 〜 %.0f / Z %.0f 〜 %.0f'
      % (lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]))
    w('#   大きさ %.0f x %.0f x %.0f m / 対角 %.0f m' % (size[0], size[1], size[2], diagonal))
    w('#   遠クリップ面 farZ = max(100, 対角x4) = %.0f m' % far_z)
    w('#   外接球の半径 %.0f m / カメラ距離 %.0f m (半径の%.1f倍)'
      % (radius, distance, CAMERA_DISTANCE_IN_RADII))
    w('#')
    w('# 【カメラを外接球の外へ置いている理由】メッシュレットLODの段はモデルの外接球の')
    w('# 投影サイズで決まる。カメラが球の内側にあると投影サイズが振り切れて常に段0になり、')
    w('# 段の選択が一度も実行されないまま「効かなかった」と読み違える。')
    w('#')
    w('# 【ShadowDistance】farZ が %.0fm あるため、指定しないと第1カスケードが数kmを' % far_z)
    w('# 1枚で覆い、地形の起伏の影が消える。')
    w('')
    w('[Scene]')
    w('Name = PLATEAU 地形 (dem / メッシュレットLOD検証)')
    w('ShadowDistance = 2000')
    w('')

    for m in models:
        code = os.path.splitext(os.path.basename(m))[0]
        w('[Model]')
        w('Path = Plateau/Dem/%s.kmodel' % code)
        w('')

    w('# 【自動配置に任せられない】カメラを書かないとバウンズの20%内側へ置かれ、')
    w('# 外接球の内側に入って段が一度も落ちなくなる(上記の理由)。')
    w('[Camera]')
    w('Position = %.1f, %.1f, %.1f' % cam)
    w('Yaw = %.1f' % CAMERA_YAW_DEGREES)
    w('Pitch = %.1f' % CAMERA_PITCH_DEGREES)
    w('')
    w('# 起伏を読むには斜めから当てる。真上からだと地形が平坦に見える')
    w('[Sun]')
    w('TimeOfDay = 9.0')
    w('AzimuthDegrees = 150.0')
    w('Shadow = true')

    text = CRLF.join(lines) + CRLF
    with open(out_path, 'w', encoding='utf-8', newline='') as f:
        f.write(text)

    print('生成: %s' % out_path)
    print('  [Model] %d 件 / 対角 %.0f m / farZ %.0f m' % (len(models), diagonal, far_z))
    print('  外接球 半径 %.0f m / カメラ距離 %.0f m' % (radius, distance))
    return 0


if __name__ == '__main__':
    sys.exit(main())
