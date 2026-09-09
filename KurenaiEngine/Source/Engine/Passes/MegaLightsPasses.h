#pragma once

#include <memory>
#include "PassHost.h"
#include <string>

#include "RHI/IRHIDevice.h"

#include "MegaLightsConstants.h"

namespace Kurenai::Core
{
    class RenderGraph;
}

namespace Kurenai::Rendering
{
    struct RenderFrameContext;
    struct RenderBlackboard;
}

// タイルライトカリングと MegaLights のパス群(段階6)。
// LightCull / MegaLightsPool / MegaLights / MegaLights の確率的サンプリング系14本。
//
// 【デノイズを走らせたかをブラックボードへ載せる】後段の直接光パスが
// 「生出力とデノイズ後のどちらを t7 へ張るか」をこの値で決めるため、bb は非 const で受ける。
namespace Kurenai
{
    class KurenaiEngine3D;

    namespace Passes
    {
        class MegaLightsPasses
        {
        public:
            explicit MegaLightsPasses(IPassHost& engine) : m_Engine(engine) {}

            void Register(
                Core::RenderGraph& graph,
                const Rendering::RenderFrameContext& frame,
                Rendering::RenderBlackboard& bb);

            // 【エンジン側の元の行位置から呼ぶこと】DX12はディスクリプタ枠を生成順に
            // 割り当てるため、生成の呼び出しを寄せ集めると他のリソースとの前後関係が崩れ、
            // パスマニフェストの採取が一斉に不一致になる。所有権だけをこの群へ移し、
            // 呼び出しは CreateSceneResources() の元あった場所に残してある
            void CreateLightCullingPipelineState(RHI::IRHIDevice& device, const std::wstring& shaderDirectory);
            // **DXR対応環境でのみ呼ばれる。** 非対応ならPSOがnullptrのままで、
            // KurenaiEngine3D の実行可否の判定が false を返し登録自体が起きない
            void CreateStochasticPipelineStates(RHI::IRHIDevice& device, const std::wstring& shaderDirectory);

            // 実行可否の判定に使う述語。エンジンの Should* から呼ぶ
            bool HasReferencePipelineState() const { return m_MegaLightsReferencePipelineState != nullptr; }
            // 【手法ごとに必要なPSOが違う】確率的サンプリングもクアッド共有も候補プールと
            // 初期RIS(リザーバ)を共有し、そのあとの段だけが違う。共有分をここで見て、
            // 手法ごとの段は呼び出し側が Has*PipelineState と併せて判定する。
            // **まとめて1つの述語にしないこと** ―― クアッド共有は Shade を使わない
            // (色を書くのは Resolve)ので、Shade まで見ると本来動く構成で黙って止まる
            bool HasCommonPipelineStates() const
            {
                return m_MegaLightsInitialPipelineState != nullptr && m_MegaLightsTilePoolPipelineState != nullptr;
            }
            bool HasShadePipelineState() const { return m_MegaLightsShadePipelineState != nullptr; }
            bool HasResolvePipelineState() const { return m_MegaLightsResolvePipelineState != nullptr; }
            bool HasTemporalPipelineState() const { return m_MegaLightsTemporalPipelineState != nullptr; }
            bool HasDenoisePipelineStates() const { return m_MegaLightsDenoiseTemporalPSO != nullptr; }

            // 履歴の反転と有効化。**Render()の末尾から呼ぶこと** ―― 書いた側を次フレームが
            // 読むので、反転したあとに有効化する。早く立てると未初期化の内容を履歴として読む
            void AdvanceHistory(bool temporalRan);
            void AdvanceDenoiseHistory(bool denoiseRan);
            // 解像度が変わると添字の意味が変わる。バッファのクリアが無いRHIなので、
            // シェーダ側へ「履歴を読むな」と伝えるために倒す
            void InvalidateHistory() { m_MegaLightsHistoryValid = false; }
            void InvalidateDenoiseHistory() { m_MegaLightsDenoiseHistoryValid = false; }
            // 蓄積と書き出しを取り直す。解像度が変わったときに呼ぶ
            void ResetAccumulation();
            // 書き出し先。空なら書き出さない
            void SetDumpPath(const std::wstring& path) { m_MegaLightsDumpPath = path; }
            void ClearDumpPath() { m_MegaLightsDumpPath.clear(); }
            // 蓄積の枚数を変えたときに取り直す。途中まで足した状態に継ぎ足すと
            // 「何サンプルの平均か」が分からなくなる
            void ResetAccumFrames() { m_MegaLightsAccumFrames = 0; }
            // 整定待ちが済んだか。摂動を効かせる判定にエンジンが使う
            uint32_t GetAccumWarmupFrames() const { return m_MegaLightsAccumWarmupFrames; }

