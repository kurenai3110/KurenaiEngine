#!/usr/bin/env python3
"""エミッシブ光源の単位変換(πと4と露出)を検算するための glTF と .kscene を生成する。

【何を確かめるためのものか】
自発光メッシュから起こす光源プロキシの明るさは

    I(θ) = L * A * [ (1-κ)/4 + κ * max(0, cosθ) ]

で決まる。この式には π と 4 と露出が絡んでいて、**どれか1つを取り違えても
0.25〜3.14倍の範囲に収まってしまう**。その帯域は「絵作りの好み」と区別がつかず、
自発光の強度倍率を上げて辻褄を合わせれば永久に隠れる。既存シーンでは判定できない。

【どう確かめるか】同じ .kmodel を使う2つの .kscene を出す。

    EmissiveLightProxy.kscene : 自発光の球だけ。-emissivelights 1 で走らせる
    EmissiveLightRef.kscene   : 同じ球 + **等価な手置きポイントライト**。
                                -emissivelights 0 で走らせる

球そのものの見た目(G-Bufferの自発光)は**両方で完全に同じ**なので、
床の明るさの差だけが単位変換の誤りを表す。ジオメトリもカメラも露出も同一。

【なぜ閉じた球なのか】κ=0 になり、遠方場が等方になる ―― つまり**点光源で厳密に置き換えられる**。
平らなパネル(κ=1)は cosθ のローブを持つので、点光源では軸上でしか一致しない。
π と 4 を切り分けたいだけなら、置き換えが厳密に成り立つ形を使うほうがよい。

【残る系統差を承知しておく】プロキシの分母は d^2 + R_eff^2、点光源は d^2 なので、
測定距離 d では 1 + (R_eff/d)^2 だけプロキシが暗くなる。
このスクリプトはその値を印字する。**測定距離はこの差が量子化より小さくなるよう選ぶこと。**

【DX11 で走らせること】プロキシは MegaLights 経路でだけ影レイを撃つ。DX12+DXR で走らせると、
球の中心から出た影レイが球自身に当たって全部遮蔽される(参照側の点光源には起きない)。

使い方:
    python Tools/generate_emissive_light_test.py                 # glTF を書く
    <KurenaiPacker で Assets/Packed/EmissiveLightTest へ変換>
    python Tools/generate_emissive_light_test.py --emit-scene    # .kmodel を読んで .kscene を書く
"""

import argparse
import json
import math
import os
import struct
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_DIR = os.path.join(SCRIPT_DIR, "..")
OUT_DIR = os.path.join(REPO_DIR, "Assets", "Source", "EmissiveLightTest")
SCENE_DIR = os.path.join(REPO_DIR, "Scenes")
NAME = "EmissiveLightTest"

# 発光体の半径[m]。**小さくすること** ―― プロキシの分母 d^2 + R_eff^2 と
# 点光源の d^2 の差は (R_eff/d)^2 で効くので、半径が大きいと測定距離を離さないといけない
EMITTER_RADIUS = 0.10
# 発光体の中心の高さ[m]。床までの距離がそのまま測定距離になる
EMITTER_HEIGHT = 3.0
# 床の半径[m]。プロキシの Range(既定のτで約5.6m)より広く取る
FLOOR_HALF_SIZE = 12.0
# 球の分割数。κ が 0 から離れないよう十分に細かくする(粗いと法線の和が消え残る)
LAT_SEGMENTS = 48
LON_SEGMENTS = 96

# .kscene に書き込む固定値。**両方のシーンで完全に同じにすること**
SCENE_EXPOSURE_EV100 = 15.0
CAMERA_POSITION = (0.0, 4.5, -7.0)
CAMERA_YAW = 0.0
CAMERA_PITCH = -28.0


def generate_sphere(lat_segments, lon_segments, radius):
    positions, normals, uvs, indices = [], [], [], []
    for lat in range(lat_segments + 1):
        theta = math.pi * lat / lat_segments
        for lon in range(lon_segments + 1):
            phi = 2.0 * math.pi * lon / lon_segments
            n = (math.sin(theta) * math.cos(phi), math.cos(theta), math.sin(theta) * math.sin(phi))
            positions.append((n[0] * radius, n[1] * radius, n[2] * radius))
            normals.append(n)
            uvs.append((lon / lon_segments, lat / lat_segments))
    for lat in range(lat_segments):
        for lon in range(lon_segments):
            a = lat * (lon_segments + 1) + lon
            b = a + lon_segments + 1
            indices.append((a, b, a + 1))
            indices.append((a + 1, b, b + 1))
    return positions, normals, uvs, indices


