"""モン・サン=ミシェル検証シーン用の、島(岩・城壁・修道院・尖塔)のプロキシを生成する。

このスクリプトが作るのは実物の詳細な3Dモデルではなく、写真の**シルエットが再現できれば
十分**という前提の簡易プロキシ(円錐台・箱・角錐の組み合わせ)。実物のおおよその寸法
(岩の高さ約92m、尖塔の先端が海抜約170m。いずれも概数)を目安にしつつ、各パーツの
具体的な半径・高さは以下の仕様として明示した値をそのまま使う:

    パーツ           形状              半径/一辺              高さ(y範囲)
    岩               円錐台(32分割)    底面125m→上面45m       0〜70
    城壁のリング      環状の壁(32分割)  外径118m/内径113m       0〜12
    修道院(裾)       直方体            半径30m(一辺60m)       70〜90
    修道院(塔)       直方体            半径14m(一辺28m)       90〜115
    尖塔             四角錐            底辺14m(一辺28m)       115〜165

床(岩の底面)を1単位=1メートルの座標系でy=0に置き、これが島全体の基準面になる
(座標系・単位の約束: 島の底面中心がワールド原点)。

法線は面ごとに正しく出す(フラットシェーディング)。各パーツは四角形/三角形の集まりで
構成し、面ごとに頂点を複製して一意の法線を持たせている(Tools/generate_probe_test.pyの
add_quadと同じ考え方: 与えた4点の周回向きから幾何法線を求め、期待する向きと逆なら
巻き順を反転して機械的に揃える)。

UVは簡易な平面投影(各面をUV空間の正方形[0,1]x[0,1]にそのまま割り当てるだけ)でよい
(このモデルにテクスチャを貼らないため)。TANGENTはUVのU方向(面の1辺)に整合する値を
入れるが、**ゼロベクトルにはしない**(接線が縮退するとGBuffer.hlslのComputeTangentFrameで
法線マップ計算が破綻するため。将来この面にテクスチャと法線マップを貼る可能性に備えて、
常に有効な値を入れておく)。

半径ノイズ: 岩の側面は完全な円錐台だと人工的すぎるため、角度依存の正弦波を数本重ねた
決定論的な凹凸(乱数ではなく純粋な三角関数)を半径に掛けて岩らしいシルエットにする。

マテリアルは2つ:
  - Limestone(石灰岩): 岩・城壁・修道院の壁
  - SlateRoof(スレート屋根): 修道院の屋根(裾側の露出した屋根面のみ)・尖塔

生成物: Assets/Source/MontSaintMichelStudy/Island.gltf + Island.bin
KurenaiPacker.exeで Assets/Packed/MontSaintMichelStudy/Island.kmodel へ変換して使う
(--bake-occlusionは付けない。プロキシ形状にAOを焼いても実物との対応が無く意味が薄く、
xatlasのUV展開が時間の無駄になるため)。
"""

import json
import math
import os
import struct
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(SCRIPT_DIR, "..", "Assets", "Source", "MontSaintMichelStudy")
GLTF_NAME = "Island.gltf"
BIN_NAME = "Island.bin"

# --- 岩(円錐台) ---
ROCK_BASE_RADIUS = 125.0
ROCK_TOP_RADIUS = 45.0
ROCK_BASE_Y = 0.0
ROCK_TOP_Y = 70.0
ROCK_SIDES = 32

# --- 城壁のリング ---
WALL_OUTER_RADIUS = 118.0
WALL_INNER_RADIUS = 113.0
WALL_BASE_Y = 0.0
WALL_TOP_Y = 12.0
WALL_SIDES = 32

# --- 修道院の建物群(段状に積む) ---
# 裾(周囲を囲む一段低い建物)。半径30m(対角42.4m)は岩の上面半径45mの内側に収まる値
# (ノイズで多少縮んでも建物が岩からはみ出さないように少し余裕を持たせてある)
TIER1_HALF_EXTENT = 30.0
TIER1_BASE_Y = 70.0
TIER1_TOP_Y = 90.0

