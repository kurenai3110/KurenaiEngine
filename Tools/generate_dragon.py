"""メッシュレット/bindless/レイトレーシングの確認用シーン(ドラゴン)を生成する。

Assets/Source/Dragon/ に置いたStanford Dragonの.ply(dragon_vrip.ply など)を
glTFへ変換する。置かれていない場合は、同程度の三角形数を持つ手続き生成の
代替メッシュ(トーラスノット)へフォールバックする。

【なぜ.plyを直接読ませないのか】KurenaiPackerがビルドするassimpは
glTF/FBX/OBJのインポータしか有効にしていない(README「assimpのビルド」参照)。
PLYインポータを足すとassimpのビルド構成が変わり、既存の手順書が全部ずれる。
このスクリプトが変換を引き受ければ、パッカー側は今までどおりでよい。

【何を確かめるためのシーンなのか】
  ・ドラゴン: 三角形が数十万ある単一メッシュ。メッシュレット分割の効きが最も見える
  ・市松模様のベースカラー: レイトレーシングのヒット面がテクスチャを読めているかは、
    定数色では判別できない。模様が反射に映って初めて確認できる
  ・鏡面の床: ドラゴンを映す相手。ここに映る像でRT反射とメッシュレット色分けを見る

外部依存を増やさないため標準ライブラリだけで完結させている
(generate_material_test.py 等と同じ方針)。
"""

import json
import math
import os
import struct
import sys
import zlib

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(SCRIPT_DIR, "..", "Assets", "Source", "Dragon")
GLTF_NAME = "Dragon.gltf"
BIN_NAME = "Dragon.bin"
TEXTURE_NAME = "DragonChecker.png"

# Stanford 3D Scanning Repository の配布ファイル名。見つかった順に使う
PLY_CANDIDATES = ["dragon_vrip.ply", "dragon_vrip_res2.ply", "dragon.ply"]

# ドラゴンの高さ(メートル)。エンジンの単位はメートルなので、実寸大より少し大きい程度にする
DRAGON_HEIGHT = 2.0
# 床の一辺(メートル)
FLOOR_SIZE = 12.0

# 市松模様テクスチャの一辺(テクセル)と1マスの大きさ
CHECKER_SIZE = 256
CHECKER_CELL = 16
CHECKER_COLOR_A = (232, 232, 232, 255)
CHECKER_COLOR_B = (40, 44, 52, 255)


# --------------------------------------------------------------------------------------
# PLY の読み込み
# --------------------------------------------------------------------------------------

def find_ply(directory):
    """変換元の.plyを探す。候補名 → 任意の.ply の順で探し、無ければNone"""
    for name in PLY_CANDIDATES:
        path = os.path.join(directory, name)
        if os.path.isfile(path):
            return path
    if os.path.isdir(directory):
        for name in sorted(os.listdir(directory)):
            if name.lower().endswith(".ply"):
                return os.path.join(directory, name)
    return None


# PLYのプロパティ型 → structのフォーマット文字とバイト数
_PLY_TYPES = {
    "char": ("b", 1), "int8": ("b", 1),
    "uchar": ("B", 1), "uint8": ("B", 1),
    "short": ("h", 2), "int16": ("h", 2),
    "ushort": ("H", 2), "uint16": ("H", 2),
    "int": ("i", 4), "int32": ("i", 4),
    "uint": ("I", 4), "uint32": ("I", 4),
    "float": ("f", 4), "float32": ("f", 4),
    "double": ("d", 8), "float64": ("d", 8),
}


