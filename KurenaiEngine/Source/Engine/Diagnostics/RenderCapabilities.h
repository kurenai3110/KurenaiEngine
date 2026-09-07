#pragma once

namespace Kurenai
{
    // 起動時のデバイス初期化で決まり、以後のフレームでは変わらない能力値。
    // UIの選択可否の判定と、対応シェーダー/パイプラインステートを作るかどうかの両方に使う
    struct RenderCapabilities
    {
        // このデバイスがメッシュシェーダーを使えるか(IRHIDevice::SupportsMeshShader()の写し)。
        // m_DeviceはKurenaiEngineBaseのprotectedメンバで、派生クラスのfriendであるUIパネルから
        // 触れるかはC++の規則の解釈が分かれるため、RaytracingAvailableと同じくここへ控える
        bool MeshShaderAvailable = false;

        // レイトレーシング反射が使える環境か。デバイスのSupportsRaytracing()を初期化時に控えたもので、
        // UIの選択可否とシェーダー/パイプラインステートを作るかどうかの両方に使う
        // (RTReflection.hlslはRayQueryを含むためSM 6.5でしかコンパイルできず、
        //  非対応環境で作ろうとすると例外になる)
        bool RaytracingAvailable = false;

        // DDGIのレイ取得をDXRで行えるか。RaytracingAvailableとは別に持つ。
        //
        // 【なぜ別なのか】DDGIProbeTrace.hlslはコンピュートシェーダーの中でテクスチャを
        // 微分付きにサンプルするため、DXILの検証がシェーダーモデル6.6を要求する
        // (Derivatives in CS/MS/AS is SM 6.6+)。RayQuery自体はSM 6.5で足りるので、
        // 「DXR Tier 1.1に対応していて、かつSM 6.5のシェーダーバリアントで動いている」環境が
        // 実在しうる ―― その場合、他のRTパスは作れるのにこれだけ作れない。
        // 作成に失敗したらここをfalseにして、DDGIのレイ取得だけをラスタ経路へ戻す
        bool DDGIRaytracedTraceAvailable = false;

        // デバイスが対応していて、かつシェーダー/リソースの作成に成功したか。
        // どちらかが欠けたらUIのチェックボックスごと無効化する
        bool SoftwareRasterAvailable = false;

        // 間接引数からメッシュシェーダーのディスパッチを発行できるか
        // (IRHIDevice::SupportsIndirectDispatchMesh()の写し)。DX11とメッシュシェーダー
        // 非対応環境では偽で、モデルカリングは従来のCPUループへ縮退する
        bool IndirectDispatchMeshAvailable = false;
    };
}