# 中央の塔状の本体。裾の内側に収まる半径14m(対角19.8m。裾の半径30mの内側)
TIER2_HALF_EXTENT = 14.0
TIER2_BASE_Y = 90.0
TIER2_TOP_Y = 115.0

# --- 尖塔(四角錐) ---
# 底辺は塔本体の上面(TIER2_HALF_EXTENT)と一致させ、継ぎ目が生じないようにする
SPIRE_BASE_Y = 115.0
SPIRE_TIP_Y = 165.0

# --- マテリアル ---
LIMESTONE_COLOR = [0.62, 0.58, 0.50, 1.0]
LIMESTONE_ROUGHNESS = 0.90
LIMESTONE_METALLIC = 0.0

SLATE_COLOR = [0.28, 0.29, 0.32, 1.0]
SLATE_ROUGHNESS = 0.55
SLATE_METALLIC = 0.0


def _cross(a, b):
    return (
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    )


def _sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def _dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def _normalize(v):
    length = math.sqrt(_dot(v, v))
    if length < 1e-12:
        # 縮退した入力(理論上は起こらないが、念のためのフォールバック)。
        # ここでゼロ除算するとNaNが全頂点に伝播するため、単位ベクトルを返して打ち切る
        print("[WARNING] ゼロ長のベクトルを正規化しようとしました。(1,0,0)で代用します", file=sys.stderr)
        return (1.0, 0.0, 0.0)
    return (v[0] / length, v[1] / length, v[2] / length)


class MeshAccumulator:
    """1マテリアル分の頂点・インデックスをためこむ。

    ProbeTest/MaterialTestのように「面ごとに個別メッシュ+ノード」にすると、この
    プロキシは面数が数百に達し(円錐台32分割+城壁32分割x3面+建物+尖塔)ノードが
    無駄に増えるため、マテリアル単位(Limestone/SlateRoofの2つ)で1メッシュにまとめる。
    """

    def __init__(self, name):
        self.name = name
        self.positions = []
        self.normals = []
        self.uvs = []
        self.tangents = []
        self.indices = []

    def add_piece(self, positions, normals, uvs, tangents, local_triangles):
        base = len(self.positions)
        self.positions.extend(positions)
        self.normals.extend(normals)
        self.uvs.extend(uvs)
        self.tangents.extend(tangents)
        for tri in local_triangles:
            self.indices.append((base + tri[0], base + tri[1], base + tri[2]))


def add_flat_quad(accumulator, corners, expected_normal):
    """4点(周回順)から、面ごとに一意の法線を持つ平坦なクアッドを1枚追加する。

    corners の周回向きが expected_normal 側から見てCCWになっているとは限らないため、
    幾何法線(corners[0..2]の外積)と expected_normal の内積で向きを判定し、
    逆向きなら巻き順を反転して機械的に揃える(generate_probe_test.pyのgenerate_quad_double_sidedと
    同じ考え方。ここでは片面のみでよいので複製はしない)。
    """
    c0, c1, c2 = corners[0], corners[1], corners[2]
    geometric_normal = _cross(_sub(c1, c0), _sub(c2, c0))
    facing = _dot(geometric_normal, expected_normal)

    if facing >= 0.0:
        winding = [(0, 1, 2), (0, 2, 3)]
    else:
        winding = [(0, 2, 1), (0, 3, 2)]

    flat_normal = _normalize(geometric_normal if facing >= 0.0 else (-geometric_normal[0], -geometric_normal[1], -geometric_normal[2]))
    tangent_vec = _normalize(_sub(c1, c0))
    tangent = (tangent_vec[0], tangent_vec[1], tangent_vec[2], 1.0)

    positions = list(corners)
    normals = [flat_normal] * 4
    tangents = [tangent] * 4
    uvs = [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)]

    accumulator.add_piece(positions, normals, uvs, tangents, winding)


