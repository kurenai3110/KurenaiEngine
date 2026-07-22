import json
import math
import struct
import os

LAT_SEGMENTS = 16
LON_SEGMENTS = 32
RADIUS = 1.0
SPHERE_COUNT = 11
SPACING = 2.5

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(SCRIPT_DIR, "..", "Assets", "MaterialTest")
GLTF_NAME = "MaterialTest.gltf"
BIN_NAME = "MaterialTest.bin"


def generate_sphere(lat_segments, lon_segments, radius):
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
            # glTFの標準(右手座標系でCCWが表)に合わせた巻き順。
            # エンジン側でaiProcess_ConvertToLeftHandedにより左手座標系用に変換される
            if i != 0:
                indices.append((k1 + j, k1 + j + 1, k2 + j))
            if i != lat_segments - 1:
                indices.append((k1 + j + 1, k2 + j + 1, k2 + j))

    return positions, normals, uvs, indices


def main():
    os.makedirs(OUT_DIR, exist_ok=True)

    positions, normals, uvs, indices = generate_sphere(LAT_SEGMENTS, LON_SEGMENTS, RADIUS)
    vertex_count = len(positions)
    index_count = len(indices) * 3

    # --- バイナリバッファの構築 ---
    buffer_bytes = bytearray()

    def append_aligned(data: bytes):
        # 4バイト境界に揃える(glTFのアクセッサ要件)
        while len(buffer_bytes) % 4 != 0:
            buffer_bytes.append(0)
        offset = len(buffer_bytes)
        buffer_bytes.extend(data)
        return offset

    pos_bytes = b"".join(struct.pack("<3f", *p) for p in positions)
    pos_offset = append_aligned(pos_bytes)

    normal_bytes = b"".join(struct.pack("<3f", *n) for n in normals)
    normal_offset = append_aligned(normal_bytes)

    uv_bytes = b"".join(struct.pack("<2f", *uv) for uv in uvs)
    uv_offset = append_aligned(uv_bytes)

    index_bytes = b"".join(struct.pack("<3I", *tri) for tri in indices)
    index_offset = append_aligned(index_bytes)

    total_length = len(buffer_bytes)

    # --- アクセッサのmin/max(POSITIONのみ必須) ---
    pos_min = [min(p[axis] for p in positions) for axis in range(3)]
    pos_max = [max(p[axis] for p in positions) for axis in range(3)]

    bin_path = os.path.join(OUT_DIR, BIN_NAME)
    with open(bin_path, "wb") as f:
        f.write(buffer_bytes)

    # --- glTF JSON の構築 ---
    gltf = {
        "asset": {"version": "2.0", "generator": "KurenaiEngine MaterialTest generator"},
        "scene": 0,
        "scenes": [{"nodes": list(range(SPHERE_COUNT))}],
        "nodes": [],
        "meshes": [],
        "materials": [],
        "accessors": [
            {
                "bufferView": 0,
                "byteOffset": 0,
                "componentType": 5126,  # FLOAT
                "count": vertex_count,
                "type": "VEC3",
                "min": pos_min,
                "max": pos_max,
            },
            {
                "bufferView": 1,
                "byteOffset": 0,
                "componentType": 5126,
                "count": vertex_count,
                "type": "VEC3",
            },
            {
                "bufferView": 2,
                "byteOffset": 0,
                "componentType": 5126,
                "count": vertex_count,
                "type": "VEC2",
            },
            {
                "bufferView": 3,
                "byteOffset": 0,
                "componentType": 5125,  # UNSIGNED_INT
                "count": index_count,
                "type": "SCALAR",
            },
        ],
        "bufferViews": [
            {"buffer": 0, "byteOffset": pos_offset, "byteLength": len(pos_bytes), "target": 34962},
            {"buffer": 0, "byteOffset": normal_offset, "byteLength": len(normal_bytes), "target": 34962},
            {"buffer": 0, "byteOffset": uv_offset, "byteLength": len(uv_bytes), "target": 34962},
            {"buffer": 0, "byteOffset": index_offset, "byteLength": len(index_bytes), "target": 34963},
        ],
        "buffers": [{"uri": BIN_NAME, "byteLength": total_length}],
    }

    for i in range(SPHERE_COUNT):
        roughness = i / (SPHERE_COUNT - 1)
        x = (i - (SPHERE_COUNT - 1) / 2.0) * SPACING

        gltf["materials"].append({
            "name": f"Roughness_{roughness:.1f}",
            "pbrMetallicRoughness": {
                "baseColorFactor": [1.0, 1.0, 1.0, 1.0],
                "metallicFactor": 0.0,
                "roughnessFactor": roughness,
            },
        })

        gltf["meshes"].append({
            "name": f"Sphere_{i}",
            "primitives": [{
                "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
                "indices": 3,
                "material": i,
            }],
        })

        gltf["nodes"].append({
            "name": f"Sphere_{i}",
            "mesh": i,
            "translation": [x, 0.0, 0.0],
        })

    gltf_path = os.path.join(OUT_DIR, GLTF_NAME)
    with open(gltf_path, "w", encoding="utf-8") as f:
        json.dump(gltf, f, indent=2)

    print(f"vertex_count={vertex_count} index_count={index_count} buffer_bytes={total_length}")
    print(f"wrote {gltf_path}")
    print(f"wrote {bin_path}")


if __name__ == "__main__":
    main()
