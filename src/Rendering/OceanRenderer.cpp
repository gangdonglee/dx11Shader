#include "OceanRenderer.h"
#include "DX11Device.h"
#include <d3dcompiler.h>
#include <string>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>

#pragma comment(lib, "d3dcompiler.lib")

namespace
{
    // Same value-fBM noise generator as WaterRenderer — kept local so
    // the two renderers stay structurally independent.
    static uint32_t NoiseHashU(uint32_t x, uint32_t y, uint32_t seed)
    {
        uint32_t h = x * 374761393u + y * 668265263u + seed * 1013904223u;
        h = (h ^ (h >> 13)) * 1274126177u;
        return h ^ (h >> 16);
    }
    static float NoiseHash01(int x, int y, int wrap, uint32_t seed)
    {
        x = ((x % wrap) + wrap) % wrap;
        y = ((y % wrap) + wrap) % wrap;
        return (NoiseHashU((uint32_t)x, (uint32_t)y, seed) & 0xffff) / 65535.0f;
    }
    static float NoiseSmooth(float t) { return t * t * (3.0f - 2.0f * t); }
    static float ValueNoise2D(float u, float v, int wrap, uint32_t seed)
    {
        float fxFloor = std::floor(u);
        float fyFloor = std::floor(v);
        int   x0 = (int)fxFloor;
        int   y0 = (int)fyFloor;
        float sx = NoiseSmooth(u - fxFloor);
        float sy = NoiseSmooth(v - fyFloor);
        float a = NoiseHash01(x0,     y0,     wrap, seed);
        float b = NoiseHash01(x0 + 1, y0,     wrap, seed);
        float c = NoiseHash01(x0,     y0 + 1, wrap, seed);
        float d = NoiseHash01(x0 + 1, y0 + 1, wrap, seed);
        float ab = a * (1.0f - sx) + b * sx;
        float cd = c * (1.0f - sx) + d * sx;
        return ab * (1.0f - sy) + cd * sy;
    }
    static float NoiseFBM(float u, float v, int baseWrap, uint32_t seed)
    {
        float total = 0.0f, amp = 1.0f, freq = 1.0f, maxv = 0.0f;
        for (int i = 0; i < 4; ++i)
        {
            int wrap = baseWrap << i;
            total += ValueNoise2D(u * freq, v * freq, wrap, seed + (uint32_t)i) * amp;
            maxv  += amp;
            amp   *= 0.5f;
            freq  *= 2.0f;
        }
        return total / maxv;
    }

    struct OceanCB
    {
        float matVP[16];
        float matWorld[16];
        float eyePos[4];
        float timeAndPad[4];
        float waveParams[4];
        float shallowColor[4];
        float deepColor[4];
        float skyHorizon[4];
        float skyZenith[4];
        float whitecapColor[4];
        float streakParams[4];
        float reflParams[4];
    };
    static_assert((sizeof(OceanCB) % 16) == 0, "OceanCB must be 16-byte aligned");

    struct OceanVertex
    {
        float x, y, z;
        float u, v;
    };

    static std::wstring ToWidePath(const char* path)
    {
        int count = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
        std::wstring wide(count, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, path, -1, &wide[0], count);
        if (!wide.empty() && wide.back() == L'\0') wide.pop_back();
        return wide;
    }

