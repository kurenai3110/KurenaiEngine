#!/usr/bin/env python3
"""DDGI の二重計上(-emissivelightsddgi)を測るための glTF と .kscene を生成する。

【なぜ専用のシーンが要るのか】単位変換を確かめる EmissiveLightTest は、発光体を
**わざと小さく**してある(半径0.1m)。プロキシの分母 d^2+R^2 と点光源の d^2 の差を
測るには半径が小さいほうがよいからで、これは GI の検証とは要求が正反対になる。

実測: EmissiveLightTest では、シーン唯一の光源である球の自発光を **500倍**にしても
DDGI のイラディアンスアトラスが**1画素も動かなかった**(プロキシの光を入れると
58%の画素が動くので、DDGI そのものは機能している)。プローブのキャプチャは1面16x16
(1テクセル約5.6度)しかなく、半径0.1mの球は最寄りプローブ以外では1テクセルに満たない。
**捉えられていないものは二重に数えられない**ので、あのシーンではつまみを検証できない。

【このシーンの作り】4x4m の発光パネルを高さ3mに**下向き**で吊る。プローブは
その真下 0.25〜2.5m に置くので、パネルは視野の大半を占める。二重計上が起きるなら
必ず出る大きさにしてある。

    -emissivelights 1 -emissivelightsddgi 0   … 抑止する
    -emissivelights 1 -emissivelightsddgi 1   … 抑止しない(二重に数える)

この2つで DDGI のイラディアンスアトラス(-debugview 26)に差が出ること。
**差ゼロを合格にしない。**

【スクリーンショットで測るときの注意】ImGui は実カーソルがウィンドウ上にあると
36x36 のソフトウェアカーソルをビューポート内に描く。撮るたびに有無が変わるので、
差分画素の**形**を先に見ること(対称な十字なら、それは信号ではない)。

使い方:
    python Tools/generate_emissive_gi_test.py                 # glTF を書く
    <KurenaiPacker で Assets/Packed/EmissiveGITest へ変換>
    python Tools/generate_emissive_gi_test.py --emit-scene    # .kscene を書く
"""

import argparse
import json
import os
import struct
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_DIR = os.path.join(SCRIPT_DIR, "..")
OUT_DIR = os.path.join(REPO_DIR, "Assets", "Source", "EmissiveGITest")
SCENE_DIR = os.path.join(REPO_DIR, "Scenes")
NAME = "EmissiveGITest"

# 発光パネルの1辺[m]と高さ[m]。プローブから見て大きく見えることだけが要件
PANEL_SIZE = 4.0
PANEL_HEIGHT = 3.0
# 床の半径[m]
FLOOR_HALF_SIZE = 12.0

SCENE_EXPOSURE_EV100 = 15.0
CAMERA_POSITION = (0.0, 1.6, -9.0)
CAMERA_YAW = 0.0
CAMERA_PITCH = -8.0


