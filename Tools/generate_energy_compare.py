# スペキュラ・エネルギー補正の3方式比較用テストアセットを生成する。
#
# generate_furnace_test.py が「F0=1・ラフネス0→1の11球」の1列だけなのに対し、
# こちらは F0 も振った2次元グリッドを作る。3方式(Linear / Series / Kulla-Conty)は
# F0=1 では数学的に一致してしまい区別できないため、F0<1 の行が比較に必須になる
# (詳細は docs/Architecture.html 14.9節)。
#
#   列(X軸) = roughness 0.0 → 1.0 の11段
#   行(Z軸) = F0
#     行0: metallic=1.0, baseColor=(1,1,1)       → F0=1.00  白金属。3方式が厳密に一致する行
#     行1: metallic=1.0, baseColor=(1,0.78,0.34) → 金。有色金属の彩度差(Kulla-ContyのFavg^2)
#     行2: metallic=0.5, baseColor=(1,1,1)       → F0≒0.52 LinearとSeriesが最も分離する行
#     行3: metallic=0.0, baseColor=(0,0,0)       → F0=0.04 黒誘電体。拡散が0なので鏡面だけが見える
#
# あわせて「環境光0」のテスト用に真っ黒な一様キューブマップも書き出す。
# IBLIntensity=0 では定数色アンビエントのフォールバックへ落ちてしまい環境光が0にならないため、
# 真っ黒なスカイボックス + IBLIntensity=1.0 が唯一の正しい「環境光0」の作り方
# (DeferredLighting.hlsl の ShadowParams.z 分岐参照)。
#
# 使い方:
#   python Tools/generate_energy_compare.py
#   その後 KurenaiPacker で Assets/Source/EnergyCompareTest → Assets/Packed/EnergyCompareTest へ変換する

import json
import os
import struct
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, SCRIPT_DIR)

from generate_material_test import generate_sphere, LAT_SEGMENTS, LON_SEGMENTS, RADIUS
from generate_furnace_test import write_uniform_cubemap_dds, CUBEMAP_FACE_SIZE

ROUGHNESS_STEPS = 11
COLUMN_SPACING = 2.5
# 行の間隔は列より広く取る。俯瞰カメラでは奥行き方向の間隔が画面上では縮んで見えるため、
# 列と同じ2.5だと手前の行が奥の行を隠してしまう(実際に隠れた)
ROW_SPACING = 6.0

# F0 は metallicFactor だけで振る。
#
# 【重要】不透明パスは baseColorFactor を意図的に適用しない(GBuffer.hlsl は
# ベースカラー"テクスチャ"のみを読み、factorはObjectConstantsにあるmetallic/roughnessだけ)。
# そのため baseColorFactor で色や黒を指定してもアルベドは常に(1,1,1)になり、
# 有色金属の行や「拡散0の黒誘電体」の行はテクスチャを用意しない限り作れない。
# F0 = lerp(0.04, albedo, metallic) なので、metallic を振れば F0 は 0.04〜1.0 を掃引できる。
#
# 副作用として metallic が小さい行では白い拡散項が乗る(kd = (1-F)(1-metallic))。
# 拡散項は3方式で全く同じなので、方式間の差を取る比較には影響しない。
#
# (行の名前, metallicFactor, 説明)
ROWS = [
    ("Metal_F0_100", 1.00, "F0=1.00 完全な金属。3方式が厳密に一致し区別できない行"),
    ("Metal_F0_076", 0.75, "F0≒0.76"),
    ("Metal_F0_052", 0.50, "F0≒0.52 LinearとSeriesが最も分離する行"),
    ("Metal_F0_028", 0.25, "F0≒0.28"),
    ("Metal_F0_004", 0.00, "F0=0.04 誘電体(白い拡散項が乗る)"),
]

MODEL_OUT_DIR = os.path.join(SCRIPT_DIR, "..", "Assets", "Source", "EnergyCompareTest")
GLTF_NAME = "EnergyCompareTest.gltf"
BIN_NAME = "EnergyCompareTest.bin"

SKYBOX_OUT_DIR = os.path.join(SCRIPT_DIR, "..", "Assets", "Packed", "Skybox")
BLACK_SKYBOX_NAME = "UniformBlack.dds"


