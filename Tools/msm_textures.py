"""モン・サン=ミシェル島モデル用の、手続き生成タイルテクスチャ(平均1.0の乗算変動場)を
作るモジュール。

設計の要: 各マテリアルの見た目のディテール(石積みの目地、スレートの段差、瓦の丸みなど)は
「ベースカラーに掛け合わせる、平均がちょうど1.0の変動場」として生成する。既存の
ISLAND_MATERIALS のベースカラーは参考写真から較正済みのため、平均1.0を厳密に保てば
遠景での平均的な見た目(色)は変わらないことが保証される。

修正パス(見た目の不合格を受けての作り直し): 初版は解像度512・タイルサイズが小さく
(例: Masonry 512px/6m=85px/m)、実際のレンダ解像度(abbey_closeupで約6.4px/m)まで
縮小すると細かい模様が平均へ潰れ、Cyclesのサンプリングノイズと見分けが付かない
「一様な粒状ノイズ」になってしまっていた。縮小に耐えて遠景でも「石らしさ」を残すのは、
特徴サイズが数メートルある低周波のむら(風化・汚れ・苔・濡れ)だけであるため、
weathering_field()という共通の低周波むら層を全マテリアルへ追加し、そこへエネルギーを
移した。副次的に、UVがワールド空間のボックス投影であることを利用し、この低周波むらが
建物ごとの色調差(全戸が同一色に見える問題)も同時に解決する。あわせて解像度を1024へ
上げ、タイルサイズも実寸のディテール密度を保ったまま拡大し、ピクセル単位の細かいノイズは
振幅を抑えて1/4解像度で生成後に拡大(バンドリミット)することでエイリアシングを抑えている。

このモジュールは bpy に依存しない純粋な生成関数(*_field() および内部の _value_noise 等)と、
bpy を使ってBlenderのImageへ流し込む関数(create_variation_image())を分離している。
前者は Blender の外(素の python + numpy)からでも単体で呼び出して検証できる。

すべてのフィールドは 1024x1024、numpy.float32、タイル可能(左右端・上下端が連続する)。
乱数はすべて固定シード(下記 SEED_* 定数)による決定論的なもので、実行のたびに同じ
結果になる。
"""

import sys

import numpy as np

RESOLUTION = 1024

# --- 固定シード(すべて出典なしの決め値。値そのものに意味は無く、実行のたびに同じ結果に
# なることだけが目的) ---
SEED_ROCK = 101
SEED_ROCK_STREAK = 102
SEED_ROCK_WEATHER = 103
SEED_MASONRY = 201
SEED_MASONRY_FINE = 202
SEED_MASONRY_WEATHER = 203
SEED_RUBBLE = 301
SEED_RUBBLE_WEATHER = 302
SEED_PLASTER = 401
SEED_PLASTER_WEATHER = 402
SEED_SLATE = 501
SEED_SLATE_WEATHER = 502
SEED_TILE_BLOCK = 601
SEED_TILE = 602
SEED_TILE_WEATHER = 603
SEED_LEAD = 701
SEED_LEAD_WEATHER = 702
SEED_CANOPY = 801
SEED_CANOPY_WEATHER = 802
SEED_CANOPY_TINT = 803


def _value_noise(resolution, freq_x, freq_y, seed):
    """周期的な値ノイズ(格子点をラップさせたタイル可能な value noise)。

    freq_x x freq_y 個の格子点にランダム値を置き、格子点間をスムーズステップで
    補間する。格子は freq_x/freq_y でラップしているため、resolution が freq_x/freq_y の
    整数倍でなくても左右端・上下端が連続する(補間の参照先が常にmodで折り返されるため)。

    freq_x と freq_y を変えることで異方性のノイズ(引き伸ばした筋など)を作れる。
    """
    try:
        rng = np.random.RandomState(seed)
        grid = rng.rand(freq_y, freq_x).astype(np.float32)

        xs = np.arange(resolution, dtype=np.float32)
        ys = np.arange(resolution, dtype=np.float32)
        gx = xs / resolution * freq_x
        gy = ys / resolution * freq_y
        gx2d, gy2d = np.meshgrid(gx, gy)

        x0 = np.floor(gx2d).astype(np.int64) % freq_x
        y0 = np.floor(gy2d).astype(np.int64) % freq_y
        x1 = (x0 + 1) % freq_x
        y1 = (y0 + 1) % freq_y

        fx = gx2d - np.floor(gx2d)
        fy = gy2d - np.floor(gy2d)
        # スムーズステップ(補間の継ぎ目を滑らかにする。格子が見えないようにするため必須)
        fx = fx * fx * (3.0 - 2.0 * fx)
        fy = fy * fy * (3.0 - 2.0 * fy)

        v00 = grid[y0, x0]
        v10 = grid[y0, x1]
        v01 = grid[y1, x0]
        v11 = grid[y1, x1]
        top = v00 + (v10 - v00) * fx
        bottom = v01 + (v11 - v01) * fx
        return (top + (bottom - top) * fy).astype(np.float32)
    except Exception as error:  # noqa: BLE001
        print(f"[ERROR] 値ノイズの生成に失敗しました(freq_x={freq_x}, freq_y={freq_y}, seed={seed}): ({error})",
              file=sys.stderr)
        raise


