"""White Furnace Test 用のアセット(一様なキューブマップ + 金属球列)を生成する。

White Furnace Test は「一様な放射輝度の環境に物体を置くと、BRDFがエネルギー保存していれば
物体が背景と完全に同じ明るさになり見分けがつかなくなる」ことを利用した検証手法。
本エンジンのスペキュラBRDFのマルチスキャッタリング・エネルギー補正
(docs/Architecture.html 14.9節)が正しく効いているかを一目で判定できる。

原理:
  スカイボックスが一様な放射輝度 L なら、そこから焼かれるIBLのプリフィルタ済み鏡面も
  全方向・全ミップで L になる。金属(F0 = baseColor = 1)の鏡面IBLは

      specularIBL = L * (F0 * A + B) = L * (A + B) = L * Ess

  となり、単一散乱のままでは Ess < 1 のぶんだけ背景より暗くなる(ラフネス1.0では0.307、
  つまり7割が失われる)。エネルギー補正 comp = 1 + F0 * (1/Ess - 1) を掛けると
  F0 = 1 では comp = 1/Ess なので

      specularIBL = L * Ess * (1/Ess) = L

  となり背景と厳密に一致して球が消える。したがって
    ・補正OFF → 粗い球ほど暗い影として浮き上がる
    ・補正ON  → 球が背景に溶けて見えなくなる
  という形で、補正の有無が定性的に判定できる。

検証を成立させるための前提(FurnaceTest.ksceneが設定する):
  ・[Scene] Skybox        … このスクリプトが生成する一様キューブマップ
  ・[Sun] Enabled = false … 太陽が乗ると球が背景より明るくなってしまう
  ・[Scene] IBLIntensity = 1.0
        背景のスカイボックスは強度倍率を掛けずにそのまま描かれるため、
        既定の0.5のままだと球だけが半分の明るさになり一致しない
  ・[Scene] AmbientOcclusion = false
        球の縁がAOで暗くなると「エネルギー損失による暗さ」と区別がつかなくなる

外部依存を増やさないため、DDS・glTF・バイナリバッファはすべて標準ライブラリだけで書き出す
(球の頂点生成は generate_material_test.py と同じものを使う)。
"""

import json
import os
import struct
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, SCRIPT_DIR)

from generate_material_test import generate_sphere, LAT_SEGMENTS, LON_SEGMENTS, RADIUS

SPHERE_COUNT = 11
SPACING = 2.5

# 一様キューブマップの面サイズ。内容が単色なので解像度は品質に影響しないが、
# プリフィルタ済み鏡面マップ(128x128、6ミップ)の生成元として十分な大きさにしておく
CUBEMAP_FACE_SIZE = 64
# 環境の放射輝度。Reinhardトーンマップ out = x / (1 + x) により 1.0 は中間調(約0.5)へ写るため、
# 補正OFFで球が暗くなったときの差が見やすい
ENVIRONMENT_RADIANCE = 1.0

MODEL_OUT_DIR = os.path.join(SCRIPT_DIR, "..", "Assets", "Source", "FurnaceTest")
GLTF_NAME = "FurnaceTest.gltf"
BIN_NAME = "FurnaceTest.bin"

# スカイボックスはKurenaiPackerを通さずランタイムが直接読むため、Packed側へ直接出力する
# (Assets/Packed/Skybox/Sky.dds と同じ扱い。generate_sky_cubemap.py も同様)
SKYBOX_OUT_DIR = os.path.join(SCRIPT_DIR, "..", "Assets", "Packed", "Skybox")
SKYBOX_NAME = "UniformWhite.dds"


def float_to_half(value: float) -> int:
    """float を IEEE 754 half(16bit)のビット列へ変換する。

    numpy等の外部依存を避けるため、struct でfloat32のビット列を取り出して
    指数・仮数を詰め直す。この用途では 0.0〜数十程度の正の有限値しか扱わないため、
    非正規化数・Inf・NaNは扱わない(範囲外は飽和させる)。
    """
    bits = struct.unpack("<I", struct.pack("<f", value))[0]
    sign = (bits >> 16) & 0x8000
    exponent = ((bits >> 23) & 0xFF) - 127 + 15
    mantissa = (bits >> 13) & 0x03FF

    if exponent <= 0:
        # halfの正規化数で表せないほど小さい値は0にする
        return sign
    if exponent >= 0x1F:
        # halfの最大値で飽和させる
        return sign | 0x7BFF
    return sign | (exponent << 10) | mantissa


