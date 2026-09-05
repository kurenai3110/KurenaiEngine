#!/usr/bin/env python3
"""段階2(三角形メッシュライト)を検証するための glTF と .kscene を生成する。

【何を確かめるためのものか】
段階1のプロキシは発光クラスタを重心1点へ潰す。段階2は同じクラスタを三角形の束のまま
面積分する。**遠方では両者は一致しなければならない** ―― これが段階1↔段階2の
突き合わせ検証になり、同時に「段階2が要る理由」と「段階1で足りる範囲」を
1つの数で表す。すなわち:

    両者が1%を超えて乖離し始める距離はどこか

【なぜ平らなパネル1枚なのか】
  - 三角形2枚で済むので、参照実装の全三角形総当たりが確実に回る
  - κ=1(平らな片面)なので段階1のローブ I(θ)=L*A*cosθ は**遠方場で厳密**。
    近似の誤差は「面を点へ潰した」ことだけに絞られ、κ の補間の誤差が混ざらない
  - 床を広く取れば、パネル直下(d/sqrt(A) が小さい)から遠方まで
    **1枚の絵の中に距離の掃引が入る**。距離ごとに撮り直す必要がない

【BRDFを Python へ移植して画素値と直接比べようとしないこと】
このエンジンの BRDF は metallic=0 でも F0=0.04 の鏡面を持ち、Kulla-Conty の
エネルギー補正で GPU 上のBRDF積分LUTを引く。移植は再現しきれず、ここで時間を溶かす。
**段階1と段階2はどちらも同じエンジンのBRDFを通る**ので、両者を比べれば
BRDFは厳密に相殺し、減衰と積分の違いだけが残る。そちらを物差しにする。

【なぜ遮蔽物を置かないのか】2a は G項と面積分の正しさだけを見る段。
半影は遮蔽ありの別シーン(MeshLightOccluded)の役目で、混ぜると
「G項が違う」のか「影レイの狙点が違う」のかを切り分けられなくなる。

使い方:
    python Tools/generate_meshlight_test.py           # glTF と .kscene を書く
    <KurenaiPacker で Assets/Packed/MeshLightTest へ変換>

    # 段階2(三角形を面積分)。**DX12 + DXR が要る**
    Sample3D.exe -dx12 -scene MeshLightQuad -megalights 1 -emissivelights 1 -meshlights 1
    # 段階1(プロキシを1点で評価)。同じシーン・同じ露出で比較対象になる
    Sample3D.exe -dx12 -scene MeshLightQuad -megalights 1 -emissivelights 1 -meshlights 0
"""

import json
import math
import os
import struct
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_DIR = os.path.join(SCRIPT_DIR, "..")
OUT_DIR = os.path.join(REPO_DIR, "Assets", "Source", "MeshLightTest")
SCENE_DIR = os.path.join(REPO_DIR, "Scenes")
NAME = "MeshLightTest"

# 発光パネルの1辺[m]。面積 A = PANEL_SIZE^2
PANEL_SIZE = 2.0
# パネルの高さ[m]。**直下では d/sqrt(A) = 1.5 で点近似が成り立たない。**
# そこが乖離の出る側で、床の外周へ向かうほど遠方場へ近づく
PANEL_HEIGHT = 3.0
# 床の半径[m]。パネルからの距離の掃引を1枚に入れるため広く取る。
# sqrt(A)=2 なので、外周 20m は d/sqrt(A) ≒ 10 にあたり「5倍則」の外側に十分入る
FLOOR_HALF_SIZE = 20.0
# 床の分割数。1枚の巨大なクアッドだと頂点が4つしかなく、
# **タイル単位の挙動を見るときに頂点密度が効く**ので適度に割る
FLOOR_SEGMENTS = 40