        private:
            IPassHost& m_Engine;

            // 時間再利用の履歴の書き込み先(RenderTargets::MegaLightsReservoirHistory の添字)。
            // もう一方が前フレームの結果。Render()の末尾で反転する
            uint32_t m_MegaLightsHistoryIndex = 0u;
            // 履歴の中身が今の解像度・今のシーンのものとして使えるか。
            // バッファのクリアが無いRHIなので、無効な間はシェーダへ「履歴を読むな」と伝える
            bool m_MegaLightsHistoryValid = false;
            // デノイザの履歴の書き込み先と有効性。
            // 【時間再利用の有無には依存しない】デノイザは「出た色」をならすもので、
            // リザーバを混ぜる時間再利用とは独立に効く
            uint32_t m_MegaLightsDenoiseHistoryIndex = 0u;
            bool m_MegaLightsDenoiseHistoryValid = false;

            // --- 蓄積平均(計測専用) ---
            // これまでに足したフレーム数。表示側はこれで割る
            uint32_t m_MegaLightsAccumFrames = 0;
            // レンダーターゲットを作り直してから何フレーム経ったか。
            //
            // 【整定を待たずに足し始めると測定そのものが壊れる】起動直後はモデルとテクスチャが
            // ストリーミングで入ってくる途中で、シーンがRenderResolutionを持つ場合は内部解像度も
            // 既定値(1920x1080)から切り替わる。その間の絵を混ぜて平均すると、**別のシーンの平均**を
            // 測ることになる。実際に、待たずに書き出したときは当時の既定1280x720のまま吐き出された
            uint32_t m_MegaLightsAccumWarmupFrames = 0;
            // 蓄積し終えた平均をこのパスへ生データで書き出す(空なら書き出さない)。
            //
            // 【なぜ画面キャプチャでは足りないのか】画面から採れるのは8bitで、しかも
            // トーンマップを通っている。ここで測りたいのは「平均が真値へ 1/√N で寄るか」で、
            // 8bitの丸めだけでRMSEに0.29階調の下限が生まれ、その下限に隠れて比が読めなくなる。
            // 物差しの分解能が足りないまま「1/√Nで落ちていない」と読むと、原因を取り違える
            std::wstring m_MegaLightsDumpPath;
            bool m_MegaLightsDumpIssued = false;
            bool m_MegaLightsDumpDone = false;
            // コピーを積んだフレーム番号(0なら未発行)。GPUの実行はCPUより数フレーム遅れるので、
            // 積んだ直後に読んではいけない(IRHICommandList::CopyBufferToReadback のコメント)
            uint32_t m_MegaLightsDumpCopyFrame = 0;
            // 読み戻しの受け皿
            std::unique_ptr<RHI::IRHIBuffer> m_MegaLightsAccumReadback;

            // タイルライトカリング(タイルごとに届くライトのインデックスリストを作る)。
            // ライトグリッド本体は解像度に依存するため RenderTargets::LightTileBuffer が持つ
            std::unique_ptr<RHI::IRHIShader> m_LightCullingComputeShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_LightCullingPipelineState;
            std::unique_ptr<RHI::IRHIBuffer> m_LightCullingConstantBuffer;

            // MegaLightsの参照実装(ポイント/スポットライトを全灯総当たりし、届いた1灯ごとに
            // 光源までの影レイを撃つ)。確率的サンプリングを評価するときの真値を作るためのパスで、
            // RayQueryを含むためシェーダーモデル6.5が要る
            std::unique_ptr<RHI::IRHIShader> m_MegaLightsReferenceComputeShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_MegaLightsReferencePipelineState;
            std::unique_ptr<RHI::IRHIBuffer> m_MegaLightsConstantBuffer;

