// MegaLights の可視灯リスト構築。タイル(16x16ピクセル)ごとに「このフレームで実際に
// 可視だった灯」を集め、重複を除いた固定長リストとして書き出す。
// 次フレームの候補プール(MegaLightsTilePool.hlsl)が、これを提案分布の第3成分として混ぜる。
//
// 【何を解こうとしているか】候補プールの重みは距離減衰だけで決まり、可視性を一切見ていない。
// 影の縁では目標関数を支配する灯が自分からは遮蔽されていることがあり、RIS は毎フレーム
// その灯を選んでは殺される ―― デノイズ前の暗黒点の主因(実測で1フレームあたり点灯画素の3.6%)。
// 「前フレームに見えていた灯」へ標本を寄せれば、その予算が生きた推定に回る。
//
// 【BlockedLights と役割が逆である】あちらは画素ごとに1灯だけ「遮蔽が確定した灯」を覚えて
// 目標関数から**外す**キャッシュ。こちらはタイルごとに「可視だった灯」を覚えて提案分布へ
// **誘導する**。除外と誘導は別の効き方をするので、当面は併存させる
// (統合の可否は、リストが入ったあとに BlockedLights を切って測って決める)。
//
// 【入力は初期サンプリング直後のリザーバ ―― 空間再利用の後ではない】空間再利用の後の
// リザーバは近傍から借りたもので、可視フラグは**別の画素から見た可視性**を指す。
// 「このタイルから見えた灯」という意味を保つため、初期可視レイの結果だけを集める。
//
// 【レイを撃たないので3バリアント(SM 5.0 / SM 6.5 / SM 6.6)すべてで焼ける】
// UE は WaveActiveMin で整列済みリストを作っているが、こちらは groupshared の
// アトミックだけで済ませてあるので wave 組み込み関数に依存しない(退避経路が要らない)。

#include "MegaLightsCommon.hlsli"

// C++側 Passes::MegaLightsVisibleListConstants と並びを一致させること
cbuffer MegaLightsVisibleListConstants : register(b0)
{
    // x=タイル数X, y=同Y, z=1タイルあたりのリスト容量, w=1画素あたりの標本数
    uint4 ListParams;
    // x=レンダー解像度の幅, y=同 高さ, zw=未使用
    uint4 ListSize;
};

StructuredBuffer<MegaLightsReservoir> Reservoirs : register(t0);

RWStructuredBuffer<uint> VisibleLights : register(u0);

// 走査しうるライト番号の上限。groupshared のビットマスクの大きさに使うため
// コンパイル時定数である必要がある。
// **C++側 KurenaiEngine3D.cpp の kMaxLights と必ず同じ値にすること**
static const uint kMegaLightsMaxLights = 1024u;
static const uint kSeenWordCount = kMegaLightsMaxLights / 32u;

static const uint kTileThreadCount = 256u;

// 【重複除去にビットマスクを使う理由】素直な「既存の要素を線形に走査してから追記」は、
// 2つのスレッドが同時に走査して両方とも取りこぼしたときに同じ灯を二重に積む。
// InterlockedOr の戻り値で「自分が最初に立てたか」を見れば競合しても厳密に1回しか積まれず、
// **相異なる灯の数も正確に数えられる**(容量を実測で決めるのにこの数が要る)。
// 1024bit = 32語 = 128バイトで、groupshared の予算から見れば無視できる
groupshared uint gsSeen[kSeenWordCount];
groupshared uint gsList[kMegaLightsVisibleListCapacityMax];
// 打ち切る前に観測された相異なる灯の数。容量を超えることがある
groupshared uint gsDistinct;
// 決定的な選抜の1巡ぶんの勝者(下の VisibleListKey を InterlockedMin で畳んだ値)
groupshared uint gsBestKey;

