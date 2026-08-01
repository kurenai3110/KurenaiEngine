"""モン・サン=ミシェル検証シーン(P1)用の、島を取り囲む干潟(砂の地形)を生成する。

実写の干潟は完全な平坦ではなく、緩やかな起伏と、島の足元に向かって盛り上がる裾野
(エプロン)を持つ。エプロンが無いと島の岩(generate_msm_proxy.pyのIsland)の底面が
干潟のノイズによって水面下に沈んだり、逆に不自然に浮いたりする箇所が出るため、
このスクリプトは「低周波ノイズ」と「島の裾のエプロン」を足し合わせて高さ場を作る。

--- ローカル座標とワールド座標の関係(重要) ---
このスクリプトは干潟メッシュ単体の生成器であり、島の位置を知らない。しかし
Scenes/MontSaintMichel.ksceneでは、このモデル(TidalFlat)にTranslation = (0, 0, -800)を
与える一方、Island(島本体)はTranslation = (0, 0, 0)のままワールド原点に置く
(座標系・単位の約束: 島の底面中心がワールド原点)。
つまりワールド原点(島の位置)は、このスクリプトのローカル座標では (0, 0, +800) にあたる
(world = local + translation なので local = world - translation = (0,0,0) - (0,0,-800))。
エプロンの中心はこの食い違いを踏まえて ISLAND_CENTER_LOCAL に置く。ここを島の実際の
ローカル原点(0,0)のままにしてしまうと、エプロンがワールド上で島から800mずれた場所に
盛り上がるだけになり、島の足元が水面から出ない/干潟に埋まるといった破綻が起きる。

--- 低周波ノイズ ---
外部ライブラリ(numpy等)を使わず、格子点にハッシュ由来の擬似乱数を置いて双一次補間する
value noiseを自前で実装している。ハッシュは(格子座標, シード)から決定論的に値を返す
純粋関数なので、NOISE_SEEDを固定してある限り毎回まったく同じ地形が生成される
(A/B比較の再現性のため。シェーダやパラメータを変えた前後で「地形自体は変わっていない」
ことを保証できないと、見た目の差がどちらに起因するのか切り分けられなくなる)。

法線は隣接する頂点の高さの差分(中心差分)から求める。TANGENTは+X軸を面へ投影して
正規化したもの(高さがほぼ平坦なので通常はほぼ(1,0,0)に近いが、傾斜がある場所では
わずかに傾く)。UVはワールド50mあたり1タイル。TEXCOORD_1はTEXCOORD_0と同値。

生成物: Assets/Source/MontSaintMichelStudy/TidalFlat.gltf + TidalFlat.bin
KurenaiPacker.exeで Assets/Packed/MontSaintMichelStudy/TidalFlat.kmodel へ変換して使う
(--bake-occlusionは付けない。緩やかな地形にAOを焼いても効果が薄く、xatlasのUV展開が
時間の無駄になるため)。
"""

import json
import math
import os
import struct
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(SCRIPT_DIR, "..", "Assets", "Source", "MontSaintMichelStudy")
GLTF_NAME = "TidalFlat.gltf"
BIN_NAME = "TidalFlat.bin"

# --- 干潟の寸法 ---
# 6000m四方。水面(4000m四方)より一回り広く取り、水面の端が地平線の手前で干潟に
# 隠れて途切れて見えないようにする
TIDAL_FLAT_SIZE = 6000.0
TIDAL_FLAT_SEGMENTS = 128

# --- UVタイリング ---
TIDAL_FLAT_UV_METERS_PER_TILE = 50.0

# --- 島の位置(このスクリプトのローカル座標系での値。モジュールdocstring参照) ---
ISLAND_CENTER_LOCAL_X = 0.0
ISLAND_CENTER_LOCAL_Z = 800.0

# --- エプロン(島の裾野) ---
# 島(generate_msm_proxy.py)の岩の底面半径と一致させる。この半径の内側は「島の真下」として
# 平坦に持ち上げる
ISLAND_BASE_RADIUS = 125.0
# この半径より外側はエプロンの影響を受けない(通常の低周波ノイズ地形に戻る)
APRON_OUTER_RADIUS = 400.0
# 島の真下の高さ。水面(.ksceneでTranslation.y=0.15)より確実に上に出るようにするための余裕
APRON_CENTER_HEIGHT = 2.0

