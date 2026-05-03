#include "Renderer.h"
#include "DX11Device.h"
#include <d3dcompiler.h>
#include <string>

#pragma comment(lib, "d3dcompiler.lib")

namespace
{
    template <class T>
    void ReleaseCOM(T*& p)
    {
        if (p)
        {
            p->Release();
            p = nullptr;
        }
    }

    struct LightingCB
    {
        float lightDir[3];
        float ambient;
        float lightColor[3];
        float exposure;
    };

    static std::wstring ToWidePath(const char* path)
    {
        int count = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
        std::wstring wide(count, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, path, -1, &wide[0], count);
        if (!wide.empty() && wide.back() == L'\0')
            wide.pop_back();
        return wide;
    }

    static bool CompileShaderFile(const char* path, const char* entry, const char* target, ID3DBlob** blob)
    {
        if (!path || !entry || !target || !blob) return false;

        ID3DBlob* errors = nullptr;
        UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

        std::string name = path;
        size_t slash = name.find_last_of("\\/");
        if (slash != std::string::npos)
            name = name.substr(slash + 1);

        const std::string candidates[] =
        {
            std::string(path),                   // as-is
            name,                                // cwd + filename
            "assets\\Shaders\\" + name,          // repo root launch
            "..\\assets\\Shaders\\" + name,      // bin launch
            "..\\..\\assets\\Shaders\\" + name   // VS out-dir launch
        };

        HRESULT hr = E_FAIL;
        for (const std::string& resolved : candidates)
        {
            std::wstring wide = ToWidePath(resolved.c_str());
            hr = D3DCompileFromFile(wide.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                    entry, target, flags, 0, blob, &errors);
            if (SUCCEEDED(hr))
                break;

            if (errors)
            {
                errors->Release();
                errors = nullptr;
            }
        }

        if (FAILED(hr))
        {
            if (errors)
            {
                MessageBoxA(nullptr, (const char*)errors->GetBufferPointer(), path, MB_OK);
                errors->Release();
            }
            return false;
        }

        if (errors) errors->Release();
        return true;
    }
}

Renderer::Renderer()
    : m_device(nullptr),
      m_width(0),
      m_height(0),
      m_deferredReady(false),
      m_fullscreenVS(nullptr),
      m_lightingPS(nullptr),
      m_linearClampSampler(nullptr),
      m_lightingCB(nullptr),
      m_overlayDepthReadOnly(nullptr)
{
}

Renderer::~Renderer()
{
    Shutdown();
}

bool Renderer::Init(DX11Device* device, int width, int height)
{
    if (!device) return false;
    m_device = device;
    m_width = width;
    m_height = height;

    if (!CreateDeferredResources(width, height))
        return false;
    if (!CreateLightingPipeline("lighting.fx"))
        return false;
    if (!CreateReflectionResources(width / 2, height / 2))
        return false;
    if (!CreateSceneColorResources(width, height))
        return false;

    ID3D11Device* dev = m_device->GetDevice();
    if (!dev) return false;
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    if (FAILED(dev->CreateDepthStencilState(&dd, &m_overlayDepthReadOnly)))
        return false;

    m_deferredReady = true;
    return true;
}

void Renderer::Shutdown()
{
    ReleaseSceneColorResources();
    ReleaseReflectionResources();
    ReleaseCOM(m_overlayDepthReadOnly);
    ReleaseLightingPipeline();
    ReleaseDeferredResources();
    m_deferredReady = false;
    m_width = 0;
    m_height = 0;
    m_device = nullptr;
}

void Renderer::BeginFrame()
{
    if (m_device) m_device->BeginFrame(0.0f, 0.0f, 0.0f, 1.0f);
}

void Renderer::EndFrame()
{
    if (m_device) m_device->EndFrame();
}

void Renderer::Present()
{
    if (m_device) m_device->Present();
}

void Renderer::OnResize(int width, int height)
{
    if (!m_device || width <= 0 || height <= 0)
        return;
    if (m_width == width && m_height == height)
        return;

    m_width = width;
    m_height = height;
    m_deferredReady = CreateDeferredResources(width, height);
    CreateReflectionResources(width / 2, height / 2);
    CreateSceneColorResources(width, height);
}

