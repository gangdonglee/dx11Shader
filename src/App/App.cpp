#include "App.h"
#include <windowsx.h>
#include <chrono>

static App* s_pApp = nullptr;

// Halton low-discrepancy sequence sample (1-based index).
static float HaltonSample(int i, int base)
{
    float f = 1.0f, r = 0.0f;
    while (i > 0)
    {
        f /= (float)base;
        r += f * (float)(i % base);
        i /= base;
    }
    return r;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (s_pApp)
        return s_pApp->HandleMessage(hwnd, msg, wp, lp);
    return DefWindowProc(hwnd, msg, wp, lp);
}

App::App() : m_hwnd(nullptr) { s_pApp = this; }
App::~App() { Shutdown(); s_pApp = nullptr; }

LRESULT App::HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (m_debugUI.HandleMessage(hwnd, msg, wp, lp))
        return true;

    bool uiCaptureMouse = m_debugUI.WantCaptureMouse();

    switch (msg)
    {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_KEYDOWN:
        m_keys.OnKeyDown((int)wp);
        if (wp == VK_ESCAPE) DestroyWindow(hwnd);
        return 0;
    case WM_KEYUP:
        m_keys.OnKeyUp((int)wp);
        return 0;

    case WM_LBUTTONDOWN:
        if (!uiCaptureMouse) { m_lmbDown = true; SetCapture(hwnd); }
        m_lastMouseX = GET_X_LPARAM(lp); m_lastMouseY = GET_Y_LPARAM(lp);
        return 0;
    case WM_LBUTTONUP:
        m_lmbDown = false; ReleaseCapture();
        return 0;
    case WM_MBUTTONDOWN:
        if (!uiCaptureMouse) { m_mmbDown = true; SetCapture(hwnd); }
        m_lastMouseX = GET_X_LPARAM(lp); m_lastMouseY = GET_Y_LPARAM(lp);
        return 0;
    case WM_MBUTTONUP:
        m_mmbDown = false; ReleaseCapture();
        return 0;
    case WM_RBUTTONDOWN:
        if (!uiCaptureMouse) { m_rmbDown = true; SetCapture(hwnd); }
        m_lastMouseX = GET_X_LPARAM(lp); m_lastMouseY = GET_Y_LPARAM(lp);
        return 0;
    case WM_RBUTTONUP:
        m_rmbDown = false; ReleaseCapture();
        return 0;
    case WM_MOUSEMOVE:
    {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        int dx = x - m_lastMouseX;
        int dy = y - m_lastMouseY;
        m_lastMouseX = x; m_lastMouseY = y;
        if (uiCaptureMouse) return 0;

        bool shift = (wp & MK_SHIFT) != 0;
        if (m_lmbDown)
        {
            if (shift) m_camera.OnMousePan(dx, dy);
            else       m_camera.OnMouseRotate(dx, dy);
        }
        else if (m_mmbDown || m_rmbDown)
        {
            m_camera.OnMousePan(dx, dy);
        }
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (!uiCaptureMouse)
            m_camera.OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wp));
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

bool App::Init(HINSTANCE hInst, int nCmdShow)
{
    WNDCLASSEX wc = { sizeof(wc) };
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"ShaderAppClass";
    if (!RegisterClassEx(&wc)) { printf("[ERROR] RegisterClassEx\n"); return false; }

    RECT rc = { 0, 0, WIDTH, HEIGHT };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    m_hwnd = CreateWindowEx(0, wc.lpszClassName, L"Shader Lab",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, hInst, nullptr);
    if (!m_hwnd) { printf("[ERROR] CreateWindowEx\n"); return false; }

    if (!m_device.Init(m_hwnd, WIDTH, HEIGHT, true)) { printf("[ERROR] DX11Device\n"); return false; }
    if (!m_debugUI.Init(m_hwnd, m_device.GetDevice(), m_device.GetContext(), this)) { printf("[ERROR] DebugUI\n"); return false; }

    m_camera.SetAspect((float)WIDTH / (float)HEIGHT);
    m_camera.SetTarget(0.0f, 1.0f, 0.0f);

    if (!m_scene.Init(&m_device))  { printf("[ERROR] Scene\n"); return false; }
    if (!m_skybox.Init(&m_device)) { printf("[ERROR] Skybox\n"); return false; }
    if (!m_shadow.Init(&m_device, 2048)) { printf("[ERROR] ShadowMap\n"); return false; }
    if (!m_water.Init(&m_device))  { printf("[ERROR] Water\n"); return false; }
    if (!m_foam.Init(&m_device, 1024)) { printf("[ERROR] FoamMap\n"); return false; }
    if (!m_post.Init(&m_device))   { printf("[ERROR] PostProcess\n"); return false; }
    if (!m_taa.Init(&m_device, WIDTH, HEIGHT)) { printf("[ERROR] TAA\n"); return false; }
    if (!CreateSceneColorRT(WIDTH, HEIGHT)) { printf("[ERROR] SceneColor RT\n"); return false; }
    D3DXMatrixIdentity(&m_prevViewProj);

    ShowWindow(m_hwnd, nCmdShow);
    UpdateWindow(m_hwnd);
    printf("[OK] App::Init complete (%dx%d)\n", WIDTH, HEIGHT);
    return true;
}