# --- 低周波ノイズ ---
# 再現性のため固定(モジュールdocstring参照)。値そのものに意味はなく「変えると別の地形になる」
# ことだけが重要
NOISE_SEED = 20260801
# (格子1マスの一辺の長さ[m], 振幅[m]) のオクターブを3枚重ねる。振幅の合計は0.5m程度に収める
# (仕様の「振幅±0.5m程度」に合わせるため)
NOISE_OCTAVES = [
    (480.0, 0.30),
    (180.0, 0.14),
    (70.0, 0.06),
]

# --- マテリアル ---
# 濡れた砂を想定。metallicは0、roughnessは高め(0.85)にして拡散反射主体にする
TIDAL_FLAT_BASE_COLOR = [0.55, 0.50, 0.43, 1.0]
TIDAL_FLAT_METALLIC = 0.0
TIDAL_FLAT_ROUGHNESS = 0.85


def _hash01(ix, iz, seed):
    """整数格子座標(ix, iz)とシードから[0, 1)の擬似乱数を1つ返す。

    出典のあるノイズアルゴリズムではなく、加算→XORシフト→乗算を組み合わせた自作の
    整数ハッシュ(値そのものに周期性の保証はないが、格子点ごとに十分ばらけた値を
    決定論的に返せれば value noise の入力としては足りる)。Pythonの整数は多倍長なので
    毎回0xFFFFFFFFでマスクしてuint32相当に丸める。
    """
    h = (ix * 374761393 + iz * 668265263 + seed * 2246822519) & 0xFFFFFFFF
    h = (h ^ (h >> 13)) & 0xFFFFFFFF
    h = (h * 1274126177) & 0xFFFFFFFF
    h = (h ^ (h >> 16)) & 0xFFFFFFFF
    return h / 4294967295.0


def _smoothstep(t):
    return t * t * (3.0 - 2.0 * t)


def _value_noise(x, z, cell_size, seed):
    """双一次補間のvalue noise。戻り値は[0, 1)。"""
    gx = x / cell_size
    gz = z / cell_size
    ix0 = math.floor(gx)
    iz0 = math.floor(gz)
    fx = gx - ix0
    fz = gz - iz0
    ix0 = int(ix0)
    iz0 = int(iz0)

    v00 = _hash01(ix0, iz0, seed)
    v10 = _hash01(ix0 + 1, iz0, seed)
    v01 = _hash01(ix0, iz0 + 1, seed)
    v11 = _hash01(ix0 + 1, iz0 + 1, seed)

    sx = _smoothstep(fx)
    sz = _smoothstep(fz)
    top = v00 + (v10 - v00) * sx
    bottom = v01 + (v11 - v01) * sx
    return top + (bottom - top) * sz


def terrain_noise_height(x, z):
    """低周波ノイズによる高さ(エプロンを含まない、m単位)。"""
    height = 0.0
    for octave_index, (cell_size, amplitude) in enumerate(NOISE_OCTAVES):
        # オクターブごとにシードをずらし、同じ格子座標でも異なる乱数列になるようにする
        n = _value_noise(x, z, cell_size, NOISE_SEED + octave_index * 97)
        height += (n * 2.0 - 1.0) * amplitude
    return height


def apron_height(x, z):
    """島の裾のエプロンによる高さの上乗せ分(m単位、0以上)。"""
    dx = x - ISLAND_CENTER_LOCAL_X
    dz = z - ISLAND_CENTER_LOCAL_Z
    dist = math.sqrt(dx * dx + dz * dz)
    if dist <= ISLAND_BASE_RADIUS:
        return APRON_CENTER_HEIGHT
    if dist >= APRON_OUTER_RADIUS:
        return 0.0
    t = (dist - ISLAND_BASE_RADIUS) / (APRON_OUTER_RADIUS - ISLAND_BASE_RADIUS)
    s = _smoothstep(t)
    return APRON_CENTER_HEIGHT * (1.0 - s)


def terrain_height(x, z):
    return terrain_noise_height(x, z) + apron_height(x, z)