void Renderer::BindGeometryPass()
{
    if (!m_device || !m_deferredReady) return;

    ID3D11DeviceContext* ctx = m_device->GetContext();
    if (!ctx) return;

    ID3D11ShaderResourceView* nullSRVs[kGBufferCount] = {};
    ctx->PSSetShaderResources(0, kGBufferCount, nullSRVs);

    ID3D11RenderTargetView* rtvs[kGBufferCount] = {};
    for (int i = 0; i < kGBufferCount; ++i)
        rtvs[i] = m_gbuffer[i].rtv;

    ctx->OMSetRenderTargets(kGBufferCount, rtvs, m_device->GetDepthStencilView());

    const float clear[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < kGBufferCount; ++i)
        if (m_gbuffer[i].rtv) ctx->ClearRenderTargetView(m_gbuffer[i].rtv, clear);
    if (m_device->GetDepthStencilView())
        ctx->ClearDepthStencilView(m_device->GetDepthStencilView(),
                                   D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

    D3D11_VIEWPORT vp = {};
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width = (float)m_width;
    vp.Height = (float)m_height;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);
}

void Renderer::BindLightingTarget()
{
    if (!m_device || !m_deferredReady) return;

    ID3D11DeviceContext* ctx = m_device->GetContext();
    if (!ctx || !m_sceneColorRTV) return;

    // Lighting writes into SceneColor (sampled later by water for
    // refraction). The blit pass copies it to the backbuffer.
    ID3D11ShaderResourceView* nullSRV = nullptr;
    ctx->PSSetShaderResources(7, 1, &nullSRV);
    ctx->OMSetRenderTargets(1, &m_sceneColorRTV, nullptr);

    D3D11_VIEWPORT vp = {};
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width = (float)m_width;
    vp.Height = (float)m_height;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);
}

void Renderer::CopySceneColorToBackbuffer()
{
    if (!m_device || !m_sceneColorTex) return;
    ID3D11DeviceContext* ctx = m_device->GetContext();
    IDXGISwapChain* swap = m_device->GetSwapChain();
    if (!ctx || !swap) return;

    ID3D11Texture2D* bb = nullptr;
    if (FAILED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) || !bb)
        return;
    ctx->CopyResource(bb, m_sceneColorTex);
    bb->Release();
}

void Renderer::DrawLightingPass()
{
    if (!m_device || !m_deferredReady || !m_fullscreenVS || !m_lightingPS) return;

    ID3D11DeviceContext* ctx = m_device->GetContext();
    if (!ctx) return;

    ID3D11ShaderResourceView* srvs[kGBufferCount] = {};
    for (int i = 0; i < kGBufferCount; ++i)
        srvs[i] = m_gbuffer[i].srv;

    LightingCB cb = {};
    cb.lightDir[0] = 0.32f;
    cb.lightDir[1] = -1.0f;
    cb.lightDir[2] = 0.20f;
    cb.ambient = 0.36f;
    cb.lightColor[0] = 0.90f;
    cb.lightColor[1] = 0.90f;
    cb.lightColor[2] = 0.90f;
    cb.exposure = 1.0f;
    ctx->UpdateSubresource(m_lightingCB, 0, nullptr, &cb, 0, 0);

    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(m_fullscreenVS, nullptr, 0);
    ctx->PSSetShader(m_lightingPS, nullptr, 0);
    ctx->PSSetShaderResources(0, kGBufferCount, srvs);
    ctx->PSSetSamplers(0, 1, &m_linearClampSampler);
    ctx->PSSetConstantBuffers(0, 1, &m_lightingCB);
    ctx->Draw(3, 0);

    ID3D11ShaderResourceView* nullSRVs[kGBufferCount] = {};
    ctx->PSSetShaderResources(0, kGBufferCount, nullSRVs);
}

void Renderer::BeginForwardOverlayPass(bool useDepthReadOnly)
{
    if (!m_device) return;

    ID3D11DeviceContext* ctx = m_device->GetContext();
    ID3D11RenderTargetView* backBuffer = m_device->GetBackBufferRTV();
    if (!ctx || !backBuffer) return;

    // Read-only DSV when forward passes (water L2+, overlays) need to
    // sample SceneDepth via SRV while still depth-testing against the
    // lit scene.
    ID3D11DepthStencilView* dsv = useDepthReadOnly ? m_device->GetDepthStencilViewReadOnly() : nullptr;
    ctx->OMSetRenderTargets(1, &backBuffer, dsv);

    if (useDepthReadOnly && m_overlayDepthReadOnly)
        ctx->OMSetDepthStencilState(m_overlayDepthReadOnly, 0);
    else
        ctx->OMSetDepthStencilState(nullptr, 0);

    D3D11_VIEWPORT vp = {};
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width = (float)m_width;
    vp.Height = (float)m_height;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);
}

