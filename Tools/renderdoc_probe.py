#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""RenderDoc をGUI無しで動かし、キャプチャを採る/中身を読む。

**これは Python 3.7 で実行すること**(他のツールと違い、RenderDoc の `renderdoc.pyd` が
ビルドしたPythonのバージョンでしか読めないため)。

値のダンプ(`-dumptex` + `texdump_inspect.py`)では**原理的に分からない**ことがある ――
どのドローがどのリソースを読み書きしたか、どのスロットに何がバインドされていたか。
それを見るための経路。

## 使う前に: renderdoc.pyd を用意する

配布版のRenderDocには `renderdoc.pyd` が入っていない(`qrenderdoc.exe` に内蔵されているだけ)。
ソースからこのモジュールだけをビルドする。手順は `docs/ImplementationDetail.md` 63.10。

**持ち回るのは `renderdoc.pyd` 1つだけでよい**(6MB)。本体の `renderdoc.dll` は
インストール済みのものを使う。置いたら環境変数で場所を教える:

    set KURENAI_RENDERDOC_PYMODULE=<renderdoc.pyd を置いたフォルダ>

**`.pyd` は `renderdoc.dll` のABIに依存する。** RenderDocを更新したら `.pyd` も作り直すこと。

## 使い方

    py -3.7 Tools/renderdoc_probe.py capture  --exe <Sample3D.exe> --frame 200 --out <dir>
                                              [--args "-dx12 -scene MaterialTest"]
    py -3.7 Tools/renderdoc_probe.py actions  <capture.rdc> [--limit 80]
    py -3.7 Tools/renderdoc_probe.py textures <capture.rdc> [--event N]
    py -3.7 Tools/renderdoc_probe.py export   <capture.rdc> --resource <id> --out <x.bin> [--event N]
    py -3.7 Tools/renderdoc_probe.py binds    <capture.rdc> --event N

`export` は `texdump_inspect.py` が読めるKTXD v2形式で書き出す。**これが要点で、
RenderDocから採った値もエンジンの `-dumptex` と同じ物差しで測れる**
(実測では `GBufferAlbedo` が両経路でバイト一致した)。

## 注意

- **アプリの起動は1回で済ませる。** `capture` は `-exitafterdump` と併用すると
  アプリが自分で閉じる(ウィンドウを占有する時間が数十秒で済む)
- キャプチャは数十MB〜数百MBになる。使い終わったら消すこと
- **リソースは `-dumptex` と同じ名前で出る**(`GBufferAlbedo` など)。
  エンジンが `IRHITexture::SetDebugName` で焼いており、`--resource` は名前でも
  `ResourceId::562` でも受ける。名前が出ないときは、起動ログの
  「グラフィックスデバッガ向けの名前を付けました: N本」を先に見ること
