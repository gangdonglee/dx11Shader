#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include "MathTypes.h"

class DX11Device;

// OceanRenderer is the open-water track, separate from WaterRenderer
// (which targets lakes/bays). The mesh stays a flat quad — but enlarged
// to multiple times the play area — and the shader uses long-wavelength
// Gerstner swell + slope-based whitecaps + foam streaks + sky gradient.
class OceanRenderer
{
public:
    OceanRenderer();
    ~OceanRenderer();

    bool Init(DX11Device* device, const char* shaderPath = "ocean.hlsl");
    void Shutdown();

    // Map extents drive the mesh center; quad is sized to map * meshMul
    // so the player never sees its edge under normal RTS camera moves.
    void SetMapExtents(float originX, float originZ, float mapW, float mapH);
    void SetMeshMultiplier(float m);
    float GetMeshMultiplier() const { return m_meshMul; }

    void  SetEnabled(bool b)            { m_enabled = b; }
    bool  GetEnabled() const            { return m_enabled; }
    void  SetOceanY(float y);
    float GetOceanY() const             { return m_oceanY; }

    void  SetSwellAmp(float a)          { m_swellAmp = a; }
    float GetSwellAmp() const           { return m_swellAmp; }
    void  SetScrollSpeed(float s)       { m_scrollSpeed = s; }
    float GetScrollSpeed() const        { return m_scrollSpeed; }

    void  SetShallowColor(float r, float g, float b);
    void  GetShallowColor(float* r, float* g, float* b) const;
    void  SetDeepColor(float r, float g, float b);
    void  GetDeepColor(float* r, float* g, float* b) const;
    void  SetSkyHorizonColor(float r, float g, float b);
    void  GetSkyHorizonColor(float* r, float* g, float* b) const;
    void  SetSkyZenithColor(float r, float g, float b);
    void  GetSkyZenithColor(float* r, float* g, float* b) const;
    void  SetWhitecapColor(float r, float g, float b);
    void  GetWhitecapColor(float* r, float* g, float* b) const;
    void  SetWhitecapStrength(float s)  { m_whitecapStrength = s; }
    float GetWhitecapStrength() const   { return m_whitecapStrength; }

    void  SetStreakEnabled(bool b)      { m_streakEnabled = b; }
    bool  GetStreakEnabled() const      { return m_streakEnabled; }
    void  SetStreakStrength(float s)    { m_streakStrength = s; }
    float GetStreakStrength() const     { return m_streakStrength; }
    void  SetStreakStretch(float s)     { m_streakStretch = s; }
    float GetStreakStretch() const      { return m_streakStretch; }
    void  SetStreakScroll(float s)      { m_streakScroll = s; }
    float GetStreakScroll() const       { return m_streakScroll; }

    void  SetReflectionEnabled(bool b)  { m_reflectionEnabled = b; }
    bool  GetReflectionEnabled() const  { return m_reflectionEnabled; }
    void  SetReflectionStrength(float s){ m_reflStrength = s; }
    float GetReflectionStrength() const { return m_reflStrength; }
    void  SetFresnelPower(float p)      { m_fresnelPower = p; }
    float GetFresnelPower() const       { return m_fresnelPower; }
    void  SetReflectionDistortion(float d) { m_reflDistort = d; }
    float GetReflectionDistortion() const  { return m_reflDistort; }

    void  Update(float dt)              { m_time += dt; }

    void  Render(const D3DXMATRIX& view, const D3DXMATRIX& proj,
                 const D3DXVECTOR3& eyePos,
                 ID3D11ShaderResourceView* reflectionSRV);

private:
    bool CreatePipeline(const char* shaderPath);
    bool CreatePlaneBuffers();
    bool CreateNoiseTexture();
    void RefreshPlaneVB();

    DX11Device* m_device;

    Microsoft::WRL::ComPtr<ID3D11VertexShader>   m_vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>    m_ps;
    Microsoft::WRL::ComPtr<ID3D11InputLayout>    m_layout;
    Microsoft::WRL::ComPtr<ID3D11Buffer>         m_vb;
    Microsoft::WRL::ComPtr<ID3D11Buffer>         m_ib;
    Microsoft::WRL::ComPtr<ID3D11Buffer>         m_cb;
    Microsoft::WRL::ComPtr<ID3D11BlendState>     m_blend;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rs;
    Microsoft::WRL::ComPtr<ID3D11SamplerState>   m_pointClamp;
    Microsoft::WRL::ComPtr<ID3D11SamplerState>   m_linearWrap;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>      m_noiseTex;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_noiseSRV;

    float m_originX  = -32.0f;
    float m_originZ  = -32.0f;
    float m_mapW     =  64.0f;
    float m_mapH     =  64.0f;
    float m_meshMul  =  8.0f;
    bool  m_meshDirty = true;

    bool  m_enabled       = false;        // off by default; user toggles
    float m_oceanY        = -0.50f;
    float m_swellAmp      = 1.0f;
    float m_scrollSpeed   = 1.0f;
    float m_time          = 0.0f;

    float m_shallow[3]    = { 0.10f, 0.45f, 0.55f };
    float m_deep[3]       = { 0.02f, 0.08f, 0.20f };
    float m_skyHorizon[3] = { 0.78f, 0.85f, 0.92f };
    float m_skyZenith[3]  = { 0.30f, 0.55f, 0.85f };
    float m_whitecap[3]   = { 1.00f, 1.00f, 1.00f };
    float m_whitecapStrength = 0.85f;

    bool  m_streakEnabled = true;
    float m_streakStrength = 0.45f;
    float m_streakStretch  = 0.20f;
    float m_streakScroll   = 0.05f;

    bool  m_reflectionEnabled = true;
    float m_reflStrength      = 0.55f;
    float m_fresnelPower      = 4.5f;
    float m_reflDistort       = 0.05f;
};