def add_flat_triangle(accumulator, corners, expected_normal):
    """3点(周回順)から、面ごとに一意の法線を持つ平坦な三角形を1枚追加する。"""
    c0, c1, c2 = corners[0], corners[1], corners[2]
    geometric_normal = _cross(_sub(c1, c0), _sub(c2, c0))
    facing = _dot(geometric_normal, expected_normal)

    if facing >= 0.0:
        winding = [(0, 1, 2)]
    else:
        winding = [(0, 2, 1)]

    flat_normal = _normalize(geometric_normal if facing >= 0.0 else (-geometric_normal[0], -geometric_normal[1], -geometric_normal[2]))
    tangent_vec = _normalize(_sub(c1, c0))
    tangent = (tangent_vec[0], tangent_vec[1], tangent_vec[2], 1.0)

    positions = list(corners)
    normals = [flat_normal] * 3
    tangents = [tangent] * 3
    uvs = [(0.0, 0.0), (1.0, 0.0), (0.5, 1.0)]

    accumulator.add_piece(positions, normals, uvs, tangents, winding)


def rock_radius_noise(angle):
    """角度依存の決定論的な凹凸(乱数ではなく正弦波の重ね合わせ)。1.0を中心に±13%程度。"""
    return (
        1.0
        + 0.06 * math.sin(angle * 5.0 + 0.7)
        + 0.04 * math.sin(angle * 11.0 + 2.1)
        + 0.03 * math.sin(angle * 17.0 + 4.0)
    )


def build_rock(accumulator):
    bottom_ring = []
    top_ring = []
    for i in range(ROCK_SIDES):
        angle = i / ROCK_SIDES * 2.0 * math.pi
        noise = rock_radius_noise(angle)
        cos_a, sin_a = math.cos(angle), math.sin(angle)
        r_bottom = ROCK_BASE_RADIUS * noise
        r_top = ROCK_TOP_RADIUS * noise
        bottom_ring.append((r_bottom * cos_a, ROCK_BASE_Y, r_bottom * sin_a))
        top_ring.append((r_top * cos_a, ROCK_TOP_Y, r_top * sin_a))

    # 側面(底面のキャップは水面下/干潟に埋まり視界に入らないため作らない)
    for i in range(ROCK_SIDES):
        j = (i + 1) % ROCK_SIDES
        mid_angle = (i + 0.5) / ROCK_SIDES * 2.0 * math.pi
        expected_normal = (math.cos(mid_angle), 0.0, math.sin(mid_angle))
        corners = [bottom_ring[i], bottom_ring[j], top_ring[j], top_ring[i]]
        add_flat_quad(accumulator, corners, expected_normal)

    # 上面(修道院が乗る台地)
    center = (0.0, ROCK_TOP_Y, 0.0)
    for i in range(ROCK_SIDES):
        j = (i + 1) % ROCK_SIDES
        corners = [center, top_ring[i], top_ring[j]]
        add_flat_triangle(accumulator, corners, (0.0, 1.0, 0.0))


