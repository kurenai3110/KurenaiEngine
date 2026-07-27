#pragma once

#include <memory>

#include "RHI/IRHIImGuiBackend.h"

namespace Kurenai::RHI
{
    class DX12Device;
    class DX12DescriptorHeap;

    class DX12ImGuiBackend : public IRHIImGuiBackend
    {
    public:
        DX12ImGuiBackend(DX12Device* device, void* windowHandle);
        ~DX12ImGuiBackend() override;

        void NewFrame() override;
        void Render() override;

    private:
        // エンジン本体が描画に使うシェーダ可視ヒープ(SRV+サンプラー)をコマンドリストへバインドする。
        // ImGuiの描画は自前のヒープを必要とするため、その前後で張り替えるのに使う
        void BindEngineDescriptorHeaps();

        DX12Device* m_Device = nullptr;
        // ImGuiが管理するフォント/テクスチャ用のシェーダ可視SRVヒープ。DX12Device本体が
        // 描画に使うヒープとは別に、ImGui専用として独立させておく
        std::unique_ptr<DX12DescriptorHeap> m_SrvHeap;
    };
}
