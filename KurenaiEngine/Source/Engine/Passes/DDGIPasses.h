#pragma once

#include <memory>
#include <string>

#include "RHI/IRHIDevice.h"

#include "DDGIConstants.h"

namespace Kurenai::Core
{
    class RenderGraph;
}

namespace Kurenai::Rendering
{
    struct RenderFrameContext;
    struct RenderBlackboard;
}

// DDGI のパス群(段階6)。
//
// 【登録位置が2箇所に離れている】プローブの捕捉と更新(DDGIInvalidate / DDGIUpdate)は
// グラフの前寄り、格子から画面へ解決する DDGIResolve は AO の後ろに登録される。
// 登録順は実行順の一部なので寄せられない。したがって登録の入口を分ける。
//
// 【キャプチャ経路は反射プローブと共有】キューブ面の行列は Rendering/CubeFaceMath.h、
// 面の射影・灯の数・読むテクスチャ一式はフレームのスナップショットから受け取る。
namespace Kurenai
{
    class KurenaiEngine3D;

    namespace Passes
    {
        class DDGIPasses
        {
        public:
            explicit DDGIPasses(KurenaiEngine3D& engine) : m_Engine(engine) {}

            // プローブの捕捉と更新(DDGIInvalidate / DDGIUpdate<probe>)
            void RegisterProbeUpdate(
                Core::RenderGraph& graph,
                const Rendering::RenderFrameContext& frame,
                const Rendering::RenderBlackboard& bb);

            // 格子から画面へ解決する(DDGIResolve)。捕捉・更新とは登録位置が離れている
            void RegisterResolve(Core::RenderGraph& graph, const Rendering::RenderFrameContext& frame);

            // 【エンジン側の元の行位置から呼ぶこと】DX12はディスクリプタ枠を生成順に
            // 割り当てるため、生成の呼び出しを寄せ集めると他のリソースとの前後関係が崩れ、
            // パスマニフェストの採取が一斉に不一致になる。所有権だけをこの群へ移し、
            // 呼び出しは CreateSceneResources() の元あった場所に残してある
            void CreateResolvePipelineState(RHI::IRHIDevice& device, const std::wstring& shaderDirectory);
            void CreateCaptureResources(RHI::IRHIDevice& device);
            void CreateProbeUpdateResources(RHI::IRHIDevice& device, const std::wstring& shaderDirectory);

            // レイ取得(DXR)。**呼び出し元の try の中から呼ぶこと** ――
            // DDGIProbeTrace.hlsl はシェーダーモデル6.6を要求し、RT反射/RTシャドウ/RTAOが
            // 作れる環境でもこれだけ作れないことがある。失敗したら呼び出し元が catch で
            // 下の Reset を呼び、ラスタ経路のまま動かす
            void CreateRaytracedTraceResources(RHI::IRHIDevice& device, const std::wstring& shaderDirectory);
            void ResetRaytracedTraceResources();
            // 上が使える状態か。KurenaiEngine3D::ShouldRunRaytracedDDGITrace が実行可否の判定に使う
            bool HasRaytracedTraceResources() const
            {
                return m_DDGIProbeTracePipelineState != nullptr && m_DDGITraceConstantBuffer != nullptr;
            }

        private:
            KurenaiEngine3D& m_Engine;

            // 格子から画面へ解決する低解像度パス(雲パスと同じ作り。拡散イラディアンスと
            // insideWeightを書く)。書き先2枚と実寸は GIResources 側が持つ
            std::unique_ptr<RHI::IRHIShader> m_DDGIResolveVertexShader;
            std::unique_ptr<RHI::IRHIShader> m_DDGIResolvePixelShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_DDGIResolvePipelineState;

            // キャプチャ経路は反射プローブとまったく同じ(ProbeCapture.hlslと
            // GIResources::ProbeCapturePipelineStateをそのまま使う)で、解像度だけ
            // kDDGICaptureSizeへ落とす。レンダーターゲットのフォーマットはPSOと
            // 一致していなければならないため、反射プローブ側と同じ組み合わせにする
            std::unique_ptr<RHI::IRHITexture> m_DDGICaptureColor;
            std::unique_ptr<RHI::IRHITexture> m_DDGICaptureDistance;
            std::unique_ptr<RHI::IRHITexture> m_DDGICaptureDepth;
            // 6面を組み上げるスクラッチのキューブ。更新CSは1テクセル(=1つの方向)を出力するのに
            // 6面ぶん1536本のレイを全て走査するため、面ごとの2Dテクスチャではキューブとして
            // 引けず具合が悪い。放射輝度と距離で2本要る
            std::unique_ptr<RHI::IRHITexture> m_DDGICaptureRadianceCube;
            std::unique_ptr<RHI::IRHITexture> m_DDGICaptureDistanceCube;

            // プローブの更新CS。アトラス(GIResources::DDGIIrradianceAtlas / DDGIDistanceAtlas)へ
            // ヒステリシス付きで書き戻す
            std::unique_ptr<RHI::IRHIShader> m_DDGIProbeUpdateComputeShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_DDGIProbeUpdatePipelineState;
            // 境界の複製は本体の書き込みが全て終わってからでなければ正しい値を読めないため、
            // 同じディスパッチ内では行えず別パスになる(オクタヘドラルの縁は対辺へ折り返して繋がるので、
            // 自分のセルの反対側のテクセルを読む必要がある)
            std::unique_ptr<RHI::IRHIShader> m_DDGIBorderCopyComputeShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_DDGIBorderCopyPipelineState;
            std::unique_ptr<RHI::IRHIBuffer> m_DDGIUpdateConstantBuffer;
            // スクロールで未確定になったプローブを、焼き直されるまでサンプリングから外すパス
            std::unique_ptr<RHI::IRHIShader> m_DDGIInvalidateProbesComputeShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_DDGIInvalidateProbesPipelineState;
            // 焼き直し待ちのスロット番号を渡す。最悪ケース(全プローブが一度に未確定)でも足りる大きさ
            std::unique_ptr<RHI::IRHIBuffer> m_DDGIDirtyProbeBuffer;

            // レイ取得(DXR)。非対応環境では3本ともnullptrのままで、ラスタ経路が使われる
            std::unique_ptr<RHI::IRHIShader> m_DDGIProbeTraceComputeShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_DDGIProbeTracePipelineState;
            std::unique_ptr<RHI::IRHIBuffer> m_DDGITraceConstantBuffer;
        };
    }
}
