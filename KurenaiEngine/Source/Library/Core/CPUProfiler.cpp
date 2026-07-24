#include "CPUProfiler.h"

#include <algorithm>

namespace Kurenai::Core
{
    void CPUProfiler::BeginFrame()
    {
        m_Results.clear();
        m_ScopeActive = false;
    }

    void CPUProfiler::BeginScope(const std::string& name)
    {
        m_CurrentScopeName = name;
        m_ScopeStart = std::chrono::steady_clock::now();
        m_ScopeActive = true;
    }

    void CPUProfiler::EndScope()
    {
        if (!m_ScopeActive)
        {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        const float timeMs = std::chrono::duration<float, std::milli>(now - m_ScopeStart).count();
        m_Results.push_back({ m_CurrentScopeName, timeMs });
        m_ScopeActive = false;
    }

    void CPUProfiler::SubtractFromScope(const std::string& name, float deltaMs)
    {
        const auto it = std::find_if(m_Results.begin(), m_Results.end(), [&name](const CPUTimingResult& result) { return result.Name == name; });
        if (it == m_Results.end())
        {
            return;
        }

        it->TimeMs = std::max(0.0f, it->TimeMs - deltaMs);
    }
}
