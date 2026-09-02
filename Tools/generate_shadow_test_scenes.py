# スクリーンスペースシャドウ(接触影)とタイルライトカリングの動作確認用シーン(.kscene)を生成する。
#
# ジオメトリは generate_light_test.py が作る LightTest.kmodel(床20x20 + 後ろ壁 + 粗さ違いの球4個)を
# そのまま流用し、このスクリプトはカメラとライトの配置だけを書いた .kscene を出力する。
#
# 出力先は Git 管理下の Scenes/ で、他の手書き .kscene と同じ扱いになる(README「Assetsフォルダに
# ついて」/ docs/Architecture.html 10.1節)。ランタイムが読むのは Assets/Packed/Scenes/ 側なので、
# 生成後に KurenaiPacker を通す必要がある:
#   KurenaiPacker.exe --scene Scenes\ScreenSpaceShadowTest.kscene -o Assets\Packed\Scenes\ScreenSpaceShadowTest.kscene
#   KurenaiPacker.exe --scene Scenes\ManyLightsTest.kscene        -o Assets\Packed\Scenes\ManyLightsTest.kscene
#
# 生成されるシーン:
#   ScreenSpaceShadowTest.kscene … 接触影が出ているかを目視する用。太陽を切り、
#                                  球と床の接地部が正面に来るカメラを置く
#   ManyLightsTest.kscene         … タイルライトカリング用。床面へポイントライトを格子状に分散させる
#
# 注意: 元の glTF は右手系で authored されており、ModelLoader の aiProcess_ConvertToLeftHanded で
# Z が反転する。glTF 上 z=-4 の球はエンジン座標では z=+4、z=-10 の壁は z=+10 になる。
# 下のカメラ位置・ライト位置は「エンジン座標(左手系)」で書いてある。

import argparse
import os

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(SCRIPT_DIR, "..", "Scenes")

MODEL_PATH = "LightTest/LightTest.kmodel"

# 床の半径(generate_light_test.py の FLOOR_HALF_SIZE と合わせる)。ライトを床の内側へ収めるのに使う
FLOOR_HALF_SIZE = 10.0


def write_scene(file_name, lines):
    os.makedirs(OUT_DIR, exist_ok=True)
    path = os.path.join(OUT_DIR, file_name)
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print(f"wrote {os.path.normpath(path)}")


def generate_screen_space_shadow_test():
    # 接触影の確認用。太陽を切ってポイント/スポットだけで照らす。
    # ライトは LightTest.kmodel に埋め込まれている2灯(ポイント+スポット)をそのまま使うため、
    # このシーンでは [Light] を書かない
    return [
        "# スクリーンスペースシャドウ(接触影)の目視確認用シーン。",
        "# Tools/generate_shadow_test_scenes.py で生成される(手で編集しない)。",
        "#",
        "# 確認手順: Post Processing の Enable AO / Indirect Light を切ってから",
        "# Lighting > Screen-Space Shadows の Enable を ON/OFF すると、球と床の接地部の影が出入りする。",
        "# AO を切るのは、SSAO の遮蔽と接触影が同じ場所に出て見分けがつかなくなるため。",
        "",
        "[Scene]",
        "Name = Screen-Space Shadow Test",
        "",
        "[Model]",
        f"Path = {MODEL_PATH}",
        "",
        "[Camera]",
        "# 球(エンジン座標で z=+4、半径1)を手前から見下ろす",
        "Position = 0.0, 3.5, -3.0",
        "# Yaw/Pitch は度(ドキュメント4.7節)。Yaw は +Z 軸を 0 度・+X 軸を 90 度として測るので、",
        "# Yaw = 0 で +Z 方向を向く。Pitch = -14.3 で約14度見下ろす",
        "Yaw = 0.0",
        "Pitch = -14.3",
        "",
        "[Sun]",
        "# 太陽のカスケードシャドウを切り、ポイント/スポットの影だけを見る",
        "Shadow = false",
        "Enabled = false",
    ]


