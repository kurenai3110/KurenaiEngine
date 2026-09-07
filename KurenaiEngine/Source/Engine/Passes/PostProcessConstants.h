#pragma once

#include <cstddef>
#include <cstdint>
#include <cmath>

#include <DirectXMath.h>

// ポストプロセスのシェーダーが読む cbuffer の C++ 側の写し(段階6)。
//
// 【なぜ独立したヘッダーなのか】これらの構造体は、パスの登録側(Passes/PostProcessPasses.cpp)と、
// 定数バッファを作る側(KurenaiEngine3D.cpp の CreateSceneResources)の**両方**が sizeof で使う。
// どちらかの翻訳単位へ閉じ込めるともう片方が見られない。
//
// 【static_assert が守るのはC++側だけ】HLSL の宣言と突き合わせているわけではない。
// ここが落ちたら「HLSL側も同じだけ動かせ」という合図として使う(FrameConstants.h と同じ規約)。
// **通すために期待値を書き換えないこと。**
namespace Kurenai::Passes
{
        // 輝度ヒストグラムのビン数。AutoExposure.hlslのHISTOGRAM_BINSと一致させること
        inline constexpr uint32_t kExposureHistogramBins = 256;

        // Tonemap.hlsl側のcbuffer TonemapConstantsと一致させる必要がある
        struct alignas(16) TonemapConstants
        {
            // TonemapCurve(0=Reinhard, 1=ACES, 2=AgX)
            int32_t Curve;
            // 手動露出時に掛ける倍率。プリ露出は時刻連動で変動するため、ユーザー設定EV100との
            // 差分 2^(実効EV100 - 設定EV100) を割り戻して固定露出の絵に戻す(1.0固定ではない)
            float ExposureScale;
            // ディザの強さ(0=無効、1=±1LSB)
            float DitherStrength;
            // 1.0=自動露出、0.0=手動
            float UseAutoExposure;
            // CPU側でライト強度へ事前乗算済みのEV100(プリ露出)
            float PreExposureEV100;
            // ブルームの合成比(0で無効)
            float BloomStrength;
            // 薄明視の適用量(0で無効、1で完全適用)
            float MesopicStrength;
            // 目が順応している明るさ(EV100)。構図にも露出設定にも依存しない
            float MesopicAdaptationEV100;
            // TAAの蓄積で失われた高域を戻すシャープネス(0で無効)。TAAが無効のときは常に0。
            //
            // 【なぜTAAではなくここなのか】TAAの入力へ掛けると、アンシャープマスクが
            // 増幅する高域は「ジッターで毎フレーム変動する成分」そのものなので静止時のちらつきが
            // 大きく増える。ここはトーンマップ後のLDR値に対して掛かるだけで
            // どこへもフィードバックされないため、ちらつきにもリンギングの累積にも寄与しない
            float Sharpness;
            // シャープネスの近傍タップに使う1テクセルぶんのUV(1/レンダー解像度)
            float InvRenderWidth;
            float InvRenderHeight;
            // 黒の締め(ブラックポイント)。0で恒等。詳細はTonemap.hlsl側のコメント参照
            float BlackPoint;
        };
        // 【HLSL側の宣言とレイアウトを揃えたまま保つための固定】cbuffer(と構造化バッファ)は
        // 宣言順でオフセットが決まるので、ここで並べ替え・挿入・型変更が起きると、
        // HLSL側を直さないかぎり黙って別の値を読むことになる。
        // **通すために期待値を書き換えないこと**(FrameConstants.h と同じ規約)。
        //
        // 【これが守るのはC++側だけ】HLSLの宣言と突き合わせているわけではない。
        // ここが落ちたら「HLSL側も同じだけ動かせ」という合図として使う
        static_assert(offsetof(TonemapConstants, Curve) == 0, "Curve のレイアウトが変わっている");
        static_assert(offsetof(TonemapConstants, ExposureScale) == 4, "ExposureScale のレイアウトが変わっている");
        static_assert(offsetof(TonemapConstants, DitherStrength) == 8, "DitherStrength のレイアウトが変わっている");
        static_assert(offsetof(TonemapConstants, UseAutoExposure) == 12, "UseAutoExposure のレイアウトが変わっている");
        static_assert(offsetof(TonemapConstants, PreExposureEV100) == 16, "PreExposureEV100 のレイアウトが変わっている");
        static_assert(offsetof(TonemapConstants, BloomStrength) == 20, "BloomStrength のレイアウトが変わっている");
        static_assert(offsetof(TonemapConstants, MesopicStrength) == 24, "MesopicStrength のレイアウトが変わっている");
        static_assert(offsetof(TonemapConstants, MesopicAdaptationEV100) == 28, "MesopicAdaptationEV100 のレイアウトが変わっている");
        static_assert(offsetof(TonemapConstants, Sharpness) == 32, "Sharpness のレイアウトが変わっている");
        static_assert(offsetof(TonemapConstants, InvRenderWidth) == 36, "InvRenderWidth のレイアウトが変わっている");
        static_assert(offsetof(TonemapConstants, InvRenderHeight) == 40, "InvRenderHeight のレイアウトが変わっている");
        static_assert(offsetof(TonemapConstants, BlackPoint) == 44, "BlackPoint のレイアウトが変わっている");
        static_assert(sizeof(TonemapConstants) == 48, "TonemapConstants の総サイズが変わっている");

