"""反射プローブ(リフレクションプローブ)の検証用シーンを生成する。

Sponzaは非金属・中〜高ラフネスのマテリアルばかりで鏡面反射が弱く、アトリウムが上に開いていて
プローブにも空が大量に写り込むため、プローブの効きが見て分かりにくい。このシーンは
「プローブが効いているかどうかが一目で分かる」ことだけを目的に構成してある。

    ┌───────────────────┬───────────┐  y=7
    │▓▓▓▓ 天井あり ▓▓▓▓│ 天井開口  │      ▓ = 中央の仕切り壁(中央に開口部)
    │   西室(密閉)     ▓  東室     │
    │   暖色の壁+ポイント▓ 寒色の壁  │
    │   ライト+エミッシブ▓ +太陽光   │
    │  ●   ●   ●   ●   ●   ●   ●  │  ← 金属球列(metallic=1.0, roughness=0.05)
    └───────────────────┴───────────┘  y=0
   x=-12                x=0        x=+12
            床 = metallic 1.0 / roughness 0.12(鏡面)

このレイアウトが担う検証項目:

- プローブ vs グローバルIBL: 西室は密閉なので、プローブを切ると「屋内なのに金属球が空を映す」
  という明らかな破綻が出る。入れれば実際の暖色の壁が映る
- プローブ間ブレンド: 西室(暖色・ポイントライト)と東室(寒色・太陽光)は焼かれる環境が
  まったく別物になるため、開口部を貫く球列の反射色が暖→寒へ遷移する。ブレンドを切ると
  そこに硬い継ぎ目が出る
- 視差補正: 鏡面の床に壁のエミッシブ帯が映る。補正が効いていれば帯の反射は帯の真下へ接地し、
  切ると壁から浮いてずれる
- 半透明パスのプローブ適用(19.11節): 各室に立てたガラス板は半透明フォワードパスで描かれ、
  SSRが適用されないため環境ソースが唯一の映り込みになる。金属球と同様、プローブを切ると
  密閉された西室のガラスにも空が映る

ライトとプローブ自体はモデルではなくScenes/ProbeTest.kscene側で配置する(数値を触りながら
確認しやすくするため)。このスクリプトが吐くのはジオメトリとマテリアルだけ。

生成物: Assets/Source/ProbeTest/ProbeTest.gltf + ProbeTest.bin
KurenaiPacker.exeで Assets/Packed/ProbeTest/ProbeTest.kmodel へ変換して使う(README手順5参照)。
"""

import json
import math
import os
import struct

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(SCRIPT_DIR, "..", "Assets", "Source", "ProbeTest")
GLTF_NAME = "ProbeTest.gltf"
BIN_NAME = "ProbeTest.bin"

# --- ホール全体の寸法 ---
HALL_MIN_X = -12.0
HALL_MAX_X = 12.0
HALL_MIN_Z = -6.0
HALL_MAX_Z = 6.0
HALL_HEIGHT = 7.0

# --- 中央の仕切り壁と開口部 ---
PARTITION_X = 0.0
# 開口部は広めに取る。仕切りの役目は「2つの部屋として別々の環境が焼かれること」であって
# 視線を遮ることではないため、狭いと球列と西室が見通せず何が起きているか読み取れなくなる。
# 左右に幅2の壁を残し、上にまぐさを渡すことで壁としては成立させる
DOORWAY_HALF_Z = 4.0    # 開口部は z ∈ [-4, 4]
DOORWAY_HEIGHT = 5.5    # その上(y ∈ [5.5, 7])はまぐさで塞ぐ

# --- 金属球列。開口部を貫くz=0の軸上に並べ、ブレンド帯を横切らせる ---
LAT_SEGMENTS = 16
LON_SEGMENTS = 32
SPHERE_RADIUS = 0.7
SPHERE_X = [-9.0, -6.0, -3.0, 0.0, 3.0, 6.0, 9.0]
SPHERE_ROUGHNESS = 0.05

