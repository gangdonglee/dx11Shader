#pragma once
#include <windows.h>
#include <wrl/client.h>
#include "DX11Device.h"
#include "KeyManager.h"
#include "Camera.h"
#include "DirectionalLight.h"
#include "Scene.h"
#include "Skybox.h"
#include "ShadowMap.h"
#include "Water.h"
#include "FoamMap.h"
#include "PostProcess.h"
#include "TAA.h"
#include "DebugUI.h"

// Phase 1 App: window + DX11 + ImGui + camera + a few meshes + sky.
// Subsequent phases hang water/post-processing off this same skeleton.
class App
{
    friend class DebugUI;

public:
    App();
    ~App();

    bool Init(HINSTANCE hInst, int nCmdShow);
    int  Run();
    void Shutdown();

    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    static const int WIDTH  = 1920;
    static const int HEIGHT = 1080;

private:
    bool CreateSceneColorRT(int w, int h);
    void ReleaseSceneColorRT();

    HWND        m_hwnd;
    DX11Device  m_device;
    KeyManager  m_keys;
    Camera           m_camera;
    DirectionalLight m_sun;
    Skybox           m_skybox;
    ShadowMap        m_shadow;
    Scene            m_scene;
    Water            m_water;
    FoamMap          m_foam;
    PostProcess      m_post;
    TAA              m_taa;
    DebugUI          m_debugUI;

    // Temporal antialiasing state — saved between frames so TAA can
    // reproject history forward by one frame.
    D3DXMATRIX m_prevViewProj;
    int        m_frameIdx   = 0;
    bool       m_firstFrame = true;

    // Phase 3-2 — offscreen RT used as the active color target while
    // scene + water draw, so water can sample a snapshot of the scene
    // for refraction. A final CopyResource lands it on the backbuffer.
    Microsoft::WRL::ComPtr<ID3D11Texture2D>          m_sceneColorTex;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView>   m_sceneColorRTV;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_sceneColorSRV;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>          m_sceneColorCopyTex;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_sceneColorCopySRV;

    // Mouse drag state
    bool m_lmbDown = false;
    bool m_mmbDown = false;
    bool m_rmbDown = false;
    int  m_lastMouseX = 0;
    int  m_lastMouseY = 0;
};
