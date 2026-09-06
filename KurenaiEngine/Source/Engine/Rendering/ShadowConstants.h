#pragma once

#include <cstdint>

namespace Kurenai::Rendering
{
    // カスケードシャドウマップの分割数。カメラ視錐台をこの数だけの深度範囲に分割し、
    // それぞれ専用のシャドウマップ・ライト正射影を持たせる。
    // FrameConstants::CascadeSplitsがXMFLOAT4(4要素)にfar距離を詰めているため、
    // この値を変える場合はKurenaiEngine3D.cppのCascadeSplits周りも合わせて変更が必要。
    // KurenaiEngine3D にも同名の別名があり、そちらがこの値を引く(移行中)
    inline constexpr uint32_t kCascadeCount = 4;

    // シャドウパス(平行光のライト視点から深度のみを描画する)。カメラ視錐台をkCascadeCount個の
    // 深度範囲に分割し(Practical Split Scheme)、それぞれ専用の正射影・シャドウマップを持たせる
    // カスケードシャドウマップ(CSM)。近いカスケードほどテクセル密度が高く、遠いカスケードほど
    // 広い範囲を粗くカバーする
    inline constexpr uint32_t kShadowMapSize = 2048;
}
