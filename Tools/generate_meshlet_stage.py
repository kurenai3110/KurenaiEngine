"""メッシュレット/bindless/レイトレーシングの確認用ステージ(鏡面の床と市松模様の背景壁)を生成する。

確認の主役はドラゴンそのもの(Assets/Source/ChineseDragon/dragon.obj、87万三角形)だが、
それだけでは確かめられないものが2つあるため、この2枚を足す。

  ・鏡面の床
      メッシュレットの色分けが「ラスタ描画」と「レイトレーシング」で一致するかを見るには、
      同じドラゴンを映す相手が要る。床が映した像の色分けが直接描画と一致すれば、
      2つの経路が同一のジオメトリ(同じインデックス並び・同じメッシュレット境界)を
      見ていることになる。

  ・市松模様の背景壁
      レイトレーシングのヒット面がbindlessでマテリアルのテクスチャを引けているかは、
      模様のある面が映って初めて判別できる。dragon.objはテクスチャを持たない3Dスキャンで、
      定数色のままでは「テクスチャが引けていない」のか「もともと単色」なのかが区別できない。
      床に映った壁に市松模様が出ればbindlessが効いている。

出力: Assets/Source/MeshletStage/ に .gltf / .bin / .png
"""

import json
import os
import struct
import sys
import zlib

OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "Assets", "Source", "MeshletStage")
GLTF_NAME = "MeshletStage.gltf"
BIN_NAME = "MeshletStage.bin"
TEXTURE_NAME = "Checker.png"

# 床の一辺(メートル)。ドラゴンは約1mなので、反射像が切れない程度に広く取る
FLOOR_SIZE = 12.0

# 背景壁。ドラゴンの背後(+Z側)に立てる。カメラは-Z側から見る
WALL_WIDTH = 8.0
WALL_HEIGHT = 4.0
WALL_Z = 2.5

# 市松模様の一辺(テクセル)と1マスの大きさ。
# 床に映ったときにマスが潰れないよう、マスは大きめに取る
CHECKER_SIZE = 512
CHECKER_CELL = 32
CHECKER_COLOR_A = (235, 235, 240, 255)
CHECKER_COLOR_B = (30, 55, 105, 255)


def write_checker_png(path, size, cell, color_a, color_b):
    """市松模様のRGBA PNGを標準ライブラリだけで書き出す
    (generate_material_test.pyのwrite_solid_rgba_pngと同じ最小構成)。
    """
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    raw = bytearray()
    for y in range(size):
        raw.append(0)  # フィルタタイプ: None
        for x in range(size):
            color = color_a if ((x // cell) + (y // cell)) % 2 == 0 else color_b
            raw.extend(color)

    header = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)  # 8bit RGBA
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", header)
           + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
           + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)