"""

import argparse
import os
import struct
import sys


# renderdoc.dll を探す既定の場所。**本体は配布版のものをそのまま使える**ので、
# 自前ビルドから持ち回るのは renderdoc.pyd だけでよい(実測で確認済み)
DEFAULT_RENDERDOC_DIRS = [
    r"C:\Program Files\RenderDoc",
    r"C:\Program Files (x86)\RenderDoc",
]


def load_renderdoc():
    """renderdoc モジュールを読み込む。

    要るのは **`renderdoc.pyd` 1つだけ**(6MB)。本体の `renderdoc.dll` は
    インストール済みのものを使う。**ただしバージョンが一致していること** ――
    `.pyd` は `renderdoc.dll` のABIに依存するので、RenderDocを更新したら
    `.pyd` も作り直しになる(用意の仕方は docs/ImplementationDetail.md 63.10)。
    """
    pym = os.environ.get("KURENAI_RENDERDOC_PYMODULE", "")
    if not pym:
        # 環境変数が無ければ、setup スクリプトの既定の置き場所を見る。
        # **ユーザー名を含む絶対パスを書かない** —— %LOCALAPPDATA% から組み立てるので
        # PCが変わっても同じ式で決まる(Tools/renderdoc_setup.ps1 の -Destination と揃えること)
        local = os.environ.get("LOCALAPPDATA", "")
        if local:
            pym = os.path.join(local, "KurenaiEngine", "renderdoc")
    if not pym or not os.path.isfile(os.path.join(pym, "renderdoc.pyd")):
        raise SystemExit(
            "renderdoc.pyd が見つかりません(探した場所: %s)。\n"
            "**このPC用にビルドしてください:**\n"
            "    powershell -NoProfile -ExecutionPolicy Bypass -File Tools\\renderdoc_setup.ps1\n"
            "別の場所に置いてあるなら KURENAI_RENDERDOC_PYMODULE で指してください\n"
            "(根拠と手順は docs/ImplementationDetail.md 63.10)" % (pym or "(未設定)")
        )

    # 本体のDLL。.pyd と同じ場所にあればそれを、無ければインストール先を使う
    dll_dir = pym if os.path.isfile(os.path.join(pym, "renderdoc.dll")) else ""
    if not dll_dir:
        for d in DEFAULT_RENDERDOC_DIRS:
            if os.path.isfile(os.path.join(d, "renderdoc.dll")):
                dll_dir = d
                break
    if not dll_dir:
        raise SystemExit(
            "renderdoc.dll が見つかりません。RenderDocをインストールするか、\n"
            "renderdoc.pyd と同じフォルダへ置いてください"
        )

    sys.path.append(pym)
    os.environ["PATH"] = dll_dir + os.pathsep + os.environ["PATH"]
    if sys.version_info >= (3, 8):
        os.add_dll_directory(dll_dir)

    try:
        import renderdoc as rd
    except ImportError as exc:
        raise SystemExit(
            "renderdoc モジュールを読めません(%s)。\n"
            "**ビルドしたときとまったく同じPythonバージョンで実行すること。** "
            "いまは %s です" % (exc, ".".join(map(str, sys.version_info[:2])))
        )
    return rd


# === texdump_inspect.py と一致させること(KTXD v2) ===
MAGIC = b"KTXD"
HEADER_FIXED = struct.Struct("<4sIIIIIIIIIII")
NAME_BYTES = 64
HEADER_BYTES = 128
# RenderDocのフォーマット名 -> (ElementType, 1成分のバイト数)
# texdump_inspect.py の ELEMENT_TYPES と同じ番号を使う
FORMAT_MAP = {
    "R8G8B8A8_UNORM": (1, 1),
    "R8G8B8A8_TYPELESS": (1, 1),
    "R8G8B8A8_SRGB": (1, 1),
    "R16G16_FLOAT": (2, 2),
    "R16G16B16A16_FLOAT": (2, 2),
    "R32_FLOAT": (3, 4),
    "R32G32_FLOAT": (3, 4),
    "R32G32B32_FLOAT": (3, 4),
    "R32G32B32A32_FLOAT": (3, 4),
    "D32_FLOAT": (3, 4),
    "R32_TYPELESS": (3, 4),
}


def open_capture(rd, path):
    if not os.path.isfile(path):
        raise SystemExit("キャプチャがありません: %s" % path)
    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    cap = rd.OpenCaptureFile()
    result = cap.OpenFile(path, "", None)
    if result != rd.ResultCode.Succeeded:
        raise SystemExit("開けません: %s" % str(result))
    if not cap.LocalReplaySupport():
        raise SystemExit("このキャプチャはローカルで再生できません")
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        raise SystemExit("リプレイを初期化できません: %s" % str(result))
    return cap, controller


def close_capture(rd, cap, controller):
    controller.Shutdown()
    cap.Shutdown()
    rd.ShutdownReplay()


def resource_names(controller):
    """resourceId -> 名前 の対応。エンジンが SetDebugName で付けた名前がここに出る。

    名前が付いていないリソースは `ResourceId::562` のような通し番号でしか識別できず、
    寸法とフォーマットで総当たりするしかなくなる
    (KurenaiEngine3D::ApplyDebugNames が -dumptex と同じ表から名前を焼いている)
    """
    names = {}
    for r in controller.GetResources():
        names[str(r.resourceId)] = str(r.name)
    return names


def walk(actions):
    for a in actions:
        yield a
        for c in walk(a.children):
            yield c


def cmd_capture(args):
    import time

    rd = load_renderdoc()
    os.makedirs(args.out, exist_ok=True)

    opts = rd.CaptureOptions()
    template = os.path.join(args.out, args.name)
    result = rd.ExecuteAndInject(
        args.exe, os.path.dirname(args.exe), args.args, [], template, opts, False)
    if result.result != rd.ResultCode.Succeeded:
        raise SystemExit("起動/注入に失敗: %s" % str(result.result))
    print("起動しました ident=%d" % result.ident)

    tc = rd.CreateTargetControl("", result.ident, "kurenai-shader-debug", True)
    if tc is None:
        raise SystemExit("ターゲット制御に接続できませんでした")
    print("接続: target=%s pid=%d" % (tc.GetTarget(), tc.GetPID()))

    # 【指定フレームを予約する】ホットキーに頼らないので撮る対象が決定的に決まる
    tc.QueueCapture(args.frame, 1)
    print("フレーム %d のキャプチャを予約しました" % args.frame)

    path = None
    deadline = time.time() + args.timeout
    while time.time() < deadline:
        msg = tc.ReceiveMessage(None)
        if msg.type == rd.TargetControlMessageType.NewCapture:
            path = msg.newCapture.path
            print("キャプチャ: frame=%d path=%s" % (msg.newCapture.frameNumber, path))
            break
        if msg.type == rd.TargetControlMessageType.Disconnected:
            print("アプリが終了しました(キャプチャ前)")
            break
        if msg.type == rd.TargetControlMessageType.Busy:
            raise SystemExit("他のクライアントが接続中です: %s" % tc.GetBusyClient())
    tc.Shutdown()

    if path is None:
        raise SystemExit("キャプチャを取得できませんでした(フレーム番号が大きすぎる可能性)")
    print("CAPTURE_OK %s" % path)
    return 0


def cmd_actions(args):
    rd = load_renderdoc()
    cap, controller = open_capture(rd, args.capture)
    acts = list(walk(controller.GetRootActions()))
    print("=== actions ===")
    print("file  : %s" % args.capture)
    print("総数  : %d" % len(acts))
    print("")
    shown = 0
    for a in acts:
        name = a.GetName(controller.GetStructuredFile())
        # ドロー/ディスパッチだけに絞る(状態変更コマンドは数が多く読めない)
        flags = a.flags
        is_work = bool(flags & (rd.ActionFlags.Drawcall | rd.ActionFlags.Dispatch |
                                rd.ActionFlags.MeshDispatch | rd.ActionFlags.Clear |
                                rd.ActionFlags.Copy))
        if not is_work:
            continue
        print("  eid=%-5d %s" % (a.eventId, name))
        shown += 1
        if shown >= args.limit:
            print("  ... (--limit %d で打ち切り)" % args.limit)
            break
    close_capture(rd, cap, controller)
    return 0


def cmd_textures(args):
    rd = load_renderdoc()
    cap, controller = open_capture(rd, args.capture)
    ev = args.event if args.event >= 0 else max(a.eventId for a in walk(controller.GetRootActions()))
    controller.SetFrameEvent(ev, True)
    texs = controller.GetTextures()
    print("=== textures ===")
    print("file  : %s" % args.capture)
    print("event : %d" % ev)
    print("総数  : %d" % len(texs))
    print("")
    names = resource_names(controller)
    print("%-28s %-16s %-10s %-22s %s" % ("name", "resourceId", "size", "format", "mips/slices"))
    for t in texs:
        fmt = str(t.format.Name())
        known = "" if fmt in FORMAT_MAP else "  <- exportは非対応"
        rid = str(t.resourceId)
        print("%-28s %-16s %-10s %-22s %d/%d%s" % (
            names.get(rid, "")[:28], rid, "%dx%d" % (t.width, t.height), fmt,
            t.mips, t.arraysize, known))
    close_capture(rd, cap, controller)
    return 0


def cmd_binds(args):
    """そのイベントで何がバインドされていたか。**値のダンプでは分からない情報**"""
    rd = load_renderdoc()
    cap, controller = open_capture(rd, args.capture)
    controller.SetFrameEvent(args.event, True)
    state = controller.GetPipelineState()
    names = resource_names(controller)
    print("=== binds ===")
    print("file  : %s" % args.capture)
    print("event : %d" % args.event)
    print("")
    # RenderDoc 1.45 は「使われたディスクリプタ」の一覧を返す(UsedDescriptor)。
    # 1つが .access(どのステージのどの番号か)と .descriptor(何を指しているか)を持つ
    for stage in (rd.ShaderStage.Vertex, rd.ShaderStage.Pixel, rd.ShaderStage.Compute):
        used = [u for u in state.GetReadOnlyResources(stage)] + \
               [u for u in state.GetReadWriteResources(stage)]
        if not used:
            continue
        print("--- %s ---" % str(stage))
        for u in used:
            acc = u.access
            desc = u.descriptor
            kind = str(acc.type).replace("DescriptorType.", "")
            rid = str(desc.resource)
            print("  %-16s index=%-4s %-16s %s" % (kind, str(acc.index), rid, names.get(rid, "")))
    # 出力側も Descriptor(.resource がリソースID)。空のスロットは Null が入る
    print("--- 出力 ---")
    empty = str(rd.ResourceId.Null())
    for i, o in enumerate(state.GetOutputTargets()):
        rid = str(o.resource)
        if rid != empty:
            print("  RTV[%d] %-16s %s" % (i, rid, names.get(rid, "")))
    depth = state.GetDepthTarget()
    if depth is not None and str(depth.resource) != empty:
        rid = str(depth.resource)
        print("  DSV    %-16s %s" % (rid, names.get(rid, "")))
    close_capture(rd, cap, controller)
    return 0


def cmd_export(args):
    rd = load_renderdoc()
    cap, controller = open_capture(rd, args.capture)
    ev = args.event if args.event >= 0 else max(a.eventId for a in walk(controller.GetRootActions()))
    controller.SetFrameEvent(ev, True)

    # --resource は resourceId でも名前でも受ける。名前はエンジンが SetDebugName で
    # 焼いたもの(`-dumptex` と同じ表)なので、**両方の経路を同じ呼び名で指せる**
    names = resource_names(controller)
    target = None
    matches = []
    for t in controller.GetTextures():
        rid = str(t.resourceId)
        if rid == args.resource or names.get(rid, "") == args.resource:
            matches.append(t)
    if len(matches) > 1:
        close_capture(rd, cap, controller)
        raise SystemExit(
            "同じ名前のテクスチャが %d 件あります: %s\n"
            "**どれか1つに決められないので resourceId で指定すること**" % (len(matches), args.resource))
    if matches:
        target = matches[0]
    if target is None:
        close_capture(rd, cap, controller)
        raise SystemExit(
            "見つかりません: %s (textures で resourceId と名前の一覧を出せます)" % args.resource)

    fmt = str(target.format.Name())
    if fmt not in FORMAT_MAP:
        close_capture(rd, cap, controller)
        raise SystemExit(
            "このフォーマットは書き出しに対応していません: %s\n"
            "**黙って別の型として書かない** —— 対応表(FORMAT_MAP)に足してから使うこと" % fmt)
    elem_type, bpe = FORMAT_MAP[fmt]
    ch = target.format.compCount

    data = controller.GetTextureData(target.resourceId, rd.Subresource(args.mip, args.slice, 0))
    close_capture(rd, cap, controller)

    width = max(1, target.width >> args.mip)
    height = max(1, target.height >> args.mip)
    expected = width * height * ch * bpe
    if len(data) != expected:
        raise SystemExit(
            "取れたバイト数が合いません(期待 %d, 実際 %d)。フォーマットの解釈が違う可能性" %
            (expected, len(data)))

    # 既定の名前は、エンジンが付けた名前をそのまま使う(無ければresourceId)。
    # texdump_inspect の diff は source名の食い違いを弾くので、
    # `-dumptex` の出力と突き合わせるときに名前が揃っているほうが素直に通る
    name = args.name or names.get(str(target.resourceId), "") or \
        ("rdc_" + str(target.resourceId).replace("::", "_"))
    header = HEADER_FIXED.pack(
        MAGIC, 2, HEADER_BYTES, width, height, ch, elem_type, bpe, ev, args.mip, args.slice,
        0)  # Backend=0(未記録)。RenderDoc経由なのでエンジンのバックエンド欄は埋められない
    name_bytes = name.encode("utf-8")[: NAME_BYTES - 1]
    name_bytes = name_bytes + b"\0" * (NAME_BYTES - len(name_bytes))
    reserved = b"\0" * (HEADER_BYTES - len(header) - NAME_BYTES)

    with open(args.out, "wb") as f:
        f.write(header)
        f.write(name_bytes)
        f.write(reserved)
        f.write(bytes(data))

    print("=== export ===")
    print("resource : %s  %s  %dx%d ch=%d" % (args.resource, fmt, width, height, ch))
    print("event    : %d  mip=%d slice=%d" % (ev, args.mip, args.slice))
    print("out      : %s (%d バイト)" % (args.out, expected))
    print("")
    print("Tools/texdump_inspect.py で読めます(**Python 3.7 ではなく通常のPythonで**)")
    return 0


def main(argv):
    p = argparse.ArgumentParser(description="RenderDocをGUI無しで動かす(Python 3.7で実行)")
    sub = p.add_subparsers(dest="command")

    c = sub.add_parser("capture", help="アプリを起動して指定フレームを撮る")
    c.add_argument("--exe", required=True)
    c.add_argument("--args", default="", help="アプリへ渡す引数(まとめて1つの文字列で)")
    c.add_argument("--frame", type=int, required=True)
    c.add_argument("--out", required=True, help="出力先フォルダ")
    c.add_argument("--name", default="capture", help="ファイル名のテンプレート")
    c.add_argument("--timeout", type=int, default=300)
    c.set_defaults(func=cmd_capture)

    c = sub.add_parser("actions", help="ドロー/ディスパッチの一覧")
    c.add_argument("capture")
    c.add_argument("--limit", type=int, default=80)
    c.set_defaults(func=cmd_actions)

    c = sub.add_parser("textures", help="テクスチャの一覧")
    c.add_argument("capture")
    c.add_argument("--event", type=int, default=-1)
    c.set_defaults(func=cmd_textures)

    c = sub.add_parser("binds", help="そのイベントでのバインド状況")
    c.add_argument("capture")
    c.add_argument("--event", type=int, required=True)
    c.set_defaults(func=cmd_binds)

    c = sub.add_parser("export", help="テクスチャをKTXD v2で書き出す")
    c.add_argument("capture")
    c.add_argument("--resource", required=True)
    c.add_argument("--out", required=True)
    c.add_argument("--event", type=int, default=-1)
    c.add_argument("--mip", type=int, default=0)
    c.add_argument("--slice", type=int, default=0)
    c.add_argument("--name", default="")
    c.set_defaults(func=cmd_export)

    args = p.parse_args(argv)
    if not getattr(args, "func", None):
        p.print_help()
        return 1
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
