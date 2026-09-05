# 半影の幅が「遮蔽物と受光面の距離」に比例することを測るための glTF を生成する。
#
# 【なぜ専用のモデルが要るのか】既存の LightTest.kmodel は球が床に接している。
# 光源を真上に置くと、影の縁を作る遮蔽物(球のシルエット)の高さは影の全周でほぼ一定に
# なるので、**遮蔽物と受光面の距離が変わらず、距離依存を測れない**。
# 実際に LightTest で測ろうとして2度失敗した(1度目は床の距離減衰を拾い、
# 2度目は光源を低くしすぎて半影が測定窓を飽和させた)。
#
# 【このモデルの形】床(30x30)の上に、同じ大きさの球を**高さだけ変えて**浮かべる。
# 球はすべて同じ半径なので、影の大きさの違いも半影の幅の違いも、高さ(= 遮蔽物と床の距離)
# だけで決まる。光源は真上の遠方に置き、影が互いに重ならない間隔で並べる。
#
# 理論: 半影の幅 w = 2R * (受光面までの距離 - 遮蔽物までの距離) / 遮蔽物までの距離
#       光源が高さ H、遮蔽物が高さ h のとき w = 2R * h / (H - h)
#       H >> h なら w はほぼ h に比例する。
#
# 出力は Assets/Source/PenumbraTest/ で、KurenaiPacker で .kmodel へ変換して使う。

import json
import os
import struct

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(SCRIPT_DIR, "..", "Assets", "Source", "PenumbraTest")
GLTF_NAME = "PenumbraTest.gltf"
BIN_NAME = "PenumbraTest.bin"

LAT_SEGMENTS = 24
LON_SEGMENTS = 48
# 遮蔽物の球の半径。**大きくすること** ―― 小さいと影そのものが数十画素しかなく、
# 半影の幅が1画素未満になって分解できない(実測: 半径0.4mでは幅0.6画素だった)
OCCLUDER_RADIUS = 0.6
# 遮蔽物を浮かべる高さ[m]。**これが「遮蔽物と受光面の距離」そのもの**
# 【1回のレンダーにつき遮蔽物1個】複数個を並べると測れない:
#   ・光源を遠くに置くと影が遮蔽物の真後ろに隠れる(真上から見るとどちらも同じ位置)
#   ・光源を近くに置くと軸から外れた遮蔽物の影が x*H/(H-h) 倍の位置へ飛び、床から落ちる
# 軸上(x=0)に1個だけ置き、高さを変えた .kmodel を高さの数だけ作る
OCCLUDER_HEIGHTS = [4.0, 6.0, 7.0, 8.0]
# 影が重ならないよう X 方向に離して並べる。
# 【広げすぎない】影は光源から見て外側へ x*H/(H-h) 倍に広がるので、
# 高い遮蔽物ほど外へ飛ぶ。間隔9m・床±15mでは h=8 の影が 18.4m へ出て床から落ちた
OCCLUDER_SPACING = 0.0  # 軸上に1個だけ置くので未使用
FLOOR_HALF_SIZE = 25.0


def generate_sphere(lat_segments, lon_segments, radius):
    import math
    positions, normals, uvs, indices = [], [], [], []
    for lat in range(lat_segments + 1):
        theta = math.pi * lat / lat_segments
        for lon in range(lon_segments + 1):
            phi = 2.0 * math.pi * lon / lon_segments
            n = (math.sin(theta) * math.cos(phi), math.cos(theta), math.sin(theta) * math.sin(phi))
            positions.append((n[0] * radius, n[1] * radius, n[2] * radius))
            normals.append(n)
            uvs.append((lon / lon_segments, lat / lat_segments))
    for lat in range(lat_segments):
        for lon in range(lon_segments):
            a = lat * (lon_segments + 1) + lon
            b = a + lon_segments + 1
            indices.append((a, b, a + 1))
            indices.append((a + 1, b, b + 1))
    return positions, normals, uvs, indices


def generate_quad(corners, normal):
    # corners は上から見て反時計回り(A,B,C,D)で渡す。
    # 【巻き順に注意】素直に (0,1,2),(0,2,3) と張ると2枚目が裏向きになり、
    # 床が対角線で半分だけ描かれる(既存の generate_light_test.py はこの状態で、
    # 影の測定中に「床の外」と誤認しかけた)。+Y 法線になる向きへ明示的に張る
    positions = list(corners)
    normals = [normal] * 4
    uvs = [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)]
    indices = [(0, 2, 1), (0, 3, 2)]
    return positions, normals, uvs, indices


