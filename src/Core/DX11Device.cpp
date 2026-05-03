#include "DX11Device.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

DX11Device::DX11Device()
    : m_device(nullptr), m_context(nullptr), m_swapChain(nullptr),
      m_backBufferRTV(nullptr), m_depthStencil(nullptr), m_depthStencilView(nullptr),
      m_depthStencilViewRO(nullptr), m_depthSRV(nullptr),
      m_width(0), m_height(0)
{
}

DX11Device::~DX11Device()
{
    Shutdown();
}

bool DX11Device::Init(HWND hwnd, int width, int height, bool windowed)
{
    m_width = width;
    m_height = height;

    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferDesc.Width = width;
    scd.BufferDesc.Height = height;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferDesc.RefreshRate.Numerator = 60;
    scd.BufferDesc.RefreshRate.Denominator = 1;
    scd.SampleDesc.Count = 1;
    scd.SampleDesc.Quality = 0;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 1;
    scd.OutputWindow = hwnd;
    scd.Windowed = windowed ? TRUE : FALSE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL requestedLevels[] =
    {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL createdLevel = D3D_FEATURE_LEVEL_11_0;

    UINT flags = 0;
#if defined(_DEBUG)
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        flags,
        requestedLevels,
        sizeof(requestedLevels) / sizeof(requestedLevels[0]),
        D3D11_SDK_VERSION,
        &scd,
        &m_swapChain,
        &m_device,
        &createdLevel,
        &m_context);

#if defined(_DEBUG)
    if (FAILED(hr))
    {
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            flags,
            requestedLevels,
            sizeof(requestedLevels) / sizeof(requestedLevels[0]),
            D3D11_SDK_VERSION,
            &scd,
            &m_swapChain,
            &m_device,
            &createdLevel,
            &m_context);
    }
#endif

    if (FAILED(hr))
        return false;

    return CreateBackBufferViews(width, height);
}

void DX11Device::Shutdown()
{
    ReleaseBackBufferViews();
    if (m_swapChain) { m_swapChain->Release(); m_swapChain = nullptr; }
    if (m_context)
    {
        m_context->ClearState();
        m_context->Release();
        m_context = nullptr;
    }
    if (m_device) { m_device->Release(); m_device = nullptr; }
}

bool DX11Device::CreateBackBufferViews(int width, int height)
{
    ID3D11Texture2D* backBuffer = nullptr;
    HRESULT hr = m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);
    if (FAILED(hr))
        return false;

    hr = m_device->CreateRenderTargetView(backBuffer, nullptr, &m_backBufferRTV);
    backBuffer->Release();
    if (FAILED(hr))
        return false;

    // Typeless depth so the same texture can serve a writable DSV, a
    // read-only DSV (for forward passes that also sample depth), and
    // a SHADER_RESOURCE for water depth-color (L2+).
    D3D11_TEXTURE2D_DESC depthDesc = {};
    depthDesc.Width = width;
    depthDesc.Height = height;
    depthDesc.MipLevels = 1;
    depthDesc.ArraySize = 1;
    depthDesc.Format = DXGI_FORMAT_R24G8_TYPELESS;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Usage = D3D11_USAGE_DEFAULT;
    depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;

    hr = m_device->CreateTexture2D(&depthDesc, nullptr, &m_depthStencil);
    if (FAILED(hr))
        return false;

    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    hr = m_device->CreateDepthStencilView(m_depthStencil, &dsvDesc, &m_depthStencilView);
    if (FAILED(hr))
        return false;

    dsvDesc.Flags = D3D11_DSV_READ_ONLY_DEPTH | D3D11_DSV_READ_ONLY_STENCIL;
    hr = m_device->CreateDepthStencilView(m_depthStencil, &dsvDesc, &m_depthStencilViewRO);
    if (FAILED(hr))
        return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    hr = m_device->CreateShaderResourceView(m_depthStencil, &srvDesc, &m_depthSRV);
    if (FAILED(hr))
        return false;

    D3D11_VIEWPORT vp = {};
    vp.Width = (float)width;
    vp.Height = (float)height;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    m_context->RSSetViewports(1, &vp);
    return true;
}

void DX11Device::ReleaseBackBufferViews()
{
    if (m_depthSRV)            { m_depthSRV->Release();            m_depthSRV = nullptr; }
    if (m_depthStencilViewRO)  { m_depthStencilViewRO->Release();  m_depthStencilViewRO = nullptr; }
    if (m_depthStencilView)    { m_depthStencilView->Release();    m_depthStencilView = nullptr; }
    if (m_depthStencil)        { m_depthStencil->Release();        m_depthStencil = nullptr; }
    if (m_backBufferRTV)       { m_backBufferRTV->Release();       m_backBufferRTV = nullptr; }
}

void DX11Device::BeginFrame(float r, float g, float b, float a)
{
    if (!IsReady())
        return;

    float clearColor[4] = { r, g, b, a };
    m_context->OMSetRenderTargets(1, &m_backBufferRTV, m_depthStencilView);
    m_context->ClearRenderTargetView(m_backBufferRTV, clearColor);
    m_context->ClearDepthStencilView(m_depthStencilView,
        D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
}

void DX11Device::EndFrame()
{
}

void DX11Device::Present()
{
    if (m_swapChain)
        m_swapChain->Present(1, 0);
}
