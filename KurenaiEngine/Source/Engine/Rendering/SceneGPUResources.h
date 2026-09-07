#pragma once

#include <memory>

#include "Assets/RaytracingScene.h"
#include "RHI/IRHIDevice.h"

// GPU側のシーンデータ。シーンの読み込みで作り直され、複数のパス群が読む。
//
// 【なぜ1つにまとめるか】レイトレーシングの高速化構造は5つ、ライトのリストは5つ、
// インスタンスのバッファは4つのパス群が読む。エンジンのメンバのままだと friend が要る。
//
// 【このメンバは m_Scene より後に宣言すること】メンバの破棄は宣言の逆順なので、
// 後に宣言すると m_Scene の頂点/インデックスバッファより先に破棄される。
// RaytracingScene の BLAS/TLAS はそれらを参照しているため、順序が逆だと
// 参照先が先に消える。**この構造体の宣言位置を動かすときは必ず確かめること。**

namespace Kurenai::Rendering
{
    struct SceneGPUResources
    {
        // ポイント/スポットライトのリスト(t8、StructuredReadOnly)。
        // 太陽(平行光)はb0のLightDirection/LightColorのまま(詳細はdocs/Architecture.html参照)
        std::unique_ptr<RHI::IRHIBuffer> LightBuffer;

        // m_ModelInstanceRecords のレコードを載せる StructuredBuffer。**1フレームに1回だけ更新する** ――
        // パスごとに詰め直す案は、DX12 の StructuredReadOnly が
        // MaxUpdatesPerFrame x kFrameCount + 1 段の UPLOAD ヒープを常時確保するため、
        // 反射プローブの6面ぶんを見込むと VRAM が跳ねる(DX12Device::CreateBuffer)
        std::unique_ptr<RHI::IRHIBuffer> ModelInstanceBuffer;

        // m_Sceneに対応するレイトレーシングの高速化構造(BLAS/TLAS)とシーンジオメトリの
        // 統合バッファ。Loaderスレッドがm_Sceneと一緒に構築し、ApplyLoadedSceneが差し替える。
        // デバイスがレイトレーシング非対応(DX11、またはDXR Tier 1.1未満のアダプタ)の
        // 場合は空のまま(IsValid()==false)で、描画側は従来のスクリーンスペース手法を使う。
        Assets::RaytracingScene RaytracingScene;
    };
}
