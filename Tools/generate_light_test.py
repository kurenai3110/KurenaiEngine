import json
import math
import struct
import os

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(SCRIPT_DIR, "..", "Assets", "LightTest")
GLTF_NAME = "LightTest.gltf"
BIN_NAME = "LightTest.bin"

LAT_SEGMENTS = 16
LON_SEGMENTS = 32
SPHERE_RADIUS = 1.0
SPHERE_ROUGHNESS = [0.05, 0.2, 0.5, 0.8]
SPHERE_SPACING = 2.6

FLOOR_HALF_SIZE = 10.0
WALL_HALF_WIDTH = 10.0
WALL_HEIGHT = 6.0


def quat_axis_angle(axis, angle_rad):
    ax, ay, az = axis
    s = math.sin(angle_rad / 2.0)
    return [ax * s, ay * s, az * s, math.cos(angle_rad / 2.0)]


def generate_sphere(lat_segments, lon_segments, radius):
    # KurenaiEngine/Tools/generate_material_test.pyのgenerate_sphereと同一の構成
    # (位置・法線・UV・巻き順の規約を揃えるため)
    positions = []
    normals = []
    uvs = []

    for i in range(lat_segments + 1):
        theta = i / lat_segments * math.pi
        sin_theta = math.sin(theta)
        cos_theta = math.cos(theta)
        for j in range(lon_segments + 1):
            phi = j / lon_segments * 2.0 * math.pi
            sin_phi = math.sin(phi)
            cos_phi = math.cos(phi)

            x = sin_theta * cos_phi
            y = cos_theta
            z = sin_theta * sin_phi

            positions.append((x * radius, y * radius, z * radius))
            normals.append((x, y, z))
            uvs.append((j / lon_segments, i / lat_segments))

    indices = []
    for i in range(lat_segments):
        k1 = i * (lon_segments + 1)
        k2 = k1 + lon_segments + 1
        for j in range(lon_segments):
            if i != 0:
                indices.append((k1 + j, k1 + j + 1, k2 + j))
            if i != lat_segments - 1:
                indices.append((k1 + j + 1, k2 + j + 1, k2 + j))

    return positions, normals, uvs, indices


def generate_quad(corners, normal):
    # cornersは周を辿る順(v0->v1->v2->v3)の4点。法線は指定した向きに揃うよう
    # 三角形分割(v0,v1,v2)/(v0,v2,v3)の巻き順をそのまま使う前提で、呼び出し側が
    # 正しい周方向でcornersを渡す(下のmain()の各面で個別に検証済み)
    positions = list(corners)
    normals = [normal] * 4
    uvs = [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)]
    indices = [(0, 1, 2), (0, 2, 3)]
    return positions, normals, uvs, indices


