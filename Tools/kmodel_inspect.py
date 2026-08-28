#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""KurenaiEngineの .kmodel / .kgeom を読んで中身をJSONで吐く検査ツール。

KurenaiPacker.exe の --inspect が「assimpが読んだ直後のソースモデル(.fbx/.gltf)」を見るのに対し、
こちらは「パッカーが書き出した成果物」を見る。用途は次の3つ。

  1. フォーマットのバージョンを上げたときの回帰確認
     (頂点数と PackageHeader の AABB が変更前と一致するか)
  2. メッシュ単位AABBの検算 (全メッシュのAABBの和が PackageHeader のAABBと一致するか)
  3. メッシュレットがマテリアルを跨いでいないことの機械的な検査

**v9 と v10 の両方を読める。** バージョンを上げる作業では「上げる前の値」と「上げた後の値」を
同じ物差しで測る必要があり、片方しか読めないと比較そのものが成立しないため。

使い方:
    python Tools/kmodel_inspect.py <.kmodel> [...]                  # 人が読む要約を標準出力へ
    python Tools/kmodel_inspect.py --json out.json <.kmodel> [...]  # 機械可読なJSONへ
    python Tools/kmodel_inspect.py --check-meshlets <.kmodel>       # メッシュレットの検査も行う(重い)
    python Tools/kmodel_inspect.py --compare a.json b.json          # 2つのJSONを突き合わせる