def load_ply(path):
    """PLY(ascii / binary_little_endian)から頂点位置と三角形を読む。

    Stanford Dragonは頂点にx/y/zとconfidence/intensityを持ち、面は
    「頂点数(uchar) + インデックス(int)」のリスト。4頂点以上の面は
    扇状に三角形化する(このデータには実際には現れないが、他の.plyでも
    黙って壊れないようにしておく)。
    """
    with open(path, "rb") as f:
        # --- ヘッダー ---
        if f.readline().strip() != b"ply":
            raise ValueError("PLYのマジックが一致しません: " + path)

        fmt = None
        elements = []  # [(name, count, [(prop_name, type_or_list)])]
        while True:
            line = f.readline()
            if not line:
                raise ValueError("PLYヘッダーが end_header で終わっていません")
            tokens = line.split()
            if not tokens:
                continue
            key = tokens[0]
            if key == b"format":
                fmt = tokens[1].decode()
            elif key == b"element":
                elements.append((tokens[1].decode(), int(tokens[2]), []))
            elif key == b"property":
                if not elements:
                    raise ValueError("elementより前にpropertyが現れました")
                if tokens[1] == b"list":
                    elements[-1][2].append(
                        (tokens[4].decode(), ("list", tokens[2].decode(), tokens[3].decode())))
                else:
                    elements[-1][2].append((tokens[2].decode(), ("scalar", tokens[1].decode())))
            elif key == b"end_header":
                break

        if fmt not in ("ascii", "binary_little_endian"):
            raise ValueError("未対応のPLYフォーマットです(ascii/binary_little_endianのみ): " + str(fmt))

        positions = []
        triangles = []

        if fmt == "ascii":
            for name, count, props in elements:
                for _ in range(count):
                    tokens = f.readline().split()
                    if name == "vertex":
                        index = {p[0]: i for i, p in enumerate(props)}
                        positions.append((
                            float(tokens[index["x"]]), float(tokens[index["y"]]), float(tokens[index["z"]])))
                    elif name == "face":
                        n = int(tokens[0])
                        idx = [int(t) for t in tokens[1:1 + n]]
                        for k in range(1, n - 1):
                            triangles.append((idx[0], idx[k], idx[k + 1]))
        else:
            for name, count, props in elements:
                # スカラーのみの要素は1レコード分をまとめて読める
                if all(p[1][0] == "scalar" for p in props):
                    layout = "<" + "".join(_PLY_TYPES[p[1][1]][0] for p in props)
                    stride = struct.calcsize(layout)
                    index = {p[0]: i for i, p in enumerate(props)}
                    data = f.read(stride * count)
                    if len(data) != stride * count:
                        raise ValueError("PLYのデータが途中で終わっています: " + name)
                    if name == "vertex":
                        xi, yi, zi = index["x"], index["y"], index["z"]
                        for values in struct.iter_unpack(layout, data):
                            positions.append((values[xi], values[yi], values[zi]))
                    continue

                # リストを含む要素(通常はface)は1件ずつ読む
                for _ in range(count):
                    values = []
                    for _, ptype in props:
                        if ptype[0] == "scalar":
                            code, size = _PLY_TYPES[ptype[1]]
                            values.append(struct.unpack("<" + code, f.read(size))[0])
                        else:
                            count_code, count_size = _PLY_TYPES[ptype[1]]
                            item_code, item_size = _PLY_TYPES[ptype[2]]
                            n = struct.unpack("<" + count_code, f.read(count_size))[0]
                            items = struct.unpack("<" + item_code * n, f.read(item_size * n))
                            values.append(list(items))
                    if name == "face":
                        idx = next(v for v in values if isinstance(v, list))
                        for k in range(1, len(idx) - 1):
                            triangles.append((idx[0], idx[k], idx[k + 1]))

        return positions, triangles


# --------------------------------------------------------------------------------------
# 手続き生成のフォールバック
# --------------------------------------------------------------------------------------

