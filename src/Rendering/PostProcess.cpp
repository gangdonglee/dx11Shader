#include "PostProcess.h"
#include "DX11Device.h"
#include "imgui.h"
#include <d3dcompiler.h>
#include <string>

#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

namespace
{
    struct PostCB
    {
        float InvViewProj[16];
        float EyePos[3];        float NearZ;
        float ScreenParams[4];  // x=invW, y=invH, z=near, w=far
        float Outline[4];       // x=enabled, y=strength, z=threshold, w=unused
        float OutlineColor[4];
        float Fog[4];           // x=enabled, y=density, z=start, w=unused
        float FogColor[4];
        float Grade[4];         // x=enabled, y=exposure, z=saturation, w=invGamma
        float Lift[4];
        float Gain[4];
        float Tonemap[4];       // x=enabled (ACES)
    };
    static_assert((sizeof(PostCB) % 16) == 0, "PostCB align");

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
        if (FAILED(hr)) {
            if (errs) {
                printf("[Post] Compile error %s/%s: %s\n", path, entry, (const char*)errs->GetBufferPointer());
                errs->Release();
            } else {
                printf("[Post] Compile failed %s/%s\n", path, entry);
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

PostProcess::PostProcess() : m_device(nullptr) {}
PostProcess::~PostProcess() { Shutdown(); }

bool PostProcess::Init(DX11Device* dev, const char* shaderPath)
{
    m_device = dev;
    if (!m_device || !m_device->GetDevice()) return false;
    return CreatePipeline(shaderPath);
}

void PostProcess::Shutdown()
{
    m_vs.Reset(); m_ps.Reset(); m_cb.Reset();
    m_dsOff.Reset(); m_rs.Reset(); m_blendNone.Reset();
    m_sampLinear.Reset(); m_sampPoint.Reset();
}

bool PostProcess::CreatePipeline(const char* shaderPath)
{
    ID3D11Device* dev = m_device->GetDevice();

    ComPtr<ID3DBlob> vsB, psB;
    if (!CompileFile(shaderPath, "PostVS", "vs_5_0", &vsB)) return false;
    if (!CompileFile(shaderPath, "PostPS", "ps_5_0", &psB)) return false;

    if (FAILED(dev->CreateVertexShader(vsB->GetBufferPointer(), vsB->GetBufferSize(), nullptr, &m_vs))) return false;
    if (FAILED(dev->CreatePixelShader (psB->GetBufferPointer(), psB->GetBufferSize(), nullptr, &m_ps))) return false;

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = sizeof(PostCB);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev->CreateBuffer(&cbd, nullptr, &m_cb))) return false;

    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = FALSE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    if (FAILED(dev->CreateDepthStencilState(&dd, &m_dsOff))) return false;

    D3D11_RASTERIZER_DESC rs = {};
    rs.FillMode = D3D11_FILL_SOLID;
    rs.CullMode = D3D11_CULL_NONE;
    rs.DepthClipEnable = TRUE;
    if (FAILED(dev->CreateRasterizerState(&rs, &m_rs))) return false;

    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable = FALSE;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(dev->CreateBlendState(&bd, &m_blendNone))) return false;

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MinLOD = 0; sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(dev->CreateSamplerState(&sd, &m_sampLinear))) return false;

    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    if (FAILED(dev->CreateSamplerState(&sd, &m_sampPoint))) return false;

    return true;
}

