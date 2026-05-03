#include "WaterRenderer.h"
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
    // Lightweight tileable value-fBM used to bake a 256x256 noise
    // texture once at init time. The grid wrap is doubled per octave
    // so the resulting texture seams perfectly under WRAP sampling.
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
        float total = 0.0f;
        float amp   = 1.0f;
        float freq  = 1.0f;
        float maxv  = 0.0f;
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

    struct WaterCB
    {
        float matVP[16];
        float matWorld[16];
        float invViewProj[16];
        float eyePos[4];
        float baseColor[4];
        float timeAndPad[4];
        float waveParams[4];     // x=amp, y=scrollSpeed
        float shallowColor[4];   // rgb, a unused
        float deepColor[4];      // rgb, a unused
        float depthParams[4];    // x=depthScale, y=invScreenW, z=invScreenH
        float foamColor[4];      // rgb, a unused
        float foamParams[4];     // x=threshold, y=speed, z=freq, w=strength
        float reflParams[4];     // x=strength, y=fresnelPower, z=distortion, w=enabled(0/1)
        float refrParams[4];     // x=refractStrength, y=causticStrength, z=causticScale, w=unused
    };
    static_assert((sizeof(WaterCB) % 16) == 0, "WaterCB must be 16-byte aligned");

    struct WaterVertex
    {
        float x, y, z;
        float u, v;
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
        if (slash != std::string::npos)
            name = name.substr(slash + 1);

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
                MessageBoxA(nullptr, (const char*)errors->GetBufferPointer(),
                            path, MB_OK);
                errors->Release();
            }
            return false;
        }
        if (errors) errors->Release();
        return true;
    }
}

WaterRenderer::WaterRenderer()
    : m_device(nullptr)
{
}

WaterRenderer::~WaterRenderer()
{
    Shutdown();
}

bool WaterRenderer::Init(DX11Device* device, const char* shaderPath)
{
    if (!device) return false;
    m_device = device;

    if (!CreatePipeline(shaderPath ? shaderPath : "water.hlsl"))
        return false;
    if (!CreatePlaneBuffers())
        return false;
    if (!CreateNoiseTexture())
        return false;
    return true;
}

void WaterRenderer::Shutdown()
{
    m_noiseSRV.Reset();
    m_noiseTex.Reset();
    m_linearWrap.Reset();
    m_depthSampler.Reset();
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

void WaterRenderer::SetMapExtents(float originX, float originZ, float mapW, float mapH)
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

void WaterRenderer::SetWaterY(float y)
{
    if (m_waterY == y) return;
    m_waterY = y;
    m_meshDirty = true;
}

void WaterRenderer::SetBaseColor(float r, float g, float b, float a)
{
    m_color[0] = r;
    m_color[1] = g;
    m_color[2] = b;
    m_color[3] = a;
}

void WaterRenderer::GetBaseColor(float* r, float* g, float* b, float* a) const
{
    if (r) *r = m_color[0];
    if (g) *g = m_color[1];
    if (b) *b = m_color[2];
    if (a) *a = m_color[3];
}

void WaterRenderer::SetDeepColor(float r, float g, float b)
{
    m_deep[0] = r;
    m_deep[1] = g;
    m_deep[2] = b;
}

void WaterRenderer::GetDeepColor(float* r, float* g, float* b) const
{
    if (r) *r = m_deep[0];
    if (g) *g = m_deep[1];
    if (b) *b = m_deep[2];
}

void WaterRenderer::SetFoamColor(float r, float g, float b)
{
    m_foam[0] = r;
    m_foam[1] = g;
    m_foam[2] = b;
}

void WaterRenderer::GetFoamColor(float* r, float* g, float* b) const
{
    if (r) *r = m_foam[0];
    if (g) *g = m_foam[1];
    if (b) *b = m_foam[2];
}

bool WaterRenderer::CreatePipeline(const char* shaderPath)
{
    ID3D11Device* dev = m_device->GetDevice();
    if (!dev) return false;

    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    if (!CompileShaderFile(shaderPath, "VS_Water", "vs_4_0", &vsBlob))
        return false;
    if (!CompileShaderFile(shaderPath, "PS_Water", "ps_4_0", &psBlob))
    {
        if (vsBlob) vsBlob->Release();
        return false;
    }

    HRESULT hr = dev->CreateVertexShader(vsBlob->GetBufferPointer(),
                                         vsBlob->GetBufferSize(),
                                         nullptr, m_vs.GetAddressOf());
    if (FAILED(hr)) { vsBlob->Release(); psBlob->Release(); return false; }

    hr = dev->CreatePixelShader(psBlob->GetBufferPointer(),
                                psBlob->GetBufferSize(),
                                nullptr, m_ps.GetAddressOf());
    if (FAILED(hr)) { vsBlob->Release(); psBlob->Release(); return false; }

    D3D11_INPUT_ELEMENT_DESC il[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    hr = dev->CreateInputLayout(il, _countof(il),
                                vsBlob->GetBufferPointer(),
                                vsBlob->GetBufferSize(),
                                m_layout.GetAddressOf());
    vsBlob->Release();
    psBlob->Release();
    if (FAILED(hr)) return false;

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = sizeof(WaterCB);
    cbd.Usage = D3D11_USAGE_DEFAULT;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(dev->CreateBuffer(&cbd, nullptr, m_cb.GetAddressOf())))
        return false;

    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable           = TRUE;
    bd.RenderTarget[0].SrcBlend              = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(dev->CreateBlendState(&bd, m_blend.GetAddressOf())))
        return false;

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;        // visible if camera dips below
    rd.FrontCounterClockwise = FALSE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(dev->CreateRasterizerState(&rd, m_rs.GetAddressOf())))
        return false;

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter         = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MinLOD         = 0.0f;
    sd.MaxLOD         = D3D11_FLOAT32_MAX;
    if (FAILED(dev->CreateSamplerState(&sd, m_depthSampler.GetAddressOf())))
        return false;

    D3D11_SAMPLER_DESC sw = {};
    sw.Filter         = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sw.AddressU       = D3D11_TEXTURE_ADDRESS_WRAP;
    sw.AddressV       = D3D11_TEXTURE_ADDRESS_WRAP;
    sw.AddressW       = D3D11_TEXTURE_ADDRESS_WRAP;
    sw.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sw.MinLOD         = 0.0f;
    sw.MaxLOD         = D3D11_FLOAT32_MAX;
    if (FAILED(dev->CreateSamplerState(&sw, m_linearWrap.GetAddressOf())))
        return false;

    return true;
}

