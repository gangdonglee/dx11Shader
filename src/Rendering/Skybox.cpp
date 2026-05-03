#include "Skybox.h"
#include "DX11Device.h"
#include "DirectionalLight.h"
#include "imgui.h"
#include <d3dcompiler.h>
#include <string>
#include <cmath>

#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

namespace
{
    struct BakeCB
    {
        float FaceIdx[4];      // x = face index 0..5
        float LightDir[4];
        float LightColor[4];
        float Horizon[4];
        float Zenith[4];
        float Ground[4];
    };

    struct DrawCB
    {
        float InvViewProj[16];
        float EyePos[4];
    };
    static_assert((sizeof(BakeCB) % 16) == 0, "BakeCB align");
    static_assert((sizeof(DrawCB) % 16) == 0, "DrawCB align");

    static std::wstring ToWidePath(const char* p)
    {
        int n = MultiByteToWideChar(CP_UTF8, 0, p, -1, nullptr, 0);
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, p, -1, &w[0], n);
        if (!w.empty() && w.back() == L'\0') w.pop_back();
        return w;
    }

    static bool CompileFile(const char* path, const char* entry, const char* tgt, ID3DBlob** blob)
    {
        ID3DBlob* errs = nullptr;
        UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
        std::string name = path;
        size_t s = name.find_last_of("\\/");
        if (s != std::string::npos) name = name.substr(s + 1);
        const std::string cands[] = {
            std::string(path), name,
            "assets\\Shaders\\" + name,
            "..\\assets\\Shaders\\" + name,
            "..\\..\\assets\\Shaders\\" + name
        };
        HRESULT hr = E_FAIL;
        for (const auto& c : cands)
        {
            std::wstring w = ToWidePath(c.c_str());
            hr = D3DCompileFromFile(w.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                    entry, tgt, flags, 0, blob, &errs);
            if (SUCCEEDED(hr)) break;
            if (errs) { errs->Release(); errs = nullptr; }
        }
        if (FAILED(hr))
        {
            if (errs) {
                printf("[Skybox] Compile error %s/%s: %s\n", path, entry, (const char*)errs->GetBufferPointer());
                errs->Release();
            } else {
                printf("[Skybox] Compile failed %s/%s\n", path, entry);
            }
            return false;
        }
        if (errs) errs->Release();
        return true;
    }

    static void MatrixToFloats(const D3DXMATRIX& m, float out[16])
    {
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                out[r * 4 + c] = m.m[c][r];
    }
}

Skybox::Skybox() : m_device(nullptr) {}
Skybox::~Skybox() { Shutdown(); }

bool Skybox::Init(DX11Device* dev, const char* bakeShader, const char* drawShader, int faceSize)
{
    m_device = dev;
    m_faceSize = faceSize;
    if (!m_device || !m_device->GetDevice()) return false;
    if (!CreatePipeline(bakeShader, drawShader)) return false;
    if (!CreateCubemap(faceSize))                return false;
    return true;
}

void Skybox::Shutdown()
{
    for (auto& r : m_faceRTV) r.Reset();
    m_cubeSRV.Reset(); m_cubeTex.Reset();
    m_bakeVS.Reset(); m_bakePS.Reset();
    m_drawVS.Reset(); m_drawPS.Reset();
    m_cb.Reset(); m_rs.Reset(); m_dsOff.Reset(); m_blendNone.Reset(); m_sampLinear.Reset();
}

