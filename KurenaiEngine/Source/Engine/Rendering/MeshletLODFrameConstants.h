#pragma once

#include <cstdint>

#include <DirectXMath.h>

// メッシュレットLODの段を配る値。**Rendering/RenderFrameContext.h と
// Rendering/ObjectConstants.h の両方が要る**が、後者は KurenaiEngine3D.h を
// インクルードするため、同じヘッダには置けない。だからここに独立させてある。

namespace Kurenai
{
    // メッシュレットLODの段を選ぶために、フレーム内の全パスへ配る値(Stage 6)。
    //
    // 【主カメラの値である】シャドウと深度プリパスは G-Buffer とまったく同じ増幅シェーダーを
    // 使うが、そちらのViewProjは光源やカスケードのものに差し替わっている。各パスのカメラで
    // 段を選ぶと、影を落とす形と本体の形が違う段になり、影の縁が本体からずれる。
    // 段の選択は主カメラだけで決め、全パスで同じ値を配る
    struct MeshletLODFrameConstants
    {
        DirectX::XMFLOAT3 CameraPos{ 0.0f, 0.0f, 0.0f };
        // 距離1メートルにある長さ1メートルが何ピクセルになるか
        // (= 射影行列の縦方向の拡大率 × レンダーターゲットの高さ / 2)
        float PixelScale = 0.0f;
        // しきい値の倍率。段を落とす投影直径は
        // Quality * sqrt(4 * モデルのLOD0三角形数 / π) [画素]。
        // 0以下なら段の選択を行わない(A/B比較のOFF側)
        float Quality = 0.0f;
        // 0以上ならその段に固定する(対照実験用)。負なら自動
        int32_t Forced = -1;
        // メッシュレットの色分け表示を「塊ごと」ではなく「段ごと」にするか
        bool DebugColorByLOD = false;
    };
}