        // Upscale.hlsl側のcbuffer UpscaleConstantsと一致させる必要がある
        struct alignas(16) UpscaleConstants
        {
            // EASUの事前計算定数。ComputeEasuConstants()が入力/出力解像度から作る
            DirectX::XMFLOAT4 EasuCon0;
            DirectX::XMFLOAT4 EasuCon1;
            DirectX::XMFLOAT4 EasuCon2;
            DirectX::XMFLOAT4 EasuCon3;
            // 書き込み先のサイズ(出力解像度)
            DirectX::XMUINT2 OutputSize;
            // RCASのシャープネス(ComputeRcasSharpnessScaleで変換済みの線形値)
            float RcasSharpnessScale;
            float UpscalePadding;
        };
        // 【HLSL側の宣言とレイアウトを揃えたまま保つための固定】cbuffer(と構造化バッファ)は
        // 宣言順でオフセットが決まるので、ここで並べ替え・挿入・型変更が起きると、
        // HLSL側を直さないかぎり黙って別の値を読むことになる。
        // **通すために期待値を書き換えないこと**(FrameConstants.h と同じ規約)。
        //
        // 【これが守るのはC++側だけ】HLSLの宣言と突き合わせているわけではない。
        // ここが落ちたら「HLSL側も同じだけ動かせ」という合図として使う
        static_assert(offsetof(UpscaleConstants, EasuCon0) == 0, "EasuCon0 のレイアウトが変わっている");
        static_assert(offsetof(UpscaleConstants, EasuCon1) == 16, "EasuCon1 のレイアウトが変わっている");
        static_assert(offsetof(UpscaleConstants, EasuCon2) == 32, "EasuCon2 のレイアウトが変わっている");
        static_assert(offsetof(UpscaleConstants, EasuCon3) == 48, "EasuCon3 のレイアウトが変わっている");
        static_assert(offsetof(UpscaleConstants, OutputSize) == 64, "OutputSize のレイアウトが変わっている");
        static_assert(offsetof(UpscaleConstants, RcasSharpnessScale) == 72, "RcasSharpnessScale のレイアウトが変わっている");
        static_assert(offsetof(UpscaleConstants, UpscalePadding) == 76, "UpscalePadding のレイアウトが変わっている");
        static_assert(sizeof(UpscaleConstants) == 80, "UpscaleConstants の総サイズが変わっている");

        // FSR1のFsrEasuCon()と同じ内容。出力画素の整数座標から入力画像の再構成位置を求めるための
        // スケール/オフセットと、12タップぶんの4回のGather4の中心へのオフセットを作る。
        //
        // 参照実装はこれらをuintへビットキャストして渡すが、それはFP16パック経路(A_HALF)と
        // 定数を共用するためで、SM5.0でも動く必要がある(=16bitパック経路を使わない)このエンジンでは
        // floatのまま持つほうがCPU側の構造体と素直に対応する
        inline void ComputeEasuConstants(
            UpscaleConstants& constants, uint32_t inputWidth, uint32_t inputHeight,
            uint32_t outputWidth, uint32_t outputHeight)
        {
            const float inputW = static_cast<float>(inputWidth);
            const float inputH = static_cast<float>(inputHeight);
            const float outputW = static_cast<float>(outputWidth);
            const float outputH = static_cast<float>(outputHeight);

            // 出力の整数座標 → 入力の画素座標。0.5を引いているのはテクセル中心合わせ
            constants.EasuCon0 = {
                inputW / outputW,
                inputH / outputH,
                0.5f * inputW / outputW - 0.5f,
                0.5f * inputH / outputH - 0.5f,
            };
            // 入力の画素座標 → 正規化UV。zwは12タップの左上ブロック('F'タップ)へのオフセット
            constants.EasuCon1 = { 1.0f / inputW, 1.0f / inputH, 1.0f / inputW, -1.0f / inputH };
            // 残り3つのGather中心へのオフセット(いずれも'F'ではなく1つめのGather中心からの相対)
            constants.EasuCon2 = { -1.0f / inputW, 2.0f / inputH, 1.0f / inputW, 2.0f / inputH };
            constants.EasuCon3 = { 0.0f, 4.0f / inputH, 0.0f, 0.0f };
        }

