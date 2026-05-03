#include "Water.h"
#include "DX11Device.h"
#include "imgui.h"
#include <d3dcompiler.h>
#include <string>
#include <cmath>

#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

namespace
{
    struct WaterCB
    {
        float World[16];
        float ViewProj[16];
        float InvViewProj[16];
        float EyePos[3];     float Time;
        float LightDir[3];   float Pad0;
        float LightColor[3]; float SkyTint;
        float Shallow[4];
        float Deep[4];
        float WaveParams[4];   // x=amp, y=wavelength, z=speed, w=steepness
        float WindParams[4];   // x=cos(dir), y=sin(dir), z=numWaves, w=fresnelPow
        float ExtraParams[4];  // x=specPower, y=refractStrength, z=unused, w=hasSceneTex(0/1)
        float ScreenParams[4]; // x=invScreenW, y=invScreenH
        float SsrParams[4];    // x=enabled, y=steps, z=stepLen, w=thickness
        float ExtraParams2[4]; // x=causticStrength, y=causticScale, z=sssStrength
        float Extinction[4];   // xyz = sigma RGB, w = scatter strength (Beer-Lambert)
        float DetailParams[4]; // x=enabled, y=strength, z=reflRoughness, w=skyMaxMip
        float DetailScales[4]; // xyz = per-layer tile freq, w = unused
    };
    static_assert((sizeof(WaterCB) % 16) == 0, "WaterCB align");

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
                printf("[Water] Compile error %s/%s: %s\n", path, entry, (const char*)errs->GetBufferPointer());
                errs->Release();
            } else {
                printf("[Water] Compile failed %s/%s (file not found?)\n", path, entry);
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

    static D3DXMATRIX InverseAffine(const D3DXMATRIX& m)
    {
        DirectX::XMMATRIX xm = DirectX::XMLoadFloat4x4(reinterpret_cast<const DirectX::XMFLOAT4X4*>(&m));
        DirectX::XMMATRIX inv = DirectX::XMMatrixInverse(nullptr, xm);
        D3DXMATRIX out;
        DirectX::XMStoreFloat4x4(reinterpret_cast<DirectX::XMFLOAT4X4*>(&out), inv);
        return out;
    }
}

Water::Water() : m_device(nullptr) {}
Water::~Water() { Shutdown(); }

bool Water::Init(DX11Device* dev, const char* shaderPath)
{
    m_device = dev;
    if (!m_device || !m_device->GetDevice()) return false;
    if (!CreatePipeline(shaderPath)) return false;
    if (!CreateGrid(m_planeDivs, m_planeSize)) return false;
    if (!CreateDetailNormalMap(256)) return false;
    return true;
}

void Water::Shutdown()
{
    m_vs.Reset(); m_ps.Reset(); m_layout.Reset();
    m_vb.Reset(); m_ib.Reset(); m_cb.Reset();
    m_rs.Reset(); m_ds.Reset(); m_blend.Reset();
    m_sampLinear.Reset(); m_sampPoint.Reset(); m_sampLinearWrap.Reset();
    m_detailNormalTex.Reset(); m_detailNormalSRV.Reset();
}