def quad(corners, normal):
    # 巻き順は generate_emissive_light_test.py と同じ((0,2,1),(0,3,2))。
    # 素直に (0,1,2),(0,2,3) と張ると2枚目が裏を向く
    return list(corners), [normal] * 4, [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)], [(0, 2, 1), (0, 3, 2)]


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
        nrm_bytes = b"".join(struct.pack("<3f", *n) for n in normals)
        nrm_offset = append_aligned(nrm_bytes)
        uv_bytes = b"".join(struct.pack("<2f", *uv) for uv in uvs)
        uv_offset = append_aligned(uv_bytes)
        idx_bytes = b"".join(struct.pack("<3I", *t) for t in indices)
        idx_offset = append_aligned(idx_bytes)

        pos_min = [min(p[a] for p in positions) for a in range(3)]
        pos_max = [max(p[a] for p in positions) for a in range(3)]

        bv = len(buffer_views)
        buffer_views.append({"buffer": 0, "byteOffset": pos_offset, "byteLength": len(pos_bytes), "target": 34962})
        buffer_views.append({"buffer": 0, "byteOffset": nrm_offset, "byteLength": len(nrm_bytes), "target": 34962})
        buffer_views.append({"buffer": 0, "byteOffset": uv_offset, "byteLength": len(uv_bytes), "target": 34962})
        buffer_views.append({"buffer": 0, "byteOffset": idx_offset, "byteLength": len(idx_bytes), "target": 34963})

        ac = len(accessors)
        accessors.append({"bufferView": bv, "byteOffset": 0, "componentType": 5126,
                          "count": len(positions), "type": "VEC3", "min": pos_min, "max": pos_max})
        accessors.append({"bufferView": bv + 1, "byteOffset": 0, "componentType": 5126,
                          "count": len(positions), "type": "VEC3"})
        accessors.append({"bufferView": bv + 2, "byteOffset": 0, "componentType": 5126,
                          "count": len(positions), "type": "VEC2"})
        accessors.append({"bufferView": bv + 3, "byteOffset": 0, "componentType": 5125,
                          "count": len(indices) * 3, "type": "SCALAR"})

        index = len(meshes)
        meshes.append({
            "name": name,
            "primitives": [{
                "attributes": {"POSITION": ac, "NORMAL": ac + 1, "TEXCOORD_0": ac + 2},
                "indices": ac + 3,
                "material": material_index,
            }],
        })
        return index

    def add_material(name, roughness, metallic, base_color, emissive=None):
        index = len(materials)
        m = {
            "name": name,
            "pbrMetallicRoughness": {
                "baseColorFactor": base_color, "metallicFactor": metallic, "roughnessFactor": roughness,
            },
        }
        if emissive is not None:
            m["emissiveFactor"] = emissive
        materials.append(m)
        return index

    # 床。反射率0.5・粗さ1.0(EmissiveLightTest と同じ理由)
    floor_mat = add_material("Floor", 1.0, 0.0, [0.5, 0.5, 0.5, 1.0])
    f = FLOOR_HALF_SIZE
    floor_geo = quad([(-f, 0.0, -f), (f, 0.0, -f), (f, 0.0, f), (-f, 0.0, f)], (0.0, 1.0, 0.0))
    nodes.append({"name": "Floor", "mesh": add_mesh("Floor", *floor_geo, floor_mat)})
    root = [len(nodes) - 1]

    # 発光パネル。**下向き**(法線 -Y)。ベースカラーは黒 ―― 自分の光を拡散で受け取ると
    # 二重計上の量に自己照射が混ざり、抑止の効果だけを取り出せなくなる
    panel_mat = add_material("Panel", 1.0, 0.0, [0.0, 0.0, 0.0, 1.0], emissive=[1.0, 1.0, 1.0])
    h = PANEL_SIZE * 0.5
    y = PANEL_HEIGHT
    # 床とは逆回りに並べて法線を下へ向ける
    panel_geo = quad([(-h, y, -h), (-h, y, h), (h, y, h), (h, y, -h)], (0.0, -1.0, 0.0))
    nodes.append({"name": "Panel", "mesh": add_mesh("Panel", *panel_geo, panel_mat)})
    root.append(len(nodes) - 1)

    bin_path = os.path.join(OUT_DIR, NAME + ".bin")
    with open(bin_path, "wb") as fp:
        fp.write(buffer_bytes)

    gltf = {
        "asset": {"version": "2.0", "generator": "KurenaiEngine EmissiveGITest generator"},
        "scene": 0,
        "scenes": [{"nodes": root}],
        "nodes": nodes, "meshes": meshes, "materials": materials,
        "accessors": accessors, "bufferViews": buffer_views,
        "buffers": [{"uri": NAME + ".bin", "byteLength": len(buffer_bytes)}],
    }
    gltf_path = os.path.join(OUT_DIR, NAME + ".gltf")
    with open(gltf_path, "w", encoding="utf-8") as fp:
        json.dump(gltf, fp, indent=2)

    print("発光パネル: %.1f x %.1f m(面積 %.1f m^2) / 高さ %.2f m / 下向き"
          % (PANEL_SIZE, PANEL_SIZE, PANEL_SIZE * PANEL_SIZE, PANEL_HEIGHT))
    print("wrote %s" % gltf_path)
    print("wrote %s" % bin_path)
    print("")
    print("次に KurenaiPacker で Assets/Packed/EmissiveGITest へ変換してから、")
    print("  python Tools/generate_emissive_gi_test.py --emit-scene")