// 【リストの中身を実行順から切り離すための鍵】
//
// かつてここは InterlockedAdd の戻り値をそのままスロット番号にしていた。原子加算の
// 戻り順は GPU のスレッド実行順で決まるので、**同じビルド・同じ引数で起動し直すだけで
// リストの並びが変わり、候補プールが引く灯が変わり、絵が変わっていた**
// (実測: 同一ビルドの2回で MegaLightsTexture の要素の 86% が相違。
//  -megalightsvisiblelist 0 にするとビット同一になる)。
// 同じ問題を LightCulling.hlsl はライト番号の昇順ソートで潰しており、対策が
// 片方にしか入っていなかった。
//
// 【番号順のソートでは足りない】並びだけ揃えても、容量を超えたときに**どの灯が残るか**が
// 原子加算の勝者(=実行順)のままになる。そこで、集合そのものを鍵で決める。
//
// 【なぜ番号順に若いものから残さないのか】それも決定的だが、若い番号だけが常に残る。
// ライト番号はシーンの記述順で、空間的に偏っていることがある ―― 偏りを入れずに
// 決定的にするため、灯とタイルから作った鍵の小さい順に採る。
// **フレーム番号は混ぜない。** 混ぜると残る集合が毎フレーム変わり、
// 「前フレームに見えていた灯へ寄せる」という狙いに時間方向のちらつきを持ち込む。
//
// 下位10ビットにライト番号そのものを置くのは、鍵を**全順序**にするため
// (同点があると順位が一意に決まらない)。ライト番号は kMegaLightsMaxLights=1024 未満で、
// 10ビットに収まることが上の枝で保証されている
uint VisibleListKey(uint lightIndex, uint2 tile)
{
    uint h = lightIndex * 0x9E3779B1u;
    h ^= tile.x * 0x85EBCA6Bu;
    h ^= tile.y * 0xC2B2AE35u;
    h ^= h >> 15;
    h *= 0x2545F491u;
    h ^= h >> 13;
    // 【最上位ビットを落として 0xFFFFFFFF を作らせない】その値は下の選抜で
    // 「候補が無い」を表す番人として使っている。鍵がたまたま一致すると、
    // 実在する灯を「無い」と読んで黙って取りこぼす
    return (h & 0x7FFFFC00u) | (lightIndex & 0x3FFu);
}