# --- 半透明のガラス板 ---
# 板の向き(法線のXZ平面上の方位)は、初期カメラからの入射角がおよそ60度になるように選んである。
# 正面(入射角0度)だと非金属のフレネル反射はF0=0.04と弱く映り込みがほとんど読めず、逆に
# 真横(90度近く)だと反射は最も強くなるが画面上ではただの細い線にしかならない。
# 60度前後が「面として見えて、かつ映り込みが読める」両立点になる
GLASS_YAW_DEG = 70.0
GLASS_HALF_LENGTH = 2.5
GLASS_MIN_Y = 0.6
GLASS_MAX_Y = 4.6
GLASS_ROUGHNESS = 0.05
GLASS_ALPHA = 0.18
# 西室・東室で同じ向き・同じ大きさの板を、ホール中心から等距離の位置へ置く
GLASS_CENTERS = {"West": (-7.0, -3.0), "East": (7.0, -3.0)}

# --- エミッシブ帯(視差補正の基準線を兼ねる) ---
STRIPE_HALF_WIDTH = 0.5
STRIPE_MIN_Y = 0.5
STRIPE_MAX_Y = 6.0
# 壁と同一平面だとZファイティングするので、法線方向へわずかに浮かせる
STRIPE_OFFSET = 0.02


def generate_sphere(lat_segments, lon_segments, radius):
    # Tools/generate_material_test.py・generate_light_test.pyのgenerate_sphereと同一の構成
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
            # glTFの標準(右手座標系でCCWが表)に合わせた巻き順。
            # エンジン側でaiProcess_ConvertToLeftHandedにより左手座標系用に変換される
            if i != 0:
                indices.append((k1 + j, k1 + j + 1, k2 + j))
            if i != lat_segments - 1:
                indices.append((k1 + j + 1, k2 + j + 1, k2 + j))

    return positions, normals, uvs, indices


def generate_quad_double_sided(corners, normal):
    """周を辿る順(v0->v1->v2->v3)の4点から、表裏そろえた板を1枚作る。

    glTFは右手座標系でCCWが表なので、面が正しく描かれる巻き順とは
    「三角形の外積(右手系)がシェーディング法線と同じ向きになる巻き順」を指す。
    これを面ごとに手で合わせると、取り違えた面だけが黒くなる(法線が裏を向く)という
    分かりにくい形で現れるため、ここでcornersの周回向きから幾何法線を求めて機械的に揃える。

    そのうえで、法線・巻き順を反転した2組目を同じ座標へ重ねて板を両面にしている。
    裏面はバックフェースカリングで落ちるので同一平面でもZファイティングは起きず、
    部屋を外から見たときに壁が消えることもない。板は数枚しかないので頂点数の増加は無視できる。
    """
    c0, c1, c2 = corners[0], corners[1], corners[2]
    edge1 = (c1[0] - c0[0], c1[1] - c0[1], c1[2] - c0[2])
    edge2 = (c2[0] - c0[0], c2[1] - c0[1], c2[2] - c0[2])
    geometric_normal = (
        edge1[1] * edge2[2] - edge1[2] * edge2[1],
        edge1[2] * edge2[0] - edge1[0] * edge2[2],
        edge1[0] * edge2[1] - edge1[1] * edge2[0],
    )
    facing = sum(geometric_normal[axis] * normal[axis] for axis in range(3))

    if facing >= 0.0:
        front = [(0, 1, 2), (0, 2, 3)]
        back = [(4, 6, 5), (4, 7, 6)]
    else:
        # 与えられた周回向きだと外積が法線と逆を向くので、巻き順を反転して揃える
        front = [(0, 2, 1), (0, 3, 2)]
        back = [(4, 5, 6), (4, 6, 7)]

    positions = list(corners) + list(corners)
    inverse_normal = (-normal[0], -normal[1], -normal[2])
    normals = [normal] * 4 + [inverse_normal] * 4
    quad_uvs = [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)]
    uvs = quad_uvs + quad_uvs
    return positions, normals, uvs, front + back


