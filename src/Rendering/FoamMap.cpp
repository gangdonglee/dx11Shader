#include "FoamMap.h"
#include "DX11Device.h"
#include "imgui.h"
#include <d3dcompiler.h>
#include <string>

#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

namespace
{
    struct FoamCB
    {
        float PlaneRect[4];   // x=originX, y=originZ, z=size, w=invSize
        float WaveParams[4];  // x=amp, y=len, z=speed, w=Q
        float WindParams[4];  // x=cos, y=sin, z=numWaves, w=time
        float UpdateParams[4];// x=decay, y=firstFrame, z/w=unused
    };
    static_assert((sizeof(FoamCB) % 16) == 0, "FoamCB align");

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
                printf("[Foam] Compile error %s/%s: %s\n", path, entry, (const char*)errs->GetBufferPointer());
                errs->Release();
            } else {
                printf("[Foam] Compile failed %s/%s\n", path, entry);
            }
            return false;
        }
        if (errs) errs->Release();
        return true;
    }
}

FoamMap::FoamMap() : m_device(nullptr) {}
FoamMap::~FoamMap() { Shutdown(); }

bool FoamMap::Init(DX11Device* device, int size, const char* shaderPath)
{
    m_device = device;
    m_size = size;
    if (!m_device || !m_device->GetDevice()) return false;
    if (!CreatePipeline(shaderPath)) return false;
    if (!CreateRTs(size))            return false;
    return true;
}

void FoamMap::Shutdown()
{
    for (int i = 0; i < 2; ++i)
    {
        m_tex[i].Reset(); m_rtv[i].Reset(); m_srv[i].Reset();
    }
    m_vs.Reset(); m_ps.Reset(); m_cb.Reset();
    m_rs.Reset(); m_dsOff.Reset(); m_blendNone.Reset();
    m_sampLinearClamp.Reset();
}

bool FoamMap::CreatePipeline(const char* shaderPath)
{
    ID3D11Device* dev = m_device->GetDevice();

    ComPtr<ID3DBlob> vsB, psB;
    if (!CompileFile(shaderPath, "FoamVS", "vs_5_0", &vsB)) return false;
    if (!CompileFile(shaderPath, "FoamPS", "ps_5_0", &psB)) return false;

    if (FAILED(dev->CreateVertexShader(vsB->GetBufferPointer(), vsB->GetBufferSize(), nullptr, &m_vs))) return false;
    if (FAILED(dev->CreatePixelShader (psB->GetBufferPointer(), psB->GetBufferSize(), nullptr, &m_ps))) return false;

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = sizeof(FoamCB);
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
    if (FAILED(dev->CreateSamplerState(&sd, &m_sampLinearClamp))) return false;

    return true;
}

bool FoamMap::CreateRTs(int size)
{
    ID3D11Device* dev = m_device->GetDevice();

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT)size; td.Height = (UINT)size;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    for (int i = 0; i < 2; ++i)
    {
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &m_tex[i]))) return false;
        if (FAILED(dev->CreateRenderTargetView (m_tex[i].Get(), nullptr, &m_rtv[i]))) return false;
        if (FAILED(dev->CreateShaderResourceView(m_tex[i].Get(), nullptr, &m_srv[i]))) return false;

        // Clear initial state to zero so first sample isn't garbage.
        float zero[4] = { 0,0,0,0 };
        m_device->GetContext()->ClearRenderTargetView(m_rtv[i].Get(), zero);
    }
    return true;
}

void FoamMap::Update(const UpdateInputs& in)
{
    if (!m_enabled) return;

    ID3D11DeviceContext* ctx = m_device->GetContext();

    m_planeOriginX = in.planeOriginX;
    m_planeOriginZ = in.planeOriginZ;
    m_planeSize    = in.planeSize;

    int writeIdx = m_writeIdx;
    int readIdx  = 1 - writeIdx;

    FoamCB cb = {};
    cb.PlaneRect[0] = in.planeOriginX;
    cb.PlaneRect[1] = in.planeOriginZ;
    cb.PlaneRect[2] = in.planeSize;
    cb.PlaneRect[3] = (in.planeSize > 0.0f) ? 1.0f / in.planeSize : 0.0f;
    cb.WaveParams[0] = in.waveAmp;
    cb.WaveParams[1] = in.waveLen;
    cb.WaveParams[2] = in.waveSpeed;
    cb.WaveParams[3] = in.waveSteep;
    cb.WindParams[0] = in.windCos;
    cb.WindParams[1] = in.windSin;
    cb.WindParams[2] = (float)in.numWaves;
    cb.WindParams[3] = in.time;
    cb.UpdateParams[0] = m_decay;
    cb.UpdateParams[1] = m_firstFrame ? 1.0f : 0.0f;

    D3D11_MAPPED_SUBRESOURCE map;
    if (SUCCEEDED(ctx->Map(m_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map)))
    {
        memcpy(map.pData, &cb, sizeof(cb));
        ctx->Unmap(m_cb.Get(), 0);
    }

    UINT n = 1;
    D3D11_VIEWPORT savedVP;
    ctx->RSGetViewports(&n, &savedVP);

    D3D11_VIEWPORT vp = {};
    vp.Width = (float)m_size; vp.Height = (float)m_size; vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);

    ID3D11RenderTargetView* rtv = m_rtv[writeIdx].Get();
    ctx->OMSetRenderTargets(1, &rtv, nullptr);

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

    ID3D11ShaderResourceView* srvs[1] = { m_srv[readIdx].Get() };
    ctx->PSSetShaderResources(0, 1, srvs);
    ID3D11SamplerState* samps[1] = { m_sampLinearClamp.Get() };
    ctx->PSSetSamplers(0, 1, samps);

    ctx->Draw(3, 0);

    ID3D11ShaderResourceView* nullSRV = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSRV);
    ctx->RSSetViewports(1, &savedVP);

    m_writeIdx = readIdx;       // next frame writes to the other texture
    m_firstFrame = false;
}

ID3D11ShaderResourceView* FoamMap::GetSRV() const
{
    if (!m_enabled) return nullptr;
    // Latest write is at (1 - m_writeIdx) since Update flipped m_writeIdx
    // for the next frame. So the just-written texture is at (1 - m_writeIdx).
    int latest = 1 - m_writeIdx;
    return m_srv[latest].Get();
}

void FoamMap::GuiPanel()
{
    if (!ImGui::CollapsingHeader("Foam Map", ImGuiTreeNodeFlags_DefaultOpen))
        return;
    ImGui::Checkbox("Enabled##foam", &m_enabled);
    ImGui::SliderFloat("Decay", &m_decay, 0.80f, 0.999f, "%.4f");
}
