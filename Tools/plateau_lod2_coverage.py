# -*- coding: utf-8 -*-
"""建築物 LOD2 が、同じメッシュコードの LOD1 をどれだけ覆っているかを測る。

【なぜ要るのか】PLATEAU の LOD2 は「そのタイル全体」が整備されているとは限らず、
半分近くは一部の建物だけが LOD2 化されている。これを 2段LOD の Path(=LOD0) に据えると
**近づくほど建物が消える**。plateau_scene.py はこの数値で採否を決める。

【AABBの被覆率では測れない】建物が2棟だけでもタイルの対角に位置していれば AABB は
満杯になる。実測で、AABB被覆1.000のタイルに「LOD1が約201棟 / LOD2が2メッシュ」が
混ざっていた。そのため頂点を平面へ落とした占有格子で比べる。

使い方:
    python Tools/plateau_lod2_coverage.py <Assets/Packed/Plateau のパス> [格子の一辺(m)]
"""
import glob
import os
import struct
import sys

CELL_SIZE_DEFAULT = 50.0


def read_package(path):
    """.kmodel から (bounds, [(vertexOffset, vertexCount), ...]) を返す"""
    with open(path, 'rb') as f:
        data = f.read()
    if data[:4] != b'KMDL':
        raise ValueError('KMDL ではない: %s' % path)
    (_, version, vstride, _istride,
     x0, y0, z0, x1, y1, z1,
     mesh_count, material_count, texture_count, _light_count,
     _gpo, _gpl, _sps, _rsv) = struct.unpack('<4sIII3f3f8I', data[:72])

    # MeshEntry は [TextureEntry x N][MaterialEntry x M] の後ろ
    offset = 72 + 16 * texture_count + 80 * material_count
    meshes = []
    for i in range(mesh_count):
        base = offset + 128 * i
        vertex_offset, _index_offset, vertex_count = struct.unpack('<QQI', data[base:base + 20])
        meshes.append((vertex_offset, vertex_count))
    return version, vstride, (x0, y0, z0), (x1, y1, z1), meshes


def occupied_cells(kmodel_path, cell_size):
    """頂点をXZ平面の格子へ落とし、占有セルの集合を返す"""
    _version, vstride, _bmin, _bmax, meshes = read_package(kmodel_path)
    geom_path = os.path.splitext(kmodel_path)[0] + '.kgeom'
    with open(geom_path, 'rb') as f:
        header = f.read(32)
        if header[:4] != b'KGEO':
            raise ValueError('KGEO ではない: %s' % geom_path)
        payload = f.read()

    cells = set()
    for vertex_offset, vertex_count in meshes:
        for v in range(vertex_count):
            base = vertex_offset + v * vstride
            x, _y, z = struct.unpack_from('<3f', payload, base)
            cells.add((int(x // cell_size), int(z // cell_size)))
    return cells


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    packed_dir = sys.argv[1]
    cell_size = float(sys.argv[2]) if len(sys.argv) > 2 else CELL_SIZE_DEFAULT

    rows = []
    for path in sorted(glob.glob(os.path.join(packed_dir, 'BldgLod2', '*.kmodel'))):
        code = os.path.splitext(os.path.basename(path))[0]
        lod1_path = os.path.join(packed_dir, '%s.kmodel' % code)
        if not os.path.exists(lod1_path):
            continue
        cells2 = occupied_cells(path, cell_size)
        cells1 = occupied_cells(lod1_path, cell_size)
        if not cells1:
            continue
        covered = len(cells1 & cells2) / float(len(cells1))
        rows.append((code, covered, len(cells1), len(cells2)))

    rows.sort(key=lambda r: r[1])
    print('# 格子 %.0fm / 比較したタイル %d 件' % (cell_size, len(rows)))
    print('# code    覆えた割合  LOD1のセル数  LOD2のセル数')
    for code, covered, n1, n2 in rows:
        print('%s %.3f %5d %5d' % (code, covered, n1, n2))
    return 0


if __name__ == '__main__':
    sys.exit(main())