def main():
    os.makedirs(OUT_DIR, exist_ok=True)

    buffer_bytes = bytearray()

    def append_aligned(data: bytes):
        # 4バイト境界に揃える(glTFのアクセッサ要件)
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

    def add_material(name, roughness, metallic, base_color, emissive=None, alpha_mode=None):
        index = len(materials)
        material = {
            "name": name,
            "pbrMetallicRoughness": {
                "baseColorFactor": base_color,
                "metallicFactor": metallic,
                "roughnessFactor": roughness,
            },
        }
        if alpha_mode is not None:
            # "BLEND"を指定したマテリアルはパッカーがIsTransparent=trueで出力し、
            # エンジン側で半透明フォワードパス(Transparent.hlsl)へ回る
            material["alphaMode"] = alpha_mode
        if emissive is not None:
            # G-BufferのエミッシブバッファがR8G8B8A8_UNorm(KurenaiEngine3D.cpp)で[0,1]に
            # クランプされるため、1.0を超える値は指定しない。ここでのエミッシブは室内を照らす
            # 主光源ではなく、反射位置を読み取るための目印として使う
            material["emissiveFactor"] = emissive
        materials.append(material)
        return index

    def add_quad(name, corners, normal, material_index):
        geometry = generate_quad_double_sided(corners, normal)
        mesh_index = add_mesh_from_geometry(name, *geometry, material_index)
        nodes.append({"name": name, "mesh": mesh_index})
        return len(nodes) - 1

    # ==== マテリアル ====
    # 床は磨いた石(非金属・低ラフネス)にする。metallic=1.0にすると拡散反射が消えて薄暗い室内では
    # ほぼ真っ黒に沈み、映り込みの位置を読み取る土台が見えなくなる。非金属ならF0=0.04と反射は
    # 弱いが、ホールを見通す浅い角度ではフレネルで十分に強く出るため、視差補正の確認には
    # こちらのほうが読みやすい
    floor_material = add_material("FloorPolished", 0.06, 0.0, [0.52, 0.52, 0.55, 1.0])
    # 室ごとに壁のベースカラーを大きく変える。プローブに焼かれる環境が別物になり、
    # ブレンドの遷移が反射色の変化として読み取れるようにするため。
    # 拡散のIBLは遮蔽されない(スカイボックスの環境光が天井を無視して屋内にも届く)ので、
    # 彩度が低いと空の青に流されて2部屋の差が出ない。西室は暗めの高彩度にしてある
    west_wall_material = add_material("WestWall", 0.6, 0.0, [0.42, 0.13, 0.06, 1.0])
    west_ceiling_material = add_material("WestCeiling", 0.8, 0.0, [0.26, 0.10, 0.05, 1.0])
    east_wall_material = add_material("EastWall", 0.6, 0.0, [0.22, 0.38, 0.66, 1.0])
    partition_material = add_material("Partition", 0.6, 0.0, [0.36, 0.34, 0.33, 1.0])
    warm_stripe_material = add_material(
        "StripeWarm", 0.9, 0.0, [0.02, 0.01, 0.01, 1.0], emissive=[1.0, 0.45, 0.12])
    cool_stripe_material = add_material(
        "StripeCool", 0.9, 0.0, [0.01, 0.01, 0.02, 1.0], emissive=[0.15, 0.65, 1.0])
    sphere_material = add_material("SphereMetal", SPHERE_ROUGHNESS, 1.0, [0.95, 0.95, 0.95, 1.0])
    # 半透明のガラス板(19.11節)。半透明フォワードパスにはSSRが適用されないため、ガラスにとっては
    # 環境ソースが唯一の映り込みになる。ほぼ無色透明(alpha=0.18)にしてあるので、見えるものの
    # 大半は「反射している環境そのもの」になり、プローブの有無が最も素直に現れる。
    # 金属球と同じく西室・東室で同一マテリアルを使い、見た目の違いがプローブ由来だけになるようにする
    glass_material = add_material(
        "Glass", GLASS_ROUGHNESS, 0.0, [0.85, 0.90, 0.92, GLASS_ALPHA], alpha_mode="BLEND")

    x0, x1 = HALL_MIN_X, HALL_MAX_X
    z0, z1 = HALL_MIN_Z, HALL_MAX_Z
    h = HALL_HEIGHT

    # ==== 床(ホール全体) ====
    add_quad("Floor", [(x0, 0.0, z0), (x1, 0.0, z0), (x1, 0.0, z1), (x0, 0.0, z1)], (0.0, 1.0, 0.0), floor_material)

    # ==== 西室の天井。東室は開口(天井を張らない)ため太陽光と空が入る ====
    add_quad(
        "CeilingWest",
        [(x0, h, z0), (PARTITION_X, h, z0), (PARTITION_X, h, z1), (x0, h, z1)],
        (0.0, -1.0, 0.0), west_ceiling_material)

    # ==== 妻壁 ====
    add_quad("WallWestEnd", [(x0, 0.0, z0), (x0, 0.0, z1), (x0, h, z1), (x0, h, z0)], (1.0, 0.0, 0.0), west_wall_material)
    add_quad("WallEastEnd", [(x1, 0.0, z0), (x1, 0.0, z1), (x1, h, z1), (x1, h, z0)], (-1.0, 0.0, 0.0), east_wall_material)

    # ==== 側壁。室ごとに色が違うのでPARTITION_Xで分割する ====
    for label, wall_x0, wall_x1, material in (
        ("West", x0, PARTITION_X, west_wall_material),
        ("East", PARTITION_X, x1, east_wall_material),
    ):
        add_quad(
            f"WallNorth{label}",
            [(wall_x0, 0.0, z1), (wall_x1, 0.0, z1), (wall_x1, h, z1), (wall_x0, h, z1)],
            (0.0, 0.0, -1.0), material)
        add_quad(
            f"WallSouth{label}",
            [(wall_x0, 0.0, z0), (wall_x1, 0.0, z0), (wall_x1, h, z0), (wall_x0, h, z0)],
            (0.0, 0.0, 1.0), material)

    # ==== 仕切り壁。中央に開口部を残し、その上をまぐさで塞ぐ ====
    px = PARTITION_X
    d = DOORWAY_HALF_Z
    add_quad(
        "PartitionSouth",
        [(px, 0.0, z0), (px, 0.0, -d), (px, h, -d), (px, h, z0)],
        (1.0, 0.0, 0.0), partition_material)
    add_quad(
        "PartitionNorth",
        [(px, 0.0, d), (px, 0.0, z1), (px, h, z1), (px, h, d)],
        (1.0, 0.0, 0.0), partition_material)
    add_quad(
        "PartitionLintel",
        [(px, DOORWAY_HEIGHT, -d), (px, DOORWAY_HEIGHT, d), (px, h, d), (px, h, -d)],
        (1.0, 0.0, 0.0), partition_material)

    # ==== エミッシブ帯 ====
    # 壁面へ縦帯を貼る。鏡面の床に映ったときの「帯の反射が帯の真下に接地しているか」が
    # 視差補正の効きをそのまま表すため、床から離れた高さまで伸ばしておく
    sw = STRIPE_HALF_WIDTH
    sy0, sy1 = STRIPE_MIN_Y, STRIPE_MAX_Y
    eps = STRIPE_OFFSET

    def add_stripes_on_end_wall(label, wall_x, inward_x, z_centers, material):
        # 妻壁(x=一定)の帯。z方向に並べる
        x = wall_x + inward_x * eps
        for i, zc in enumerate(z_centers):
            corners = [(x, sy0, zc - sw), (x, sy0, zc + sw), (x, sy1, zc + sw), (x, sy1, zc - sw)]
            add_quad(f"Stripe{label}End{i}", corners, (inward_x, 0.0, 0.0), material)

    def add_stripes_on_side_wall(label, wall_z, inward_z, x_centers, material):
        # 側壁(z=一定)の帯。x方向に並べる
        z = wall_z + inward_z * eps
        for i, xc in enumerate(x_centers):
            corners = [(xc - sw, sy0, z), (xc + sw, sy0, z), (xc + sw, sy1, z), (xc - sw, sy1, z)]
            add_quad(f"Stripe{label}{i}", corners, (0.0, 0.0, inward_z), material)

    add_stripes_on_end_wall("Warm", x0, 1.0, [-3.5, 0.0, 3.5], warm_stripe_material)
    add_stripes_on_side_wall("WarmNorth", z1, -1.0, [-9.0, -4.0], warm_stripe_material)
    add_stripes_on_side_wall("WarmSouth", z0, 1.0, [-9.0, -4.0], warm_stripe_material)

    add_stripes_on_end_wall("Cool", x1, -1.0, [-3.5, 0.0, 3.5], cool_stripe_material)
    add_stripes_on_side_wall("CoolNorth", z1, -1.0, [4.0, 9.0], cool_stripe_material)
    add_stripes_on_side_wall("CoolSouth", z0, 1.0, [4.0, 9.0], cool_stripe_material)

    # ==== 金属球列 ====
    # 全球で同じジオメトリ・同じマテリアルを使い、位置だけをノードで変える。
    # 反射色の違いはプローブ由来のものだけになるので、ブレンドの遷移が素直に読める
    sphere_geometry = generate_sphere(LAT_SEGMENTS, LON_SEGMENTS, SPHERE_RADIUS)
    sphere_mesh = add_mesh_from_geometry("SphereMetal", *sphere_geometry, sphere_material)
    for i, x in enumerate(SPHERE_X):
        nodes.append({
            "name": f"Sphere_{i}",
            "mesh": sphere_mesh,
            # 床からわずかに浮かせる(接地させると鏡面床との交差部にちらつきが出る)
            "translation": [x, SPHERE_RADIUS + 0.1, 0.0],
        })

    # ==== 半透明のガラス板 ====
    # generate_quad_double_sidedが裏面も張るため、反対側から見ても消えない
    glass_yaw = math.radians(GLASS_YAW_DEG)
    glass_normal = (math.cos(glass_yaw), 0.0, math.sin(glass_yaw))
    # 板が伸びる向きは、法線とXZ平面上で直交する向き
    along_x = math.sin(glass_yaw) * GLASS_HALF_LENGTH
    along_z = -math.cos(glass_yaw) * GLASS_HALF_LENGTH
    for label, (gcx, gcz) in GLASS_CENTERS.items():
        add_quad(
            f"Glass{label}",
            [(gcx - along_x, GLASS_MIN_Y, gcz - along_z), (gcx + along_x, GLASS_MIN_Y, gcz + along_z),
             (gcx + along_x, GLASS_MAX_Y, gcz + along_z), (gcx - along_x, GLASS_MAX_Y, gcz - along_z)],
            glass_normal, glass_material)

    total_length = len(buffer_bytes)
    bin_path = os.path.join(OUT_DIR, BIN_NAME)
    with open(bin_path, "wb") as bin_file:
        bin_file.write(buffer_bytes)

    gltf = {
        "asset": {"version": "2.0", "generator": "KurenaiEngine ProbeTest generator"},
        "scene": 0,
        "scenes": [{"nodes": list(range(len(nodes)))}],
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

    print(f"nodes={len(nodes)} meshes={len(meshes)} materials={len(materials)} buffer_bytes={total_length}")
    print(f"hall = x[{x0}, {x1}] y[0, {h}] z[{z0}, {z1}], partition at x={PARTITION_X}")
    print(f"spheres={len(SPHERE_X)} (metallic=1.0, roughness={SPHERE_ROUGHNESS})")
    print(f"glass panes={len(GLASS_CENTERS)} (alphaMode=BLEND, alpha={GLASS_ALPHA}, roughness={GLASS_ROUGHNESS}, yaw={GLASS_YAW_DEG}deg)")
    print(f"wrote {gltf_path}")
    print(f"wrote {bin_path}")


if __name__ == "__main__":
    main()
