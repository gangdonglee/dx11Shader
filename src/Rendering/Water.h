#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <vector>
#include "MathTypes.h"

class DX11Device;

// Phase 3 Water: a tessellated grid quad displaced by a stack of Gerstner
// waves in the vertex shader, shaded with Fresnel-blended sky reflection
// (computed inline against the same sky function used by Scene).
//
// Phase 4 will add screen-space reflection (SSR) and caustics on top of
// this. The pixel shader keeps an explicit "scene color" SRV slot so the
// refraction pass can plug straight in once we add the SceneColor RT.
class Water
{
public:
    Water();
    ~Water();

    bool Init(DX11Device* device, const char* shaderPath = "water.hlsl");
    void Shutdown();

    void Update(float dt) { m_time += dt; }
    // sceneColorSRV / sceneDepthSRV may be null — in that case the
    // refraction term collapses to the shallow tint and depth-blend
    // collapses to "everything is shallow". skyCubeSRV must be a valid
    // cubemap SRV; reflection samples it with the reflected view dir.
    void Render(const D3DXMATRIX& view, const D3DXMATRIX& proj,
                const D3DXVECTOR3& eye,
                const D3DXVECTOR3& lightDir,
                const float lightColor[3],
                ID3D11ShaderResourceView* sceneColorSRV,
                ID3D11ShaderResourceView* sceneDepthSRV,
                ID3D11ShaderResourceView* skyCubeSRV,
                ID3D11ShaderResourceView* foamMapSRV,
                float foamPlaneOriginX, float foamPlaneOriginZ, float foamPlaneSize,
                float screenW, float screenH);

    // Wave params used by FoamMap for Jacobian recomputation.
    float GetWaveAmp()    const { return m_waveAmp; }
    float GetWaveLen()    const { return m_waveLen; }
    float GetWaveSpeed()  const { return m_waveSpeed; }
    float GetWaveSteep()  const { return m_waveSteep; }
    float GetWindDir()    const { return m_windDir; }
    int   GetNumWaves()   const { return m_numWaves; }
    float GetTime()       const { return m_time; }
    float GetPlaneSize()  const { return m_planeSize; }

    void GuiPanel();

    void  SetEnabled(bool b) { m_enabled = b; }
    bool  GetEnabled() const { return m_enabled; }
    void  SetWaterY(float y) { m_waterY = y; }
    float GetWaterY() const  { return m_waterY; }

    // Setters used by preset functions (DebugUI). Values are clamped to
    // sensible ranges by the source slider widgets, but presets bypass
    // those so callers should pre-validate.
    void  SetWaveAmp(float a)         { m_waveAmp = a; }
    void  SetRefractStrength(float s) { m_refractStrength = s; }
    void  SetFresnelPow(float p)      { m_fresnelPow = p; }
    void  SetSkyTint(float t)         { m_skyTint = t; }
    void  SetSsrEnabled(bool b)       { m_ssrEnabled = b; }
    void  SetExtinction(float r, float g, float b)
                                       { m_extinction[0]=r; m_extinction[1]=g; m_extinction[2]=b; }
    void  SetScatterStrength(float s) { m_scatterStrength = s; }

private:
    struct Vertex
    {
        float pos[3];
        float uv[2];
    };

    bool CreatePipeline(const char* shaderPath);
    bool CreateGrid(int divs, float size);
    bool CreateDetailNormalMap(int size);

    DX11Device* m_device;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>  m_ps;
    Microsoft::WRL::ComPtr<ID3D11InputLayout>  m_layout;
    Microsoft::WRL::ComPtr<ID3D11Buffer>       m_vb;
    Microsoft::WRL::ComPtr<ID3D11Buffer>       m_ib;
    Microsoft::WRL::ComPtr<ID3D11Buffer>       m_cb;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState>   m_rs;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_ds;
    Microsoft::WRL::ComPtr<ID3D11BlendState>        m_blend;
    Microsoft::WRL::ComPtr<ID3D11SamplerState>      m_sampLinear;
    Microsoft::WRL::ComPtr<ID3D11SamplerState>      m_sampPoint;
    Microsoft::WRL::ComPtr<ID3D11SamplerState>      m_sampLinearWrap;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>         m_detailNormalTex;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_detailNormalSRV;

    UINT  m_indexCount = 0;

    bool  m_enabled       = true;
    float m_waterY        = 0.30f;
    float m_time          = 0.0f;
    float m_planeSize     = 80.0f;
    int   m_planeDivs     = 192;

    // Wave globals
    float m_waveAmp        = 0.18f;
    float m_waveLen        = 6.0f;
    float m_waveSpeed      = 1.0f;
    float m_waveSteep      = 0.55f;
    float m_windDir        = 0.7f;     // radians, around Y
    int   m_numWaves       = 24;

    // Surface look
    float m_shallow[3]     = { 0.18f, 0.42f, 0.55f };
    float m_deep[3]        = { 0.02f, 0.10f, 0.20f };
    float m_fresnelPow     = 5.0f;
    float m_specPower      = 320.0f;
    float m_skyTint        = 1.0f;
    float m_refractStrength = 0.04f;
    bool  m_ssrEnabled      = true;
    int   m_ssrSteps        = 24;
    float m_ssrStepLen      = 0.45f;
    float m_ssrThickness    = 0.50f;
    float m_causticStrength = 0.55f;
    float m_causticScale    = 1.20f;
    float m_sssStrength     = 0.50f;

    // Beer-Lambert absorption: per-channel extinction in m^-1.
    // Default ≈ clear ocean. Higher = murkier; lower = clearer.
    float m_extinction[3]   = { 0.45f, 0.10f, 0.04f };
    float m_scatterStrength = 1.0f;     // scales Deep.rgb as the scattering color

    // Detail normal layers — high-frequency ripples on top of Gerstner.
    bool  m_detailEnabled   = true;
    float m_detailStrength  = 0.55f;
    float m_detailScale[3]  = { 0.35f, 0.85f, 1.80f };  // tile freq per layer

    // Reflection roughness — picks the mip level of the sky cubemap.
    // 0 = mirror (mip 0), 1 = very rough (top mip). Use to soften
    // reflection on choppy water without changing the geometry.
    float m_reflRoughness   = 0.05f;
    float m_skyMaxMip       = 8.0f;     // log2(faceSize); set in Init from Skybox.
};
