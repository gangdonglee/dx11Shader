#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include "MathTypes.h"

class DX11Device;

// Phase 5 post-process. Single fullscreen pass that samples SceneColor +
// SceneDepth and writes into the currently bound render target (the
// backbuffer). Replaces the App's "CopyResource SceneColor → backbuffer"
// step. Layers in:
//   - Depth-edge outlines (Sobel-like 3x3)
//   - Exponential height/distance fog
//   - Lift / gamma / gain color grading + saturation tweak
//
// Each effect is independently togglable via ImGui.
class PostProcess
{
public:
    PostProcess();
    ~PostProcess();

    bool Init(DX11Device* device, const char* shaderPath = "post.hlsl");
    void Shutdown();

    // Renders into the currently bound RTV. Caller binds backbuffer.
    void Render(ID3D11ShaderResourceView* sceneColorSRV,
                ID3D11ShaderResourceView* sceneDepthSRV,
                const D3DXMATRIX& invViewProj,
                const D3DXVECTOR3& eyePos,
                float screenW, float screenH,
                float nearZ, float farZ);

    void GuiPanel();

private:
    bool CreatePipeline(const char* shaderPath);

    DX11Device* m_device;

    Microsoft::WRL::ComPtr<ID3D11VertexShader>      m_vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>       m_ps;
    Microsoft::WRL::ComPtr<ID3D11Buffer>            m_cb;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_dsOff;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState>   m_rs;
    Microsoft::WRL::ComPtr<ID3D11BlendState>        m_blendNone;
    Microsoft::WRL::ComPtr<ID3D11SamplerState>      m_sampLinear;
    Microsoft::WRL::ComPtr<ID3D11SamplerState>      m_sampPoint;

public:
    // ---- knobs (public so DebugUI / App can read defaults) ----
    bool  m_outlineEnabled  = true;
    float m_outlineStrength = 0.7f;
    float m_outlineThresh   = 0.0008f;
    float m_outlineColor[3] = { 0.05f, 0.07f, 0.10f };

    bool  m_fogEnabled      = true;
    float m_fogDensity      = 0.020f;     // exp fog density
    float m_fogStart        = 6.0f;
    float m_fogColor[3]     = { 0.78f, 0.86f, 0.96f };

    bool  m_gradeEnabled    = true;
    float m_exposure        = 1.0f;
    float m_saturation      = 1.05f;
    float m_lift[3]         = { 0.0f, 0.0f, 0.0f };
    float m_gain[3]         = { 1.0f, 1.0f, 1.0f };
    float m_gamma           = 1.0f;       // applied as pow(c, 1/gamma)

    // ACES filmic tonemap: HDR -> LDR. Always on by default since
    // SceneColor is now a float16 RT and writing it raw to the LDR
    // backbuffer would just clamp at 1.0.
    bool  m_tonemapEnabled  = true;
};