def main():
    out_dir = os.path.abspath(OUT_DIR)
    os.makedirs(out_dir, exist_ok=True)

    # --- 床(鏡面)。ドラゴンと壁を映す相手 ---
    half = FLOOR_SIZE * 0.5
    floor_positions = [(-half, 0.0, -half), (half, 0.0, -half), (half, 0.0, half), (-half, 0.0, half)]
    floor_normals = [(0.0, 1.0, 0.0)] * 4
    floor_uvs = [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)]
    # glTFの標準(右手座標系でCCWが表)。上から見て表になる巻き順
    floor_triangles = [(0, 3, 1), (1, 3, 2)]

    # --- 背景壁(市松模様)。法線は-Z向き(カメラ側を向く) ---
    wx = WALL_WIDTH * 0.5
    wall_positions = [
        (-wx, 0.0, WALL_Z),
        (wx, 0.0, WALL_Z),
        (wx, WALL_HEIGHT, WALL_Z),
        (-wx, WALL_HEIGHT, WALL_Z),
    ]
    wall_normals = [(0.0, 0.0, -1.0)] * 4
    # マスが引き伸ばされないよう、UVは壁の縦横比に合わせて繰り返す
    ru = WALL_WIDTH / 2.0
    rv = WALL_HEIGHT / 2.0
    wall_uvs = [(0.0, rv), (ru, rv), (ru, 0.0), (0.0, 0.0)]
    wall_triangles = [(0, 1, 2), (0, 2, 3)]

    # --- バイナリバッファ ---
    buffer_bytes = bytearray()

    def append_aligned(data):
        while len(buffer_bytes) % 4 != 0:
            buffer_bytes.append(0)
        offset = len(buffer_bytes)
        buffer_bytes.extend(data)
        return offset, len(data)

    views = []
    accessors = []

    def add_attribute(values, fmt, gltf_type, target, with_bounds=False):
        data = b"".join(struct.pack(fmt, *v) for v in values)
        offset, length = append_aligned(data)
        views.append({"buffer": 0, "byteOffset": offset, "byteLength": length, "target": target})
        accessor = {
            "bufferView": len(views) - 1,
            "byteOffset": 0,
            "componentType": 5126,  # FLOAT
            "count": len(values),
            "type": gltf_type,
        }
        if with_bounds:
            components = len(values[0])
            accessor["min"] = [min(v[k] for v in values) for k in range(components)]
            accessor["max"] = [max(v[k] for v in values) for k in range(components)]
        accessors.append(accessor)
        return len(accessors) - 1

    def add_indices(tris):
        data = b"".join(struct.pack("<3I", *t) for t in tris)
        offset, length = append_aligned(data)
        views.append({"buffer": 0, "byteOffset": offset, "byteLength": length, "target": 34963})
        accessors.append({
            "bufferView": len(views) - 1,
            "byteOffset": 0,
            "componentType": 5125,  # UNSIGNED_INT
            "count": len(tris) * 3,
            "type": "SCALAR",
        })
        return len(accessors) - 1

    floor_pos = add_attribute(floor_positions, "<3f", "VEC3", 34962, with_bounds=True)
    floor_nrm = add_attribute(floor_normals, "<3f", "VEC3", 34962)
    floor_uv = add_attribute(floor_uvs, "<2f", "VEC2", 34962)
    floor_idx = add_indices(floor_triangles)

    wall_pos = add_attribute(wall_positions, "<3f", "VEC3", 34962, with_bounds=True)
    wall_nrm = add_attribute(wall_normals, "<3f", "VEC3", 34962)
    wall_uv = add_attribute(wall_uvs, "<2f", "VEC2", 34962)
    wall_idx = add_indices(wall_triangles)

    bin_path = os.path.join(out_dir, BIN_NAME)
    with open(bin_path, "wb") as f:
        f.write(buffer_bytes)

    texture_path = os.path.join(out_dir, TEXTURE_NAME)
    write_checker_png(texture_path, CHECKER_SIZE, CHECKER_CELL, CHECKER_COLOR_A, CHECKER_COLOR_B)

    gltf = {
        "asset": {"version": "2.0", "generator": "KurenaiEngine meshlet stage generator"},
        "scene": 0,
        "scenes": [{"nodes": [0, 1]}],
        "nodes": [
            {"name": "MirrorFloor", "mesh": 0},
            {"name": "CheckerBackdrop", "mesh": 1},
        ],
        "meshes": [
            {
                "name": "MirrorFloor",
                "primitives": [{
                    "attributes": {"POSITION": floor_pos, "NORMAL": floor_nrm, "TEXCOORD_0": floor_uv},
                    "indices": floor_idx,
                    "material": 0,
                }],
            },
            {
                "name": "CheckerBackdrop",
                "primitives": [{
                    "attributes": {"POSITION": wall_pos, "NORMAL": wall_nrm, "TEXCOORD_0": wall_uv},
                    "indices": wall_idx,
                    "material": 1,
                }],
            },
        ],
        "materials": [
            {
                # ドラゴンと壁を映す鏡面。テクスチャは持たせない(反射像を濁らせないため)
                "name": "MirrorFloor",
                "pbrMetallicRoughness": {
                    "baseColorFactor": [0.9, 0.9, 0.92, 1.0],
                    "metallicFactor": 1.0,
                    "roughnessFactor": 0.12,
                },
            },
            {
                # レイトレーシングのヒット面がテクスチャを読めているかを見るための模様。
                # 金属にすると映り込みが模様を覆ってしまうので、粗い誘電体にする
                "name": "CheckerBackdrop",
                "pbrMetallicRoughness": {
                    "baseColorTexture": {"index": 0},
                    "metallicFactor": 0.0,
                    "roughnessFactor": 0.8,
                },
            },
        ],
        "images": [{"uri": TEXTURE_NAME}],
        "textures": [{"source": 0}],
        "accessors": accessors,
        "bufferViews": views,
        "buffers": [{"uri": BIN_NAME, "byteLength": len(buffer_bytes)}],
    }

    gltf_path = os.path.join(out_dir, GLTF_NAME)
    with open(gltf_path, "w", encoding="utf-8") as f:
        json.dump(gltf, f, indent=2)

    print(f"floor: {FLOOR_SIZE}m square, mirror (metallic=1.0 roughness=0.12)")
    print(f"wall : {WALL_WIDTH}x{WALL_HEIGHT}m at z={WALL_Z}, checker {CHECKER_SIZE}px/cell {CHECKER_CELL}")
    print(f"buffer_bytes={len(buffer_bytes)}")
    print(f"wrote {gltf_path}")
    print(f"wrote {bin_path}")
    print(f"wrote {texture_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
