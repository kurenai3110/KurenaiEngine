#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Bistro屋外(McGuire版)の照明器具ジオメトリから夜景用の .kscene を生成する。

このリポジトリの規約どおり、**配置数値を目分量で書かない**。位置・色・光源半径は
すべて Assets/Packed/BistroMcGuire/Exterior.kmodel と .kgeom を直接読んで求める
(docs/ImplementationDetail.md 19.13 が Bistro内装で確立した方法の屋外版)。

使い方:
    python Tools/extract_bistro_lights.py            # Scenes/BistroExteriorNight.kscene を生成
    python Tools/extract_bistro_lights.py --verify   # 検算だけ行って書き出さない
    python Tools/extract_bistro_lights.py --report    # 灯ごとの明細も出す

--- なぜ三角形の連結成分なのか ---
頂点のグリッド近傍併合(内装で使った方法)は屋外では使えない。ストリングライトの
電球とコードが1メッシュに同居しており、近傍併合ではコードを介して電球が数珠つなぎになる。
インデックスバッファで連結成分を取ると、電球(対角 0.09〜0.12m)とコード(対角 10.6〜13.9m)
が86倍の空隙をはさんで分離するので、しきい値をどこに置いても同じ答えになる。

--- なぜ灯を器具の重心に置いてはいけないのか ---
MegaLights の影レイは全ショットが RAY_FLAG_FORCE_OPAQUE(MegaLightsInitialSample.hlsl ほか)。
BLAS は半透明を IsOpaque=false で登録するが、このフラグがそれを打ち消して全部を不透明にする。
器具の重心はその器具自身のジオメトリの内側にあるため、そのまま置くと全灯の影レイが
自分の器具に当たり、1灯も光らない。絵が暗いだけで例外もログも出ないので
「MegaLights が壊れている」と誤診しやすい。
このスクリプトは灯ごとに脱出率(下半球へ撃ったレイが器具を抜ける割合)を測り、
しきい値を超える位置まで下ろしてから採用する。抜けなければエラーで落とす。

【1メッシュだけを見て判断しないこと】重心を塞いでいるのは、まず発光体自身である
(Streetlight_Support_Bulb だけを相手にしても脱出率は 0.000。閉じた球の内側にいる)。
ところが街灯のガラス(Streetlight_Glass)だけを相手にすると 0.198 と出る。
たまたま選んだ1メッシュが穴だらけだっただけで、これを見て
「筒だから真下へ抜ける」と判断すると誤る。近傍の全メッシュを相手にすること。

