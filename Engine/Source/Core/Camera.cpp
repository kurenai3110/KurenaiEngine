#include "Camera.h"

#include <algorithm>

using namespace DirectX;

namespace Kurenai::Core
{
    void Camera::SetYawPitch(float yaw, float pitch)
    {
        m_Yaw = yaw;
        m_Pitch = pitch;
    }

    void Camera::Move(const XMFLOAT3& delta)
    {
        m_Position.x += delta.x;
        m_Position.y += delta.y;
        m_Position.z += delta.z;
    }

    void Camera::Rotate(float deltaYaw, float deltaPitch)
    {
        m_Yaw += deltaYaw;
        m_Pitch += deltaPitch;

        const float pitchLimit = XM_PIDIV2 - 0.01f;
        m_Pitch = std::max(-pitchLimit, std::min(pitchLimit, m_Pitch));
    }

    void Camera::SetLens(float fovYRadians, float nearZ, float farZ)
    {
        m_FovY = fovYRadians;
        m_NearZ = nearZ;
        m_FarZ = farZ;
    }

    XMFLOAT3 Camera::GetForward() const
    {
        XMVECTOR forward = XMVectorSet(
            cosf(m_Pitch) * sinf(m_Yaw),
            sinf(m_Pitch),
            cosf(m_Pitch) * cosf(m_Yaw),
            0.0f);

        XMFLOAT3 result;
        XMStoreFloat3(&result, forward);
        return result;
    }

    XMFLOAT3 Camera::GetRight() const
    {
        XMFLOAT3 forward = GetForward();
        XMVECTOR forwardVec = XMLoadFloat3(&forward);
        XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        XMVECTOR right = XMVector3Normalize(XMVector3Cross(up, forwardVec));

        XMFLOAT3 result;
        XMStoreFloat3(&result, right);
        return result;
    }

    XMMATRIX Camera::GetViewMatrix() const
    {
        XMVECTOR eye = XMLoadFloat3(&m_Position);
        XMFLOAT3 forward = GetForward();
        XMVECTOR forwardVec = XMLoadFloat3(&forward);
        XMVECTOR target = XMVectorAdd(eye, forwardVec);
        XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        return XMMatrixLookAtLH(eye, target, up);
    }

    XMMATRIX Camera::GetProjectionMatrix() const
    {
        // Reverse-Z: 近平面をNDC z=1.0、遠平面をNDC z=0.0にマッピングする独自の透視投影行列。
        // 浮動小数点深度バッファ(D32_FLOAT)は0.0付近の表現密度が高いため、標準のXMMatrixPerspectiveFovLH
        // (近平面=0.0/遠平面=1.0)のままだと遠方の精度がほとんど残らずZファイティングが起きやすい。
        // マッピングを反転させることで、この高精度域を遠方に割り当てて分布を均す
        const float h = 1.0f / tanf(m_FovY * 0.5f);
        const float w = h / m_Aspect;
        const float n = m_NearZ;
        const float f = m_FarZ;
        const float a = n / (n - f);
        const float b = -a * f;

        return XMMatrixSet(
            w, 0.0f, 0.0f, 0.0f,
            0.0f, h, 0.0f, 0.0f,
            0.0f, 0.0f, a, 1.0f,
            0.0f, 0.0f, b, 0.0f);
    }
}
