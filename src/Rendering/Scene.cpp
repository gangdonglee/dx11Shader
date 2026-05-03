#include "Scene.h"
#include "DX11Device.h"
#include "DirectionalLight.h"
#include "ShadowMap.h"
#include "imgui.h"
#include <d3dcompiler.h>
#include <string>
#include <cmath>

#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

namespace
{
    struct CB
    {
        float World[16];
        float ViewProj[16];
        float InvViewProj[16];
        float LightViewProj[16];
        float EyePos[3];      float Time;
        float LightDir[3];    float Ambient;
        float LightColor[3];  float pad0;
        float BaseColor[4];
        float Params[4];        // x=mode (0=Lambert), y=metallic, z=roughness
        float ShadowParams[4];  // x=enabled, y=depthBias, z=normalBias
    };
    static_assert((sizeof(CB) % 16) == 0, "CB must be 16-byte aligned");

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
            if (errs)
            {
                printf("[Scene] Compile error %s/%s: %s\n", path, entry, (const char*)errs->GetBufferPointer());
                errs->Release();
            }
            else
            {
                printf("[Scene] Compile failed %s/%s (file not found?)\n", path, entry);
            }
            return false;
        }
        if (errs) errs->Release();
        return true;
    }

    static void MatrixToFloats(const D3DXMATRIX& m, float out[16])
    {
        // store transposed for HLSL column-major default with mul(v, M)
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                out[r * 4 + c] = m.m[c][r];
    }

    static void Identity(D3DXMATRIX& m)
    {
        D3DXMatrixIdentity(&m);
    }

    static D3DXMATRIX TranslateScale(float x, float y, float z, float s)
    {
        D3DXMATRIX scale, trans;
        D3DXMatrixScaling(&scale, s, s, s);
        D3DXMatrixTranslation(&trans, x, y, z);
        return scale * trans;
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

Scene::Scene() : m_device(nullptr) {}
Scene::~Scene() { Shutdown(); }

bool Scene::Init(DX11Device* dev, const char* shaderPath)
{
    m_device = dev;
    if (!m_device || !m_device->GetDevice()) return false;

    if (!CreatePipeline(shaderPath)) return false;
    if (!BuildMeshes())               return false;

    // Spheres demo the dielectric→metal axis (left to right) and box row
    // demos the rough→smooth axis. Same setup looks good in any mode.
    auto add = [&](Mesh* m, float x, float y, float z, float s,
                   float r, float g, float b, float metallic, float rough)
    {
        Instance inst;
        inst.mesh = m;
        inst.world = TranslateScale(x, y, z, s);
        inst.baseColor[0] = r; inst.baseColor[1] = g; inst.baseColor[2] = b; inst.baseColor[3] = 1.0f;
        inst.metallic  = metallic;
        inst.roughness = rough;
        m_instances.push_back(inst);
    };

    add(&m_plane,   0.0f, 0.0f,  0.0f, 1.0f, 0.55f, 0.50f, 0.45f, 0.0f, 0.85f); // ground
    add(&m_sphere, -3.0f, 1.0f, -1.0f, 1.0f, 0.85f, 0.20f, 0.20f, 0.0f, 0.30f); // dielectric red
    add(&m_sphere,  0.0f, 1.0f, -1.0f, 1.0f, 0.20f, 0.85f, 0.40f, 0.5f, 0.20f); // half-metal green
    add(&m_sphere,  3.0f, 1.0f, -1.0f, 1.0f, 1.00f, 0.85f, 0.55f, 1.0f, 0.15f); // gold metal
    add(&m_box,    -3.0f, 0.6f,  3.0f, 1.2f, 0.90f, 0.78f, 0.30f, 0.0f, 0.80f); // rough yellow
    add(&m_box,     0.0f, 0.6f,  3.0f, 1.2f, 0.40f, 0.40f, 0.45f, 0.0f, 0.40f); // medium dark
    add(&m_box,     3.0f, 0.6f,  3.0f, 1.2f, 0.85f, 0.85f, 0.85f, 1.0f, 0.10f); // chrome

    return true;
}

void Scene::Shutdown()
{
    m_instances.clear();
    m_plane = {}; m_sphere = {}; m_box = {};
    m_meshVS.Reset(); m_meshPS.Reset(); m_shadowVS.Reset();
    m_shadowCmp.Reset();
    m_layout.Reset(); m_cb.Reset();
    m_rs.Reset(); m_dsOn.Reset(); m_blendNone.Reset();
}

bool Scene::CreatePipeline(const char* shaderPath)
{
    ID3D11Device* dev = m_device->GetDevice();

    ComPtr<ID3DBlob> vsB, psB, svB;
    if (!CompileFile(shaderPath, "MeshVS",   "vs_5_0", &vsB)) return false;
    if (!CompileFile(shaderPath, "MeshPS",   "ps_5_0", &psB)) return false;
    if (!CompileFile(shaderPath, "ShadowVS", "vs_5_0", &svB)) return false;

    if (FAILED(dev->CreateVertexShader(vsB->GetBufferPointer(), vsB->GetBufferSize(), nullptr, &m_meshVS))) return false;
    if (FAILED(dev->CreatePixelShader (psB->GetBufferPointer(), psB->GetBufferSize(), nullptr, &m_meshPS))) return false;
    if (FAILED(dev->CreateVertexShader(svB->GetBufferPointer(), svB->GetBufferSize(), nullptr, &m_shadowVS))) return false;

    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    if (FAILED(dev->CreateInputLayout(layout, _countof(layout),
        vsB->GetBufferPointer(), vsB->GetBufferSize(), &m_layout))) return false;

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = sizeof(CB);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev->CreateBuffer(&cbd, nullptr, &m_cb))) return false;

    D3D11_RASTERIZER_DESC rs = {};
    rs.FillMode = D3D11_FILL_SOLID;
    rs.CullMode = D3D11_CULL_BACK;
    rs.FrontCounterClockwise = FALSE;
    rs.DepthClipEnable = TRUE;
    if (FAILED(dev->CreateRasterizerState(&rs, &m_rs))) return false;

    D3D11_DEPTH_STENCIL_DESC dsOn = {};
    dsOn.DepthEnable = TRUE;
    dsOn.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsOn.DepthFunc = D3D11_COMPARISON_LESS;
    if (FAILED(dev->CreateDepthStencilState(&dsOn, &m_dsOn))) return false;

    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable = FALSE;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(dev->CreateBlendState(&bd, &m_blendNone))) return false;

    D3D11_SAMPLER_DESC sm = {};
    sm.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
    sm.BorderColor[0] = sm.BorderColor[1] = sm.BorderColor[2] = sm.BorderColor[3] = 1.0f;
    sm.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
    sm.MaxAnisotropy = 1;
    sm.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(dev->CreateSamplerState(&sm, &m_shadowCmp))) return false;

    return true;
}