def build_wall(accumulator):
    outer_ring = []
    inner_ring = []
    for i in range(WALL_SIDES):
        angle = i / WALL_SIDES * 2.0 * math.pi
        cos_a, sin_a = math.cos(angle), math.sin(angle)
        outer_ring.append((WALL_OUTER_RADIUS * cos_a, 0.0, WALL_OUTER_RADIUS * sin_a))
        inner_ring.append((WALL_INNER_RADIUS * cos_a, 0.0, WALL_INNER_RADIUS * sin_a))

    for i in range(WALL_SIDES):
        j = (i + 1) % WALL_SIDES
        mid_angle = (i + 0.5) / WALL_SIDES * 2.0 * math.pi
        outward = (math.cos(mid_angle), 0.0, math.sin(mid_angle))
        inward = (-outward[0], 0.0, -outward[2])

        # 外面
        o_bottom_i = (outer_ring[i][0], WALL_BASE_Y, outer_ring[i][2])
        o_bottom_j = (outer_ring[j][0], WALL_BASE_Y, outer_ring[j][2])
        o_top_i = (outer_ring[i][0], WALL_TOP_Y, outer_ring[i][2])
        o_top_j = (outer_ring[j][0], WALL_TOP_Y, outer_ring[j][2])
        add_flat_quad(accumulator, [o_bottom_i, o_bottom_j, o_top_j, o_top_i], outward)

        # 内面
        i_bottom_i = (inner_ring[i][0], WALL_BASE_Y, inner_ring[i][2])
        i_bottom_j = (inner_ring[j][0], WALL_BASE_Y, inner_ring[j][2])
        i_top_i = (inner_ring[i][0], WALL_TOP_Y, inner_ring[i][2])
        i_top_j = (inner_ring[j][0], WALL_TOP_Y, inner_ring[j][2])
        add_flat_quad(accumulator, [i_bottom_i, i_bottom_j, i_top_j, i_top_i], inward)

        # 天面(内外をつなぐ環状の面)
        add_flat_quad(accumulator, [o_top_i, o_top_j, i_top_j, i_top_i], (0.0, 1.0, 0.0))


def build_box(limestone_acc, half_extent, base_y, top_y, roof_acc):
    """XZ中心(0,0)の直方体。側面はlimestone_accへ、天面はroof_accへ追加する。

    天面を描くかどうかは呼び出し側がroof_accにNoneを渡すかどうかで制御する
    (この島では、より高い段が上に乗って完全に覆う天面はZファイティングの元になるため
    描かない。詳細はmain()のコメント参照)。
    """
    h = half_extent
    corners_bottom = [(-h, base_y, -h), (h, base_y, -h), (h, base_y, h), (-h, base_y, h)]
    corners_top = [(-h, top_y, -h), (h, top_y, -h), (h, top_y, h), (-h, top_y, h)]

    # -Z面, +X面, +Z面, -X面 の順に4枚
    faces = [
        ([corners_bottom[0], corners_bottom[1], corners_top[1], corners_top[0]], (0.0, 0.0, -1.0)),
        ([corners_bottom[1], corners_bottom[2], corners_top[2], corners_top[1]], (1.0, 0.0, 0.0)),
        ([corners_bottom[2], corners_bottom[3], corners_top[3], corners_top[2]], (0.0, 0.0, 1.0)),
        ([corners_bottom[3], corners_bottom[0], corners_top[0], corners_top[3]], (-1.0, 0.0, 0.0)),
    ]
    for corners, expected_normal in faces:
        add_flat_quad(limestone_acc, corners, expected_normal)

    if roof_acc is not None:
        add_flat_quad(roof_acc, corners_top, (0.0, 1.0, 0.0))


def build_spire(accumulator):
    h = TIER2_HALF_EXTENT
    base_corners = [
        (-h, SPIRE_BASE_Y, -h),
        (h, SPIRE_BASE_Y, -h),
        (h, SPIRE_BASE_Y, h),
        (-h, SPIRE_BASE_Y, h),
    ]
    apex = (0.0, SPIRE_TIP_Y, 0.0)

    for i in range(4):
        j = (i + 1) % 4
        c0, c1 = base_corners[i], base_corners[j]
        mid_x = (c0[0] + c1[0]) * 0.5
        mid_z = (c0[2] + c1[2]) * 0.5
        # 厳密な外向き法線でなくてよい(add_flat_triangleは向き判定にしか使わない)。
        # 水平方向は底辺中点の外向き、Yは緩やかな上向きを混ぜて傾斜に寄せておく
        expected_normal = _normalize((mid_x, 0.3, mid_z))
        add_flat_triangle(accumulator, [c0, c1, apex], expected_normal)


