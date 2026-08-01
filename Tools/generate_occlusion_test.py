import json
import math
import struct
import os
import zlib

# マテリアルの遮蔽マップ(ベイク済みAO。glTFのocclusionTexture)の検証用シーンを生成する。
# docs/Architecture.htmlの22章に対応。
#
# 同梱のアセット(Sponza/Bistro等)はいずれも遮蔽マップを持たないため、効きを目視で確認できる
# 専用のシーンが必要になる。generate_material_test.pyの球体列を土台にして、
# 「縦縞のAOテクスチャ」を occlusionTexture.strength を変えながら割り当てる構成にした。
# 縞にしているのは、遮蔽が効いた/効いていないを一目で判別でき、かつ強度の中間値も
# 縞のコントラストとして読み取れるため。
#
# 球の並び(-X側から):
#   0-2  : 遮蔽マップなし(対照群)
#   3-8  : strength = 1.0 / 0.8 / 0.6 / 0.4 / 0.2 / 0.0
#   9-10 : 遮蔽マップなし(対照群)
#   11   : 半透明ガラス球(alphaMode=BLEND、strength=1.0)。半透明フォワードパスでの効きを見る
#
# 期待される見え方(22.5節):
#   - strengthが小さいほど縞が薄くなり、0.0では対照群と区別が付かなくなる
#   - 縞は太陽の直接光が当たる球の上部にはほとんど出ず、環境光が支配的な下部で強く出る
#     (遮蔽マップは間接光にのみ効かせる方針のため)

LAT_SEGMENTS = 16
LON_SEGMENTS = 32
RADIUS = 1.0
SPHERE_COUNT = 11
SPACING = 2.5

# 遮蔽マップ。赤チャンネルが遮蔽率(0=完全遮蔽、1=遮蔽なし)。
# BC7圧縮のブロック(4x4)より十分太い縞になるよう、1本あたり32pxにしてある
OCCLUSION_TEXTURE_NAME = "Occlusion.png"
OCCLUSION_TEXTURE_SIZE = 256
OCCLUSION_STRIPE_COUNT = 8

# 球ごとの occlusionTexture.strength。None は occlusionTexture を付けない(対照群)
OCCLUSION_STRENGTHS = [None, None, None, 1.0, 0.8, 0.6, 0.4, 0.2, 0.0, None, None]

GLASS_TEXTURE_NAME = "GlassRed.png"
GLASS_TEXTURE_SIZE = 4
GLASS_TEXTURE_RGBA = (220, 40, 40, 128)
GLASS_ROUGHNESS = 0.1
GLASS_OCCLUSION_STRENGTH = 1.0

# 遮蔽の効きが粗さで埋もれないよう、球体列は中程度の粗さで揃える
# (MaterialTestのように粗さを振ると、遮蔽の差か粗さの差かが判別しづらい)
SPHERE_ROUGHNESS = 0.5

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(SCRIPT_DIR, "..", "Assets", "Source", "OcclusionTest")
GLTF_NAME = "OcclusionTest.gltf"
BIN_NAME = "OcclusionTest.bin"


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


def _png_chunk(kind: bytes, data: bytes) -> bytes:
    # PNGのチャンクは [長さ4][種別4][データ][CRC32 4] で、CRCは種別+データに対して取る
    return (
        struct.pack(">I", len(data))
        + kind
        + data
        + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    )


def _write_png(path: str, size: int, color_type: int, rows: list):
    """8bitのPNG(フィルタなし・非インターレース)を書き出す。

    generate_material_test.pyと同じく、Pillow等の外部依存を増やさないため
    標準ライブラリ(zlib/struct)だけで組み立てている。
    color_type: 0=グレースケール、6=トゥルーカラー+アルファ
    """
    ihdr = struct.pack(">IIBBBBB", size, size, 8, color_type, 0, 0, 0)
    # 各スキャンラインの先頭にフィルタタイプ(0=None)を1バイト置くのがPNGの規定
    raw = b"".join(b"\x00" + row for row in rows)

    png = (
        b"\x89PNG\r\n\x1a\n"
        + _png_chunk(b"IHDR", ihdr)
        + _png_chunk(b"IDAT", zlib.compress(raw, 9))
        + _png_chunk(b"IEND", b"")
    )

    with open(path, "wb") as f:
        f.write(png)