--check-meshlets は .kgeom のメッシュレットブロックを全部展開して三角形を1つずつ辿るため、
100万三角形級のモデルでは数十秒かかる。既定では行わない。
"""

import argparse
import glob
import json
import os
import struct
import sys

# === フォーマット定義 (KurenaiEngine/Source/Library/Assets/ModelPackage.h と一致させること) ===

PACKAGE_MAGIC = b"KMDL"
GEOMETRY_MAGIC = b"KGEO"

# PackageHeader。v9は64バイト、v10は MaterialCount と Reserved が増えて72バイト。
# **BoundsMin のオフセット16はどちらも同じ**(先頭だけ読んでAABBを取る用途があるため動かさない)。
HEADER_V9 = struct.Struct("<4sIII3f3fIIIIII")
HEADER_V10 = struct.Struct("<4sIII3f3fIIIIIIII")

TEXTURE_ENTRY = struct.Struct("<IIII")            # 16バイト (v9/v10 共通)
LIGHT_ENTRY = struct.Struct("<I3f3f3fffffIIII")   # 72バイト (v9/v10 共通)

MESH_ENTRY_V9 = struct.Struct("<QQIIfff3fiiiiIf4fifiIQQQIIII")    # 144バイト
MESH_ENTRY_V10 = struct.Struct("<QQII3f3fiIQQQIIII4I4I")          # 128バイト
MATERIAL_ENTRY_V10 = struct.Struct("<fff3f4fffiiiiiiII")          # 80バイト

MESHLET_ENTRY_V9 = struct.Struct("<IIII3ff3ff")    # 48バイト
MESHLET_ENTRY_V10 = struct.Struct("<IIII3ff3ffII2I")  # 64バイト

GEOMETRY_HEADER = struct.Struct("<4sIIIQQ")        # 32バイト

VERTEX_STRIDE = 56          # Assets::Vertex。Position は先頭12バイト
MAX_MESHLET_LOD = 4         # ModelPackage.h の kMaxMeshletLODCount


class KModelError(Exception):
    """.kmodel / .kgeom を読めなかったときに投げる。呼び出し側がファイル名を添えて報告する。"""


def _unpack_header(buf):
    """先頭を読んで (辞書, ヘッダサイズ) を返す。バージョンでレイアウトが違うので先に版を見る。"""
    if len(buf) < HEADER_V9.size:
        raise KModelError("ファイルが小さすぎてヘッダを読めません")
    magic, version = struct.unpack_from("<4sI", buf, 0)
    if magic != PACKAGE_MAGIC:
        raise KModelError("マジックが KMDL ではありません: %r" % (magic,))

    if version <= 9:
        if len(buf) < HEADER_V9.size:
            raise KModelError("v%d のヘッダ(64バイト)に足りません" % version)
        f = HEADER_V9.unpack_from(buf, 0)
        header = {
            "Version": f[1], "VertexStride": f[2], "IndexStride": f[3],
            "BoundsMin": list(f[4:7]), "BoundsMax": list(f[7:10]),
            "MeshCount": f[10], "MaterialCount": None, "TextureCount": f[11],
            "LightCount": f[12], "GeometryPathOffset": f[13],
            "GeometryPathLength": f[14], "StringPoolSize": f[15],
        }
        return header, HEADER_V9.size

    if len(buf) < HEADER_V10.size:
        raise KModelError("v%d のヘッダ(72バイト)に足りません" % version)
    f = HEADER_V10.unpack_from(buf, 0)
    header = {
        "Version": f[1], "VertexStride": f[2], "IndexStride": f[3],
        "BoundsMin": list(f[4:7]), "BoundsMax": list(f[7:10]),
        "MeshCount": f[10], "MaterialCount": f[11], "TextureCount": f[12],
        "LightCount": f[13], "GeometryPathOffset": f[14],
        "GeometryPathLength": f[15], "StringPoolSize": f[16],
    }
    return header, HEADER_V10.size


def _unpack_mesh_v9(buf, offset):
    f = MESH_ENTRY_V9.unpack_from(buf, offset)
    return {
        "VertexOffset": f[0], "IndexOffset": f[1],
        "VertexCount": f[2], "IndexCount": f[3],
        # v9 はメッシュ単位AABBを持たない。持っていないことを None で明示する
        "BoundsMin": None, "BoundsMax": None,
        # v9 はマテリアルテーブルを持たず、材質は MeshEntry に直接入っている。
        # mesh↔material は 1:1 なので、比較のためメッシュ番号をそのまま材質番号とみなす
        "MaterialIndex": None,
        "MeshletOffset": f[24], "MeshletVertexOffset": f[25], "MeshletTriangleOffset": f[26],
        "MeshletCount": f[27], "MeshletVertexCount": f[28], "MeshletTriangleCount": f[29],
        "MeshletLODCount": 1 if f[27] > 0 else 0,
        "MeshletLODOffsets": [0, 0, 0, 0],
        "MeshletLODCounts": [f[27], 0, 0, 0],
        "Material": {
            "MetallicFactor": f[4], "RoughnessFactor": f[5], "AlphaCutoff": f[6],
            "EmissiveFactor": list(f[7:10]),
            "BaseColorTextureIndex": f[10], "NormalTextureIndex": f[11],
            "MetallicRoughnessTextureIndex": f[12], "EmissiveTextureIndex": f[13],
            "Flags": f[14], "Translucency": f[15],
            "BaseColorFactor": list(f[16:20]),
            "OcclusionTextureIndex": f[20], "OcclusionStrength": f[21],
            "BentNormalTextureIndex": f[22],
        },
    }


def _unpack_mesh_v10(buf, offset):
    f = MESH_ENTRY_V10.unpack_from(buf, offset)
    return {
        "VertexOffset": f[0], "IndexOffset": f[1],
        "VertexCount": f[2], "IndexCount": f[3],
        "BoundsMin": list(f[4:7]), "BoundsMax": list(f[7:10]),
        "MaterialIndex": f[10],
        "MeshletOffset": f[12], "MeshletVertexOffset": f[13], "MeshletTriangleOffset": f[14],
        "MeshletCount": f[15], "MeshletVertexCount": f[16], "MeshletTriangleCount": f[17],
        "MeshletLODCount": f[18],
        "MeshletLODOffsets": list(f[19:23]),
        "MeshletLODCounts": list(f[23:27]),
        "Material": None,   # v10 は MaterialEntry 配列側にある
    }


def _unpack_material_v10(buf, offset):
    f = MATERIAL_ENTRY_V10.unpack_from(buf, offset)
    return {
        "MetallicFactor": f[0], "RoughnessFactor": f[1], "AlphaCutoff": f[2],
        "EmissiveFactor": list(f[3:6]), "BaseColorFactor": list(f[6:10]),
        "OcclusionStrength": f[10], "Translucency": f[11],
        "BaseColorTextureIndex": f[12], "NormalTextureIndex": f[13],
        "MetallicRoughnessTextureIndex": f[14], "EmissiveTextureIndex": f[15],
        "OcclusionTextureIndex": f[16], "BentNormalTextureIndex": f[17],
        "Flags": f[18],
    }


def read_kmodel(path):
    """.kmodel を読んで辞書で返す。.kgeom はまだ開かない(--check-meshlets のときだけ開く)。"""
    try:
        with open(path, "rb") as fp:
            buf = fp.read()
    except OSError as e:
        raise KModelError("開けません: %s" % e)

    header, header_size = _unpack_header(buf)
    version = header["Version"]
    cursor = header_size

    # [TextureEntry × TextureCount]
    textures = []
    for _ in range(header["TextureCount"]):
        if cursor + TEXTURE_ENTRY.size > len(buf):
            raise KModelError("TextureEntry の読み出しがファイル末尾を超えました")
        t = TEXTURE_ENTRY.unpack_from(buf, cursor)
        textures.append({"PathOffset": t[0], "PathLength": t[1], "Flags": t[2]})
        cursor += TEXTURE_ENTRY.size

    # [MaterialEntry × MaterialCount] (v10 から。テクスチャ番号を参照するのでテクスチャの後ろ)
    materials = []
    if version >= 10:
        for _ in range(header["MaterialCount"]):
            if cursor + MATERIAL_ENTRY_V10.size > len(buf):
                raise KModelError("MaterialEntry の読み出しがファイル末尾を超えました")
            materials.append(_unpack_material_v10(buf, cursor))
            cursor += MATERIAL_ENTRY_V10.size

    # [MeshEntry × MeshCount]
    mesh_struct = MESH_ENTRY_V10 if version >= 10 else MESH_ENTRY_V9
    unpack_mesh = _unpack_mesh_v10 if version >= 10 else _unpack_mesh_v9
    meshes = []
    for i in range(header["MeshCount"]):
        if cursor + mesh_struct.size > len(buf):
            raise KModelError("MeshEntry の読み出しがファイル末尾を超えました")
        mesh = unpack_mesh(buf, cursor)
        if version < 10:
            # v9 は 1:1 なのでメッシュ番号を材質番号として扱う(v10 との比較用)
            mesh["MaterialIndex"] = i
            materials.append(mesh.pop("Material"))
        else:
            mesh.pop("Material", None)
        meshes.append(mesh)
        cursor += mesh_struct.size

    # [LightEntry × LightCount]
    lights = []
    for _ in range(header["LightCount"]):
        if cursor + LIGHT_ENTRY.size > len(buf):
            raise KModelError("LightEntry の読み出しがファイル末尾を超えました")
        l = LIGHT_ENTRY.unpack_from(buf, cursor)
        lights.append({"Type": l[0], "Position": list(l[1:4]), "Intensity": l[10]})
        cursor += LIGHT_ENTRY.size

    # [StringPool]
    pool = buf[cursor:cursor + header["StringPoolSize"]]
    if len(pool) < header["StringPoolSize"]:
        raise KModelError("StringPool の読み出しがファイル末尾を超えました")

    def pool_str(offset, length):
        return pool[offset:offset + length].decode("utf-8", errors="replace")

    geometry_path = pool_str(header["GeometryPathOffset"], header["GeometryPathLength"])
    for t in textures:
        t["Path"] = pool_str(t["PathOffset"], t["PathLength"])

    trailing = len(buf) - (cursor + header["StringPoolSize"])

    return {
        "File": os.path.abspath(path),
        "FileSize": len(buf),
        "TrailingBytes": trailing,      # 0 以外なら未知のブロックが末尾に付いている
        "Header": header,
        "GeometryPath": geometry_path,
        "Textures": textures,
        "Materials": materials,
        "Meshes": meshes,
        "Lights": lights,
    }


def read_geometry_payload(kmodel_path, geometry_path):
    """.kgeom を開いてヘッダとペイロード(bytes)を返す。"""
    full = os.path.join(os.path.dirname(os.path.abspath(kmodel_path)),
                        geometry_path.replace("/", os.sep))
    try:
        with open(full, "rb") as fp:
            raw = fp.read()
    except OSError as e:
        raise KModelError(".kgeom を開けません(%s): %s" % (full, e))
    if len(raw) < GEOMETRY_HEADER.size:
        raise KModelError(".kgeom が小さすぎます: %s" % full)
    magic, version, vstride, istride, payload_size, _ = GEOMETRY_HEADER.unpack_from(raw, 0)
    if magic != GEOMETRY_MAGIC:
        raise KModelError(".kgeom のマジックが KGEO ではありません: %r" % (magic,))
    payload = raw[GEOMETRY_HEADER.size:GEOMETRY_HEADER.size + payload_size]
    if len(payload) < payload_size:
        raise KModelError(".kgeom のペイロードが宣言より短いです: %s" % full)
    return {"Version": version, "VertexStride": vstride, "IndexStride": istride}, payload


def _aabb_union(dst, src_min, src_max):
    if dst is None:
        return [list(src_min), list(src_max)]
    for i in range(3):
        dst[0][i] = min(dst[0][i], src_min[i])
        dst[1][i] = max(dst[1][i], src_max[i])
    return dst


def check_meshlets(model):
    """メッシュレットを全部展開し、材質を跨いでいないか / 段ごとの三角形数とAABBを調べる。

    材質跨ぎの検出のしかた: メッシュレットの頂点は「所属メッシュの頂点ブロック内の番号」でなければ
    ならない。メッシュはマテリアル単位でマージされているので、番号がメッシュの VertexCount を
    はみ出したら、それは別マテリアルの頂点を掴んでいることを意味する。三角形1つずつ、
    2段の間接参照(三角形→ローカル頂点番号→グローバル頂点番号)を実際に辿って確かめる。
    """
    version = model["Header"]["Version"]
    meshlet_struct = MESHLET_ENTRY_V10 if version >= 10 else MESHLET_ENTRY_V9
    geom_header, payload = read_geometry_payload(model["File"], model["GeometryPath"])

    report = {"GeometryVersion": geom_header["Version"], "Meshes": [],
              "CrossMaterialMeshlets": [], "OutOfRange": []}

    for mesh_index, mesh in enumerate(model["Meshes"]):
        total = mesh["MeshletCount"]
        if total == 0:
            report["Meshes"].append({"MeshIndex": mesh_index, "MeshletCount": 0, "LODs": []})
            continue

        entries = []
        for m in range(total):
            off = mesh["MeshletOffset"] + m * meshlet_struct.size
            if off + meshlet_struct.size > len(payload):
                raise KModelError("メッシュ%d のメッシュレット%d がペイロード外です" % (mesh_index, m))
            f = meshlet_struct.unpack_from(payload, off)
            e = {"VertexOffset": f[0], "TriangleOffset": f[1],
                 "VertexCount": f[2], "TriangleCount": f[3],
                 "MaterialIndex": f[12] if version >= 10 else None,
                 "LODLevel": f[13] if version >= 10 else 0}
            entries.append(e)

        # 頂点位置(段ごとのAABB用)
        positions = []
        for v in range(mesh["VertexCount"]):
            po = mesh["VertexOffset"] + v * VERTEX_STRIDE
            positions.append(struct.unpack_from("<3f", payload, po))

        lod_count = max(1, mesh["MeshletLODCount"])
        lods = []
        for lod in range(lod_count):
            start = mesh["MeshletLODOffsets"][lod] if version >= 10 else 0
            count = mesh["MeshletLODCounts"][lod] if version >= 10 else total
            if count == 0:
                continue
            tri_total = 0
            bounds = None
            for m in range(start, start + count):
                e = entries[m]
                tri_total += e["TriangleCount"]
                if version >= 10 and e["LODLevel"] != lod:
                    report["OutOfRange"].append(
                        "メッシュ%d メッシュレット%d の LODLevel=%d が段%d の範囲に入っています"
                        % (mesh_index, m, e["LODLevel"], lod))

                # 【v10 は番号を直接突き合わせられる】MeshletEntry 自身が MaterialIndex を持つので、
                # 所属メッシュのものと一致するかを見ればよい。
                # 下の頂点番号による検出は間接的で、
                # **MaterialIndex だけが壊れているケースを見逃す**。
                # ランタイム側(ModelLoader.cpp の meshletMaterialMismatch)はこの番号を見ているので、
                # 検査ツールが見逃すと「ツールは通るが起動するとエラーログが出る」ことになる
                if version >= 10 and e["MaterialIndex"] != mesh["MaterialIndex"]:
                    report["CrossMaterialMeshlets"].append(
                        "メッシュ%d(材質%s) メッシュレット%d: MaterialIndex=%s が所属メッシュの材質と違います"
                        % (mesh_index, mesh["MaterialIndex"], m, e["MaterialIndex"]))
                for t in range(e["TriangleCount"]):
                    to = mesh["MeshletTriangleOffset"] + (e["TriangleOffset"] + t) * 4
                    packed = struct.unpack_from("<I", payload, to)[0]
                    for local in (packed & 0xFF, (packed >> 8) & 0xFF, (packed >> 16) & 0xFF):
                        if local >= e["VertexCount"]:
                            report["OutOfRange"].append(
                                "メッシュ%d メッシュレット%d: ローカル頂点番号%d が VertexCount=%d を超えています"
                                % (mesh_index, m, local, e["VertexCount"]))
                            continue
                        vo = mesh["MeshletVertexOffset"] + (e["VertexOffset"] + local) * 4
                        gv = struct.unpack_from("<I", payload, vo)[0]
                        if gv >= mesh["VertexCount"]:
                            # メッシュの頂点ブロックの外 = 別マテリアルの頂点を掴んでいる
                            report["CrossMaterialMeshlets"].append(
                                "メッシュ%d(材質%s) メッシュレット%d: グローバル頂点番号%d が "
                                "このメッシュの VertexCount=%d を超えています"
                                % (mesh_index, mesh["MaterialIndex"], m, gv, mesh["VertexCount"]))
                            continue
                        p = positions[gv]
                        bounds = _aabb_union(bounds, p, p)
            lods.append({"LOD": lod, "MeshletCount": count, "TriangleCount": tri_total,
                         "BoundsMin": bounds[0] if bounds else None,
                         "BoundsMax": bounds[1] if bounds else None})

        report["Meshes"].append({
            "MeshIndex": mesh_index, "MaterialIndex": mesh["MaterialIndex"],
            "MeshletCount": total, "LODs": lods,
        })

    return report


def summarize(model, meshlet_report=None):
    """人が読む要約と、検算の結果を返す。"""
    h = model["Header"]
    total_vertices = sum(m["VertexCount"] for m in model["Meshes"])
    total_indices = sum(m["IndexCount"] for m in model["Meshes"])

    # メッシュ単位AABBの和が PackageHeader のAABBと一致するか(v10のみ。別々に作った値なので非自明)
    union = None
    for m in model["Meshes"]:
        if m["BoundsMin"] is None:
            union = None
            break
        union = _aabb_union(union, m["BoundsMin"], m["BoundsMax"])

    summary = {
        "File": model["File"],
        "Version": h["Version"],
        "MeshCount": h["MeshCount"],
        "MaterialCount": h["MaterialCount"],
        "TextureCount": h["TextureCount"],
        "LightCount": h["LightCount"],
        "TotalVertexCount": total_vertices,
        "TotalIndexCount": total_indices,
        "BoundsMin": h["BoundsMin"],
        "BoundsMax": h["BoundsMax"],
        "MeshBoundsUnionMin": union[0] if union else None,
        "MeshBoundsUnionMax": union[1] if union else None,
        "TrailingBytes": model["TrailingBytes"],
        "PerMesh": [{"VertexCount": m["VertexCount"], "IndexCount": m["IndexCount"],
                     "MaterialIndex": m["MaterialIndex"],
                     "MeshletCount": m["MeshletCount"],
                     "MeshletLODCounts": m["MeshletLODCounts"]}
                    for m in model["Meshes"]],
    }
    if meshlet_report is not None:
        summary["MeshletReport"] = meshlet_report
    return summary


def compare(baseline, current):
    """2つの検査結果(ファイル名 -> 要約 の辞書)を突き合わせ、差分の一覧を返す。

    比べるのは **頂点数・インデックス数・PackageHeader の AABB** の3つ。
    フォーマットのバージョンを上げても幾何が動いていないことを示すための比較なので、
    材質やテクスチャ数は対象にしない(そちらは意図して変わりうる)。
    """
    diffs = []
    for key in sorted(set(baseline) | set(current)):
        if key not in baseline:
            diffs.append("%s: 変更前に存在しません" % key)
            continue
        if key not in current:
            diffs.append("%s: 変更後に存在しません" % key)
            continue
        a, b = baseline[key], current[key]
        if a["TotalVertexCount"] != b["TotalVertexCount"]:
            diffs.append("%s: 頂点数 %d -> %d" % (key, a["TotalVertexCount"], b["TotalVertexCount"]))
        if a["TotalIndexCount"] != b["TotalIndexCount"]:
            diffs.append("%s: インデックス数 %d -> %d" % (key, a["TotalIndexCount"], b["TotalIndexCount"]))
        if a["MeshCount"] != b["MeshCount"]:
            diffs.append("%s: メッシュ数 %d -> %d" % (key, a["MeshCount"], b["MeshCount"]))
        for axis, name in enumerate("XYZ"):
            # AABB はビット単位で一致することを求める(丸めを許すと「だいたい同じ」で通ってしまう)
            if a["BoundsMin"][axis] != b["BoundsMin"][axis]:
                diffs.append("%s: BoundsMin.%s %r -> %r" % (key, name, a["BoundsMin"][axis], b["BoundsMin"][axis]))
            if a["BoundsMax"][axis] != b["BoundsMax"][axis]:
                diffs.append("%s: BoundsMax.%s %r -> %r" % (key, name, a["BoundsMax"][axis], b["BoundsMax"][axis]))
        if len(a["PerMesh"]) == len(b["PerMesh"]):
            for i, (ma, mb) in enumerate(zip(a["PerMesh"], b["PerMesh"])):
                if ma["VertexCount"] != mb["VertexCount"]:
                    diffs.append("%s: メッシュ%d の頂点数 %d -> %d"
                                 % (key, i, ma["VertexCount"], mb["VertexCount"]))
                if ma["IndexCount"] != mb["IndexCount"]:
                    diffs.append("%s: メッシュ%d のインデックス数 %d -> %d"
                                 % (key, i, ma["IndexCount"], mb["IndexCount"]))
    return diffs


def main(argv):
    parser = argparse.ArgumentParser(description=".kmodel / .kgeom の中身を検査する")
    parser.add_argument("paths", nargs="+", help=".kmodel のパス(グロブ可)。--compare のときはJSON 2つ")
    parser.add_argument("--json", dest="json_out", help="検査結果をこのJSONへ書き出す")
    parser.add_argument("--check-meshlets", action="store_true",
                        help="メッシュレットを全部展開して材質跨ぎと段を検査する(重い)")
    parser.add_argument("--compare", action="store_true",
                        help="paths に与えた2つのJSONを突き合わせる")
    parser.add_argument("--root", help="JSONのキーをこのディレクトリからの相対パスにする")
    args = parser.parse_args(argv)

    if args.compare:
        if len(args.paths) != 2:
            print("[ERROR] --compare には比較する2つのJSONを渡してください", file=sys.stderr)
            return 1
        try:
            with open(args.paths[0], "r", encoding="utf-8") as fp:
                baseline = json.load(fp)
            with open(args.paths[1], "r", encoding="utf-8") as fp:
                current = json.load(fp)
        except (OSError, ValueError) as e:
            print("[ERROR] JSONを読めません: %s" % e, file=sys.stderr)
            return 1
        diffs = compare(baseline, current)
        if diffs:
            print("[NG] %d件の差分があります" % len(diffs))
            for d in diffs:
                print("  " + d)
            return 1
        print("[OK] %d件のモデルで、頂点数・インデックス数・AABB が完全に一致しました" % len(baseline))
        return 0

    targets = []
    for p in args.paths:
        matched = sorted(glob.glob(p, recursive=True))
        if not matched:
            print("[WARN] 一致するファイルがありません: %s" % p, file=sys.stderr)
        targets.extend(matched)
    if not targets:
        print("[ERROR] 検査するファイルがありません", file=sys.stderr)
        return 1

    results = {}
    failures = 0
    for path in targets:
        try:
            model = read_kmodel(path)
            report = check_meshlets(model) if args.check_meshlets else None
        except KModelError as e:
            print("[ERROR] %s: %s" % (path, e), file=sys.stderr)
            failures += 1
            continue
        summary = summarize(model, report)
        key = os.path.relpath(path, args.root).replace("\\", "/") if args.root else os.path.basename(path)
        results[key] = summary

        if not args.json_out:
            print("%s" % key)
            print("  v%d  メッシュ %d / 材質 %s / テクスチャ %d / ライト %d"
                  % (summary["Version"], summary["MeshCount"],
                     summary["MaterialCount"], summary["TextureCount"], summary["LightCount"]))
            print("  頂点 %d / インデックス %d" % (summary["TotalVertexCount"], summary["TotalIndexCount"]))
            print("  AABB min %s max %s" % (summary["BoundsMin"], summary["BoundsMax"]))
            if summary["MeshBoundsUnionMin"] is not None:
                same = (summary["MeshBoundsUnionMin"] == summary["BoundsMin"]
                        and summary["MeshBoundsUnionMax"] == summary["BoundsMax"])
                print("  メッシュAABBの和: %s (ヘッダと%s)"
                      % (summary["MeshBoundsUnionMin"], "一致" if same else "**不一致**"))
            if summary["TrailingBytes"] != 0:
                print("  [WARN] 末尾に %d バイトの未知のデータがあります" % summary["TrailingBytes"])
            if report is not None:
                cross = len(report["CrossMaterialMeshlets"])
                oor = len(report["OutOfRange"])
                print("  メッシュレット: 材質跨ぎ %d件 / 範囲外 %d件 (.kgeom v%d)"
                      % (cross, oor, report["GeometryVersion"]))
                for line in report["CrossMaterialMeshlets"][:10]:
                    print("    [NG] " + line)
                for line in report["OutOfRange"][:10]:
                    print("    [NG] " + line)
                if cross or oor:
                    failures += 1

    if args.json_out:
        try:
            os.makedirs(os.path.dirname(os.path.abspath(args.json_out)), exist_ok=True)
            with open(args.json_out, "w", encoding="utf-8") as fp:
                json.dump(results, fp, ensure_ascii=False, indent=1, sort_keys=True)
        except OSError as e:
            print("[ERROR] JSONを書けません: %s" % e, file=sys.stderr)
            return 1
        print("[OK] %d件を %s へ書き出しました" % (len(results), args.json_out))

    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