bool Scene::BuildBufferGPU(Mesh& out, const std::vector<Vertex>& v, const std::vector<UINT>& i)
{
    ID3D11Device* dev = m_device->GetDevice();
    D3D11_BUFFER_DESC vbd = {};
    vbd.ByteWidth = (UINT)(v.size() * sizeof(Vertex));
    vbd.Usage = D3D11_USAGE_IMMUTABLE;
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vinit{ v.data() };
    if (FAILED(dev->CreateBuffer(&vbd, &vinit, &out.vb))) return false;

    D3D11_BUFFER_DESC ibd = {};
    ibd.ByteWidth = (UINT)(i.size() * sizeof(UINT));
    ibd.Usage = D3D11_USAGE_IMMUTABLE;
    ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA iinit{ i.data() };
    if (FAILED(dev->CreateBuffer(&ibd, &iinit, &out.ib))) return false;

    out.indexCount = (UINT)i.size();
    return true;
}

bool Scene::BuildPlane(Mesh& out, float size, int divs)
{
    std::vector<Vertex> v;
    std::vector<UINT>   idx;
    float h = size * 0.5f;
    float step = size / divs;
    for (int z = 0; z <= divs; ++z)
    {
        for (int x = 0; x <= divs; ++x)
        {
            Vertex vx{};
            vx.pos[0] = -h + x * step;
            vx.pos[1] = 0.0f;
            vx.pos[2] = -h + z * step;
            vx.nor[0] = 0; vx.nor[1] = 1; vx.nor[2] = 0;
            vx.uv[0]  = (float)x / divs;
            vx.uv[1]  = (float)z / divs;
            v.push_back(vx);
        }
    }
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
    return BuildBufferGPU(out, v, idx);
}

bool Scene::BuildSphere(Mesh& out, float radius, int slices, int stacks)
{
    std::vector<Vertex> v;
    std::vector<UINT>   idx;
    for (int s = 0; s <= stacks; ++s)
    {
        float phi = (float)s / stacks * D3DX_PI;        // 0..PI (top to bottom)
        float sp  = std::sin(phi), cp = std::cos(phi);
        for (int i = 0; i <= slices; ++i)
        {
            float theta = (float)i / slices * 2.0f * D3DX_PI;
            float st = std::sin(theta), ct = std::cos(theta);
            Vertex vx{};
            vx.nor[0] = sp * ct;
            vx.nor[1] = cp;
            vx.nor[2] = sp * st;
            vx.pos[0] = vx.nor[0] * radius;
            vx.pos[1] = vx.nor[1] * radius;
            vx.pos[2] = vx.nor[2] * radius;
            vx.uv[0]  = (float)i / slices;
            vx.uv[1]  = (float)s / stacks;
            v.push_back(vx);
        }
    }
    int row = slices + 1;
    for (int s = 0; s < stacks; ++s)
    {
        for (int i = 0; i < slices; ++i)
        {
            UINT a = s * row + i;
            UINT b = a + 1;
            UINT c = a + row;
            UINT d = c + 1;
            idx.push_back(a); idx.push_back(c); idx.push_back(b);
            idx.push_back(b); idx.push_back(c); idx.push_back(d);
        }
    }
    return BuildBufferGPU(out, v, idx);
}

