#include "DirectionalLight.h"
#include "imgui.h"
#include <cmath>

DirectionalLight::DirectionalLight()
    : m_yaw(0.6f),
      m_pitch(0.55f),
      m_intensity(1.0f),
      m_version(1)
{
    m_color[0] = 1.00f;
    m_color[1] = 0.96f;
    m_color[2] = 0.88f;
}

void DirectionalLight::SetYaw(float r)
{
    if (r != m_yaw) { m_yaw = r; ++m_version; }
}

void DirectionalLight::SetPitch(float r)
{
    if (r != m_pitch) { m_pitch = r; ++m_version; }
}

void DirectionalLight::SetIntensity(float i)
{
    if (i != m_intensity) { m_intensity = i; ++m_version; }
}

void DirectionalLight::SetColor(float r, float g, float b)
{
    if (r != m_color[0] || g != m_color[1] || b != m_color[2])
    {
        m_color[0] = r; m_color[1] = g; m_color[2] = b;
        ++m_version;
    }
}

D3DXVECTOR3 DirectionalLight::GetDirection() const
{
    float cp = std::cos(m_pitch), sp = std::sin(m_pitch);
    float cy = std::cos(m_yaw),   sy = std::sin(m_yaw);
    return D3DXVECTOR3(cp * sy, -sp, cp * cy);
}

void DirectionalLight::GuiPanel()
{
    if (!ImGui::CollapsingHeader("Sun / Directional Light", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    float y = m_yaw, p = m_pitch, i = m_intensity;
    float c[3] = { m_color[0], m_color[1], m_color[2] };

    if (ImGui::SliderAngle("Yaw##sun",   &y, -180.0f, 180.0f)) SetYaw(y);
    if (ImGui::SliderAngle("Pitch##sun", &p,    1.0f,  89.0f)) SetPitch(p);
    if (ImGui::SliderFloat("Intensity##sun", &i, 0.0f, 4.0f))  SetIntensity(i);
    if (ImGui::ColorEdit3 ("Color##sun", c))                    SetColor(c[0], c[1], c[2]);
}
