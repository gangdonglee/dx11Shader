#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

class DX11Device
{
public:
    DX11Device();
    ~DX11Device();

    bool Init(HWND hwnd, int width, int height, bool windowed = true);
    void Shutdown();

    void BeginFrame(float r = 0.0f, float g = 0.0f, float b = 0.5f, float a = 1.0f);
    void EndFrame();
    void Present();

    ID3D11Device* GetDevice() const { return m_device; }
    ID3D11DeviceContext* GetContext() const { return m_context; }
    IDXGISwapChain* GetSwapChain() const { return m_swapChain; }
    ID3D11RenderTargetView* GetBackBufferRTV() const { return m_backBufferRTV; }
    ID3D11DepthStencilView* GetDepthStencilView() const { return m_depthStencilView; }
    // Read-only DSV that can be bound for depth-test while the same
    // depth texture is sampled as an SRV. Forward overlay passes that
    // read SceneDepth (water L2+) bind this view instead of the rw DSV.
    ID3D11DepthStencilView* GetDepthStencilViewReadOnly() const { return m_depthStencilViewRO; }
    ID3D11ShaderResourceView* GetDepthSRV() const { return m_depthSRV; }
    int GetWidth() const { return m_width; }
    int GetHeight() const { return m_height; }
    bool IsReady() const { return m_device && m_context && m_swapChain; }

private:
    bool CreateBackBufferViews(int width, int height);
    void ReleaseBackBufferViews();

    ID3D11Device* m_device;
    ID3D11DeviceContext* m_context;
    IDXGISwapChain* m_swapChain;
    ID3D11RenderTargetView* m_backBufferRTV;
    ID3D11Texture2D* m_depthStencil;
    ID3D11DepthStencilView* m_depthStencilView;
    ID3D11DepthStencilView* m_depthStencilViewRO;
    ID3D11ShaderResourceView* m_depthSRV;
    int m_width;
    int m_height;
};
