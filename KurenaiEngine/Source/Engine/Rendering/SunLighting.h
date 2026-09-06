#pragma once

#include <DirectXMath.h>

// 太陽・月・空の状態(段階6)。
// ComputeSunLighting が組み立て、Render() と各パス群が読む。
// 純粋なデータなので、KurenaiEngine3D.cpp の無名名前空間から出してここへ置いてある。
//
// 【namespace Kurenai に直接置く理由】KurenaiEngine3D.cpp(namespace Kurenai)と
// Passes/*.cpp(namespace Kurenai::Passes)のどちらからも修飾なしで引ける位置にある。
namespace Kurenai
{
    // 太陽光の向き・色・環境光を時刻(0〜24時)から計算する
    struct SunLighting
    {
        // 支配ライト(太陽 or 月)の、光が進む向き(サーフェスに当たる方向)。
        // カスケードシャドウの行列もこの向きから作ること
        DirectX::XMFLOAT3 Direction;
        DirectX::XMFLOAT4 Color;
        DirectX::XMFLOAT4 Ambient; // rgb=環境光の色, a=昼度(0=夜,1=昼)
        // 支配ライトが太陽か月か(ImGuiの表示とデバッグ用)
        bool DominantIsSun;
        // 手続き空の天頂輝度を正規化する際の目標照度[lx]。薄明係数と月明かりで変調済み
        float SkyIlluminanceLux;
        // 薄明係数(仰角[-15°,+15°] = 時刻でちょうど5-7時/17-19時)
        float TwilightFactor;
        // 太陽が「ある」向き。手続き空(SkyGenerate.hlsl)がPerez分布のcircumsolar項の
        // 基準に使う。月が支配的なときも**常に太陽の位置**であることに注意
        DirectX::XMFLOAT3 SunPosition;
        // このフレームのキーとなる照度[lx]。可変プリ露出の基準になる
        float KeyIlluminanceLux;
    };
}
