#include "Core/RenderGraph.h"

#include <queue>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace Kurenai::Core
{
    RenderGraph::RenderGraph(RHI::IRHICommandList* commandList, RHI::IRHIGPUProfiler* gpuProfiler, CPUProfiler* cpuProfiler)
        : m_CommandList(commandList)
        , m_GPUProfiler(gpuProfiler)
        , m_CPUProfiler(cpuProfiler)
    {
    }

    void RenderGraph::AddPass(RenderGraphPassDesc desc)
    {
        if (!desc.Execute)
        {
            throw std::runtime_error("RenderGraph: パス '" + desc.Name + "' にExecuteが設定されていません");
        }
        if (desc.SwapChainTarget != nullptr && (!desc.RenderTargets.empty() || desc.DepthTarget != nullptr))
        {
            throw std::runtime_error("RenderGraph: パス '" + desc.Name + "' はSwapChainTargetとRenderTargets/DepthTargetを同時に指定できません");
        }
        if (desc.DepthTargetArraySlice != 0 && desc.DepthTarget == nullptr)
        {
            throw std::runtime_error("RenderGraph: パス '" + desc.Name + "' はDepthTargetを指定せずにDepthTargetArraySliceを指定しています");
        }

        m_Passes.push_back(std::move(desc));
    }

    std::vector<size_t> RenderGraph::ResolveExecutionOrder() const
    {
        const size_t passCount = m_Passes.size();

        // 各テクスチャを最後に書いたパスのインデックス。同じテクスチャに複数のパスが書く場合
        // (Write-after-Write)も順序を保持するため、書き込みのたびにここを更新する
        std::unordered_map<const RHI::IRHITexture*, size_t> lastWriter;

        std::vector<std::vector<size_t>> dependents(passCount); // パスi -> iの完了を待つパス群
        std::vector<size_t> inDegree(passCount, 0);

        for (size_t i = 0; i < passCount; ++i)
        {
            const RenderGraphPassDesc& pass = m_Passes[i];
            std::unordered_set<size_t> addedEdges; // 同じ書き手への多重エッジを避ける

            auto addDependencyOn = [&](size_t writerIndex)
            {
                if (writerIndex != i && addedEdges.insert(writerIndex).second)
                {
                    dependents[writerIndex].push_back(i);
                    ++inDegree[i];
                }
            };

            // Read-after-Write: このパスが読むテクスチャの直近の書き手より後に実行する
            for (const RHI::IRHITexture* texture : pass.Reads)
            {
                if (texture == nullptr)
                {
                    continue;
                }
                const auto it = lastWriter.find(texture);
                if (it != lastWriter.end())
                {
                    addDependencyOn(it->second);
                }
            }

            // Write-after-Write: 同じテクスチャへ既に書いたパスがあればその後に実行する
            auto recordWrite = [&](const RHI::IRHITexture* texture)
            {
                if (texture == nullptr)
                {
                    return;
                }
                const auto it = lastWriter.find(texture);
                if (it != lastWriter.end())
                {
                    addDependencyOn(it->second);
                }
                lastWriter[texture] = i;
            };
            for (const RHI::IRHITexture* texture : pass.RenderTargets)
            {
                recordWrite(texture);
            }
            recordWrite(pass.DepthTarget);
            for (const RHI::IRHITexture* texture : pass.Writes)
            {
                recordWrite(texture);
            }
        }

        // Kahn法によるトポロジカルソート。準備完了パスの中では登録順(インデックス最小)を優先し、
        // 依存関係が同点の場合はAddPassした順番のまま実行されるようにする
        std::priority_queue<size_t, std::vector<size_t>, std::greater<>> ready;
        for (size_t i = 0; i < passCount; ++i)
        {
            if (inDegree[i] == 0)
            {
                ready.push(i);
            }
        }

        std::vector<size_t> order;
        order.reserve(passCount);
        while (!ready.empty())
        {
            const size_t current = ready.top();
            ready.pop();
            order.push_back(current);

            for (const size_t next : dependents[current])
            {
                if (--inDegree[next] == 0)
                {
                    ready.push(next);
                }
            }
        }

        if (order.size() != passCount)
        {
            throw std::runtime_error("RenderGraph: パスの依存関係に循環があります");
        }

        return order;
    }

    void RenderGraph::Execute()
    {
        const std::vector<size_t> order = ResolveExecutionOrder();

        for (const size_t index : order)
        {
            const RenderGraphPassDesc& pass = m_Passes[index];

            // スワップチェインへのバインドはvsync有効時にバックバッファ確保待ちでブロックしうるため、
            // その待ち時間をこのパスの計測スコープに含めないよう、BeginScopeより前にバインドする
            if (pass.SwapChainTarget != nullptr)
            {
                m_CommandList->SetRenderTarget(pass.SwapChainTarget);
            }

            m_GPUProfiler->BeginScope(pass.Name);
            m_CPUProfiler->BeginScope(pass.Name);

            if (pass.SwapChainTarget == nullptr)
            {
                RHI::IRHITexture* const* targets = pass.RenderTargets.empty() ? nullptr : pass.RenderTargets.data();
                m_CommandList->SetRenderTargets(
                    targets, static_cast<uint32_t>(pass.RenderTargets.size()), pass.DepthTarget, pass.DepthTargetArraySlice);
            }

            pass.Execute(m_CommandList);

            m_CPUProfiler->EndScope();
            m_GPUProfiler->EndScope();
        }

        m_Passes.clear();
    }
}
