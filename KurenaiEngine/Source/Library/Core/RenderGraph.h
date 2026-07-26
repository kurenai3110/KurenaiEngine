#pragma once

#include <functional>
#include <string>
#include <vector>

#include "KurenaiTypes.h"

#include "RHI/IRHICommandList.h"
#include "RHI/IRHIGPUProfiler.h"
#include "RHI/IRHISwapChain.h"
#include "RHI/IRHITexture.h"

#include "Core/CPUProfiler.h"

// dllexportされたクラスが非export型(std::vector<RenderGraphPassDesc>など)をメンバに持つ
// ことによるC4251警告を抑制する。KurenaiEngineLibrary.dllと各サンプルは常に同一コンパイラ・
// 同一ランタイムライブラリ設定でビルドされるため、実務上は問題にならない
#pragma warning(push)
#pragma warning(disable: 4251)

namespace Kurenai::Core
{
    // レンダーグラフの1パスが読み書きするリソースと実行内容の宣言。
    // RenderTargets/DepthTargetとSwapChainTargetは同時に指定しない(どちらか一方、または両方とも未指定)
    struct RenderGraphPassDesc
    {
        // GPU/CPUプロファイラのスコープ名として使う(既存のBeginScope("Shadow")等と同じ命名)
        std::string Name;

        // このパスがSRV/コンピュートSRVとして読むテクスチャ。パスの実行順序を決めるための
        // 依存関係の宣言のみに使い、実際のバインドはExecute内で行う(RenderGraphは自動バインドしない)
        std::vector<RHI::IRHITexture*> Reads;

        // このパスがRTVとして書くテクスチャ。SetRenderTargetsで自動的にバインドされる
        std::vector<RHI::IRHITexture*> RenderTargets;
        // このパスがDSVとして書くテクスチャ
        RHI::IRHITexture* DepthTarget = nullptr;

        // RTV/DSV以外の手段(コンピュートシェーダーのUAV書き込み等)でこのパスが書くテクスチャ。
        // RenderTargets/DepthTargetと違い自動バインドはされず、依存関係の解決にのみ使う
        // (実際のUAVバインドはExecute内でSetComputeUnorderedAccessTexture等を呼んで行う)
        std::vector<RHI::IRHITexture*> Writes;

        // バックバッファへ直接描画するパス(Present)の場合に指定する。RenderTargets/DepthTargetの
        // 代わりにSetRenderTarget(swapChain)で自動的にバインドされる。vsync有効時、このバインド呼び出し
        // 自体がバックバッファ確保待ちで内部的にブロックしうるため、他のパスと異なりこのバインドは
        // BeginScopeより前(パスの計測スコープ外)で行われる
        RHI::IRHISwapChain* SwapChainTarget = nullptr;

        // パス本体。SetRenderTargets/SwapChainのバインドが済んだ状態で呼ばれるため、
        // ビューポート設定・クリア・描画/ディスパッチ呼び出しのみを行えばよい
        std::function<void(RHI::IRHICommandList*)> Execute;
    };

    // 宣言されたパス群をリソースの読み書き依存関係から自動的に順序付けて実行する軽量レンダーグラフ。
    // トランジェントリソースの確保・エイリアシングは行わず、既存の永続確保済みテクスチャ(G-Buffer・
    // SceneColor等)をそのまま扱う。DX12側のリソース状態遷移はテクスチャ単位で自動化済み
    // (DX12Texture::TransitionTo、SetRenderTargets/SetTexture呼び出し時に暗黙に発行される)ため、
    // このクラスの責務は純粋に「パスの実行順序の決定」「レンダーターゲットの自動バインド」
    // 「CPU/GPUプロファイラのスコープ計測」の3点に絞っている
    class KURENAI_LIB_API RenderGraph
    {
    public:
        RenderGraph(RHI::IRHICommandList* commandList, RHI::IRHIGPUProfiler* gpuProfiler, CPUProfiler* cpuProfiler);

        // パスを1つ登録する。登録順は実行順のヒント(依存関係が同点の場合はこの順が優先される)であり、
        // 実際の実行順は各パスのReads/RenderTargets/DepthTargetから解決した依存関係で決まる
        void AddPass(RenderGraphPassDesc desc);

        // 依存関係からパスの実行順序を解決し、順番にExecuteを呼び出す。
        // 循環依存(あるパスが直接・間接に自分自身の出力に依存する)を検出した場合は
        // std::runtime_errorを送出する
        void Execute();

    private:
        RHI::IRHICommandList* m_CommandList;
        RHI::IRHIGPUProfiler* m_GPUProfiler;
        CPUProfiler* m_CPUProfiler;
        std::vector<RenderGraphPassDesc> m_Passes;

        // 依存関係からパスの実行順序(m_Passesへのインデックス列)を解決する。
        // 循環依存があった場合はstd::runtime_errorを送出する
        std::vector<size_t> ResolveExecutionOrder() const;
    };
}

#pragma warning(pop)