def generate_torus_knot(tube_segments=1024, ring_segments=96, radius=1.0, tube_radius=0.28):
    """.plyが無い場合の代替メッシュ(トーラスノット)。

    ドラゴンの代わりに求められるのは「三角形が十分多く、曲がっていて、
    メッシュレットの分かれ方が目で分かる」ことなので、その条件を満たす
    手続き形状にしている。既定値で約20万三角形。
    """
    positions = []
    p, q = 2, 3

    def curve(t):
        r = radius * (2.0 + math.cos(q * t))
        return (r * math.cos(p * t) * 0.5, r * math.sin(q * t) * 0.5, r * math.sin(p * t) * 0.5)

    for i in range(tube_segments):
        t = i / tube_segments * 2.0 * math.pi
        center = curve(t)
        # 中心曲線の接線を差分で近似し、そこから正規直交基底を作る
        ahead = curve(t + 1e-3)
        tangent = [ahead[k] - center[k] for k in range(3)]
        length = math.sqrt(sum(c * c for c in tangent)) or 1.0
        tangent = [c / length for c in tangent]
        # 接線と平行でない適当なベクトルから法線・従法線を得る
        up = (0.0, 0.0, 1.0) if abs(tangent[2]) < 0.9 else (1.0, 0.0, 0.0)
        normal = [
            tangent[1] * up[2] - tangent[2] * up[1],
            tangent[2] * up[0] - tangent[0] * up[2],
            tangent[0] * up[1] - tangent[1] * up[0],
        ]
        length = math.sqrt(sum(c * c for c in normal)) or 1.0
        normal = [c / length for c in normal]
        binormal = [
            tangent[1] * normal[2] - tangent[2] * normal[1],
            tangent[2] * normal[0] - tangent[0] * normal[2],
            tangent[0] * normal[1] - tangent[1] * normal[0],
        ]

        for j in range(ring_segments):
            a = j / ring_segments * 2.0 * math.pi
            cos_a, sin_a = math.cos(a), math.sin(a)
            positions.append(tuple(
                center[k] + tube_radius * (cos_a * normal[k] + sin_a * binormal[k]) for k in range(3)))

    triangles = []
    for i in range(tube_segments):
        i_next = (i + 1) % tube_segments
        for j in range(ring_segments):
            j_next = (j + 1) % ring_segments
            a = i * ring_segments + j
            b = i * ring_segments + j_next
            c = i_next * ring_segments + j
            d = i_next * ring_segments + j_next
            triangles.append((a, c, b))
            triangles.append((b, c, d))
    return positions, triangles


# --------------------------------------------------------------------------------------
# ジオメトリの後処理
# --------------------------------------------------------------------------------------

def normalize_transform(positions, target_height, lift_to_ground=True):
    """バウンズを見て、指定した高さになるよう原点中心へ正規化する"""
    lo = [min(p[k] for p in positions) for k in range(3)]
    hi = [max(p[k] for p in positions) for k in range(3)]
    height = hi[1] - lo[1]
    scale = (target_height / height) if height > 1e-9 else 1.0
    center = [(lo[k] + hi[k]) * 0.5 for k in range(3)]

    out = []
    for p in positions:
        x = (p[0] - center[0]) * scale
        y = (p[1] - center[1]) * scale
        z = (p[2] - center[2]) * scale
        if lift_to_ground:
            # 足元がy=0に来るよう持ち上げる(床にめり込ませないため)
            y += target_height * 0.5
        out.append((x, y, z))
    return out


def compute_smooth_normals(positions, triangles):
    """面法線を頂点へ面積重みで積んで平均する。

    3Dスキャンの.plyは法線を持たないことが多い。面積重みにしているのは、
    細長い三角形が集まる部分で小さな面の向きが過剰に効くのを避けるため
    (単純平均だとスキャンデータ特有の細長い面で法線が暴れる)。
    """
    normals = [[0.0, 0.0, 0.0] for _ in positions]
    for (i0, i1, i2) in triangles:
        p0, p1, p2 = positions[i0], positions[i1], positions[i2]
        u = (p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2])
        v = (p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2])
        # 外積の長さが三角形の面積の2倍なので、正規化しないまま足せば面積重みになる
        n = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
        for idx in (i0, i1, i2):
            normals[idx][0] += n[0]
            normals[idx][1] += n[1]
            normals[idx][2] += n[2]

    out = []
    for n in normals:
        length = math.sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2])
        if length > 1e-12:
            out.append((n[0] / length, n[1] / length, n[2] / length))
        else:
            # どの三角形からも参照されていない孤立頂点。+Yの既定値にしておく
            out.append((0.0, 1.0, 0.0))
    return out


def compute_spherical_uvs(positions):
    """球面投影のUV。スキャンデータはUVを持たないため機械的に与える。

    経度で継ぎ目ができるが、確認したいのは「テクスチャが引けているか」であって
    展開の質ではないため、これで足りる(継ぎ目に模様のずれが出るのは想定内)。
    """
    uvs = []
    for (x, y, z) in positions:
        length = math.sqrt(x * x + y * y + z * z)
        if length < 1e-12:
            uvs.append((0.0, 0.0))
            continue
        uvs.append((
            0.5 + math.atan2(z, x) / (2.0 * math.pi),
            0.5 - math.asin(max(-1.0, min(1.0, y / length))) / math.pi,
        ))
    return uvs


