#pragma once

#include <cstdint>

#include "../EngineDefaults.h"

namespace Kurenai
{
    // デバッグ表示用: Presentパスで最終的に表示するレンダーターゲットの種類
    enum class DebugView
    {
        Final,
        Albedo,
        Normal,
        Material,
        Emissive,
        Depth,
        DepthRaw,           // 深度テクスチャの生値(0〜1)を加工せずそのままグレースケール表示
        DirectLight,        // DirectLightingパスの結果(HDR、シャドウ適用済みの直接光)をトーンマッピングして表示
        AOIndirectLight,    // AO/GIバッファのrgb(間接拡散光、ブラー後)をそのまま表示
        AOIndirectLightRaw, // AO/GIバッファのrgb(間接拡散光、ブラー前の生値)
        AOOcclusion,        // AO/GIバッファのa(遮蔽率、ブラー後)をグレースケール表示
        AOOcclusionRaw,     // AO/GIバッファのa(遮蔽率、ブラー前の生値)
        ShadowMap,          // m_ShadowSettings.DebugCascadeで選択したカスケードのシャドウマップを表示
        RTShadow,           // RTシャドウの可視率(0=影, 1=光)をグレースケール表示。RTシャドウ未実行時は最終結果
        SSR,                // 反射パスの出力(SceneColor+反射)。反射がOffのときはSceneColorと同一
        HiZ,                // Hi-Zミップチェーンの指定ミップ(m_DebugViewSettings.HiZDebugMipLevel)をグレースケール表示
        IBLIrradiance,      // IBL拡散イラディアンスマップ(TextureCube。現在の視線方向で球面を見回す表示)
        IBLPrefilter,       // IBLプリフィルタ済み鏡面マップの指定ミップ(m_IBLSettings.PrefilterDebugMipLevel、TextureCube)
        IBLBRDFLUT,         // IBL BRDF積分LUT(x=NdotV, y=ラフネス。R=A, G=B, B=Eavg)
        Bloom,              // ブルームのピラミッド最上段(半解像度、HDR)をトーンマッピングして表示
        LightTiles,         // タイルライトカリングのライトグリッド(タイルあたりのライト数)をヒートマップ表示
        // 反射プローブは鏡面専任なので拡散イラディアンスの表示は持たない(拡散はDDGIIrradiance)
        ProbePrefilter,     // 反射プローブのプリフィルタ済み鏡面(ミップ0がキャプチャ結果そのもの)
        ProbeInfluence,     // どのプローブが効いているかをプローブ番号ごとの色で塗り分けて表示
        ProbeDistance,      // 反射プローブの距離キューブ(プローブから見た各方向の被写体までの距離)
        MotionVector,       // モーションベクター(速度バッファ)。静止で灰色、動くと移動方向に応じて色が付く
        SceneColorRaw,      // トーンマップ前のHDRシーンカラーをリニアのまま無加工で表示(測定用)
        DDGIIrradiance,     // DDGIのイラディアンスアトラス(オクタヘドラル2D、22章)
        DDGIDistance,       // DDGIの距離モーメントアトラス(R=平均距離、G=平均二乗距離)
        BentNormal,         // bent normal(34章)。Debug View Gainが1なら軸を色表示、
                            // 1.5より大きいと長さ(=aoB)をグレースケール表示。
                            // データを持たないマテリアルはマゼンタで塗る
        WaterMask,          // G-BufferのMaterial.a(水面のマテリアルID)をグレースケール表示
        PlanarReflection,   // 平面反射パスの出力(m_RenderTargets.PlanarReflectionColor)をトーンマッピングして表示
        CloudNoiseSlice,    // 雲の3Dノイズの任意スライス。m_CloudSettings.NoiseDebugSlice/Detailで選ぶ
        AtmosphereLUT,      // 大気散乱のLUT。m_AtmosphereLUTDebugMultiで2枚を切り替える
        DDGIProbeBackface,  // DDGIのプローブ裏面率(イラディアンスアトラスのα、22章)。
                            // 白いほど「面の裏側ばかり見ている」=壁の内部に埋まっている。
                            // 分類のしきい値を実測で決めるための表示。ラスタ経路では常に黒
        // 以下3つは自前ソフトウェアラスタライザ(46章)の出力。パスが実行されていない
        // フレームでは中身が前フレーム/未定義の残骸なので、最終結果のまま切り替えない
        SoftwareRaster,       // ソフトウェアラスタライザのフラットな陰影(HDR)
        SoftwareRasterDepth,  // 同 深度(生値)。DebugView::DepthRawと並べて差分を取る
        SoftwareRasterNormal, // 同 法線。DebugView::Normalとまったく同じ符号化・同じ表示
        // MegaLightsパスが書いたポイント/スポットライトの直接光(トーンマップして表示)。
        // 上の3つと同じく、パスが実行されていないフレームでは中身が前フレーム/未定義の
        // 残骸なので、最終結果のまま切り替えない
        MegaLights,
        // MegaLightsの候補プールが数えた「そのタイルへ届いたライト数」。
        // 色付けは DebugView::LightTiles とまったく同じで、両者は同じ判定を使うので
        // 同じシーン・同じカメラなら画素単位で一致するはず(定義域のずれの検出用)
        MegaLightsTilePool,
        // MegaLightsの出力を線形空間で蓄積した平均(計測専用)。参照実装と確率的サンプリングの
        // これどうしを比べて、平均が真値へ寄るかを測る。蓄積が無効なら最終結果のまま
        MegaLightsAverage,
    };
    // デバッグ表示の総数。**enumの末尾を足したらここも直すこと**。
    // enumのすぐ隣に置いてあるのは、離れた場所にあると更新を忘れるため
    // (実際に DDGIProbeBackface を足したとき、範囲チェックが古い末尾のままで
    //  起動オプションからの選択が弾かれた)
    inline constexpr int kDebugViewCount = static_cast<int>(DebugView::MegaLightsAverage) + 1;

    struct DebugViewSettings
    {
        DebugView View = DebugView::Final;
        // デバッグ表示の輝度倍率(Present.hlslのGain)。AO/GIバッファの間接拡散光のように
        // 値そのものが小さいバッファ(この暗い室内では0.02〜0.1程度)は、等倍で表示しても
        // ほぼ真っ黒で階調の粗さが判別できない。持ち上げて表示することで、8bit格納時の
        // ポスタリゼーションが何段あるかを目視で確認できるようにする。
        // 色として表示するモード(Present.hlsl Mode 0/3/4)にのみ効く
        float Gain = Defaults::DebugViewGain;
        // デバッグ表示(Render Targets - Hi-Z)で確認するミップレベル
        int32_t HiZDebugMipLevel = 0;
        // DebugView::LightTilesのヒートマップで赤に振り切る基準のライト数。容量(64)を基準にすると
        // 実データ(数灯)ではほぼ真っ青で差が読めないため、別のつまみにしてある
        int LightTileHeatmapMax = Defaults::LightTileHeatmapMax;
    };
}