def write_uniform_cubemap_dds(path, face_size, radiance):
    """全6面が単色のキューブマップDDS(R16G16B16A16_FLOAT)を書き出す。

    ヘッダ構成は generate_sky_cubemap.py と同じ(DX10拡張ヘッダ付き、ミップ1枚)。
    """
    DDSD_CAPS = 0x1
    DDSD_HEIGHT = 0x2
    DDSD_WIDTH = 0x4
    DDSD_PIXELFORMAT = 0x1000
    DDSD_MIPMAPCOUNT = 0x20000
    DDSD_LINEARSIZE = 0x80000

    DDPF_FOURCC = 0x4
    DDSCAPS_TEXTURE = 0x1000
    DDSCAPS_COMPLEX = 0x8
    # 6面すべてを持つキューブマップであることを示すフラグ一式
    DDSCAPS2_CUBEMAP_ALLFACES = 0x200 | 0x400 | 0x800 | 0x1000 | 0x2000 | 0x4000 | 0x8000

    bytes_per_pixel = 8  # R16G16B16A16_FLOAT
    pitch = face_size * bytes_per_pixel

    header = bytearray()
    header += struct.pack("<I", 124)  # dwSize
    header += struct.pack("<I", DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT | DDSD_MIPMAPCOUNT | DDSD_LINEARSIZE)
    header += struct.pack("<I", face_size)  # dwHeight
    header += struct.pack("<I", face_size)  # dwWidth
    header += struct.pack("<I", pitch)      # dwPitchOrLinearSize
    header += struct.pack("<I", 0)          # dwDepth
    header += struct.pack("<I", 1)          # dwMipMapCount
    header += b"\x00" * 44                  # dwReserved1[11]
    # DDS_PIXELFORMAT
    header += struct.pack("<I", 32)         # dwSize
    header += struct.pack("<I", DDPF_FOURCC)
    header += b"DX10"
    header += struct.pack("<I", 0) * 5      # dwRGBBitCount, dwR/G/B/ABitMask
    header += struct.pack("<I", DDSCAPS_TEXTURE | DDSCAPS_COMPLEX)
    header += struct.pack("<I", DDSCAPS2_CUBEMAP_ALLFACES)
    header += struct.pack("<I", 0) * 3      # dwCaps3, dwCaps4, dwReserved2
    assert len(header) == 124, len(header)

    DXGI_FORMAT_R16G16B16A16_FLOAT = 10
    D3D10_RESOURCE_DIMENSION_TEXTURE2D = 3
    DDS_RESOURCE_MISC_TEXTURECUBE = 0x4

    header_dx10 = struct.pack(
        "<5I",
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D10_RESOURCE_DIMENSION_TEXTURE2D,
        DDS_RESOURCE_MISC_TEXTURECUBE,
        1,  # arraySize(キューブ数)
        0,  # miscFlags2
    )

    half = float_to_half(radiance)
    texel = struct.pack("<4H", half, half, half, float_to_half(1.0))
    face_bytes = texel * (face_size * face_size)

    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(b"DDS ")
        f.write(bytes(header))
        f.write(header_dx10)
        for _ in range(6):
            f.write(face_bytes)


def main():
    os.makedirs(MODEL_OUT_DIR, exist_ok=True)

    skybox_path = os.path.join(SKYBOX_OUT_DIR, SKYBOX_NAME)
    write_uniform_cubemap_dds(skybox_path, CUBEMAP_FACE_SIZE, ENVIRONMENT_RADIANCE)

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

    gltf = {
        "asset": {"version": "2.0", "generator": "KurenaiEngine FurnaceTest generator"},
        "scene": 0,
        "scenes": [{"nodes": list(range(SPHERE_COUNT))}],
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

    for i in range(SPHERE_COUNT):
        roughness = i / (SPHERE_COUNT - 1)
        x = (i - (SPHERE_COUNT - 1) / 2.0) * SPACING

        gltf["materials"].append({
            "name": f"FurnaceMetal_Roughness_{roughness:.1f}",
            "pbrMetallicRoughness": {
                # baseColor = 1 かつ metallic = 1 なので F0 = 1(完全反射の金属)。
                # このときエネルギー補正は comp = 1/Ess となり、補正後の方向アルベドが
                # 厳密に1になる ―― つまり球が背景と同じ明るさになる
                "baseColorFactor": [1.0, 1.0, 1.0, 1.0],
                "metallicFactor": 1.0,
                "roughnessFactor": roughness,
            },
        })

        gltf["meshes"].append({
            "name": f"FurnaceSphere_{i}",
            "primitives": [{
                "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
                "indices": 3,
                "material": i,
            }],
        })

        gltf["nodes"].append({
            "name": f"FurnaceSphere_{i}",
            "mesh": i,
            "translation": [x, 0.0, 0.0],
        })

    gltf_path = os.path.join(MODEL_OUT_DIR, GLTF_NAME)
    with open(gltf_path, "w", encoding="utf-8") as f:
        json.dump(gltf, f, indent=2)

    print(f"spheres={SPHERE_COUNT} (metallic=1.0, roughness 0.0-1.0)")
    print(f"environment radiance={ENVIRONMENT_RADIANCE} ({CUBEMAP_FACE_SIZE}x{CUBEMAP_FACE_SIZE} x6, R16G16B16A16_FLOAT)")
    print(f"wrote {gltf_path}")
    print(f"wrote {bin_path}")
    print(f"wrote {skybox_path}")


if __name__ == "__main__":
    main()