def generate_quad(corners, normal):
    # 【巻き順に注意】素直に (0,1,2),(0,2,3) と張ると2枚目が裏向きになり、床が対角線で
    # 半分だけ描かれる(generate_penumbra_test.py の同じ注記を参照)
    return list(corners), [normal] * 4, [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)], [(0, 2, 1), (0, 3, 2)]


def write_gltf():
    os.makedirs(OUT_DIR, exist_ok=True)
    buffer_bytes = bytearray()

    def append_aligned(data):
        while len(buffer_bytes) % 4 != 0:
            buffer_bytes.append(0)
        offset = len(buffer_bytes)
        buffer_bytes.extend(data)
        return offset

    accessors, buffer_views, meshes, materials, nodes = [], [], [], [], []

    def add_mesh(name, positions, normals, uvs, indices, material_index):
        pos_bytes = b"".join(struct.pack("<3f", *p) for p in positions)
        pos_offset = append_aligned(pos_bytes)
        nrm_bytes = b"".join(struct.pack("<3f", *n) for n in normals)
        nrm_offset = append_aligned(nrm_bytes)
        uv_bytes = b"".join(struct.pack("<2f", *uv) for uv in uvs)
        uv_offset = append_aligned(uv_bytes)
        idx_bytes = b"".join(struct.pack("<3I", *t) for t in indices)
        idx_offset = append_aligned(idx_bytes)

        pos_min = [min(p[a] for p in positions) for a in range(3)]
        pos_max = [max(p[a] for p in positions) for a in range(3)]

        bv = len(buffer_views)
        buffer_views.append({"buffer": 0, "byteOffset": pos_offset, "byteLength": len(pos_bytes), "target": 34962})
        buffer_views.append({"buffer": 0, "byteOffset": nrm_offset, "byteLength": len(nrm_bytes), "target": 34962})
        buffer_views.append({"buffer": 0, "byteOffset": uv_offset, "byteLength": len(uv_bytes), "target": 34962})
        buffer_views.append({"buffer": 0, "byteOffset": idx_offset, "byteLength": len(idx_bytes), "target": 34963})

        ac = len(accessors)
        accessors.append({"bufferView": bv, "byteOffset": 0, "componentType": 5126,
                          "count": len(positions), "type": "VEC3", "min": pos_min, "max": pos_max})
        accessors.append({"bufferView": bv + 1, "byteOffset": 0, "componentType": 5126,
                          "count": len(positions), "type": "VEC3"})
        accessors.append({"bufferView": bv + 2, "byteOffset": 0, "componentType": 5126,
                          "count": len(positions), "type": "VEC2"})
        accessors.append({"bufferView": bv + 3, "byteOffset": 0, "componentType": 5125,
                          "count": len(indices) * 3, "type": "SCALAR"})

        index = len(meshes)
        meshes.append({
            "name": name,
            "primitives": [{
                "attributes": {"POSITION": ac, "NORMAL": ac + 1, "TEXCOORD_0": ac + 2},
                "indices": ac + 3,
                "material": material_index,
            }],
        })
        return index

    def add_material(name, roughness, metallic, base_color, emissive=None):
        index = len(materials)
        m = {
            "name": name,
            "pbrMetallicRoughness": {
                "baseColorFactor": base_color, "metallicFactor": metallic, "roughnessFactor": roughness,
            },
        }
        if emissive is not None:
            m["emissiveFactor"] = emissive
        materials.append(m)
        return index

    # 床。**反射率を 0.5 に固定する** ―― 予測値を手で出すときに π で割るだけで済む。
    # 粗さ1.0で鏡面ハイライトを潰す(プロキシと参照で同じ L が出るので鏡面も一致するはずだが、
    # 一致の確認に鏡面の輝点を混ぜる理由が無い)
    floor_mat = add_material("Floor", 1.0, 0.0, [0.5, 0.5, 0.5, 1.0])
    f = FLOOR_HALF_SIZE
    floor_geo = generate_quad([(-f, 0.0, -f), (f, 0.0, -f), (f, 0.0, f), (-f, 0.0, f)], (0.0, 1.0, 0.0))
    nodes.append({"name": "Floor", "mesh": add_mesh("Floor", *floor_geo, floor_mat)})
    root = [len(nodes) - 1]

    # 発光体。**ベースカラーは黒**にする ―― 参照シーンでは同じ位置に点光源が入るので、
    # 球が自分の光を拡散で受けると両シーンで差が出てしまう
    emitter_mat = add_material("Emitter", 1.0, 0.0, [0.0, 0.0, 0.0, 1.0], emissive=[1.0, 1.0, 1.0])
    sphere_geo = generate_sphere(LAT_SEGMENTS, LON_SEGMENTS, EMITTER_RADIUS)
    nodes.append({"name": "Emitter", "mesh": add_mesh("Emitter", *sphere_geo, emitter_mat),
                  "translation": [0.0, EMITTER_HEIGHT, 0.0]})
    root.append(len(nodes) - 1)

    bin_path = os.path.join(OUT_DIR, NAME + ".bin")
    with open(bin_path, "wb") as fp:
        fp.write(buffer_bytes)

    gltf = {
        "asset": {"version": "2.0", "generator": "KurenaiEngine EmissiveLightTest generator"},
        "scene": 0,
        "scenes": [{"nodes": root}],
        "nodes": nodes, "meshes": meshes, "materials": materials,
        "accessors": accessors, "bufferViews": buffer_views,
        "buffers": [{"uri": NAME + ".bin", "byteLength": len(buffer_bytes)}],
    }
    gltf_path = os.path.join(OUT_DIR, NAME + ".gltf")
    with open(gltf_path, "w", encoding="utf-8") as fp:
        json.dump(gltf, fp, indent=2)

    print("発光体: 半径 %.3f m / 高さ %.2f m / 床までの距離 %.2f m"
          % (EMITTER_RADIUS, EMITTER_HEIGHT, EMITTER_HEIGHT))
    print("wrote %s" % gltf_path)
    print("wrote %s" % bin_path)
    print("")
    print("次に KurenaiPacker で Assets/Packed/EmissiveLightTest へ変換してから、")
    print("  python Tools/generate_emissive_light_test.py --emit-scene")