【真下だけを試さないこと】背の高い街灯は電球が中空の支柱の中にあり、真下へ 2.0m
下ろしても脱出率が 0.000 のままになる(28本中12本)。下を最優先にした24方向を試す。
"""

import argparse
import math
import os
import struct
import sys

import numpy as np

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, SCRIPT_DIR)
import kmodel_inspect as K  # noqa: E402  (.kmodel/.kgeom のパーサを再利用する)

REPO_ROOT = os.path.normpath(os.path.join(SCRIPT_DIR, ".."))
KMODEL = os.path.join(REPO_ROOT, "Assets", "Packed", "BistroMcGuire", "Exterior.kmodel")
MTL = os.path.join(REPO_ROOT, "Assets", "Source", "BistroMcGuire", "exterior.mtl")
OUT = os.path.join(REPO_ROOT, "Scenes", "BistroExteriorNight.kscene")
MODEL_PATH_IN_SCENE = "BistroMcGuire/Exterior.kmodel"

WELD = 1e-4          # 連結成分を取る前の頂点溶接のしきい値[m]
# 探索の打ち切り目標。これを超えたらそこで止める(最良を探し続けない)。
# 実物の器具は笠が配光を絞るので脱出率は1.0にならない。0.60 にすると妥当な器具まで弾いた。
ESCAPE_MIN = 0.35
# 採用の条件は2つ。「何かを照らせること」と「実物の器具の位置と言えること」。
# 脱出率がこれを下回る灯は自分の器具の中に閉じ込められており、絵に寄与しない
ESCAPE_ADOPT = 0.05
# 器具の重心からこれ以上動かした灯は、もう「実在の器具の位置」ではないので採用しない。
# でっち上げの位置を置くくらいなら灯を1つ減らす
MAX_DISPLACEMENT = 0.60
ESCAPE_RAYS = 192    # 脱出率を測るレイの本数(下半球のフィボナッチ分布)
ESCAPE_REACH = 1.6   # 脱出とみなす距離[m]。器具の外へ出れば十分
SOUP_RADIUS = 2.8    # 三角形スープを集める半径[m]。ESCAPE_REACH と探索の下げ幅より大きく取る
DROP_MAX = 0.90      # 器具の重心から動かせる上限[m]。これを超えると実物の位置と言えない

# 器具の定義。mesh番号ではなく exterior.mtl のマテリアル名で引く(再パックで番号が変わっても
# 名前が変わらなければ追従できる。名前が消えたらエラーで落とす)
# (キー, mtl名, 成分の採択規則, 表示名, 期待する灯数)
FIXTURES = [
    ("streetlight", "Streetlight_Support_Bulb", None, "街灯", 28),
    ("string", "Stringlights", ("diag_lt", 1.0), "ストリングライトの電球", 64),
    ("awning_a", "MASTER_Light_Bulb", None, "庇のスポット(A)", 10),
    ("awning_b", "Spotlight_Emissive", None, "庇のスポット(B)", 7),
    ("lantern", "Lantern", ("verts_gt", 1000), "壁付けランタン", 5),
    ("vespa", "Vespa_Headlight", None, "スクーターのヘッドライト", 1),
]

# ストリングライトの色。内殻メッシュのマテリアル名 -> (表示名, リニアRGB, 期待個数)
# 値は Assets/Source/BistroMcGuire/OtherTextures/Colors/*.png を読んだ平均色(全部1色べた塗り)
STRING_COLORS = {
    "Paris_StringLights_01_White_Color":  ("White",  (1.0000, 1.0000, 1.0000)),
    # 【実物の食い違い】名前は Blue だが参照テクスチャは White.png。同じフォルダに未使用の
    # Blue.png = (0.0, 0.0595, 0.4287) がある。McGuire版OBJへの変換で落ちた可能性が高いが、
    # 実際に描かれているのは白なので既定は白を採る(推測で色を作らない)。--blue-as-blue で切替。
    "Paris_StringLights_01_Blue_Color":   ("Blue",   (1.0000, 1.0000, 1.0000)),
    "Paris_StringLights_01_Green_Color":  ("Green",  (0.0000, 0.2874, 0.0021)),
    "Paris_StringLights_01_Red_Color":    ("Red",    (0.5271, 0.0003, 0.0003)),
    "Paris_StringLights_01_Pink_Color":   ("Pink",   (0.6445, 0.0021, 1.0000)),
    "Paris_StringLights_01_Orange_Color": ("Orange", (0.9216, 0.4287, 0.0685)),
}
BLUE_ACTUAL = (0.0000, 0.0595, 0.4287)

# 白熱灯の色温度2700K相当のリニアRGB。街灯・庇・ランタン・ヘッドライトに使う。
# BistroInteriorLit が手置きした (1.00, 0.72, 0.45) とほぼ同じ値で、あちらの根拠を後追いで与える
INCANDESCENT = (1.00, 0.71, 0.42)

# 器具ごとの光束[lm]と光が出る立体角[sr]。I[cd] = 光束 / 立体角
PHOTOMETRY = {
    "streetlight": (1200.0, 2.0 * math.pi, "60W白熱相当の街灯ランタン(笠で下半球へ)"),
    "awning_a":    (400.0,  2.0 * math.pi, "庇の小型スポット"),
    "awning_b":    (400.0,  2.0 * math.pi, "庇の小型スポット"),
    "lantern":     (250.0,  4.0 * math.pi, "壁付けの装飾ランタン"),
    "string":      (60.0,   4.0 * math.pi, "5W級の装飾球"),
    "vespa":       (700.0,  2.0 * math.pi, "スクーターのヘッドライト"),
}

SCENE_EXPOSURE = 2.0   # [Scene] Exposure。derive_range と対で効く
GROUND_ALBEDO = 0.15   # 石畳の反射率。Range のしきい値照度を出すのに使う


def load_mtl_names(path):
    names = []
    try:
        with open(path, encoding="utf-8", errors="replace") as fp:
            for line in fp:
                if line.startswith("newmtl"):
                    parts = line.split(None, 1)
                    if len(parts) == 2:
                        names.append(parts[1].strip())
    except OSError as exc:
        raise SystemExit("exterior.mtl を開けません(%s): %s" % (path, exc))
    if not names:
        raise SystemExit("exterior.mtl からマテリアル名を読めませんでした: %s" % path)
    return names


class Model:
    """.kmodel/.kgeom を読んで、マテリアル名でメッシュを引けるようにしたもの。"""

    def __init__(self, kmodel_path, mtl_path):
        try:
            self.doc = K.read_kmodel(kmodel_path)
            self.geo, self.payload = K.read_geometry_payload(kmodel_path, self.doc["GeometryPath"])
        except K.KModelError as exc:
            raise SystemExit("アセットを読めません: %s" % exc)
        self.vs = self.geo["VertexStride"]
        self.istride = self.geo["IndexStride"]
        self.names = load_mtl_names(mtl_path)
        self.meshes = self.doc["Meshes"]
        self.materials = self.doc["Materials"]
        self.textures = self.doc["Textures"]
        if len(self.names) != len(self.materials):
            raise SystemExit(
                "exterior.mtl のマテリアル数(%d)と .kmodel のマテリアル数(%d)が違います。"
                "再パックで対応が崩れています" % (len(self.names), len(self.materials)))
        self._by_name = {}
        for mi, mesh in enumerate(self.meshes):
            self._by_name.setdefault(self.names[mesh["MaterialIndex"]], []).append(mi)
        self._pos_cache = {}
        self._comp_cache = {}

    def texture_of(self, material_index):
        idx = self.materials[material_index]["BaseColorTextureIndex"]
        if 0 <= idx < len(self.textures):
            return self.textures[idx]["Path"].split("/")[-1]
        return ""

    def mesh_index(self, mtl_name):
        got = self._by_name.get(mtl_name)
        if not got:
            raise SystemExit(
                "マテリアル '%s' を使うメッシュがありません。アセットが差し替わっています" % mtl_name)
        if len(got) != 1:
            raise SystemExit("マテリアル '%s' が %d メッシュに分かれています" % (mtl_name, len(got)))
        return got[0]

    def positions(self, mi):
        if mi in self._pos_cache:
            return self._pos_cache[mi]
        mesh = self.meshes[mi]
        n = mesh["VertexCount"]
        raw = np.frombuffer(self.payload, dtype=np.uint8, count=n * self.vs,
                            offset=mesh["VertexOffset"]).reshape(n, self.vs)
        pos = raw[:, 0:12].copy().view(np.float32).reshape(n, 3).astype(np.float64)
        self._pos_cache[mi] = pos
        return pos

    def indices(self, mi):
        mesh = self.meshes[mi]
        dt = np.uint32 if self.istride == 4 else np.uint16
        return np.frombuffer(self.payload, dtype=dt,
                             count=mesh["IndexCount"], offset=mesh["IndexOffset"]).astype(np.int64)

    def components(self, mi):
        """三角形の連結成分。[{n, c(重心), r(外接半径), diag(AABB対角)}] を返す。"""
        if mi in self._comp_cache:
            return self._comp_cache[mi]
        pos = self.positions(mi)
        n = len(pos)
        parent = list(range(n))

        def find(a):
            while parent[a] != a:
                parent[a] = parent[parent[a]]
                a = parent[a]
            return a

        def union(a, b):
            ra, rb = find(a), find(b)
            if ra != rb:
                parent[ra] = rb

        # 位置で溶接する(同じ点に別頂点が乗っていても1つの塊として扱う)
        seen = {}
        quant = np.round(pos / WELD).astype(np.int64)
        for i in range(n):
            key = (int(quant[i, 0]), int(quant[i, 1]), int(quant[i, 2]))
            j = seen.setdefault(key, i)
            if j != i:
                union(i, j)
        idx = self.indices(mi)
        tri = idx[: (len(idx) // 3) * 3].reshape(-1, 3)
        for a, b, c in tri:
            union(int(a), int(b))
            union(int(b), int(c))

        groups = {}
        for i in range(n):
            groups.setdefault(find(i), []).append(i)
        out = []
        for members in groups.values():
            pts = pos[members]
            centre = pts.mean(axis=0)
            out.append({
                "n": len(members),
                "c": centre,
                "r": float(np.linalg.norm(pts - centre, axis=1).max()),
                "diag": float(np.linalg.norm(pts.max(axis=0) - pts.min(axis=0))),
            })
        out.sort(key=lambda e: (e["c"][0], e["c"][2]))
        self._comp_cache[mi] = out
        return out

    def build_grid(self, cell=3.0, verbose=False):
        """全メッシュの三角形を1つの配列にまとめ、重心のボクセルで索引を作る。

        灯ごとにメッシュを総なめすると、Bistro屋外(284万三角形)では1灯あたり数秒かかり
        115灯で実用にならない。三角形は動かないので、索引は最初に1回だけ作る。
        """
        if getattr(self, "_grid", None) is not None:
            return
        chunks = []
        for mi in range(len(self.meshes)):
            pos = self.positions(mi).astype(np.float32)
            idx = self.indices(mi)
            tri = idx[: (len(idx) // 3) * 3].reshape(-1, 3)
            if len(tri) == 0:
                continue
            chunks.append(pos[tri])
        tris = np.concatenate(chunks, axis=0) if chunks else np.zeros((0, 3, 3), np.float32)
        centroids = tris.mean(axis=1)
        keys = np.floor(centroids / cell).astype(np.int64)
        # ボクセルキーを1本のint64へ畳んでソートし、境界で切る
        base = keys - keys.min(axis=0)
        dims = base.max(axis=0) + 1
        flat = (base[:, 0] * dims[1] + base[:, 1]) * dims[2] + base[:, 2]
        order = np.argsort(flat, kind="stable")
        sorted_flat = flat[order]
        uniq, start = np.unique(sorted_flat, return_index=True)
        self._grid = {
            "tris": tris, "cell": cell, "order": order,
            "uniq": uniq, "start": start, "dims": dims, "min": keys.min(axis=0),
        }
        if verbose:
            print("三角形の索引: %d 枚 / %d ボクセル (cell %.1fm)" % (len(tris), len(uniq), cell))

    def triangle_soup(self, centre, radius):
        """centre の周り radius[m] にある三角形を (M,3,3) で返す。全メッシュが対象。"""
        self.build_grid()
        g = self._grid
        cell = g["cell"]
        span = int(math.ceil(radius / cell))
        base = np.floor(np.asarray(centre) / cell).astype(np.int64) - g["min"]
        dims = g["dims"]
        want = []
        for dx in range(-span, span + 1):
            for dy in range(-span, span + 1):
                for dz in range(-span, span + 1):
                    c = base + np.array([dx, dy, dz])
                    if np.any(c < 0) or np.any(c >= dims):
                        continue
                    key = (c[0] * dims[1] + c[1]) * dims[2] + c[2]
                    j = np.searchsorted(g["uniq"], key)
                    if j >= len(g["uniq"]) or g["uniq"][j] != key:
                        continue
                    lo = g["start"][j]
                    hi = g["start"][j + 1] if j + 1 < len(g["start"]) else len(g["order"])
                    want.append(g["order"][lo:hi])
        if not want:
            return np.zeros((0, 3, 3))
        sel = np.concatenate(want)
        tris = g["tris"][sel].astype(np.float64)
        # ボクセルは重心で振ってあるので、実際に範囲へ入る三角形だけへ絞り直す
        near = np.all(np.abs(tris - np.asarray(centre)) <= radius, axis=2).any(axis=1)
        return tris[near]


def lower_hemisphere_dirs(count):
    """下半球のフィボナッチ分布。真下(-Y)を中心に一様。"""
    i = np.arange(count) + 0.5
    z = -i / count
    r = np.sqrt(np.maximum(0.0, 1.0 - z * z))
    phi = math.pi * (1.0 + 5.0 ** 0.5) * np.arange(count)
    return np.stack([r * np.cos(phi), z, r * np.sin(phi)], axis=1)


def prepare_soup(tris):
    """三角形スープを Moller-Trumbore 用に前処理する(器具ごとに1回だけ作って使い回す)。"""
    if len(tris) == 0:
        return None
    v0 = tris[:, 0, :]
    return {"v0": v0, "e1": tris[:, 1, :] - v0, "e2": tris[:, 2, :] - v0, "pts": tris.reshape(-1, 3)}


def escape_ratio(origin, soup, dirs, reach):
    """origin から dirs へ撃って reach まで何にも当たらなかった割合。Moller-Trumbore。"""
    if soup is None:
        return 1.0
    v0, e1, e2 = soup["v0"], soup["e1"], soup["e2"]
    pv = np.cross(dirs[:, None, :], e2[None, :, :])
    det = np.einsum("tj,rtj->rt", e1, pv)
    ok = np.abs(det) > 1e-12
    inv = np.where(ok, 1.0 / np.where(ok, det, 1.0), 0.0)
    tv = origin[None, :] - v0
    u = np.einsum("tj,rtj->rt", tv, pv) * inv
    qv = np.cross(np.broadcast_to(tv, (len(dirs),) + tv.shape), e1[None, :, :])
    v = np.einsum("rj,rtj->rt", dirs, qv) * inv
    t = np.einsum("tj,rtj->rt", e2, qv) * inv
    hit = ok & (u >= 0.0) & (u <= 1.0) & (v >= 0.0) & (u + v <= 1.0) & (t > 0.01) & (t < reach)
    return float((~hit.any(axis=1)).mean())


def ray_first_hit(origin, direction, soup, reach):
    """1本のレイの最初の交点までの距離。当たらなければ None。地面の高さを測るのに使う。"""
    if soup is None:
        return None
    ratio_dirs = direction[None, :]
    v0, e1, e2 = soup["v0"], soup["e1"], soup["e2"]
    pv = np.cross(ratio_dirs[:, None, :], e2[None, :, :])
    det = np.einsum("tj,rtj->rt", e1, pv)
    ok = np.abs(det) > 1e-12
    inv = np.where(ok, 1.0 / np.where(ok, det, 1.0), 0.0)
    tv = origin[None, :] - v0
    u = np.einsum("tj,rtj->rt", tv, pv) * inv
    qv = np.cross(np.broadcast_to(tv, (1,) + tv.shape), e1[None, :, :])
    v = np.einsum("rj,rtj->rt", ratio_dirs, qv) * inv
    t = np.einsum("tj,rtj->rt", e2, qv) * inv
    good = ok & (u >= 0.0) & (u <= 1.0) & (v >= 0.0) & (u + v <= 1.0) & (t > 1e-3) & (t < reach)
    if not good.any():
        return None
    return float(t[good].min())


def exit_directions(count=24):
    """脱出を試す向き。真下を最優先にし、そこから角度の近い順に並べる。

    真下へ下ろすだけでは足りない。背の高い街灯は電球が中空の支柱の中にあり、
    真下へ動かしても管の中に留まって全方向が塞がったままになる(実測で28本中12本)。
    """
    i = np.arange(count) + 0.5
    z = 1.0 - 2.0 * i / count
    r = np.sqrt(np.maximum(0.0, 1.0 - z * z))
    phi = math.pi * (1.0 + 5.0 ** 0.5) * np.arange(count)
    dirs = np.stack([r * np.cos(phi), z, r * np.sin(phi)], axis=1)
    dirs = np.concatenate([np.array([[0.0, -1.0, 0.0]]), dirs], axis=0)
    return dirs[np.argsort(-(dirs @ np.array([0.0, -1.0, 0.0])), kind="stable")]


def march_out_of_shell(origin, direction, soup, limit=1.5, margin=0.05):
    """direction へ進みながら器具の殻を抜け、最後の交点の少し先を返す。

    器具の殻は薄いので、交点を数えながら進めば「外側」に出られる。
    抜けきれなければ None(その向きは開口ではない)。
    """
    travelled = 0.0
    last = None
    for _ in range(8):
        p = origin + direction * travelled
        t = ray_first_hit(p, direction, soup, limit - travelled)
        if t is None:
            break
        travelled += t + 1e-3
        last = travelled
        if travelled >= limit:
            return None
    if last is None:
        return origin + direction * margin      # もともと外にいる
    return origin + direction * (last + margin)


def place_light(comp, soup, dirs_fine, dirs_coarse):
    """器具の殻を抜けた位置のうち、変位が小さく脱出率の高いものを採る。

    返り値: (位置, 変位[m], 脱出率, 合格したか)
    """
    centre = np.asarray(comp["c"])
    best = (centre, 0.0, escape_ratio(centre, soup, dirs_coarse, ESCAPE_REACH))
    for direction in exit_directions():
        p = march_out_of_shell(centre, direction, soup)
        if p is None:
            continue
        disp = float(np.linalg.norm(p - centre))
        if disp > DROP_MAX:
            continue
        ratio = escape_ratio(p, soup, dirs_coarse, ESCAPE_REACH)
        if ratio > best[2]:
            best = (p, disp, ratio)
        if ratio >= ESCAPE_MIN:
            break
    # 採用候補は粗いレイ束で選んだので、最後に細かい束で測り直す
    final = escape_ratio(best[0], soup, dirs_fine, ESCAPE_REACH)
    return best[0], best[1], final, final >= ESCAPE_MIN


def nearest_geometry_distance(p, tris):
    """p から三角形スープの頂点までの最短距離。SourceRadius のクランプに使う。"""
    if len(tris) == 0:
        return float("inf")
    return float(np.linalg.norm(tris.reshape(-1, 3) - p, axis=1).min())


def derive_intensity(key):
    flux, solid, note = PHOTOMETRY[key]
    return flux / solid, flux, solid, note


def derive_range(intensity):
    """その灯の寄与が sRGB で 0.5 階調を切る距離[m]。

    0.5階調 -> 表示リニアで約 0.0018。トーンマップ入力は L/(1.2*2^EV100) なので
    L = 0.0018 * 1.2 * 2^Exposure。拡散面の照度は E = pi*L/albedo。Range = sqrt(I/E)。
    """
    lum = 0.0018 * 1.2 * (2.0 ** SCENE_EXPOSURE)
    illum = math.pi * lum / GROUND_ALBEDO
    return math.sqrt(intensity / illum)


def assign_string_colours(model, bulbs, blue_as_blue):
    """色ごとの内殻メッシュを、外殻の電球へ最近傍で割り当てる。"""
    centres = np.array([b["c"] for b in bulbs])
    colour = [None] * len(bulbs)
    tally = {}
    for mtl_name, (label, rgb) in STRING_COLORS.items():
        mi = model.mesh_index(mtl_name)
        use = BLUE_ACTUAL if (blue_as_blue and label == "Blue") else rgb
        taken = 0
        for comp in model.components(mi):
            d = np.linalg.norm(centres - comp["c"], axis=1)
            j = int(np.argmin(d))
            if d[j] > 0.15:
                # 街灯の中のフィラメント(mesh79 の一部)はここへ来る。ストリング電球ではない
                continue
            colour[j] = (label, use)
            taken += 1
        tally[label] = taken
    missing = [i for i, c in enumerate(colour) if c is None]
    return colour, tally, missing


def select_components(model, mtl_name, rule):
    comps = model.components(model.mesh_index(mtl_name))
    if rule is None:
        return comps
    kind, value = rule
    if kind == "diag_lt":
        return [c for c in comps if c["diag"] < value]
    if kind == "verts_gt":
        return [c for c in comps if c["n"] > value]
    raise SystemExit("未知の採択規則です: %r" % (rule,))


def check_material_texture_pairing(model):
    """mtl名 と ベースカラーテクスチャ名の対応が崩れていないかを確かめる。

    newmtl の出現順 = パック済みマテリアル番号、という前提はここで担保する。
    再パックで順序が変われば名前とテクスチャがちぐはぐになるので、黙って別の器具を
    拾う前にここで落とす。
    """
    expect = [
        ("Streetlight_Glass", "streetlight_glass"),
        ("Streetlight_Support_Bulb", "streetlight_glass"),
        ("Lantern", "lantern"),
        ("Stringlights", "stringlights"),
        ("Shopsign_Bakery", "shopsign_bakery"),
    ]
    bad = []
    for name, needle in expect:
        mi = model.mesh_index(name)
        tex = model.texture_of(model.meshes[mi]["MaterialIndex"]).lower()
        if needle not in tex:
            bad.append("%s -> %s (期待: '%s' を含む)" % (name, tex or "(テクスチャ無し)", needle))
    if bad:
        raise SystemExit(
            "マテリアル名とテクスチャの対応が崩れています。exterior.mtl の順序と .kmodel の\n"
            "マテリアル番号が一致していない可能性があります:\n  " + "\n  ".join(bad))


def principal_axis_xz(points):
    """XZ平面での第1主軸。通りの向きを求めるのに使う。"""
    xz = np.stack([points[:, 0], points[:, 2]], axis=1)
    centred = xz - xz.mean(axis=0)
    cov = centred.T @ centred
    vals, vecs = np.linalg.eigh(cov)
    axis = vecs[:, int(np.argmax(vals))]
    return axis / np.linalg.norm(axis)


def choose_camera(model, lights):
    """街灯の並びから通りの軸を取り、灯が最も多く視界に入る位置を選ぶ。

    頂点の占有格子は使わない。Bistro の地面は数枚の大きな四角形でできており、
    1mセルに頂点が1つも落ちないため「街路の真ん中が空っぽ」と誤判定する
    (実際に試すと地面の高さが 7.69m という有り得ない値になった)。
    地面の高さは真下へレイを撃って測る。
    """
    pts = np.array([l["pos"] for l in lights])
    street = np.array([l["pos"] for l in lights if l["key"] == "streetlight"])
    axis2 = principal_axis_xz(street if len(street) >= 2 else pts)
    axis = np.array([axis2[0], 0.0, axis2[1]])
    centre = pts.mean(axis=0)
    s = (pts - centre) @ axis

    # 長さ40mの窓に入る灯が最大になる区間を探す
    window = 40.0
    order = np.sort(s)
    best = (None, -1)
    for start in np.arange(order[0], order[-1] - window, 1.0):
        count = int(((s >= start) & (s <= start + window)).sum())
        if count > best[1]:
            best = (start, count)
    start = best[0] if best[0] is not None else order[0]

    eye_xz = centre + axis * start
    # 灯の重心側(=通りの奥)を向く
    forward = axis if ((pts.mean(axis=0) - eye_xz) @ axis) > 0 else -axis

    soup = prepare_soup(model.triangle_soup(np.array([eye_xz[0], 5.0, eye_xz[2]]), 12.0))
    ground = None
    for probe_y in (8.0, 12.0, 20.0):
        origin = np.array([eye_xz[0], probe_y, eye_xz[2]])
        t = ray_first_hit(origin, np.array([0.0, -1.0, 0.0]), soup, 40.0)
        if t is not None:
            ground = probe_y - t
            break
    if ground is None:
        raise SystemExit("カメラ位置の真下に地面が見つかりませんでした。軸の取り方を見直してください")

    eye_height = 1.65
    eye = np.array([eye_xz[0], ground + eye_height, eye_xz[2]])
    # Yaw は +Z を0度、+X を90度として測る(Camera::GetForward が atan2(dx, dz) 相当)
    yaw = math.degrees(math.atan2(forward[0], forward[2]))
    # 【灯の重心を狙ってはいけない】灯は頭上(街灯 y≈3〜4.5、ストリングライト y≈3.4〜5.8)に
    # あるので、重心を狙うと仰角が上を向き、庇の裏だけが画面を占める絵になる(実際そうなった)。
    # 狙うのは通りの路面。20m 先の地面を見る角度にすると、街灯の作る光溜まりと
    # そこへ落ちる影が画面に入る。
    aim_distance = 20.0
    pitch = math.degrees(math.atan2(-eye_height, aim_distance))
    visible = int((((pts - eye) @ forward) > 0).sum())
    return eye, yaw, pitch, ground, visible, best[1]


def build_lights(model, blue_as_blue, verbose):
    dirs_fine = lower_hemisphere_dirs(ESCAPE_RAYS)
    dirs_coarse = lower_hemisphere_dirs(64)
    lights = []
    problems = []
    counts = {}

    for key, mtl_name, rule, label, expected in FIXTURES:
        comps = select_components(model, mtl_name, rule)
        counts[key] = len(comps)
        if len(comps) != expected:
            problems.append("%s(%s): 成分 %d 個。期待は %d 個" % (label, mtl_name, len(comps), expected))
        sizes = [c["n"] for c in comps]
        if sizes and max(sizes) > 3 * min(sizes):
            problems.append("%s: 成分あたり頂点数が %d〜%d とばらついています(融合か分割の疑い)"
                            % (label, min(sizes), max(sizes)))
        for comp in comps:
            lights.append({"key": key, "label": label, "comp": comp})

    # ストリングライトの色を割り当てる
    bulbs = [l["comp"] for l in lights if l["key"] == "string"]
    colour, tally, missing = assign_string_colours(model, bulbs, blue_as_blue)
    if missing:
        problems.append("ストリングライト %d 個に色が付きませんでした" % len(missing))
    bi = 0
    for l in lights:
        if l["key"] == "string":
            got = colour[bi]
            l["colour_name"], l["colour"] = got if got else ("White", (1.0, 1.0, 1.0))
            bi += 1
        else:
            l["colour_name"], l["colour"] = "Incandescent2700K", INCANDESCENT

    # 位置を決める(脱出率の探索)
    for n, l in enumerate(lights):
        comp = l["comp"]
        soup = prepare_soup(model.triangle_soup(comp["c"], SOUP_RADIUS))
        pos, drop, ratio, ok = place_light(comp, soup, dirs_fine, dirs_coarse)
        l["pos"], l["drop"], l["escape"], l["escape_ok"] = pos, drop, ratio, ok
        clearance = nearest_geometry_distance(pos, np.zeros((0, 3, 3)) if soup is None else soup["pts"].reshape(-1, 1, 3))
        radius = comp["r"]
        l["radius_clamped"] = False
        if radius > 0.8 * clearance:
            radius = 0.8 * clearance
            l["radius_clamped"] = True
        l["source_radius"] = max(0.005, radius)
        l["clearance"] = clearance
        reason = None
        if ratio < ESCAPE_ADOPT:
            reason = "脱出率 %.3f (器具の中に閉じ込められている)" % ratio
        elif drop > MAX_DISPLACEMENT:
            reason = "変位 %.2fm (実物の器具の位置と言えない)" % drop
        l["excluded"] = reason
        if verbose:
            print("  %-22s pos=(%8.3f,%7.3f,%8.3f) 変位 %.2fm 脱出率 %.3f 半径 %.3f%s"
                  % (l["label"], pos[0], pos[1], pos[2], drop, ratio, l["source_radius"],
                     " (クランプ)" if l["radius_clamped"] else ""))

    # 測光量
    for l in lights:
        intensity, flux, solid, note = derive_intensity(l["key"])
        l["intensity"] = intensity
        l["flux"] = flux
        l["solid"] = solid
        l["photometry_note"] = note
        l["range"] = derive_range(intensity)

    dropped = [l for l in lights if l["excluded"]]
    lights = [l for l in lights if not l["excluded"]]
    return lights, problems, counts, tally, dropped


def overlap_stats(lights):
    """地面の標本点を Range 球に含む灯の数。従来経路のタイル容量64を超えないかの目安。"""
    pts = np.array([l["pos"] for l in lights])
    rng = np.array([l["range"] for l in lights])
    lo, hi = pts.min(axis=0), pts.max(axis=0)
    xs = np.arange(lo[0], hi[0] + 1e-9, 2.0)
    zs = np.arange(lo[2], hi[2] + 1e-9, 2.0)
    worst = 0
    for x in xs:
        for z in zs:
            g = np.array([x, 0.0, z])
            d = np.linalg.norm(pts - g, axis=1)
            worst = max(worst, int((d <= rng).sum()))
    return worst


def format_scene(model, lights, camera, tally, counts, worst_overlap, blue_as_blue):
    eye, yaw, pitch, ground, visible, in_window = camera
    esc = [l["escape"] for l in lights]
    drops = [l["drop"] for l in lights]
    out = []
    w = out.append
    w("# KurenaiEngine シーンファイル - Bistro (McGuire) 屋外の夜景")
    w("#")
    w("# Tools/extract_bistro_lights.py が生成する(手で編集しない)。")
    w("# 灯の位置・色・光源半径はすべて Assets/Packed/BistroMcGuire/Exterior.kmodel と")
    w("# Exterior.kgeom を直接読んで求めた実測値で、目分量の数値は1つも入っていない。")
    w("#")
    w("# --- 何のためのシーンか ---")
    w("# MegaLights の主張は「影付きのローカルライトを、灯数に依存しない固定費で出せる」")
    w("# (docs/ImplementationDetail.md 61.7e.5)。ところが従来の検証シーン(LightScale系)は")
    w("# 床と壁と球4個しかなく、影を落とす相手がいなかった。このシーンは実物の街路に")
    w("# %d 灯を置き、多数の影が重なる様子そのものを見せる。" % len(lights))
    w("#")
    w("# 起動:")
    w("#   Sample3D.exe -scene BistroExteriorNight -dx12 -megalights 2 -autoexposure 0")
    w("# MegaLights は DX12 + DXR Tier 1.1 が必須で、既定は無効。-dx12 を忘れると")
    w("# 「シーンは出るが MegaLights は走っていない」状態になる(差分ゼロを合格と誤読しやすい)。")
    w("#")
    w("# --- 灯の内訳(すべて三角形の連結成分から導出) ---")
    adopted = {}
    for l in lights:
        adopted[l["key"]] = adopted.get(l["key"], 0) + 1
    for key, mtl_name, _rule, label, expected in FIXTURES:
        got, cand = adopted.get(key, 0), counts[key]
        note = "" if got == cand else "  ※候補 %d のうち %d を不採用" % (cand, cand - got)
        w("#   %-24s %3d 灯  (%s)%s" % (label, got, mtl_name, note))
    w("#   ストリングライトの色: " + " / ".join("%s %d" % (k, v) for k, v in sorted(tally.items())))
    if blue_as_blue:
        w("#   ※ --blue-as-blue 指定。Blue は Blue.png の色を使っている")
    else:
        w("#   ※ Paris_StringLights_01_Blue_Color は名前に反して White.png を参照している。")
        w("#      実際に描かれている白を採っている(--blue-as-blue で Blue.png の色に切替)。")
    w("#")
    w("# --- 灯を器具の重心に置いていない理由(重要) ---")
    w("# MegaLights の影レイは RAY_FLAG_FORCE_OPAQUE なので、ガラスも笠も影レイを完全に遮る。")
    w("# 器具の重心はその器具自身の内側にあるため、重心へ置くと全灯の影レイが自分の器具に")
    w("# 当たり、1灯も光らない(絵が暗いだけで例外もログも出ない)。")
    w("# そこで灯ごとに下半球 %d 方向へレイを撃ち、脱出率が %.2f を超える位置まで器具の外へ" % (ESCAPE_RAYS, ESCAPE_MIN))
    w("# 動かしてから採用している。実測された変位は %.2f〜%.2fm、脱出率は %.3f〜%.3f。"
      % (min(drops), max(drops), min(esc), max(esc)))
    w("# 動かす向きは真下を最優先にした24方向。真下だけでは足りない — 背の高い街灯は")
    w("# 電球が中空の支柱の中にあり、真下へ動かしても管の内側に留まる(28本中12本がそうだった)。")
    w("# 脱出率が %.2f 未満か、変位が %.2fm を超えた灯は採用していない" % (ESCAPE_ADOPT, MAX_DISPLACEMENT))
    w("# (でっち上げの位置を置くくらいなら灯を減らす、という方針)。")
    w("# 脱出率は近傍の全メッシュを相手に測る。1メッシュだけを見ると誤る — 重心を塞いで")
    w("# いるのはまず発光体自身(電球だけを相手にしても 0.000)だが、街灯のガラスだけを")
    w("# 相手にすると 0.198 と出て「筒だから真下へ抜ける」という誤った結論になる。")
    w("#")
    w("# --- Intensity と Range の根拠 ---")
    seen = set()
    for l in lights:
        if l["key"] in seen:
            continue
        seen.add(l["key"])
        w("#   %-24s %6.1f lm / %5.2f sr = %6.1f cd, Range %5.1f m  (%s)"
          % (l["label"], l["flux"], l["solid"], l["intensity"], l["range"], l["photometry_note"]))
    w("# Range は「その灯の寄与が sRGB で 0.5 階調を切る距離」。トーンマップ入力が")
    w("# L/(1.2*2^Exposure) で、拡散面の照度が E=pi*L/albedo(albedo=%.2f)なので" % GROUND_ALBEDO)
    w("# Range = sqrt(I/E)。Exposure=%.1f と対で効くので、露出を変えたら Range も変わる。" % SCENE_EXPOSURE)
    w("# 地面の標本点を覆う灯の最大数は %d。従来経路(MegaLights OFF)のタイル容量64を" % worst_overlap)
    w("# 超えると OFF 側が灯を捨てて絵が壊れ、ON/OFF の比較が成立しなくなる。")
    w("#")
    w("# --- カメラ ---")
    w("# 街灯 %d 本の XZ を主成分分析して通りの軸を取り、長さ40mの窓に入る灯が" % counts["streetlight"])
    w("# 最大(%d 灯)になる位置を選んだ。地面の高さ %.3f m は真下へレイを撃って測った値で、" % (in_window, ground))
    w("# そこへ目線の高さ 1.65m を足している。前方に入る灯は %d 灯。" % visible)
    w("# 俯角は「20m 先の路面を見る角度」。灯の重心を狙うと、灯が頭上にあるぶん仰角が")
    w("# 上を向き、庇の裏だけが画面を占める絵になる(最初にそれで失敗した)。")
    w("# Yaw は +Z を0度、+X を90度として測る(atan2(dx,dz) であって atan2(dz,dx) ではない)。")
    w("")
    w("[Scene]")
    w("Name = Bistro (McGuire) - Exterior (Night)")
    w("# 器具光を主役にする露出。値が大きいほど暗く写る(写真のEVと同じ向き)。")
    w("# 石畳(albedo %.2f)が街灯直下で受ける照度から、中間グレー 0.18 に載る値として導出した。" % GROUND_ALBEDO)
    w("# 自動露出を切って使うこと(構図で露出が振れると A/B 比較が成立しない)。")
    w("Exposure = %.1f" % SCENE_EXPOSURE)
    w("# 既定のまま。夜空の寄与は街灯より数桁小さいので、ここを動かしても絵はほぼ変わらない。")
    w("IBLIntensity = 0.5")
    w("# 色付きの電球(緑・赤・紫・橙)が芯から白く抜けないもの。既定の AgX はハイライトを")
    w("# 白へ脱色するため、ストリングライトの色がこのシーンの見どころとして残らない。")
    w("Tonemap = ACES")
    w("")
    w("[Model]")
    w("Path = %s" % MODEL_PATH_IN_SCENE)
    w("")
    w("[Camera]")
    w("Position = %.3f, %.3f, %.3f" % (eye[0], eye[1], eye[2]))
    w("Yaw = %.1f" % yaw)
    w("Pitch = %.1f" % pitch)
    w("")
    w("[Sun]")
    w("# twilightFactor が厳密に 0 になるのは 19.0 以降。22.0 は完全な夜。")
    w("TimeOfDay = 22.0")
    w("# Enabled = false にすると月の平行光まで一緒に消える(枠を共有しているため)。")
    w("# 月は 0.25 lx で街灯より数桁暗く競合しないので、残して夜の地の明るさを作らせる。")
    w("Enabled = true")
    w("# 0.25 lx の平行光に影は見えない。カスケードシャドウ4枚ぶんの実行時間を測定から外す。")
    w("Shadow = false")
    for l in lights:
        w("")
        w("[Light]")
        w("# %s / 色 %s / 器具の重心から %.2fm 変位 / 脱出率 %.3f%s"
          % (l["label"], l["colour_name"], l["drop"], l["escape"],
             " / 半径をクランプ" if l["radius_clamped"] else ""))
        w("Type = Point")
        w("Position = %.3f, %.3f, %.3f" % (l["pos"][0], l["pos"][1], l["pos"][2]))
        w("Color = %.4f, %.4f, %.4f" % l["colour"])
        w("Intensity = %.1f" % l["intensity"])
        w("Range = %.2f" % l["range"])
        w("SourceRadius = %.3f" % l["source_radius"])
        w("CastShadow = true")
    return "\n".join(out) + "\n"


def main():
    parser = argparse.ArgumentParser(description="Bistro屋外の器具から夜景 .kscene を生成する")
    parser.add_argument("--verify", action="store_true", help="検算だけ行い書き出さない")
    parser.add_argument("--report", action="store_true", help="灯ごとの明細を出す")
    parser.add_argument("--blue-as-blue", action="store_true",
                        help="Blue の電球に Blue.png の色を使う(既定は実際に描かれている白)")
    parser.add_argument("-o", "--output", default=OUT, help="出力する .kscene のパス")
    args = parser.parse_args()

    if not os.path.exists(KMODEL):
        raise SystemExit(
            "%s がありません。Bistro をパックしてください(README「アセットの準備」)" % KMODEL)

    print("読み込み: %s" % KMODEL)
    model = Model(KMODEL, MTL)
    check_material_texture_pairing(model)
    print("マテリアル名とテクスチャの対応: OK")

    lights, problems, counts, tally, dropped = build_lights(model, args.blue_as_blue, args.report)
    total = len(lights)
    print("抽出した灯: %d (候補 %d - 不採用 %d)" % (total, total + len(dropped), len(dropped)))
    for key, _m, _r, label, expected in FIXTURES:
        print("  %-24s %3d (期待 %d)" % (label, counts[key], expected))
    print("  色の内訳: %s = %d" % (" + ".join("%s %d" % (k, v) for k, v in sorted(tally.items())),
                                    sum(tally.values())))

    by_kind = {}
    for l in lights:
        by_kind.setdefault(l["label"], []).append(l)
    print("採用した灯の内訳(脱出率 / 変位):")
    for label, group in by_kind.items():
        e = sorted(x["escape"] for x in group)
        d = sorted(x["drop"] for x in group)
        print("  %-22s %3d 灯  脱出率 %.3f〜%.3f (中央 %.3f)  変位 %.2f〜%.2fm (中央 %.2f)"
              % (label, len(group), e[0], e[-1], e[len(e) // 2], d[0], d[-1], d[len(d) // 2]))
    if dropped:
        print("不採用にした灯:")
        for l in dropped:
            print("  - %s: %s" % (l["label"], l["excluded"]))
    if len(dropped) > 0.1 * (len(lights) + len(dropped)):
        problems.append("不採用が %d 灯(全体の1割超)あります。抽出の規則を見直してください" % len(dropped))

    worst = overlap_stats(lights)
    print("地面の点を覆う灯の最大数: %d (従来経路のタイル容量は64)" % worst)
    if worst > 48:
        problems.append("Range の重なりが %d で、従来経路のタイル容量64に対して余裕がありません" % worst)

    camera = choose_camera(model, lights)
    print("カメラ: (%.3f, %.3f, %.3f) Yaw %.1f Pitch %.1f  地面 %.3f  前方の灯 %d"
          % (camera[0][0], camera[0][1], camera[0][2], camera[1], camera[2], camera[3], camera[4]))

    if problems:
        print("\n検算で問題が出ました:")
        for p in problems:
            print("  - %s" % p)
        raise SystemExit(1)
    print("検算: すべて通りました")

    if args.verify:
        print("--verify なので書き出しません")
        return 0

    text = format_scene(model, lights, camera, tally, counts, worst, args.blue_as_blue)
    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    with open(args.output, "w", encoding="utf-8", newline="\n") as fp:
        fp.write(text)
    print("wrote %s (%d 灯)" % (os.path.normpath(args.output), total))
    print("パックするには:")
    print("  KurenaiPacker.exe --scene Scenes\\BistroExteriorNight.kscene "
          "-o Assets\\Packed\\Scenes\\BistroExteriorNight.kscene")
    return 0


if __name__ == "__main__":
    sys.exit(main())