# 【自発光の係数】glTF の emissiveFactor は [0,1] に収まるので、
# 面積の小さい器具は物理的に暗すぎて8bitの1階調に届かない(段階1で実測済み)。
# パネルは 4 m^2 と大きいので 1.0 のままで足りる
EMISSIVE_FACTOR = 1.0
# 床の反射率。0.5 は段階1の検証と揃えてある
FLOOR_ALBEDO = 0.5

# .kscene に書き込む固定値
SCENE_EXPOSURE_EV100 = 15.0
# パネルを斜め上から見下ろし、直下から外周までを一度に入れる
CAMERA_POSITION = (0.0, 9.0, -16.0)
CAMERA_YAW = 0.0
CAMERA_PITCH = -30.0


def generate_quad(corners, normal):
    # 【巻き順に注意】素直に (0,1,2),(0,2,3) と張ると2枚目が裏向きになり、
    # 床が対角線で半分だけ描かれる(generate_emissive_light_test.py の同じ注記を参照)
    return (list(corners), [normal] * 4,
            [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)], [(0, 2, 1), (0, 3, 2)])


def generate_grid(half_size, segments, y, normal):
    """XZ平面のグリッド。床をある程度細かく割るため"""
    positions, normals, uvs, indices = [], [], [], []
    for iz in range(segments + 1):
        for ix in range(segments + 1):
            fx = ix / segments
            fz = iz / segments
            positions.append((-half_size + 2.0 * half_size * fx, y, -half_size + 2.0 * half_size * fz))
            normals.append(normal)
            uvs.append((fx, fz))
    for iz in range(segments):
        for ix in range(segments):
            a = iz * (segments + 1) + ix
            b = a + segments + 1
            # 上向きの面になる巻き順
            indices.append((a, b, a + 1))
            indices.append((a + 1, b, b + 1))
    return positions, normals, uvs, indices


