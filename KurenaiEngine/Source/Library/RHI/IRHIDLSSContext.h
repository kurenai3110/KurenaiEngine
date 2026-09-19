#pragma once

#include <cstdint>

#include "KurenaiTypes.h"

namespace Kurenai::RHI
{
    class IRHICommandList;
    class IRHITexture;

    // DLSSの品質モード。NVSDK_NGX_PerfQuality_Valueへ1対1で写す(対応表はDX12DLSSContext.cpp)。
    // DLAAはアップスケールしない(レンダー解像度=出力解像度)モードで、DLSSのAAだけを使う
    enum class DLSSQuality : int32_t
    {
        DLAA,
        UltraQuality,
        Quality,
        Balanced,
        Performance,
        UltraPerformance,
    };

    // NGXが返す、その出力解像度・品質モードでの推奨レンダー解像度と許容範囲。
    // 【自前の倍率表を使ってはいけない】推奨値はDLSSのバージョンと品質モードで決まるため、
    // エンジン側で1.5倍などと決め打ちすると、NGXが許容しない解像度でフィーチャを作ることになる
    struct DLSSOptimalSettings
    {
        uint32_t RenderWidth = 0;
        uint32_t RenderHeight = 0;
        uint32_t MinRenderWidth = 0;
        uint32_t MinRenderHeight = 0;
        uint32_t MaxRenderWidth = 0;
        uint32_t MaxRenderHeight = 0;
    };

    // フィーチャ(DLSSの内部状態と履歴)の生成条件。どれか1つでも変わったら作り直しになる
    struct DLSSFeatureDesc
    {
        uint32_t RenderWidth = 0;
        uint32_t RenderHeight = 0;
        uint32_t OutputWidth = 0;
        uint32_t OutputHeight = 0;
        DLSSQuality Quality = DLSSQuality::Quality;

        // 深度が近平面=1.0・遠平面=0.0(Reverse-Z)であることをNGXへ伝える。
        // このエンジンは常にReverse-Zなのでtrue固定だが、黙った前提にしないため引数で持つ
        bool DepthInverted = true;
        // NGXに露出を自前で推定させる。エンジンのHDRバッファはプリ露出済みで、
        // 自動露出パスがDLSSより後ろにあるため、渡せる「今フレームの露出テクスチャ」が無い
        bool AutoExposure = true;
        // モーションベクターが出力解像度ではなくレンダー解像度で入っている(このエンジンは常にそう)
        bool MotionVectorsAtRenderResolution = true;
    };

    // 1フレーム分の評価入力。テクスチャはすべてRHIの不透明ハンドルで受ける
    struct DLSSEvaluateDesc
    {
        // 入力のHDRシーン色(レンダー解像度・プリ露出済み)
        IRHITexture* Color = nullptr;
        // 深度(レンダー解像度・Reverse-Z)
        IRHITexture* Depth = nullptr;
        // モーションベクター(レンダー解像度)
        IRHITexture* MotionVectors = nullptr;
        // 出力先(出力解像度・UAV)
        IRHITexture* Output = nullptr;

        // 今フレームのサブピクセルジッター。**ピクセル単位**(UVでもNDCでもない)
        float JitterOffsetX = 0.0f;
        float JitterOffsetY = 0.0f;

        // モーションベクターのテクセル値へ掛ける係数。
        // エンジンのGBufferVelocityは「画面UV単位・current - previous」で、
        // DLSSはレンダー解像度のピクセルで「現在位置へ足すと前フレームの位置になる」ベクトルを
        // 期待する。したがって導出上は負のレンダー解像度になるが、符号は実測で確かめること
        // (符号を逆にしても絵は出る ―― 残像が増えるだけで気づきにくい)
        float MotionVectorScaleX = 0.0f;
        float MotionVectorScaleY = 0.0f;

        // 入力色に既に掛かっているプリ露出の線形倍率(エンジンのComputeExposure(EV100))
        float PreExposure = 1.0f;

        // 履歴を捨てる(シーン切り替え・解像度変更の直後)
        bool ResetHistory = false;

        // 入力として実際に使う矩形。レンダーターゲットの実サイズと同じでよいが、
        // NGXは「フィーチャ生成時のレンダー解像度以下」であることを要求する
        uint32_t RenderWidth = 0;
        uint32_t RenderHeight = 0;
    };

    // DLSS(NVIDIA NGX)の不透明ハンドル。IRHIAccelerationStructureと同じく、中身はバックエンド
    // 実装(DX12DLSSContext)側が持ち、D3D12固有の型を1つも上位層へ漏らさない。
    //
    // 【DX12専用】NGXのDX11経路はDLSS Super Resolutionに限られ、実装・検証の量が倍になるため
    // 採っていない。DX11Device::CreateDLSSContextは理由をログに残してnullptrを返す
    class KURENAI_LIB_API IRHIDLSSContext
    {
    public:
        virtual ~IRHIDLSSContext() = default;

        // 出力解像度と品質モードから推奨レンダー解像度を問い合わせる。
        // 失敗時(その品質モードが非対応など)はログを出してfalseを返す
        virtual bool QueryOptimalSettings(
            uint32_t outputWidth, uint32_t outputHeight, DLSSQuality quality, DLSSOptimalSettings& outSettings) = 0;

        // 生成条件が前回と同じなら何もしない。変わっていれば作り直す。
        // 【コマンドリストが必要】NGXのフィーチャ生成はコマンドリストへコマンドを積む。
        // 記録中のコマンドリストを渡すこと。失敗時はログを出してfalseを返す
        virtual bool EnsureFeature(IRHICommandList* commandList, const DLSSFeatureDesc& desc) = 0;

        // 1フレーム分の評価。EnsureFeatureが成功していない状態で呼ぶとログを出してfalseを返す。
        // 【副作用】NGXは評価中にディスクリプタヒープとルートシグネチャを差し替えるため、
        // 実装はこの中でコマンドリストのバインドキャッシュを捨てる
        virtual bool Evaluate(IRHICommandList* commandList, const DLSSEvaluateDesc& desc) = 0;
    };
}
