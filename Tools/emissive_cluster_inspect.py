#!/usr/bin/env python3
"""エミッシブなメッシュから起こす「光源のかたまり」を、アプリを起動せずに数える検査ツール。

ModelLoader.cpp の BuildEmissiveClusters と**同じ手順**を Python で再現する。目的は2つ:

  1. 「7個に割れた」が溶接の効き具合によるものか、グリッド分割によるものかを切り分ける。
     エンジンを起動して数えるだけだと、内訳が分からないまま数字だけが出る
  2. かたまりの重心を出して、手置きのポイントライト(.kscene の [Light])と突き合わせる。
     クラスタリングの良し悪しには「いくつに割るのが正しいか」の内部基準が無いので、
     外部の基準がこれしか無い

【これは独立な実装ではない】C++ 版と同じ式・同じしきい値を意図的に写している。
数が一致しても「C++ 版が正しい」ことの証明にはならない。分かるのは
「手順のどの段が何個を作っているか」と「重心がどこか」だけ。

使い方:
    python Tools/emissive_cluster_inspect.py Assets/Packed/BistroMcGuire/Interior.kmodel
    python Tools/emissive_cluster_inspect.py <model.kmodel> --split 0        # 段Bを無効化
    python Tools/emissive_cluster_inspect.py <model.kmodel> --scene Scenes/BistroInteriorLit.kscene
"""

import argparse
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from kmodel_inspect import KModelError, read_geometry_payload, read_kmodel  # noqa: E402

# Vertex.h と一致させること(Position[3] / Normal[3] / UV[2] / Tangent[4] / UV1[2])
VERTEX_STRIDE = 56
POSITION_OFFSET = 0


def _llround(x):
    """C++ の std::llround と同じ丸め(0から遠い側へ。-0.5 は -1)。

    【Python の round() を使ってはいけない】あちらは偶数丸めなので、ちょうど .5 の位置で
    C++ と食い違う。溶接キーの丸めが1つずれると、その頂点だけ別のかたまりに落ちる。
    今のアセットでは差が出ないが、写しとして非等価な状態にしておくと、
    いつか「Pythonでは合っているのにエンジンでは違う」を追うことになる。
    """
    return int(math.floor(x + 0.5)) if x >= 0.0 else -int(math.floor(-x + 0.5))


def _weld_epsilon(bounds_min, bounds_max):
    """ModelLoader.cpp と同じ溶接しきい値。メッシュのAABB対角に比例させる。"""
    diagonal = math.sqrt(sum((bounds_max[i] - bounds_min[i]) ** 2 for i in range(3)))
    return min(max(1e-5 * diagonal, 1e-5), 1e-3)


class UnionFind(object):
    def __init__(self, count):
        self.parent = list(range(count))

    def find(self, v):
        root = v
        while self.parent[root] != root:
            root = self.parent[root]
        while self.parent[v] != root:
            self.parent[v], v = root, self.parent[v]
        return root

    def unite(self, a, b):
        ra, rb = self.find(a), self.find(b)
        if ra == rb:
            return
        # 小さい番号を親にする(C++ 側と同じ。決定性のため)
        if ra < rb:
            self.parent[rb] = ra
        else:
            self.parent[ra] = rb


def _merge_groups(order, groups, cluster_scale):
    """段C: 重心が cluster_scale より近いかたまりを併合する。ModelLoader.cpp と同じ貪欲法。

    種は面積の大きい順(同値なら重心の辞書順)。C++ 側は空間ハッシュで近傍だけを見るが、
    セル幅を併合距離に取っているので結果は総当たりと同一になる(枝刈りしかしていない)。
    """
    if cluster_scale <= 0.0:
        return {key: i for i, key in enumerate(order)}, len(order)

    provisional = {}
    for key in order:
        g = groups[key]
        inv = 1.0 / g["Area"] if g["Area"] > 0.0 else 0.0
        provisional[key] = [g["CentroidSum"][i] * inv for i in range(3)]

    seeds = sorted(order, key=lambda k: (-groups[k]["Area"], provisional[k]))
    assigned = {}
    count = 0
    for seed in seeds:
        if seed in assigned:
            continue
        target = count
        count += 1
        assigned[seed] = target
        ps = provisional[seed]
        for other in seeds:
            if other in assigned:
                continue
            d2 = sum((provisional[other][i] - ps[i]) ** 2 for i in range(3))
            if d2 <= cluster_scale * cluster_scale:
                assigned[other] = target
    return assigned, count