def write_gltf():
    os.makedirs(OUT_DIR, exist_ok=True)
    buffer_bytes = bytearray()

    def append_aligned(data):
        while len(buffer_bytes) % 4 != 0:
            buffer_bytes.append(0)
        offset = len(buffer_bytes)
        buffer_bytes.extend(data)
        return offset

    accessors, buffer_views, meshes, materials, nodes = [], [], [], [], []

    def add_mesh(name, positions, normals, uvs, indices, material_index):
        pos_bytes = b"".join(struct.pack("<3f", *p) for p in positions)
        pos_offset = append_aligned(pos_bytes)
        buffer_views.append({"buffer": 0, "byteOffset": pos_offset, "byteLength": len(pos_bytes)})
        pos_view = len(buffer_views) - 1

        nrm_bytes = b"".join(struct.pack("<3f", *n) for n in normals)
        nrm_offset = append_aligned(nrm_bytes)
        buffer_views.append({"buffer": 0, "byteOffset": nrm_offset, "byteLength": len(nrm_bytes)})
        nrm_view = len(buffer_views) - 1

        uv_bytes = b"".join(struct.pack("<2f", *t) for t in uvs)
        uv_offset = append_aligned(uv_bytes)
        buffer_views.append({"buffer": 0, "byteOffset": uv_offset, "byteLength": len(uv_bytes)})
        uv_view = len(buffer_views) - 1

        idx_flat = [i for tri in indices for i in tri]
        idx_bytes = b"".join(struct.pack("<I", i) for i in idx_flat)
        idx_offset = append_aligned(idx_bytes)
        buffer_views.append({"buffer": 0, "byteOffset": idx_offset, "byteLength": len(idx_bytes)})
        idx_view = len(buffer_views) - 1

        xs = [p[0] for p in positions]
        ys = [p[1] for p in positions]
        zs = [p[2] for p in positions]
        accessors.append({"bufferView": pos_view, "componentType": 5126, "count": len(positions),
                          "type": "VEC3", "min": [min(xs), min(ys), min(zs)],
                          "max": [max(xs), max(ys), max(zs)]})
        pos_acc = len(accessors) - 1
        accessors.append({"bufferView": nrm_view, "componentType": 5126, "count": len(normals),
                          "type": "VEC3"})
        nrm_acc = len(accessors) - 1
        accessors.append({"bufferView": uv_view, "componentType": 5126, "count": len(uvs),
                          "type": "VEC2"})
        uv_acc = len(accessors) - 1
        accessors.append({"bufferView": idx_view, "componentType": 5125, "count": len(idx_flat),
                          "type": "SCALAR"})
        idx_acc = len(accessors) - 1

        meshes.append({"name": name, "primitives": [{
            "attributes": {"POSITION": pos_acc, "NORMAL": nrm_acc, "TEXCOORD_0": uv_acc},
            "indices": idx_acc, "material": material_index}]})
        nodes.append({"mesh": len(meshes) - 1, "name": name})

    # --- 材質 ---
    # 発光パネル。**片面発光**(doubleSided を立てない)。
    # 【EmissiveFactor != 0 が光源化の条件】材質名で選んではいけない ――
    # 名前だけがエミッシブで Ke=0 の材質が実アセットに実在する(段階1で確認済み)
    materials.append({
        "name": "EmissivePanel",
        "pbrMetallicRoughness": {"baseColorFactor": [0.0, 0.0, 0.0, 1.0],
                                 "metallicFactor": 0.0, "roughnessFactor": 1.0},
        "emissiveFactor": [EMISSIVE_FACTOR, EMISSIVE_FACTOR, EMISSIVE_FACTOR],
        "doubleSided": False,
    })
    # 床。拡散のみ。roughness=1 / metallic=0 で鏡面の寄与を最小にする
    # (**0にはならない** ―― F0=0.04 の鏡面が残る。だから段階1との比較で相殺させる)
    materials.append({
        "name": "Floor",
        "pbrMetallicRoughness": {"baseColorFactor": [FLOOR_ALBEDO, FLOOR_ALBEDO, FLOOR_ALBEDO, 1.0],
                                 "metallicFactor": 0.0, "roughnessFactor": 1.0},
        "doubleSided": False,
    })

    # --- 発光パネル: 下向き(法線 -Y)。床を照らす ---
    h = PANEL_SIZE * 0.5
    y = PANEL_HEIGHT
    # 下向きの面になる巻き順。generate_quad は corners の順に (0,2,1),(0,3,2) を張る
    panel_corners = [(-h, y, -h), (-h, y, h), (h, y, h), (h, y, -h)]
    p, n, t, i = generate_quad(panel_corners, (0.0, -1.0, 0.0))
    add_mesh("EmissivePanel", p, n, t, i, 0)

    # --- 床 ---
    p, n, t, i = generate_grid(FLOOR_HALF_SIZE, FLOOR_SEGMENTS, 0.0, (0.0, 1.0, 0.0))
    add_mesh("Floor", p, n, t, i, 1)

    while len(buffer_bytes) % 4 != 0:
        buffer_bytes.append(0)

    bin_name = NAME + ".bin"
    gltf = {
        "asset": {"version": "2.0", "generator": "generate_meshlight_test.py"},
        "scene": 0,
        "scenes": [{"nodes": list(range(len(nodes)))}],
        "nodes": nodes,
        "meshes": meshes,
        "materials": materials,
        "accessors": accessors,
        "bufferViews": buffer_views,
        "buffers": [{"uri": bin_name, "byteLength": len(buffer_bytes)}],
    }
    with open(os.path.join(OUT_DIR, NAME + ".gltf"), "w", encoding="utf-8") as fp:
        json.dump(gltf, fp, indent=2)
    with open(os.path.join(OUT_DIR, bin_name), "wb") as fp:
        fp.write(buffer_bytes)
    print("glTF を書き出しました: %s" % os.path.abspath(OUT_DIR))