def _fbm(resolution, base_freq, octaves, seed):
    """等方な fractal Brownian motion(オクターブを重ねた値ノイズ)。戻り値は概ね[0,1]、平均は概ね0.5。"""
    try:
        total = np.zeros((resolution, resolution), dtype=np.float32)
        amplitude = 1.0
        amplitude_sum = 0.0
        freq = base_freq
        for i in range(octaves):
            # オクターブごとにシードをずらし、互いに無相関なノイズにする(7919は大きめの素数という
            # 以外に意味の無い出典なしの決め値)
            total += amplitude * _value_noise(resolution, freq, freq, seed + i * 7919)
            amplitude_sum += amplitude
            amplitude *= 0.5
            freq *= 2
        return (total / amplitude_sum).astype(np.float32)
    except Exception as error:  # noqa: BLE001
        print(f"[ERROR] fBmノイズの生成に失敗しました(base_freq={base_freq}, octaves={octaves}, seed={seed}): "
              f"({error})", file=sys.stderr)
        raise


def _band_bounds(resolution, count):
    """resolutionを count 個の帯へ、端数を切り上げ/切り捨てで均等に近く割り振った
    (start, end) のリストを返す。境界は必ず 0 から resolution まで隙間なく連続するため、
    count が resolution の約数でなくても(例: 1024pxを40段に分けるなど)タイルの継ぎ目に
    隙間や重なりが生じない。
    """
    edges = np.round(np.linspace(0, resolution, count + 1)).astype(np.int64)
    return [(int(edges[i]), int(edges[i + 1])) for i in range(count)]


def _resize_periodic(field_small, target_size):
    """タイル可能な(周期境界を持つ)field_smallを、双線形補間でtarget_sizeへ拡大する。

    ピクセル単位の細かいノイズを低解像度で生成してからここで拡大することで、最高周波数を
    下げ(バンドリミット)、遠景での縮小時にちらつく(エイリアシングする)のを防ぐ。
    """
    try:
        small = field_small.shape[0]
        gx = (np.arange(target_size, dtype=np.float32) + 0.5) / target_size * small - 0.5
        gy = (np.arange(target_size, dtype=np.float32) + 0.5) / target_size * small - 0.5
        gx2d, gy2d = np.meshgrid(gx, gy)

        x0 = np.floor(gx2d).astype(np.int64) % small
        y0 = np.floor(gy2d).astype(np.int64) % small
        x1 = (x0 + 1) % small
        y1 = (y0 + 1) % small
        fx = gx2d - np.floor(gx2d)
        fy = gy2d - np.floor(gy2d)

        v00 = field_small[y0, x0]
        v10 = field_small[y0, x1]
        v01 = field_small[y1, x0]
        v11 = field_small[y1, x1]
        top = v00 + (v10 - v00) * fx
        bottom = v01 + (v11 - v01) * fx
        return (top + (bottom - top) * fy).astype(np.float32)
    except Exception as error:  # noqa: BLE001
        print(f"[ERROR] 周期的リサイズに失敗しました(target_size={target_size}): ({error})", file=sys.stderr)
        raise