bool Scene::BuildBox(Mesh& out, float size)
{
    float h = size * 0.5f;
    // 6 faces, 4 verts each, normal + uv per face
    struct Face { float n[3]; float u[3]; float v[3]; };
    Face faces[6] = {
        {{ 0, 0, 1},{1,0,0},{0,1,0}},   // +Z
        {{ 0, 0,-1},{-1,0,0},{0,1,0}},  // -Z
        {{ 1, 0, 0},{0,0,-1},{0,1,0}},  // +X
        {{-1, 0, 0},{0,0,1},{0,1,0}},   // -X
        {{ 0, 1, 0},{1,0,0},{0,0,-1}},  // +Y
        {{ 0,-1, 0},{1,0,0},{0,0,1}},   // -Y
    };
    std::vector<Vertex> v;
    std::vector<UINT>   idx;
    for (int f = 0; f < 6; ++f)
    {
        const Face& F = faces[f];
        // center of face = normal * h; corners = center + u*h + v*h
        for (int j = 0; j < 4; ++j)
        {
            float us = (j == 0 || j == 3) ? -1.0f : 1.0f;
            float vs = (j == 0 || j == 1) ? -1.0f : 1.0f;
            Vertex vx{};
            vx.pos[0] = F.n[0] * h + F.u[0] * us * h + F.v[0] * vs * h;
            vx.pos[1] = F.n[1] * h + F.u[1] * us * h + F.v[1] * vs * h;
            vx.pos[2] = F.n[2] * h + F.u[2] * us * h + F.v[2] * vs * h;
            vx.nor[0] = F.n[0]; vx.nor[1] = F.n[1]; vx.nor[2] = F.n[2];
            vx.uv[0]  = (us + 1.0f) * 0.5f;
            vx.uv[1]  = (vs + 1.0f) * 0.5f;
            v.push_back(vx);
        }
        UINT base = (UINT)(f * 4);
        idx.push_back(base + 0); idx.push_back(base + 1); idx.push_back(base + 2);
        idx.push_back(base + 0); idx.push_back(base + 2); idx.push_back(base + 3);
    }
    return BuildBufferGPU(out, v, idx);
}

bool Scene::BuildMeshes()
{
    if (!BuildPlane (m_plane,  60.0f, 16)) return false;
    if (!BuildSphere(m_sphere, 1.0f, 48, 32)) return false;
    if (!BuildBox   (m_box,    1.0f))      return false;
    return true;
}

