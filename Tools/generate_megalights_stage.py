#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""MegaLights の日常検証用ステージ(glTF)を生成する。

--- なぜ作るのか ---
これまで MegaLights を確かめていた LightTest.kmodel は「床20x20 + 後ろ壁1枚 + 球4個」で、
**影を落とす相手がほとんどいなかった**。MegaLights の主張は
「影付きのローカルライトを、灯数に依存しない固定費で出せる」(docs 61.7e.5)なので、
遮蔽が薄いシーンでは主張そのものを確かめられない。

実物の街(Scenes/BistroExteriorNight.kscene)は画質とデモの本命だが、.kgeom が 264MB あり
毎回の切り分けには重い。こちらは**遮蔽が濃く、灯数を制御でき、軽い**ステージを担当する。

--- 何を入れてあるか ---
  回廊(2層)   … 柱と迫り持ちアーチ。1灯の影が別の灯の照らす面に落ちる「深い遮蔽」を作る
  手すり       … 細い縦桟。接触影と半影の分解能が見える「薄い遮蔽物」
  中庭の箱     … 高さの違う直方体。落ち影の長さが灯ごとに変わる
  粗さの帯     … 床を4段の粗さで分ける。多数の灯の鏡面ハイライトの挙動が見える
  球           … 粗さ違いの球。ハイライトの形の比較用(LightTest から引き継ぎ)

灯は glTF の KHR_lights_punctual として埋め込む。**高さ・強度・色をばらす**
(従来の LightScale 系は全灯が高さ2.0・Intensity 300 固定だった)。

使い方:
    python Tools/generate_megalights_stage.py            # 既定の灯数で生成
    python Tools/generate_megalights_stage.py --lights 256
生成後、KurenaiPacker を通すこと:
    KurenaiPacker.exe Assets\\Source\\MegaLightsStage\\MegaLightsStage.gltf ^
        -o Assets\\Packed\\MegaLightsStage\\MegaLightsStage.kmodel