def build_clusters(positions, indices, bounds_min, bounds_max, cluster_scale, merge=True):
    """ModelLoader.cpp の BuildEmissiveClusters と同じ手順でかたまりへ分ける。

    merge=False にすると段C(併合)だけを切る。
    【段Bと段Cを1つのノブで兼ねさせない】cluster_scale=0 は分割と併合を**同時に**切るので、
    既定値との差を「併合の効果」として読むことはできない。実測では ProbeTest が
    連結成分14個 → 段Bで28個 → 段Cで28個(併合は0個)で、両方を切ると符号すら逆に見える。
    """
    vertex_count = len(positions)
    triangle_count = len(indices) // 3
    if vertex_count == 0 or triangle_count == 0:
        return []

    # --- 段A: 位置で溶接してから連結成分 ---
    inv_weld = 1.0 / _weld_epsilon(bounds_min, bounds_max)
    uf = UnionFind(vertex_count)
    weld_map = {}
    for v, p in enumerate(positions):
        key = (_llround(p[0] * inv_weld), _llround(p[1] * inv_weld), _llround(p[2] * inv_weld))
        first = weld_map.setdefault(key, v)
        if first != v:
            uf.unite(first, v)

    for t in range(triangle_count):
        i0, i1, i2 = indices[t * 3], indices[t * 3 + 1], indices[t * 3 + 2]
        if max(i0, i1, i2) >= vertex_count:
            continue
        uf.unite(i0, i1)
        uf.unite(i1, i2)

    # --- 三角形を「かたまり」へ割り当てて累積する ---
    inv_split = (1.0 / cluster_scale) if cluster_scale > 0.0 else 0.0
    groups = {}
    order = []
    for t in range(triangle_count):
        i0, i1, i2 = indices[t * 3], indices[t * 3 + 1], indices[t * 3 + 2]
        if max(i0, i1, i2) >= vertex_count:
            continue
        p0, p1, p2 = positions[i0], positions[i1], positions[i2]
        e1 = [p1[i] - p0[i] for i in range(3)]
        e2 = [p2[i] - p0[i] for i in range(3)]
        cross = [
            e1[1] * e2[2] - e1[2] * e2[1],
            e1[2] * e2[0] - e1[0] * e2[2],
            e1[0] * e2[1] - e1[1] * e2[0],
        ]
        area = 0.5 * math.sqrt(sum(c * c for c in cross))
        if area <= 1e-12:
            continue
        centroid = [(p0[i] + p1[i] + p2[i]) / 3.0 for i in range(3)]

        cell = (0, 0, 0)
        if cluster_scale > 0.0:
            cell = tuple(int(math.floor((centroid[i] - bounds_min[i]) * inv_split)) for i in range(3))
        key = (uf.find(i0),) + cell

        g = groups.get(key)
        if g is None:
            g = {"Area": 0.0, "CentroidSum": [0.0] * 3, "NormalSum": [0.0] * 3,
                 "OwnMoment": 0.0, "Triangles": [], "Count": 0}
            groups[key] = g
            order.append(key)
        g["Area"] += area
        g["Count"] += 1
        g["Triangles"].append((centroid, area))
        for i in range(3):
            g["CentroidSum"][i] += area * centroid[i]
            g["NormalSum"][i] += 0.5 * cross[i]
        edge_sq = 0.0
        for a, b in ((p0, p1), (p1, p2), (p2, p0)):
            edge_sq += sum((b[i] - a[i]) ** 2 for i in range(3))
        g["OwnMoment"] += area * edge_sq / 36.0

    # --- 段C: 近すぎるかたまりを併合する ---
    assigned, merged_count = _merge_groups(order, groups, cluster_scale if merge else 0.0)
    merged = []
    for _ in range(merged_count):
        merged.append({"Area": 0.0, "CentroidSum": [0.0] * 3, "NormalSum": [0.0] * 3,
                       "OwnMoment": 0.0, "Triangles": [], "Count": 0})
    for key in order:
        src = groups[key]
        dst = merged[assigned[key]]
        dst["Area"] += src["Area"]
        dst["OwnMoment"] += src["OwnMoment"]
        dst["Count"] += src["Count"]
        dst["Triangles"].extend(src["Triangles"])
        for i in range(3):
            dst["CentroidSum"][i] += src["CentroidSum"][i]
            dst["NormalSum"][i] += src["NormalSum"][i]

    # --- かたまりごとの値を確定する ---
    clusters = []
    for g in merged:
        if g["Area"] <= 0.0:
            continue
        inv_area = 1.0 / g["Area"]
        centroid = [g["CentroidSum"][i] * inv_area for i in range(3)]
        spread = 0.0
        for c, area in g["Triangles"]:
            spread += area * sum((c[i] - centroid[i]) ** 2 for i in range(3))
        normal_length = math.sqrt(sum(n * n for n in g["NormalSum"]))
        clusters.append({
            "Centroid": centroid,
            "Area": g["Area"],
            "Directionality": min(max(normal_length * inv_area, 0.0), 1.0),
            "Normal": [n / normal_length for n in g["NormalSum"]] if normal_length > 1e-12 else [0.0, 1.0, 0.0],
            "SourceRadius": math.sqrt(max(0.0, 2.0 * (g["OwnMoment"] + spread) * inv_area)),
            "TriangleCount": g["Count"],
        })
    return clusters


