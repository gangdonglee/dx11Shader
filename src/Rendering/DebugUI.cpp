#include "DebugUI.h"
#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"
#include "App.h"
#include "Camera.h"
#include "DirectionalLight.h"
#include "Scene.h"
#include "Skybox.h"
#include "ShadowMap.h"
#include "Water.h"
#include "FoamMap.h"
#include "PostProcess.h"
#include "TAA.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ---- presets --------------------------------------------------------------
// Each preset is a one-click "recognizable look". Sliders still work after.

void DebugUI::ApplyDefault(App* a)
{
    a->m_scene.SetMode(Scene::Mode_PBR);
    a->m_sun.SetYaw(0.6f);
    a->m_sun.SetPitch(0.55f);
    a->m_sun.SetIntensity(1.0f);
    a->m_sun.SetColor(1.00f, 0.96f, 0.88f);

    a->m_post.m_outlineEnabled = false;
    a->m_post.m_fogEnabled     = true;
    a->m_post.m_fogDensity     = 0.020f;
    a->m_post.m_fogStart       = 6.0f;
    float fog[3] = { 0.78f, 0.86f, 0.96f };
    for (int i=0;i<3;++i) a->m_post.m_fogColor[i] = fog[i];
    a->m_post.m_gradeEnabled   = true;
    a->m_post.m_exposure       = 1.0f;
    a->m_post.m_saturation     = 1.05f;
    a->m_post.m_gamma          = 1.0f;
}

void DebugUI::ApplyRealisticSunset(App* a)
{
    a->m_scene.SetMode(Scene::Mode_PBR);
    a->m_sun.SetYaw(2.85f);
    a->m_sun.SetPitch(0.12f);
    a->m_sun.SetIntensity(1.2f);
    a->m_sun.SetColor(1.00f, 0.78f, 0.55f);

    a->m_water.SetWaterY(0.30f);
    a->m_post.m_outlineEnabled = false;
    a->m_post.m_fogEnabled     = true;
    a->m_post.m_fogDensity     = 0.025f;
    a->m_post.m_fogStart       = 3.0f;
    float fog[3] = { 0.95f, 0.62f, 0.42f };
    for (int i=0;i<3;++i) a->m_post.m_fogColor[i] = fog[i];

    a->m_post.m_gradeEnabled = true;
    a->m_post.m_exposure     = 1.25f;
    a->m_post.m_saturation   = 1.10f;
    a->m_post.m_gamma        = 1.05f;
    float lift[3] = { 0.04f, 0.00f, -0.03f };
    float gain[3] = { 1.10f, 0.98f, 0.85f };
    for (int i=0;i<3;++i) { a->m_post.m_lift[i] = lift[i]; a->m_post.m_gain[i] = gain[i]; }
}

void DebugUI::ApplySwimmingPool(App* a)
{
    // Crystal-clear shallow water: minimal absorption + scatter, tight
    // fresnel so refraction dominates from above, calm waves, water
    // raised so the box/sphere row is partially submerged.
    a->m_scene.SetMode(Scene::Mode_PBR);
    a->m_sun.SetYaw(0.40f);
    a->m_sun.SetPitch(0.85f);          // near-overhead sun
    a->m_sun.SetIntensity(1.15f);
    a->m_sun.SetColor(1.00f, 0.98f, 0.92f);

    a->m_water.SetWaterY(1.40f);       // submerge boxes / partial spheres
    a->m_water.SetWaveAmp(0.04f);
    a->m_water.SetFresnelPow(10.0f);
    a->m_water.SetSkyTint(0.55f);
    a->m_water.SetSsrEnabled(false);
    a->m_water.SetRefractStrength(0.012f);
    a->m_water.SetExtinction(0.05f, 0.02f, 0.01f);
    a->m_water.SetScatterStrength(0.20f);

    a->m_post.m_outlineEnabled = false;
    a->m_post.m_fogEnabled     = false;     // intimate pool — no atmospheric fog
    a->m_post.m_gradeEnabled   = true;
    a->m_post.m_exposure       = 1.10f;
    a->m_post.m_saturation     = 1.10f;
    a->m_post.m_gamma          = 1.0f;
    float lift[3] = { 0.0f, 0.01f, 0.02f };
    float gain[3] = { 1.0f, 1.05f, 1.05f };
    for (int i=0;i<3;++i) { a->m_post.m_lift[i] = lift[i]; a->m_post.m_gain[i] = gain[i]; }
}

