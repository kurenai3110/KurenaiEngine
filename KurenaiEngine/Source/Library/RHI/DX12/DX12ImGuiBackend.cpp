#include "DX12ImGuiBackend.h"

#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_win32.h>
#include <imgui.h>

#include <stdexcept>

#include "DX12DescriptorHeap.h"
#include "DX12Device.h"
#include "RHI/ImGuiContextSetup.h"

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
        m_SrvHeap->GetHeap()->SetName(L"ImGui Shader Visible SRV Heap");

        // コンテキスト生成と共通設定(ドッキング有効化等)はDX11側と共通のためImGuiContextSetupへ集約した。
        // スタイル・フォントはRHIの責務ではないためここでは設定せず、KurenaiEngine3D側が行う
        Detail::CreateImGuiContextCommon();

        ImGui_ImplDX12_InitInfo initInfo{};
        initInfo.Device = device->GetDevice();
        initInfo.CommandQueue = device->GetCommandQueue();
        initInfo.NumFramesInFlight = 2;
        initInfo.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        // ImGuiはスワップチェインのレンダーターゲット(DSVがバインドされた状態)へ描くため、
        // 実際にバインドされるDSVと同じフォーマットを申告する必要がある(UNKNOWNのままだと
        // D3D12デバッグレイヤーがRT/DSVとPSOのフォーマット不一致としてエラーを出す)
        initInfo.DSVFormat = DXGI_FORMAT_D32_FLOAT;
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

        // ImGuiのフォントテクスチャ生成などでヒープの割り当て状態が変わっても、以降の描画が
        // SetTexture/SetSamplerSetで使うヒープを正しく参照できるよう、ここで明示的に戻しておく
        BindEngineDescriptorHeaps();
    }

    void DX12ImGuiBackend::Render()
    {
        ImGui::Render();

        // ImGui_ImplDX12_RenderDrawDataはSetGraphicsRootDescriptorTableへImGui自身のヒープ上の
        // ハンドルを渡すが、SetDescriptorHeapsは呼ばない(呼び出し側が事前にバインドしておく規約)。
        // エンジンのヒープを張ったまま呼ぶと「ハンドルの所属ヒープと現在バインド中のヒープが違う」
        // というD3D12の仕様違反になるため、ImGuiのヒープへ切り替えてから描画する
        ID3D12DescriptorHeap* imguiHeaps[] = { m_SrvHeap->GetHeap() };
        m_Device->GetCommandList()->SetDescriptorHeaps(1, imguiHeaps);

        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), m_Device->GetCommandList());

        // 同じフレーム内でこの後に描画が続く場合に備えて元へ戻す
        BindEngineDescriptorHeaps();
    }

    void DX12ImGuiBackend::BindEngineDescriptorHeaps()
    {
        ID3D12DescriptorHeap* heaps[] = { m_Device->GetShaderVisibleSrvHeap()->GetHeap(), m_Device->GetShaderVisibleSamplerHeap()->GetHeap() };
        m_Device->GetCommandList()->SetDescriptorHeaps(2, heaps);
    }
}
