// Renders the cubemap as a sky background. Fullscreen triangle, ray
// reconstructed from screen UV via InvViewProj, sampled from the
// cubemap built by Skybox::BakeCubemap.

cbuffer CB : register(b0)
{
    float4x4 InvViewProj;
    float4   EyePos;
};

TextureCube  SkyCube  : register(t0);
SamplerState SamCube  : register(s0);

struct VOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

VOut SkyVS(uint id : SV_VertexID)
{
    VOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv  = uv;
    o.pos = float4(uv * 2.0f - 1.0f, 1.0f, 1.0f);
    o.pos.y = -o.pos.y;
    return o;
}

float4 SkyPS(VOut i) : SV_Target
{
    float2 ndc = i.uv * 2.0f - 1.0f;
    ndc.y = -ndc.y;
    float4 farClip  = float4(ndc, 1.0f, 1.0f);
    float4 farWorld = mul(farClip, InvViewProj);
    float3 wpos     = farWorld.xyz / farWorld.w;
    float3 dir      = normalize(wpos - EyePos.xyz);

    float3 col = SkyCube.Sample(SamCube, dir).rgb;
    return float4(col, 1.0f);
}
