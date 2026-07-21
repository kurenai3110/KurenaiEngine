#pragma once

#include <DirectXMath.h>

namespace Kurenai::Core
{
    class Camera
    {
    public:
        void SetPosition(const DirectX::XMFLOAT3& position) { m_Position = position; }
        const DirectX::XMFLOAT3& GetPosition() const { return m_Position; }

        void SetYawPitch(float yaw, float pitch);
        void Move(const DirectX::XMFLOAT3& delta);
        void Rotate(float deltaYaw, float deltaPitch);

        void SetAspectRatio(float aspect) { m_Aspect = aspect; }
        void SetLens(float fovYRadians, float nearZ, float farZ);

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
    };
}
