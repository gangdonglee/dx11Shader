#pragma once
#include "MathTypes.h"

class KeyManager;

// Orbit camera. LMB drag = rotate around target. MMB drag (or Shift+LMB)
// = pan target. Wheel = zoom (multiplicative). WASD = move target along
// camera-relative XZ. Pitch is clamped to avoid gimbal flip.
class Camera
{
public:
    Camera();

    void  SetAspect(float a)             { m_aspect = a; }
    void  SetTarget(float x, float y, float z) { m_target = D3DXVECTOR3(x, y, z); }
    void  SetDistance(float d);

    void  OnMouseRotate(int dx, int dy);
    void  OnMousePan(int dx, int dy);
    void  OnMouseWheel(int wheelDelta);
    void  Update(float dt, KeyManager* keys);

    void          GetView(D3DXMATRIX* out) const;
    void          GetProj(D3DXMATRIX* out) const;
    D3DXVECTOR3   GetEyePos() const;
    D3DXVECTOR3   GetTarget() const      { return m_target; }
    float         GetYaw() const         { return m_yaw; }
    float         GetPitch() const       { return m_pitch; }
    float         GetDistance() const    { return m_distance; }

private:
    D3DXVECTOR3 m_target;
    float m_distance;
    float m_yaw;     // around Y, radians
    float m_pitch;   // up/down, radians
    float m_aspect;
    float m_fovY;
    float m_nearZ;
    float m_farZ;
    float m_rotateSpeed;
    float m_panSpeed;
    float m_zoomSpeed;
    float m_moveSpeed;
};