            // MegaLightsの候補プール(MegaLightsTilePool.hlsl)。タイルごとに「届くライト」を走査し、
            // 寄与に比例した確率でK灯を重みつきで抽出する。読み手は Initial(RISの提案分布)と
            // Spatial(不偏化の分母で「その灯が隣のタイルへ届くか」を判定する)。
            // 参照実装はこれを使わず全灯を回すので、参照実装のときは出力が使われない。
            // レイを撃たないパスだがMegaLightsと同時にしか使わないので、生成もRT対応時だけにしてある。
            // 候補プール本体は、PresentPassのタイル表示も読むため RenderTargets が持つ
            std::unique_ptr<RHI::IRHIShader> m_MegaLightsTilePoolComputeShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_MegaLightsTilePoolPipelineState;
            std::unique_ptr<RHI::IRHIBuffer> m_MegaLightsTilePoolConstantBuffer;

            // 確率的サンプリング本体。2パスに分かれる。
            //   Initial (MegaLightsInitialSample.hlsl) … 候補プールからM個引きRISで1灯へ絞り、
            //                                            結果を**リザーバ**として書く(色は作らない)
            //   Shade   (MegaLightsShade.hlsl)         … そのリザーバへ影レイを1本撃ちHDRを書く
            //
            // 【なぜ分けるのか】時間・空間の再利用は「どの灯を選んだか」を持ち回って現フレームで
            // 評価し直す形でしか書けない。選択とシェードが1パスに混ざっていると再利用の段を
            // 差し込む場所が無い。出力先は参照実装と同じRenderTargets::MegaLightsTexture
            // (同じ表示経路・同じ後段のままA/Bが撮れるようにするため)
            std::unique_ptr<RHI::IRHIShader> m_MegaLightsInitialComputeShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_MegaLightsInitialPipelineState;
            std::unique_ptr<RHI::IRHIShader> m_MegaLightsShadeComputeShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_MegaLightsShadePipelineState;
            // クアッド共有(手法3)の解決パス。Initial が書いた4画素ぶんのリザーバを読み、
            // **自分の面で評価し直して平均する**。レイを1本も撃たないのでTLASを束縛しない。
            // Shade と分けてあるのは、あちらが RayQuery を持ちSM6.5でしか焼けないのに対し、
            // こちらはレイを撃たず3バリアントすべてで焼けるため
            // (混ぜると使わないTLASを束縛したままレイ経路が残る)
            std::unique_ptr<RHI::IRHIShader> m_MegaLightsResolveComputeShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_MegaLightsResolvePipelineState;
            // 空間再利用(MegaLightsSpatial.hlsl)。近傍が選んだ灯を借りて自分の面で評価し直す。
            // レイは1本も増えない ―― 借りるのは「どの灯か」だけ
            std::unique_ptr<RHI::IRHIShader> m_MegaLightsSpatialComputeShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_MegaLightsSpatialPipelineState;
            std::unique_ptr<RHI::IRHIShader> m_MegaLightsTemporalComputeShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_MegaLightsTemporalPipelineState;

            // デノイズ(時間累積 → a-trous → 再変調)
            std::unique_ptr<RHI::IRHIShader> m_MegaLightsDenoiseTemporalShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_MegaLightsDenoiseTemporalPSO;
            std::unique_ptr<RHI::IRHIShader> m_MegaLightsDenoiseAtrousShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_MegaLightsDenoiseAtrousPSO;
            std::unique_ptr<RHI::IRHIShader> m_MegaLightsDenoiseRemodulateShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_MegaLightsDenoiseRemodulatePSO;
            std::unique_ptr<RHI::IRHIBuffer> m_MegaLightsDenoiseConstantBuffer;

            // 2パスで共有する定数バッファ(中身はフレーム内で不変なのでInitial側で1回更新する)
            std::unique_ptr<RHI::IRHIBuffer> m_MegaLightsStochasticConstantBuffer;
            // 空間再利用の反復ごとの定数(中身は共有分と同じで、反復番号だけが違う)。
            // 【1本を使い回してはいけない】UpdateBuffer は同じフレームで2回書くと
            // 後の値が両方のパスに見えるため、反復の数だけバッファを分ける
            std::unique_ptr<RHI::IRHIBuffer> m_MegaLightsSpatialConstantBuffer[Passes::kMegaLightsMaxSpatialIterations];

            // 蓄積平均(計測専用)。レイを撃たないがMegaLightsと同時にしか使わない
            std::unique_ptr<RHI::IRHIShader> m_MegaLightsAccumComputeShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_MegaLightsAccumPipelineState;
            std::unique_ptr<RHI::IRHIBuffer> m_MegaLightsAccumConstantBuffer;
        };
    }
}