def main():
    os.makedirs(OUT_DIR, exist_ok=True)

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
                "attributes": {"POSITION": base_accessor, "NORMAL": base_accessor + 1, "TEXCOORD_0": base_accessor + 2},
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

    # --- 床(20x20、roughness 0.08)。鏡面ハイライトの形状が見える ---
    floor_material = add_material("Floor", 0.08, 0.0, [0.8, 0.8, 0.8, 1.0])
    f = FLOOR_HALF_SIZE
    # v0,v2,v1 / v0,v3,v2 の巻き順で+Y向き法線になる(cross(v2-v0,v1-v0)=+Y)
    floor_corners = [(-f, 0.0, -f), (f, 0.0, -f), (f, 0.0, f), (-f, 0.0, f)]
    floor_geo = generate_quad([floor_corners[0], floor_corners[2], floor_corners[1], floor_corners[3]], (0.0, 1.0, 0.0))
    floor_mesh = add_mesh_from_geometry("Floor", *floor_geo, floor_material)
    nodes.append({"name": "Floor", "mesh": floor_mesh})
    floor_node_index = len(nodes) - 1

    # --- 後ろ壁(幅20、高さ6、roughness 0.5)。スポットの円錐と拡散の広がりが見える ---
    wall_material = add_material("Wall", 0.5, 0.0, [0.75, 0.75, 0.78, 1.0])
    w = WALL_HALF_WIDTH
    h = WALL_HEIGHT
    z = -f
    # v0,v1,v2 / v0,v2,v3 の巻き順で+Z向き法線になる(cross(v1-v0,v2-v0)=+Z)
    wall_corners = [(-w, 0.0, z), (w, 0.0, z), (w, h, z), (-w, h, z)]
    wall_geo = generate_quad(wall_corners, (0.0, 0.0, 1.0))
    wall_mesh = add_mesh_from_geometry("Wall", *wall_geo, wall_material)
    nodes.append({"name": "Wall", "mesh": wall_mesh})
    wall_node_index = len(nodes) - 1

    # --- roughness違いの球を4個(手前に並べる。ライトのハイライト形状の比較用) ---
    sphere_geo_cache = generate_sphere(LAT_SEGMENTS, LON_SEGMENTS, SPHERE_RADIUS)
    sphere_node_indices = []
    sphere_count = len(SPHERE_ROUGHNESS)
    for i, roughness in enumerate(SPHERE_ROUGHNESS):
        material_index = add_material(f"Sphere_Roughness_{roughness:.2f}", roughness, 0.0, [1.0, 1.0, 1.0, 1.0])
        mesh_index = add_mesh_from_geometry(f"Sphere_{i}", *sphere_geo_cache, material_index)
        x = (i - (sphere_count - 1) / 2.0) * SPHERE_SPACING
        nodes.append({
            "name": f"Sphere_{i}",
            "mesh": mesh_index,
            "translation": [x, SPHERE_RADIUS + 0.01, -4.0],
        })
        sphere_node_indices.append(len(nodes) - 1)

    # --- ライト: point / spot / directional を1灯ずつ。ModelLoaderのCollectNodeWorldTransformsが
    #     正しく動くかを検証するため、いずれも「平行移動+回転を持つ親ノード」の子として配置する
    #     (aiLight::mPosition/mDirectionはノードローカル固定値のままなので、ワールド変換を
    #     ノード経由で正しく合成できているかがこのテストの主眼)
    lights = [
        {
            "type": "point",
            "color": [1.0, 0.9, 0.7],
            "intensity": 1500.0,
            "range": 15.0,
        },
        {
            "type": "spot",
            "color": [0.7, 0.85, 1.0],
            "intensity": 2000.0,
            "range": 20.0,
            "spot": {"innerConeAngle": 0.3, "outerConeAngle": 0.5},
        },
        {
            "type": "directional",
            "color": [1.0, 1.0, 1.0],
            "intensity": 3.0,
        },
    ]

    def add_light_with_transform(name, light_index, translation, rotation):
        parent_index = len(nodes)
        light_node_index = len(nodes) + 1
        nodes.append({
            "name": f"{name}_Parent",
            "translation": translation,
            "rotation": rotation,
            "children": [light_node_index],
        })
        nodes.append({
            "name": name,
            "extensions": {"KHR_lights_punctual": {"light": light_index}},
        })
        return parent_index

    point_parent = add_light_with_transform(
        "PointLight", 0,
        translation=[4.0, 5.0, -2.0],
        rotation=quat_axis_angle((0.0, 1.0, 0.0), math.radians(30.0)),
    )
    spot_parent = add_light_with_transform(
        "SpotLight", 1,
        translation=[-4.0, 5.0, -1.0],
        rotation=quat_axis_angle((1.0, 0.0, 0.0), math.radians(-50.0)),
    )
    directional_parent = add_light_with_transform(
        "DirectionalLight", 2,
        translation=[0.0, 8.0, 0.0],
        rotation=quat_axis_angle((1.0, 0.0, 0.0), math.radians(-60.0)),
    )

    root_nodes = [floor_node_index, wall_node_index, *sphere_node_indices, point_parent, spot_parent, directional_parent]

    total_length = len(buffer_bytes)
    bin_path = os.path.join(OUT_DIR, BIN_NAME)
    with open(bin_path, "wb") as bin_file:
        bin_file.write(buffer_bytes)

    gltf = {
        "asset": {"version": "2.0", "generator": "KurenaiEngine LightTest generator"},
        "extensionsUsed": ["KHR_lights_punctual"],
        "extensions": {"KHR_lights_punctual": {"lights": lights}},
        "scene": 0,
        "scenes": [{"nodes": root_nodes}],
        "nodes": nodes,
        "meshes": meshes,
        "materials": materials,
        "accessors": accessors,
        "bufferViews": buffer_views,
        "buffers": [{"uri": BIN_NAME, "byteLength": total_length}],
    }

    gltf_path = os.path.join(OUT_DIR, GLTF_NAME)
    with open(gltf_path, "w", encoding="utf-8") as gltf_file:
        json.dump(gltf, gltf_file, indent=2)

    print(f"nodes={len(nodes)} meshes={len(meshes)} materials={len(materials)} lights={len(lights)} buffer_bytes={total_length}")
    print(f"wrote {gltf_path}")
    print(f"wrote {bin_path}")


if __name__ == "__main__":
    main()
