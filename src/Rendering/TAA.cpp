#include "TAA.h"
#include "DX11Device.h"
#include "imgui.h"
#include <d3dcompiler.h>
#include <string>

#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

namespace
{
    struct TaaCB
    {
        float PrevViewProj[16];
        float InvViewProjGuess[16];   // unused; kept for layout symmetry
        float ScreenParams[4];        // x=invW, y=invH, z/w=unused
        float TaaParams[4];           // x=enabled, y=historyBlend, z=clamp, w=firstFrame
        float EyePos[4];
    };
    static_assert((sizeof(TaaCB) % 16) == 0, "TaaCB align");

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
                printf("[TAA] Compile error %s/%s: %s\n", path, entry, (const char*)errs->GetBufferPointer());
                errs->Release();
            } else {
                printf("[TAA] Compile failed %s/%s\n", path, entry);
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

TAA::TAA() : m_device(nullptr), m_width(0), m_height(0) {}
TAA::~TAA() { Shutdown(); }

bool TAA::Init(DX11Device* dev, int w, int h, const char* shaderPath)
{
    m_device = dev;
    m_width = w; m_height = h;
    if (!CreatePipeline(shaderPath)) return false;
    if (!CreateRTs(w, h))             return false;
    return true;
}

void TAA::Shutdown()
{
    m_outputRTV.Reset(); m_outputSRV.Reset(); m_outputTex.Reset();
    m_historySRV.Reset(); m_historyTex.Reset();
    m_vs.Reset(); m_ps.Reset(); m_cb.Reset();
    m_rs.Reset(); m_dsOff.Reset(); m_blendNone.Reset();
    m_sampLinear.Reset(); m_sampPoint.Reset();
}

bool TAA::CreatePipeline(const char* shaderPath)
{
    ID3D11Device* dev = m_device->GetDevice();

    ComPtr<ID3DBlob> vsB, psB;
    if (!CompileFile(shaderPath, "TaaVS", "vs_5_0", &vsB)) return false;
    if (!CompileFile(shaderPath, "TaaPS", "ps_5_0", &psB)) return false;

    if (FAILED(dev->CreateVertexShader(vsB->GetBufferPointer(), vsB->GetBufferSize(), nullptr, &m_vs))) return false;
    if (FAILED(dev->CreatePixelShader (psB->GetBufferPointer(), psB->GetBufferSize(), nullptr, &m_ps))) return false;

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = sizeof(TaaCB);
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
    if (FAILED(dev->CreateDepthStencilState(&dd, &m_dsOff))) return false;

    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(dev->CreateBlendState(&bd, &m_blendNone))) return false;

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(dev->CreateSamplerState(&sd, &m_sampLinear))) return false;

    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    if (FAILED(dev->CreateSamplerState(&sd, &m_sampPoint))) return false;

    return true;
}

bool TAA::CreateRTs(int w, int h)
{
    ID3D11Device* dev = m_device->GetDevice();

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT)w; td.Height = (UINT)h;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &m_outputTex))) return false;
    if (FAILED(dev->CreateRenderTargetView(m_outputTex.Get(), nullptr, &m_outputRTV))) return false;
    if (FAILED(dev->CreateShaderResourceView(m_outputTex.Get(), nullptr, &m_outputSRV))) return false;

    D3D11_TEXTURE2D_DESC hd = td;
    hd.BindFlags = D3D11_BIND_SHADER_RESOURCE;       // history is SRV-only
    if (FAILED(dev->CreateTexture2D(&hd, nullptr, &m_historyTex))) return false;
    if (FAILED(dev->CreateShaderResourceView(m_historyTex.Get(), nullptr, &m_historySRV))) return false;

    return true;
}

void TAA::Resolve(ID3D11ShaderResourceView* sceneColorSRV,
                  ID3D11ShaderResourceView* depthSRV,
                  const D3DXMATRIX& prevViewProj,
                  const D3DXVECTOR3& eyePos,
                  float screenW, float screenH,
                  bool firstFrame)
{
    if (!m_enabled)
    {
        // When disabled, just copy SceneColor straight to Output so the
        // PostProcess pass downstream still has a valid input texture.
        ID3D11DeviceContext* c = m_device->GetContext();
        ID3D11Resource* srcRes = nullptr;
        sceneColorSRV->GetResource(&srcRes);
        c->CopyResource(m_outputTex.Get(), srcRes);
        srcRes->Release();
        return;
    }

    ID3D11DeviceContext* ctx = m_device->GetContext();

    TaaCB cb = {};
    MatrixToFloats(prevViewProj, cb.PrevViewProj);
    cb.ScreenParams[0] = (screenW > 0.0f) ? 1.0f / screenW : 1.0f;
    cb.ScreenParams[1] = (screenH > 0.0f) ? 1.0f / screenH : 1.0f;
    cb.TaaParams[0] = 1.0f;
    cb.TaaParams[1] = m_historyBlend;
    cb.TaaParams[2] = m_clampHistory ? 1.0f : 0.0f;
    cb.TaaParams[3] = firstFrame ? 1.0f : 0.0f;
    cb.EyePos[0] = eyePos.x; cb.EyePos[1] = eyePos.y; cb.EyePos[2] = eyePos.z;

    D3D11_MAPPED_SUBRESOURCE map;
    if (SUCCEEDED(ctx->Map(m_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map)))
    {
        memcpy(map.pData, &cb, sizeof(cb));
        ctx->Unmap(m_cb.Get(), 0);
    }

    ID3D11RenderTargetView* rtv = m_outputRTV.Get();
    ctx->OMSetRenderTargets(1, &rtv, nullptr);

    D3D11_VIEWPORT vp = {};
    vp.Width = (float)m_width; vp.Height = (float)m_height; vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);

    ctx->RSSetState(m_rs.Get());
    ctx->OMSetDepthStencilState(m_dsOff.Get(), 0);
    float bf[4] = { 0,0,0,0 };
    ctx->OMSetBlendState(m_blendNone.Get(), bf, 0xffffffff);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11Buffer* nullVB = nullptr; UINT zero = 0;
    ctx->IASetVertexBuffers(0, 1, &nullVB, &zero, &zero);

    ctx->VSSetShader(m_vs.Get(), nullptr, 0);
    ctx->PSSetShader(m_ps.Get(), nullptr, 0);
    ID3D11Buffer* cbs[1] = { m_cb.Get() };
    ctx->VSSetConstantBuffers(0, 1, cbs);
    ctx->PSSetConstantBuffers(0, 1, cbs);

    ID3D11ShaderResourceView* srvs[3] = { sceneColorSRV, m_historySRV.Get(), depthSRV };
    ctx->PSSetShaderResources(0, 3, srvs);
    ID3D11SamplerState* samps[2] = { m_sampLinear.Get(), m_sampPoint.Get() };
    ctx->PSSetSamplers(0, 2, samps);

    ctx->Draw(3, 0);

    ID3D11ShaderResourceView* nullSRVs[3] = { nullptr, nullptr, nullptr };
    ctx->PSSetShaderResources(0, 3, nullSRVs);
}

void TAA::Swap()
{
    if (!m_enabled) return;
    m_device->GetContext()->CopyResource(m_historyTex.Get(), m_outputTex.Get());
}

void TAA::GuiPanel()
{
    if (!ImGui::CollapsingHeader("TAA", ImGuiTreeNodeFlags_DefaultOpen))
        return;
    ImGui::Checkbox("Enabled##taa",        &m_enabled);
    ImGui::SliderFloat("History blend",    &m_historyBlend, 0.0f, 0.99f);
    ImGui::Checkbox("Neighborhood clamp",  &m_clampHistory);
}
