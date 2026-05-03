#pragma once

#include <d3d11.h>

class DX11Device;

// Renderer is the central frame orchestrator.
//
// Phase 1 (current): a thin wrapper around DX11Device's frame primitives so
// App::Run can express the per-frame work as Begin/End/Present without holding
// the device directly. Subsequent phases will expand this class to own the
// G-buffer, the lighting pass, and per-pass RTV bindings without changing the
// caller surface.
class Renderer
{
public:
    Renderer();
    ~Renderer();

    bool Init(DX11Device* device, int width, int height);
    void Shutdown();

    void BeginFrame();
    void EndFrame();
    void Present();

    // Phase 2: deferred skeleton
    void OnResize(int width, int height);
    bool IsDeferredReady() const { return m_deferredReady; }
    void BindGeometryPass();
    void BindLightingTarget();
    void DrawLightingPass();
    void BeginForwardOverlayPass(bool useDepthReadOnly);
    void EndForwardOverlayPass();
    ID3D11ShaderResourceView* GetGBufferSRV(int index) const;

    // Phase C — SceneColor (lit RT) is what the lighting pass writes to,
    // then copied onto the backbuffer just before forward overlays so
    // water can sample it as an SRV for refraction.
    void CopySceneColorToBackbuffer();
    ID3D11ShaderResourceView* GetSceneColorSRV() const { return m_sceneColorSRV; }

    // Phase 5 — half-resolution reflection target. Map3D writes the
    // mirrored terrain into this RT/DSV; WaterRenderer samples it
    // through GetReflectionSRV in its pixel shader.
    bool BeginReflectionPass();
    void EndReflectionPass();
    int  GetReflectionWidth() const  { return m_reflectionWidth; }
    int  GetReflectionHeight() const { return m_reflectionHeight; }
    ID3D11ShaderResourceView* GetReflectionSRV() const { return m_reflectionSRV; }

    DX11Device* GetDevice() const { return m_device; }

private:
    struct GBufferTarget
    {
        GBufferTarget()
            : texture(nullptr), rtv(nullptr), srv(nullptr), format(DXGI_FORMAT_UNKNOWN) {}
        ID3D11Texture2D* texture;
        ID3D11RenderTargetView* rtv;
        ID3D11ShaderResourceView* srv;
        DXGI_FORMAT format;
    };

    bool CreateDeferredResources(int width, int height);
    void ReleaseDeferredResources();
    bool CreateLightingPipeline(const char* shaderPath);
    void ReleaseLightingPipeline();
    bool CreateReflectionResources(int width, int height);
    void ReleaseReflectionResources();
    bool CreateSceneColorResources(int width, int height);
    void ReleaseSceneColorResources();

    static const int kGBufferCount = 4;

    DX11Device* m_device;
    int m_width;
    int m_height;
    bool m_deferredReady;

    GBufferTarget m_gbuffer[kGBufferCount];
    ID3D11VertexShader* m_fullscreenVS;
    ID3D11PixelShader* m_lightingPS;
    ID3D11SamplerState* m_linearClampSampler;
    ID3D11Buffer* m_lightingCB;
    ID3D11DepthStencilState* m_overlayDepthReadOnly;

    // Reflection pass — half-resolution single-RT forward render of
    // the mirrored terrain. Sampled by water.hlsl.
    int                     m_reflectionWidth   = 0;
    int                     m_reflectionHeight  = 0;
    ID3D11Texture2D*        m_reflectionTex     = nullptr;
    ID3D11RenderTargetView* m_reflectionRTV     = nullptr;
    ID3D11ShaderResourceView* m_reflectionSRV   = nullptr;
    ID3D11Texture2D*        m_reflectionDepth   = nullptr;
    ID3D11DepthStencilView* m_reflectionDSV     = nullptr;

    // Lighting pass writes here; CopySceneColorToBackbuffer blits this
    // onto the swapchain backbuffer. Water samples the SRV.
    ID3D11Texture2D*        m_sceneColorTex     = nullptr;
    ID3D11RenderTargetView* m_sceneColorRTV     = nullptr;
    ID3D11ShaderResourceView* m_sceneColorSRV   = nullptr;
};
