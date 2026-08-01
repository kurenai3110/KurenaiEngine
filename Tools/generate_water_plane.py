"""モン・サン=ミシェル検証シーン(P1)用の、干潟を覆う水面板を生成する。

実写のモン・サン=ミシェルは、大潮の干満差が大きく、満潮時には島の周囲が水面に覆われて
干潟の鏡面反射に城が映り込む構図になる(このシーンが再現したい写真の構図そのもの)。
このスクリプトはその「水面」だけを担当する、ほぼ平坦なXZ平面のメッシュを吐く。

なぜ64x64セグメントに分割するか(平面1枚にしない理由):
  (a) 今回のフェーズでは頂点変位による波は行わないが、将来VS側で頂点をY方向に動かして
      波を付ける拡張の余地を残すため、あらかじめ細かい格子にしておく
  (b) 遠方では法線マップのディテールを距離に応じて平坦化(フェードアウト)したくなるが、
      それをピクセル単位ではなく頂点補間で軽量に行う場合、頂点密度が要る
  (c) 4000m四方を巨大三角形2枚で覆うと、カメラ近傍で深度値の変化に対して画面空間の
      デプス精度が粗くなり、Zファイティングや補間誤差が出やすい。格子を細かくして
      1三角形が受け持つ深度レンジを狭めておく

法線は全頂点(0,1,0)固定(このフェーズでは頂点変位を行わないため水面は数学的に平坦)。
TANGENTは(1,0,0,1)固定(平面全体でX軸に一致するため)。
UVはワールド20mあたり1タイルになるよう単純にワールド座標を20で割った値を敷き詰める。
法線マップ(generate_water_normal.pyが生成するWaterNormal.png)をタイリングして使う想定のため。
TEXCOORD_1はTEXCOORD_0と同値にしておく(他の2スクリプトと同じ理由: ライトマップUV等
将来の用途にTEXCOORD_0とは独立に使えるチャンネルを残しておくため)。

配置(Translation)はこのスクリプトでは行わない。原点(0,0,0)を中心に4000m四方の板を作るだけで、
実際の高さ・XZオフセットはScenes/MontSaintMichel.ksceneの[Model]Translationで与える。

生成物: Assets/Source/MontSaintMichelStudy/Water.gltf + Water.bin
KurenaiPacker.exeで Assets/Packed/MontSaintMichelStudy/Water.kmodel へ変換して使う
(--bake-occlusionは付けない。平面にAOを焼いても意味がなく、xatlasのUV展開が時間の無駄になるため)。
"""

import json
import math
import os
import struct
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(SCRIPT_DIR, "..", "Assets", "Source", "MontSaintMichelStudy")
GLTF_NAME = "Water.gltf"
BIN_NAME = "Water.bin"

# --- 水面板の寸法 ---
# 4000m四方。カメラ(Scenes/MontSaintMichel.kscene側でz=-600に置く想定)から島(z=0付近)まで
# 見渡せて、かつ地平線近くまで水面が続いて見える広さとして選んだ
WATER_SIZE = 4000.0
WATER_SEGMENTS = 64

# --- UVタイリング ---
# 「ワールド20mあたり1タイル」。法線マップの繰り返し単位を波の見た目として自然な大きさに
# するための値で、generate_water_normal.pyが吐くタイル可能な法線マップと組み合わせて使う
WATER_UV_METERS_PER_TILE = 20.0

# --- マテリアル ---
# 静かな水面を想定した低ラフネス・非金属。ベースカラーは干潟の濁った海水を想定した
# 暗い青緑(ほぼ黒に近い値。反射がほとんどを占めるため拡散色自体は目立たなくてよい)
WATER_BASE_COLOR = [0.02, 0.03, 0.04, 1.0]
WATER_METALLIC = 0.0
WATER_ROUGHNESS = 0.03


def build_grid(size, segments, uv_meters_per_tile):
    """XZ平面の格子を生成する。原点(0,0,0)が中心。

    戻り値: positions, normals, tangents, uv0, uv1, indices(三角形のタプルのリスト)
    """
    half = size * 0.5
    step = size / segments
    verts_per_row = segments + 1

    positions = []
    normals = []
    tangents = []
    uv0 = []
    uv1 = []

    for j in range(verts_per_row):
        z = -half + j * step
        for i in range(verts_per_row):
            x = -half + i * step
            positions.append((x, 0.0, z))
            normals.append((0.0, 1.0, 0.0))
            tangents.append((1.0, 0.0, 0.0, 1.0))
            uv = (x / uv_meters_per_tile, z / uv_meters_per_tile)
            uv0.append(uv)
            uv1.append(uv)

    def index_of(i, j):
        return j * verts_per_row + i

    indices = []
    for j in range(segments):
        for i in range(segments):
            a = index_of(i, j)
            b = index_of(i + 1, j)
            c = index_of(i + 1, j + 1)
            d = index_of(i, j + 1)
            # 巻き順は「cross(edge1, edge2)が+Yを向く」ことを解析的に確認済み
            # (a→d→bの外積が+Y、d→c→bの外積も+Y。glTFは右手系でCCWが表)
            indices.append((a, d, b))
            indices.append((d, c, b))

    return positions, normals, tangents, uv0, uv1, indices


