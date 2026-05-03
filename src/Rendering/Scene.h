#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <vector>
#include "MathTypes.h"

class DX11Device;
class DirectionalLight;
class ShadowMap;

// Phase 1 Scene: a few hand-built meshes (plane, spheres, boxes) drawn
// with a basic Lambert+sky shader. Phase 2 adds PBR/Cel paths to the
// same shader; Phase 3+ leaves this alone and adds water on top.
class Scene
{
public:
    Scene();
    ~Scene();

    bool Init(DX11Device* device, const char* shaderPath = "scene.hlsl");
    void Shutdown();

    void Render(const D3DXMATRIX& view, const D3DXMATRIX& proj,
                const D3DXVECTOR3& eyePos, float time,
                const DirectionalLight& sun,
                const ShadowMap* shadow);

    // Depth-only pass for shadow map. Caller binds the ShadowMap DSV
    // and rasterizer state before calling.
    void RenderShadowDepth(const D3DXMATRIX& lightViewProj);

    // ImGui hook (called from DebugUI).
    void GuiPanel();

    enum Mode { Mode_Lambert = 0, Mode_PBR = 1, Mode_Cel = 2 };
    void  SetMode(int m)          { m_mode = m; }
    int   GetMode() const         { return m_mode; }

private:
    struct Vertex
    {
        float pos[3];
        float nor[3];
        float uv[2];
    };

    struct Mesh
    {
        Microsoft::WRL::ComPtr<ID3D11Buffer> vb;
        Microsoft::WRL::ComPtr<ID3D11Buffer> ib;
        UINT indexCount = 0;
    };

    struct Instance
    {
        Mesh*       mesh;
        D3DXMATRIX  world;
        float       baseColor[4];
        float       metallic;
        float       roughness;
    };

    bool CreatePipeline(const char* shaderPath);
    bool BuildMeshes();
    bool BuildPlane (Mesh& out, float size, int divs);
    bool BuildSphere(Mesh& out, float radius, int slices, int stacks);
    bool BuildBox   (Mesh& out, float size);
    bool BuildBufferGPU(Mesh& out, const std::vector<Vertex>& verts, const std::vector<UINT>& idx);

    DX11Device* m_device;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_meshVS;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>  m_meshPS;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_shadowVS;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_shadowCmp;
    Microsoft::WRL::ComPtr<ID3D11InputLayout>  m_layout;
    Microsoft::WRL::ComPtr<ID3D11Buffer>       m_cb;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState>   m_rs;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_dsOn;
    Microsoft::WRL::ComPtr<ID3D11BlendState>        m_blendNone;

    Mesh m_plane;
    Mesh m_sphere;
    Mesh m_box;

    std::vector<Instance> m_instances;

    float m_ambient       = 0.18f;
    int   m_mode          = Mode_PBR;
    float m_rimAmount     = 0.6f;   // used by Cel mode
};
