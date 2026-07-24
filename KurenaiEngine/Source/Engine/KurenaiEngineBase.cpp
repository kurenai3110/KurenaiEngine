#include "KurenaiEngineBase.h"

#include <Windows.h>

namespace Kurenai
{
    namespace
    {
        RHI::GraphicsAPI ToRHI(GraphicsAPI api)
        {
            return api == GraphicsAPI::DX12 ? RHI::GraphicsAPI::DX12 : RHI::GraphicsAPI::DX11;
        }
    }

    KurenaiEngineBase::KurenaiEngineBase(const std::wstring& title, uint32_t width, uint32_t height, GraphicsAPI api)
    {
        m_Window = std::make_unique<Core::Window>(title, width, height);
        m_Window->SetResizeCallback(
            [this](uint32_t newWidth, uint32_t newHeight)
            {
                if (m_SwapChain)
                {
                    m_SwapChain->Resize(newWidth, newHeight);
                }
            });

        m_Device = ToRHI(api) == RHI::GraphicsAPI::DX12 ? RHI::CreateDX12Device() : RHI::CreateDX11Device();
        m_SwapChain = m_Device->CreateSwapChain(m_Window->GetHandle(), m_Window->GetWidth(), m_Window->GetHeight());

        m_AudioEngine = std::make_unique<Core::AudioEngine>();
    }

    KurenaiEngineBase::~KurenaiEngineBase() = default;

    bool KurenaiEngineBase::ShouldClose() const
    {
        return m_Window->ShouldClose();
    }

    void KurenaiEngineBase::PumpEvents()
    {
        m_Window->PumpMessages();
    }

    void KurenaiEngineBase::Close()
    {
        PostMessageW(m_Window->GetHandle(), WM_CLOSE, 0, 0);
    }

    uint32_t KurenaiEngineBase::GetWidth() const
    {
        return m_Window->GetWidth();
    }

    uint32_t KurenaiEngineBase::GetHeight() const
    {
        return m_Window->GetHeight();
    }

    HWND KurenaiEngineBase::GetWindowHandle() const
    {
        return m_Window->GetHandle();
    }

    bool KurenaiEngineBase::IsMouseOverWindow() const
    {
        return m_Window->IsMouseOverWindow();
    }

    bool KurenaiEngineBase::IsMouseButtonDown(MouseButton button) const
    {
        return m_Window->IsMouseButtonDown(button);
    }

    bool KurenaiEngineBase::IsKeyDown(KeyCode key) const
    {
        return m_Window->IsKeyDown(key);
    }

    bool KurenaiEngineBase::WasMouseButtonPressed(MouseButton button) const
    {
        return m_Window->WasMouseButtonPressed(button);
    }

    bool KurenaiEngineBase::WasKeyPressed(KeyCode key) const
    {
        return m_Window->WasKeyPressed(key);
    }

    POINT KurenaiEngineBase::GetClientMousePosition() const
    {
        return m_Window->GetClientMousePosition();
    }

    SoundHandle KurenaiEngineBase::LoadSound(const std::wstring& filePath)
    {
        return SoundHandle(m_AudioEngine->LoadSound(filePath));
    }

    VoiceHandle KurenaiEngineBase::PlaySound(SoundHandle sound, float volume, bool loop)
    {
        if (!sound.IsValid())
        {
            return VoiceHandle();
        }
        return VoiceHandle(m_AudioEngine->PlaySound(sound.m_Index, volume, loop));
    }

    void KurenaiEngineBase::StopSound(VoiceHandle voice)
    {
        if (!voice.IsValid())
        {
            return;
        }
        m_AudioEngine->StopSound(voice.m_Id);
    }

    RHI::IRHICommandList* KurenaiEngineBase::GetCommandList() const
    {
        return m_Device->GetImmediateCommandList();
    }
}
