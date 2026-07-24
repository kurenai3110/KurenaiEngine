#include "DX11ImGuiBackend.h"

#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>
#include <imgui.h>

#include <stdexcept>

namespace Kurenai::RHI
{
    DX11ImGuiBackend::DX11ImGuiBackend(ID3D11Device* device, ID3D11DeviceContext* context, void* windowHandle)
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();

        if (!ImGui_ImplWin32_Init(windowHandle) || !ImGui_ImplDX11_Init(device, context))
        {
            ImGui::DestroyContext();
            throw std::runtime_error("ImGuiの初期化に失敗しました");
        }
    }

    DX11ImGuiBackend::~DX11ImGuiBackend()
    {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }

    void DX11ImGuiBackend::NewFrame()
    {
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
    }

    void DX11ImGuiBackend::Render()
    {
        ImGui::Render();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }
}
