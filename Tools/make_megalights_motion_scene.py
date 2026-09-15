#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""MegaLights の「カメラを動かしたときの品質」を測るシーンを生成する。

**なぜ別のシーンにするのか。** 計測専用のカメラ経路を、製品として使うシーンデータへ
混ぜるのは筋が悪い。`BistroExteriorNight.kscene` は絵を見るためのシーンであって、
検証の仕掛けを置く場所ではない。

**なぜ手で複製しないのか。** 照明が143灯あり、片方だけ直せば黙ってドリフトする。
元のシーンから機械的に起こせば、元を直したときに再生成するだけで済む。

使い方:
    python Tools/make_megalights_motion_scene.py
    python Tools/make_megalights_motion_scene.py --check   # 生成物が最新かだけ見る

元: Scenes/BistroExteriorNight.kscene
出力: Scenes/MegaLightsMotionCheck.kscene
"""

import argparse
import io
import os
import sys

SOURCE = "Scenes/BistroExteriorNight.kscene"
OUTPUT = "Scenes/MegaLightsMotionCheck.kscene"

BANNER = [
    "# ============================================================================",
    "# 【このファイルは生成物。手で編集しないこと】",
    "#   生成: python Tools/make_megalights_motion_scene.py",
    "#   元  : Scenes/BistroExteriorNight.kscene",
    "#",
    "# MegaLights の「カメラを動かしたときの品質」を測るためのシーン。",
    "# 中身は元のシーンと**照明もモデルも同一**で、違うのは下の [CameraPath] だけ。",
    "# 元のシーンを直したら、このスクリプトを流し直すこと。",
    "#",
    "# 【計測用の経路を元のシーンへ戻さないこと】絵を見るためのシーンデータへ",
    "# 検証の仕掛けを混ぜると、使う側が何を見ているのか分からなくなる。",
    "# ============================================================================",
    "",
]

# 計測用のカメラ経路。角度は度。Key = フレーム, X, Y, Z, Yaw, Pitch
PATHS = """\
# --- 決定的なカメラ経路(計測専用) -------------------------------------------------
#
# 【何のためにあるか】「カメラを動かしたときのノイズと遅れ(残像)」は、同じ軌跡を
# 2回再現する手段が無いと測れない。通常の操作は移動量がΔtに比例し、視点回転は
# GetAsyncKeyState を見るので PostMessage からは駆動できない。
# ここに経路を置くと -camerapath <名前> で再生でき、同じ経路で参照実装(-megalights 1)を
# 走らせれば **1フレームごとの真値** が作れる。
#
# 【回転を含むものと含まないもの、両方ある】等速の平行移動だけだと「時間シフト」と
# 「空間シフト」が縮退し、空間ぼけが遅れとして検出されてしまう。指標をノイズ・ぼけ・
# 遅れに分けたいときは回転を含む経路(Strafe / Orbit)を使うこと。
# 前進の経路(Dolly / WKey1s / WKeyHalf)は回転を含まない ―― こちらは**申告された操作を
# 忠実に再現する**のが目的で、真値と1フレームずつ突き合わせるので縮退は問題にならない。

[CameraPath]
# 右へ滑りながら少しずつ左右を向く。ビストロの庇・柱・テーブル脚といった細い形状が
# 次々に遮蔽を作っては外すので、履歴の棄却(= 移動中の粒の発生源)が実際に起きる
Name = Strafe
Interp = CatmullRom
End = Hold
# Frame, PosX, PosY, PosZ, YawDeg, PitchDeg
Key = 0,   8.610, 1.882, -8.257, -58.6, -4.7
Key = 60,  9.563, 1.882, -7.097, -50.6, -4.7
Key = 120, 10.516, 1.882, -5.937, -42.6, -4.7

[CameraPath]
# 【わざと動かない経路。消さないこと】
# 「動いていない経路は拒否する」という検算そのものが効いていることを確かめるための陽性対照。
# -camerapath Still を指定すると Error を出して再生を拒否するのが正しい挙動で、
# 再生されてしまったら検算が実装されていない(あるいは壊れた)ということになる。
Name = Still
Interp = Linear
End = Hold
Key = 0,   9.563, 1.882, -7.097, -50.6, -4.7
Key = 120, 9.563, 1.882, -7.097, -50.6, -4.7

[CameraPath]
# ほぼその場で首を振る。並進が小さいので、遮蔽の付け外しではなく
# 「画面の縁から新しい領域が入ってくる」side の棄却が支配的になる。
Name = Orbit
Interp = CatmullRom
End = Hold
Key = 0,   9.263, 1.882, -7.097, -70.6, -4.7
Key = 60,  9.563, 1.882, -7.097, -50.6, -4.7
Key = 120, 9.863, 1.882, -7.097, -30.6, -4.7

[CameraPath]
# まっすぐ前進する(4m / 120フレーム = 2 m/s)。向きは一切変えない。
# 前進では画面中央の動きがほぼ0、周辺ほど大きいという速度場になるので、
# 横滑りとは履歴の壊れ方が違う。
# 【終点が壁の中に入っていないことを確認済みなのはこの経路の終点だけ】
# 距離を伸ばすときは、先に深度ダンプで終点の可視性を確かめること。
Name = Dolly
Interp = Linear
End = Hold
Key = 0,   9.563, 1.882, -7.097, -50.6, -4.7
Key = 120, 6.483, 1.554, -4.565, -50.6, -4.7