def _band_limited_fine_noise(size, freq, seed, downsample_factor=4):
    """ピクセル単位の細かいノイズを、size/downsample_factor の解像度で生成してから
    _resize_periodic()でsizeへ拡大する(バンドリミット済みの[0,1]値ノイズ)。
    """
    small = max(1, size // downsample_factor)
    small_noise = _value_noise(small, freq, freq, seed)
    return _resize_periodic(small_noise, size)


def weathering_field(size, seed, cells_low=3, cells_mid=7, target_std=0.22):
    """低周波の「風化むら」層(タイル内3x3セル + 7x7セル(振幅半分)、平滑補間)。

    遠景で縮小されても潰れずに残る、数メートル規模のむら(風化・汚れ・苔・濡れを想定)。
    ワールド空間のボックス投影UVと組み合わさることで、建物ごとの色調差も生む。
    合成後の標準偏差(1.0からの偏差)をtarget_stdへ厳密に合わせて返す(乱数の引きに
    よらず狙った振幅を保証するため)。

    【修正・実機の石が写真の1/3の変動しかなかった】参考写真と実機の同じ領域を、島の幅で
    正規化した同じ倍率で測ると(値は輝度の変動係数。括弧内は平均する画素数):
                     写真 CV(1/2/4/8px)        実機 CV(1.5/3/6/12px)
      裸の花崗岩      0.465 0.397 0.328 0.289   0.130 0.127 0.116 0.107
      城壁(石積み)    0.546 0.484 0.425 0.318   0.120 0.117 0.114 0.100
      修道院の石壁    0.520 0.462 0.399 0.340   0.160 0.154 0.143 0.132
    縮小に耐えて遠景まで残るのはこの低周波の層だけなので(細かいノイズは平均へ潰れる)、
    ここを上げるのが低周波では最も効く。ただし上げすぎると「石」ではなく「大きな染み」に
    見えるため、0.28で一度試したあと0.22へ戻した。石の造形(明暗)の主成分は
    テクスチャではなく太陽の向きで、方位を270度(カメラ真後ろの順光)から210度へ変えると
    石積の変動係数は0.137→0.260になった(テクスチャの寄与は0.094→0.137)。
    ※写真側の変動には窓・狭間・控え壁の陰影など立体の起伏も含まれるため、
      テクスチャだけで写真のCVに並ぶわけではない。狙いは1/3を1/2強まで戻すこと
    """
    try:
        low = _value_noise(size, cells_low, cells_low, seed)
        mid = _value_noise(size, cells_mid, cells_mid, seed + 1)
        c_low = (low - 0.5) * 2.0
        c_mid = (mid - 0.5) * 2.0
        combined = c_low + 0.5 * c_mid

        combined = combined - combined.mean()
        std = combined.std()
        if std > 1e-8:
            combined = combined / std * target_std

        return (1.0 + combined).astype(np.float32)
    except Exception as error:  # noqa: BLE001
        print(f"[ERROR] weathering_field()の生成に失敗しました(seed={seed}): ({error})", file=sys.stderr)
        raise


def _apply_block_pattern(field, resolution, rows, cols, seed, mult_range, mortar_px, joint_mult,
                          offset_mode, offset_rng=None):
    """段(row)x列(col)のブロックパターン(石積み・スレート等の共通処理)を field に乗算で焼き込む。

    offset_mode:
      "half"   : 1段おきに半ブロック分ずらす(布積み)
      "random" : 段ごとにランダムな量ずらす(乱石積み)
      "none"   : ずらさない
    """
    try:
        rng = np.random.RandomState(seed)
        row_bounds = _band_bounds(resolution, rows)
        col_width = resolution / float(cols)

        for row_index, (y0, y1) in enumerate(row_bounds):
            if offset_mode == "half":
                offset = col_width / 2.0 if (row_index % 2 == 1) else 0.0
            elif offset_mode == "random":
                offset = rng.uniform(0.0, col_width)
            else:
                offset = 0.0

            row_ys = np.arange(y0, y1)
            for col_index in range(cols):
                mult = rng.uniform(mult_range[0], mult_range[1])
                x_start = col_index * col_width + offset
                x_end = x_start + col_width
                xs = np.arange(int(round(x_start)), int(round(x_end))) % resolution
                field[np.ix_(row_ys, xs)] *= mult

                if mortar_px > 0 and joint_mult is not None:
                    # 縦目地(ブロックの左端に沿って乗せる)
                    joint_xs = np.arange(int(round(x_start)), int(round(x_start)) + mortar_px) % resolution
                    field[np.ix_(row_ys, joint_xs)] *= joint_mult

        if mortar_px > 0 and joint_mult is not None:
            # 横目地(各段の下端)
            for (y0, y1) in row_bounds:
                field[y0:min(y0 + mortar_px, y1), :] *= joint_mult

        return field
    except Exception as error:  # noqa: BLE001
        print(f"[ERROR] ブロックパターンの生成に失敗しました(rows={rows}, cols={cols}, seed={seed}): ({error})",
              file=sys.stderr)
        raise


def rock_field():
    """花崗岩。3オクターブの値ノイズfBmに、縦方向へ引き伸ばした筋(V方向に4倍引き伸ばし=
    水平方向の周波数の1/4)を重ねる。振幅は±22%程度。暗い側(裂け目)へ強めに裾を引かせる。
    さらに低周波の風化むら層を掛ける(遠景での「石らしさ」の主成分)。

    base_freq/streak周波数は、タイルサイズがMATERIAL_UV_TILE_METERSで30m→45mへ拡大された
    のに合わせ、特徴の実寸(メートル単位のスケール)を保つよう1.5倍(45/30)した
    (出典なしの決め値の換算)。
    """
    try:
        base = _fbm(RESOLUTION, base_freq=9, octaves=3, seed=SEED_ROCK)  # [0,1]、平均0.5
        # V方向(縦)に4倍引き伸ばした筋。freq_y=freq_x/4 とすることで縦に伸びた模様になる
        streak = _value_noise(RESOLUTION, freq_x=24, freq_y=6, seed=SEED_ROCK_STREAK)

        c_base = (base - 0.5) * 2.0        # [-1, 1]
        c_streak = (streak - 0.5) * 2.0    # [-1, 1]

        # 暗い側(裂け目)への裾引き: 負側は0.7乗で伸ばし、正側は1.4乗で抑える
        # (べき指数は出典なしの決め値)。np.power に負数を渡さないよう符号と絶対値に分けて計算する
        sign = np.sign(c_base)
        mag = np.abs(c_base)
        skewed = sign * np.where(c_base < 0.0, np.power(mag, 0.7), np.power(mag, 1.4))

        # 基礎ノイズと筋ノイズの合成比は出典なしの決め値
        # 【修正】参考写真の花崗岩の崖(正規化x0.19〜0.40)は縦方向の割れ目が目立つので、
        # 筋の比を0.25→0.45へ上げ、全体の振幅も0.22→0.34にする
        # (weathering_fieldのdocstringの実測表を参照)
        combined = 0.55 * skewed + 0.45 * c_streak
        field = 1.0 + 0.34 * combined

        field *= weathering_field(RESOLUTION, SEED_ROCK_WEATHER)

        field = field / field.mean()
        return field.astype(np.float32)
    except Exception as error:  # noqa: BLE001
        print(f"[ERROR] rock_field()の生成に失敗しました: ({error})", file=sys.stderr)
        raise


def masonry_field():
    """切石の布積み(running bond ashlar)。1タイル40段×20列(タイル15m、段高さ0.375m・
    ブロック幅0.75mを維持)、1段おきに半個ずらす。目地は幅4ピクセル・乗数0.78。
    ブロックごとに[0.85,1.15]のばらつき。ピクセル単位の細かいノイズは振幅を±2.5%
    (初版の半分)に抑え、1/4解像度で生成して拡大することでエイリアシングを防ぐ。
    さらに低周波の風化むら層を掛ける。
    """
    try:
        field = np.ones((RESOLUTION, RESOLUTION), dtype=np.float32)
        field = _apply_block_pattern(
            field, RESOLUTION, rows=40, cols=20, seed=SEED_MASONRY,
            # 【修正】ブロックごとのばらつきと目地の暗さを上げる(weathering_fieldの実測表を参照)
            mult_range=(0.72, 1.28), mortar_px=4, joint_mult=0.62, offset_mode="half",
        )
        fine = _band_limited_fine_noise(RESOLUTION, freq=32, seed=SEED_MASONRY_FINE)
        field *= (1.0 + 0.025 * (fine - 0.5) * 2.0)

        field *= weathering_field(RESOLUTION, SEED_MASONRY_WEATHER)

        field /= field.mean()
        return field.astype(np.float32)
    except Exception as error:  # noqa: BLE001
        print(f"[ERROR] masonry_field()の生成に失敗しました: ({error})", file=sys.stderr)
        raise


def rubble_field():
    """乱石積み(rubble wall)。1タイル32段×24列(タイル12m)、段ごとにランダムなずれ。
    ブロックごとに[0.80,1.20]とばらつきを大きくし、目地は乗数0.80・幅4ピクセル。
    さらに低周波の風化むら層を掛ける。
    """
    try:
        field = np.ones((RESOLUTION, RESOLUTION), dtype=np.float32)
        field = _apply_block_pattern(
            field, RESOLUTION, rows=32, cols=24, seed=SEED_RUBBLE,
            # 【修正】切石より乱れが大きいので、切石よりさらに広く振る
            mult_range=(0.68, 1.34), mortar_px=4, joint_mult=0.62, offset_mode="random",
        )

        field *= weathering_field(RESOLUTION, SEED_RUBBLE_WEATHER)

        field /= field.mean()
        return field.astype(np.float32)
    except Exception as error:  # noqa: BLE001
        print(f"[ERROR] rubble_field()の生成に失敗しました: ({error})", file=sys.stderr)
        raise


def plaster_field():
    """漆喰。低周波のまだら(2オクターブ)、±10%。目地は無し。低周波の風化むら層を掛ける
    (漆喰の汚れ・苔じみをこの層が担うため、既存のまだらと合わせて二重に低周波成分を持つ)。

    base_freqは、タイルサイズが5m→12mへ拡大されたのに合わせ、特徴の実寸を保つよう
    2.4倍(12/5)した(出典なしの決め値の換算)。
    """
    try:
        base = _fbm(RESOLUTION, base_freq=10, octaves=2, seed=SEED_PLASTER)
        c = (base - 0.5) * 2.0
        field = 1.0 + 0.10 * c

        field *= weathering_field(RESOLUTION, SEED_PLASTER_WEATHER)

        field /= field.mean()
        return field.astype(np.float32)
    except Exception as error:  # noqa: BLE001
        print(f"[ERROR] plaster_field()の生成に失敗しました: ({error})", file=sys.stderr)
        raise


def slate_field():
    """スレート葺き。1タイル40段×26枚(タイル8m、1枚の丈0.2mを維持)、1段おきに半枚ずらす。
    各段の下端(段の開始側、V=0に近い側。詳細はcreate_variation_image()のY方向の規約を
    参照)に暗い線(乗数0.7・4ピクセル)。スレート1枚ごとに[0.82,1.18]のばらつき。
    さらに低周波の風化むら層を掛ける。
    """
    try:
        resolution = RESOLUTION
        rows, cols = 40, 26
        field = np.ones((resolution, resolution), dtype=np.float32)
        field = _apply_block_pattern(
            field, resolution, rows=rows, cols=cols, seed=SEED_SLATE,
            mult_range=(0.82, 1.18), mortar_px=0, joint_mult=None, offset_mode="half",
        )
        row_bounds = _band_bounds(resolution, rows)
        dark_px = 4
        for (y0, y1) in row_bounds:
            field[y0:min(y0 + dark_px, y1), :] *= 0.7

        field *= weathering_field(resolution, SEED_SLATE_WEATHER)

        field /= field.mean()
        return field.astype(np.float32)
    except Exception as error:  # noqa: BLE001
        print(f"[ERROR] slate_field()の生成に失敗しました: ({error})", file=sys.stderr)
        raise


def tile_field():
    """瓦(スパニッシュ瓦風)。1タイル20段×32枚(タイル8m)。縦(U)方向には正弦波で丸瓦の
    陰影(±15%)を付ける。段の境界に暗い線。瓦ごとの乗数[0.88,1.12]。さらに低周波の
    風化むら層を掛ける。

    col_width=1024/32=32ピクセルはRESOLUTIONの約数のため、タイルの継ぎ目でも正弦波の
    位相が連続する。
    """
    try:
        resolution = RESOLUTION
        rows = 20
        cols = 32
        col_width = resolution / float(cols)

        field = np.ones((resolution, resolution), dtype=np.float32)

        # U方向の正弦波(丸瓦の陰影)
        xs = np.arange(resolution, dtype=np.float32)
        sinusoid = np.sin(2.0 * np.pi * xs / col_width)
        field *= (1.0 + 0.15 * sinusoid)[np.newaxis, :]

        # 瓦ごとの乗数
        rng = np.random.RandomState(SEED_TILE_BLOCK)
        row_bounds = _band_bounds(resolution, rows)
        for (y0, y1) in row_bounds:
            for col_index in range(cols):
                mult = rng.uniform(0.88, 1.12)
                x0 = int(round(col_index * col_width))
                x1 = int(round((col_index + 1) * col_width))
                field[y0:y1, x0:x1] *= mult

        # 段の境界の暗い線(乗数は明記が無いため出典なしの決め値)
        dark_px = 4
        for (y0, y1) in row_bounds:
            field[y0:min(y0 + dark_px, y1), :] *= 0.75

        field *= weathering_field(resolution, SEED_TILE_WEATHER)

        field /= field.mean()
        return field.astype(np.float32)
    except Exception as error:  # noqa: BLE001
        print(f"[ERROR] tile_field()の生成に失敗しました: ({error})", file=sys.stderr)
        raise


def lead_field():
    """鉛葺き。U方向へ16本のはぜ(暗い縦線、乗数0.75、幅6ピクセル、0.5m間隔・タイル8m)+
    ±3%(初版の半分)の緩いノイズ。緩いノイズは1/4解像度で生成して拡大しエイリアシングを
    防ぐ。さらに低周波の風化むら層を掛ける。
    """
    try:
        resolution = RESOLUTION
        field = np.ones((resolution, resolution), dtype=np.float32)

        seam_count = 16
        seam_px = 6
        spacing = resolution / float(seam_count)
        for i in range(seam_count):
            x0 = int(round(i * spacing))
            xs = np.arange(x0, x0 + seam_px) % resolution
            field[:, xs] *= 0.75

        small = resolution // 4
        noise_small = _fbm(small, base_freq=3, octaves=2, seed=SEED_LEAD)
        noise = _resize_periodic(noise_small, resolution)
        c = (noise - 0.5) * 2.0
        field *= (1.0 + 0.03 * c)

        field *= weathering_field(resolution, SEED_LEAD_WEATHER)

        field /= field.mean()
        return field.astype(np.float32)
    except Exception as error:  # noqa: BLE001
        print(f"[ERROR] lead_field()の生成に失敗しました: ({error})", file=sys.stderr)
        raise


def canopy_field():
    """樹冠。塊状のノイズ(2オクターブ、周波数高め)。振幅±35%で暗い側(葉の隙間)へ
    強く裾を引かせる。さらに低周波の風化むら層を掛ける(木ごとの色調差にもなる)。

    base_freqは、タイルサイズが6m→12mへ拡大されたのに合わせ、特徴の実寸を保つよう
    2倍(12/6)した(出典なしの決め値の換算)。
    """
    try:
        base = _fbm(RESOLUTION, base_freq=32, octaves=2, seed=SEED_CANOPY)
        c = (base - 0.5) * 2.0
        sign = np.sign(c)
        mag = np.abs(c)
        # 負側(暗)は0.5乗で強く伸ばし、正側(明)は1.6乗で抑える(出典なしの決め値)
        skewed = sign * np.where(c < 0.0, np.power(mag, 0.5), np.power(mag, 1.6))
        # 【修正・樹冠の明暗の幅が写真の半分だった】材質IDで樹冠の画素を特定して最終画の
        # 輝度を測ると p90/p10 は 写真3.50 対 実機1.82。葉の隙間の暗がりが足りない
        field = 1.0 + 0.62 * skewed

        field *= weathering_field(RESOLUTION, SEED_CANOPY_WEATHER)

        field /= field.mean()
        return field.astype(np.float32)
    except Exception as error:  # noqa: BLE001
        print(f"[ERROR] canopy_field()の生成に失敗しました: ({error})", file=sys.stderr)
        raise


def canopy_tint_field():
    """樹冠の色を2色のあいだで混ぜるための[0,1]の低周波マスク。

    【なぜ要るか】既存の変動場はすべて「ベースカラーに掛けるスカラー」なので、明暗しか
    変えられず色相は動かない。参考写真の樹林を測ると R/G の中央が0.831・p10〜p90の幅が
    0.291あり(実機は中央0.692・幅0.172)、日向の黄緑から日陰の青緑まで色相が振れている。
    スカラーの変動場ではこれを再現できないため、2色を混ぜるマスクを別に持つ。

    タイルは12mでワールド空間のボックス投影なので、3x3セル(=4m)の低周波にすると
    木ごと・枝ごとにゆっくり色が変わる(1本の中でも多少変わる)。出典なしの決め値
    """
    try:
        low = _value_noise(RESOLUTION, 3, 3, SEED_CANOPY_TINT)
        mid = _value_noise(RESOLUTION, 7, 7, SEED_CANOPY_TINT + 1)
        blend = 0.7 * low + 0.3 * mid
        blend = (blend - blend.min()) / max(float(blend.max() - blend.min()), 1e-8)
        return blend.astype(np.float32)
    except Exception as error:  # noqa: BLE001
        print(f"[ERROR] canopy_tint_field()の生成に失敗しました: ({error})", file=sys.stderr)
        raise


# 色相を振るためのマスク(FIELD_FUNCTIONSとは別。乗算ではなく2色の混合率として使う)
TINT_FIELD_FUNCTIONS = {
    "canopy_tint": canopy_tint_field,
}


# フィールド名(MATERIAL_TEXTURE_FIELDSの値)から生成関数を引く対応表
FIELD_FUNCTIONS = {
    "rock": rock_field,
    "masonry": masonry_field,
    "rubble": rubble_field,
    "plaster": plaster_field,
    "slate": slate_field,
    "tile": tile_field,
    "lead": lead_field,
    "canopy": canopy_field,
}


# 法線マップの強さ(材質名 -> 高さの倍率)。変動場を高さとみなしたときの起伏の深さで、
# 数値そのものに物理的な意味は無い(出典なしの決め値)。
#
# 【なぜ法線マップが要るか】幾何の無い平らな面だけを切り出して、島の幅を揃えてから
# 局所コントラスト(局所標準偏差の中央値÷島の平均輝度、窓7画素)を測ると:
#     城壁の素の壁面  参考写真 0.378 / 実機 0.118  (3.2倍)
#     裸の花崗岩      参考写真 0.448 / 実機 0.131  (3.4倍)
# 面そのものが平らで、狭間や塔を足しても壁の高さの2割弱しか占めないので届かない。
# かといってアルベドの振幅を上げるのは筋が悪い。写真の変動の主成分は岩の凹凸が作る
# **陰影**で、アルベドで真似ると光の向きに関係なく染みが焼き付いた面になる
# (weathering_fieldのdocstring参照。実際に0.28まで上げて「大きな染み」に見え0.22へ戻した)。
# 法線マップなら光の向きに応じた陰影が出る。太陽を仰角15度へ下げた直後なので噛み合う。
# 材質名 -> (起伏の深さ[m], タイルの実寸[m])。タイルの実寸は
# blender_msm_island.MATERIAL_UV_TILE_METERS と一致させること(勾配をメートルで取るため)。
# 深さは出典なしの決め値だが、根拠の軸は「その材質の凹凸の実際の深さ」:
#   花崗岩 0.5m  … 露岩の割れ目・転石の段差
#   切石   0.12m … 目地の彫り込みと石の面のばらつき
#   乱石積 0.18m … 石が不揃いに出入りするぶん切石より深い
#
# 【深さを物理的にありうる範囲へ戻した】以前は局所コントラストだけを根拠に
# 岩8m→16m・石積1.6m・乱石積2.0mまで上げていたが、これは上の「実際の凹凸の深さ」の
# 13〜32倍で、傾きが上限60度に張り付いた画素が岩で23.4%・石積で21.9%あった。
# 上限に張り付いた法線は高さ場の勾配ではなく**向きの揃わない斑**になり、
# 面が「起伏のある石」ではなく「ざらついたノイズ」に見える。
#
# 傾きの分布(1024画素タイル。中央値 / p90 / 60度で頭打ちの割合):
#   岩   深さ 0.5m  2.0度 /  4.2度 / 0.0%    深さ 4.0m 15.7度 / 30.4度 / 0.0%
#        深さ 8.0m 29.3度 / 49.6度 / 1.8%    深さ16.0m 48.3度 / 66.9度 / 23.4%
#   石積 深さ0.12m  1.4度 / 60.0度 / 10.0%   深さ 1.6m 17.6度 / 87.5度 / 21.9%
#   乱石 深さ0.18m  2.5度 / 72.2度 / 17.3%   深さ 2.0m 25.7度 / 88.3度 / 20.4%
#
# 岩は4.0mを採る。45mのタイルに対し起伏4mは自然の花崗岩の斜面として無理が無く、
# **頭打ちが0.0%**で法線が高さ場の勾配のまま残る(中央15.7度・p90 30.4度)。
# 石積・乱石積は上の実寸(0.12m / 0.18m)へ戻す。この2つは深さを下げても頭打ちが
# 10〜17%残るが、それは目地そのものが急峻だからで正しい ——切石の目地は幅6cmに対し
# 深さ12cmの垂直な彫り込みで、実際に60度を超える。深さを上げると目地ではなく
# **石の面まで傾き**(1.6mでは面の中央値が17.6度)、平らであるべき切石が波打つ。
NORMAL_MAP_PARAMS = {
    "rock":    (4.00, 45.0),
    "masonry": (0.12, 15.0),
    "rubble":  (0.18, 12.0),
}
# 法線の傾きの上限(tan)。目地は幅4画素で急峻なため、そのままだと勾配が桁で跳ねて
# 直立した壁のようになる。tan(60度)=1.73で頭を打たせる
NORMAL_MAP_MAX_SLOPE = 1.73


def field_to_normal_map(field, depth_meters, tile_meters):
    """変動場(平均1.0前後の乗数)を高さとみなして、接空間の法線マップRGB([0,1])を作る。

    石は「暗いところ=凹んでいるところ」(裂け目・目地・影の溜まり)なので、
    変動場をそのまま高さとして使える。タイルは繰り返すので勾配は周期境界で取る。

    【勾配はメートルで取る】最初は隣接**画素**の差をそのまま傾きにしていたが、
    1024画素が45mのタイルなので1画素=0.044m、変動±0.34が5m幅に広がっている場合の
    傾きは約1度にしかならず、**法線マップを入れても絵がまったく変わらなかった**
    (局所コントラスト 0.1177→0.1180)。画素ではなく実寸で微分する。

    depth_meters: 変動場の振幅1.0ぶんを何メートルの起伏とみなすか。
    tile_meters: このテクスチャが貼られる1タイルの実寸(MATERIAL_UV_TILE_METERSと同じ値)。

    返すのは float32 の (H, W, 3)。**リニア色空間**の値なので、Blender側では
    colorspace を 'Non-Color' にすること(sRGBデコードされると法線が歪む)。

    Y方向の規約は create_variation_image と同じ(fieldの行インデックスが増える方向が
    画像の下から上)。glTF/Blenderの接空間法線は +Y が上なので、行方向の勾配は
    そのままYへ入れてよい。
    """
    try:
        size = field.shape[0]
        pixels_per_meter = size / float(tile_meters)
        height = field.astype(np.float64) * depth_meters
        # 周期境界の中心差分[m/画素] → メートルあたりの傾きへ直す
        dx = (np.roll(height, -1, axis=1) - np.roll(height, 1, axis=1)) * 0.5 * pixels_per_meter
        dy = (np.roll(height, -1, axis=0) - np.roll(height, 1, axis=0)) * 0.5 * pixels_per_meter

        slope = np.sqrt(dx * dx + dy * dy)
        scale = np.where(slope > NORMAL_MAP_MAX_SLOPE, NORMAL_MAP_MAX_SLOPE / np.maximum(slope, 1e-9), 1.0)
        dx, dy = dx * scale, dy * scale

        nx, ny = -dx, -dy
        nz = np.ones_like(height)
        norm = np.sqrt(nx * nx + ny * ny + nz * nz)
        nx, ny, nz = nx / norm, ny / norm, nz / norm

        print(f"[INFO] 法線マップ: 深さ{depth_meters}m/タイル{tile_meters}m "
              f"傾きの中央値={np.degrees(np.arctan(np.median(slope))):.1f}度 "
              f"上限で頭打ち={float((slope > NORMAL_MAP_MAX_SLOPE).mean()):.1%}")
        rgb = np.stack([nx * 0.5 + 0.5, ny * 0.5 + 0.5, nz * 0.5 + 0.5], axis=2)
        return np.clip(rgb, 0.0, 1.0).astype(np.float32)
    except Exception as error:  # noqa: BLE001
        print(f"[ERROR] field_to_normal_map()の生成に失敗しました"
              f"(depth={depth_meters}, tile={tile_meters}): ({error})", file=sys.stderr)
        raise


def create_normal_image(name, field, depth_meters, tile_meters):
    """変動場から法線マップのBlender画像を作る(colorspaceは'Non-Color')。

    create_variation_imageと同じく、同名の画像があれば作り直さず再利用する。
    colorspaceはpixels代入より**先に**設定する(後から設定するとBlenderがバッファを
    再読み込みして書き込んだ値が失われる。create_variation_imageのコメント参照)。
    """
    try:
        import bpy
    except Exception as error:  # noqa: BLE001
        print(f"[ERROR] bpyのインポートに失敗しました(Blender内でのみ呼び出せます): ({error})",
              file=sys.stderr)
        raise

    try:
        rgb = field_to_normal_map(field, depth_meters, tile_meters)
        h, w, _ = rgb.shape
        alpha = np.ones((h, w, 1), dtype=np.float32)
        rgba = np.concatenate([rgb, alpha], axis=2).astype(np.float32)

        image = bpy.data.images.get(name)
        if image is None:
            image = bpy.data.images.new(name, w, h, alpha=False)
        image.colorspace_settings.name = 'Non-Color'
        image.pixels[:] = rgba.reshape(-1).tolist()
        image.file_format = 'PNG'
        image.pack()
        return image
    except Exception as error:  # noqa: BLE001
        print(f"[ERROR] 法線マップ画像({name})の作成に失敗しました: ({error})", file=sys.stderr)
        raise


def _linear_to_srgb(v):
    """リニア値([0,1]にクランプ済み想定)をsRGBエンコードする。"""
    v = np.clip(v, 0.0, 1.0)
    return np.where(v > 0.0031308, 1.055 * np.power(v, 1.0 / 2.4) - 0.055, 12.92 * v)


def create_variation_image(name, field, base_color_linear, tint_color_linear=None, tint_field=None):
    """変動場fieldとベースカラー(リニアRGB)から、Blenderの画像(sRGBエンコード済み)を作る。

    同名の画像が既に存在する場合は作り直さず再利用する(build_island()が複数回
    呼ばれてもRock.001のような重複を作らないため)。

    なぜsRGBエンコードして書き込むのか:
    Blenderは画像バッファをcolorspaceの指定に従ってscene-linearへ変換して使う。
    'sRGB'指定のバッファにsRGBエンコード済みの値を入れればBlender側は正しくデコードする。
    そしてglTFのbaseColorTextureはsRGBエンコードされている前提のため、このバッファが
    そのままPNGとして書き出されればエンジン側のデコードとも一致する。リニア値を入れて
    'sRGB'のままにすると二重デコードで暗くなってしまう。

    Y方向の規約: field[0, :](配列の最初の行)をimage.pixelsの先頭(=画像の下端。
    Blenderのpixelsは下端から上へ並ぶ)へそのまま書き込む。つまり「fieldの行インデックスが
    増える方向」=「画像の下から上」。UV投影側(_apply_box_projection_uv)ではワールドの
    高さ(Z)が大きいほどUVのvが大きくなるので、この規約により石積みの段・スレートの段は
    fieldの行インデックスが増える方向(=高さが増す方向)へ積み上がって見える。
    """
    try:
        import bpy  # bpyへの依存はこの関数内に閉じ、他の生成関数は素のpythonで動く
    except Exception as error:  # noqa: BLE001
        print(f"[ERROR] bpyのインポートに失敗しました(Blender内でのみ呼び出せます): ({error})", file=sys.stderr)
        raise

    try:
        height, width = field.shape
        color = np.array(base_color_linear, dtype=np.float32).reshape(1, 1, 3)
        if tint_color_linear is not None and tint_field is not None:
            # 2色のあいだをtint_field(0〜1)で混ぜてから明暗の変動場を掛ける。
            # 混合は平均0.5になるとは限らないので、遠景の平均色は2色の中間あたりになる
            tint = np.array(tint_color_linear, dtype=np.float32).reshape(1, 1, 3)
            t = tint_field[:, :, np.newaxis]
            color = color * (1.0 - t) + tint * t
        linear_rgb = np.clip(field[:, :, np.newaxis] * color, 0.0, 1.0)
        srgb_rgb = _linear_to_srgb(linear_rgb)
        alpha = np.ones((height, width, 1), dtype=np.float32)
        rgba = np.concatenate([srgb_rgb, alpha], axis=2).astype(np.float32)

        image = bpy.data.images.get(name)
        if image is None:
            image = bpy.data.images.new(name, width, height, alpha=False)

        # 修正パス(実測で確認済みの不具合): colorspace_settings.nameをpixels代入の後に
        # 設定すると、Blenderがバッファを再読み込み(生成画像には元データが無いため空/黒へ
        # リセット)してしまい、書き込んだピクセルが失われる(RGBが全て0になる)。
        # 必ずpixels代入より先にcolorspaceを設定すること
        image.colorspace_settings.name = 'sRGB'

        # Blender 2.82のImage.pixelsにはforeach_set/foreach_getが無いためスライス代入する
        image.pixels[:] = rgba.reshape(-1).tolist()
        image.file_format = 'PNG'
        image.pack()
        return image
    except Exception as error:  # noqa: BLE001
        print(f"[ERROR] 変動テクスチャ画像({name})の作成に失敗しました: ({error})", file=sys.stderr)
        raise