[numthreads(16, 16, 1)]
void CSMain(uint3 groupID : SV_GroupID, uint3 groupThreadID : SV_GroupThreadID, uint groupIndex : SV_GroupIndex)
{
    const uint tileCountX = ListParams.x;
    const uint tileCountY = ListParams.y;
    const uint capacity = min(ListParams.z, kMegaLightsVisibleListCapacityMax);
    const uint samplesPerPixel = max(ListParams.w, 1u);
    const uint2 renderSize = ListSize.xy;

    if (groupID.x >= tileCountX || groupID.y >= tileCountY)
    {
        return;
    }

    // 【クリアを省いてはいけない】groupshared の初期値は未定義で、しかも同じグループが
    // 別のタイルへ使い回される。消さずに数えると前のタイルの灯を「見た」ことにする
    for (uint w = groupIndex; w < kSeenWordCount; w += kTileThreadCount)
    {
        gsSeen[w] = 0u;
    }
    for (uint c = groupIndex; c < kMegaLightsVisibleListCapacityMax; c += kTileThreadCount)
    {
        gsList[c] = kMegaLightsInvalidLight;
    }
    if (groupIndex == 0u)
    {
        gsDistinct = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    const int2 tilePixelOrigin = int2(groupID.xy * 16u);
    const int2 pixel = tilePixelOrigin + int2(groupThreadID.xy);

    if (all(pixel >= int2(0, 0)) && all(pixel < int2(renderSize)))
    {
        const uint reservoirBase = (uint(pixel.y) * renderSize.x + uint(pixel.x)) * samplesPerPixel;

        [loop]
        for (uint s = 0u; s < samplesPerPixel; ++s)
        {
            const MegaLightsReservoir reservoir = Reservoirs[reservoirBase + s];
            const uint packed = reservoir.IndexAndFlags;

            // 可視レイを通った標本だけを集める。殺された標本は可視フラグが落ちている
            if (!MegaLightsUnpackVisible(packed))
            {
                continue;
            }
            // 【メッシュライトを混ぜてはいけない】番号の枠は共通だが、あちらは三角形番号で、
            // 候補プールが扱うのはライト番号。混ぜると無関係な灯を提案することになる
            if (MegaLightsUnpackIsMeshLight(packed))
            {
                continue;
            }
            const uint lightIndex = MegaLightsUnpackLight(packed);
            if (lightIndex >= kMegaLightsMaxLights)
            {
                continue;
            }

            const uint word = lightIndex >> 5u;
            const uint bit = 1u << (lightIndex & 31u);
            uint previous = 0u;
            InterlockedOr(gsSeen[word], bit, previous);
            if ((previous & bit) != 0u)
            {
                // 別のスレッドが既に積んでいる
                continue;
            }

            // 【ここでは数えるだけ。リストへは積まない】原子加算の戻り値をスロット番号に
            // 使うと、並びも「容量を超えたとき残る集合」も実行順で決まってしまう
            // (VisibleListKey のコメント)。**加算した合計そのものは順序に依らない**ので、
            // 相異なる灯の数としては正しく、容量を決める実測にそのまま使える
            uint ignoredSlot = 0u;
            InterlockedAdd(gsDistinct, 1u, ignoredSlot);
            // 【あふれたぶんは黙って捨ててよい】リストは提案分布を*寄せる*ためのもので、
            // 定義域そのものは候補プールの一様枝が押さえている。載らなかった灯にも
            // 正の確率が残るので、捨てても偏らない(効率が落ちるだけ)
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // --- 決定的な選抜: 鍵の小さい順に capacity 個を採る ---
    // 【ビットマスクは順序に依らない】OR は可換なので、gsSeen が表す「相異なる灯の集合」は
    // スレッドの実行順に関係なく同じになる。非決定だったのは gsList への積み方だけなので、
    // 出力はここで集合から作り直す。
    // 【InterlockedMin も順序に依らない】どの順で畳んでも最小値は同じ。
    // 1巡ごとに勝者のビットを落とし、次の最小を採る。巡回数は容量(最大16)で、
    // 1巡あたりの走査は 1024ビット = 32語。
    // 【選ばれた灯はビットを落とす】gsSeen はこの先で使わないので壊してよい
    // (相異なる灯の数は gsDistinct に取ってある)
    for (uint round = 0u; round < capacity; ++round)
    {
        if (groupIndex == 0u)
        {
            gsBestKey = 0xFFFFFFFFu;
        }
        GroupMemoryBarrierWithGroupSync();

        for (uint sw = groupIndex; sw < kSeenWordCount; sw += kTileThreadCount)
        {
            uint bits = gsSeen[sw];
            while (bits != 0u)
            {
                const uint lowBit = firstbitlow(bits);
                bits &= ~(1u << lowBit);
                const uint candidate = (sw << 5u) + lowBit;
                InterlockedMin(gsBestKey, VisibleListKey(candidate, groupID.xy));
            }
        }
        GroupMemoryBarrierWithGroupSync();

        if (groupIndex == 0u && gsBestKey != 0xFFFFFFFFu)
        {
            // 下位10ビットにライト番号を埋めてあるので、勝った鍵から番号を取り出せる
            const uint winner = gsBestKey & 0x3FFu;
            gsList[round] = winner;
            gsSeen[winner >> 5u] &= ~(1u << (winner & 31u));
        }
        GroupMemoryBarrierWithGroupSync();
    }

    // 配置は容量の上限で固定されている(実行時の容量では割らない)。
    // 容量は「1タイルに何個書くか」だけを決め、添字の作り方は一生変わらない
    const uint tileBase = MegaLightsVisibleListBase(groupID.xy, tileCountX);

    if (groupIndex == 0u)
    {
        VisibleLights[tileBase + 0u] = min(gsDistinct, capacity);
        // 打ち切る前の数。提案分布には使わない(容量を実測で決めるための計測専用)
        VisibleLights[tileBase + 1u] = gsDistinct;
    }
    // 【空でも、容量を下げたときでも、常に上限ぶん全部書くこと】RHIにUAVのクリアが無いため、
    // 実行時の容量ぶんしか書かないと、容量を下げた直後に後ろのスロットへ
    // 前の容量で書いた灯が残る。長さ(ヘッダ)で守られてはいるが、
    // 「書かずにreturnしない」という約束はここでも同じ形で守る。
    // gsList は上限ぶん無効で初期化してあるので、溢れた先は無効のまま出る
    for (uint slot = groupIndex; slot < kMegaLightsVisibleListCapacityMax; slot += kTileThreadCount)
    {
        VisibleLights[tileBase + kMegaLightsVisibleListHeader + slot] = gsList[slot];
    }
}