void PostProcess::Render(ID3D11ShaderResourceView* sceneColorSRV,
                         ID3D11ShaderResourceView* sceneDepthSRV,
                         const D3DXMATRIX& invViewProj,
                         const D3DXVECTOR3& eyePos,
                         float screenW, float screenH,
                         float nearZ, float farZ)
{
    ID3D11DeviceContext* ctx = m_device->GetContext();

    PostCB cb = {};
    MatrixToFloats(invViewProj, cb.InvViewProj);
    cb.EyePos[0] = eyePos.x; cb.EyePos[1] = eyePos.y; cb.EyePos[2] = eyePos.z;
    cb.NearZ = nearZ;
    cb.ScreenParams[0] = (screenW > 0.0f) ? 1.0f / screenW : 1.0f;
    cb.ScreenParams[1] = (screenH > 0.0f) ? 1.0f / screenH : 1.0f;
    cb.ScreenParams[2] = nearZ;
    cb.ScreenParams[3] = farZ;

    cb.Outline[0] = m_outlineEnabled ? 1.0f : 0.0f;
    cb.Outline[1] = m_outlineStrength;
    cb.Outline[2] = m_outlineThresh;
    cb.OutlineColor[0] = m_outlineColor[0];
    cb.OutlineColor[1] = m_outlineColor[1];
    cb.OutlineColor[2] = m_outlineColor[2];
    cb.OutlineColor[3] = 1.0f;

    cb.Fog[0] = m_fogEnabled ? 1.0f : 0.0f;
    cb.Fog[1] = m_fogDensity;
    cb.Fog[2] = m_fogStart;
    cb.FogColor[0] = m_fogColor[0];
    cb.FogColor[1] = m_fogColor[1];
    cb.FogColor[2] = m_fogColor[2];
    cb.FogColor[3] = 1.0f;

    cb.Grade[0] = m_gradeEnabled ? 1.0f : 0.0f;
    cb.Grade[1] = m_exposure;
    cb.Grade[2] = m_saturation;
    cb.Grade[3] = (m_gamma > 0.001f) ? (1.0f / m_gamma) : 1.0f;
    cb.Lift[0] = m_lift[0]; cb.Lift[1] = m_lift[1]; cb.Lift[2] = m_lift[2]; cb.Lift[3] = 0.0f;
    cb.Gain[0] = m_gain[0]; cb.Gain[1] = m_gain[1]; cb.Gain[2] = m_gain[2]; cb.Gain[3] = 0.0f;
    cb.Tonemap[0] = m_tonemapEnabled ? 1.0f : 0.0f;

    D3D11_MAPPED_SUBRESOURCE map;
    if (SUCCEEDED(ctx->Map(m_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map)))
    {
        memcpy(map.pData, &cb, sizeof(cb));
        ctx->Unmap(m_cb.Get(), 0);
    }

    ctx->RSSetState(m_rs.Get());
    ctx->OMSetDepthStencilState(m_dsOff.Get(), 0);
    float blendf[4] = { 0,0,0,0 };
    ctx->OMSetBlendState(m_blendNone.Get(), blendf, 0xffffffff);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11Buffer* nullVB = nullptr; UINT zero = 0;
    ctx->IASetVertexBuffers(0, 1, &nullVB, &zero, &zero);
    ctx->VSSetShader(m_vs.Get(), nullptr, 0);
    ctx->PSSetShader(m_ps.Get(), nullptr, 0);

    ID3D11Buffer* cbs[1] = { m_cb.Get() };
    ctx->VSSetConstantBuffers(0, 1, cbs);
    ctx->PSSetConstantBuffers(0, 1, cbs);

    ID3D11ShaderResourceView* srvs[2] = { sceneColorSRV, sceneDepthSRV };
    ctx->PSSetShaderResources(0, 2, srvs);
    ID3D11SamplerState* samps[2] = { m_sampLinear.Get(), m_sampPoint.Get() };
    ctx->PSSetSamplers(0, 2, samps);

    ctx->Draw(3, 0);

    ID3D11ShaderResourceView* nullSRV[2] = { nullptr, nullptr };
    ctx->PSSetShaderResources(0, 2, nullSRV);
}

void PostProcess::GuiPanel()
{
    if (!ImGui::CollapsingHeader("PostProcess", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    ImGui::Checkbox("Outline##post", &m_outlineEnabled);
    if (m_outlineEnabled)
    {
        ImGui::SliderFloat("Outline strength", &m_outlineStrength, 0.0f, 2.0f);
        ImGui::SliderFloat("Outline threshold", &m_outlineThresh, 0.0f, 0.01f, "%.5f");
        ImGui::ColorEdit3("Outline color", m_outlineColor);
    }
    ImGui::Separator();
    ImGui::Checkbox("Fog##post", &m_fogEnabled);
    if (m_fogEnabled)
    {
        ImGui::SliderFloat("Fog density", &m_fogDensity, 0.0f, 0.2f, "%.4f");
        ImGui::SliderFloat("Fog start",   &m_fogStart,   0.0f, 100.0f);
        ImGui::ColorEdit3 ("Fog color",   m_fogColor);
    }
    ImGui::Separator();
    ImGui::Checkbox("ACES tonemap##post", &m_tonemapEnabled);
    ImGui::Checkbox("Color grading##post", &m_gradeEnabled);
    if (m_gradeEnabled)
    {
        ImGui::SliderFloat("Exposure",   &m_exposure,   0.0f, 4.0f);
        ImGui::SliderFloat("Saturation", &m_saturation, 0.0f, 2.0f);
        ImGui::SliderFloat("Gamma",      &m_gamma,      0.4f, 2.5f);
        ImGui::SliderFloat3("Lift",      m_lift, -0.3f, 0.3f);
        ImGui::SliderFloat3("Gain",      m_gain,  0.0f, 2.0f);
    }
}
