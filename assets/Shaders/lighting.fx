Texture2D gGBuffer0 : register(t0); // albedo + roughness (phase 2: only albedo used)
Texture2D gGBuffer1 : register(t1); // normal (future phase)
Texture2D gGBuffer2 : register(t2); // motion (future phase)
Texture2D gGBuffer3 : register(t3); // misc (future phase)
SamplerState gLinearClamp : register(s0);

cbuffer LightingCB : register(b0)
{
    float3 gLightDirWS;
    float  gAmbient;
    float3 gLightColor;
    float  gExposure;
};

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut VS_FullScreen(uint vid : SV_VertexID)
{
    VSOut o;
    float2 p;
    p.x = (vid == 2) ? 3.0 : -1.0;
    p.y = (vid == 1) ? 3.0 : -1.0;
    o.pos = float4(p, 0.0, 1.0);
    o.uv = float2(0.5 * (p.x + 1.0), 1.0 - 0.5 * (p.y + 1.0));
    return o;
}

float3 DecodeOcta(float2 e)
{
    float3 v = float3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
    if (v.z < 0.0)
    {
        float2 signNotZero = float2(
            (v.x >= 0.0) ? 1.0 : -1.0,
            (v.y >= 0.0) ? 1.0 : -1.0);
        v.xy = (1.0 - abs(v.yx)) * signNotZero;
    }
    return normalize(v);
}

float4 PS_Lighting(VSOut i) : SV_Target
{
    float4 albedo = gGBuffer0.Sample(gLinearClamp, i.uv);
    float4 normalPacked = gGBuffer1.Sample(gLinearClamp, i.uv);

    float3 n;
    if (normalPacked.a > 0.001)
    {
        float2 enc = normalPacked.xy * 2.0 - 1.0;
        n = DecodeOcta(enc);
    }
    else
    {
        // Phase 2 fallback: if no normal was written yet, keep a stable up-normal.
        n = float3(0.0, 1.0, 0.0);
    }

    float3 L = normalize(-gLightDirWS);
    float ndl = saturate(dot(n, L));
    float3 lit = albedo.rgb * (gAmbient + ndl * gLightColor);
    return float4(lit * gExposure, 1.0);
}
