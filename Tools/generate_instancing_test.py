# -*- coding: utf-8 -*-
"""インスタンシングの効果を測るための .kscene を生成する。

使い方:
    python Tools/generate_instancing_test.py <出力する .kscene のパス>
    python Tools/generate_instancing_test.py Scenes/InstancingTest.kscene --grid 16 --spacing 18

【なぜ専用シーンが要るのか】同じ .kmodel を多重配置しているシーンは
MultiModelTest.kscene(3配置)しかなく、ドローコールの削減が「3 → 1」では計測誤差に埋もれる。
PLATEAU 東京23区も Sponza も Bistro も全モデルがユニークで、インスタンシングは一度も
発動しない。効き方を数値で示すには、まとめられる配置が多数あるシーンが要る。

【なぜ MeshletStage.kmodel なのか】3つの条件で選んでいる:

  1. **1インスタンスあたりのVRAMが小さい**(.kgeom 1KB / テクスチャ1枚)。
     モデル共有が入る前は同じ .kmodel を N 回置くと VB/IB/テクスチャが N 組できるため、
     MaterialTest(35MB/体)では256体で8.9GBになり載らない。
  2. **メッシュ数が少ない**(2)。ドローコール数は「配置数 × メッシュ数 × パス数」で効くが、
     DX12は1フレームに発行できる描画回数に上限がある(IRHIDevice::GetMaxDrawsPerFrame、
     現在4096)。インスタンシングを切ったときの側が上限を超えて落ちてしまうと、
     そもそもA/B比較が成立しない。
  3. 平らな板なので、格子に並べても互いを隠さない(オクルージョンの有無で
     比較条件がぶれない)。

  256配置 × 2メッシュ × (シャドウ4カスケード + 深度プリパス + G-Buffer) = 3,072ドロー/フレーム。
  上限4096に収まりつつ、削減が誤差に埋もれない規模になっている。

【ミラーリングを混ぜている理由】ワールド行列の行列式が負のインスタンスは三角形の
ワインディングが反転するため、表裏判定を入れ替えたパイプラインで描かなければならない
(ModelInstance::IsMirrored)。パイプラインステートはドロー単位の設定なので、
**ミラーリングの有無が違うインスタンスを1つのドローへまとめてはいけない。**
まとめてしまうと片方が裏面として全部捨てられる。それが起きていないことを絵で確かめられるよう、
格子の一部を負スケールにしてある。
"""
import argparse
import sys

# Assets ルートからの相対パス。0テクスチャではなく1テクスチャのモデルを選んでいるのは、
# テクスチャのバインドを伴う実際の描画に近い条件で測るため
MODEL_PATH = 'MeshletStage/MeshletStage.kmodel'

HEADER = """\
# KurenaiEngine シーンファイル - インスタンシングの効果測定用
#
# Tools/generate_instancing_test.py が生成する(手で編集しないこと)。
#   python Tools/generate_instancing_test.py Scenes/InstancingTest.kscene
#
# 同じ .kmodel を格子状に {count} 体並べたシーン。インスタンシングが有効なら、
# 同じモデル・同じミラーリングのインスタンスが1回のドローへまとまる。
# 無効なら1体ずつ描かれるので、性能ログのドローコール数が {count} 倍近く違う。
#
# 格子の一部({mirrored} 体)はX軸のみ負スケールにしてある。ワインディングが反転するため
# 別のパイプラインステートで描く必要があり、**インスタンシングでも別のバッチに
# 分かれなければならない**。まとめてしまうと片方が裏面として捨てられ、絵から消える。
#
# 影がドローコールの主な発生源(4カスケード)なので、ShadowDistance は既定のままにしてある。

[Scene]
Name = Instancing Test{suffix}
# 空の輝度に引っ張られず、板の明るさで比較できるようにする(EmeraldSquare と同じ値)
Exposure = 15{sceneExtra}
"""

WATER_AND_PROBE = """
# --- 平面反射と反射プローブを通すための追加(--with-reflections) ---
#
# インスタンシングは深度プリパス / G-Buffer / シャドウのほかに、
# **反射プローブの焼き込み**と**平面反射**でも働く。どちらも通らないシーンだけで
# 確かめていると、その2経路は「絵が合っている」ことにも「壊れている」ことにも
# 気付けない(実際に、カットアウトの影の欠落を検証シーンが通っていなかったために
# 見逃した前例がある)。
#
# 水面はモン・サン=ミシェルの4000m四方の板を借りる。板は水面より上(y=3)に置いてある。

[Model]
Path = MontSaintMichelStudy/Water.kmodel
Translation = 0, 0, 0
Water = true

[Water]
NormalMap = MontSaintMichelStudy/WaterNormal.png

[ReflectionProbe]
Name = Grid Center
Position = 0, {probeY:.1f}, 0
Shape = Box
BoxExtents = {probeExtent:.1f}, 12.0, {probeExtent:.1f}
BlendDistance = 2.0
"""