bool Skybox::CreatePipeline(const char* bakeShader, const char* drawShader)
{
    ID3D11Device* dev = m_device->GetDevice();

    ComPtr<ID3DBlob> bvs, bps, dvs, dps;
    if (!CompileFile(bakeShader, "BakeVS", "vs_5_0", &bvs)) return false;
    if (!CompileFile(bakeShader, "BakePS", "ps_5_0", &bps)) return false;
    if (!CompileFile(drawShader, "SkyVS",  "vs_5_0", &dvs)) return false;
    if (!CompileFile(drawShader, "SkyPS",  "ps_5_0", &dps)) return false;

    if (FAILED(dev->CreateVertexShader(bvs->GetBufferPointer(), bvs->GetBufferSize(), nullptr, &m_bakeVS))) return false;
    if (FAILED(dev->CreatePixelShader (bps->GetBufferPointer(), bps->GetBufferSize(), nullptr, &m_bakePS))) return false;
    if (FAILED(dev->CreateVertexShader(dvs->GetBufferPointer(), dvs->GetBufferSize(), nullptr, &m_drawVS))) return false;
    if (FAILED(dev->CreatePixelShader (dps->GetBufferPointer(), dps->GetBufferSize(), nullptr, &m_drawPS))) return false;

    // Single CB sized to the larger of the two structs so we can bind the
    // same buffer for both passes.
    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = (sizeof(BakeCB) > sizeof(DrawCB)) ? sizeof(BakeCB) : sizeof(DrawCB);
    if (cbd.ByteWidth < 16) cbd.ByteWidth = 16;
    cbd.ByteWidth = ((cbd.ByteWidth + 15) & ~15);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev->CreateBuffer(&cbd, nullptr, &m_cb))) return false;

    D3D11_RASTERIZER_DESC rs = {};
    rs.FillMode = D3D11_FILL_SOLID;
    rs.CullMode = D3D11_CULL_NONE;
    rs.DepthClipEnable = TRUE;
    if (FAILED(dev->CreateRasterizerState(&rs, &m_rs))) return false;

    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = FALSE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    if (FAILED(dev->CreateDepthStencilState(&dd, &m_dsOff))) return false;

    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable = FALSE;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(dev->CreateBlendState(&bd, &m_blendNone))) return false;

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MinLOD = 0; sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(dev->CreateSamplerState(&sd, &m_sampLinear))) return false;

    return true;
}

bool Skybox::CreateCubemap(int faceSize)
{
    ID3D11Device* dev = m_device->GetDevice();

    // HDR cubemap with a full mip chain. GenerateMips() runs after each
    // bake to fill levels 1..N as a box-filtered approximation of GGX
    // prefiltering — water samples a higher LOD when its roughness is up.
    D3D11_TEXTURE2D_DESC td = {};
    td.Width  = (UINT)faceSize;
    td.Height = (UINT)faceSize;
    td.MipLevels = 0;             // 0 = full chain auto-allocated
    td.ArraySize = 6;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE | D3D11_RESOURCE_MISC_GENERATE_MIPS;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &m_cubeTex))) return false;

    // RTVs for the top mip (mip 0) only — GenerateMips fills the rest.
    for (int f = 0; f < 6; ++f)
    {
        D3D11_RENDER_TARGET_VIEW_DESC rd = {};
        rd.Format = td.Format;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
        rd.Texture2DArray.MipSlice = 0;
        rd.Texture2DArray.FirstArraySlice = (UINT)f;
        rd.Texture2DArray.ArraySize = 1;
        if (FAILED(dev->CreateRenderTargetView(m_cubeTex.Get(), &rd, &m_faceRTV[f]))) return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = td.Format;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
    sd.TextureCube.MostDetailedMip = 0;
    sd.TextureCube.MipLevels       = (UINT)-1;   // all mips
    if (FAILED(dev->CreateShaderResourceView(m_cubeTex.Get(), &sd, &m_cubeSRV))) return false;

    return true;
}

void Skybox::Update(const DirectionalLight& sun)
{
    if (!m_dirty && sun.GetVersion() == m_lastBakedVersion) return;
    BakeCubemap(sun);
    m_lastBakedVersion = sun.GetVersion();
    m_dirty = false;
}