void Scene::Render(const D3DXMATRIX& view, const D3DXMATRIX& proj,
                   const D3DXVECTOR3& eye, float time,
                   const DirectionalLight& sun,
                   const ShadowMap* shadow)
{
    ID3D11DeviceContext* ctx = m_device->GetContext();

    D3DXMATRIX vp = view * proj;
    D3DXMATRIX invVP = InverseAffine(vp);

    D3DXVECTOR3 Ldir = sun.GetDirection();   // points INTO the scene
    const float* col = sun.GetColor();
    float intensity  = sun.GetIntensity();

    CB cb = {};
    MatrixToFloats(vp,    cb.ViewProj);
    MatrixToFloats(invVP, cb.InvViewProj);
    if (shadow)
        MatrixToFloats(shadow->GetLightViewProj(), cb.LightViewProj);
    cb.EyePos[0] = eye.x; cb.EyePos[1] = eye.y; cb.EyePos[2] = eye.z;
    cb.Time = time;
    cb.LightDir[0] = Ldir.x; cb.LightDir[1] = Ldir.y; cb.LightDir[2] = Ldir.z;
    cb.Ambient = m_ambient;
    cb.LightColor[0] = col[0] * intensity;
    cb.LightColor[1] = col[1] * intensity;
    cb.LightColor[2] = col[2] * intensity;
    cb.ShadowParams[0] = (shadow && shadow->m_enabled) ? 1.0f : 0.0f;
    cb.ShadowParams[1] = shadow ? shadow->m_depthBias  : 0.0f;
    cb.ShadowParams[2] = shadow ? shadow->m_normalBias : 0.0f;

    auto upload = [&]() {
        D3D11_MAPPED_SUBRESOURCE map;
        if (FAILED(ctx->Map(m_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) return;
        memcpy(map.pData, &cb, sizeof(cb));
        ctx->Unmap(m_cb.Get(), 0);
    };

    // ---- Mesh pass (sky background is now drawn by Skybox before us) ----
    ctx->RSSetState(m_rs.Get());
    float blendf[4] = { 0,0,0,0 };
    ctx->OMSetBlendState(m_blendNone.Get(), blendf, 0xffffffff);
    ctx->OMSetDepthStencilState(m_dsOn.Get(), 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(m_layout.Get());
    ctx->VSSetShader(m_meshVS.Get(), nullptr, 0);
    ctx->PSSetShader(m_meshPS.Get(), nullptr, 0);
    ID3D11Buffer* cbs[1] = { m_cb.Get() };
    ctx->VSSetConstantBuffers(0, 1, cbs);
    ctx->PSSetConstantBuffers(0, 1, cbs);

    // Bind shadow map for sampling in MeshPS.
    if (shadow)
    {
        ID3D11ShaderResourceView* srvs[1] = { shadow->GetSRV() };
        ctx->PSSetShaderResources(0, 1, srvs);
        ID3D11SamplerState* samps[1] = { m_shadowCmp.Get() };
        ctx->PSSetSamplers(0, 1, samps);
    }

    UINT stride = sizeof(Vertex), offset = 0;
    for (const Instance& inst : m_instances)
    {
        if (!inst.mesh || !inst.mesh->vb || !inst.mesh->ib) continue;
        MatrixToFloats(inst.world, cb.World);
        for (int i = 0; i < 4; ++i) cb.BaseColor[i] = inst.baseColor[i];
        cb.Params[0] = (float)m_mode;
        cb.Params[1] = inst.metallic;
        cb.Params[2] = inst.roughness;
        cb.Params[3] = m_rimAmount;
        upload();

        ID3D11Buffer* vb = inst.mesh->vb.Get();
        ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        ctx->IASetIndexBuffer(inst.mesh->ib.Get(), DXGI_FORMAT_R32_UINT, 0);
        ctx->DrawIndexed(inst.mesh->indexCount, 0, 0);
    }

    // Unbind shadow SRV.
    if (shadow)
    {
        ID3D11ShaderResourceView* nullSRV = nullptr;
        ctx->PSSetShaderResources(0, 1, &nullSRV);
    }
}

void Scene::RenderShadowDepth(const D3DXMATRIX& lightViewProj)
{
    ID3D11DeviceContext* ctx = m_device->GetContext();

    CB cb = {};
    // ShadowVS reads only World + ViewProj; everything else is dead code
    // for that entry point. Stuff lightViewProj into ViewProj.
    MatrixToFloats(lightViewProj, cb.ViewProj);

    auto upload = [&]() {
        D3D11_MAPPED_SUBRESOURCE map;
        if (FAILED(ctx->Map(m_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) return;
        memcpy(map.pData, &cb, sizeof(cb));
        ctx->Unmap(m_cb.Get(), 0);
    };

    ctx->IASetInputLayout(m_layout.Get());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(m_shadowVS.Get(), nullptr, 0);
    ctx->PSSetShader(nullptr, nullptr, 0);   // depth-only
    ID3D11Buffer* cbs[1] = { m_cb.Get() };
    ctx->VSSetConstantBuffers(0, 1, cbs);

    UINT stride = sizeof(Vertex), offset = 0;
    for (const Instance& inst : m_instances)
    {
        if (!inst.mesh || !inst.mesh->vb || !inst.mesh->ib) continue;
        MatrixToFloats(inst.world, cb.World);
        upload();

        ID3D11Buffer* vb = inst.mesh->vb.Get();
        ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        ctx->IASetIndexBuffer(inst.mesh->ib.Get(), DXGI_FORMAT_R32_UINT, 0);
        ctx->DrawIndexed(inst.mesh->indexCount, 0, 0);
    }
}

void Scene::GuiPanel()
{
    if (!ImGui::CollapsingHeader("Scene", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    const char* modes[] = { "Lambert", "PBR (Realistic)", "Cel (BotW)" };
    ImGui::Combo("Render mode", &m_mode, modes, IM_ARRAYSIZE(modes));
    ImGui::SliderFloat("Ambient", &m_ambient, 0.0f, 0.5f);
    if (m_mode == Mode_Cel)
        ImGui::SliderFloat("Rim amount", &m_rimAmount, 0.0f, 2.0f);
}
