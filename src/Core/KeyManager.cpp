#include "KeyManager.h"
#include <cstring>

KeyManager::KeyManager()
    : m_imguiCaptured(false)
{
    std::memset(m_curr, 0, sizeof(m_curr));
    std::memset(m_prev, 0, sizeof(m_prev));
}

void KeyManager::OnKeyDown(int vkey)
{
    if (vkey < 0 || vkey >= kKeyCount) return;
    m_curr[vkey] = true;
}

void KeyManager::OnKeyUp(int vkey)
{
    if (vkey < 0 || vkey >= kKeyCount) return;
    m_curr[vkey] = false;
}

void KeyManager::EndFrame()
{
    std::memcpy(m_prev, m_curr, sizeof(m_curr));
}

bool KeyManager::IsDown(int vkey) const
{
    if (m_imguiCaptured) return false;
    if (vkey < 0 || vkey >= kKeyCount) return false;
    return m_curr[vkey];
}

bool KeyManager::WasPressed(int vkey) const
{
    if (m_imguiCaptured) return false;
    if (vkey < 0 || vkey >= kKeyCount) return false;
    return m_curr[vkey] && !m_prev[vkey];
}

bool KeyManager::WasReleased(int vkey) const
{
    if (m_imguiCaptured) return false;
    if (vkey < 0 || vkey >= kKeyCount) return false;
    return !m_curr[vkey] && m_prev[vkey];
}
