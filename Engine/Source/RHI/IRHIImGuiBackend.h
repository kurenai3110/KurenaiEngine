#pragma once

namespace Kurenai::RHI
{
    // ImGui連携。ImGuiはバックエンド(DX11/DX12)ごとに専用の実装が必要なため、
    // IRHIDevice::CreateImGuiBackend()でバックエンド固有の実装を生成し、このインタフェース越しに利用する。
    // 生成(コンストラクタ)でImGuiの初期化、破棄(デストラクタ)で終了処理を行う
    class IRHIImGuiBackend
    {
    public:
        virtual ~IRHIImGuiBackend() = default;

        virtual void NewFrame() = 0;
        virtual void Render() = 0;
    };
}