int App::Run()
{
    using clock = std::chrono::high_resolution_clock;
    auto last = clock::now();
    float total = 0.0f;

    MSG msg = {};
    while (msg.message != WM_QUIT)
    {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg); DispatchMessage(&msg);
            continue;
        }

        auto now = clock::now();
        float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        if (dt > 0.1f) dt = 0.1f;
        total += dt;

        m_keys.SetImGuiCapture(m_debugUI.WantCaptureKeyboard());
        m_camera.Update(dt, &m_keys);
        m_water.Update(dt);

        // Foam map: world-space persistence buffer. Updated each frame
        // before the water draw so the SRV is fresh.
        {
            FoamMap::UpdateInputs fi;
            float ps = m_water.GetPlaneSize();
            fi.planeOriginX = -ps * 0.5f;
            fi.planeOriginZ = -ps * 0.5f;
            fi.planeSize    = ps;
            fi.waveAmp   = m_water.GetWaveAmp();
            fi.waveLen   = m_water.GetWaveLen();
            fi.waveSpeed = m_water.GetWaveSpeed();
            fi.waveSteep = m_water.GetWaveSteep();
            float wd = m_water.GetWindDir();
            fi.windCos = std::cos(wd);
            fi.windSin = std::sin(wd);
            fi.numWaves = m_water.GetNumWaves();
            fi.time = m_water.GetTime();
            m_foam.Update(fi);
        }

        ID3D11DeviceContext* ctx = m_device.GetContext();
        ID3D11DepthStencilView* dsv = m_device.GetDepthStencilView();
        ID3D11RenderTargetView* sceneRTV = m_sceneColorRTV.Get();

        // 0) Re-bake skybox if the sun changed.
        m_skybox.Update(m_sun);

        D3DXMATRIX view, proj;
        m_camera.GetView(&view);
        m_camera.GetProj(&proj);
        D3DXVECTOR3 eye = m_camera.GetEyePos();

        // Apply Halton(2,3) sub-pixel jitter to projection so TAA can
        // accumulate sub-pixel samples across frames.
        if (m_taa.m_enabled)
        {
            int j = (m_frameIdx % 16) + 1;
            float jx = (HaltonSample(j, 2) - 0.5f) * (2.0f / (float)WIDTH);
            float jy = (HaltonSample(j, 3) - 0.5f) * (2.0f / (float)HEIGHT);
            proj.m[2][0] += jx;
            proj.m[2][1] += jy;
        }

        // 0.5) Shadow pass — render scene depth from light POV.
        if (m_shadow.m_enabled)
        {
            m_shadow.BeginPass(m_sun);
            m_scene.RenderShadowDepth(m_shadow.GetLightViewProj());
            m_shadow.EndPass();
        }

        // 1) Bind SceneColor RT + writable depth, clear depth (skybox writes
        //    every color pixel so a color clear is wasted).
        ctx->OMSetRenderTargets(1, &sceneRTV, dsv);
        ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

        // Reusable inverse view-proj for sky + post.
        D3DXMATRIX vp = view * proj;
        DirectX::XMMATRIX xmVP = DirectX::XMLoadFloat4x4(reinterpret_cast<const DirectX::XMFLOAT4X4*>(&vp));
        DirectX::XMMATRIX xmInvVP = DirectX::XMMatrixInverse(nullptr, xmVP);
        D3DXMATRIX invVP;
        DirectX::XMStoreFloat4x4(reinterpret_cast<DirectX::XMFLOAT4X4*>(&invVP), xmInvVP);

        // 2) Skybox background (writes every pixel, depth disabled).
        m_skybox.Render(invVP, eye);

        // 3) Scene meshes (depth-on, sun from DirectionalLight, shadow sampled).
        m_scene.Render(view, proj, eye, total, m_sun,
                       m_shadow.m_enabled ? &m_shadow : nullptr);

        // 4) Snapshot SceneColor for refraction sampling.
        ctx->CopyResource(m_sceneColorCopyTex.Get(), m_sceneColorTex.Get());
        ctx->OMSetRenderTargets(1, &sceneRTV, dsv);

        // 5) Water — uses skybox cubemap for reflection.
        D3DXVECTOR3 lightDir = m_sun.GetDirection();
        const float* sunCol  = m_sun.GetColor();
        float lightCol[3] = { sunCol[0] * m_sun.GetIntensity(),
                              sunCol[1] * m_sun.GetIntensity(),
                              sunCol[2] * m_sun.GetIntensity() };
        m_water.Render(view, proj, eye, lightDir, lightCol,
                       m_sceneColorCopySRV.Get(), m_device.GetDepthSRV(),
                       m_skybox.GetCubeSRV(),
                       m_foam.GetSRV(),
                       m_foam.GetPlaneOriginX(),
                       m_foam.GetPlaneOriginZ(),
                       m_foam.GetPlaneSize(),
                       (float)WIDTH, (float)HEIGHT);

        // 6) TAA resolve: blends SceneColor with the reprojected
        //    previous-frame history. Output replaces SceneColor as the
        //    input to PostProcess.
        D3DXMATRIX curVP    = view * proj;   // already includes jitter
        DirectX::XMMATRIX xmCurVP = DirectX::XMLoadFloat4x4(reinterpret_cast<const DirectX::XMFLOAT4X4*>(&curVP));
        DirectX::XMMATRIX xmInvCurVP = DirectX::XMMatrixInverse(nullptr, xmCurVP);
        D3DXMATRIX invCurVP;
        DirectX::XMStoreFloat4x4(reinterpret_cast<DirectX::XMFLOAT4X4*>(&invCurVP), xmInvCurVP);
        D3DXMATRIX prevFromCur = invCurVP * m_prevViewProj;
        m_taa.Resolve(m_sceneColorSRV.Get(), m_device.GetDepthSRV(),
                      prevFromCur, eye, (float)WIDTH, (float)HEIGHT,
                      m_firstFrame);

        // 7) Post-process samples TAA output (or SceneColor if disabled).
        ID3D11RenderTargetView* bbRTV = m_device.GetBackBufferRTV();
        ctx->OMSetRenderTargets(1, &bbRTV, nullptr);
        ID3D11ShaderResourceView* postInput = m_taa.m_enabled
            ? m_taa.GetOutputSRV()
            : m_sceneColorSRV.Get();
        m_post.Render(postInput, m_device.GetDepthSRV(),
                      invVP, eye, (float)WIDTH, (float)HEIGHT,
                      0.1f, 500.0f);

        // ImGui draws on top of the post-processed backbuffer.

        m_debugUI.BeginFrame();
        m_debugUI.RenderPanels();
        m_debugUI.EndFrame();

        m_device.EndFrame();
        m_device.Present();

        // End of frame: copy TAA output → history for next frame, save
        // current view-proj as previous, advance jitter index.
        m_taa.Swap();
        m_prevViewProj = curVP;
        m_firstFrame = false;
        ++m_frameIdx;

        m_keys.EndFrame();
    }
    return (int)msg.wParam;
}