bool Water::CreatePipeline(const char* shaderPath)
{
    ID3D11Device* dev = m_device->GetDevice();

    ComPtr<ID3DBlob> vsB, psB;
    if (!CompileFile(shaderPath, "WaterVS", "vs_5_0", &vsB)) return false;
    if (!CompileFile(shaderPath, "WaterPS", "ps_5_0", &psB)) return false;

    if (FAILED(dev->CreateVertexShader(vsB->GetBufferPointer(), vsB->GetBufferSize(), nullptr, &m_vs))) return false;
    if (FAILED(dev->CreatePixelShader (psB->GetBufferPointer(), psB->GetBufferSize(), nullptr, &m_ps))) return false;

    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    if (FAILED(dev->CreateInputLayout(layout, _countof(layout),
        vsB->GetBufferPointer(), vsB->GetBufferSize(), &m_layout))) return false;

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = sizeof(WaterCB);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev->CreateBuffer(&cbd, nullptr, &m_cb))) return false;

    D3D11_RASTERIZER_DESC rs = {};
    rs.FillMode = D3D11_FILL_SOLID;
    rs.CullMode = D3D11_CULL_BACK;
    rs.DepthClipEnable = TRUE;
    if (FAILED(dev->CreateRasterizerState(&rs, &m_rs))) return false;

    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_LESS;
    if (FAILED(dev->CreateDepthStencilState(&dd, &m_ds))) return false;

    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend       = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend      = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp        = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha  = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].BlendOpAlpha   = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(dev->CreateBlendState(&bd, &m_blend))) return false;

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MinLOD = 0; sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(dev->CreateSamplerState(&sd, &m_sampLinear))) return false;

    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    if (FAILED(dev->CreateSamplerState(&sd, &m_sampPoint))) return false;

    // Detail normal sampler — wraps so world-space tiling is seamless.
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    if (FAILED(dev->CreateSamplerState(&sd, &m_sampLinearWrap))) return false;

    return true;
}

bool Water::CreateDetailNormalMap(int N)
{
    // Bake a tileable height field with value-noise fBM, then derive
    // per-pixel normals via central difference. Stored RGBA8 (xyz packed
    // to [0,1], alpha unused). Sampled in WaterPS at three different
    // world-space scales/scrolls to fake high-frequency ripples.
    auto hashU = [](uint32_t x, uint32_t y, uint32_t s) -> uint32_t {
        uint32_t h = x * 374761393u + y * 668265263u + s * 1013904223u;
        h = (h ^ (h >> 13)) * 1274126177u;
        return h ^ (h >> 16);
    };
    auto hash01 = [&](int x, int y, int wrap, uint32_t s) {
        x = ((x % wrap) + wrap) % wrap;
        y = ((y % wrap) + wrap) % wrap;
        return (hashU((uint32_t)x, (uint32_t)y, s) & 0xffff) / 65535.0f;
    };
    auto smooth = [](float t) { return t * t * (3.0f - 2.0f * t); };
    auto valueNoise = [&](float u, float v, int wrap, uint32_t s) {
        float fxF = std::floor(u), fyF = std::floor(v);
        int   x0 = (int)fxF, y0 = (int)fyF;
        float sx = smooth(u - fxF), sy = smooth(v - fyF);
        float a = hash01(x0,     y0,     wrap, s);
        float b = hash01(x0 + 1, y0,     wrap, s);
        float c = hash01(x0,     y0 + 1, wrap, s);
        float d = hash01(x0 + 1, y0 + 1, wrap, s);
        float ab = a * (1.0f - sx) + b * sx;
        float cd = c * (1.0f - sx) + d * sx;
        return ab * (1.0f - sy) + cd * sy;
    };
    auto fbm = [&](float u, float v, int baseWrap, uint32_t s) {
        float total = 0.0f, amp = 1.0f, freq = 1.0f, maxV = 0.0f;
        for (int i = 0; i < 5; ++i)
        {
            int wrap = baseWrap << i;
            total += valueNoise(u * freq, v * freq, wrap, s + (uint32_t)i) * amp;
            maxV  += amp;
            amp   *= 0.55f;
            freq  *= 2.0f;
        }
        return total / maxV;
    };

    std::vector<float> H((size_t)N * N);
    for (int y = 0; y < N; ++y)
    {
        for (int x = 0; x < N; ++x)
        {
            float u = (float)x / (float)N * 8.0f;   // base wrap = 8
            float v = (float)y / (float)N * 8.0f;
            H[(size_t)y * N + x] = fbm(u, v, 8, 1u);
        }
    }

    std::vector<uint8_t> rgba((size_t)N * N * 4);
    const float heightScale = 16.0f;   // controls steepness of derived normals
    for (int y = 0; y < N; ++y)
    {
        int yU = (y - 1 + N) % N;
        int yD = (y + 1) % N;
        for (int x = 0; x < N; ++x)
        {
            int xL = (x - 1 + N) % N;
            int xR = (x + 1) % N;
            float dx = H[(size_t)y  * N + xR] - H[(size_t)y  * N + xL];
            float dy = H[(size_t)yD * N + x ] - H[(size_t)yU * N + x ];
            float nx = -dx * heightScale;
            float ny = -dy * heightScale;
            float nz = 1.0f;
            float il = 1.0f / std::sqrt(nx * nx + ny * ny + nz * nz);
            nx *= il; ny *= il; nz *= il;
            uint8_t* p = &rgba[((size_t)y * N + x) * 4];
            p[0] = (uint8_t)((nx * 0.5f + 0.5f) * 255.0f);
            p[1] = (uint8_t)((ny * 0.5f + 0.5f) * 255.0f);
            p[2] = (uint8_t)((nz * 0.5f + 0.5f) * 255.0f);
            p[3] = 255;
        }
    }

    ID3D11Device* dev = m_device->GetDevice();
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT)N; td.Height = (UINT)N;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init = {};
    init.pSysMem = rgba.data();
    init.SysMemPitch = (UINT)(N * 4);
    if (FAILED(dev->CreateTexture2D(&td, &init, &m_detailNormalTex))) return false;
    if (FAILED(dev->CreateShaderResourceView(m_detailNormalTex.Get(), nullptr, &m_detailNormalSRV))) return false;
    return true;
}