[CameraPath]
# 【タイル形のムラを再現する経路】既定カメラの位置で W を1秒押したのと同じ動き。
# 速度 5.00 m/s(= [Scene]CameraSpeed の既定)、向きは変えない。
# 行き60フレーム / 帰り60フレームの往復で End = Loop なので、**常に 5m/s で動いている**。
#
# 【速度が効く】2 m/s(Dolly)では16x16のタイル形のムラは目に見えない。5 m/s では
# はっきり見える。履歴の妥当性判定がカメラの前進による深度変化を補正していないため、
# 1フレームの前進量 v に対して **視距離 z < v/0.05 の画素は遮蔽が変わらなくても
# 履歴を捨てられる**(v=0.0833m なら 1.67m 以内すべて)。
# 根拠と実測は docs/ImplementationDetail.md を参照。
Name = WKey1s
Interp = Linear
End = Loop
Key = 0,   9.563, 1.882, -7.097, -50.6, -4.7
Key = 60,  5.712, 1.472, -3.934, -50.6, -4.7
Key = 120, 9.563, 1.882, -7.097, -50.6, -4.7

[CameraPath]
# WKey1s と同じ区間を半分の速さ(2.5 m/s)で通る**対照**。
# 「履歴の棄却は速度で決まる(z < v/0.05)」という原因の断定を、
# 原因を動かして効果を見る形で確かめるために使う。
# 同じカメラ位置で棄却率が大きく下がれば、遮蔽の変化では説明できない。
Name = WKeyHalf
Interp = Linear
End = Loop
Key = 0,   9.563, 1.882, -7.097, -50.6, -4.7
Key = 120, 5.712, 1.472, -3.934, -50.6, -4.7
Key = 240, 9.563, 1.882, -7.097, -50.6, -4.7
"""


def strip_camera_paths(text, newline):
    """[CameraPath] セクションと、その直前に付いている説明コメントを取り除く。

    セクションの切れ目は行頭の '[' で判断する。[CameraPath] の直前に続く
    コメント行(#)と空行も、その経路の説明なので一緒に落とす。
    """
    lines = text.split(newline)
    out = []
    i = 0
    while i < len(lines):
        line = lines[i]
        if line.strip() == "[CameraPath]":
            # 直前のコメント・空行を出力から取り下げる
            while out and (out[-1].strip().startswith("#") or out[-1].strip() == ""):
                out.pop()
            i += 1
            while i < len(lines) and not lines[i].lstrip().startswith("["):
                i += 1
            continue
        out.append(line)
        i += 1
    return newline.join(out)


def build(source_text, newline):
    body = strip_camera_paths(source_text, newline)
    # 末尾の空行を整理してから、計測用の経路を足す
    body = body.rstrip(newline + " \t") + newline + newline
    banner = newline.join(BANNER) + newline
    paths = PATHS.replace("\n", newline)
    return banner + body + paths


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true",
                        help="生成せず、出力が最新かどうかだけを判定する(CI向け)")
    args = parser.parse_args()

    if not os.path.exists(SOURCE):
        print(f"元のシーンが見つかりません: {SOURCE}", file=sys.stderr)
        print("リポジトリのルートで実行してください。", file=sys.stderr)
        return 1

    try:
        raw = io.open(SOURCE, "rb").read().decode("utf-8")
    except (OSError, UnicodeDecodeError) as e:
        print(f"元のシーンを読めません: {SOURCE}: {e}", file=sys.stderr)
        return 1

    newline = "\r\n" if "\r\n" in raw else "\n"
    text = build(raw, newline)

    if "[CameraPath]" in strip_camera_paths(raw, newline):
        print("元のシーンから [CameraPath] を取り除けませんでした。書式を確認してください。",
              file=sys.stderr)
        return 1

    if args.check:
        if not os.path.exists(OUTPUT):
            print(f"生成物がありません: {OUTPUT}", file=sys.stderr)
            return 1
        current = io.open(OUTPUT, "rb").read().decode("utf-8")
        if current != text:
            print(f"生成物が古くなっています: {OUTPUT}", file=sys.stderr)
            print("python Tools/make_megalights_motion_scene.py を流し直してください。",
                  file=sys.stderr)
            return 1
        print(f"最新です: {OUTPUT}")
        return 0

    try:
        io.open(OUTPUT, "wb").write(text.encode("utf-8"))
    except OSError as e:
        print(f"生成物を書けません: {OUTPUT}: {e}", file=sys.stderr)
        return 1

    # コメント中の言及を数えないよう、行頭のセクション見出しだけを数える
    n = sum(1 for line in text.splitlines() if line.strip() == "[CameraPath]")
    print(f"生成しました: {OUTPUT}（カメラ経路 {n} 本 / 改行 {'CRLF' if newline == chr(13) + chr(10) else 'LF'}）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