void App::Shutdown()
{
    ReleaseSceneColorRT();
    m_taa.Shutdown();
    m_post.Shutdown();
    m_foam.Shutdown();
    m_water.Shutdown();
    m_shadow.Shutdown();
    m_skybox.Shutdown();
    m_scene.Shutdown();
    m_debugUI.Shutdown();
    m_device.Shutdown();
    if (m_hwnd) { DestroyWindow(m_hwnd); m_hwnd = nullptr; }
}

bool App::CreateSceneColorRT(int w, int h)
{
    ID3D11Device* dev = m_device.GetDevice();

    // HDR scene color: float16 so specular/sun glint can exceed 1.0; the
    // PostProcess pass tonemaps it back to LDR for the swapchain.
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w; td.Height = h;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &m_sceneColorTex))) return false;
    if (FAILED(dev->CreateRenderTargetView (m_sceneColorTex.Get(), nullptr, &m_sceneColorRTV))) return false;
    if (FAILED(dev->CreateShaderResourceView(m_sceneColorTex.Get(), nullptr, &m_sceneColorSRV))) return false;

    // Copy target — water samples this while writing into m_sceneColorTex.
    D3D11_TEXTURE2D_DESC cd = td;
    cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&cd, nullptr, &m_sceneColorCopyTex))) return false;
    if (FAILED(dev->CreateShaderResourceView(m_sceneColorCopyTex.Get(), nullptr, &m_sceneColorCopySRV))) return false;
    return true;
}

void App::ReleaseSceneColorRT()
{
    m_sceneColorCopySRV.Reset();
    m_sceneColorCopyTex.Reset();
    m_sceneColorSRV.Reset();
    m_sceneColorRTV.Reset();
    m_sceneColorTex.Reset();
}