    static bool CompileShaderFile(const char* path, const char* entry,
                                  const char* target, ID3DBlob** blob)
    {
        if (!path || !entry || !target || !blob) return false;
        ID3DBlob* errors = nullptr;
        UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
        std::string name = path;
        size_t slash = name.find_last_of("\\/");
        if (slash != std::string::npos) name = name.substr(slash + 1);
        const std::string candidates[] =
        {
            std::string(path),
            name,
            "assets\\Shaders\\" + name,
            "..\\assets\\Shaders\\" + name,
            "..\\..\\assets\\Shaders\\" + name
        };
        HRESULT hr = E_FAIL;
        for (const std::string& resolved : candidates)
        {
            std::wstring wide = ToWidePath(resolved.c_str());
            hr = D3DCompileFromFile(wide.c_str(), nullptr,
                                    D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                    entry, target, flags, 0, blob, &errors);
            if (SUCCEEDED(hr)) break;
            if (errors) { errors->Release(); errors = nullptr; }
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

OceanRenderer::OceanRenderer() : m_device(nullptr) {}
OceanRenderer::~OceanRenderer() { Shutdown(); }

bool OceanRenderer::Init(DX11Device* device, const char* shaderPath)
{
    if (!device) return false;
    m_device = device;
    if (!CreatePipeline(shaderPath ? shaderPath : "ocean.hlsl"))
        return false;
    if (!CreatePlaneBuffers())
        return false;
    if (!CreateNoiseTexture())
        return false;
    return true;
}

void OceanRenderer::Shutdown()
{
    m_noiseSRV.Reset();
    m_noiseTex.Reset();
    m_linearWrap.Reset();
    m_pointClamp.Reset();
    m_rs.Reset();
    m_blend.Reset();
    m_cb.Reset();
    m_ib.Reset();
    m_vb.Reset();
    m_layout.Reset();
    m_ps.Reset();
    m_vs.Reset();
    m_device = nullptr;
}

void OceanRenderer::SetMapExtents(float originX, float originZ, float mapW, float mapH)
{
    if (m_originX == originX && m_originZ == originZ &&
        m_mapW    == mapW    && m_mapH    == mapH)
        return;
    m_originX = originX;
    m_originZ = originZ;
    m_mapW    = mapW;
    m_mapH    = mapH;
    m_meshDirty = true;
}

void OceanRenderer::SetMeshMultiplier(float m)
{
    if (m_meshMul == m) return;
    m_meshMul = m;
    m_meshDirty = true;
}

void OceanRenderer::SetOceanY(float y)
{
    if (m_oceanY == y) return;
    m_oceanY = y;
    m_meshDirty = true;
}

void OceanRenderer::SetShallowColor(float r, float g, float b) { m_shallow[0]=r; m_shallow[1]=g; m_shallow[2]=b; }
void OceanRenderer::GetShallowColor(float* r, float* g, float* b) const { if(r)*r=m_shallow[0]; if(g)*g=m_shallow[1]; if(b)*b=m_shallow[2]; }
void OceanRenderer::SetDeepColor(float r, float g, float b)    { m_deep[0]=r; m_deep[1]=g; m_deep[2]=b; }
void OceanRenderer::GetDeepColor(float* r, float* g, float* b) const    { if(r)*r=m_deep[0]; if(g)*g=m_deep[1]; if(b)*b=m_deep[2]; }
void OceanRenderer::SetSkyHorizonColor(float r, float g, float b) { m_skyHorizon[0]=r; m_skyHorizon[1]=g; m_skyHorizon[2]=b; }
void OceanRenderer::GetSkyHorizonColor(float* r, float* g, float* b) const { if(r)*r=m_skyHorizon[0]; if(g)*g=m_skyHorizon[1]; if(b)*b=m_skyHorizon[2]; }
void OceanRenderer::SetSkyZenithColor(float r, float g, float b)  { m_skyZenith[0]=r; m_skyZenith[1]=g; m_skyZenith[2]=b; }
void OceanRenderer::GetSkyZenithColor(float* r, float* g, float* b) const  { if(r)*r=m_skyZenith[0]; if(g)*g=m_skyZenith[1]; if(b)*b=m_skyZenith[2]; }
void OceanRenderer::SetWhitecapColor(float r, float g, float b)   { m_whitecap[0]=r; m_whitecap[1]=g; m_whitecap[2]=b; }
void OceanRenderer::GetWhitecapColor(float* r, float* g, float* b) const   { if(r)*r=m_whitecap[0]; if(g)*g=m_whitecap[1]; if(b)*b=m_whitecap[2]; }

bool OceanRenderer::CreatePipeline(const char* shaderPath)
{
    ID3D11Device* dev = m_device->GetDevice();
    if (!dev) return false;

    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    if (!CompileShaderFile(shaderPath, "VS_Ocean", "vs_4_0", &vsBlob)) return false;
    if (!CompileShaderFile(shaderPath, "PS_Ocean", "ps_4_0", &psBlob))
    {
        if (vsBlob) vsBlob->Release();
        return false;
    }

    HRESULT hr = dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, m_vs.GetAddressOf());
    if (FAILED(hr)) { vsBlob->Release(); psBlob->Release(); return false; }
    hr = dev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, m_ps.GetAddressOf());
    if (FAILED(hr)) { vsBlob->Release(); psBlob->Release(); return false; }

    D3D11_INPUT_ELEMENT_DESC il[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    hr = dev->CreateInputLayout(il, _countof(il), vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), m_layout.GetAddressOf());
    vsBlob->Release(); psBlob->Release();
    if (FAILED(hr)) return false;

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = sizeof(OceanCB);
    cbd.Usage = D3D11_USAGE_DEFAULT;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(dev->CreateBuffer(&cbd, nullptr, m_cb.GetAddressOf()))) return false;

    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable           = TRUE;
    bd.RenderTarget[0].SrcBlend              = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(dev->CreateBlendState(&bd, m_blend.GetAddressOf()))) return false;

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.FrontCounterClockwise = FALSE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(dev->CreateRasterizerState(&rd, m_rs.GetAddressOf()))) return false;

    D3D11_SAMPLER_DESC sp = {};
    sp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sp.AddressU = sp.AddressV = sp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sp.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sp.MinLOD = 0.0f; sp.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(dev->CreateSamplerState(&sp, m_pointClamp.GetAddressOf()))) return false;

    D3D11_SAMPLER_DESC sw = {};
    sw.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sw.AddressU = sw.AddressV = sw.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sw.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sw.MinLOD = 0.0f; sw.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(dev->CreateSamplerState(&sw, m_linearWrap.GetAddressOf()))) return false;

    return true;
}