bool Water::CreateGrid(int divs, float size)
{
    std::vector<Vertex> v;
    std::vector<UINT>   idx;
    float h = size * 0.5f;
    float step = size / divs;
    v.reserve((divs + 1) * (divs + 1));
    for (int z = 0; z <= divs; ++z)
    {
        for (int x = 0; x <= divs; ++x)
        {
            Vertex vx{};
            vx.pos[0] = -h + x * step;
            vx.pos[1] = 0.0f;
            vx.pos[2] = -h + z * step;
            vx.uv[0]  = (float)x / divs;
            vx.uv[1]  = (float)z / divs;
            v.push_back(vx);
        }
    }
    idx.reserve(divs * divs * 6);
    for (int z = 0; z < divs; ++z)
    {
        for (int x = 0; x < divs; ++x)
        {
            UINT a = z * (divs + 1) + x;
            UINT b = a + 1;
            UINT c = a + (divs + 1);
            UINT d = c + 1;
            idx.push_back(a); idx.push_back(c); idx.push_back(b);
            idx.push_back(b); idx.push_back(c); idx.push_back(d);
        }
    }

    ID3D11Device* dev = m_device->GetDevice();

    D3D11_BUFFER_DESC vbd = {};
    vbd.ByteWidth = (UINT)(v.size() * sizeof(Vertex));
    vbd.Usage = D3D11_USAGE_IMMUTABLE;
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vinit{ v.data() };
    if (FAILED(dev->CreateBuffer(&vbd, &vinit, &m_vb))) return false;

    D3D11_BUFFER_DESC ibd = {};
    ibd.ByteWidth = (UINT)(idx.size() * sizeof(UINT));
    ibd.Usage = D3D11_USAGE_IMMUTABLE;
    ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA iinit{ idx.data() };
    if (FAILED(dev->CreateBuffer(&ibd, &iinit, &m_ib))) return false;

    m_indexCount = (UINT)idx.size();
    return true;
}

