import json
import math
import struct
import os
import zlib

LAT_SEGMENTS = 16
LON_SEGMENTS = 32
RADIUS = 1.0
SPHERE_COUNT = 11
SPACING = 2.5

# 半透明描画(alphaMode=BLEND、docs/Architecture.htmlの15章)の検証用に、
# 白色球体列の右端(+X側)へ半透明の赤いガラス球を1つ追加する。
# ベースカラーテクスチャのアルファで半透明になるケースを再現したいので、
# 単色RGBAのPNGをこのスクリプトが生成して参照する
# (baseColorFactorのアルファだけで半透明にするケースはBistroのガラスで検証済みのため、
#  こちらはテクスチャのアルファ経路を通す構成にしている)
GLASS_TEXTURE_NAME = "GlassRed.png"
GLASS_TEXTURE_SIZE = 4
GLASS_TEXTURE_RGBA = (220, 40, 40, 128)
GLASS_ROUGHNESS = 0.1

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(SCRIPT_DIR, "..", "Assets", "Source", "MaterialTest")
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


def write_solid_rgba_png(path, size, rgba):
    """単色のRGBA PNGを書き出す。

    Pillow等の外部依存を増やさないため、標準ライブラリ(zlib/struct)だけで
    最小構成のPNG(8bit/RGBA、フィルタなし、非インターレース)を組み立てている。
    このスクリプトの他の処理も標準ライブラリのみで完結しているため方針を揃えた。
    """

    def chunk(kind: bytes, data: bytes) -> bytes:
        # PNGのチャンクは [長さ4][種別4][データ][CRC32 4] で、CRCは種別+データに対して取る
        return (
            struct.pack(">I", len(data))
            + kind
            + data
            + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
        )

    # IHDR: 幅・高さ・ビット深度8・カラータイプ6(トゥルーカラー+アルファ)・
    # 圧縮方式0・フィルタ方式0・インターレースなし
    ihdr = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)

    # 各スキャンラインの先頭にフィルタタイプ(0=None)を1バイト置くのがPNGの規定
    row = bytes(rgba) * size
    raw = b"".join(b"\x00" + row for _ in range(size))

    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", ihdr)
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )

    with open(path, "wb") as f:
        f.write(png)


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
    # 白色球体列(SPHERE_COUNT個)+ 半透明ガラス球(1個)
    node_count = SPHERE_COUNT + 1

    gltf = {
        "asset": {"version": "2.0", "generator": "KurenaiEngine MaterialTest generator"},
        "scene": 0,
        "scenes": [{"nodes": list(range(node_count))}],
        "nodes": [],
        "meshes": [],
        "materials": [],
        # ガラス球のベースカラーテクスチャ(このスクリプトが生成する単色RGBA PNG)
        "images": [{"uri": GLASS_TEXTURE_NAME}],
        "textures": [{"source": 0}],
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

    # --- 半透明ガラス球(白色球体列の右端の1つ先へ置く) ---
    glass_index = SPHERE_COUNT
    glass_x = (glass_index - (SPHERE_COUNT - 1) / 2.0) * SPACING

    glass_texture_path = os.path.join(OUT_DIR, GLASS_TEXTURE_NAME)
    write_solid_rgba_png(glass_texture_path, GLASS_TEXTURE_SIZE, GLASS_TEXTURE_RGBA)

    gltf["materials"].append({
        "name": "GlassRed_BLEND",
        "pbrMetallicRoughness": {
            # baseColorFactorは指定しない(既定[1,1,1,1])。アルファはテクスチャ側が持つ
            "baseColorTexture": {"index": 0},
            "metallicFactor": 0.0,
            "roughnessFactor": GLASS_ROUGHNESS,
        },
        # KurenaiPackerがこれを見てMeshEntryのFlags bit0(半透明)を立て、
        # ランタイムはG-Bufferではなく専用のフォワードパスで描画する(15章)
        "alphaMode": "BLEND",
    })

    gltf["meshes"].append({
        "name": "Sphere_Transparent",
        "primitives": [{
            "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
            "indices": 3,
            "material": glass_index,
        }],
    })

    gltf["nodes"].append({
        "name": "Sphere_Transparent",
        "mesh": glass_index,
        "translation": [glass_x, 0.0, 0.0],
    })

    gltf_path = os.path.join(OUT_DIR, GLTF_NAME)
    with open(gltf_path, "w", encoding="utf-8") as f:
        json.dump(gltf, f, indent=2)

    print(f"vertex_count={vertex_count} index_count={index_count} buffer_bytes={total_length}")
    print(f"spheres={SPHERE_COUNT} (roughness 0.0-1.0) + 1 transparent (alphaMode=BLEND)")
    print(f"wrote {gltf_path}")
    print(f"wrote {bin_path}")
    print(f"wrote {glass_texture_path}")


if __name__ == "__main__":
    main()