bool WaterRenderer::CreatePlaneBuffers()
{
    ID3D11Device* dev = m_device->GetDevice();
    if (!dev) return false;

    // 4-vertex flat plane. RefreshPlaneVB re-uploads corner positions
    // whenever the map extents or water Y change.
    D3D11_BUFFER_DESC vbd = {};
    vbd.ByteWidth = sizeof(WaterVertex) * 4;
    vbd.Usage     = D3D11_USAGE_DEFAULT;
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    if (FAILED(dev->CreateBuffer(&vbd, nullptr, m_vb.GetAddressOf())))
        return false;

    // CW winding viewed from +Y. V0=-x-z, V1=+x-z, V2=+x+z, V3=-x+z.
    const uint16_t indices[6] = { 0, 3, 2, 0, 2, 1 };
    D3D11_BUFFER_DESC ibd = {};
    ibd.ByteWidth = sizeof(indices);
    ibd.Usage     = D3D11_USAGE_IMMUTABLE;
    ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA isd = { indices, 0, 0 };
    if (FAILED(dev->CreateBuffer(&ibd, &isd, m_ib.GetAddressOf())))
        return false;

    m_meshDirty = true;
    return true;
}

bool WaterRenderer::CreateNoiseTexture()
{
    ID3D11Device* dev = m_device->GetDevice();
    if (!dev) return false;

    const int      W        = 256;
    const int      H        = 256;
    const int      baseWrap = 4;     // 4 cells across the 256 px tile
    const uint32_t seed     = 1337u;

    std::vector<uint8_t> data((size_t)W * H);
    for (int y = 0; y < H; ++y)
    {
        for (int x = 0; x < W; ++x)
        {
            float u = (float)x / (float)W * (float)baseWrap;
            float v = (float)y / (float)H * (float)baseWrap;
            float n = NoiseFBM(u, v, baseWrap, seed);
            // Raw fBM (no contrast stretch) — natural cloud-like
            // distribution; the shader picks ranges via smoothstep.
            if (n < 0.0f) n = 0.0f;
            if (n > 1.0f) n = 1.0f;
            data[(size_t)y * W + x] = (uint8_t)(n * 255.0f);
        }
    }

    D3D11_TEXTURE2D_DESC td = {};
    td.Width     = W;
    td.Height    = H;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format    = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage     = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA sd = {};
    sd.pSysMem     = data.data();
    sd.SysMemPitch = (UINT)W;

    if (FAILED(dev->CreateTexture2D(&td, &sd, m_noiseTex.GetAddressOf())))
        return false;
    if (FAILED(dev->CreateShaderResourceView(m_noiseTex.Get(), nullptr, m_noiseSRV.GetAddressOf())))
        return false;
    return true;
}

void WaterRenderer::RefreshPlaneVB()
{
    if (!m_meshDirty || !m_vb) return;
    ID3D11DeviceContext* ctx = m_device->GetContext();
    if (!ctx) return;

    const float x0 = m_originX;
    const float z0 = m_originZ;
    const float x1 = m_originX + m_mapW;
    const float z1 = m_originZ + m_mapH;
    const float y  = m_waterY;

    WaterVertex verts[4] =
    {
        { x0, y, z0, 0.0f, 0.0f },
        { x1, y, z0, 1.0f, 0.0f },
        { x1, y, z1, 1.0f, 1.0f },
        { x0, y, z1, 0.0f, 1.0f },
    };
    ctx->UpdateSubresource(m_vb.Get(), 0, nullptr, verts, 0, 0);
    m_meshDirty = false;
}

