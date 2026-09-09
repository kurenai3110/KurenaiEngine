#pragma once

#include <cstdint>

// インスタンスごとのモデルLODの段と、段の切り替えのフェード状態。
//
// 【エンジンの入れ子型にしない】UIパネルがLOD段ごとの内訳を出すために読む。
// 入れ子のままだとエンジンの公開ヘッダを引かないと型名すら書けない。
namespace Kurenai::Scene
{
    struct InstanceLODState
    {
        uint32_t CurrentLOD = 0;   // 0 = ModelInstance::Model、1以上は LODModels[n-1]
        uint32_t PreviousLOD = 0;  // フェード中の切り替え元
        float FadeT = 1.0f;        // 1.0でフェード完了。0→1へ進み、その間だけ2段を重ねる
    };
}
