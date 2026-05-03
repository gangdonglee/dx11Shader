#pragma once

#include <windows.h>
#include <DirectXMath.h>
#include <cmath>

static const float D3DX_PI = 3.14159265358979323846f;

#ifndef D3DCOLOR_ARGB
#define D3DCOLOR_ARGB(a, r, g, b) \
    ((DWORD)((((a) & 0xff) << 24) | (((r) & 0xff) << 16) | (((g) & 0xff) << 8) | ((b) & 0xff)))
#endif

struct D3DXVECTOR3
{
    float x, y, z;

    D3DXVECTOR3() : x(0), y(0), z(0) {}
    D3DXVECTOR3(float xx, float yy, float zz) : x(xx), y(yy), z(zz) {}

    D3DXVECTOR3 operator+(const D3DXVECTOR3& rhs) const { return D3DXVECTOR3(x + rhs.x, y + rhs.y, z + rhs.z); }
    D3DXVECTOR3 operator-(const D3DXVECTOR3& rhs) const { return D3DXVECTOR3(x - rhs.x, y - rhs.y, z - rhs.z); }
    D3DXVECTOR3 operator*(float s) const { return D3DXVECTOR3(x * s, y * s, z * s); }
    D3DXVECTOR3 operator/(float s) const { return D3DXVECTOR3(x / s, y / s, z / s); }
    D3DXVECTOR3& operator+=(const D3DXVECTOR3& rhs) { x += rhs.x; y += rhs.y; z += rhs.z; return *this; }
    D3DXVECTOR3& operator-=(const D3DXVECTOR3& rhs) { x -= rhs.x; y -= rhs.y; z -= rhs.z; return *this; }
};

inline D3DXVECTOR3 operator*(float s, const D3DXVECTOR3& v) { return v * s; }

struct D3DXVECTOR4
{
    float x, y, z, w;
    D3DXVECTOR4() : x(0), y(0), z(0), w(0) {}
    D3DXVECTOR4(float xx, float yy, float zz, float ww) : x(xx), y(yy), z(zz), w(ww) {}
};

struct D3DXMATRIX
{
    float m[4][4];
};

inline DirectX::XMVECTOR XMFromVec3(const D3DXVECTOR3& v)
{
    return DirectX::XMVectorSet(v.x, v.y, v.z, 1.0f);
}

inline void D3DXMatrixIdentity(D3DXMATRIX* out)
{
    DirectX::XMStoreFloat4x4(reinterpret_cast<DirectX::XMFLOAT4X4*>(out), DirectX::XMMatrixIdentity());
}

inline void D3DXMatrixRotationY(D3DXMATRIX* out, float angle)
{
    DirectX::XMStoreFloat4x4(reinterpret_cast<DirectX::XMFLOAT4X4*>(out), DirectX::XMMatrixRotationY(angle));
}

inline void D3DXMatrixTranslation(D3DXMATRIX* out, float x, float y, float z)
{
    DirectX::XMStoreFloat4x4(reinterpret_cast<DirectX::XMFLOAT4X4*>(out), DirectX::XMMatrixTranslation(x, y, z));
}

inline void D3DXMatrixScaling(D3DXMATRIX* out, float x, float y, float z)
{
    DirectX::XMStoreFloat4x4(reinterpret_cast<DirectX::XMFLOAT4X4*>(out), DirectX::XMMatrixScaling(x, y, z));
}

inline void D3DXMatrixLookAtLH(D3DXMATRIX* out, const D3DXVECTOR3* eye, const D3DXVECTOR3* at, const D3DXVECTOR3* up)
{
    DirectX::XMStoreFloat4x4(reinterpret_cast<DirectX::XMFLOAT4X4*>(out),
        DirectX::XMMatrixLookAtLH(XMFromVec3(*eye), XMFromVec3(*at), XMFromVec3(*up)));
}

inline void D3DXMatrixPerspectiveFovLH(D3DXMATRIX* out, float fovY, float aspect, float zn, float zf)
{
    DirectX::XMStoreFloat4x4(reinterpret_cast<DirectX::XMFLOAT4X4*>(out),
        DirectX::XMMatrixPerspectiveFovLH(fovY, aspect, zn, zf));
}

inline void D3DXMatrixOrthoLH(D3DXMATRIX* out, float w, float h, float zn, float zf)
{
    DirectX::XMStoreFloat4x4(reinterpret_cast<DirectX::XMFLOAT4X4*>(out),
        DirectX::XMMatrixOrthographicLH(w, h, zn, zf));
}

inline D3DXMATRIX operator*(const D3DXMATRIX& a, const D3DXMATRIX& b)
{
    DirectX::XMMATRIX ma = DirectX::XMLoadFloat4x4(reinterpret_cast<const DirectX::XMFLOAT4X4*>(&a));
    DirectX::XMMATRIX mb = DirectX::XMLoadFloat4x4(reinterpret_cast<const DirectX::XMFLOAT4X4*>(&b));
    D3DXMATRIX out;
    DirectX::XMStoreFloat4x4(reinterpret_cast<DirectX::XMFLOAT4X4*>(&out), DirectX::XMMatrixMultiply(ma, mb));
    return out;
}

inline float D3DXVec3Length(const D3DXVECTOR3* v)
{
    return std::sqrt(v->x * v->x + v->y * v->y + v->z * v->z);
}

inline void D3DXVec3Normalize(D3DXVECTOR3* out, const D3DXVECTOR3* v)
{
    float len = D3DXVec3Length(v);
    if (len <= 1e-6f)
    {
        *out = D3DXVECTOR3(0, 0, 0);
        return;
    }
    *out = *v / len;
}

inline bool ProjectPoint(const D3DXVECTOR3& world, const D3DXMATRIX& view, const D3DXMATRIX& proj,
                         int width, int height, D3DXVECTOR3* out)
{
    DirectX::XMMATRIX v = DirectX::XMLoadFloat4x4(reinterpret_cast<const DirectX::XMFLOAT4X4*>(&view));
    DirectX::XMMATRIX p = DirectX::XMLoadFloat4x4(reinterpret_cast<const DirectX::XMFLOAT4X4*>(&proj));
    DirectX::XMVECTOR pt = DirectX::XMVector3Project(XMFromVec3(world), 0.0f, 0.0f,
        (float)width, (float)height, 0.0f, 1.0f, p, v, DirectX::XMMatrixIdentity());
    DirectX::XMFLOAT3 f;
    DirectX::XMStoreFloat3(&f, pt);
    *out = D3DXVECTOR3(f.x, f.y, f.z);
    return f.z >= 0.0f && f.z <= 1.0f;
}

inline void UnprojectPoint(const D3DXVECTOR3& screen, const D3DXMATRIX& view, const D3DXMATRIX& proj,
                           int width, int height, D3DXVECTOR3* out)
{
    DirectX::XMMATRIX v = DirectX::XMLoadFloat4x4(reinterpret_cast<const DirectX::XMFLOAT4X4*>(&view));
    DirectX::XMMATRIX p = DirectX::XMLoadFloat4x4(reinterpret_cast<const DirectX::XMFLOAT4X4*>(&proj));
    DirectX::XMVECTOR pt = DirectX::XMVector3Unproject(XMFromVec3(screen), 0.0f, 0.0f,
        (float)width, (float)height, 0.0f, 1.0f, p, v, DirectX::XMMatrixIdentity());
    DirectX::XMFLOAT3 f;
    DirectX::XMStoreFloat3(&f, pt);
    *out = D3DXVECTOR3(f.x, f.y, f.z);
}