void WaterRenderer::Render(const D3DXMATRIX& view, const D3DXMATRIX& proj,
                           const D3DXVECTOR3& eyePos,
                           ID3D11ShaderResourceView* sceneDepthSRV,
                           ID3D11ShaderResourceView* reflectionSRV,
                           ID3D11ShaderResourceView* sceneColorSRV)
{
    if (!m_enabled || !m_device || !m_vs || !m_ps) return;
    ID3D11DeviceContext* ctx = m_device->GetContext();
    if (!ctx) return;

    RefreshPlaneVB();

    WaterCB cb = {};
    D3DXMATRIX vp = view * proj;
    memcpy(cb.matVP, &vp, sizeof(cb.matVP));
    D3DXMATRIX world;
    D3DXMatrixIdentity(&world);
    memcpy(cb.matWorld, &world, sizeof(cb.matWorld));

    // invViewProj reconstructs floor world position from SceneDepth.
    DirectX::XMMATRIX vpX  = DirectX::XMLoadFloat4x4(reinterpret_cast<const DirectX::XMFLOAT4X4*>(&vp));
    DirectX::XMMATRIX inv  = DirectX::XMMatrixInverse(nullptr, vpX);
    D3DXMATRIX invVP;
    DirectX::XMStoreFloat4x4(reinterpret_cast<DirectX::XMFLOAT4X4*>(&invVP), inv);
    memcpy(cb.invViewProj, &invVP, sizeof(cb.invViewProj));

    cb.eyePos[0] = eyePos.x;
    cb.eyePos[1] = eyePos.y;
    cb.eyePos[2] = eyePos.z;
    cb.eyePos[3] = 0.0f;
    cb.baseColor[0] = m_color[0];
    cb.baseColor[1] = m_color[1];
    cb.baseColor[2] = m_color[2];
    cb.baseColor[3] = m_color[3];
    cb.timeAndPad[0] = m_time;
    cb.waveParams[0] = m_waveAmp;
    cb.waveParams[1] = m_scrollSpeed;
    cb.shallowColor[0] = m_color[0];
    cb.shallowColor[1] = m_color[1];
    cb.shallowColor[2] = m_color[2];
    cb.shallowColor[3] = 1.0f;
    cb.deepColor[0] = m_deep[0];
    cb.deepColor[1] = m_deep[1];
    cb.deepColor[2] = m_deep[2];
    cb.deepColor[3] = 1.0f;
    cb.depthParams[0] = m_depthScale;
    cb.depthParams[1] = (m_screenW > 0) ? (1.0f / (float)m_screenW) : 0.0f;
    cb.depthParams[2] = (m_screenH > 0) ? (1.0f / (float)m_screenH) : 0.0f;
    cb.depthParams[3] = 0.0f;
    cb.foamColor[0]   = m_foam[0];
    cb.foamColor[1]   = m_foam[1];
    cb.foamColor[2]   = m_foam[2];
    cb.foamColor[3]   = 1.0f;
    cb.foamParams[0]  = m_foamThreshold;
    cb.foamParams[1]  = m_foamSpeed;
    cb.foamParams[2]  = m_foamFreq;
    cb.foamParams[3]  = m_foamStrength;
    cb.reflParams[0]  = m_reflStrength;
    cb.reflParams[1]  = m_fresnelPower;
    cb.reflParams[2]  = m_reflDistort;
    cb.reflParams[3]  = (m_reflectionEnabled && reflectionSRV) ? 1.0f : 0.0f;
    cb.refrParams[0]  = sceneColorSRV ? m_refrStrength : 0.0f;
    cb.refrParams[1]  = m_causticStrength;
    cb.refrParams[2]  = m_causticScale;
    cb.refrParams[3]  = 0.0f;
    ctx->UpdateSubresource(m_cb.Get(), 0, nullptr, &cb, 0, 0);

    UINT stride = sizeof(WaterVertex);
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

    ID3D11ShaderResourceView* psSrvs[4] = { sceneDepthSRV, reflectionSRV, m_noiseSRV.Get(), sceneColorSRV };
    ctx->PSSetShaderResources(0, 4, psSrvs);
    ID3D11SamplerState* samps[2] = { m_depthSampler.Get(), m_linearWrap.Get() };
    ctx->PSSetSamplers(0, 2, samps);

    const float blendFactor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    ctx->OMSetBlendState(m_blend.Get(), blendFactor, 0xFFFFFFFF);
    ctx->RSSetState(m_rs.Get());

    ctx->DrawIndexed(6, 0, 0);

    // Restore defaults so following overlay passes don't inherit our state.
    ctx->OMSetBlendState(nullptr, blendFactor, 0xFFFFFFFF);
    ctx->RSSetState(nullptr);
    ID3D11ShaderResourceView* nullSRVs[4] = { nullptr, nullptr, nullptr, nullptr };
    ctx->PSSetShaderResources(0, 4, nullSRVs);
}