        // Bloom.hlsl側のcbuffer BloomConstantsと一致させる必要がある
        struct alignas(16) BloomConstants
        {
            DirectX::XMUINT2 SrcSize;
            DirectX::XMUINT2 DstSize;

            float Threshold;
            float SoftKnee;
            // 1.0なら最初のダウンサンプル(Karis平均としきい値を適用する)
            float ApplyKarisAndThreshold;
            // 1.0=自動露出、0.0=手動(Tonemapと同じ意味)
            float UseAutoExposure;

            // CPU側でライト強度へ事前乗算済みのEV100(プリ露出)
            float PreExposureEV100;
            // 手動露出時に掛ける倍率(TonemapConstants::ExposureScaleと同じ値)
            float ExposureScale;
            float Padding[2];
        };
        // 【HLSL側の宣言とレイアウトを揃えたまま保つための固定】cbuffer(と構造化バッファ)は
        // 宣言順でオフセットが決まるので、ここで並べ替え・挿入・型変更が起きると、
        // HLSL側を直さないかぎり黙って別の値を読むことになる。
        // **通すために期待値を書き換えないこと**(FrameConstants.h と同じ規約)。
        //
        // 【これが守るのはC++側だけ】HLSLの宣言と突き合わせているわけではない。
        // ここが落ちたら「HLSL側も同じだけ動かせ」という合図として使う
        static_assert(offsetof(BloomConstants, SrcSize) == 0, "SrcSize のレイアウトが変わっている");
        static_assert(offsetof(BloomConstants, DstSize) == 8, "DstSize のレイアウトが変わっている");
        static_assert(offsetof(BloomConstants, Threshold) == 16, "Threshold のレイアウトが変わっている");
        static_assert(offsetof(BloomConstants, SoftKnee) == 20, "SoftKnee のレイアウトが変わっている");
        static_assert(offsetof(BloomConstants, ApplyKarisAndThreshold) == 24, "ApplyKarisAndThreshold のレイアウトが変わっている");
        static_assert(offsetof(BloomConstants, UseAutoExposure) == 28, "UseAutoExposure のレイアウトが変わっている");
        static_assert(offsetof(BloomConstants, PreExposureEV100) == 32, "PreExposureEV100 のレイアウトが変わっている");
        static_assert(offsetof(BloomConstants, ExposureScale) == 36, "ExposureScale のレイアウトが変わっている");
        static_assert(offsetof(BloomConstants, Padding) == 40, "Padding のレイアウトが変わっている");
        static_assert(sizeof(BloomConstants) == 48, "BloomConstants の総サイズが変わっている");

        // AutoExposure.hlsl側のcbuffer AutoExposureConstantsと一致させる必要がある
        struct alignas(16) AutoExposureConstants
        {
            DirectX::XMUINT2 InputSize;
            float MinEV100;
            float MaxEV100;

            float PreExposureEV100;
            float DeltaTime;
            float AdaptationSpeedUp;
            float AdaptationSpeedDown;

            float LowPercentile;
            float HighPercentile;
            float ExposureCompensation;

            // 暗いシーンをわざと暗いまま写すための補正カーブ(AutoExposure.hlsl参照)
            float NightRolloffEV;
            float NightRolloffDarkEV100;
            float NightRolloffBrightEV100;

            // 測光値の上側クランプ(構図依存を抑える。AutoExposure.hlsl参照)
            float KeyReferenceEV100;
            float KeyCeilingEV;

