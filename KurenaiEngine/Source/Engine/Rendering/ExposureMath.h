#pragma once

#include <cmath>

// 露出の変換。**パス群とライトの構築の両方が使う**ため、
// KurenaiEngine3D.cpp の無名名前空間から出してここへ置いてある(段階6)。
//
// 【namespace Kurenai に直接置く理由】呼び出し側は KurenaiEngine3D.cpp(namespace Kurenai)と
// Passes/*.cpp(namespace Kurenai::Passes)の両方で、どちらも修飾なしで引ける位置にある。
namespace Kurenai
{
    // 実在の写真露出値(EV100)から露出係数を求める。絞り値・シャッター速度・ISO感度から一意に
    // 定まる実在の量で、Lagarde & de Rousiers, "Moving Frostbite to Physically Based Rendering"
    // (SIGGRAPH 2014 course notes)やGoogle FilamentのPhysically Based Cameraドキュメントが
    // 採る標準式。カンデラ/ルクスの測光量に直接掛けることで表示レンジへ変換する
    // (放射量(W)への変換は行わない。本エンジンには放射量ベースの大気モデルが無く、
    // 変換段を増やす意味が無いため)
    inline float ComputeExposure(float ev100)
    {
        return 1.0f / (1.2f * std::pow(2.0f, ev100));
    }
}