def main():
    os.makedirs(MODEL_OUT_DIR, exist_ok=True)
    os.makedirs(SKYBOX_OUT_DIR, exist_ok=True)

    # 環境光0のテスト用。放射輝度0の一様キューブマップ
    black_skybox_path = os.path.join(SKYBOX_OUT_DIR, BLACK_SKYBOX_NAME)
    write_uniform_cubemap_dds(black_skybox_path, CUBEMAP_FACE_SIZE, 0.0)

    positions, normals, uvs, indices = generate_sphere(LAT_SEGMENTS, LON_SEGMENTS, RADIUS)
    vertex_count = len(positions)
    index_count = len(indices) * 3

    buffer_bytes = bytearray()

    def append_aligned(data: bytes):
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

    pos_min = [min(p[axis] for p in positions) for axis in range(3)]
    pos_max = [max(p[axis] for p in positions) for axis in range(3)]

    bin_path = os.path.join(MODEL_OUT_DIR, BIN_NAME)
    with open(bin_path, "wb") as f:
        f.write(buffer_bytes)

    sphere_count = ROUGHNESS_STEPS * len(ROWS)
    gltf = {
        "asset": {"version": "2.0", "generator": "KurenaiEngine EnergyCompareTest generator"},
        "scene": 0,
        "scenes": [{"nodes": list(range(sphere_count))}],
        "nodes": [],
        "meshes": [],
        "materials": [],
        "accessors": [
            {
                "bufferView": 0, "byteOffset": 0, "componentType": 5126,
                "count": vertex_count, "type": "VEC3", "min": pos_min, "max": pos_max,
            },
            {"bufferView": 1, "byteOffset": 0, "componentType": 5126, "count": vertex_count, "type": "VEC3"},
            {"bufferView": 2, "byteOffset": 0, "componentType": 5126, "count": vertex_count, "type": "VEC2"},
            {"bufferView": 3, "byteOffset": 0, "componentType": 5125, "count": index_count, "type": "SCALAR"},
        ],
        "bufferViews": [
            {"buffer": 0, "byteOffset": pos_offset, "byteLength": len(pos_bytes), "target": 34962},
            {"buffer": 0, "byteOffset": normal_offset, "byteLength": len(normal_bytes), "target": 34962},
            {"buffer": 0, "byteOffset": uv_offset, "byteLength": len(uv_bytes), "target": 34962},
            {"buffer": 0, "byteOffset": index_offset, "byteLength": len(index_bytes), "target": 34963},
        ],
        "buffers": [{"uri": BIN_NAME, "byteLength": len(buffer_bytes)}],
    }

    index = 0
    for row, (row_name, metallic, _description) in enumerate(ROWS):
        z = (row - (len(ROWS) - 1) / 2.0) * ROW_SPACING
        for column in range(ROUGHNESS_STEPS):
            roughness = column / (ROUGHNESS_STEPS - 1)
            x = (column - (ROUGHNESS_STEPS - 1) / 2.0) * COLUMN_SPACING

            gltf["materials"].append({
                "name": f"{row_name}_Roughness_{roughness:.1f}",
                "pbrMetallicRoughness": {
                    "baseColorFactor": [1.0, 1.0, 1.0, 1.0],
                    "metallicFactor": metallic,
                    "roughnessFactor": roughness,
                },
            })
            gltf["meshes"].append({
                "name": f"{row_name}_{column}",
                "primitives": [{
                    "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
                    "indices": 3,
                    "material": index,
                }],
            })
            gltf["nodes"].append({
                "name": f"{row_name}_{column}",
                "mesh": index,
                "translation": [x, 0.0, z],
            })
            index += 1

    gltf_path = os.path.join(MODEL_OUT_DIR, GLTF_NAME)
    with open(gltf_path, "w", encoding="utf-8") as f:
        json.dump(gltf, f, indent=2)

    print(f"spheres={sphere_count} ({ROUGHNESS_STEPS} roughness steps x {len(ROWS)} F0 rows)")
    for row_name, metallic, description in ROWS:
        print(f"  {row_name:<16} metallic={metallic:<5} {description}")
    print(f"wrote {gltf_path}")
    print(f"wrote {bin_path}")
    print(f"wrote {black_skybox_path} (radiance=0, 環境光0テスト用)")


if __name__ == "__main__":
    main()
