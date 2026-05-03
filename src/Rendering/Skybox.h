#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include "MathTypes.h"

class DX11Device;
class DirectionalLight;

// Skybox: owns a 256x256 cubemap that gets baked from the procedural
// sky function whenever the directional light changes. The same cubemap
// SRV is sampled by:
//   - this class's Render() for the background pass
//   - Water's reflection (passed in via Render signature)
//
// HDR cubemap loading from .dds/.hdr can replace BakeCubemap() later.
class Skybox
{
public:
    Skybox();
    ~Skybox();

    bool Init(DX11Device* device,
              const char* bakeShader = "skybake.hlsl",
              const char* drawShader = "skybox.hlsl",
              int faceSize = 256);
    void Shutdown();

    // Re-bakes the cubemap if the light's version has changed since the
    // last bake. Cheap when no change; one-time work otherwise.
    void Update(const DirectionalLight& sun);

    // Renders the skybox background. Call after RT clear, before scene
    // meshes (or skip clearing color since this writes every pixel).
    void Render(const D3DXMATRIX& invViewProj, const D3DXVECTOR3& eyePos);

    ID3D11ShaderResourceView* GetCubeSRV() const { return m_cubeSRV.Get(); }
    int  GetFaceSize() const { return m_faceSize; }

    // ImGui knobs for the procedural sky bands. Mutating these via the
    // panel sets m_dirty so the next Update() re-bakes.
    void GuiPanel();

private:
    bool CreatePipeline(const char* bakeShader, const char* drawShader);
    bool CreateCubemap(int faceSize);
    void BakeCubemap(const DirectionalLight& sun);

    DX11Device* m_device;

    // Cubemap target.
    Microsoft::WRL::ComPtr<ID3D11Texture2D>          m_cubeTex;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_cubeSRV;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView>   m_faceRTV[6];

    // Pipelines.
    Microsoft::WRL::ComPtr<ID3D11VertexShader>      m_bakeVS;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>       m_bakePS;
    Microsoft::WRL::ComPtr<ID3D11VertexShader>      m_drawVS;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>       m_drawPS;
    Microsoft::WRL::ComPtr<ID3D11Buffer>            m_cb;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState>   m_rs;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_dsOff;
    Microsoft::WRL::ComPtr<ID3D11BlendState>        m_blendNone;
    Microsoft::WRL::ComPtr<ID3D11SamplerState>      m_sampLinear;

    int      m_faceSize = 256;
    unsigned m_lastBakedVersion = 0;
    bool     m_dirty            = true;     // forces a re-bake on next Update

    // Procedural sky bands. Sun disc/glow is added on top in skybake.hlsl.
    float m_horizon[3] = { 0.72f, 0.78f, 0.86f };
    float m_zenith[3]  = { 0.18f, 0.36f, 0.62f };
    float m_ground[3]  = { 0.32f, 0.30f, 0.27f };
};