void DebugUI::ApplyBotWPlains(App* a)
{
    a->m_scene.SetMode(Scene::Mode_Cel);
    a->m_sun.SetYaw(0.45f);
    a->m_sun.SetPitch(0.65f);
    a->m_sun.SetIntensity(1.0f);
    a->m_sun.SetColor(1.00f, 0.98f, 0.92f);

    a->m_post.m_outlineEnabled  = true;
    a->m_post.m_outlineStrength = 0.85f;
    a->m_post.m_outlineThresh   = 0.0006f;
    float outCol[3] = { 0.04f, 0.05f, 0.08f };
    for (int i=0;i<3;++i) a->m_post.m_outlineColor[i] = outCol[i];

    a->m_post.m_fogEnabled = true;
    a->m_post.m_fogDensity = 0.012f;
    a->m_post.m_fogStart   = 12.0f;
    float fog[3] = { 0.88f, 0.92f, 0.96f };
    for (int i=0;i<3;++i) a->m_post.m_fogColor[i] = fog[i];

    a->m_post.m_gradeEnabled = true;
    a->m_post.m_exposure     = 1.05f;
    a->m_post.m_saturation   = 1.20f;
    a->m_post.m_gamma        = 1.0f;
    float lift[3] = { 0.0f, 0.02f, 0.04f };
    float gain[3] = { 1.05f, 1.10f, 1.05f };
    for (int i=0;i<3;++i) { a->m_post.m_lift[i] = lift[i]; a->m_post.m_gain[i] = gain[i]; }
}

DebugUI::DebugUI()
    : m_app(nullptr), m_initialized(false)
{
}

DebugUI::~DebugUI()
{
    Shutdown();
}

bool DebugUI::Init(HWND hwnd, ID3D11Device* dev, ID3D11DeviceContext* ctx, App* app)
{
    if (m_initialized) return true;
    m_app = app;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    if (!ImGui_ImplWin32_Init(hwnd)) return false;
    if (!ImGui_ImplDX11_Init(dev, ctx)) return false;

    m_initialized = true;
    return true;
}

void DebugUI::Shutdown()
{
    if (!m_initialized) return;
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    m_initialized = false;
}

bool DebugUI::HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (!m_initialized) return false;
    return ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp) != 0;
}

bool DebugUI::WantCaptureMouse() const
{
    return m_initialized && ImGui::GetIO().WantCaptureMouse;
}

bool DebugUI::WantCaptureKeyboard() const
{
    return m_initialized && ImGui::GetIO().WantCaptureKeyboard;
}

void DebugUI::BeginFrame()
{
    if (!m_initialized) return;
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

void DebugUI::RenderPanels()
{
    if (!m_initialized) return;

    ImGui::Begin("Shader Lab");
    ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
    if (m_app)
    {
        D3DXVECTOR3 eye = m_app->m_camera.GetEyePos();
        D3DXVECTOR3 tgt = m_app->m_camera.GetTarget();
        ImGui::Text("Eye    %.2f %.2f %.2f", eye.x, eye.y, eye.z);
        ImGui::Text("Target %.2f %.2f %.2f", tgt.x, tgt.y, tgt.z);
        ImGui::Text("Yaw %.2f  Pitch %.2f  Dist %.2f",
                    m_app->m_camera.GetYaw(),
                    m_app->m_camera.GetPitch(),
                    m_app->m_camera.GetDistance());
        ImGui::Separator();
        ImGui::Text("Presets");
        if (ImGui::Button("Default"))           ApplyDefault(m_app);
        ImGui::SameLine();
        if (ImGui::Button("Realistic Sunset"))  ApplyRealisticSunset(m_app);
        ImGui::SameLine();
        if (ImGui::Button("BotW Plains"))       ApplyBotWPlains(m_app);
        ImGui::SameLine();
        if (ImGui::Button("Swimming Pool"))     ApplySwimmingPool(m_app);
        ImGui::Separator();
        m_app->m_sun.GuiPanel();
        ImGui::Separator();
        m_app->m_skybox.GuiPanel();
        ImGui::Separator();
        m_app->m_shadow.GuiPanel();
        ImGui::Separator();
        m_app->m_scene.GuiPanel();
        ImGui::Separator();
        m_app->m_water.GuiPanel();
        ImGui::Separator();
        m_app->m_foam.GuiPanel();
        ImGui::Separator();
        m_app->m_taa.GuiPanel();
        ImGui::Separator();
        m_app->m_post.GuiPanel();
    }
    ImGui::End();
}

void DebugUI::EndFrame()
{
    if (!m_initialized) return;
    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}
