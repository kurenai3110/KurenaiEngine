#pragma once

#include <cstdint>
#include "PassHost.h"

#include "EnvironmentConstants.h"
#include "ReflectionProbeConstants.h"

namespace Kurenai::Core
{
    class RenderGraph;
}

namespace Kurenai::Rendering
{
    struct RenderFrameContext;
    struct RenderBlackboard;
}

// 反射プローブのパス群(段階6)。
// ProbeBakeCapture / ProbeBakeConvolvePrefilter / ProbeRealtime*。
//
// 【キャプチャ経路はDDGIと共有】キューブ面の行列は Rendering/CubeFaceMath.h にあり、
// 面ごとの射影(ProbeFaceProjection)と焼き込みに入れる灯の数(BakedLightCount)は
// フレームのスナップショット越しに両者へ配る。
namespace Kurenai
{
    class KurenaiEngine3D;

    namespace Passes
    {
        class ReflectionProbePasses
        {
        public:
            explicit ReflectionProbePasses(IPassHost& engine) : m_Engine(engine) {}

            void Register(
                Core::RenderGraph& graph,
                const Rendering::RenderFrameContext& frame,
                const Rendering::RenderBlackboard& bb);

            // 【publicにしてある】下の焼き上がり状態を**進める**のはこの群だけだが、
            // 外から立て直す経路がある: ImGuiのパネル(Bakeボタン)、シーン読み込み
            // (焼き直しの要求とラウンドロビンの仕切り直し)、フレーム定数の組み立て
            // (露出が大きく動いたときの焼き直しの要求)。読むだけの経路も同じ3つにある。
            // **参照を返すのは、これらの旧来の書き方をそのまま保つため**
            // (KurenaiEngine3D の同名のアクセサがここへ委譲している)
            bool& GetProbeBaked() { return m_ProbeBaked; }
            bool& GetProbeBakeRequested() { return m_ProbeBakeRequested; }
            float& GetProbeBakedExposureEV100() { return m_ProbeBakedExposureEV100; }
            uint32_t GetProbeRealtimeProbeIndex() const { return m_ProbeRealtimeProbeIndex; }
            uint32_t GetProbeRealtimeFace() const { return m_ProbeRealtimeFace; }
            // シーンが変わればプローブの数も並びも変わるため、Realtimeのラウンドロビンを
            // 先頭から仕切り直す。**プリフィルタの途中ステップは意図的に触らない**
            // (シーン読み込みが元から触っていなかった。挙動を変えないため揃えてある)
            void ResetRealtimeProgress()
            {
                m_ProbeRealtimeProbeIndex = 0;
                m_ProbeRealtimeFace = 0;
            }

        private:
            IPassHost& m_Engine;

            // 次のRender()でプローブを焼き直す要求。シーン読み込み時とImGuiのBakeボタンで立てる。
            // スカイボックス由来のIBLと違いシーンのジオメトリ・ライトに依存するため、
            // 「一度焼いたら二度と焼かない」ではなく明示的な要求ベースにしている
            bool m_ProbeBakeRequested = false;
            // 一度でも焼けたか。焼く前のプローブは中身が未定義なので、それまでは影響を無効にして
            // グローバルIBLのまま描く(未初期化のキューブマップが映り込むのを防ぐ)
            bool m_ProbeBaked = false;
            // Realtimeの進行状態。次に焼くプローブ番号と面番号
            uint32_t m_ProbeRealtimeProbeIndex = 0;
            uint32_t m_ProbeRealtimeFace = 0;
            // プリフィルタ畳み込みの途中ステップ。kProbePrefilterStepCount(36)が
            // 「プリフィルタ中でない」を表す番兵値。
            // **上の2つと違い群の外からは見えない** ―― 外が知りたいのは
            // 「どのプローブのどの面まで進んだか」であって、畳み込みの内訳ではない
            uint32_t m_ProbeRealtimePrefilterStep = kProbePrefilterStepCount;
            // OnDemandの変化検出用。最後にフルベイクを発行した時点の状態の署名。
            // 毎フレームの署名(RenderFrameContext::ProbeBakeSignature)と突き合わせ、
            // 変わっていれば焼き直しを要求する
            uint64_t m_ProbeBakeSignature = 0;
            // 最後にキャプチャしたときの実効プリ露出(KurenaiEngine3Dのm_EffectiveExposureEV100)。
            //
            // 【なぜ記録しておく必要があるか】プローブのキューブマップにはプリ露出済みの放射輝度が
            // 入っている(21.5節)。その倍率は時刻に連動して最大18段(約26万倍)動くのに対し、
            // Bakedモードのプローブはシーン読み込み時に一度焼いたきり更新されない。そのため
            // 焼いた時点と現在とで実効プリ露出が食い違うと、プローブの寄与だけが桁違いの明るさで
            // 合成される。実測では夜のProbeTestからSponzaへ切り替えたとき、EV100=-2.36で焼かれた
            // プローブをEV100=15.0のフレームが読み、17.4段(約17万倍)過剰になって画面が
            // 白飛びしたまま戻らなくなっていた。
            //
            // 空(手続き空)は実効プリ露出が0.05段動くたびに焼き直して追従しているが、プローブは
            // 1回のフルベイクが全プローブ×6面の描画になり同じ頻度では焼き直せない。そこで
            // 焼き直す代わりに、読み出し時へ 2^(焼いたEV - 現在のEV) を掛けて現在の露出へ
            // 換算する(FrameConstants.ProbeParams2.w、ReflectionProbe.hlsliのSampleEnvironment)。
            // キューブマップの中身は触らないのでfp16の値域も変わらない
            float m_ProbeBakedExposureEV100 = 15.0f;
        };
    }
}