def generate_many_lights_test(grid, intensity, light_range, height):
    lines = [
        "# タイルライトカリングの動作確認・性能計測用シーン。",
        "# Tools/generate_shadow_test_scenes.py で生成される(手で編集しない)。",
        "#",
        f"# 床面へポイントライトを {grid}x{grid} = {grid * grid} 灯、格子状に分散させる。",
        "# LightTest.kmodel に埋め込まれた2灯(ポイント+スポット)が加わるため、",
        f"# 実行時の有効ライト数は {grid * grid + 2} 灯になる。",
        "#",
        "# 確認手順: Render Targets の View を Light Tiles にするとタイルごとのライト数が",
        "# ヒートマップで見える(黒=0灯、青→緑→赤=多い、マゼンタ=タイル容量64の超過)。",
        "# Lighting > Tiled Light Culling の Enable を ON/OFF しても最終画像は変わらないこと",
        "# (カリングは見た目を変えない最適化であること)も、このシーンで確認する。",
        "",
        "[Scene]",
        "Name = Many Lights Test",
        "",
        "[Model]",
        f"Path = {MODEL_PATH}",
        "",
        "[Camera]",
        "# 床全体を俯瞰し、多数のライトが画面内に収まる位置",
        "Position = 0.0, 6.0, -12.0",
        "# Yaw/Pitch は度(ドキュメント4.7節)。Pitch = -20.1 で約20度見下ろす",
        "Yaw = 0.0",
        "Pitch = -20.1",
        "",
        "[Sun]",
        "Shadow = false",
        "Enabled = false",
        "",
    ]

    # 床(±FLOOR_HALF_SIZE)の内側へ等間隔に配置する。端に寄せすぎると床外へ落ちるため半ステップ分内側から始める
    span = FLOOR_HALF_SIZE * 2.0
    step = span / grid
    start = -FLOOR_HALF_SIZE + step * 0.5

    for iz in range(grid):
        for ix in range(grid):
            x = start + ix * step
            z = start + iz * step
            # タイルごとの寄与を見分けやすいよう、格子位置で色を変える
            r = 0.4 + 0.6 * (ix / max(grid - 1, 1))
            b = 0.4 + 0.6 * (iz / max(grid - 1, 1))
            lines += [
                "[Light]",
                "Type = Point",
                f"Position = {x:.2f}, {height:.2f}, {z:.2f}",
                f"Color = {r:.2f}, 0.50, {b:.2f}",
                f"Intensity = {intensity:.1f}",
                f"Range = {light_range:.1f}",
                "CastShadow = true",
                "",
            ]

    return lines


def generate_penumbra_test(source_radius, light_height, intensity):
    # 球光源の半影(soft shadow)の測定用シーン。
    #
    # 【なぜ球4個の既存モデルで足りるか】半影の幅は「遮蔽物と受光面の距離」に比例して
    # 広がる。球は床に接しているので、接地点では距離0(ハードエッジ)、影の先へ行くほど
    # 距離が増える。**1つの球の影を接地点から追うだけで距離依存が測れる**ので、
    # 高さ違いの棒を並べたテスト形状を新しく作る必要がない。
    #
    # 半径を振ったときに半影の幅が比例して広がることも、このシーンで測る
    # (SourceRadius を変えて撮り、影の縁の勾配の幅を比べる)。
    return [
        "# 球光源の半影の測定用シーン。",
        "# Tools/generate_shadow_test_scenes.py で生成される(手で編集しない)。",
        "#",
        "# MegaLights を有効(Stochastic か Reference)にして使う。従来のライトループと",
        "# スクリーンスペースシャドウは光源を点として扱うため、このシーンでも半影は出ない。",
        "#",
        "# 確認手順: -megalights 1 -megalightsrays 64 が半影の真値。",
        "# -megalights 2 はそれを1本のレイで推定するので、蓄積平均が真値へ寄る。",
        "# SourceRadius = 0 にすると点光源と完全に一致すること(退化テスト)も、ここで確かめる。",
        "",
        "[Scene]",
        "Name = Penumbra Test",
        "",
        "[Model]",
        f"Path = {MODEL_PATH}",
        "",
        "[Camera]",
        "# 球の影が伸びる先(手前側)を見下ろす。影の縁の勾配を画素で測るための固定カメラ",
        "Position = 0.0, 4.0, -9.0",
        "Yaw = 0.0",
        "Pitch = -22.0",
        "",
        "[Sun]",
        "# 太陽を切り、球光源1灯だけで照らす",
        "Shadow = false",
        "Enabled = false",
        "",
        "[Light]",
        "# 球の真上やや奥。影が手前へ伸びるので、接地点から影の先まで一続きに見える",
        "Type = Point",
        "Position = 0.0, %.1f, 4.0" % light_height,
        "Color = 1.0, 1.0, 1.0",
        "Intensity = %.1f" % intensity,
        "Range = 40.0",
        "# 光源そのものの半径[m]。0なら点光源(ハードシャドウ)",
        "SourceRadius = %.2f" % source_radius,
        "CastShadow = true",
    ]


