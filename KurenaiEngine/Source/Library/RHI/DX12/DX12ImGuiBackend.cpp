#include "DX12ImGuiBackend.h"

#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_win32.h>
#include <imgui.h>

#include <stdexcept>

#include "DX12DescriptorHeap.h"
#include "DX12Device.h"

namespace Kurenai::RHI
{
    namespace
    {
        // ImGui用シェーダ可視SRVヒープのフリーリスト割当コールバック。ImGui_ImplDX12_InitInfo::UserDataに
        // DX12DescriptorHeap*を渡しておき、ここでキャストして使う
        void ImGuiSrvAlloc(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* outCpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE* outGpuHandle)
        {
            auto* heap = static_cast<DX12DescriptorHeap*>(info->UserData);
            const uint32_t index = heap->Allocate();
            *outCpuHandle = heap->GetCpuHandle(index);
            *outGpuHandle = heap->GetGpuHandle(index);
        }

        void ImGuiSrvFree(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle)
        {
            (void)gpuHandle;
            auto* heap = static_cast<DX12DescriptorHeap*>(info->UserData);
            const D3D12_CPU_DESCRIPTOR_HANDLE base = heap->GetCpuHandle(0);
            const uint32_t index = static_cast<uint32_t>((cpuHandle.ptr - base.ptr) / heap->GetDescriptorSize());
            heap->Free(index);
        }
    }

    DX12ImGuiBackend::DX12ImGuiBackend(DX12Device* device, void* windowHandle)
        : m_Device(device)
    {
        m_SrvHeap = std::make_unique<DX12DescriptorHeap>(device->GetDevice(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 64, true);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();

        ImGui_ImplDX12_InitInfo initInfo{};
        initInfo.Device = device->GetDevice();
        initInfo.CommandQueue = device->GetCommandQueue();
        initInfo.NumFramesInFlight = 2;
        initInfo.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        initInfo.DSVFormat = DXGI_FORMAT_UNKNOWN;
        initInfo.SrvDescriptorHeap = m_SrvHeap->GetHeap();
        initInfo.UserData = m_SrvHeap.get();
        initInfo.SrvDescriptorAllocFn = &ImGuiSrvAlloc;
        initInfo.SrvDescriptorFreeFn = &ImGuiSrvFree;

        if (!ImGui_ImplWin32_Init(windowHandle) || !ImGui_ImplDX12_Init(&initInfo))
        {
            ImGui::DestroyContext();
            throw std::runtime_error("ImGuiの初期化に失敗しました");
        }
    }

    DX12ImGuiBackend::~DX12ImGuiBackend()
    {
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }

    void DX12ImGuiBackend::NewFrame()
    {
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // ImGui_ImplDX12_NewFrame()はテクスチャ管理のため内部でSetDescriptorHeapsを呼び、
        // シェーダ可視ヒープの割り当てをImGui自身のヒープへ切り替えてしまう。以降の描画が
        // SetTexture/SetSamplerSetで使うヒープを正しく参照できるよう、ここで明示的に戻す
        ID3D12DescriptorHeap* heaps[] = { m_Device->GetShaderVisibleSrvHeap()->GetHeap(), m_Device->GetShaderVisibleSamplerHeap()->GetHeap() };
        m_Device->GetCommandList()->SetDescriptorHeaps(2, heaps);
    }

    void DX12ImGuiBackend::Render()
    {
        ImGui::Render();
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), m_Device->GetCommandList());
    }
}