            // 0以外なら順応を飛ばして測光値へ即座に合わせる(シーン切り替え時。
            // m_AutoExposureResetRequested参照)
            float ResetAdaptation;
            float Padding[3];
        };
        // 【HLSL側の宣言とレイアウトを揃えたまま保つための固定】cbuffer(と構造化バッファ)は
        // 宣言順でオフセットが決まるので、ここで並べ替え・挿入・型変更が起きると、
        // HLSL側を直さないかぎり黙って別の値を読むことになる。
        // **通すために期待値を書き換えないこと**(FrameConstants.h と同じ規約)。
        //
        // 【これが守るのはC++側だけ】HLSLの宣言と突き合わせているわけではない。
        // ここが落ちたら「HLSL側も同じだけ動かせ」という合図として使う
        static_assert(offsetof(AutoExposureConstants, InputSize) == 0, "InputSize のレイアウトが変わっている");
        static_assert(offsetof(AutoExposureConstants, MinEV100) == 8, "MinEV100 のレイアウトが変わっている");
        static_assert(offsetof(AutoExposureConstants, MaxEV100) == 12, "MaxEV100 のレイアウトが変わっている");
        static_assert(offsetof(AutoExposureConstants, PreExposureEV100) == 16, "PreExposureEV100 のレイアウトが変わっている");
        static_assert(offsetof(AutoExposureConstants, DeltaTime) == 20, "DeltaTime のレイアウトが変わっている");
        static_assert(offsetof(AutoExposureConstants, AdaptationSpeedUp) == 24, "AdaptationSpeedUp のレイアウトが変わっている");
        static_assert(offsetof(AutoExposureConstants, AdaptationSpeedDown) == 28, "AdaptationSpeedDown のレイアウトが変わっている");
        static_assert(offsetof(AutoExposureConstants, LowPercentile) == 32, "LowPercentile のレイアウトが変わっている");
        static_assert(offsetof(AutoExposureConstants, HighPercentile) == 36, "HighPercentile のレイアウトが変わっている");
        static_assert(offsetof(AutoExposureConstants, ExposureCompensation) == 40, "ExposureCompensation のレイアウトが変わっている");
        static_assert(offsetof(AutoExposureConstants, NightRolloffEV) == 44, "NightRolloffEV のレイアウトが変わっている");
        static_assert(offsetof(AutoExposureConstants, NightRolloffDarkEV100) == 48, "NightRolloffDarkEV100 のレイアウトが変わっている");
        static_assert(offsetof(AutoExposureConstants, NightRolloffBrightEV100) == 52, "NightRolloffBrightEV100 のレイアウトが変わっている");
        static_assert(offsetof(AutoExposureConstants, KeyReferenceEV100) == 56, "KeyReferenceEV100 のレイアウトが変わっている");
        static_assert(offsetof(AutoExposureConstants, KeyCeilingEV) == 60, "KeyCeilingEV のレイアウトが変わっている");
        static_assert(offsetof(AutoExposureConstants, ResetAdaptation) == 64, "ResetAdaptation のレイアウトが変わっている");
        static_assert(offsetof(AutoExposureConstants, Padding) == 68, "Padding のレイアウトが変わっている");
        static_assert(sizeof(AutoExposureConstants) == 80, "AutoExposureConstants の総サイズが変わっている");