def build_grid(size, segments, uv_meters_per_tile):
    half = size * 0.5
    step = size / segments
    verts_per_row = segments + 1

    # 先に全頂点の高さを求めておく(法線の中心差分に使い回すため)
    heights = [[0.0] * verts_per_row for _ in range(verts_per_row)]
    for j in range(verts_per_row):
        z = -half + j * step
        for i in range(verts_per_row):
            x = -half + i * step
            heights[j][i] = terrain_height(x, z)

    positions = []
    normals = []
    tangents = []
    uv0 = []
    uv1 = []

    for j in range(verts_per_row):
        z = -half + j * step
        for i in range(verts_per_row):
            x = -half + i * step
            y = heights[j][i]
            positions.append((x, y, z))

            # 中心差分(境界では片側差分)で高さ場の勾配を求め、法線を再構成する
            i_prev = max(i - 1, 0)
            i_next = min(i + 1, verts_per_row - 1)
            j_prev = max(j - 1, 0)
            j_next = min(j + 1, verts_per_row - 1)
            dx_world = (i_next - i_prev) * step
            dz_world = (j_next - j_prev) * step
            dhdx = (heights[j][i_next] - heights[j][i_prev]) / dx_world if dx_world > 0.0 else 0.0
            dhdz = (heights[j_next][i] - heights[j_prev][i]) / dz_world if dz_world > 0.0 else 0.0

            normal_vec = (-dhdx, 1.0, -dhdz)
            normal_len = math.sqrt(sum(c * c for c in normal_vec))
            normal = tuple(c / normal_len for c in normal_vec)
            normals.append(normal)

            # +X軸を面へ投影(Gram-Schmidt)して正規化する。地形がほぼ平坦なため通常は
            # (1,0,0)に近い値になるが、傾斜のある場所ではその分だけ傾く
            world_x_axis = (1.0, 0.0, 0.0)
            dot_nx = sum(world_x_axis[k] * normal[k] for k in range(3))
            tangent_vec = tuple(world_x_axis[k] - normal[k] * dot_nx for k in range(3))
            tangent_len = math.sqrt(sum(c * c for c in tangent_vec))
            if tangent_len < 1e-8:
                # 法線がほぼ+X(ありえないほどの急斜面)の場合のフォールバック。
                # 通常の地形では起こらないが、ゼロ除算を避けるため+Z方向を代わりに使う
                tangent = (0.0, 0.0, 1.0)
            else:
                tangent = tuple(c / tangent_len for c in tangent_vec)
            tangents.append((tangent[0], tangent[1], tangent[2], 1.0))

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
            # generate_water_plane.pyと同じ巻き順の規約(a→d→b、d→c→bで+Y寄りの法線)。
            # 高さの起伏は緩やかなので、この巻き順のままジオメトリ法線がひっくり返ることはない
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
        TIDAL_FLAT_SIZE, TIDAL_FLAT_SEGMENTS, TIDAL_FLAT_UV_METERS_PER_TILE)

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
        "asset": {"version": "2.0", "generator": "KurenaiEngine MontSaintMichelStudy TidalFlat generator"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "TidalFlat", "mesh": 0}],
        "meshes": [{
            "name": "TidalFlat",
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
            "name": "TidalFlat",
            "pbrMetallicRoughness": {
                "baseColorFactor": TIDAL_FLAT_BASE_COLOR,
                "metallicFactor": TIDAL_FLAT_METALLIC,
                "roughnessFactor": TIDAL_FLAT_ROUGHNESS,
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
    print(f"size={TIDAL_FLAT_SIZE}m segments={TIDAL_FLAT_SEGMENTS} uv_meters_per_tile={TIDAL_FLAT_UV_METERS_PER_TILE}")
    print(f"island_center_local=({ISLAND_CENTER_LOCAL_X}, {ISLAND_CENTER_LOCAL_Z}) apron=[{ISLAND_BASE_RADIUS}, {APRON_OUTER_RADIUS}]m height={APRON_CENTER_HEIGHT}m")
    print(f"noise_seed={NOISE_SEED} octaves={NOISE_OCTAVES}")
    print(f"wrote {gltf_path}")
    print(f"wrote {bin_path}")


if __name__ == "__main__":
    main()