SCENE_TEMPLATE = """\
# 段階2(三角形メッシュライト)の検証シーン。
# Tools/generate_meshlight_test.py で生成される(手で編集しない)。
#
# 発光パネル1枚(片面・下向き・{size}x{size} m、高さ {height} m)と広い床だけ。遮蔽物は無い。
# 床の外周へ向かうほどパネルからの距離が伸びるので、**1枚の絵に距離の掃引が入る**。
#
# 確認手順(**DX12 + DXR が要る**。メッシュライトは MegaLights 経路でのみ効く):
#   Sample3D.exe -dx12 -scene MeshLightQuad -megalights 1 -emissivelights 1 -meshlights 1
#   Sample3D.exe -dx12 -scene MeshLightQuad -megalights 1 -emissivelights 1 -meshlights 0
# 前者が段階2(三角形を面積分)、後者が段階1(プロキシを1点で評価)。
# **遠方では一致しなければならない。**1%を超えて乖離し始める距離が、
# 「段階2が要る理由」と「段階1で足りる範囲」を同時に表す。
#
# 比較は蓄積ダンプで取ること(画面キャプチャは8bitかつトーンマップ後で収束を測れない):
#   -megalightsaccum 256 -megalightsdump <出力先>
#   python Tools/megalights_metrics.py dump <段階1.bin> <段階2.bin>

[Scene]
Name = Mesh Light Quad
# 露出を固定する。三角形テーブルの放射輝度は露出前なので、
# 露出が動くと段階1との比較が壊れる
Exposure = {exposure}
# 比較にノイズを混ぜない
TAA = false
AmbientOcclusion = false
IBLIntensity = 0.0

[Model]
Path = MeshLightTest/MeshLightTest.kmodel

[Camera]
Position = {cam_x:.2f}, {cam_y:.2f}, {cam_z:.2f}
Yaw = {yaw:.1f}
Pitch = {pitch:.1f}

[Sun]
# 太陽を切り、確かめたい光だけで照らす
Enabled = false
Shadow = false
"""


def emit_scenes():
    os.makedirs(SCENE_DIR, exist_ok=True)
    path = os.path.join(SCENE_DIR, "MeshLightQuad.kscene")
    with open(path, "w", encoding="utf-8", newline="\r\n") as fp:
        fp.write(SCENE_TEMPLATE.format(
            size=PANEL_SIZE, height=PANEL_HEIGHT, exposure=SCENE_EXPOSURE_EV100,
            cam_x=CAMERA_POSITION[0], cam_y=CAMERA_POSITION[1], cam_z=CAMERA_POSITION[2],
            yaw=CAMERA_YAW, pitch=CAMERA_PITCH))
    print(".kscene を書き出しました: %s" % os.path.abspath(path))


def report():
    area = PANEL_SIZE * PANEL_SIZE
    print("")
    print("発光パネル: %.1f x %.1f m  面積 A = %.2f m^2  高さ %.1f m" %
          (PANEL_SIZE, PANEL_SIZE, area, PANEL_HEIGHT))
    print("三角形は2枚。参照実装(全三角形総当たり)が確実に回る規模")
    print("")
    print("距離ごとの d/sqrt(A) ―― 点近似の目安は 5 以上で誤差1%以下:")
    root = math.sqrt(area)
    for r in (0.0, 2.0, 5.0, 10.0, 20.0):
        d = math.sqrt(r * r + PANEL_HEIGHT * PANEL_HEIGHT)
        print("  床の中心から %5.1f m (距離 %5.2f m): d/sqrt(A) = %5.2f%s" %
              (r, d, d / root, "  ← 点近似が成り立たない" if d / root < 5.0 else ""))
    print("")
    print("【この表は目安であって合格線ではない】実際に乖離が1%を超える距離は")
    print("測って決めること。上の 5倍則 は円板の軸上の話で、床の斜めの点では別の値になる")


def main(argv):
    write_gltf()
    emit_scenes()
    report()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