void Renderer::EndForwardOverlayPass()
{
    if (!m_device) return;
    ID3D11DeviceContext* ctx = m_device->GetContext();
    if (!ctx) return;
    ctx->OMSetDepthStencilState(nullptr, 0);
}

ID3D11ShaderResourceView* Renderer::GetGBufferSRV(int index) const
{
    if (!m_deferredReady) return nullptr;
    if (index < 0 || index >= kGBufferCount) return nullptr;
    return m_gbuffer[index].srv;
}

bool Renderer::BeginReflectionPass()
{
    if (!m_device || !m_reflectionRTV || !m_reflectionDSV) return false;
    ID3D11DeviceContext* ctx = m_device->GetContext();
    if (!ctx) return false;

    // Clear any previously bound copy of the RT as a shader input so
    // the OM bind below doesn't trip the runtime hazard tracker.
    ID3D11ShaderResourceView* nullSRV = nullptr;
    ctx->PSSetShaderResources(7, 1, &nullSRV);

    ctx->OMSetRenderTargets(1, &m_reflectionRTV, m_reflectionDSV);

    // Sky-blue clear color so areas the discard pass leaves untouched
    // (above the horizon, between mountains) read like a flat sky tone
    // when sampled by water.
    const float clear[4] = { 0.55f, 0.72f, 0.88f, 1.0f };
    ctx->ClearRenderTargetView(m_reflectionRTV, clear);
    ctx->ClearDepthStencilView(m_reflectionDSV,
                               D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

    D3D11_VIEWPORT vp = {};
    vp.Width  = (float)m_reflectionWidth;
    vp.Height = (float)m_reflectionHeight;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);
    return true;
}

void Renderer::EndReflectionPass()
{
    if (!m_device) return;
    ID3D11DeviceContext* ctx = m_device->GetContext();
    if (!ctx) return;

    // Detach reflection RT before someone else binds the same texture
    // as a shader resource (water shader does so during its draw).
    ID3D11RenderTargetView* nullRTV = nullptr;
    ctx->OMSetRenderTargets(1, &nullRTV, nullptr);

    D3D11_VIEWPORT vp = {};
    vp.Width  = (float)m_width;
    vp.Height = (float)m_height;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);
}

bool Renderer::CreateReflectionResources(int width, int height)
{
    ReleaseReflectionResources();
    if (!m_device || width <= 0 || height <= 0) return false;

    ID3D11Device* dev = m_device->GetDevice();
    if (!dev) return false;

    m_reflectionWidth  = width;
    m_reflectionHeight = height;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width  = (UINT)width;
    td.Height = (UINT)height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &m_reflectionTex))) return false;
    if (FAILED(dev->CreateRenderTargetView(m_reflectionTex, nullptr, &m_reflectionRTV))) return false;
    if (FAILED(dev->CreateShaderResourceView(m_reflectionTex, nullptr, &m_reflectionSRV))) return false;

    D3D11_TEXTURE2D_DESC dd = {};
    dd.Width  = (UINT)width;
    dd.Height = (UINT)height;
    dd.MipLevels = 1;
    dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dd.SampleDesc.Count = 1;
    dd.Usage = D3D11_USAGE_DEFAULT;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(dev->CreateTexture2D(&dd, nullptr, &m_reflectionDepth))) return false;
    if (FAILED(dev->CreateDepthStencilView(m_reflectionDepth, nullptr, &m_reflectionDSV))) return false;

    return true;
}

void Renderer::ReleaseReflectionResources()
{
    ReleaseCOM(m_reflectionDSV);
    ReleaseCOM(m_reflectionDepth);
    ReleaseCOM(m_reflectionSRV);
    ReleaseCOM(m_reflectionRTV);
    ReleaseCOM(m_reflectionTex);
    m_reflectionWidth  = 0;
    m_reflectionHeight = 0;
}

