#pragma once

#include <cstdint>

#include "EnvironmentConstants.h"
#include "../Rendering/CubeFaceMath.h"

namespace Kurenai::Passes
{
        // キューブマップ配列の枚数上限。TextureCubeArrayは実行時に伸縮できないため固定容量で
        // 確保し、これを超えるプローブが置かれたシーンは先頭からこの数だけを採用する(警告ログを出す)
        inline constexpr uint32_t kMaxReflectionProbes = 8;

    inline constexpr uint32_t kProbeCaptureSize = kIBLPrefilterBaseSize;

    // プリフィルタ畳み込み(6ミップ×6面=36ディスパッチ)を1フレームへ集中させず、
    // kProbeRealtimePrefilterStepsPerFrameずつ複数フレームへ分ける(集中させると
    // 「6フレームに1回のスパイク」になる)。
    // kProbePrefilterStepCount(36)が「プリフィルタ中でない」を表す番兵値
    inline constexpr uint32_t kProbePrefilterStepCount = kIBLPrefilterMipLevels * kCubeFaceCount;
    // 1フレームに進めるステップ数。ステップ番号は「面を外側・ミップを内側」で(face, mip)へ
    // 割り当てるため(KurenaiEngine3D.cppのRealtimeプリフィルタフェーズのコメント参照)、
    // ここを kIBLPrefilterMipLevels と一致させると
    // 「1フレーム = 1面ぶんのミップチェーン全部」となり6フレームすべてが厳密に同じ量になる。
    // 一致させないとフレームごとにミップ0の面の数が0個/1個/2個とばらつき、
    // ミップ0が畳み込み全体の75%を占めるためそのままスパイクの高さのばらつきになる。
    // capture フェーズ(6面=6フレーム)ともデューティ比が対称になる
    inline constexpr uint32_t kProbeRealtimePrefilterStepsPerFrame = kIBLPrefilterMipLevels;
}
