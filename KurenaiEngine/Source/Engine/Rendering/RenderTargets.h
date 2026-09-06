#pragma once

#include <cstdint>
#include <memory>

#include "RHI/IRHIDevice.h"

namespace Kurenai::Rendering
{
    // 複数のパス群が読み書きする共有レンダーターゲット。
    // 【なぜ群へ渡さないか】G-Buffer は8群が触る。どれか1つの群に持たせると、
    // 他の群がその群を経由して取りに行くことになり、依存が増える。ここが唯一の持ち主。
    struct RenderTargets
    {
        std::unique_ptr<RHI::IRHITexture> GBufferAlbedo;
        std::unique_ptr<RHI::IRHITexture> GBufferNormal;
        std::unique_ptr<RHI::IRHITexture> GBufferMaterial;
        // 自発光(エミッシブ)。AO/シャドウの影響を受けずライティングパスで常に加算される
        std::unique_ptr<RHI::IRHITexture> GBufferEmissive;
        std::unique_ptr<RHI::IRHITexture> GBufferDepth;
        // モーションベクター(速度バッファ)。「この画素に映っているものが前フレームでは画面の
        // どこにいたか」をUV単位の2Dベクトルで持ち、TAAが履歴を引く位置の決定に使う。
        // 現在のシーンは全インスタンスが静的(ModelInstance::Worldは読み込み時に確定し以降
        // 変わらない)なので、速度の発生源はカメラの移動・回転だけである。そのためGBuffer.hlslは
        // 同じワールド座標を今フレームと前フレームのビュー射影行列で投影して差を取るだけでよく、
        // インスタンスごとの前フレームのワールド行列(PrevWorld)を持つ必要がない。
        // 動的オブジェクトを入れる際はObjectConstantsへPrevWorldを追加すること
        std::unique_ptr<RHI::IRHITexture> GBufferVelocity;
        // bent normal(ワールド空間の正規化しない可視方向の平均)。.rgb = bRaw、.a = 有効フラグ
        std::unique_ptr<RHI::IRHITexture> GBufferBentNormal;
        // RTシャドウの可視率(0〜1のスカラー)。RWTexture2D<float>として書くため単チャンネルの
        // R32_Floatにする(型付きUAVの読み書きが保証されているのはR32系のみ。AutoExposure.hlsl参照)
        std::unique_ptr<RHI::IRHITexture> RTShadowTexture;

        // 全カスケードの深度を1つのTexture2DArray(スライス番号=カスケード番号)として保持する。
        // 書き込みはスライスごとの個別DSV(RenderGraphPassDesc::DepthTargetArraySlice)で行い、
        // 読み取りは配列全体を指す1本のSRV(t4)を1回バインドするだけでよい。シェーダ側は
        // ShadowMapArray.Sample(DataSampler, float3(uv, cascadeIndex))で動的にカスケードを選べる
        // (ShadowSampling.hlsli参照)。ウィンドウ/レンダー解像度に依存しないため一度だけ作成し、
        // 解像度変更時に作り直す他のメンバとは生成契機が異なる。
        std::unique_ptr<RHI::IRHITexture> ShadowCascadeArray;

        // 直接光(シャドウ適用済みのPBR直接光をHDRで持つ)。DeferredLightingパスと
        // SSIL_VisibilityBitmask.hlslの両方が読むため、G-Bufferと同じレンダー解像度で保つ
        std::unique_ptr<RHI::IRHITexture> DirectLightTexture;
        // AO/GIの生バッファとブラー後。フォーマットはどちらもGetAOFormat()に従う
        // (バッファ精度の設定に追従する)
        std::unique_ptr<RHI::IRHITexture> SSAORawTexture;
        std::unique_ptr<RHI::IRHITexture> SSAOTexture;
        std::unique_ptr<RHI::IRHITexture> SSILRawTexture;
        std::unique_ptr<RHI::IRHITexture> SSILTexture;
        // ライティングパスの出力。トーンマッピング前のHDR値をそのまま持つ
        std::unique_ptr<RHI::IRHITexture> SceneColor;
        // SSRの出力。後段(Tonemap)から見るとSceneColorと入れ替え可能なバッファになる
        std::unique_ptr<RHI::IRHITexture> SSRTexture;
        // Tonemapの出力(LDR)。内部レンダー解像度で、超解像の出力とは作り直す契機が違う
        std::unique_ptr<RHI::IRHITexture> TonemapTexture;
        // TAAの履歴2枚。読みながら同じテクスチャへ書けないので毎フレーム役割を入れ替える
        // (どちらが今フレームの書き込み先かはKurenaiEngine3D::m_TAAHistoryIndexが持つ)
        std::unique_ptr<RHI::IRHITexture> TAAHistory[2];
        // 階層深度。ミップ段数はKurenaiEngine3D側が決めてCreateHiZへ渡す
        std::unique_ptr<RHI::IRHITexture> HiZTexture;

        // G-Buffer の生成は元の位置ごとに3つへ分ける。間に他のテクスチャ生成があるため、
        // 順序を変えるとDX12のディスクリプタ枠の割り当て順が変わり、意味の無い差分になる。
        // 呼び出し元のtry内から呼ぶこと。確保失敗時のHDR→Legacy8bitフォールバックは
        // KurenaiEngine3D::CreateRenderTargets が持つ。
        void CreateGBufferCore(RHI::IRHIDevice& device, uint32_t width, uint32_t height, RHI::Format emissiveFormat);
        void CreateGBufferVelocity(RHI::IRHIDevice& device, uint32_t width, uint32_t height);
        void CreateGBufferBentNormal(RHI::IRHIDevice& device, uint32_t width, uint32_t height);
        void CreateLightingChain(RHI::IRHIDevice& device, uint32_t width, uint32_t height, RHI::Format aoFormat);
        void CreateTonemap(RHI::IRHIDevice& device, uint32_t width, uint32_t height);
        void CreateRTShadow(RHI::IRHIDevice& device, uint32_t width, uint32_t height);
        void CreateTAAHistory(RHI::IRHIDevice& device, uint32_t width, uint32_t height);
        void CreateHiZ(RHI::IRHIDevice& device, uint32_t width, uint32_t height, uint32_t mipLevels);
        void CreateShadowCascadeArray(RHI::IRHIDevice& device, uint32_t size, uint32_t cascadeCount);
    };
}
