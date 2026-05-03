#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include "MathTypes.h"

class DX11Device;
class DirectionalLight;

// Single-cascade shadow map for the directional sun.
// Owns the depth target + an orthographic light view-proj matrix that
// fits a sphere of radius m_sceneRadius at the world origin. Scene calls
// its own RenderShadowDepth() while this DSV is bound.
class ShadowMap
{
public:
    ShadowMap();
    ~ShadowMap();

    bool Init(DX11Device* device, int size = 2048);
    void Shutdown();

    // Recomputes the light VP from the sun direction and binds the DSV
    // for a depth-only pass. Caller draws scene meshes between
    // BeginPass and EndPass with their own shadow VS.
    void BeginPass(const DirectionalLight& sun);
    void EndPass();

    const D3DXMATRIX&         GetLightViewProj() const { return m_lightVP; }
    ID3D11ShaderResourceView* GetSRV() const           { return m_srv.Get(); }

    void GuiPanel();

    bool  m_enabled    = true;
    float m_sceneRadius = 30.0f;     // ortho frustum half-size in world units
    float m_depthBias  = 0.0008f;
    float m_normalBias = 0.05f;

private:
    DX11Device* m_device;
    int  m_size;

    Microsoft::WRL::ComPtr<ID3D11Texture2D>          m_tex;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView>   m_dsv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_srv;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState>    m_rs;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState>  m_ds;

    D3D11_VIEWPORT m_savedVP;
    D3DXMATRIX     m_lightVP;
};
