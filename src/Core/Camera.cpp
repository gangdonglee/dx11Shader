#include "Camera.h"
#include "KeyManager.h"

Camera::Camera()
    : m_target(0.0f, 0.0f, 0.0f),
      m_distance(12.0f),
      m_yaw(D3DX_PI * 0.25f),
      m_pitch(D3DX_PI * 0.20f),
      m_aspect(16.0f / 9.0f),
      m_fovY(D3DX_PI / 3.0f),
      m_nearZ(0.1f),
      m_farZ(500.0f),
      m_rotateSpeed(0.005f),
      m_panSpeed(0.012f),
      m_zoomSpeed(0.0015f),
      m_moveSpeed(8.0f)
{
}

void Camera::SetDistance(float d)
{
    m_distance = (d < 0.5f) ? 0.5f : d;
}

void Camera::OnMouseRotate(int dx, int dy)
{
    m_yaw   -= dx * m_rotateSpeed;
    m_pitch -= dy * m_rotateSpeed;
    const float lim = D3DX_PI * 0.49f;
    if (m_pitch >  lim) m_pitch =  lim;
    if (m_pitch < -lim) m_pitch = -lim;
}

void Camera::OnMousePan(int dx, int dy)
{
    float cy = std::cos(m_yaw), sy = std::sin(m_yaw);
    float cp = std::cos(m_pitch);

    D3DXVECTOR3 right(cy, 0.0f, -sy);
    D3DXVECTOR3 up   (0.0f, 1.0f, 0.0f);

    float distScale = (m_distance < 1.0f) ? 1.0f : m_distance;
    float scale = m_panSpeed * distScale;
    (void)cp;
    m_target -= right * (dx * scale);
    m_target += up    * (dy * scale);
}

void Camera::OnMouseWheel(int wheelDelta)
{
    float factor = std::pow(0.9f, wheelDelta / 120.0f);
    SetDistance(m_distance * factor);
}

void Camera::Update(float dt, KeyManager* keys)
{
    if (!keys) return;

    float cy = std::cos(m_yaw), sy = std::sin(m_yaw);
    D3DXVECTOR3 fwd  (sy, 0.0f, cy);   // ground plane forward (no pitch)
    D3DXVECTOR3 right(cy, 0.0f, -sy);

    float step = m_moveSpeed * dt;
    if (keys->IsDown('W')) m_target += fwd   * step;
    if (keys->IsDown('S')) m_target -= fwd   * step;
    if (keys->IsDown('D')) m_target += right * step;
    if (keys->IsDown('A')) m_target -= right * step;
    if (keys->IsDown('E')) m_target.y += step;
    if (keys->IsDown('Q')) m_target.y -= step;
}

D3DXVECTOR3 Camera::GetEyePos() const
{
    float cp = std::cos(m_pitch), sp = std::sin(m_pitch);
    float cy = std::cos(m_yaw),   sy = std::sin(m_yaw);
    D3DXVECTOR3 offset(cp * sy, sp, cp * cy);
    return m_target + offset * m_distance;
}

void Camera::GetView(D3DXMATRIX* out) const
{
    D3DXVECTOR3 eye = GetEyePos();
    D3DXVECTOR3 up(0.0f, 1.0f, 0.0f);
    D3DXMatrixLookAtLH(out, &eye, &m_target, &up);
}

void Camera::GetProj(D3DXMATRIX* out) const
{
    D3DXMatrixPerspectiveFovLH(out, m_fovY, m_aspect, m_nearZ, m_farZ);
}