def main():
    try:
        os.makedirs(OUT_DIR, exist_ok=True)
    except OSError as error:
        print(f"[ERROR] 出力ディレクトリの作成に失敗しました: {OUT_DIR} ({error})", file=sys.stderr)
        raise

    limestone = MeshAccumulator("Limestone")
    slate = MeshAccumulator("SlateRoof")

    build_rock(limestone)
    build_wall(limestone)
    # TIER1(裾)の天面は、TIER2(塔)に覆われない部分がそのまま屋根として露出するので描く。
    # TIER2(塔)の天面は、この直後に建てるSPIRE(尖塔)の底面と同じ正方形に完全一致するため、
    # 両方描くと同一平面上に2枚のポリゴンが重なりZファイティングを起こす。SPIRE側にも
    # 底面ポリゴンを作っていないので、ここでもTIER2の天面は描かない(roof_acc=None)
    build_box(limestone, TIER1_HALF_EXTENT, TIER1_BASE_Y, TIER1_TOP_Y, roof_acc=slate)
    build_box(limestone, TIER2_HALF_EXTENT, TIER2_BASE_Y, TIER2_TOP_Y, roof_acc=None)
    build_spire(slate)

    buffer_bytes = bytearray()

    def append_aligned(data: bytes):
        while len(buffer_bytes) % 4 != 0:
            buffer_bytes.append(0)
        offset = len(buffer_bytes)
        buffer_bytes.extend(data)
        return offset

    accessors = []
    buffer_views = []
    meshes = []
    materials = []
    nodes = []

    limestone_material = len(materials)
    materials.append({
        "name": "Limestone",
        "pbrMetallicRoughness": {
            "baseColorFactor": LIMESTONE_COLOR,
            "metallicFactor": LIMESTONE_METALLIC,
            "roughnessFactor": LIMESTONE_ROUGHNESS,
        },
    })
    slate_material = len(materials)
    materials.append({
        "name": "SlateRoof",
        "pbrMetallicRoughness": {
            "baseColorFactor": SLATE_COLOR,
            "metallicFactor": SLATE_METALLIC,
            "roughnessFactor": SLATE_ROUGHNESS,
        },
    })

    total_vertex_count = 0
    total_triangle_count = 0

    for accumulator, material_index in ((limestone, limestone_material), (slate, slate_material)):
        positions = accumulator.positions
        normals = accumulator.normals
        uvs = accumulator.uvs
        tangents = accumulator.tangents
        indices = accumulator.indices

        pos_bytes = b"".join(struct.pack("<3f", *p) for p in positions)
        pos_offset = append_aligned(pos_bytes)
        normal_bytes = b"".join(struct.pack("<3f", *n) for n in normals)
        normal_offset = append_aligned(normal_bytes)
        tangent_bytes = b"".join(struct.pack("<4f", *t) for t in tangents)
        tangent_offset = append_aligned(tangent_bytes)
        uv_bytes = b"".join(struct.pack("<2f", *uv) for uv in uvs)
        uv_offset = append_aligned(uv_bytes)
        index_bytes = b"".join(struct.pack("<3I", *tri) for tri in indices)
        index_offset = append_aligned(index_bytes)

        pos_min = [min(p[axis] for p in positions) for axis in range(3)]
        pos_max = [max(p[axis] for p in positions) for axis in range(3)]

        base_view = len(buffer_views)
        buffer_views.append({"buffer": 0, "byteOffset": pos_offset, "byteLength": len(pos_bytes), "target": 34962})
        buffer_views.append({"buffer": 0, "byteOffset": normal_offset, "byteLength": len(normal_bytes), "target": 34962})
        buffer_views.append({"buffer": 0, "byteOffset": tangent_offset, "byteLength": len(tangent_bytes), "target": 34962})
        buffer_views.append({"buffer": 0, "byteOffset": uv_offset, "byteLength": len(uv_bytes), "target": 34962})
        buffer_views.append({"buffer": 0, "byteOffset": index_offset, "byteLength": len(index_bytes), "target": 34963})

        base_accessor = len(accessors)
        accessors.append({
            "bufferView": base_view, "byteOffset": 0, "componentType": 5126,
            "count": len(positions), "type": "VEC3", "min": pos_min, "max": pos_max,
        })
        accessors.append({
            "bufferView": base_view + 1, "byteOffset": 0, "componentType": 5126,
            "count": len(positions), "type": "VEC3",
        })
        accessors.append({
            "bufferView": base_view + 2, "byteOffset": 0, "componentType": 5126,
            "count": len(positions), "type": "VEC4",
        })
        accessors.append({
            "bufferView": base_view + 3, "byteOffset": 0, "componentType": 5126,
            "count": len(positions), "type": "VEC2",
        })
        accessors.append({
            "bufferView": base_view + 4, "byteOffset": 0, "componentType": 5125,
            "count": len(indices) * 3, "type": "SCALAR",
        })

        mesh_index = len(meshes)
        meshes.append({
            "name": accumulator.name,
            "primitives": [{
                "attributes": {
                    "POSITION": base_accessor,
                    "NORMAL": base_accessor + 1,
                    "TANGENT": base_accessor + 2,
                    "TEXCOORD_0": base_accessor + 3,
                },
                "indices": base_accessor + 4,
                "material": material_index,
            }],
        })
        nodes.append({"name": accumulator.name, "mesh": mesh_index})

        total_vertex_count += len(positions)
        total_triangle_count += len(indices)

    total_length = len(buffer_bytes)
    bin_path = os.path.join(OUT_DIR, BIN_NAME)
    try:
        with open(bin_path, "wb") as bin_file:
            bin_file.write(buffer_bytes)
    except OSError as error:
        print(f"[ERROR] .binの書き込みに失敗しました: {bin_path} ({error})", file=sys.stderr)
        raise

    gltf = {
        "asset": {"version": "2.0", "generator": "KurenaiEngine MontSaintMichelStudy Island generator"},
        "scene": 0,
        "scenes": [{"nodes": list(range(len(nodes)))}],
        "nodes": nodes,
        "meshes": meshes,
        "materials": materials,
        "accessors": accessors,
        "bufferViews": buffer_views,
        "buffers": [{"uri": BIN_NAME, "byteLength": total_length}],
    }

    gltf_path = os.path.join(OUT_DIR, GLTF_NAME)
    try:
        with open(gltf_path, "w", encoding="utf-8") as gltf_file:
            json.dump(gltf, gltf_file, indent=2)
    except OSError as error:
        print(f"[ERROR] .gltfの書き込みに失敗しました: {gltf_path} ({error})", file=sys.stderr)
        raise

    print(f"vertex_count={total_vertex_count} triangle_count={total_triangle_count} buffer_bytes={total_length}")
    print(f"rock: base_r={ROCK_BASE_RADIUS} top_r={ROCK_TOP_RADIUS} y=[{ROCK_BASE_Y},{ROCK_TOP_Y}] sides={ROCK_SIDES}")
    print(f"wall: outer_r={WALL_OUTER_RADIUS} inner_r={WALL_INNER_RADIUS} y=[{WALL_BASE_Y},{WALL_TOP_Y}] sides={WALL_SIDES}")
    print(f"tier1: half_extent={TIER1_HALF_EXTENT} y=[{TIER1_BASE_Y},{TIER1_TOP_Y}]")
    print(f"tier2: half_extent={TIER2_HALF_EXTENT} y=[{TIER2_BASE_Y},{TIER2_TOP_Y}]")
    print(f"spire: half_extent={TIER2_HALF_EXTENT} y=[{SPIRE_BASE_Y},{SPIRE_TIP_Y}]")
    print(f"wrote {gltf_path}")
    print(f"wrote {bin_path}")


if __name__ == "__main__":
    main()
