#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include "MathTypes.h"

class DX11Device;

// Persistent foam map. World-XZ-space scalar texture that accumulates
// the wave Jacobian from frame to frame and decays over time, so foam
// patches trail behind breaking waves instead of vanishing the moment
// the wave moves past.
//
// A second pass each frame samples the previous frame, multiplies by
// `decay`, computes the current frame's Jacobian-based foam at every
// pixel via the same Gerstner formula water.hlsl uses, and writes the
// per-pixel max into the new texture. Ping-pong of two RTs avoids
// reading and writing the same target.
class FoamMap
{
public:
    FoamMap();
    ~FoamMap();

    bool Init(DX11Device* device, int size = 1024,
              const char* shaderPath = "foam.hlsl");
    void Shutdown();

    // Wave params mirror Water's so the foam shader can recompute the
    // Jacobian. Plane rect maps worldXZ → texture UV.
    struct UpdateInputs
    {
        float planeOriginX, planeOriginZ;
        float planeSize;        // width in world units (square plane)
        float waveAmp, waveLen, waveSpeed, waveSteep;
        float windCos, windSin;
        int   numWaves;
        float time;
    };
    void Update(const UpdateInputs& in);

    // Sampled by water.hlsl with worldXZ → UV. Returns the *latest*
    // (post-Update) target's SRV.
    ID3D11ShaderResourceView* GetSRV() const;

    // Plane mapping is needed in water.hlsl too — exposed so App can
    // pass it through the water cbuffer.
    float GetPlaneOriginX() const { return m_planeOriginX; }
    float GetPlaneOriginZ() const { return m_planeOriginZ; }
    float GetPlaneSize()    const { return m_planeSize; }

    void GuiPanel();

    bool  m_enabled = true;
    float m_decay   = 0.965f;       // per-frame multiplicative; ~30 frames to halve

private:
    bool CreatePipeline(const char* shaderPath);
    bool CreateRTs(int size);

    DX11Device* m_device;
    int m_size = 0;

    Microsoft::WRL::ComPtr<ID3D11Texture2D>          m_tex[2];
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView>   m_rtv[2];
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_srv[2];
    int m_writeIdx = 0;             // texture being written this frame

    Microsoft::WRL::ComPtr<ID3D11VertexShader>      m_vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>       m_ps;
    Microsoft::WRL::ComPtr<ID3D11Buffer>            m_cb;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState>   m_rs;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_dsOff;
    Microsoft::WRL::ComPtr<ID3D11BlendState>        m_blendNone;
    Microsoft::WRL::ComPtr<ID3D11SamplerState>      m_sampLinearClamp;

    float m_planeOriginX = -40.0f;
    float m_planeOriginZ = -40.0f;
    float m_planeSize    =  80.0f;

    bool  m_firstFrame = true;
};