def generate_penumbra_height_test(source_radius, height, light_height=10.0):
    # 半影の幅が「遮蔽物と受光面の距離」に比例することの測定用シーン。
    #
    # 【専用モデルを使う】Tools/generate_penumbra_test.py が作る PenumbraTest.kmodel は、
    # 半径0.6mの球を軸上の指定した高さへ1個だけ浮かべたもの。
    # 高さがそのまま「遮蔽物と受光面の距離」になる。
    #
    # LightTest では測れない: 球が床に接しているため、影の縁を作るシルエットの高さが
    # 影の全周でほぼ一定になり、距離が変わらない。
    #
    # 【光源は近くに置く】影の半径は R_occ * H/(H-h) なので、H が h に近いほど影が
    # 遮蔽物より大きくなり、真上から見ても遮蔽物の外へはみ出して見える。
    # H=10 なら h=4/6/7/8 で理論値 w = 2R*h/(H-h) が 4 倍以上開き、
    # 「比例するか」より強い検証(理論値そのものとの突き合わせ)ができる。
    return [
        "# 半影の幅が「遮蔽物と受光面の距離」に比例することの測定用シーン。",
        "# Tools/generate_shadow_test_scenes.py で生成される(手で編集しない)。",
        "#",
        "# ジオメトリは Tools/generate_penumbra_test.py が作る PenumbraTest.kmodel",
        "# (同じ球を高さ 1 / 2 / 4 / 8 m へ浮かべたもの)。",
        "#",
        "# 測り方: 影あり(-megalightsrays 256)を影なし(-megalightsrays 0)で割ると",
        "# 可視率が出る。床の距離減衰が約分されるので影の縁だけを測れる。",
        "# SourceRadius=0 を対照に取ること(半影が消えるはず)。",
        "",
        "[Scene]",
        "Name = Penumbra Height Test",
        "",
        "[Model]",
        "Path = PenumbraTest/PenumbraH%d.kmodel" % int(height),
        "",
        "[Camera]",
        "# 真上から見下ろす。斜めから見ると遠近で幅が縮み、距離依存と混ざる",
        "Position = 0.0, 48.0, 0.0",
        "Yaw = 0.0",
        "Pitch = -89.9",
        "",
        "[Sun]",
        "Shadow = false",
        "Enabled = false",
        "",
        "[Light]",
        "# 遮蔽物の真上・遠方。高く置くほど w = 2R*h/(H-h) が h への比例に近づく",
        "Type = Point",
        "Position = 0.0, %.1f, 0.0" % light_height,
        "Color = 1.0, 1.0, 1.0",
        "Intensity = 4000.0",
        "Range = 200.0",
        "SourceRadius = %.2f" % source_radius,
        "CastShadow = true",
    ]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--grid", type=int, default=8, help="ManyLightsTest のライト格子の一辺(既定: 8 → 64灯)")
    parser.add_argument("--intensity", type=float, default=300.0, help="ライトの強度(カンデラ)")
    parser.add_argument(
        "--range",
        dest="light_range",
        type=float,
        default=5.0,
        help="ライトの影響半径。大きくするとタイルあたりのライト数が増え、カリングの効きを見やすくなる",
    )
    parser.add_argument("--height", type=float, default=2.0, help="ライトを置く高さ")
    parser.add_argument(
        "--source-radius",
        dest="source_radius",
        type=float,
        default=0.5,
        help="PenumbraTest の光源半径[m](既定: 0.5)。0にすると点光源になり半影が消える",
    )
    parser.add_argument(
        "--penumbra-light-height",
        dest="penumbra_light_height",
        type=float,
        default=8.0,
        help="PenumbraTest の光源の高さ[m](既定: 8.0)",
    )
    args = parser.parse_args()

    if args.grid < 1:
        parser.error("--grid は1以上を指定してください")

    write_scene("ScreenSpaceShadowTest.kscene", generate_screen_space_shadow_test())
    # --- ライト数スケーリングの測定用(段階7)---
    # 【同じカメラ・同じ配置で灯数だけを変える】ライト数に対して MegaLights の合計msが
    # 横ばいかを測るためのもの。格子の一辺を変えるので、灯数は 4/36/144/529/1024 になる
    # (LightTest.kmodel 埋め込みの2灯が加わる)。Range は格子が広がっても
    # 1タイルあたりの灯数が増えるよう固定する
    for scaling_grid in (2, 6, 12, 23, 32):
        write_scene(
            "LightScale%d.kscene" % (scaling_grid * scaling_grid),
            generate_many_lights_test(scaling_grid, args.intensity, args.light_range, args.height),
        )
    print("LightScale: " + ", ".join(
        "%d灯" % (g * g + 2) for g in (2, 6, 12, 23, 32)))

    write_scene(
        "ManyLightsTest.kscene",
        generate_many_lights_test(args.grid, args.intensity, args.light_range, args.height),
    )
    write_scene(
        "PenumbraTest.kscene",
        generate_penumbra_test(args.source_radius, args.penumbra_light_height, args.intensity),
    )
    for h in (4, 6, 7, 8):
        write_scene(
            "PenumbraH%d.kscene" % h, generate_penumbra_height_test(args.source_radius, h))
    print(f"PenumbraTest: SourceRadius={args.source_radius}, light height={args.penumbra_light_height}")
    print(f"ManyLightsTest: {args.grid}x{args.grid} = {args.grid * args.grid} lights "
          f"(intensity={args.intensity}, range={args.light_range}, height={args.height})")


if __name__ == "__main__":
    main()