# --------------------------------------------------------------------------------------
# 出力
# --------------------------------------------------------------------------------------

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

    ply_path = find_ply(out_dir)
    if ply_path:
        print(f"読み込み: {ply_path}")
        positions, triangles = load_ply(ply_path)
        source_name = os.path.basename(ply_path)
    else:
        print(
            f"{out_dir} に.plyが見つからないため、代替メッシュ(トーラスノット)を生成する。\n"
            "  Stanford Dragon を使う場合は dragon_vrip.ply をこのフォルダへ置いて再実行すること\n"
            "  (http://graphics.stanford.edu/data/3Dscanrep/)")
        positions, triangles = generate_torus_knot()
        source_name = "procedural torus knot"

    if not positions or not triangles:
        print("エラー: 頂点または三角形が空", file=sys.stderr)
        return 1

    positions = normalize_transform(positions, DRAGON_HEIGHT)
    normals = compute_smooth_normals(positions, triangles)
    uvs = compute_spherical_uvs(positions)

    # --- 床(鏡面)。ドラゴンを映す相手 ---
    half = FLOOR_SIZE * 0.5
    floor_positions = [(-half, 0.0, -half), (half, 0.0, -half), (half, 0.0, half), (-half, 0.0, half)]
    floor_normals = [(0.0, 1.0, 0.0)] * 4
    floor_uvs = [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)]
    # glTFの標準(右手座標系でCCWが表)。上から見て表になる巻き順
    floor_triangles = [(0, 3, 1), (1, 3, 2)]

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

    dragon_pos = add_attribute(positions, "<3f", "VEC3", 34962, with_bounds=True)
    dragon_nrm = add_attribute(normals, "<3f", "VEC3", 34962)
    dragon_uv = add_attribute(uvs, "<2f", "VEC2", 34962)
    dragon_idx = add_indices(triangles)

    floor_pos = add_attribute(floor_positions, "<3f", "VEC3", 34962, with_bounds=True)
    floor_nrm = add_attribute(floor_normals, "<3f", "VEC3", 34962)
    floor_uv = add_attribute(floor_uvs, "<2f", "VEC2", 34962)
    floor_idx = add_indices(floor_triangles)

    bin_path = os.path.join(out_dir, BIN_NAME)
    with open(bin_path, "wb") as f:
        f.write(buffer_bytes)

    texture_path = os.path.join(out_dir, TEXTURE_NAME)
    write_checker_png(texture_path, CHECKER_SIZE, CHECKER_CELL, CHECKER_COLOR_A, CHECKER_COLOR_B)

    gltf = {
        "asset": {"version": "2.0", "generator": "KurenaiEngine Dragon generator"},
        "scene": 0,
        "scenes": [{"nodes": [0, 1]}],
        "nodes": [
            {"name": "Dragon", "mesh": 0},
            {"name": "MirrorFloor", "mesh": 1},
        ],
        "meshes": [
            {
                "name": "Dragon",
                "primitives": [{
                    "attributes": {"POSITION": dragon_pos, "NORMAL": dragon_nrm, "TEXCOORD_0": dragon_uv},
                    "indices": dragon_idx,
                    "material": 0,
                }],
            },
            {
                "name": "MirrorFloor",
                "primitives": [{
                    "attributes": {"POSITION": floor_pos, "NORMAL": floor_nrm, "TEXCOORD_0": floor_uv},
                    "indices": floor_idx,
                    "material": 1,
                }],
            },
        ],
        "materials": [
            {
                # 市松模様のベースカラー。レイトレーシングのヒット面がテクスチャを
                # 読めているかは定数色では判別できないため、模様のあるものを与える
                "name": "DragonChecker",
                "pbrMetallicRoughness": {
                    "baseColorTexture": {"index": 0},
                    "metallicFactor": 0.0,
                    "roughnessFactor": 0.35,
                },
            },
            {
                # ドラゴンを映す鏡面。テクスチャは持たせない(反射像を濁らせないため)
                "name": "MirrorFloor",
                "pbrMetallicRoughness": {
                    "baseColorFactor": [0.9, 0.9, 0.92, 1.0],
                    "metallicFactor": 1.0,
                    "roughnessFactor": 0.12,
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

    print(f"source={source_name}")
    print(f"dragon: vertices={len(positions)} triangles={len(triangles)}")
    print(f"floor : vertices={len(floor_positions)} triangles={len(floor_triangles)}")
    print(f"buffer_bytes={len(buffer_bytes)}")
    print(f"wrote {gltf_path}")
    print(f"wrote {bin_path}")
    print(f"wrote {texture_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