void Skybox::BakeCubemap(const DirectionalLight& sun)
{
    ID3D11DeviceContext* ctx = m_device->GetContext();

    // Save viewport and restore at the end — the rest of the frame still
    // wants the backbuffer-sized one.
    UINT vpCount = 1;
    D3D11_VIEWPORT savedVP;
    ctx->RSGetViewports(&vpCount, &savedVP);

    D3D11_VIEWPORT vp = {};
    vp.Width = (float)m_faceSize;
    vp.Height = (float)m_faceSize;
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);

    ctx->RSSetState(m_rs.Get());
    ctx->OMSetDepthStencilState(m_dsOff.Get(), 0);
    float bf[4] = { 0,0,0,0 };
    ctx->OMSetBlendState(m_blendNone.Get(), bf, 0xffffffff);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11Buffer* nullVB = nullptr; UINT zero = 0;
    ctx->IASetVertexBuffers(0, 1, &nullVB, &zero, &zero);

    ctx->VSSetShader(m_bakeVS.Get(), nullptr, 0);
    ctx->PSSetShader(m_bakePS.Get(), nullptr, 0);
    ID3D11Buffer* cbs[1] = { m_cb.Get() };
    ctx->VSSetConstantBuffers(0, 1, cbs);
    ctx->PSSetConstantBuffers(0, 1, cbs);

    D3DXVECTOR3 dir = sun.GetDirection();
    const float* col = sun.GetColor();
    float intensity  = sun.GetIntensity();

    for (int f = 0; f < 6; ++f)
    {
        BakeCB cb = {};
        cb.FaceIdx[0] = (float)f;
        cb.LightDir[0] = dir.x; cb.LightDir[1] = dir.y; cb.LightDir[2] = dir.z;
        cb.LightColor[0] = col[0] * intensity;
        cb.LightColor[1] = col[1] * intensity;
        cb.LightColor[2] = col[2] * intensity;
        cb.Horizon[0] = m_horizon[0]; cb.Horizon[1] = m_horizon[1]; cb.Horizon[2] = m_horizon[2];
        cb.Zenith [0] = m_zenith [0]; cb.Zenith [1] = m_zenith [1]; cb.Zenith [2] = m_zenith [2];
        cb.Ground [0] = m_ground [0]; cb.Ground [1] = m_ground [1]; cb.Ground [2] = m_ground [2];

        D3D11_MAPPED_SUBRESOURCE map;
        if (SUCCEEDED(ctx->Map(m_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map)))
        {
            memcpy(map.pData, &cb, sizeof(cb));
            ctx->Unmap(m_cb.Get(), 0);
        }

        ID3D11RenderTargetView* rtv = m_faceRTV[f].Get();
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->Draw(3, 0);
    }

    // Unbind and restore viewport so the caller can keep going.
    ID3D11RenderTargetView* nullRTV = nullptr;
    ctx->OMSetRenderTargets(1, &nullRTV, nullptr);
    ctx->RSSetViewports(1, &savedVP);

    // Fill the rest of the mip chain (box-filter approximation of GGX
    // pre-filtering). Higher mips = blurrier reflection = rougher water.
    ctx->GenerateMips(m_cubeSRV.Get());
}

void Skybox::Render(const D3DXMATRIX& invViewProj, const D3DXVECTOR3& eyePos)
{
    ID3D11DeviceContext* ctx = m_device->GetContext();

    DrawCB cb = {};
    MatrixToFloats(invViewProj, cb.InvViewProj);
    cb.EyePos[0] = eyePos.x; cb.EyePos[1] = eyePos.y; cb.EyePos[2] = eyePos.z; cb.EyePos[3] = 1.0f;

    D3D11_MAPPED_SUBRESOURCE map;
    if (SUCCEEDED(ctx->Map(m_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map)))
    {
        memcpy(map.pData, &cb, sizeof(cb));
        ctx->Unmap(m_cb.Get(), 0);
    }

    ctx->RSSetState(m_rs.Get());
    ctx->OMSetDepthStencilState(m_dsOff.Get(), 0);
    float bf[4] = { 0,0,0,0 };
    ctx->OMSetBlendState(m_blendNone.Get(), bf, 0xffffffff);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11Buffer* nullVB = nullptr; UINT zero = 0;
    ctx->IASetVertexBuffers(0, 1, &nullVB, &zero, &zero);

    ctx->VSSetShader(m_drawVS.Get(), nullptr, 0);
    ctx->PSSetShader(m_drawPS.Get(), nullptr, 0);
    ID3D11Buffer* cbs[1] = { m_cb.Get() };
    ctx->VSSetConstantBuffers(0, 1, cbs);
    ctx->PSSetConstantBuffers(0, 1, cbs);

    ID3D11ShaderResourceView* srvs[1] = { m_cubeSRV.Get() };
    ctx->PSSetShaderResources(0, 1, srvs);
    ID3D11SamplerState* samps[1] = { m_sampLinear.Get() };
    ctx->PSSetSamplers(0, 1, samps);

    ctx->Draw(3, 0);

    ID3D11ShaderResourceView* nullSRV = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSRV);
}

void Skybox::GuiPanel()
{
    if (!ImGui::CollapsingHeader("Skybox", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    if (ImGui::ColorEdit3("Horizon",   m_horizon)) m_dirty = true;
    if (ImGui::ColorEdit3("Zenith",    m_zenith))  m_dirty = true;
    if (ImGui::ColorEdit3("Ground",    m_ground))  m_dirty = true;
    if (ImGui::Button("Re-bake skybox")) m_dirty = true;
}