def write_solid_rgba_png(path, size, rgba):
    _write_png(path, size, 6, [bytes(rgba) * size for _ in range(size)])


def write_stripe_occlusion_png(path, size, stripe_count):
    """縦縞のグレースケールPNG(遮蔽マップ)を書き出す。

    シェーダーは赤チャンネルだけを読む。グレースケールPNGを展開したときR=G=Bになるため、
    1チャンネルで意図どおりに機能する。
    """
    stripe_width = size // stripe_count
    row = bytes(0 if (x // stripe_width) % 2 == 0 else 255 for x in range(size))
    _write_png(path, size, 0, [row for _ in range(size)])


def main():
    os.makedirs(OUT_DIR, exist_ok=True)

    if len(OCCLUSION_STRENGTHS) != SPHERE_COUNT:
        raise ValueError(
            f"OCCLUSION_STRENGTHSの要素数({len(OCCLUSION_STRENGTHS)})を"
            f"SPHERE_COUNT({SPHERE_COUNT})と一致させてください"
        )

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

    # --- テクスチャの生成 ---
    occlusion_texture_path = os.path.join(OUT_DIR, OCCLUSION_TEXTURE_NAME)
    write_stripe_occlusion_png(occlusion_texture_path, OCCLUSION_TEXTURE_SIZE, OCCLUSION_STRIPE_COUNT)

    glass_texture_path = os.path.join(OUT_DIR, GLASS_TEXTURE_NAME)
    write_solid_rgba_png(glass_texture_path, GLASS_TEXTURE_SIZE, GLASS_TEXTURE_RGBA)

    # --- glTF JSON の構築 ---
    node_count = SPHERE_COUNT + 1
    occlusion_texture_index = 0
    glass_texture_index = 1

    gltf = {
        "asset": {"version": "2.0", "generator": "KurenaiEngine OcclusionTest generator"},
        "scene": 0,
        "scenes": [{"nodes": list(range(node_count))}],
        "nodes": [],
        "meshes": [],
        "materials": [],
        "images": [{"uri": OCCLUSION_TEXTURE_NAME}, {"uri": GLASS_TEXTURE_NAME}],
        "textures": [{"source": 0}, {"source": 1}],
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
        strength = OCCLUSION_STRENGTHS[i]
        x = (i - (SPHERE_COUNT - 1) / 2.0) * SPACING

        material = {
            "name": f"Occlusion_{'none' if strength is None else f'{strength:.1f}'}",
            "pbrMetallicRoughness": {
                "baseColorFactor": [1.0, 1.0, 1.0, 1.0],
                "metallicFactor": 0.0,
                "roughnessFactor": SPHERE_ROUGHNESS,
            },
        }
        if strength is not None:
            # strengthは既定値1.0のときも明示して書く(パッカーの読み取り経路を必ず通すため)
            material["occlusionTexture"] = {"index": occlusion_texture_index, "strength": strength}

        gltf["materials"].append(material)

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

    # --- 半透明ガラス球(球体列の右端の1つ先へ置く) ---
    # 半透明フォワードパスはSSAO/SSILを持たないが、遮蔽マップはテクスチャなので効く(22.3節)
    glass_index = SPHERE_COUNT
    glass_x = (glass_index - (SPHERE_COUNT - 1) / 2.0) * SPACING

    gltf["materials"].append({
        "name": "GlassRed_BLEND",
        "pbrMetallicRoughness": {
            "baseColorTexture": {"index": glass_texture_index},
            "metallicFactor": 0.0,
            "roughnessFactor": GLASS_ROUGHNESS,
        },
        "occlusionTexture": {"index": occlusion_texture_index, "strength": GLASS_OCCLUSION_STRENGTH},
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

    strengths = ", ".join("none" if s is None else f"{s:.1f}" for s in OCCLUSION_STRENGTHS)
    print(f"vertex_count={vertex_count} index_count={index_count} buffer_bytes={total_length}")
    print(f"spheres={SPHERE_COUNT} (occlusionTexture.strength: {strengths}) + 1 transparent (strength={GLASS_OCCLUSION_STRENGTH})")
    print(f"wrote {gltf_path}")
    print(f"wrote {bin_path}")
    print(f"wrote {occlusion_texture_path}")
    print(f"wrote {glass_texture_path}")


if __name__ == "__main__":
    main()
