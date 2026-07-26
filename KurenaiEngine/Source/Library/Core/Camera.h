#pragma once

#include <DirectXMath.h>

#include "KurenaiTypes.h"

// dllexportされたクラスが非export型(DirectX::XMFLOAT3など)をメンバに持つことによる
// C4251警告を抑制する。KurenaiEngine.dllと各サンプルは常に同一コンパイラ・同一ランタイム
// ライブラリ設定でビルドされるため、実務上は問題にならない
#pragma warning(push)
#pragma warning(disable: 4251)

namespace Kurenai::Core
{
    class KURENAI_API Camera
    {
    public:
        void SetPosition(const DirectX::XMFLOAT3& position) { m_Position = position; }
        const DirectX::XMFLOAT3& GetPosition() const { return m_Position; }

        void SetYawPitch(float yaw, float pitch);
        void Move(const DirectX::XMFLOAT3& delta);
        void Rotate(float deltaYaw, float deltaPitch);

        void SetAspectRatio(float aspect) { m_Aspect = aspect; }
        void SetLens(float fovYRadians, float nearZ, float farZ);

        // カスケードシャドウマップの分割距離・視錐台コーナー計算に使う
        float GetFovY() const { return m_FovY; }
        float GetAspectRatio() const { return m_Aspect; }
        float GetNearZ() const { return m_NearZ; }
        float GetFarZ() const { return m_FarZ; }

        // 正射影(2D/UI向け)に切り替える。viewWidth/viewHeightはワールド単位で見える範囲
        // (例えばピクセルとワールド単位を1:1にしたい場合は画面の幅・高さをそのまま渡す)。
        // 呼び出すとGetProjectionMatrixは以後この正射影行列を返すようになる。
        // 遠近投影と異なりReverse-Zは使わない(近平面=0.0/遠平面=1.0の標準マッピング)ため、
        // このカメラを使うパイプラインではPipelineStateDesc::ReverseZをfalseにすること
        void SetOrthographic(float viewWidth, float viewHeight, float nearZ, float farZ);

        // SetLensを呼んで遠近投影に戻す(SetOrthographicとは排他)
        bool IsOrthographic() const { return m_Orthographic; }

        DirectX::XMFLOAT3 GetForward() const;
        DirectX::XMFLOAT3 GetRight() const;
        DirectX::XMMATRIX GetViewMatrix() const;
        DirectX::XMMATRIX GetProjectionMatrix() const;

    private:
        DirectX::XMFLOAT3 m_Position{ 0.0f, 0.0f, 0.0f };
        float m_Yaw = 0.0f;
        float m_Pitch = 0.0f;
        float m_FovY = DirectX::XM_PIDIV4;
        float m_Aspect = 16.0f / 9.0f;
        float m_NearZ = 0.1f;
        float m_FarZ = 1000.0f;

        bool m_Orthographic = false;
        float m_OrthoWidth = 1.0f;
        float m_OrthoHeight = 1.0f;
    };
}

#pragma warning(pop)