void Water::Render(const D3DXMATRIX& view, const D3DXMATRIX& proj,
                   const D3DXVECTOR3& eye,
                   const D3DXVECTOR3& lightDir,
                   const float lightColor[3],
                   ID3D11ShaderResourceView* sceneColorSRV,
                   ID3D11ShaderResourceView* sceneDepthSRV,
                   ID3D11ShaderResourceView* skyCubeSRV,
                   float screenW, float screenH)
{
    if (!m_enabled || m_indexCount == 0) return;

    ID3D11DeviceContext* ctx = m_device->GetContext();

    D3DXMATRIX world, vp;
    D3DXMatrixTranslation(&world, 0.0f, m_waterY, 0.0f);
    vp = view * proj;
    D3DXMATRIX invVP = InverseAffine(vp);

    WaterCB cb = {};
    MatrixToFloats(world, cb.World);
    MatrixToFloats(vp,    cb.ViewProj);
    MatrixToFloats(invVP, cb.InvViewProj);
    cb.EyePos[0] = eye.x; cb.EyePos[1] = eye.y; cb.EyePos[2] = eye.z;
    cb.Time = m_time;
    cb.LightDir[0] = lightDir.x; cb.LightDir[1] = lightDir.y; cb.LightDir[2] = lightDir.z;
    cb.LightColor[0] = lightColor[0];
    cb.LightColor[1] = lightColor[1];
    cb.LightColor[2] = lightColor[2];
    cb.SkyTint = m_skyTint;

    cb.Shallow[0] = m_shallow[0]; cb.Shallow[1] = m_shallow[1]; cb.Shallow[2] = m_shallow[2]; cb.Shallow[3] = 1.0f;
    cb.Deep[0]    = m_deep[0];    cb.Deep[1]    = m_deep[1];    cb.Deep[2]    = m_deep[2];    cb.Deep[3]    = 1.0f;

    cb.WaveParams[0] = m_waveAmp;
    cb.WaveParams[1] = m_waveLen;
    cb.WaveParams[2] = m_waveSpeed;
    cb.WaveParams[3] = m_waveSteep;

    cb.WindParams[0] = std::cos(m_windDir);
    cb.WindParams[1] = std::sin(m_windDir);
    cb.WindParams[2] = (float)m_numWaves;
    cb.WindParams[3] = m_fresnelPow;

    cb.ExtraParams[0] = m_specPower;
    cb.ExtraParams[1] = m_refractStrength;
    cb.ExtraParams[2] = 0.0f;
    cb.ExtraParams[3] = (sceneColorSRV ? 1.0f : 0.0f);
    cb.ScreenParams[0] = (screenW > 0.0f) ? 1.0f / screenW : 1.0f;
    cb.ScreenParams[1] = (screenH > 0.0f) ? 1.0f / screenH : 1.0f;

    cb.SsrParams[0] = m_ssrEnabled ? 1.0f : 0.0f;
    cb.SsrParams[1] = (float)m_ssrSteps;
    cb.SsrParams[2] = m_ssrStepLen;
    cb.SsrParams[3] = m_ssrThickness;

    cb.ExtraParams2[0] = m_causticStrength;
    cb.ExtraParams2[1] = m_causticScale;
    cb.ExtraParams2[2] = m_sssStrength;

    cb.Extinction[0] = m_extinction[0];
    cb.Extinction[1] = m_extinction[1];
    cb.Extinction[2] = m_extinction[2];
    cb.Extinction[3] = m_scatterStrength;

    cb.DetailParams[0] = m_detailEnabled ? 1.0f : 0.0f;
    cb.DetailParams[1] = m_detailStrength;
    cb.DetailParams[2] = m_reflRoughness;
    cb.DetailParams[3] = m_skyMaxMip;
    cb.DetailScales[0] = m_detailScale[0];
    cb.DetailScales[1] = m_detailScale[1];
    cb.DetailScales[2] = m_detailScale[2];

    D3D11_MAPPED_SUBRESOURCE map;
    if (SUCCEEDED(ctx->Map(m_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map)))
    {
        memcpy(map.pData, &cb, sizeof(cb));
        ctx->Unmap(m_cb.Get(), 0);
    }

    ctx->RSSetState(m_rs.Get());
    ctx->OMSetDepthStencilState(m_ds.Get(), 0);
    float blendf[4] = { 0,0,0,0 };
    ctx->OMSetBlendState(m_blend.Get(), blendf, 0xffffffff);
    ctx->IASetInputLayout(m_layout.Get());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    UINT stride = sizeof(Vertex), offset = 0;
    ID3D11Buffer* vb = m_vb.Get();
    ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    ctx->IASetIndexBuffer(m_ib.Get(), DXGI_FORMAT_R32_UINT, 0);

    ctx->VSSetShader(m_vs.Get(), nullptr, 0);
    ctx->PSSetShader(m_ps.Get(), nullptr, 0);
    ID3D11Buffer* cbs[1] = { m_cb.Get() };
    ctx->VSSetConstantBuffers(0, 1, cbs);
    ctx->PSSetConstantBuffers(0, 1, cbs);

    ID3D11ShaderResourceView* srvs[4] = { sceneColorSRV, sceneDepthSRV, skyCubeSRV, m_detailNormalSRV.Get() };
    ctx->PSSetShaderResources(0, 4, srvs);
    ID3D11SamplerState* samps[3] = { m_sampLinear.Get(), m_sampPoint.Get(), m_sampLinearWrap.Get() };
    ctx->PSSetSamplers(0, 3, samps);

    ctx->DrawIndexed(m_indexCount, 0, 0);

    // Unbind to keep the device clean for the composite step.
    ID3D11ShaderResourceView* nullSRV[4] = { nullptr, nullptr, nullptr, nullptr };
    ctx->PSSetShaderResources(0, 4, nullSRV);
}

