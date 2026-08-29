# -*- coding: utf-8 -*-
"""Project PLATEAU の .kmodel 群から 23区全域の .kscene を生成する。

800個超の [Model] を手で書くのは現実的でないため機械生成する。
先頭コメントには、このリポジトリの慣習どおり変換コマンドと判断の根拠を書く。

使い方:
    python Tools/plateau_scene.py <Assets/Packed/Plateau のパス> <出力する .kscene のパス>
"""
import glob
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from plateau_lod2_coverage import occupied_cells  # noqa: E402

# .kmodel のバージョン。不一致は読み込みを拒否されるので、生成の時点で弾く
EXPECTED_VERSION = 10

# --- シーンの既定値。根拠は下の生成コメントと docs/ImplementationDetail.md に書く ---

# 【LODDistance】タイルは約1.1km四方。この距離を超えたインスタンスは LOD1(箱)へ落ちる。
# 距離は AABB の最近接点までなので、自分がいるタイルは0m、隣接タイルは約1.1km。
# 1500 は「自分のタイルと隣接1周ぶんを LOD2 のまま保つ」位置にある。
LOD_DISTANCE = 1500.0

# 【StreamingDistance】必ず指定する。未指定(HasStreamingDistance=false)だとストリーミングが
# 丸ごと無効になり、SceneLoader が全 LOD 段を起動時に確保してしまう
# ―― LOD2 だけで約9GBになる。
# 指定するとエンジンは各インスタンスの「現在の LOD 段」だけを読むので、
# LOD2 は LODDistance の内側にあるタイルにしか載らない。
#
# 【値はシーン対角にする】距離で切ると遠景がそこで途切れる。33km四方のシーンを
# 高度900mから俯瞰すると20km先まで見えるので、たとえば12000だと街が12kmで
# ぷつりと終わる。対角(約45km)にしておけば距離による切り捨ては起きず、
# 「現在のLOD段だけ読む」という本来効かせたい性質だけが残る。
#
# 【代償: LOD2 は一度載ると降りない】破棄は StreamingDistance の1.25倍より遠い
# インスタンスにしか起きないため、この値だと破棄が一度も走らない。
# LOD2 の整備地区を全部回ると最大で約9GBまで積み上がる。
# 常駐量を抑えるのはミップ常駐制御(テクスチャストリーミング)の役目で、
# ここではジオメトリの読み込み単位だけを決めている。
STREAMING_DISTANCE_MIN = 20000.0

# 【CameraSpeed】対角45kmから自動決定させると 600m/s 級になり、LOD の切り替わりを
# 目で追えない。33km四方を数分で横断でき、街区単位の移動も効く値にする。
CAMERA_SPEED = 150.0

# 【ShadowDistance】farZ は対角×4で180km級になる。打ち切らないと第1カスケードが
# 数kmを2048x2048の1枚で覆い、近景の影が事実上消える。
SHADOW_DISTANCE = 500.0

# 【LOD2 を採用する条件】PLATEAU の LOD2 は「そのメッシュコードのタイル全体」が
# 整備されているとは限らない。たとえば 53394515 は LOD1 が384セルを占めるのに対し
# LOD2 は3セルしか持たない。これをそのまま Path(=LOD0) に据えると、
# **近づくほど建物が消える**という逆の絵になる。
#
# 判定は「LOD1 の頂点が占める %.0fm 格子のセルを、LOD2 がどれだけ覆うか」で行う
# (Tools/plateau_lod2_coverage.py)。
#
# 【AABBの被覆率では測れない】建物が2棟だけでもタイルの対角に位置すれば AABB は満杯になる。
# 実測で、AABB被覆1.000のタイルに占有被覆0.01のものが混ざっていた。
# 【ジオメトリ量の比でも測れない】LOD2 は1棟あたりの頂点が桁で多いため、
# 一部の建物しか無くても LOD1 の総量を超える(80件中51件が通ってしまう)。
#
# 0.95 は実測の分布から採った。格子を25m/50m/100mへ振っても採用数は26〜27で動かず、
# 「完全整備」の塊(0.969〜1.000)をちょうど捉える。
# 0.8 まで下げると32タイルになり、丸の内(0.869)・池袋(0.833)など6地区が加わるが、
# その分だけ近づいたときに建物が抜ける。
LOD2_MIN_COVERAGE = 0.95
LOD2_COVERAGE_CELL_SIZE = 50.0