def emit_scenes(emissive_intensity, cutoff):
    """パック済みの .kmodel からプロキシの実効値を読み、等価な点光源を持つ .kscene を書く。

    【強度を上げられるようにしてある理由】IBLIntensity=0 にしても定数アンビエントへ
    フォールバックする(DeferredLighting.hlsl が「真っ暗にはしない」と明記)。既定の強度だと
    床はアンビエントで G≒36 になり、確かめたい光の寄与は 0.6階調しか乗らない。
    **その状態では単位が2倍ずれていても検出できない。**
    強度を上げて信号をアンビエントより十分上へ出し、参照側の点光源も同じ倍率で作る。

    打ち切り照度も同時に上げるのは、強度を上げると Range が伸びてシーンAABBの対角で
    クランプされ、参照側と食い違うのを避けるため(Range は強度の平方根で伸びる)。
    """
    sys.path.insert(0, SCRIPT_DIR)
    from kmodel_inspect import read_geometry_payload, read_kmodel
    from emissive_cluster_inspect import build_clusters, VERTEX_STRIDE

    model_path = os.path.join(REPO_DIR, "Assets", "Packed", "EmissiveLightTest", NAME + ".kmodel")
    if not os.path.exists(model_path):
        print("パックされた .kmodel がありません: %s" % model_path, file=sys.stderr)
        return 1
    model = read_kmodel(model_path)
    _, payload = read_geometry_payload(model_path, model["GeometryPath"])

    clusters = []
    for mesh in model["Meshes"]:
        mi = mesh["MaterialIndex"]
        if mi < 0 or mi >= len(model["Materials"]):
            continue
        emissive = model["Materials"][mi]["EmissiveFactor"]
        if max(emissive) <= 0.0:
            continue
        pos = [list(struct.unpack_from("<3f", payload, mesh["VertexOffset"] + v * VERTEX_STRIDE))
               for v in range(mesh["VertexCount"])]
        idx = list(struct.unpack_from("<%dI" % mesh["IndexCount"], payload, mesh["IndexOffset"]))
        for c in build_clusters(pos, idx, mesh["BoundsMin"], mesh["BoundsMax"], 1.0):
            c["Emissive"] = emissive
            clusters.append(c)

    if len(clusters) != 1:
        print("かたまりが1個になりませんでした(%d個)。球が分割されています" % len(clusters), file=sys.stderr)
        return 1
    c = clusters[0]

    # KurenaiEngine3D::MakeGPULightFromEmissiveProxy と同じ式で実効値を出す
    radiance = max(c["Emissive"]) * emissive_intensity
    color_range = radiance * c["Area"]     # = L * A * 強度倍率
    kappa = c["Directionality"]
    lobe_iso = (1.0 - kappa) * 0.25        # 等方成分。κ=0 ならこれが全部
    lobe_max = lobe_iso + kappa
    r_eff = c["SourceRadius"]
    solved = color_range * lobe_max / cutoff - r_eff * r_eff
    proxy_range = math.sqrt(solved) if solved > 0.0 else 0.0
    proxy_range = max(proxy_range, 2.0 * r_eff)

    # 等価な点光源。ColorRange_point = Intensity[cd] * exposure と一致させる
    exposure = 1.0 / (1.2 * (2.0 ** SCENE_EXPOSURE_EV100))
    intensity_cd = color_range * lobe_iso / exposure

    d = EMITTER_HEIGHT
    # プロキシ(分母 d^2+R^2)が点光源(分母 d^2)より暗くなる割合。
    # 【R^2/d^2 ではない】1 - d^2/(d^2+R^2) = R^2/(d^2+R^2) が厳密
    residual = (r_eff * r_eff) / (d * d + r_eff * r_eff)

    print("かたまり: 面積 %.6f m^2 / 半径 %.6f m / κ %.6f" % (c["Area"], r_eff, kappa))
    print("走らせるときの引数: -emissiveintensity %g -emissivelightscutoff %g"
          % (emissive_intensity, cutoff))
    print("プロキシ: ColorRange %.6f / 等方ローブ %.6f / Range %.4f m" % (color_range, lobe_iso, proxy_range))
    print("等価な点光源: Intensity %.3f cd / Range %.4f m (EV100=%.1f, exposure=%.6e)"
          % (intensity_cd, proxy_range, SCENE_EXPOSURE_EV100, exposure))
    print("測定距離 %.2f m での系統差(プロキシが暗い側): %.3f%%" % (d, 100.0 * residual))
    if kappa > 0.01:
        print("!! κ が %.4f と大きいため、点光源では厳密に置き換えられません" % kappa, file=sys.stderr)

    header = (
        "# エミッシブ光源の単位変換(π と 4 と露出)を検算するためのシーン。\n"
        "# Tools/generate_emissive_light_test.py で生成される(手で編集しない)。\n"
        "#\n"
        "# EmissiveLightProxy と EmissiveLightRef は**ジオメトリもカメラも露出も同一**で、\n"
        "# 床を照らす経路だけが違う。床の明るさに差が出たら単位変換が間違っている。\n"
        "#\n"
        "# 【両側に同じ -emissiveintensity を渡すこと】倍率は G-Buffer の自発光にも掛かるので、\n"
        "# 片側だけに渡すと球の見た目が変わり、球の位置にだけ大きな差が出る(実際に踏んだ)。\n"
        "# 【スクリーンスペースシャドウは切っておくこと】プロキシは常に影を落とす設定で、\n"
        "# 参照側の点光源は CastShadow=false。SSS を有効にすると片側だけ球の影が落ちる。\n"
        "#\n"
        "# 確認手順(**DX11 で走らせること**。DX12 だと影レイが球自身に当たる):\n"
        "#   Sample3D.exe -scene EmissiveLightProxy -emissivelights 1"
        " -emissiveintensity %g -emissivelightscutoff %g\n"
        "#   Sample3D.exe -scene EmissiveLightRef   -emissivelights 0\n"
        "# 床の同じ画素どうしを比べる。系統差は %.3f%%(プロキシの分母が d^2 + R^2 のぶん)。\n" % (emissive_intensity, cutoff, 100.0 * residual)
    )
    common = (
        "\n[Scene]\nName = %s\n"
        "# 露出を固定する。等価な点光源の強度が exposure に依存するため、これを外すと比較が壊れる\n"
        "Exposure = %.1f\n"
        "# 比較にノイズを混ぜない\n"
        "TAA = false\n"
        "AmbientOcclusion = false\n"
        "IBLIntensity = 0.0\n"
        "\n[Model]\nPath = EmissiveLightTest/EmissiveLightTest.kmodel\n"
        "\n[Camera]\nPosition = %.2f, %.2f, %.2f\nYaw = %.1f\nPitch = %.1f\n"
        "\n[Sun]\n"
        "# 太陽を切り、確かめたい光だけで照らす\n"
        "Enabled = false\nShadow = false\n"
        "# **昼に固定する。** 自動露出のバイアスが 0 でないと等価性が成り立たない ――\n"
        "# 手置きライトは exposure(Effective) を掛けたあと Tonemap の 2^(Effective-Scene) で\n"
        "# 相殺されるが、自発光とプロキシは相殺相手を持たずバイアスがそのまま残る\n"
        "# (docs/ImplementationDetail.md 62.4)\n"
        "TimeOfDay = 12.0\n"
    )

    scenes = {
        "EmissiveLightProxy": (header + "# こちらは自発光の球だけ。-emissivelights 1 で走らせる\n"
                               + common % ("Emissive Light Test (Proxy)", SCENE_EXPOSURE_EV100,
                                           CAMERA_POSITION[0], CAMERA_POSITION[1], CAMERA_POSITION[2],
                                           CAMERA_YAW, CAMERA_PITCH)),
        "EmissiveLightRef": (header + "# こちらは等価な手置きポイントライト。-emissivelights 0 で走らせる\n"
                             + common % ("Emissive Light Test (Reference)", SCENE_EXPOSURE_EV100,
                                         CAMERA_POSITION[0], CAMERA_POSITION[1], CAMERA_POSITION[2],
                                         CAMERA_YAW, CAMERA_PITCH)
                             + ("\n[Light]\n"
                                "# プロキシと等価になるよう生成した値。手で書き換えない\n"
                                "#   Intensity = L * A * (1-κ)/4 / exposure\n"
                                "#   Range     = プロキシが打ち切り照度から解いた値\n"
                                "Type = Point\n"
                                "Position = 0.0, %.4f, 0.0\n"
                                "Color = 1.0, 1.0, 1.0\n"
                                "Intensity = %.4f\n"
                                "Range = %.4f\n"
                                "# 影を出すと参照側だけ球に遮られる。両方とも影なしで比べる\n"
                                "CastShadow = false\n" % (EMITTER_HEIGHT, intensity_cd, proxy_range))),
    }

    os.makedirs(SCENE_DIR, exist_ok=True)
    for name, text in scenes.items():
        path = os.path.join(SCENE_DIR, name + ".kscene")
        with open(path, "w", encoding="utf-8", newline="\r\n") as fp:
            fp.write(text)
        print("wrote %s" % path)
    return 0


def main(argv):
    parser = argparse.ArgumentParser(description="エミッシブ光源の単位を検算するシーンを生成する")
    parser.add_argument("--emit-scene", action="store_true",
                        help="パック済みの .kmodel を読んで .kscene を書く(glTF は書かない)")
    parser.add_argument("--intensity", type=float, default=1.0,
                        help="自発光の強度倍率。参照側の点光源も同じ倍率で作る(既定 1.0)")
    parser.add_argument("--cutoff", type=float, default=1e-3,
                        help="打ち切り照度τ。強度を上げるときは一緒に上げる(既定 0.001)")
    args = parser.parse_args(argv[1:])
    if args.emit_scene:
        return emit_scenes(args.intensity, args.cutoff)
    write_gltf()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