def read_scene_lights(path):
    """.kscene の [Light] セクションから Position だけを拾う(突き合わせ用)。"""
    lights = []
    current = None
    with open(path, "r", encoding="utf-8") as fp:
        for raw in fp:
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            if line.startswith("["):
                if current is not None:
                    lights.append(current)
                current = {} if line.lower() == "[light]" else None
                continue
            if current is None or "=" not in line:
                continue
            key, value = (s.strip() for s in line.split("=", 1))
            if key.lower() == "position":
                current["Position"] = [float(v) for v in value.split(",")]
            elif key.lower() == "intensity":
                current["Intensity"] = float(value)
    if current is not None:
        lights.append(current)
    return [l for l in lights if "Position" in l]


def main(argv):
    parser = argparse.ArgumentParser(description="エミッシブから起こす光源のかたまりを数える")
    parser.add_argument("model", help=".kmodel のパス")
    parser.add_argument("--split", type=float, default=1.0,
                        help="かたまりの長さ尺度[m]。分割と併合の両方に使う。0で両方とも行わない(既定 1.0)")
    parser.add_argument("--no-merge", action="store_true",
                        help="段C(併合)だけを切る。段Bの分割は残す(段ごとの内訳を見るため)")
    parser.add_argument("--scene", help="突き合わせる .kscene(手置きライトとの距離を出す)")
    args = parser.parse_args(argv[1:])

    try:
        model = read_kmodel(args.model)
        _, payload = read_geometry_payload(args.model, model["GeometryPath"])
    except KModelError as e:
        print("読み込みに失敗しました: %s" % e, file=sys.stderr)
        return 1

    materials = model["Materials"]
    total = 0
    all_clusters = []
    for mesh_index, mesh in enumerate(model["Meshes"]):
        mi = mesh["MaterialIndex"]
        if mi < 0 or mi >= len(materials):
            continue
        emissive = materials[mi]["EmissiveFactor"]
        if max(emissive) <= 0.0:
            continue

        vcount, icount = mesh["VertexCount"], mesh["IndexCount"]
        vbase, ibase = mesh["VertexOffset"], mesh["IndexOffset"]
        positions = []
        for v in range(vcount):
            off = vbase + v * VERTEX_STRIDE + POSITION_OFFSET
            positions.append(list(struct.unpack_from("<3f", payload, off)))
        indices = list(struct.unpack_from("<%dI" % icount, payload, ibase))

        clusters = build_clusters(positions, indices, mesh["BoundsMin"], mesh["BoundsMax"],
                                  args.split, merge=not args.no_merge)
        total += len(clusters)
        all_clusters.extend(clusters)
        print("メッシュ %d (材質 %d, EmissiveFactor=%s, 三角形 %d): かたまり %d 個"
              % (mesh_index, mi, ["%.3f" % e for e in emissive], icount // 3, len(clusters)))
        for ci, c in enumerate(clusters):
            print("  #%-2d 重心 (%7.2f, %7.2f, %7.2f)  面積 %8.4f m^2  半径 %6.3f m  κ %.3f  三角形 %d"
                  % (ci, c["Centroid"][0], c["Centroid"][1], c["Centroid"][2],
                     c["Area"], c["SourceRadius"], c["Directionality"], c["TriangleCount"]))

    print("合計: かたまり %d 個 (長さ尺度 %.3g m / 併合 %s)"
          % (total, args.split, "なし" if args.no_merge else "あり"))

    if args.scene:
        lights = read_scene_lights(args.scene)
        print("\n手置きライト %d 個との突き合わせ (%s)" % (len(lights), os.path.basename(args.scene)))
        for li, light in enumerate(lights):
            p = light["Position"]
            best, best_d = -1, None
            for ci, c in enumerate(all_clusters):
                d = math.sqrt(sum((c["Centroid"][i] - p[i]) ** 2 for i in range(3)))
                if best_d is None or d < best_d:
                    best, best_d = ci, d
            print("  ライト%-2d (%6.2f,%6.2f,%6.2f) Intensity=%-7.1f -> 最寄りのかたまり #%-2d 距離 %6.3f m"
                  % (li, p[0], p[1], p[2], light.get("Intensity", 0.0), best, best_d))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