def main(height, out_name):
    os.makedirs(OUT_DIR, exist_ok=True)
    buffer_bytes = bytearray()

    def append_aligned(data: bytes):
        while len(buffer_bytes) % 4 != 0:
            buffer_bytes.append(0)
        offset = len(buffer_bytes)
        buffer_bytes.extend(data)
        return offset

    accessors, buffer_views, meshes, materials, nodes = [], [], [], [], []

    def add_mesh_from_geometry(name, positions, normals, uvs, indices, material_index):
        pos_bytes = b"".join(struct.pack("<3f", *p) for p in positions)
        pos_offset = append_aligned(pos_bytes)
        normal_bytes = b"".join(struct.pack("<3f", *n) for n in normals)
        normal_offset = append_aligned(normal_bytes)
        uv_bytes = b"".join(struct.pack("<2f", *uv) for uv in uvs)
        uv_offset = append_aligned(uv_bytes)
        index_bytes = b"".join(struct.pack("<3I", *tri) for tri in indices)
        index_offset = append_aligned(index_bytes)

        pos_min = [min(p[axis] for p in positions) for axis in range(3)]
        pos_max = [max(p[axis] for p in positions) for axis in range(3)]

        base_view = len(buffer_views)
        buffer_views.append({"buffer": 0, "byteOffset": pos_offset, "byteLength": len(pos_bytes), "target": 34962})
        buffer_views.append({"buffer": 0, "byteOffset": normal_offset, "byteLength": len(normal_bytes), "target": 34962})
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
            "count": len(positions), "type": "VEC2",
        })
        accessors.append({
            "bufferView": base_view + 3, "byteOffset": 0, "componentType": 5125,
            "count": len(indices) * 3, "type": "SCALAR",
        })

        mesh_index = len(meshes)
        meshes.append({
            "name": name,
            "primitives": [{
                "attributes": {
                    "POSITION": base_accessor, "NORMAL": base_accessor + 1, "TEXCOORD_0": base_accessor + 2
                },
                "indices": base_accessor + 3,
                "material": material_index,
            }],
        })
        return mesh_index

    def add_material(name, roughness, metallic, base_color):
        index = len(materials)
        materials.append({
            "name": name,
            "pbrMetallicRoughness": {
                "baseColorFactor": base_color,
                "metallicFactor": metallic,
                "roughnessFactor": roughness,
            },
        })
        return index

    # --- 床。粗くして鏡面ハイライトを消す(半影の測定に鏡面は邪魔) ---
    floor_material = add_material("Floor", 0.9, 0.0, [0.8, 0.8, 0.8, 1.0])
    f = FLOOR_HALF_SIZE
    floor_corners = [(-f, 0.0, -f), (f, 0.0, -f), (f, 0.0, f), (-f, 0.0, f)]
    floor_geo = generate_quad(floor_corners, (0.0, 1.0, 0.0))
    floor_mesh = add_mesh_from_geometry("Floor", *floor_geo, floor_material)
    nodes.append({"name": "Floor", "mesh": floor_mesh})
    root_nodes = [len(nodes) - 1]

    # --- 高さだけを変えた同じ球。これが遮蔽物 ---
    occluder_material = add_material("Occluder", 0.9, 0.0, [0.6, 0.6, 0.6, 1.0])
    sphere_geo = generate_sphere(LAT_SEGMENTS, LON_SEGMENTS, OCCLUDER_RADIUS)
    mesh_index = add_mesh_from_geometry(f"Occluder_h{height}", *sphere_geo, occluder_material)
    nodes.append({"name": f"Occluder_h{height}", "mesh": mesh_index, "translation": [0.0, height, 0.0]})
    root_nodes.append(len(nodes) - 1)

    total_length = len(buffer_bytes)
    bin_path = os.path.join(OUT_DIR, out_name + ".bin")
    with open(bin_path, "wb") as bin_file:
        bin_file.write(buffer_bytes)

    gltf = {
        "asset": {"version": "2.0", "generator": "KurenaiEngine PenumbraTest generator"},
        "scene": 0,
        "scenes": [{"nodes": root_nodes}],
        "nodes": nodes,
        "meshes": meshes,
        "materials": materials,
        "accessors": accessors,
        "bufferViews": buffer_views,
        "buffers": [{"uri": out_name + ".bin", "byteLength": total_length}],
    }
    gltf_path = os.path.join(OUT_DIR, out_name + ".gltf")
    with open(gltf_path, "w", encoding="utf-8") as gltf_file:
        json.dump(gltf, gltf_file, indent=2)

    print(f"occluder radius={OCCLUDER_RADIUS}, height={height}")
    print(f"wrote {gltf_path}")
    print(f"wrote {bin_path}")


if __name__ == "__main__":
    for h in OCCLUDER_HEIGHTS:
        main(h, "PenumbraH%d" % int(h))
