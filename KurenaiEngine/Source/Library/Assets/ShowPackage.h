#pragma once

#include <cstdint>

// KurenaiEngine専用ドローンショー形式(.kshow)の定義。
// KurenaiShowEditor(オフラインのオーサリングツール)とランタイム(ShowLoader.cpp)の
// 両方から参照される、フォーマットの単一の正とするヘッダー。ヘッダオンリーで
// KURENAI_API(DLLエクスポート)は不要(値はコンパイル時定数、構造体はPOD)。
//
// 設計方針: 編隊の形をC++の生成関数として持っていたものを、点データそのものへ置き換える。
// 形がエンジンのビルド成果物である限り、新しい編隊を足すのにエンジンの再ビルドが要り、
// かつ「手続きで書ける形」しか作れない。点を持てばどんな形でも置ける。
//
// バイト列はすべてリトルエンディアン。#pragma packは使わない
// (各構造体は自然アラインメントのままパディングが生じない配置に設計済みで、
// static_assertでサイズを固定している。フィールドの追加・削除・並び替えを行う場合は
// このstatic_assertも必ず更新し、ランタイム側のVersion検証に頼って互換性を保つこと)。
// 文字列はUTF-8・NUL終端なし(長さで管理)で、.kmodelのStringPoolと同じ規約に従う。

namespace Kurenai::Assets
{
    // === .kshow ===
    //
    // ファイルレイアウト:
    //   [ShowHeader]
    //   [FormationEntry × FormationCount]
    //   [ShowPoint      × DroneCount × FormationCount]   ← 編隊の並び順に連結
    //   [StringPool (StringPoolSize bytes)]
    //
    // 【座標は正規化して持つ】点は「原点中心・代表半径1」の空間にある。実寸への変換は
    // .kscene側の[DroneShow]CenterとScaleが受け持つ。こうしておくと同じショーを
    // 別のシーンで別の場所・別の規模に置ける(ショーは「形」であって「配置」ではない)。
    //
    // 【全編隊が同じ点数を持つ】モーフは形Aのi番目と形Bのi番目を結ぶだけなので、
    // 点数が違うと変形の途中で機体が消える。エディタが保存時に必ずDroneCountへ揃える。
    //
    // 【モーフの対応づけはエディタが焼き込む】ファイル内の順序がそのまま対応関係になる
    // (エディタが「方位角を第1キー、高さを第2キーとする安定ソート」で並べる)。
    // 対応づけはデータの性質なので、ランタイムはインデックスiとiを結ぶだけで並べ替えない。

    constexpr char kShowMagic[4] = { 'K', 'S', 'H', 'W' };
    constexpr uint32_t kShowVersion = 1;

    struct ShowHeader
    {
        char     Magic[4];          // 'K','S','H','W' (kShowMagic)
        uint32_t Version;           // kShowVersion。不一致なら読み込み拒否
        uint32_t PointStride;       // sizeof(ShowPoint)。不一致なら読み込み拒否
        uint32_t DroneCount;        // 1編隊あたりの機体数。全編隊で共通
        uint32_t FormationCount;    // 編隊の数。この順にショーが進む
        // 再生の速さの倍率。1.0で等倍。ショーそのものの性質なのでデータが持つ
        float    Speed;
        // 1つの形を保つ時間[秒]
        float    HoldSeconds;
        // 次の形へ変形する時間[秒]
        float    MorphSeconds;
        // 発光の強さ。描画側がプリ露出と併せて一括で掛ける
        float    Brightness;
        // 1機のビルボード半径[m]。正規化空間ではなくワールドの実寸で持つ
        // (機体の大きさは編隊の規模を変えても変わらないため、Scaleに連動させない)
        float    Radius;
        // 機体ごとの微小な揺れ(ホバリング)の振幅[m]。0で完全に静止する
        float    HoverAmplitude;
        // 揺れと出発タイミングのばらつきを決める種。固定しておけば毎回同じ絵になる
        uint32_t Seed;
        uint32_t StringPoolSize;
        uint32_t Reserved[3];       // 0固定
    };
    static_assert(sizeof(ShowHeader) == 64, "ShowHeaderのレイアウトは64バイト固定");

    struct FormationEntry
    {
        uint32_t NameOffset;        // StringPool内オフセット。編隊の表示名(エディタ用)
        uint32_t NameLength;
        uint32_t Reserved[2];       // 0固定
    };
    static_assert(sizeof(FormationEntry) == 16, "FormationEntryのレイアウトは16バイト固定");

    struct ShowPoint
    {
        float Position[3];          // 正規化空間(原点中心・代表半径1)
        float Color[3];             // 線形RGB。明るさはShowHeader::Brightnessが持つ
    };
    static_assert(sizeof(ShowPoint) == 24, "ShowPointのレイアウトは24バイト固定");
}
