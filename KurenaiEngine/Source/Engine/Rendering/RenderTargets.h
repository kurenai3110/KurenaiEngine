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

        // G-Buffer の生成は元の位置ごとに3つへ分ける。間に他のテクスチャ生成があるため、
        // 順序を変えるとDX12のディスクリプタ枠の割り当て順が変わり、意味の無い差分になる。
        // 呼び出し元のtry内から呼ぶこと。確保失敗時のHDR→Legacy8bitフォールバックは
        // KurenaiEngine3D::CreateRenderTargets が持つ。
        void CreateGBufferCore(RHI::IRHIDevice& device, uint32_t width, uint32_t height, RHI::Format emissiveFormat);
        void CreateGBufferVelocity(RHI::IRHIDevice& device, uint32_t width, uint32_t height);
        void CreateGBufferBentNormal(RHI::IRHIDevice& device, uint32_t width, uint32_t height);
    };
}
