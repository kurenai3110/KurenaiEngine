// MegaLights の各パスが共有するリザーバ(reservoir)の定義。
//
// 【リザーバとは何か】「候補の中から1つだけ残した結果」を1画素ぶん保持する箱。
// 残したライト番号と、その1灯だけで全灯ぶんの寄与を推定するための重み W、
// そして「何個の候補から絞ったか」を表す M を持つ。
// 時間・空間の再利用は、この箱どうしを結合していく形で行う。
//
// 【なぜ色ではなくリザーバを持ち回るのか】前フレームの「色」を混ぜると、
// 遮蔽物が動いたときに古い明るさが残り続ける。リザーバなら「どの灯を選んだか」が
// 分かるので、現フレームのその画素で改めて評価し直せる(影レイも撃ち直せる)。
//
// 【インクルードする側の責務】このヘッダーより前に struct GPULight
// (PunctualLighting.hlsli)を用意しておくこと。

#ifndef KURENAI_MEGALIGHTS_COMMON_HLSLI
#define KURENAI_MEGALIGHTS_COMMON_HLSLI

// 無効なライト番号。リザーバが空であることを表す
static const uint kMegaLightsInvalidLight = 0xFFFFu;

// --- 候補プールのレイアウト(MegaLightsTilePool.hlsl が書き、Initial と Spatial が読む) ---
//
// 【1か所に集めてある理由】書き手と読み手が別ファイルにあり、以前は各ファイルが
// 「4 + 2K」を直接書いていた。ヘッダを1つ増やすだけで全部を直す必要があり、
// **片方だけ直すと無関係な値を候補として読む**(絵は出るので気付けない)。
//
//   [base + 0] = asuint(SumW)          そのタイルに届く全灯の重みの合計
//   [base + 1] = 届いたライト数         (デバッグ表示と検証用)
//   [base + 2] = 有効な候補数           (0 か K)
//   [base + 3] = 予約
//   [base + 4] = asfloat(nearestViewZ) タイル内で最も手前のサーフェスのView空間Z
//   [base + 5] = asfloat(farthestViewZ) 同 最も奥
//   以降、候補1つにつき2要素(ライト番号と重み)
//
// **C++側 KurenaiEngine3D::kMegaLightsTilePoolStride と必ず一致させること。**
static const uint kMegaLightsTilePoolHeader = 6u;

uint MegaLightsTilePoolBase(uint2 tileCoord, uint tileCountX, uint candidateCount)
{
    return (tileCoord.y * tileCountX + tileCoord.x) * (kMegaLightsTilePoolHeader + 2u * candidateCount);
}

// 1画素ぶんのリザーバ。**C++側の確保(16バイト/画素)と一致させること。**
// GPULightと同じく、パッキング規則の解釈揺れを避けるため要素はすべて32bit単位で持つ
struct MegaLightsReservoir
{
    // 下位16bit = ライト番号(kMegaLightsInvalidLight で空)、
    // 上位16bit = フラグ。bit0 = 初期可視レイで生き残った
    uint LightAndFlags;
    // 光源面上のどこを選んだか(fp16 x2)。punctual の現段階では常に0で、
    // 光源に半径が入る段階で使う。ここを空けておかないと後でレイアウトを変えることになる
    uint SampleUV;
    // 不偏寄与重み W = (1/p̂(y)) * (1/M) * Σw。シェード時に「1灯ぶんの寄与」へ掛ける
    float W;
    // これまでに何個の候補から絞ったか。時間再利用で足し込み、上限でクランプする
    float M;
};

// 【p̂(y) を保存しない】保存すると露出変化やライト移動で古くなり、静かに間違ったWを
// 配り歩くことになる。再計算はBRDF1回ぶんで、G-Bufferを既に全部読んでいるパスの中なので
// ほぼ無償である

uint MegaLightsPackLightAndFlags(uint lightIndex, bool visible)
{
    return (lightIndex & 0xFFFFu) | (visible ? (1u << 16u) : 0u);
}

uint MegaLightsUnpackLight(uint packed)
{
    return packed & 0xFFFFu;
}

bool MegaLightsUnpackVisible(uint packed)
{
    return (packed & (1u << 16u)) != 0u;
}

MegaLightsReservoir MegaLightsMakeEmptyReservoir()
{
    MegaLightsReservoir reservoir;
    reservoir.LightAndFlags = MegaLightsPackLightAndFlags(kMegaLightsInvalidLight, false);
    reservoir.SampleUV = 0u;
    reservoir.W = 0.0f;
    reservoir.M = 0.0f;
    return reservoir;
}

bool MegaLightsReservoirIsEmpty(MegaLightsReservoir reservoir)
{
    return MegaLightsUnpackLight(reservoir.LightAndFlags) == kMegaLightsInvalidLight || reservoir.W <= 0.0f;
}

#endif // KURENAI_MEGALIGHTS_COMMON_HLSLI
