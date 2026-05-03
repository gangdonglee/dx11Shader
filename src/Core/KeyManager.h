#pragma once

// KeyManager — central per-frame keyboard state.
//
// App feeds raw WM_KEYDOWN/WM_KEYUP into OnKeyDown/OnKeyUp. Subsystems
// (Map3D, RenderHealthBars, etc.) read state via IsDown / IsHeld /
// WasPressed / WasReleased. Edge queries are valid for the frame in
// which the event arrived; EndFrame() shifts state forward each tick.
class KeyManager
{
public:
    KeyManager();

    // Win32 event hooks. Caller passes virtual-key code (0x00-0xFF).
    void OnKeyDown(int vkey);
    void OnKeyUp(int vkey);

    // Call once per game tick AFTER all subsystems have read their keys.
    // Snapshots current state into prev so the next frame's edge queries
    // can compare.
    void EndFrame();

    // ImGui keyboard capture: when set true, IsDown / WasPressed / etc.
    // all report `false` so game shortcuts don't fire while typing into
    // an ImGui text field. Update once per frame from
    // `ImGui::GetIO().WantCaptureKeyboard`.
    void SetImGuiCapture(bool keyboardCaptured) { m_imguiCaptured = keyboardCaptured; }

    bool IsDown(int vkey) const;
    bool IsHeld(int vkey) const { return IsDown(vkey); }
    bool WasPressed(int vkey) const;   // down edge this frame
    bool WasReleased(int vkey) const;  // up edge this frame

private:
    static constexpr int kKeyCount = 256;
    bool m_curr[kKeyCount];
    bool m_prev[kKeyCount];
    bool m_imguiCaptured;
};
