#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include "MathTypes.h"

class DX11Device;

// Temporal antialiasing.
// Each frame: SceneColor + Depth + PrevHistory → Output (= NewHistory).
// Reprojection uses the previous frame's view-projection matrix and the
// scene depth buffer to find where each pixel was last frame; the sample
// from history is then variance-clamped against the current 3x3
// neighborhood to prevent ghosting.
//
// Camera projection must be jittered by sub-pixel Halton (2,3) values
// so that the temporal sequence reconstructs sub-pixel detail.
class TAA
{
public:
    TAA();
    ~TAA();

    bool Init(DX11Device* device, int w, int h, const char* shaderPath = "taa.hlsl");
    void Shutdown();

    // Run the resolve pass. sceneColorSRV is this frame's HDR scene
    // color (post-water, pre-tonemap). depthSRV is the matching depth.
    // prevViewProj is what the camera was using one frame ago.
    void Resolve(ID3D11ShaderResourceView* sceneColorSRV,
                 ID3D11ShaderResourceView* depthSRV,
                 const D3DXMATRIX& prevViewProj,
                 const D3DXVECTOR3& eyePos,
                 float screenW, float screenH,
                 bool firstFrame);

    // Output of the current frame (also serves as next frame's history
    // after Swap()).
    ID3D11ShaderResourceView* GetOutputSRV() const { return m_outputSRV.Get(); }

    // Copies output → history so the next frame's Resolve sees this
    // frame's blended result. Call after PostProcess samples GetOutputSRV.
    void Swap();

    void GuiPanel();

    bool  m_enabled       = true;
    float m_historyBlend  = 0.92f;   // weight on history when no clamping needed
    bool  m_clampHistory  = true;    // 3x3 neighborhood clamp (kills ghosting)

private:
    bool CreatePipeline(const char* shaderPath);
    bool CreateRTs(int w, int h);

    DX11Device* m_device;
    int m_width;
    int m_height;

    Microsoft::WRL::ComPtr<ID3D11Texture2D>          m_outputTex;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView>   m_outputRTV;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_outputSRV;

    Microsoft::WRL::ComPtr<ID3D11Texture2D>          m_historyTex;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_historySRV;

    Microsoft::WRL::ComPtr<ID3D11VertexShader>      m_vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>       m_ps;
    Microsoft::WRL::ComPtr<ID3D11Buffer>            m_cb;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState>   m_rs;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_dsOff;
    Microsoft::WRL::ComPtr<ID3D11BlendState>        m_blendNone;
    Microsoft::WRL::ComPtr<ID3D11SamplerState>      m_sampLinear;
    Microsoft::WRL::ComPtr<ID3D11SamplerState>      m_sampPoint;
};