def read_kmodel_header(path):
    """.kmodel の PackageHeader から (version, boundsMin, boundsMax) を返す"""
    with open(path, 'rb') as f:
        buf = f.read(64)
    if len(buf) < 64 or buf[:4] != b'KMDL':
        return None
    (_, version, _, _,
     x0, y0, z0, x1, y1, z1,
     _, _, _, _, _, _) = struct.unpack('<4sIII3f3fIIIIII', buf)
    return version, (x0, y0, z0), (x1, y1, z1)


def collect(directory, pattern='*.kmodel'):
    """ディレクトリ直下の .kmodel を メッシュコード -> パス で返す"""
    found = {}
    for path in sorted(glob.glob(os.path.join(directory, pattern))):
        code = os.path.splitext(os.path.basename(path))[0]
        found[code] = path
    return found


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 1

    packed_dir, out_path = sys.argv[1], sys.argv[2]

    # LOD1 だけ Plateau 直下、他は種別ごとのサブディレクトリ。
    # (LOD1 を先に置いた経緯によるもので、移すと既存 .kscene の参照が全部変わる)
    lod1 = collect(packed_dir)
    lod2 = collect(os.path.join(packed_dir, 'BldgLod2'))
    dem = collect(os.path.join(packed_dir, 'Dem'))
    tran = collect(os.path.join(packed_dir, 'Tran'))
    brid = collect(os.path.join(packed_dir, 'Brid'))

    if not lod1:
        print('[ERROR] 建築物LOD1の .kmodel が見つからない: %s' % packed_dir, file=sys.stderr)
        return 1

    # 全部のAABBを合成する。カメラの初期位置と、シーンの規模から決まる farZ の見積りに使う
    lo = [float('inf')] * 3
    hi = [float('-inf')] * 3
    bad_version = []
    for group in (lod1, lod2, dem, tran, brid):
        for code, path in group.items():
            header = read_kmodel_header(path)
            if header is None:
                print('[ERROR] .kmodel として読めない: %s' % path, file=sys.stderr)
                return 1
            version, bmin, bmax = header
            if version != EXPECTED_VERSION:
                bad_version.append((os.path.basename(path), version))
            for i in range(3):
                lo[i] = min(lo[i], bmin[i])
                hi[i] = max(hi[i], bmax[i])

    if bad_version:
        print('[ERROR] v%d でない .kmodel がある(再パックが要る):' % EXPECTED_VERSION, file=sys.stderr)
        for name, v in bad_version[:5]:
            print('    %s (v%d)' % (name, v), file=sys.stderr)
        return 1

    size = [hi[i] - lo[i] for i in range(3)]
    diagonal = (size[0] ** 2 + size[1] ** 2 + size[2] ** 2) ** 0.5
    far_z = max(100.0, diagonal * 4.0)
    # 距離による切り捨てで遠景が途切れないよう、シーン対角まで伸ばす(定数のコメント参照)
    streaming_distance = max(STREAMING_DISTANCE_MIN, diagonal)

    # LOD2 が整備されているタイルは、LOD1 と同じメッシュコードで両方存在する。
    # 独立した [Model] として並べると同じ建物が二重になりZファイティングを起こすので、
    # LOD2 を Path、LOD1 を LODPath に置いた2段LODとして1つの [Model] にまとめる。
    #
    # ただし採用するのは「タイル全体が LOD2 化されているもの」だけ(上の定数のコメント参照)。
    # 部分整備のタイルは LOD2 を使わず LOD1 のままにする。
    lod2_codes = []
    lod2_rejected = []
    for code in sorted(set(lod2) & set(lod1)):
        cells1 = occupied_cells(lod1[code], LOD2_COVERAGE_CELL_SIZE)
        cells2 = occupied_cells(lod2[code], LOD2_COVERAGE_CELL_SIZE)
        coverage = (len(cells1 & cells2) / float(len(cells1))) if cells1 else 0.0
        if coverage >= LOD2_MIN_COVERAGE:
            lod2_codes.append(code)
        else:
            lod2_rejected.append((code, coverage))

    lod2_use = set(lod2_codes)
    lod2_orphan = sorted(set(lod2) - set(lod1))
    lod1_only = sorted(set(lod1) - lod2_use)

    total_models = len(lod1) + len(lod2_orphan) + len(dem) + len(tran) + len(brid)

    lines = []
    w = lines.append
    w('# KurenaiEngine シーンファイル - Project PLATEAU 東京都23区(全種)')
    w('#')
    w('# 東京23区の全域(約 %.1f km x %.1f km)を、建築物・地形・道路・橋梁の4種で構成したもの。'
      % (size[0] / 1000.0, size[2] / 1000.0))
    w('#')
    w('#   建築物 %4d タイル(3次メッシュ=約1km四方)。うち %d タイルは LOD2 が整備されており、'
      % (len(lod1), len(lod2_codes)))
    w('#          Path=LOD2 / LODPath=LOD1 の2段LODにしてある(下記)')
    w('#   地形   %4d タイル(2次メッシュ=約10km四方)' % len(dem))
    w('#   道路   %4d タイル(同上)' % len(tran))
    w('#   橋梁   %4d タイル(3次メッシュ)' % len(brid))
    w('#   [Model] 合計 %d 件' % total_models)
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
    w('#   KurenaiPacker.exe <展開先>\\<種別>\\<メッシュコード>_*.fbx ^')
    w('#     -o Assets\\Packed\\Plateau\\<種別>\\<メッシュコード>.kmodel --origin -8096,0,-36118')
    w('#')
    w('# 【--origin は全種で同じ値】PLATEAU の FBX は EPSG:6677(平面直角座標系 第9系)の絶対座標で、')
    w('# 系原点から北へ最大52km離れている。頂点は float32 で、.kmodel の AABB もそのまま巨大になり、')
    w('# 遠クリップ面が桁で狂う。タイルごとに「自分のAABBの中心」で寄せると相対位置が壊れるため、')
    w('# 全種・全タイルで同じ値を引く。値は Tools\\plateau_mesh.py origin が算出する。')
    w('#')
    w('# 【brid のファイル名に _6697 を名乗る個体がある】EPSG:6697 は緯度経度系だが、中身は')
    w('# 他と同じ平面直角座標のメートル値だった。全68件のAABBをメッシュコードから計算した')
    w('# 期待矩形と突き合わせて確認済み(Tools\\plateau_mesh.py rect で再現できる)。')
    w('#')
    w('# --- シーンの規模と、そこから決まるもの ---')
    w('#   AABB  X %.0f 〜 %.0f / Y %.0f 〜 %.0f / Z %.0f 〜 %.0f'
      % (lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]))
    w('#   大きさ %.0f x %.0f x %.0f m / 対角 %.0f m' % (size[0], size[1], size[2], diagonal))
    w('#   遠クリップ面 farZ = max(100, 対角x4) = %.0f m' % far_z)
    w('')
    w('[Scene]')
    w('Name = PLATEAU 東京23区 (建築物LOD1/LOD2・地形・道路・橋梁)')
    w('')
    w('# 【カスケードの分割範囲を打ち切る】farZ が %.0fm のままだと第1カスケードが数kmを' % far_z)
    w('# 2048x2048の1枚で覆うことになり、近景の影が事実上消える。')
    w('# 500m で打ち切ると第1カスケードは約31mを覆う(遠景の描画距離は farZ のまま変わらない)。')
    w('ShadowDistance = %.0f' % SHADOW_DISTANCE)
    w('')
    w('# 【必ず指定する】未指定だとストリーミングが丸ごと無効になり、SceneLoader が')
    w('# 全 LOD 段を起動時に確保する ―― LOD2 の%dタイルだけで約9GBになる。' % len(lod2_codes))
    w('# 指定するとエンジンは各インスタンスの「現在の LOD 段」だけを読むので、')
    w('# LOD2 は LODDistance の内側にあるタイルにしか載らない。')
    w('#')
    w('# 【値はシーン対角】距離で切ると遠景がそこで途切れる。高度300mからでも数km先まで')
    w('# 見えるので、対角(%.0fm)まで伸ばして距離による切り捨てを起こさせない。' % diagonal)
    w('# 代償として破棄(この1.25倍より遠いインスタンスが対象)は一度も走らないため、')
    w('# LOD2 は一度載ると降りない。常駐量を抑えるのはミップ常駐制御の役目。')
    w('#')
    w('# 【副作用: メッシュ単位フラスタムカリングが無効になる】SceneLoaderは')
    w('# MeshWorldBoundsList を「読み込み済みの実体」からしか作れず、ストリーミング時は')
    w('# 実体が無いまま構築が終わる。あとから読み込まれたぶんも作り直されないため、')
    w('# 描画側は保守側(間引かない)へ倒れる。実測でメッシュ単位は判定5964/間引き0(0.0%)。')
    w('# モデル単位は85.9%効いており、DX11/Release/1280x720 で 56 FPS 出ているので')
    w('# 実害は出ていないが、「メッシュ単位が0%なのはカリングの不具合ではない」と')
    w('# 分かるようにここへ書いておく。')
    w('StreamingDistance = %.0f' % streaming_distance)
    w('')
    w('# 【対角から自動決定させると速すぎる】対角%.0fkmだと自動値は600m/s級になり、' % (diagonal / 1000.0))
    w('# LOD の切り替わりを目で追えない。')
    w('CameraSpeed = %.0f' % CAMERA_SPEED)
    w('')

    w('# --- 建築物 -------------------------------------------------------------')
    w('# LOD2 を採用した %d タイルは2段LOD。%.0fm を超えると LOD1(箱)へ落ちる。'
      % (len(lod2_codes), LOD_DISTANCE))
    w('# 距離は「カメラ位置からインスタンスのAABBの最近接点まで」で測られるので、')
    w('# 自分がいるタイルは0m、隣接タイルは約1.1kmになる。')
    w('#')
    w('# 【LOD2 があっても使っていないタイルが %d 件ある】PLATEAU の LOD2 はタイル全体が'
      % len(lod2_rejected))
    w('# 整備されているとは限らず、一部の建物だけのものが混ざる。それを Path(=LOD0) に据えると')
    w('# 近づくほど建物が消えるので、足切りしてある。')
    w('# 判定は「LOD1 の頂点が占める%.0fm格子のセルを LOD2 がどれだけ覆うか」で、しきい値 %.2f。'
      % (LOD2_COVERAGE_CELL_SIZE, LOD2_MIN_COVERAGE))
    w('# (Tools\\plateau_lod2_coverage.py で再現できる。AABBの被覆率やジオメトリ量の比では')
    w('#  測れない ―― 建物が2棟でも対角にあればAABBは満杯になり、LOD2は1棟あたりの頂点が')
    w('#  桁で多いのでジオメトリ量は一部整備でもLOD1を超える)')
    if lod2_rejected:
        w('# 除外したタイルと被覆率:')
        for code, coverage in lod2_rejected:
            w('#   %s 被覆 %.3f' % (code, coverage))
    w('')
    for code in sorted(lod1):
        w('[Model]')
        if code in lod2_use:
            w('Path = Plateau/BldgLod2/%s.kmodel' % code)
            w('LODPath = Plateau/%s.kmodel' % code)
            w('LODDistance = %.0f' % LOD_DISTANCE)
        else:
            w('Path = Plateau/%s.kmodel' % code)
        w('')

    # LOD1 に対応が無い LOD2(現状は0件のはず)。黙って落とさず、あれば単独で置く
    for code in lod2_orphan:
        w('[Model]')
        w('Path = Plateau/BldgLod2/%s.kmodel' % code)
        w('')

    for label, group, sub in (('地形', dem, 'Dem'), ('道路', tran, 'Tran'), ('橋梁', brid, 'Brid')):
        if not group:
            continue
        w('# --- %s (%d タイル) ---' % (label, len(group)))
        w('')
        for code in sorted(group):
            w('[Model]')
            w('Path = Plateau/%s/%s.kmodel' % (sub, code))
            w('')

    w('# 【LOD2 地区が見える位置に置く】以前は高度900mから30km先を見る俯瞰にしていたが、')
    w('# それだと大気遠近で全面が白く飛び、LOD2 のテクスチャも LODDistance の外なので')
    w('# 一度も出ない。採用タイルのうち最も建物が高い 53393599(264.8m / X -19 / Z -1305)を、')
    w('# LODDistance(%.0fm)の内側から北東に見込む位置へ移した。' % LOD_DISTANCE)
    w('# 手前に LOD2 のテクスチャ付きの街、その奥に LOD1 の箱が地平まで続く絵になる。')
    w('# 【自動配置に任せられない】カメラを書かないとバウンズの20%内側へ置かれ、')
    w('# 33km四方のシーンでは建物の中に埋まる(BistroExterior.kscene と同じ理由)。')
    w('[Camera]')
    w('Position = -1500.0, 300.0, -2400.0')
    w('Yaw = 53.5')
    w('Pitch = -6.0')
    w('')
    w('[Sun]')
    w('TimeOfDay = 10.0')
    w('AzimuthDegrees = 150.0')
    w('Shadow = true')

    text = '\r\n'.join(lines) + '\r\n'
    with open(out_path, 'w', encoding='utf-8', newline='') as f:
        f.write(text)

    print('生成: %s' % out_path)
    print('  [Model] %d 件 (建築物 %d / 地形 %d / 道路 %d / 橋梁 %d)'
          % (total_models, len(lod1) + len(lod2_orphan), len(dem), len(tran), len(brid)))
    print('  うち2段LOD %d 件 / LOD1のみ %d 件' % (len(lod2_codes), len(lod1_only)))
    print('  LOD2があっても部分整備のため使わなかったタイル %d 件' % len(lod2_rejected))
    print('  対角 %.0f m / farZ %.0f m' % (diagonal, far_z))
    return 0


if __name__ == '__main__':
    sys.exit(main())