CAMERA = """
[Camera]
# 格子全体が視錐台に入る俯瞰。ここから前進すると視錐台カリングが効き始めるので、
# 「カリングとインスタンシングのどちらで減ったのか」を分けて見られる
Position = {x:.1f}, {y:.1f}, {z:.1f}
Yaw = {yaw:.1f}
Pitch = {pitch:.1f}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('out_path', help='出力する .kscene のパス')
    parser.add_argument('--grid', type=int, default=16, help='格子の1辺の数(既定16 = 256体)')
    parser.add_argument('--spacing', type=float, default=18.0,
                        help='格子の間隔[m](既定18。モデルのXZの広がり12mより広く取り重ならないようにする)')
    parser.add_argument('--mirror-every', type=int, default=8,
                        help='何体ごとにミラーリング(X軸の負スケール)にするか(既定8)')
    parser.add_argument('--with-reflections', action='store_true',
                        help='水面(平面反射)と反射プローブを足す。この2つのパスも'
                             'インスタンシングの対象なので、通さないと検証できない')
    args = parser.parse_args()

    if args.grid < 1:
        print('[ERROR] --grid は1以上であること', file=sys.stderr)
        return 1
    if args.mirror_every < 1:
        print('[ERROR] --mirror-every は1以上であること', file=sys.stderr)
        return 1

    count = args.grid * args.grid
    half = (args.grid - 1) * args.spacing * 0.5

    entries = []
    mirrored = 0
    for iz in range(args.grid):
        for ix in range(args.grid):
            index = iz * args.grid + ix
            x = ix * args.spacing - half
            z = iz * args.spacing - half
            is_mirrored = (index % args.mirror_every) == 0
            if is_mirrored:
                mirrored += 1
            # 水面を張るときは板を水面より上へ持ち上げる。水面下にあるものは
            # 平面反射のクリップ平面で落とされ、反射に何も映らなくなる
            y = 3.0 if args.with_reflections else 0.0
            lines = ['[Model]', 'Path = %s' % MODEL_PATH,
                     'Translation = %.1f, %.1f, %.1f' % (x, y, z)]
            if is_mirrored:
                # X軸のみ負 = 行列式が負。ワインディングが反転する
                lines.append('Scale = -1, 1, 1')
            entries.append('\n'.join(lines))

    # 格子全体を俯瞰する位置。Yaw=180 は forward=(0, sinP, -cosP)(Core/Camera.cpp の
    # GetForward)なので -Z を向く。カメラを (0, E, E) に置き Pitch=-45 とすると
    # 視線がちょうど原点(格子の中心)を通る。
    # E を格子の1辺ぶん取ると中心までの距離が 1.41E となり、垂直画角60度の半分から
    # 見える半径は 0.82E ≒ 格子の半対角(0.71E)より広い ―― 全インスタンスが視錐台に入る。
    # **入りきらないと「カリングで減った」のか「まとめて減った」のかを取り違える**
    extent = (args.grid - 1) * args.spacing
    camera = CAMERA.format(x=0.0, y=extent, z=extent, yaw=180.0, pitch=-45.0)

    # 【[Scene]節に書くこと】ScreenSpaceReflection は [Scene] のキーなので、
    # [Camera] より後ろに置くと別の節の未知キーとして警告になり、値が効かない
    scene_extra = ''
    if args.with_reflections:
        # 【明示しないと平面反射パスが走らない】エンジンの既定は「レイトレーシングが
        # 使えない環境では反射なし」で、DX11 では書かない限り無効になる。
        # それではこのシーンを作った目的(平面反射でインスタンシングが働くことの確認)を果たせない
        scene_extra = chr(10) + 'ScreenSpaceReflection = true'

    extra = ''
    if args.with_reflections:
        # 水面(平面反射パス)と反射プローブ(プローブ焼き込みパス)。
        # どちらもインスタンシングの対象パスなので、通さないとその経路が検証できない。
        # 水面はモン・サン=ミシェルの4000m四方の板をそのまま借りる
        extra = WATER_AND_PROBE.format(
            probeY=8.0, probeExtent=max(extent * 0.6, 20.0))

    text = (HEADER.format(count=count, mirrored=mirrored, sceneExtra=scene_extra,
                          suffix=' (Reflections)' if args.with_reflections else '')
            + camera + extra
            + '\n' + '\n\n'.join(entries) + '\n')

    with open(args.out_path, 'w', encoding='utf-8', newline='\r\n') as f:
        f.write(text)

    print('%s を書き出した: %d 体(うちミラーリング %d 体) / 間隔 %.1fm'
          % (args.out_path, count, mirrored, args.spacing))
    return 0


if __name__ == '__main__':
    sys.exit(main())
