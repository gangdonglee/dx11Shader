#pragma once
#include <windows.h>
#include <d3d11.h>

class App;

// Phase 0 DebugUI: ImGui lifecycle + a placeholder panel.
// Subsequent phases hang shader parameter panels off RenderPanels().
class DebugUI
{
public:
    DebugUI();
    ~DebugUI();

    bool Init(HWND hwnd, ID3D11Device* dev, ID3D11DeviceContext* ctx, App* app);
    void Shutdown();

    bool HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    bool WantCaptureMouse() const;
    bool WantCaptureKeyboard() const;

    void BeginFrame();
    void RenderPanels();
    void EndFrame();

private:
    static void ApplyDefault(App* a);
    static void ApplyRealisticSunset(App* a);
    static void ApplyBotWPlains(App* a);

    App* m_app;
    bool m_initialized;
};
