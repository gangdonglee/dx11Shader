#include "ShadowMap.h"
#include "DX11Device.h"
#include "DirectionalLight.h"
#include "imgui.h"
#include <cmath>

using Microsoft::WRL::ComPtr;

ShadowMap::ShadowMap() : m_device(nullptr), m_size(0)
{
    D3DXMatrixIdentity(&m_lightVP);
}

ShadowMap::~ShadowMap() { Shutdown(); }

bool ShadowMap::Init(DX11Device* device, int size)
{
    m_device = device;
    m_size = size;
    if (!m_device || !m_device->GetDevice()) return false;

    ID3D11Device* dev = m_device->GetDevice();

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT)size; td.Height = (UINT)size;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32_TYPELESS;     // typeless so we can DSV(D32) + SRV(R32)
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &m_tex))) return false;

    D3D11_DEPTH_STENCIL_VIEW_DESC dvd = {};
    dvd.Format = DXGI_FORMAT_D32_FLOAT;
    dvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    if (FAILED(dev->CreateDepthStencilView(m_tex.Get(), &dvd, &m_dsv))) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = DXGI_FORMAT_R32_FLOAT;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    if (FAILED(dev->CreateShaderResourceView(m_tex.Get(), &sd, &m_srv))) return false;

    D3D11_RASTERIZER_DESC rsd = {};
    rsd.FillMode = D3D11_FILL_SOLID;
    rsd.CullMode = D3D11_CULL_BACK;
    rsd.DepthClipEnable = TRUE;
    rsd.DepthBias = 16;                      // hardware depth bias to fight acne
    rsd.SlopeScaledDepthBias = 1.5f;
    if (FAILED(dev->CreateRasterizerState(&rsd, &m_rs))) return false;

    D3D11_DEPTH_STENCIL_DESC dsd = {};
    dsd.DepthEnable = TRUE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsd.DepthFunc = D3D11_COMPARISON_LESS;
    if (FAILED(dev->CreateDepthStencilState(&dsd, &m_ds))) return false;

    return true;
}

void ShadowMap::Shutdown()
{
    m_tex.Reset(); m_dsv.Reset(); m_srv.Reset();
    m_rs.Reset();  m_ds.Reset();
}

void ShadowMap::BeginPass(const DirectionalLight& sun)
{
    ID3D11DeviceContext* ctx = m_device->GetContext();

    // Build light view-proj. Sun direction points INTO the scene; the
    // light "eye" sits opposite, far enough back that the ortho frustum
    // captures the whole sphere of radius m_sceneRadius around origin.
    D3DXVECTOR3 dir = sun.GetDirection();
    float dlen = std::sqrt(dir.x*dir.x + dir.y*dir.y + dir.z*dir.z);
    if (dlen > 1e-4f) { dir = dir * (1.0f / dlen); }
    D3DXVECTOR3 target(0.0f, 0.0f, 0.0f);
    D3DXVECTOR3 lightPos = target - dir * (m_sceneRadius * 2.5f);
    D3DXVECTOR3 up = (std::fabs(dir.y) < 0.99f) ? D3DXVECTOR3(0,1,0) : D3DXVECTOR3(0,0,1);

    D3DXMATRIX view, proj;
    D3DXMatrixLookAtLH(&view, &lightPos, &target, &up);
    D3DXMatrixOrthoLH (&proj, 2.0f * m_sceneRadius, 2.0f * m_sceneRadius,
                       0.1f, 5.0f * m_sceneRadius);
    m_lightVP = view * proj;

    // Save current viewport, then apply shadow viewport.
    UINT n = 1;
    ctx->RSGetViewports(&n, &m_savedVP);

    D3D11_VIEWPORT vp = {};
    vp.Width = (float)m_size;
    vp.Height = (float)m_size;
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);

    // No color target — depth only.
    ID3D11RenderTargetView* nullRTV = nullptr;
    ctx->OMSetRenderTargets(1, &nullRTV, m_dsv.Get());
    ctx->ClearDepthStencilView(m_dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

    ctx->RSSetState(m_rs.Get());
    ctx->OMSetDepthStencilState(m_ds.Get(), 0);
}

void ShadowMap::EndPass()
{
    ID3D11DeviceContext* ctx = m_device->GetContext();
    ctx->RSSetViewports(1, &m_savedVP);

    // Caller is responsible for re-binding the main scene RT/DSV.
}

void ShadowMap::GuiPanel()
{
    if (!ImGui::CollapsingHeader("Shadow Map", ImGuiTreeNodeFlags_DefaultOpen))
        return;
    ImGui::Checkbox("Enabled##shadow", &m_enabled);
    ImGui::SliderFloat("Scene radius", &m_sceneRadius, 5.0f, 100.0f);
    ImGui::SliderFloat("Depth bias",   &m_depthBias,   0.0f, 0.005f, "%.5f");
    ImGui::SliderFloat("Normal bias",  &m_normalBias,  0.0f, 0.5f);
}