"""

import argparse
import json
import math
import os
import struct

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(SCRIPT_DIR, "..", "Assets", "Source", "MegaLightsStage")
GLTF_NAME = "MegaLightsStage.gltf"
BIN_NAME = "MegaLightsStage.bin"

# 中庭の寸法[m]。回廊は外周に付く
COURT_HALF = 12.0
BAY_COUNT = 8               # 1辺あたりの柱間の数
COLUMN_HALF = 0.35          # 柱の半幅
LEVEL_HEIGHT = 4.2          # 1層の高さ
LEVELS = 2
RAIL_POSTS_PER_BAY = 5      # 手すりの縦桟の本数
RAIL_HALF = 0.045           # 縦桟の半幅(細い遮蔽物)
ARCH_SEGMENTS = 7           # アーチの分割数

SPHERE_LAT, SPHERE_LON = 16, 32
SPHERE_ROUGHNESS = [0.05, 0.2, 0.5, 0.8]
SPHERE_RADIUS = 0.9

FLOOR_ROUGHNESS = [0.08, 0.25, 0.45, 0.7]   # 床の粗さの帯


def box(cx, cy, cz, hx, hy, hz):
    """軸並行の直方体。位置・法線・UV・インデックスを返す(面ごとに頂点を分ける)。"""
    faces = [
        ((0, 0, 1), [(-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1)]),
        ((0, 0, -1), [(1, -1, -1), (-1, -1, -1), (-1, 1, -1), (1, 1, -1)]),
        ((1, 0, 0), [(1, -1, 1), (1, -1, -1), (1, 1, -1), (1, 1, 1)]),
        ((-1, 0, 0), [(-1, -1, -1), (-1, -1, 1), (-1, 1, 1), (-1, 1, -1)]),
        ((0, 1, 0), [(-1, 1, 1), (1, 1, 1), (1, 1, -1), (-1, 1, -1)]),
        ((0, -1, 0), [(-1, -1, -1), (1, -1, -1), (1, -1, 1), (-1, -1, 1)]),
    ]
    pos, nrm, uv, idx = [], [], [], []
    for normal, corners in faces:
        base = len(pos)
        for sx, sy, sz in corners:
            pos.append((cx + sx * hx, cy + sy * hy, cz + sz * hz))
            nrm.append(normal)
        uv.extend([(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)])
        idx.extend([(base, base + 1, base + 2), (base, base + 2, base + 3)])
    return pos, nrm, uv, idx


def merge(parts):
    pos, nrm, uv, idx = [], [], [], []
    for p, n, u, i in parts:
        off = len(pos)
        pos.extend(p)
        nrm.extend(n)
        uv.extend(u)
        idx.extend([(a + off, b + off, c + off) for a, b, c in i])
    return pos, nrm, uv, idx


def sphere(radius, lat=SPHERE_LAT, lon=SPHERE_LON):
    """generate_material_test.py / generate_light_test.py と同じ構成の球。"""
    pos, nrm, uv = [], [], []
    for i in range(lat + 1):
        theta = i / lat * math.pi
        st, ct = math.sin(theta), math.cos(theta)
        for j in range(lon + 1):
            phi = j / lon * 2.0 * math.pi
            x, y, z = st * math.cos(phi), ct, st * math.sin(phi)
            pos.append((x * radius, y * radius, z * radius))
            nrm.append((x, y, z))
            uv.append((j / lon, i / lat))
    idx = []
    for i in range(lat):
        k1 = i * (lon + 1)
        k2 = k1 + lon + 1
        for j in range(lon):
            if i != 0:
                idx.append((k1 + j, k1 + j + 1, k2 + j))
            if i != lat - 1:
                idx.append((k1 + j + 1, k2 + j + 1, k2 + j))
    return pos, nrm, uv, idx


def bay_centres():
    """回廊の柱間の中心を、外周に沿って返す。(x, z, 内向きの法線) の並び。"""
    out = []
    step = 2.0 * COURT_HALF / BAY_COUNT
    for b in range(BAY_COUNT):
        t = -COURT_HALF + step * (b + 0.5)
        out.append((t, -COURT_HALF, (0.0, 0.0, 1.0)))
        out.append((t, COURT_HALF, (0.0, 0.0, -1.0)))
        out.append((-COURT_HALF, t, (1.0, 0.0, 0.0)))
        out.append((COURT_HALF, t, (-1.0, 0.0, 0.0)))
    # (x, z) の順に直す
    fixed = []
    for i, (a, b, n) in enumerate(out):
        fixed.append((a, b, n) if i % 4 < 2 else (b, a, n))
    return fixed


def column_positions():
    """柱の中心。柱間の境目に立てる。"""
    step = 2.0 * COURT_HALF / BAY_COUNT
    xs = [-COURT_HALF + step * b for b in range(BAY_COUNT + 1)]
    out = []
    for x in xs:
        out.append((x, -COURT_HALF))
        out.append((x, COURT_HALF))
    for z in xs:
        out.append((-COURT_HALF, z))
        out.append((COURT_HALF, z))
    return sorted(set(out))


def build_geometry():
    """(名前, 粗さ, メタリック, ベースカラー, ジオメトリ) の並びを返す。"""
    groups = []
    step = 2.0 * COURT_HALF / BAY_COUNT
    depth = 3.0                      # 回廊の奥行き
    outer = COURT_HALF + depth

    # --- 床。粗さの帯で4枚に分ける(鏡面ハイライトの比較用) ---
    bands = len(FLOOR_ROUGHNESS)
    for i, rough in enumerate(FLOOR_ROUGHNESS):
        z0 = -outer + 2.0 * outer * i / bands
        z1 = -outer + 2.0 * outer * (i + 1) / bands
        groups.append((
            "Floor_%d" % i, rough, 0.0, [0.62, 0.60, 0.58, 1.0],
            box(0.0, -0.05, (z0 + z1) * 0.5, outer, 0.05, (z1 - z0) * 0.5)))

    # --- 外壁(回廊の背面)。灯が外へ漏れないようにして遮蔽を濃くする ---
    walls = []
    h = LEVEL_HEIGHT * LEVELS
    for sx, sz in ((0, 1), (0, -1), (1, 0), (-1, 0)):
        cx, cz = outer * sx, outer * sz
        hx = 0.25 if sx else outer
        hz = 0.25 if sz else outer
        walls.append(box(cx, h * 0.5, cz, hx, h * 0.5, hz))
    groups.append(("Wall", 0.6, 0.0, [0.55, 0.53, 0.50, 1.0], merge(walls)))

    # --- 柱(各層) ---
    cols = []
    for level in range(LEVELS):
        y0 = level * LEVEL_HEIGHT
        for x, z in column_positions():
            cols.append(box(x, y0 + LEVEL_HEIGHT * 0.5, z,
                            COLUMN_HALF, LEVEL_HEIGHT * 0.5, COLUMN_HALF))
    groups.append(("Column", 0.35, 0.0, [0.72, 0.70, 0.66, 1.0], merge(cols)))

    # --- アーチ(柱間の上部を階段状の楔で埋める。深い遮蔽を作る) ---
    arches = []
    for level in range(LEVELS):
        y0 = level * LEVEL_HEIGHT
        top = y0 + LEVEL_HEIGHT
        span = step * 0.5 - COLUMN_HALF
        for cx, cz, n in bay_centres():
            for s in range(ARCH_SEGMENTS):
                f0 = s / ARCH_SEGMENTS
                f1 = (s + 1) / ARCH_SEGMENTS
                # 円弧の高さぶんだけ下へ張り出す楔
                d0 = span * math.sqrt(max(0.0, 1.0 - f0 * f0))
                d1 = span * math.sqrt(max(0.0, 1.0 - f1 * f1))
                hy = (d0 - d1) * 0.5
                if hy <= 1e-4:
                    continue
                # 楔の中心高さ。アーチの起拱点(top - span)から円弧のぶんだけ持ち上げる
                cy = top - span + (d0 + d1) * 0.5
                along = span * (f0 + f1) * 0.5
                for sign in (-1.0, 1.0):
                    if abs(n[2]) > 0.5:
                        arches.append(box(cx + sign * along, cy, cz, span / ARCH_SEGMENTS, hy, 0.30))
                    else:
                        arches.append(box(cx, cy, cz + sign * along, 0.30, hy, span / ARCH_SEGMENTS))
    groups.append(("Arch", 0.45, 0.0, [0.70, 0.68, 0.64, 1.0], merge(arches)))

    # --- 2層目の床(回廊のギャラリー)。中庭は吹き抜け ---
    gallery = []
    y = LEVEL_HEIGHT
    for sx, sz in ((0, 1), (0, -1), (1, 0), (-1, 0)):
        cx = (COURT_HALF + outer) * 0.5 * sx
        cz = (COURT_HALF + outer) * 0.5 * sz
        hx = depth * 0.5 if sx else outer
        hz = depth * 0.5 if sz else outer
        gallery.append(box(cx, y, cz, hx, 0.12, hz))
    groups.append(("Gallery", 0.5, 0.0, [0.60, 0.58, 0.55, 1.0], merge(gallery)))

    # --- 手すりの縦桟(細い遮蔽物。接触影と半影の分解能が見える) ---
    rails = []
    for cx, cz, n in bay_centres():
        for k in range(RAIL_POSTS_PER_BAY):
            f = (k + 0.5) / RAIL_POSTS_PER_BAY - 0.5
            if abs(n[2]) > 0.5:
                px, pz = cx + f * step, cz + n[2] * 0.15
            else:
                px, pz = cx + n[0] * 0.15, cz + f * step
            rails.append(box(px, LEVEL_HEIGHT + 0.12 + 0.55, pz, RAIL_HALF, 0.55, RAIL_HALF))
    # 笠木
    for sx, sz in ((0, 1), (0, -1), (1, 0), (-1, 0)):
        cx, cz = COURT_HALF * sx, COURT_HALF * sz
        hx = 0.09 if sx else COURT_HALF
        hz = 0.09 if sz else COURT_HALF
        rails.append(box(cx, LEVEL_HEIGHT + 0.12 + 1.15, cz, hx, 0.06, hz))
    groups.append(("Rail", 0.3, 0.6, [0.45, 0.44, 0.42, 1.0], merge(rails)))

    # --- 中庭の箱。高さを変えて落ち影の長さの違いを見る ---
    boxes = []
    for i, (bx, bz, bh) in enumerate([(-6.0, -5.0, 1.2), (5.5, -6.5, 2.4), (0.5, 4.5, 1.8),
                                      (-4.0, 6.0, 3.0), (7.0, 3.0, 0.8)]):
        boxes.append(box(bx, bh * 0.5, bz, 1.1, bh * 0.5, 1.1))
    groups.append(("Block", 0.55, 0.0, [0.66, 0.64, 0.60, 1.0], merge(boxes)))

    # --- 粗さ違いの球(ハイライトの形の比較) ---
    for i, rough in enumerate(SPHERE_ROUGHNESS):
        x = (i - (len(SPHERE_ROUGHNESS) - 1) / 2.0) * 2.8
        p, n, u, idx = sphere(SPHERE_RADIUS)
        p = [(a + x, b + SPHERE_RADIUS + 0.01, c - 1.5) for a, b, c in p]
        groups.append(("Sphere_%.2f" % rough, rough, 0.0, [0.9, 0.9, 0.9, 1.0], (p, n, u, idx)))

    return groups


def build_lights(count):
    """灯を作る。高さ・強度・色をばらす(従来の格子は全灯同じ値だった)。"""
    lights = []
    step = 2.0 * COURT_HALF / BAY_COUNT
    depth = 3.0
    palette = [
        (1.00, 0.71, 0.42),   # 白熱
        (1.00, 0.85, 0.65),   # 温白
        (0.65, 0.80, 1.00),   # 青白
        (1.00, 0.45, 0.30),   # 橙
        (0.45, 1.00, 0.60),   # 緑
        (0.85, 0.50, 1.00),   # 紫
    ]
    # 回廊の柱間に、層ごとに吊る
    slots = []
    for level in range(LEVELS):
        y = level * LEVEL_HEIGHT + LEVEL_HEIGHT - 0.9
        for cx, cz, n in bay_centres():
            px = cx + n[0] * depth * 0.45
            pz = cz + n[2] * depth * 0.45
            slots.append((px, y, pz))
    # 中庭の上にも吊る(吹き抜けなので下の階まで光が落ちる)
    for gx in range(5):
        for gz in range(5):
            x = -COURT_HALF * 0.7 + gx * (COURT_HALF * 1.4 / 4)
            z = -COURT_HALF * 0.7 + gz * (COURT_HALF * 1.4 / 4)
            slots.append((x, LEVEL_HEIGHT * LEVELS - 1.4, z))
    # 手すり際の低い灯(細い遮蔽物ごしの影を作る)
    for cx, cz, n in bay_centres():
        slots.append((cx + n[0] * 0.5, LEVEL_HEIGHT + 0.5, cz + n[2] * 0.5))

    for i in range(min(count, len(slots))):
        x, y, z = slots[i]
        colour = palette[i % len(palette)]
        # 強度を3段でばらす。高い灯ほど強く、届く距離も長い
        tier = i % 3
        intensity = (35.0, 70.0, 130.0)[tier]
        rng = (6.0, 9.0, 13.0)[tier]
        lights.append({
            "type": "point",
            "color": list(colour),
            "intensity": intensity,
            "range": rng,
            "_pos": (x, y, z),
        })
    return lights


def main():
    parser = argparse.ArgumentParser(description="MegaLights の検証ステージを生成する")
    parser.add_argument("--lights", type=int, default=0,
                        help="灯数(既定は配置枠すべて)。枠の数を超える指定は枠数に丸める")
    parser.add_argument("--out-dir", default=OUT_DIR)
    args = parser.parse_args()

    out_dir = os.path.abspath(args.out_dir)
    os.makedirs(out_dir, exist_ok=True)

    buffer_bytes = bytearray()
    accessors, buffer_views, meshes, materials, nodes = [], [], [], [], []

    def append_aligned(data):
        while len(buffer_bytes) % 4:
            buffer_bytes.append(0)
        offset = len(buffer_bytes)
        buffer_bytes.extend(data)
        return offset

    def add_material(name, roughness, metallic, base_color):
        materials.append({
            "name": name,
            "pbrMetallicRoughness": {
                "baseColorFactor": base_color,
                "metallicFactor": metallic,
                "roughnessFactor": roughness,
            },
        })
        return len(materials) - 1

    def add_mesh(name, geom, material_index):
        positions, normals, uvs, indices = geom
        pos_b = b"".join(struct.pack("<3f", *p) for p in positions)
        nrm_b = b"".join(struct.pack("<3f", *n) for n in normals)
        uv_b = b"".join(struct.pack("<2f", *u) for u in uvs)
        idx_b = b"".join(struct.pack("<3I", *t) for t in indices)
        offsets = [append_aligned(pos_b), append_aligned(nrm_b),
                   append_aligned(uv_b), append_aligned(idx_b)]
        lengths = [len(pos_b), len(nrm_b), len(uv_b), len(idx_b)]
        targets = [34962, 34962, 34962, 34963]
        base_view = len(buffer_views)
        for off, ln, tg in zip(offsets, lengths, targets):
            buffer_views.append({"buffer": 0, "byteOffset": off, "byteLength": ln, "target": tg})
        pmin = [min(p[a] for p in positions) for a in range(3)]
        pmax = [max(p[a] for p in positions) for a in range(3)]
        base_acc = len(accessors)
        accessors.append({"bufferView": base_view, "byteOffset": 0, "componentType": 5126,
                          "count": len(positions), "type": "VEC3", "min": pmin, "max": pmax})
        accessors.append({"bufferView": base_view + 1, "byteOffset": 0, "componentType": 5126,
                          "count": len(normals), "type": "VEC3"})
        accessors.append({"bufferView": base_view + 2, "byteOffset": 0, "componentType": 5126,
                          "count": len(uvs), "type": "VEC2"})
        accessors.append({"bufferView": base_view + 3, "byteOffset": 0, "componentType": 5125,
                          "count": len(indices) * 3, "type": "SCALAR"})
        meshes.append({
            "name": name,
            "primitives": [{
                "attributes": {"POSITION": base_acc, "NORMAL": base_acc + 1,
                               "TEXCOORD_0": base_acc + 2},
                "indices": base_acc + 3,
                "material": material_index,
            }],
        })
        return len(meshes) - 1

    root_nodes = []
    tri_total = 0
    for name, rough, metal, colour, geom in build_geometry():
        mat = add_material(name, rough, metal, colour)
        mesh = add_mesh(name, geom, mat)
        nodes.append({"name": name, "mesh": mesh})
        root_nodes.append(len(nodes) - 1)
        tri_total += len(geom[3])

    count = args.lights if args.lights > 0 else 10 ** 9
    lights = build_lights(count)
    exposure, illuminance = derive_exposure(lights)
    print("床の照度(中央値) %.1f lx -> Exposure %.1f" % (illuminance, exposure))
    gltf_lights = []
    for i, light in enumerate(lights):
        pos = light.pop("_pos")
        gltf_lights.append(light)
        # KHR_lights_punctual のライトはノードの原点に置かれる。位置は親ノードで与える
        nodes.append({"name": "Light_%03d" % i, "translation": [pos[0], pos[1], pos[2]],
                      "children": [len(nodes) + 1]})
        parent = len(nodes) - 1
        nodes.append({"name": "LightNode_%03d" % i,
                      "extensions": {"KHR_lights_punctual": {"light": i}}})
        root_nodes.append(parent)

    with open(os.path.join(out_dir, BIN_NAME), "wb") as fp:
        fp.write(buffer_bytes)

    gltf = {
        "asset": {"version": "2.0", "generator": "KurenaiEngine MegaLightsStage generator"},
        "extensionsUsed": ["KHR_lights_punctual"],
        "extensions": {"KHR_lights_punctual": {"lights": gltf_lights}},
        "scene": 0,
        "scenes": [{"nodes": root_nodes}],
        "nodes": nodes,
        "meshes": meshes,
        "materials": materials,
        "accessors": accessors,
        "bufferViews": buffer_views,
        "buffers": [{"uri": BIN_NAME, "byteLength": len(buffer_bytes)}],
    }
    path = os.path.join(out_dir, GLTF_NAME)
    with open(path, "w", encoding="utf-8", newline="\n") as fp:
        json.dump(gltf, fp, indent=2)

    print("メッシュ %d / マテリアル %d / 三角形 %d / 灯 %d / buffer %d bytes"
          % (len(meshes), len(materials), tri_total, len(gltf_lights), len(buffer_bytes)))
    print("wrote %s" % os.path.normpath(path))
    print("wrote %s" % os.path.normpath(os.path.join(out_dir, BIN_NAME)))

    scene_path = os.path.normpath(os.path.join(SCRIPT_DIR, "..", "Scenes", "MegaLightsStage.kscene"))
    os.makedirs(os.path.dirname(scene_path), exist_ok=True)
    with open(scene_path, "w", encoding="utf-8", newline="\n") as fp:
        fp.write(scene_text(len(gltf_lights), tri_total, exposure, illuminance))
    print("wrote %s" % scene_path)
    print("パックするには:")
    print("  KurenaiPacker.exe Assets\\Source\\MegaLightsStage\\MegaLightsStage.gltf "
          "-o Assets\\Packed\\MegaLightsStage\\MegaLightsStage.kmodel")
    print("  KurenaiPacker.exe --scene Scenes\\MegaLightsStage.kscene "
          "-o Assets\\Packed\\Scenes\\MegaLightsStage.kscene")
    return 0


FLOOR_ALBEDO = 0.62      # 床のベースカラー(build_geometry の Floor_* と合わせる)
MID_GREY = 0.18          # トーンマップの中間グレー


def derive_exposure(lights):
    """灯の配置から [Scene] Exposure(EV100)を導く。

    【値を他のシーンから借りてはいけない】最初 Bistro夜景と同じ 2.0 をそのまま書いたら
    画面全体が真っ白に飽和した。あちらは街灯1灯が路面へ作る 15 lx 程度から導いた値で、
    このステージは 121 灯が回廊の内側を互いに照らすので桁が違う。

    中庭の床を格子状に標本し、各点の照度 E = Σ I·cosθ/d²(Range で打ち切る)を求める。
    拡散面の輝度は L = E·albedo/π、トーンマップ入力は L/(1.2·2^EV100) なので、
    標本の中央値が中間グレー 0.18 に載る EV100 を返す。
    """
    samples = []
    step = 1.5
    x = -COURT_HALF
    while x <= COURT_HALF:
        z = -COURT_HALF
        while z <= COURT_HALF:
            total = 0.0
            for l in lights:
                lx, ly, lz = l["_pos"]
                dx, dy, dz = lx - x, ly - 0.0, lz - z
                d2 = dx * dx + dy * dy + dz * dz
                d = math.sqrt(d2)
                if d >= l["range"] or d < 1e-3:
                    continue
                # Karis/Frostbite の窓関数つき逆二乗(PunctualLighting.hlsli と同じ形)
                t = d / l["range"]
                window = max(0.0, 1.0 - t * t * t * t)
                cos_theta = max(0.0, dy / d)       # 床の法線は +Y
                total += l["intensity"] * cos_theta * window * window / d2
            samples.append(total)
            z += step
        x += step
    samples.sort()
    illuminance = samples[len(samples) // 2]
    if illuminance <= 0.0:
        return 2.0, 0.0
    luminance = illuminance * FLOOR_ALBEDO / math.pi
    return math.log2(luminance / (1.2 * MID_GREY)), illuminance


def scene_text(light_count, tri_total, exposure, illuminance):
    """ステージ用の .kscene。灯は .kmodel に埋め込まれているのでここでは置かない。"""
    eye_y = LEVEL_HEIGHT * 0.5
    # -Z 側の柱間のうち、中央にいちばん近いものの X
    nearest_bay_x = min((c[0] for c in bay_centres() if c[1] == -COURT_HALF), key=abs)
    lines = [
        "# KurenaiEngine シーンファイル - MegaLights 検証ステージ",
        "#",
        "# Tools/generate_megalights_stage.py が生成する(手で編集しない)。",
        "#",
        "# --- 何のためのシーンか ---",
        "# MegaLights の主張は「影付きのローカルライトを、灯数に依存しない固定費で出せる」",
        "# (docs/ImplementationDetail.md 61.7e.5)。従来の LightScale 系は床と壁と球4個しか",
        "# 無く、影を落とす相手がいなかったため主張を確かめられなかった。",
        "# このステージは2層の回廊・アーチ・手すりの縦桟・中庭の箱で遮蔽を濃くしてある。",
        "#   深い遮蔽   … 柱とアーチ。1灯の影が別の灯の照らす面に落ちる",
        "#   薄い遮蔽物 … 手すりの縦桟(半幅 %.3fm)。接触影と半影の分解能が見える" % RAIL_HALF,
        "#   粗さの帯   … 床を4段(%s)に分け、多数の灯の鏡面ハイライトを見る"
        % ", ".join("%.2f" % r for r in FLOOR_ROUGHNESS),
        "#",
        "# 灯 %d 個は .kmodel に埋め込まれている(glTF の KHR_lights_punctual)。" % light_count,
        "# 三角形は %d 枚で、Bistro屋外(284万枚)に比べて桁違いに軽い。" % tri_total,
        "# **高さ・強度・色をばらしてある**(従来の LightScale 系は全灯が高さ2.0・",
        "# Intensity 300 固定で、灯ごとの違いが絵に出なかった)。",
        "#",
        "# 起動:",
        "#   Sample3D.exe -scene MegaLightsStage -dx12 -megalights 2 -autoexposure 0",
        "# MegaLights は DX12 + DXR Tier 1.1 が必須で、既定は無効。-dx12 を忘れると",
        "# 「シーンは出るが MegaLights は走っていない」状態になる。",
        "#",
        "# 確認手順:",
        "#   1. -megalights 0 と 2 を見比べる。2 では柱とアーチと手すりの影が出る",
        "#   2. Lighting パネルの光源半径を振ると、手すりの縦桟の影の縁のボケ方が変わる",
        "#   3. -megalights 1(参照実装・全灯総当たり)と 2(確率的)を -megalightsaccum で",
        "#      蓄積平均して突き合わせる。系統差が出なければ確率的サンプリングは正しい",
        "",
        "[Scene]",
        "# 器具光が主役になる露出。自動露出は切って使うこと(構図で露出が振れると",
        "# A/B 比較が成立しない)。",
        "#",
        "# 【この値は灯の配置から導いている】中庭の床を1.5m格子で標本し、各点の照度を",
        "# E = Σ I·cosθ/d²(Range の窓関数つき)で求めた中央値が %.1f lx。" % illuminance,
        "# 拡散面の輝度 L = E·albedo/π(albedo %.2f)がトーンマップの中間グレー 0.18 に" % FLOOR_ALBEDO,
        "# 載る EV100 を採っている。",
        "# 最初 Bistro夜景と同じ 2.0 をそのまま書いたら画面全体が真っ白に飽和した。",
        "# あちらは街灯1灯が路面へ作る 15 lx 程度から導いた値で、%d 灯が回廊の内側を" % light_count,
        "# 互いに照らすこのステージとは桁が違う。**露出を他のシーンから借りてはいけない。**",
        "Exposure = %.1f" % exposure,
        "# 色付きの灯が芯から白く抜けないもの。既定の AgX はハイライトを白へ脱色する。",
        "Tonemap = ACES",
        "",
        "[Model]",
        "Path = MegaLightsStage/MegaLightsStage.kmodel",
        "",
        "[Camera]",
        "# 中庭の手前から回廊を見通す位置。目線の高さは1層目の中ほど。",
        "# X は柱間の中心に置く。柱は %.1fm 間隔で立っているので、x=0 だと柱を" % (2.0 * COURT_HALF / BAY_COUNT),
        "# 正面から見ることになり、中庭が柱で塞がれる。",
        "Position = %.3f, %.3f, %.3f" % (nearest_bay_x, eye_y, -(COURT_HALF + 1.5)),
        "# Yaw は +Z を0度、+X を90度として測る。0 は +Z 方向(中庭の奥)を向く。",
        "Yaw = 0.0",
        "Pitch = -4.0",
        "",
        "[Sun]",
        "# 【Enabled = false だけでは足りない】これが消すのは太陽の平行光だけで、",
        "# 空のIBL(環境光)は TimeOfDay が決める。既定は 12.0(真昼)で、天空照度は",
        "# 20000 lx 級。中庭は空へ開いているので、灯の寄与(床で中央値 %.1f lx)が" % illuminance,
        "# 3桁埋もれて画面全体が真っ白に飽和する。実際これで一度失敗した。",
        "# twilightFactor が厳密に 0 になるのは 19.0 以降なので 22.0 を採る。",
        "TimeOfDay = 22.0",
        "Enabled = false",
        "Shadow = false",
        "",
    ]
    return "\n".join(lines)


if __name__ == "__main__":
    raise SystemExit(main())