bool OceanRenderer::CreatePlaneBuffers()
{
    ID3D11Device* dev = m_device->GetDevice();
    if (!dev) return false;

    D3D11_BUFFER_DESC vbd = {};
    vbd.ByteWidth = sizeof(OceanVertex) * 4;
    vbd.Usage     = D3D11_USAGE_DEFAULT;
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    if (FAILED(dev->CreateBuffer(&vbd, nullptr, m_vb.GetAddressOf()))) return false;

    const uint16_t indices[6] = { 0, 3, 2, 0, 2, 1 };
    D3D11_BUFFER_DESC ibd = {};
    ibd.ByteWidth = sizeof(indices);
    ibd.Usage     = D3D11_USAGE_IMMUTABLE;
    ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA isd = { indices, 0, 0 };
    if (FAILED(dev->CreateBuffer(&ibd, &isd, m_ib.GetAddressOf()))) return false;

    m_meshDirty = true;
    return true;
}

bool OceanRenderer::CreateNoiseTexture()
{
    ID3D11Device* dev = m_device->GetDevice();
    if (!dev) return false;

    const int W = 256, H = 256, baseWrap = 4;
    const uint32_t seed = 4242u;
    std::vector<uint8_t> data((size_t)W * H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
        {
            float u = (float)x / (float)W * (float)baseWrap;
            float v = (float)y / (float)H * (float)baseWrap;
            float n = NoiseFBM(u, v, baseWrap, seed);
            if (n < 0.0f) n = 0.0f;
            if (n > 1.0f) n = 1.0f;
            data[(size_t)y * W + x] = (uint8_t)(n * 255.0f);
        }

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = W; td.Height = H;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd = { data.data(), (UINT)W, 0 };
    if (FAILED(dev->CreateTexture2D(&td, &sd, m_noiseTex.GetAddressOf()))) return false;
    if (FAILED(dev->CreateShaderResourceView(m_noiseTex.Get(), nullptr, m_noiseSRV.GetAddressOf()))) return false;
    return true;
}

void OceanRenderer::RefreshPlaneVB()
{
    if (!m_meshDirty || !m_vb) return;
    ID3D11DeviceContext* ctx = m_device->GetContext();
    if (!ctx) return;

    const float cx = m_originX + m_mapW * 0.5f;
    const float cz = m_originZ + m_mapH * 0.5f;
    const float halfW = m_mapW * m_meshMul * 0.5f;
    const float halfH = m_mapH * m_meshMul * 0.5f;
    const float y = m_oceanY;

    OceanVertex verts[4] =
    {
        { cx - halfW, y, cz - halfH, 0.0f, 0.0f },
        { cx + halfW, y, cz - halfH, 1.0f, 0.0f },
        { cx + halfW, y, cz + halfH, 1.0f, 1.0f },
        { cx - halfW, y, cz + halfH, 0.0f, 1.0f },
    };
    ctx->UpdateSubresource(m_vb.Get(), 0, nullptr, verts, 0, 0);
    m_meshDirty = false;
}

void OceanRenderer::Render(const D3DXMATRIX& view, const D3DXMATRIX& proj,
                           const D3DXVECTOR3& eyePos,
                           ID3D11ShaderResourceView* reflectionSRV)
{
    if (!m_enabled || !m_device || !m_vs || !m_ps) return;
    ID3D11DeviceContext* ctx = m_device->GetContext();
    if (!ctx) return;

    RefreshPlaneVB();

    OceanCB cb = {};
    D3DXMATRIX vp = view * proj;
    memcpy(cb.matVP, &vp, sizeof(cb.matVP));
    D3DXMATRIX world; D3DXMatrixIdentity(&world);
    memcpy(cb.matWorld, &world, sizeof(cb.matWorld));

    cb.eyePos[0] = eyePos.x; cb.eyePos[1] = eyePos.y; cb.eyePos[2] = eyePos.z; cb.eyePos[3] = 0.0f;
    cb.timeAndPad[0] = m_time;
    cb.waveParams[0] = m_swellAmp;
    cb.waveParams[1] = m_scrollSpeed;
    cb.shallowColor[0] = m_shallow[0];      cb.shallowColor[1] = m_shallow[1];      cb.shallowColor[2] = m_shallow[2];      cb.shallowColor[3] = 1.0f;
    cb.deepColor[0]    = m_deep[0];         cb.deepColor[1]    = m_deep[1];         cb.deepColor[2]    = m_deep[2];         cb.deepColor[3]    = 1.0f;
    cb.skyHorizon[0]   = m_skyHorizon[0];   cb.skyHorizon[1]   = m_skyHorizon[1];   cb.skyHorizon[2]   = m_skyHorizon[2];   cb.skyHorizon[3]   = 1.0f;
    cb.skyZenith[0]    = m_skyZenith[0];    cb.skyZenith[1]    = m_skyZenith[1];    cb.skyZenith[2]    = m_skyZenith[2];    cb.skyZenith[3]    = 1.0f;
    cb.whitecapColor[0] = m_whitecap[0];    cb.whitecapColor[1] = m_whitecap[1];    cb.whitecapColor[2] = m_whitecap[2];    cb.whitecapColor[3] = m_whitecapStrength;
    cb.streakParams[0] = m_streakStrength;  cb.streakParams[1] = m_streakStretch;   cb.streakParams[2] = m_streakScroll;    cb.streakParams[3] = m_streakEnabled ? 1.0f : 0.0f;
    cb.reflParams[0]   = m_reflStrength;    cb.reflParams[1]   = m_fresnelPower;    cb.reflParams[2]   = m_reflDistort;     cb.reflParams[3]   = (m_reflectionEnabled && reflectionSRV) ? 1.0f : 0.0f;

    ctx->UpdateSubresource(m_cb.Get(), 0, nullptr, &cb, 0, 0);

    UINT stride = sizeof(OceanVertex);
    UINT offset = 0;
    ID3D11Buffer* vb = m_vb.Get();
    ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    ctx->IASetIndexBuffer(m_ib.Get(), DXGI_FORMAT_R16_UINT, 0);
    ctx->IASetInputLayout(m_layout.Get());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ctx->VSSetShader(m_vs.Get(), nullptr, 0);
    ctx->PSSetShader(m_ps.Get(), nullptr, 0);
    ID3D11Buffer* cbBuf = m_cb.Get();
    ctx->VSSetConstantBuffers(0, 1, &cbBuf);
    ctx->PSSetConstantBuffers(0, 1, &cbBuf);

    // t1 = reflection RT, t2 = our local noise. (t0 unused — slot kept
    // free so future depth-aware effects can plug in.)
    ID3D11ShaderResourceView* psSrvs[3] = { nullptr, reflectionSRV, m_noiseSRV.Get() };
    ctx->PSSetShaderResources(0, 3, psSrvs);
    ID3D11SamplerState* samps[2] = { m_pointClamp.Get(), m_linearWrap.Get() };
    ctx->PSSetSamplers(0, 2, samps);

    const float blendFactor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    ctx->OMSetBlendState(m_blend.Get(), blendFactor, 0xFFFFFFFF);
    ctx->RSSetState(m_rs.Get());

    ctx->DrawIndexed(6, 0, 0);

    ctx->OMSetBlendState(nullptr, blendFactor, 0xFFFFFFFF);
    ctx->RSSetState(nullptr);
    ID3D11ShaderResourceView* nullSRVs[3] = { nullptr, nullptr, nullptr };
    ctx->PSSetShaderResources(0, 3, nullSRVs);
}
