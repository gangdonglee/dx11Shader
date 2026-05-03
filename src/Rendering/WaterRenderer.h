#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include "MathTypes.h"

class DX11Device;

// WaterRenderer draws the global sea-level plane on top of the deferred
// lighting result. Phase 1 = single base color with alpha blend (depth
// read-only). Subsequent phases will extend this same class with normal
// scroll, depth-based color, foam, and planar reflection.
class WaterRenderer
{
public:
    WaterRenderer();
    ~WaterRenderer();

    bool Init(DX11Device* device, const char* shaderPath = "water.hlsl");
    void Shutdown();

    // Match the world map. Called every frame from App once the
    // pathfinder reports its grid extents.
    void SetMapExtents(float originX, float originZ, float mapW, float mapH);

    void  SetEnabled(bool b)   { m_enabled = b; }
    bool  GetEnabled() const   { return m_enabled; }
    void  SetWaterY(float y);
    float GetWaterY() const    { return m_waterY; }
    void  SetBaseColor(float r, float g, float b, float a);
    void  GetBaseColor(float* r, float* g, float* b, float* a) const;

    // Phase 2 — surface wave tuning. amp scales the perturbation
    // normal's steepness; scroll multiplies the elapsed time fed
    // into the two-layer sin field.
    void  SetWaveAmp(float a)        { m_waveAmp = a; }
    float GetWaveAmp() const         { return m_waveAmp; }
    void  SetScrollSpeed(float s)    { m_scrollSpeed = s; }
    float GetScrollSpeed() const     { return m_scrollSpeed; }

    // Phase 3 — depth-based color blend between shallow (= base color
    // rgb) and deep. depthScale is the worldY distance at which the
    // lerp reaches the deep color.
    void  SetDeepColor(float r, float g, float b);
    void  GetDeepColor(float* r, float* g, float* b) const;
    void  SetDepthScale(float s)     { m_depthScale = s; }
    float GetDepthScale() const      { return m_depthScale; }

    // Phase 4 — shore foam. threshold = depthY at which foam fully
    // dissipates; speed = noise drift rate; freq = world-space noise
    // tiling; strength = max contribution to the lerp toward foamColor.
    void  SetFoamColor(float r, float g, float b);
    void  GetFoamColor(float* r, float* g, float* b) const;
    void  SetFoamThreshold(float t)  { m_foamThreshold = t; }
    float GetFoamThreshold() const   { return m_foamThreshold; }
    void  SetFoamSpeed(float s)      { m_foamSpeed = s; }
    float GetFoamSpeed() const       { return m_foamSpeed; }
    void  SetFoamStrength(float s)   { m_foamStrength = s; }
    float GetFoamStrength() const    { return m_foamStrength; }

    // Phase 5 — planar reflection. strength scales the contribution of
    // the reflected scene; fresnelPower controls how rapidly reflection
    // ramps up at grazing angles; distortion offsets the reflection UV
    // by the surface normal so wave ripples shimmer the reflection.
    void  SetReflectionEnabled(bool b)      { m_reflectionEnabled = b; }
    bool  GetReflectionEnabled() const      { return m_reflectionEnabled; }
    void  SetReflectionStrength(float s)    { m_reflStrength = s; }
    float GetReflectionStrength() const     { return m_reflStrength; }
    void  SetFresnelPower(float p)          { m_fresnelPower = p; }
    float GetFresnelPower() const           { return m_fresnelPower; }
    void  SetReflectionDistortion(float d)  { m_reflDistort = d; }
    float GetReflectionDistortion() const   { return m_reflDistort; }

    // Phase C — refraction (samples SceneColor with normal-distorted UV)
    // and caustics (animated noise tinting the floor sample).
    void  SetRefractionStrength(float s)    { m_refrStrength = s; }
    float GetRefractionStrength() const     { return m_refrStrength; }
    void  SetCausticStrength(float s)       { m_causticStrength = s; }
    float GetCausticStrength() const        { return m_causticStrength; }
    void  SetCausticScale(float s)          { m_causticScale = s; }
    float GetCausticScale() const           { return m_causticScale; }

    // Screen size is consumed by SV_POSITION → screen UV in the PS.
    // App passes WIDTH/HEIGHT once after Init.
    void  SetScreenSize(int w, int h) { m_screenW = w; m_screenH = h; }

    void  Update(float dt)     { m_time += dt; }

    // sceneDepthSRV may be null (Phase 1/2 fallback); when null the
    // depth-based blend collapses to the shallow color and the bound
    // SRV slot is cleared.
    void  Render(const D3DXMATRIX& view, const D3DXMATRIX& proj,
                 const D3DXVECTOR3& eyePos,
                 ID3D11ShaderResourceView* sceneDepthSRV,
                 ID3D11ShaderResourceView* reflectionSRV,
                 ID3D11ShaderResourceView* sceneColorSRV);

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
    Microsoft::WRL::ComPtr<ID3D11SamplerState>   m_depthSampler;
    Microsoft::WRL::ComPtr<ID3D11SamplerState>   m_linearWrap;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>      m_noiseTex;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_noiseSRV;

    float m_originX  = -32.0f;
    float m_originZ  = -32.0f;
    float m_mapW     = 64.0f;
    float m_mapH     = 64.0f;
    bool  m_meshDirty = true;

    bool  m_enabled    = true;
    float m_waterY     = 0.0f;
    float m_color[4]   = { 0.18f, 0.42f, 0.55f, 0.55f };  // shallow rgba
    float m_deep[3]    = { 0.05f, 0.18f, 0.30f };          // deep rgb
    float m_depthScale = 3.0f;
    float m_time       = 0.0f;
    float m_waveAmp    = 0.35f;
    float m_scrollSpeed = 1.0f;
    int   m_screenW    = 1;
    int   m_screenH    = 1;

    float m_foam[3]        = { 1.0f, 1.0f, 1.0f };
    float m_foamThreshold  = 0.40f;
    float m_foamSpeed      = 1.5f;
    float m_foamStrength   = 0.85f;
    float m_foamFreq       = 1.6f;

    bool  m_reflectionEnabled = true;
    float m_reflStrength      = 0.6f;
    float m_fresnelPower      = 5.0f;
    float m_reflDistort       = 0.05f;

    float m_refrStrength      = 0.04f;   // SceneColor UV distortion in screen-uv units
    float m_causticStrength   = 0.55f;
    float m_causticScale      = 0.18f;
};