def main():
    try:
        os.makedirs(OUT_DIR, exist_ok=True)
    except OSError as error:
        print(f"[ERROR] 出力ディレクトリの作成に失敗しました: {OUT_DIR} ({error})", file=sys.stderr)
        raise

    positions, normals, tangents, uv0, uv1, indices = build_grid(
        WATER_SIZE, WATER_SEGMENTS, WATER_UV_METERS_PER_TILE)

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

    tangent_bytes = b"".join(struct.pack("<4f", *t) for t in tangents)
    tangent_offset = append_aligned(tangent_bytes)

    uv0_bytes = b"".join(struct.pack("<2f", *uv) for uv in uv0)
    uv0_offset = append_aligned(uv0_bytes)

    uv1_bytes = b"".join(struct.pack("<2f", *uv) for uv in uv1)
    uv1_offset = append_aligned(uv1_bytes)

    index_bytes = b"".join(struct.pack("<3I", *tri) for tri in indices)
    index_offset = append_aligned(index_bytes)

    total_length = len(buffer_bytes)

    pos_min = [min(p[axis] for p in positions) for axis in range(3)]
    pos_max = [max(p[axis] for p in positions) for axis in range(3)]

    bin_path = os.path.join(OUT_DIR, BIN_NAME)
    try:
        with open(bin_path, "wb") as bin_file:
            bin_file.write(buffer_bytes)
    except OSError as error:
        print(f"[ERROR] .binの書き込みに失敗しました: {bin_path} ({error})", file=sys.stderr)
        raise

    vertex_count = len(positions)
    index_count = len(indices) * 3

    gltf = {
        "asset": {"version": "2.0", "generator": "KurenaiEngine MontSaintMichelStudy Water generator"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "Water", "mesh": 0}],
        "meshes": [{
            "name": "Water",
            "primitives": [{
                "attributes": {
                    "POSITION": 0,
                    "NORMAL": 1,
                    "TANGENT": 2,
                    "TEXCOORD_0": 3,
                    "TEXCOORD_1": 4,
                },
                "indices": 5,
                "material": 0,
            }],
        }],
        "materials": [{
            "name": "Water",
            "pbrMetallicRoughness": {
                "baseColorFactor": WATER_BASE_COLOR,
                "metallicFactor": WATER_METALLIC,
                "roughnessFactor": WATER_ROUGHNESS,
            },
        }],
        "accessors": [
            {
                "bufferView": 0, "byteOffset": 0, "componentType": 5126,
                "count": vertex_count, "type": "VEC3", "min": pos_min, "max": pos_max,
            },
            {
                "bufferView": 1, "byteOffset": 0, "componentType": 5126,
                "count": vertex_count, "type": "VEC3",
            },
            {
                "bufferView": 2, "byteOffset": 0, "componentType": 5126,
                "count": vertex_count, "type": "VEC4",
            },
            {
                "bufferView": 3, "byteOffset": 0, "componentType": 5126,
                "count": vertex_count, "type": "VEC2",
            },
            {
                "bufferView": 4, "byteOffset": 0, "componentType": 5126,
                "count": vertex_count, "type": "VEC2",
            },
            {
                "bufferView": 5, "byteOffset": 0, "componentType": 5125,
                "count": index_count, "type": "SCALAR",
            },
        ],
        "bufferViews": [
            {"buffer": 0, "byteOffset": pos_offset, "byteLength": len(pos_bytes), "target": 34962},
            {"buffer": 0, "byteOffset": normal_offset, "byteLength": len(normal_bytes), "target": 34962},
            {"buffer": 0, "byteOffset": tangent_offset, "byteLength": len(tangent_bytes), "target": 34962},
            {"buffer": 0, "byteOffset": uv0_offset, "byteLength": len(uv0_bytes), "target": 34962},
            {"buffer": 0, "byteOffset": uv1_offset, "byteLength": len(uv1_bytes), "target": 34962},
            {"buffer": 0, "byteOffset": index_offset, "byteLength": len(index_bytes), "target": 34963},
        ],
        "buffers": [{"uri": BIN_NAME, "byteLength": total_length}],
    }

    gltf_path = os.path.join(OUT_DIR, GLTF_NAME)
    try:
        with open(gltf_path, "w", encoding="utf-8") as gltf_file:
            json.dump(gltf, gltf_file, indent=2)
    except OSError as error:
        print(f"[ERROR] .gltfの書き込みに失敗しました: {gltf_path} ({error})", file=sys.stderr)
        raise

    triangle_count = len(indices)
    print(f"vertex_count={vertex_count} triangle_count={triangle_count} buffer_bytes={total_length}")
    print(f"size={WATER_SIZE}m segments={WATER_SEGMENTS} uv_meters_per_tile={WATER_UV_METERS_PER_TILE}")
    print(f"wrote {gltf_path}")
    print(f"wrote {bin_path}")


if __name__ == "__main__":
    main()
