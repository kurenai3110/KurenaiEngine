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

// --- 球光源のサンプリング(段階6) ---
//
// 【何をサンプリングしているか】光源が半径を持つと、遮蔽の判定は「中心へ1本」ではなく
// 「球面上のどこか1点へ1本」になる。受光点から見て球のどの部分が遮蔽物に隠れているかが
// 半影そのもので、遮蔽物と受光面が離れるほど球の見かけの大きさに対して影が広がる。
//
// 【減衰と目標関数は中心のままにする】半径ぶんの違いは遮蔽の判定にだけ現れるようにしてある。
// 減衰まで球面上の点で測ると、光源に近い面で 1/d^2 が発散しやすくなるうえ、
// 目標関数と実際の寄与がずれてRISの効率が落ちる。**期待値は変わらない**
// (どちらもW側で割り戻すため)ので、安定する側を採る。
//
// 【なぜ立体角ではなく球面の一様サンプルか】厳密には「受光点から見た球の可視錐」を
// 立体角一様にサンプリングするほうが分散が小さい。ただしこちらは受光点に依存するので、
// **時空間再利用で別の画素へサンプルを渡したときにヤコビアンが要る**。段階6では
// 光源に固定した球面一様に留め、ヤコビアンが1のまま正しくなるようにしてある
// (受光点に依存しない写像なのでシフトが恒等になる)。分散を詰めるのは次段階。

// 2つの一様乱数から球面上の点(単位ベクトル)を作る。極に偏らないよう z を一様に取る
float3 MegaLightsSampleUnitSphere(float2 u)
{
    const float z = u.x * 2.0f - 1.0f;
    const float r = sqrt(saturate(1.0f - z * z));
    const float phi = 6.28318530718f * u.y;
    return float3(r * cos(phi), r * sin(phi), z);
}

// サンプル位置を fp16 x2 でリザーバへ詰める(時空間再利用で持ち回るため)
uint MegaLightsPackSampleUV(float2 uv)
{
    return f32tof16(uv.x) | (f32tof16(uv.y) << 16u);
}

float2 MegaLightsUnpackSampleUV(uint packed)
{
    return float2(f16tof32(packed & 0xFFFFu), f16tof32(packed >> 16u));
}

// 半径Rの円板を面積一様にサンプルする。円板の法線は axis で、接空間はここで作る。
//
// 【球面一様と同じくヤコビアンが1のまま】写像が受光点に依存しないので、時空間再利用で
// 別の画素へサンプルを渡してもシフトが恒等になる(理由は上の球面サンプリングのコメント)。
// r = R*sqrt(u1) にするのは面積一様にするため。r = R*u1 だと中心へ寄る
float3 MegaLightsSampleDisk(float3 axis, float radius, float2 u)
{
    // axis に直交する適当な基底。axis と平行になりにくい軸を選んで外積する
    const float3 helper = (abs(axis.y) < 0.99f) ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
    const float3 tangent = normalize(cross(helper, axis));
    const float3 bitangent = cross(axis, tangent);

    const float r = radius * sqrt(saturate(u.x));
    const float phi = 6.28318530718f * u.y;
    return tangent * (r * cos(phi)) + bitangent * (r * sin(phi));
}

// 光源の中心と半径から、実際に狙う点を求める。半径0なら中心そのもの(点光源と完全に一致)。
//
// 【エミッシブ光源プロキシ(型3)は球ではなく円板を引く】平らな発光パネルへ球面一様を使うと、
// **サンプルの約半分がパネルの裏側に落ちる**。受光点からその点へのレイはパネル自身を貫くので
// 遮蔽と判定され、光が半分ほど暗くなり、しかもノイズとして出る。
// 「半影が出ている」ように見えるので気付きにくい。
// 減衰に使った円板近似(分母が d^2 + R^2)とも形が揃う。
//
// 【GPULight を受け取る】円板の向きに DirectionAngle.xyz(発光面の平均法線)が要る
float3 MegaLightsLightSamplePosition(GPULight light, float2 sampleUV)
{
    const float sourceRadius = light.Params.z;
    if (sourceRadius <= 0.0f)
    {
        return light.PositionType.xyz;
    }
    if ((uint)light.PositionType.w == 3u) // EmissiveProxy
    {
        return light.PositionType.xyz +
               MegaLightsSampleDisk(light.DirectionAngle.xyz, sourceRadius, sampleUV);
    }
    return light.PositionType.xyz + MegaLightsSampleUnitSphere(sampleUV) * sourceRadius;
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
