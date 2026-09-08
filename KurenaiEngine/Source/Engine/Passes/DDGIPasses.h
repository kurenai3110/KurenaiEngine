#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <DirectXMath.h>

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

            // アトラスを確保し直したときに、進行状態を一巡目からやり直させる。
            // **KurenaiEngine3D::RecreateDDGIAtlases から、アトラスを作った直後に呼ぶこと** ――
            // 中身が未定義の新しいアトラスに対して「もう焼いてある」と誤判定させないため。
            // probeCount は確保し直したアトラスが持つプローブ数
            void ResetProgress(uint32_t probeCount);

            // 【publicにしてある】焼き上がりと更新の状態を**進める**のはこの群だけだが、
            // ImGui のパネルが表示と手動リセットのために読み書きし、
            // 品質設定の切り替えも更新の一時停止を解除する。
            // KurenaiEngine3D の同名のアクセサがここへ委譲している
            bool& GetEmissiveSuppressLoggedRaster() { return m_DDGIEmissiveSuppressLoggedRaster; }
            bool& GetEmissiveSuppressLoggedTrace() { return m_DDGIEmissiveSuppressLoggedTrace; }
            bool& GetUpdateSuspended() { return m_DDGIUpdateSuspended; }
            uint32_t& GetStableCycles() { return m_DDGIStableCycles; }
            bool IsWarmingUp() const { return m_DDGIWarmingUp; }
            // 全プローブが一度でも書かれたか。書かれる前のアトラスは中身が未定義なので、
            // それまではDDGIを無効にして従来のIBLのまま描く
            bool IsBaked() const { return m_DDGIBaked; }

        private:
            KurenaiEngine3D& m_Engine;

            // 各スロットが「最後に焼いたときのワールド格子座標」。いまの座標と違えば未確定(dirty)。
            // 【ワールド座標で持つこと】アトラスのセル番号で持つと、スクロールしてもセル番号は
            // 変わらないので「別の場所を担当するようになった」ことを検出できない
            std::vector<DirectX::XMINT3> m_DDGIProbeBakedCoord;
            // 焼き直し待ちのスロット番号(毎フレーム組み直す。GPUへ渡す一時の並び)
            std::vector<uint32_t> m_DDGIDirtyProbeList;

            // 全プローブが一度でも書かれたか。書かれる前のアトラスは中身が未定義なので、
            // それまではDDGIを無効にして従来のIBLのまま描く(反射プローブの「一度でも焼けたか」と同じ方針)
            bool m_DDGIBaked = false;
            // 初回の一巡が終わっていないか。
            //
            // 【反射プローブと違い「フルベイク」を持たない】反射プローブは8個までなので全プローブを
            // 1フレームで焼けるが、DDGIは数百個ある。同じことをするとBistroのようなシーンでは
            // 数百×6回のシーン描画が1フレームに集中して数秒のハングになる。
            // DDGIはヒステリシスで時間収束させる手法なので、初回も時間分割で埋めるのが素直。
            // ただし初回だけは「前の値」が存在しないため、一巡目はヒステリシスを使わず上書きする
            // (未初期化のアトラスと混ぜてはいけない)
            bool m_DDGIWarmingUp = true;
            // 時間分割の進行状態。1フレームにm_DDGISettings.ProbesPerFrame個ずつ順に焼き直す
            uint32_t m_DDGIUpdateCursor = 0;
            // ヒステリシスを使わず上書きで焼き直す残りプローブ数。
            //
            // 【なぜ要るか】実効プリ露出は時刻に連動して最大18段動く(21.5節)。アトラス自体は
            // 露出非依存の物理量で持っているので数値が壊れることはないが、ヒステリシス0.97と
            // ラウンドロビンの積で時定数が約17秒あるため、時刻を大きく動かすとその間ずっと
            // 「前の時刻の間接光」が表示され続ける。露出が急変する時間帯ほど、この遅れが
            // 露出倍率で拡大されて目に見える(夕方に昼の間接光を夕方の露出で見ることになる)。
            // そこで露出が一定以上動いたら、一巡ぶんだけ上書きへ切り替えて即座に追従させる。
            // m_DDGIWarmingUpと違いDDGI自体は有効なまま(無効にすると従来のIBLとの間でちらつく)
            uint32_t m_DDGIOverwriteRemaining = 0;
            // 最後にアトラスを追従させた時点の実効プリ露出EV100
            float m_DDGILastExposureEV100 = 0.0f;
            bool m_DDGILastExposureValid = false;
            // どちらの経路(ラスタ / DXR)が実際に走ったかを、切り替わったときだけログへ出すための状態。
            // 毎フレーム出すと埋もれるが、出さないと「切り替えたつもり」の取り違えに気づけない
            bool m_DDGIRayModeReported = false;
            bool m_DDGIRayModeReportedRaytraced = false;
            // DDGIの二重計上の抑止が「実際に何をしたか」を1回だけログへ出したか。
            // 【絵から分からない】抑止はプローブのイラディアンスにしか出ず、しかも
            // 「効いていない」と「効いた結果が小さい」が同じ絵になる。実効値を出すしかない。
            //
            // 【2経路で別々に持つ】1つのフラグを共有すると、先に走ったほうがもう一方のログを
            // 永久に潰す。どちらの経路の話なのか区別できないログは、切り分けの役に立たない
            bool m_DDGIEmissiveSuppressLoggedRaster = false;
            bool m_DDGIEmissiveSuppressLoggedTrace = false;
            // 停止判定用。最後に「焼き上がりに影響する状態」が変わった時点の署名。
            // 反射プローブと同じKurenaiEngine3D::ComputeProbeBakeSignature()を使う ――
            // DDGIのキャプチャも同じFrameConstants(太陽・時刻・影・ライト・IBL・自発光)を
            // 読むため、影響する状態は同じ
            uint64_t m_DDGIBakeSignature = 0;
            bool m_DDGIBakeSignatureValid = false;
            // 署名が変わらないまま完了した巡回数。停止判定に使う
            uint32_t m_DDGIStableCycles = 0;
            // 収束済みとみなして更新を止めている状態。署名が変わると倒れる。
            // 停止までの巡回数(kDDGIBounceCycles)の根拠は Passes/DDGIConstants.h を参照
            bool m_DDGIUpdateSuspended = false;

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