void Water::GuiPanel()
{
    if (!ImGui::CollapsingHeader("Water", ImGuiTreeNodeFlags_DefaultOpen))
        return;
    ImGui::Checkbox("Enabled##water", &m_enabled);
    ImGui::SliderFloat("Water Y",   &m_waterY,   -2.0f, 5.0f);
    ImGui::Separator();
    ImGui::SliderFloat("Wave amp",        &m_waveAmp,    0.0f, 1.0f);
    ImGui::SliderFloat("Wavelength",      &m_waveLen,    1.0f, 30.0f);
    ImGui::SliderFloat("Wave speed",      &m_waveSpeed,  0.0f, 4.0f);
    ImGui::SliderFloat("Steepness Q",     &m_waveSteep,  0.0f, 1.5f);
    ImGui::SliderAngle("Wind dir",        &m_windDir);
    ImGui::SliderInt  ("Num waves",       &m_numWaves,   1, 32);
    ImGui::Separator();
    ImGui::ColorEdit3("Shallow color", m_shallow);
    ImGui::ColorEdit3("Deep color",    m_deep);
    ImGui::SliderFloat("Fresnel power",   &m_fresnelPow, 1.0f, 8.0f);
    ImGui::SliderFloat("Spec power",      &m_specPower, 16.0f, 1024.0f);
    ImGui::SliderFloat("Sky tint",        &m_skyTint,    0.0f, 2.0f);
    ImGui::Separator();
    ImGui::SliderFloat("Refract strength",&m_refractStrength, 0.0f, 0.20f);
    ImGui::Separator();
    ImGui::Text("Beer-Lambert absorption (m^-1)");
    ImGui::SliderFloat("Sigma R", &m_extinction[0], 0.0f, 2.0f, "%.3f");
    ImGui::SliderFloat("Sigma G", &m_extinction[1], 0.0f, 2.0f, "%.3f");
    ImGui::SliderFloat("Sigma B", &m_extinction[2], 0.0f, 2.0f, "%.3f");
    ImGui::SliderFloat("Scatter strength", &m_scatterStrength, 0.0f, 2.0f);
    ImGui::Separator();
    ImGui::Checkbox  ("Detail normals",   &m_detailEnabled);
    ImGui::SliderFloat("Detail strength", &m_detailStrength,  0.0f, 2.0f);
    ImGui::SliderFloat3("Detail scales",  m_detailScale,     0.05f, 4.0f);
    ImGui::SliderFloat("Refl roughness",  &m_reflRoughness,   0.0f, 1.0f);
    ImGui::Separator();
    ImGui::Checkbox  ("SSR##water",       &m_ssrEnabled);
    ImGui::SliderInt ("SSR steps",        &m_ssrSteps,        4, 64);
    ImGui::SliderFloat("SSR step len",    &m_ssrStepLen,      0.05f, 2.0f);
    ImGui::SliderFloat("SSR thickness",   &m_ssrThickness,    0.05f, 2.0f);
    ImGui::Separator();
    ImGui::SliderFloat("Caustic strength",&m_causticStrength, 0.0f, 2.0f);
    ImGui::SliderFloat("Caustic scale",   &m_causticScale,    0.1f, 5.0f);
    ImGui::SliderFloat("SSS strength",    &m_sssStrength,     0.0f, 2.0f);
}