        // TAA.hlsl側のcbuffer TAAConstants(register b1)と並びを一致させる必要がある。
        // TAAパスはb0(FrameConstants)を使わず、必要な行列もすべてこちらへ入れている。
        // FrameConstantsは末尾追加を重ねて700バイトを超えており、cbufferは途中のフィールドを
        // 飛ばせないため、末尾の2つを読むためだけに全フィールドを宣言する羽目になるのを避けている
        struct alignas(16) TAAConstants
        {
            DirectX::XMFLOAT4X4 InvViewProj;  // 今フレームのジッター済み逆VP(空の速度の補完に使う)
            DirectX::XMFLOAT4X4 PrevViewProj; // 前フレームのジッター済みVP
            DirectX::XMFLOAT4 JitterUv;       // xy=今フレームのジッター(UV単位), zw=前フレーム
            DirectX::XMFLOAT4 ScreenParams;   // xy=レンダー解像度, zw=その逆数
            // x: 今フレームの色を混ぜる割合(m_PostProcessSettings.TAABlendWeight)
            // y: 近傍クリップのボックス幅(標準偏差の何倍か。m_PostProcessSettings.TAAClipGamma)
            // z: 履歴が使えるか(0=使えない。TAA.hlslは履歴をサンプルすらしない)
            // w: プリ露出の変化を打ち消す倍率(今フレームの露出 / 前フレームの露出)
            DirectX::XMFLOAT4 Params0;
            // x: 近傍クリップの方式(TAAClipMode)
            // y: 静止時のちらつき抑制の強さ(m_PostProcessSettings.TAAAntiFlicker)。zwは未使用
            DirectX::XMFLOAT4 Params1;
        };
        // 【HLSL側の宣言とレイアウトを揃えたまま保つための固定】cbuffer(と構造化バッファ)は
        // 宣言順でオフセットが決まるので、ここで並べ替え・挿入・型変更が起きると、
        // HLSL側を直さないかぎり黙って別の値を読むことになる。
        // **通すために期待値を書き換えないこと**(FrameConstants.h と同じ規約)。
        //
        // 【これが守るのはC++側だけ】HLSLの宣言と突き合わせているわけではない。
        // ここが落ちたら「HLSL側も同じだけ動かせ」という合図として使う
        static_assert(offsetof(TAAConstants, InvViewProj) == 0, "InvViewProj のレイアウトが変わっている");
        static_assert(offsetof(TAAConstants, PrevViewProj) == 64, "PrevViewProj のレイアウトが変わっている");
        static_assert(offsetof(TAAConstants, JitterUv) == 128, "JitterUv のレイアウトが変わっている");
        static_assert(offsetof(TAAConstants, ScreenParams) == 144, "ScreenParams のレイアウトが変わっている");
        static_assert(offsetof(TAAConstants, Params0) == 160, "Params0 のレイアウトが変わっている");
        static_assert(offsetof(TAAConstants, Params1) == 176, "Params1 のレイアウトが変わっている");
        static_assert(sizeof(TAAConstants) == 192, "TAAConstants の総サイズが変わっている");

        // DroneShow.hlsl側のcbuffer DroneShowConstantsと一致させる必要がある。
        // b0のFrameConstantsには相乗りさせない(理由はDroneShow.hlsl冒頭。
        // 巨大なcbufferの途中のフィールドを宣言し忘れるとオフセットが静かにずれる)
        struct alignas(16) DroneShowConstants
        {
            // 転置済み。メイン描画ではカメラのビュー行列、平面反射では鏡映×カメラのビュー行列
            DirectX::XMFLOAT4X4 View;
            // 転置済み。どちらのパスでもメインカメラのジッター済みProj
            DirectX::XMFLOAT4X4 Proj;
            // x=明るさ倍率(実効プリ露出を乗算済み)、y=画面上の最小半径(NDC)、
            // z=射影行列の[0][0]成分、w=未使用
            DirectX::XMFLOAT4 Params0;
            // 平面反射で水面より下の機体を落とすクリップ平面(xyz=法線、w=距離項)
            DirectX::XMFLOAT4 ClipPlane;
            // x=クリップ平面を使うか(0=メイン描画、1=平面反射)、yzw=未使用
            DirectX::XMFLOAT4 Params1;
        };
        // 【HLSL側の宣言とレイアウトを揃えたまま保つための固定】cbuffer(と構造化バッファ)は
        // 宣言順でオフセットが決まるので、ここで並べ替え・挿入・型変更が起きると、
        // HLSL側を直さないかぎり黙って別の値を読むことになる。
        // **通すために期待値を書き換えないこと**(FrameConstants.h と同じ規約)。
        //
        // 【これが守るのはC++側だけ】HLSLの宣言と突き合わせているわけではない。
        // ここが落ちたら「HLSL側も同じだけ動かせ」という合図として使う
        static_assert(offsetof(DroneShowConstants, View) == 0, "View のレイアウトが変わっている");
        static_assert(offsetof(DroneShowConstants, Proj) == 64, "Proj のレイアウトが変わっている");
        static_assert(offsetof(DroneShowConstants, Params0) == 128, "Params0 のレイアウトが変わっている");
        static_assert(offsetof(DroneShowConstants, ClipPlane) == 144, "ClipPlane のレイアウトが変わっている");
        static_assert(offsetof(DroneShowConstants, Params1) == 160, "Params1 のレイアウトが変わっている");
        static_assert(sizeof(DroneShowConstants) == 176, "DroneShowConstants の総サイズが変わっている");
}
