#pragma once

#include <Windows.h>

#include <cstdint>
#include <functional>
#include <string>

namespace Kurenai::Core
{
    class Window
    {
    public:
        using ResizeCallback = std::function<void(uint32_t, uint32_t)>;

        Window(const std::wstring& title, uint32_t width, uint32_t height);
        ~Window();

        Window(const Window&) = delete;
        Window& operator=(const Window&) = delete;

        void PumpMessages();
        bool ShouldClose() const { return m_ShouldClose; }

        HWND GetHandle() const { return m_Handle; }
        uint32_t GetWidth() const { return m_Width; }
        uint32_t GetHeight() const { return m_Height; }

        void SetResizeCallback(ResizeCallback callback) { m_ResizeCallback = std::move(callback); }

    private:
        static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
        LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

        HWND m_Handle = nullptr;
        uint32_t m_Width;
        uint32_t m_Height;
        bool m_ShouldClose = false;
        ResizeCallback m_ResizeCallback;
    };
}
