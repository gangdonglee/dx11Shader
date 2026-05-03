#pragma once
#include "MathTypes.h"

// Single directional sun. Owned by App, shared by Scene / Water / Skybox.
//
// `version` increments on any state change so consumers (notably Skybox)
// can detect "sun moved → re-bake cubemap" without polling every field.
class DirectionalLight
{
public:
    DirectionalLight();

    // Setters bump the version on actual change so we don't dirty things
    // when ImGui slides the same value.
    void  SetYaw(float r);
    void  SetPitch(float r);
    void  SetIntensity(float i);
    void  SetColor(float r, float g, float b);

    float       GetYaw() const       { return m_yaw; }
    float       GetPitch() const     { return m_pitch; }
    float       GetIntensity() const { return m_intensity; }
    void        GetColor(float* r, float* g, float* b) const { *r=m_color[0]; *g=m_color[1]; *b=m_color[2]; }
    const float* GetColor() const    { return m_color; }

    // World-space direction the light TRAVELS (i.e., points from sun INTO
    // the scene). Surface→light is just -GetDirection().
    D3DXVECTOR3 GetDirection() const;

    unsigned    GetVersion() const   { return m_version; }

    // ImGui knobs (called from DebugUI). Mutates state and bumps version.
    void GuiPanel();

private:
    float    m_yaw;
    float    m_pitch;
    float    m_intensity;
    float    m_color[3];
    unsigned m_version;
};
