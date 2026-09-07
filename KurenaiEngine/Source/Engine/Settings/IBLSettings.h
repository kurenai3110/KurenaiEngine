#pragma once

#include "../EngineDefaults.h"

namespace Kurenai
{
    struct IBLSettings
    {
        // IBL(拡散イラディアンス+プリフィルタ済み鏡面)のON/OFFと強度。無効時はシェーダ側
        // (DeferredLighting.hlsl)でEvaluateIBLの代わりに定数色アンビエント
        // (AmbientColor.rgb)へフォールバックする(真っ暗にはしない)。既定値を1.0でなく0.5に
        // しているのは、明るく補正した空の輝度分布(14.6節)ではIBL全体の寄与が強すぎるため
        bool Enabled = Defaults::IBLEnabled;
        float Intensity = Defaults::IBLIntensity;

        // 拡散イラディアンスの球面調和関数(SH L2)経路。CSIrradianceの高速な
        // 代替で、UseSHIrradianceでA/B比較できるようトグルにしてある。詳細は
        // IBLConvolve.hlsl冒頭のコメントとdocs/Architecture.htmlを参照
        //
        // trueならCSIrradianceの代わりにSH L2経路を使う。既定false(検証で選べるようにしてあるが、
        // どちらを既定にするかはリンギングの実測(ProbeTestのエミッシブ帯周り)で決めること。
        // UseDedicatedIrradiance/デバッグビューでイラディアンス焼き込みが要る場面でのみ意味を持つ
        bool UseSHIrradiance = Defaults::IBLUseSHIrradiance;
        // SHのウィンドウ関数(Sloan)の強さ。0=無効(既定)。リンギングが実測で出た場合のつまみ
        float SHWindowLambda = Defaults::SHWindowLambda;

        // デバッグ表示(Render Targets)で確認するプリフィルタ済み鏡面マップのミップレベル
        int32_t PrefilterDebugMipLevel = 0;

        // 拡散イラディアンスを専用マップ(m_IBLResources.IrradianceTexture)から取るかどうか。既定はfalseで、
        // プリフィルタ済み鏡面の最終ミップ(roughness=1)を使う。CSPrefilterがV=R=Nを仮定して
        // いるためroughness=1ではGGXの実効カーネルがコサイン畳み込みへ厳密に退化し、両者は同じ
        // E(N)/πを格納する(14.10節)。White Furnace Testで画素一致、実スカイボックスでも
        // 最大2〜4/255の差しか出ないことを実機で確認してあるため、既定では専用マップを使わない。
        // これによりリフレクションプローブのような実行時のキューブマップ焼き直しから、最も重い
        // CSIrradiance(約9750万サンプル)を丸ごと省ける。
        // 畳み込み処理自体はいつでも検証できるよう残してあり、このトグルをONにすると
        // その場で焼いて(m_IBLIrradianceBaked)従来経路に切り替わる
        bool UseDedicatedIrradiance = Defaults::IBLUseDedicatedIrradiance;

        // 環境光(間接光)の拡散・鏡面それぞれの倍率。FrameConstants.IBLParams.y / .z として渡す。
        //
        // Intensityが拡散と鏡面へ一様に掛かる「環境光全体の明るさ」なのに対し、こちらは
        // 両者の比率を意図的に崩すための画作り用のつまみ。金属やガラスの映り込みだけを強めたい、
        // 逆に環境の照り返しを残したまま反射を抑えたい、といった調整がIBL強度単独ではできないため
        // 分けている。
        //
        // 【IBLの有効/無効に関わらず効く】無効時の定数色アンビエントにも同じ倍率を掛ける。
        // 片方にしか効かないとトグルを切り替えたときにつまみの意味が変わり、比較にならないため。
        // 【間接光にのみ効く】直接光・自発光には掛けない(遮蔽マップと同じ方針。22.1節)。
        // SSILの間接拡散光にも掛けない ―― あれはスクリーンスペースで得た周囲のサーフェスからの
        // 光であって、ここで言う環境(空・プローブ)由来のアンビエントとは別の項のため
        float AmbientDiffuseScale = Defaults::AmbientDiffuseScale;
        float AmbientSpecularScale = Defaults::AmbientSpecularScale;
        // Enable IBL無効時に使う定数色アンビエントフォールバックの強度倍率。シェーダ側ではなく
        // Render()がFrameConstants.AmbientColorへ書き込む時点でrgb(alphaのdayFactorは除く)に
        // 乗算する(HLSL側は素のAmbientColor.rgbを読むだけでよい)
        float AmbientScale = Defaults::AmbientScale;
    };
}
