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

// --- 履歴の幾何(時間再利用が「再投影先は同じ面か」を判定するのに使う) ---
//
// 【なぜ専用に持つのか】G-Bufferは毎フレーム上書きされ、前フレームの写しはどこにも残らない。
// 再投影しただけでは、そこに写っているのが同じ面なのか、手前を別の物が横切ったのかが分からない。
// **C++側の確保(12バイト/画素)と一致させること。**
struct MegaLightsHistoryGuide
{
    // オクタヘドラル符号化した法線(fp16 x2)。G-Bufferと同じ符号化を使う
    uint NormalOct;
    // View空間の線形深度。【Reverse-Zの生値を入れてはいけない】
    // 生値で比べると遠景は常に「一致」・近景は常に「不一致」になり、
    // 遠景でゴースト・近景でノイズが残るという分かりにくい壊れ方をする
    float ViewZ;
    // 材質(金属度と粗さを8bitずつ)。「別の物に化けた」ことの検出用
    uint Material;
};

// オクタヘドラル法線(範囲[-1,1]の2成分)を1つのuintへ詰める。
// f32tof16 は SM5.0 から使えるので3バリアントすべてで通る
uint MegaLightsPackNormalOct(float2 oct)
{
    return f32tof16(oct.x) | (f32tof16(oct.y) << 16u);
}

float2 MegaLightsUnpackNormalOct(uint packed)
{
    return float2(f16tof32(packed & 0xFFFFu), f16tof32(packed >> 16u));
}

uint MegaLightsPackMaterial(float metallic, float roughness)
{
    const uint m = (uint)(saturate(metallic) * 255.0f + 0.5f);
    const uint r = (uint)(saturate(roughness) * 255.0f + 0.5f);
    return m | (r << 8u);
}

void MegaLightsUnpackMaterial(uint packed, out float metallic, out float roughness)
{
    metallic = float(packed & 0xFFu) / 255.0f;
    roughness = float((packed >> 8u) & 0xFFu) / 255.0f;
}

MegaLightsHistoryGuide MegaLightsMakeEmptyGuide()
{
    MegaLightsHistoryGuide guide;
    guide.NormalOct = 0u;
    // 背景を表す。時間再利用側はこれを「一致しない」として弾く
    guide.ViewZ = 0.0f;
    guide.Material = 0u;
    return guide;
}

#endif // KURENAI_MEGALIGHTS_COMMON_HLSLI
