#pragma once

// KurenaiEngine.dllのエクスポート境界。KurenaiEngine本体をビルドするときのみ
// KURENAI_ENGINE_EXPORTSが定義される(KurenaiEngine.vcxproj参照)。
// 公開クラス(KurenaiEngineBase/KurenaiEngine3D/KurenaiEngine2D)のみがこのマクロを持ち、
// Library/配下の内部実装クラスはDLLからエクスポートされない
#if defined(KURENAI_ENGINE_EXPORTS)
    #define KURENAI_API __declspec(dllexport)
#else
    #define KURENAI_API __declspec(dllimport)
#endif

namespace Kurenai
{
    // 使用するグラフィックスAPIバックエンドの選択(サンプルプログラム向け公開API)
    enum class GraphicsAPI
    {
        DX11,
        DX12,
    };
}