bool Renderer::CreateSceneColorResources(int width, int height)
{
    ReleaseSceneColorResources();
    if (!m_device || width <= 0 || height <= 0) return false;
    ID3D11Device* dev = m_device->GetDevice();
    if (!dev) return false;

    // Match the swapchain backbuffer format so CopyResource is valid.
    D3D11_TEXTURE2D_DESC td = {};
    td.Width  = (UINT)width;
    td.Height = (UINT)height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &m_sceneColorTex))) return false;
    if (FAILED(dev->CreateRenderTargetView(m_sceneColorTex, nullptr, &m_sceneColorRTV))) return false;
    if (FAILED(dev->CreateShaderResourceView(m_sceneColorTex, nullptr, &m_sceneColorSRV))) return false;
    return true;
}

void Renderer::ReleaseSceneColorResources()
{
    ReleaseCOM(m_sceneColorSRV);
    ReleaseCOM(m_sceneColorRTV);
    ReleaseCOM(m_sceneColorTex);
}

bool Renderer::CreateDeferredResources(int width, int height)
{
    ReleaseDeferredResources();
    if (!m_device || width <= 0 || height <= 0) return false;

    ID3D11Device* dev = m_device->GetDevice();
    if (!dev) return false;

    const DXGI_FORMAT formats[kGBufferCount] =
    {
        DXGI_FORMAT_R8G8B8A8_UNORM,         // RT0: albedo + roughness
        DXGI_FORMAT_R16G16B16A16_FLOAT,     // RT1: normal (future)
        DXGI_FORMAT_R16G16_FLOAT,           // RT2: motion (future)
        DXGI_FORMAT_R8G8B8A8_UNORM          // RT3: misc (future)
    };

    for (int i = 0; i < kGBufferCount; ++i)
    {
        m_gbuffer[i].format = formats[i];

        D3D11_TEXTURE2D_DESC td = {};
        td.Width = (UINT)width;
        td.Height = (UINT)height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = formats[i];
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        if (FAILED(dev->CreateTexture2D(&td, nullptr, &m_gbuffer[i].texture)))
            return false;
        if (FAILED(dev->CreateRenderTargetView(m_gbuffer[i].texture, nullptr, &m_gbuffer[i].rtv)))
            return false;
        if (FAILED(dev->CreateShaderResourceView(m_gbuffer[i].texture, nullptr, &m_gbuffer[i].srv)))
            return false;
    }

    return true;
}

void Renderer::ReleaseDeferredResources()
{
    for (int i = 0; i < kGBufferCount; ++i)
    {
        ReleaseCOM(m_gbuffer[i].srv);
        ReleaseCOM(m_gbuffer[i].rtv);
        ReleaseCOM(m_gbuffer[i].texture);
        m_gbuffer[i].format = DXGI_FORMAT_UNKNOWN;
    }
}

bool Renderer::CreateLightingPipeline(const char* shaderPath)
{
    ReleaseLightingPipeline();
    if (!m_device || !shaderPath) return false;

    ID3D11Device* dev = m_device->GetDevice();
    if (!dev) return false;

    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    if (!CompileShaderFile(shaderPath, "VS_FullScreen", "vs_4_0", &vsBlob)) return false;
    if (!CompileShaderFile(shaderPath, "PS_Lighting", "ps_4_0", &psBlob))
    {
        if (vsBlob) vsBlob->Release();
        return false;
    }

    bool ok = true;
    if (FAILED(dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_fullscreenVS)))
        ok = false;
    if (ok && FAILED(dev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_lightingPS)))
        ok = false;

    if (vsBlob) vsBlob->Release();
    if (psBlob) psBlob->Release();
    if (!ok) return false;

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MinLOD = 0.0f;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(dev->CreateSamplerState(&sd, &m_linearClampSampler)))
        return false;

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = sizeof(LightingCB);
    cbd.Usage = D3D11_USAGE_DEFAULT;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(dev->CreateBuffer(&cbd, nullptr, &m_lightingCB)))
        return false;

    return true;
}

void Renderer::ReleaseLightingPipeline()
{
    ReleaseCOM(m_lightingCB);
    ReleaseCOM(m_linearClampSampler);
    ReleaseCOM(m_lightingPS);
    ReleaseCOM(m_fullscreenVS);
}