def emit_scene():
    os.makedirs(SCENE_DIR, exist_ok=True)
    text = (
        "# DDGI の二重計上(-emissivelightsddgi)を測るためのシーン。\n"
        "# Tools/generate_emissive_gi_test.py で生成される(手で編集しない)。\n"
        "#\n"
        "#   Sample3D.exe -dx12 -scene EmissiveGITest -emissivelights 1 -debugview 26"
        " -emissivelightsddgi 0\n"
        "#   同上 -emissivelightsddgi 1\n"
        "#   (-ddgiraster を足すと DDGI のラスタ経路になる。**両方の経路を踏むこと**)\n"
        "#\n"
        "# DDGI のイラディアンスアトラスに差が出ること。**差ゼロを合格にしない。**\n"
        "# 差分画素の形を先に見る ―― 対称な十字は ImGui のカーソルであって信号ではない。\n"
        "#\n"
        "# 【AO/GI を切らない】DDGI が走る条件は AO/GI が有効かつ GIボリュームがあること。\n"
        "# EmissiveLightProxy 側はノイズを避けるため切ってあり、あちらでは DDGI が走らない。\n"
        "# 【パネルは大きく作ってある】プローブのキャプチャは1面16x16(1テクセル約5.6度)。\n"
        "# 小さい発光体はプローブに写らず、写らないものは二重に数えられない。\n"
        "\n[Scene]\n"
        "Name = Emissive GI Test\n"
        "Exposure = %.1f\n"
        "TAA = false\n"
        "AmbientOcclusion = true\n"
        "IBLIntensity = 0.0\n"
        "\n[Model]\nPath = EmissiveGITest/EmissiveGITest.kmodel\n"
        "\n[Camera]\nPosition = %.2f, %.2f, %.2f\nYaw = %.1f\nPitch = %.1f\n"
        "\n[Sun]\n"
        "# 太陽を切り、発光パネルだけで照らす\n"
        "Enabled = false\nShadow = false\n"
        "TimeOfDay = 12.0\n"
        "\n[GIVolume]\n"
        "# パネル(高さ %.1f m)の**真下**にプローブを敷く。上に出すとパネルの裏を見ることになる\n"
        "Name = EmissiveGITest\n"
        "Origin = -6.0, 0.25, -6.0\n"
        "ProbeSpacing = 1.5, 0.75, 1.5\n"
        "ProbeCounts = 9, 4, 9\n"
        "NormalBias = 0.25\n"
        "ViewBias = 0.10\n"
        "Hysteresis = 0.97\n"
        "MaxRayDistance = 20.0\n"
        % (SCENE_EXPOSURE_EV100,
           CAMERA_POSITION[0], CAMERA_POSITION[1], CAMERA_POSITION[2], CAMERA_YAW, CAMERA_PITCH,
           PANEL_HEIGHT)
    )
    path = os.path.join(SCENE_DIR, NAME + ".kscene")
    with open(path, "w", encoding="utf-8", newline="\r\n") as fp:
        fp.write(text)
    print("wrote %s" % path)
    return 0


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--emit-scene", action="store_true", help=".kscene を書く(glTF は書かない)")
    args = parser.parse_args(argv)
    if args.emit_scene:
        return emit_scene()
    write_gltf()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
