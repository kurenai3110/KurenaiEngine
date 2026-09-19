#pragma once

#include <cstdint>

// シェーダーへバインドできるスロット数の**唯一の定義**。
//
// 【なぜ1か所へ寄せるのか】以前はこれらの値が DX11CommandList.h / DX12CommandList.h /
// DX12Device.cpp のルートシグネチャに生の数値として散らばり、
// 「必ず一致させること(3か所)」というコメントで手で同期させていた。
// **H3でt21を足したときに実際に踏んでいる** —— DX11だけで確認していたため気付けなかった。
//
// 【依存を <cstdint> だけに閉じてある】D3Dのヘッダを1本も引かないので、
// KurenaiShaderPacker(include パスに Source\Library がある)からも引ける。
// シェーダー側の `#define` との突き合わせは、パッカーが `-D KURENAI_EXPECT_*` を渡し、
// HLSL の `#if` が `#error` を出す形で行う(Shaders/3D/ShaderInterop/GroupSizes.hlsli と同じ機構)。
//
// 各バックエンドのクラスには同名の定数が別名として残っている。既存の参照が25箇所以上あり、
// 別名にしておけばそれらを1行も触らずに済むため
namespace Kurenai::RHI::RHIBindingLimits
{
    // SetTexture / SetShaderResourceBuffer で使えるピクセルシェーダーのSRVスロット数(t0〜t22)。
    //
    // 【超えるとDX11では黙って落ち、DX12ではPSOの作成が 0x80070057 で失敗する】
    // DX11側はさらに、この値が足りないと反射プローブ(19章)のキューブマップ配列・影響範囲バッファ・
    // 距離キューブや、DDGI(22章)のアトラス2枚が m_BoundPixelSrvs の追跡から漏れる。
    // そうなるとUnbindPixelSrvForResourceがこれらを外せず、ベイクがUAVで書き込む際の
    // SRVアンバインドがドライバ任せ(警告付きの自動アンバインド)になる。
    // **SetTextureは範囲外スロットも素通しするため、漏れていても描画結果には現れない。**
    //
    // 【現在の23の内訳】最も多く使うDeferredLighting.hlslがt0〜t22をちょうど使い切る:
    //   t0〜t7   G-Buffer一式(アルベド/直接光/マテリアル/深度/スカイボックス/AO/自発光/法線)
    //   t8,t9    グローバルIBL(放射照度・プリフィルタ済み鏡面)
    //   t10      BRDF LUT
    //   t11      空パラメータ(GPUSkyParameters、SkyIntegrate.hlslが書く構造化バッファ)
    //   t12〜t14 反射プローブ(鏡面専任。拡散はDDGIへ一本化した)
    //   t15,t16  DDGIのオクタヘドラルアトラス2枚
    //   t17      bent normalのG-Buffer(34章)
    //   t18      低解像度の雲パス(SkyCloud.hlsl)の出力。rgb=事前乗算済みの散乱光 / a=透過率
    //   t19      DDGIResolveが書いた低解像度のイラディアンス
    //   t20      大気散乱のSkyView LUT
    //   t21      DDGIResolveが書いた低解像度の深度(41.24節)
    //   t22      低解像度の雲パスが書いたfogInFront(雲の手前の霞。P18bの補正に使う)
    inline constexpr uint32_t kTextureSlotCount = 23;

    // SetVertexShaderResourceBuffer で使える頂点シェーダーのSRVスロット数。
    //
    // DX11自体は128本持っているが、DX12側はルートSRVを1本(t0固定)しか割り当てていない。
    // **ここを合わせておかないと「DX11では通るがDX12では黙って描画が消える」非対称なバグが書ける。**
    // 増やすときはDX12のルートシグネチャ(CreateRootSignature の rootParams[4])も同時に直すこと
    inline constexpr uint32_t kVertexShaderSrvSlotCount = 1;

    // コンピュートシェーダーのSRVスロット数(t0〜t17)。
    // 18あるのはレイトレーシングのパス(RT反射)がTLAS・G-Buffer・シーンジオメトリに加えて
    // bent normal(t16、34章)とメッシュレット表(t17、38章)を1回のディスパッチで同時に読むため
    inline constexpr uint32_t kComputeSrvSlotCount = 18;

    // コンピュートシェーダーのUAVスロット数(u0〜u4)。
    // 5本目は MegaLights デノイザのタイル勾配(MegaLightsDenoise.hlsl の CSTileGradient)が
    // 使う。あちらは時間累積が使う u0〜u3 とは別のパスだが、**同じ .hlsl の中で宣言が
    // 重なる**ため、同じ register を別の型で二重宣言することができない。
    // 【足りないと PSO の生成が E_INVALIDARG で落ちる】シェーダが宣言した register が
    // ルートシグネチャの範囲を超えるため。**DX11 側の null クリアと DX12 の
    // ルートシグネチャ(DX12DevicePipelines.cpp の ranges[1])はこの定数から作られる**ので、
    // 直すのはここ1か所でよい
    inline constexpr uint32_t kComputeUavSlotCount = 5;

    // 1つのサンプラーセット(=1つのディスクリプタテーブル)が持つスロット数。
    // s0 = MaterialSampler、s1 = ColorSampler、s2 = DataSampler、s3 = VolumeSampler
    // (役割の定義は Shaders/3D/Samplers.hlsli)。
    //
    // 【Samplers.hlsli の役割数と必ず一致させること】小さいままだと CreateSamplerSet が
    // 超過分を**切り捨てて**しまい、DX11は正しく動くのにDX12でだけサンプラーが既定のものに
    // 差し替わる、という片側だけ静かに壊れる形になる。
    // 一部のスロットしか宣言しないシェーダーでもテーブルはこの個数ぶんまとめてバインドされるため、
    // セット生成時に余ったスロットは既定のサンプラーで埋める
    inline constexpr uint32_t kSamplerSlotCount = 4;
}
