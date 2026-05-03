// Bakes the procedural sky into one face of a cubemap. Driven from C++
// by Skybox::BakeCubemap, which draws 6 fullscreen triangles, one per
// face index in the FaceIdx CB.

cbuffer CB : register(b0)
{
    float4 FaceIdx;     // x = face index (0..5)
    float4 LightDir;    // xyz = direction the sun travels (INTO scene)
    float4 LightColor;  // xyz = color * intensity
    float4 Horizon;     // xyz = horizon band color
    float4 Zenith;      // xyz = zenith band color
    float4 Ground;      // xyz = below-horizon ground color
};

struct VOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

VOut BakeVS(uint id : SV_VertexID)
{
    VOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv  = uv;
    o.pos = float4(uv * 2.0f - 1.0f, 0.0f, 1.0f);
    return o;
}

// DX cubemap face direction lookup. uv is the face's [0,1] coords.
float3 FaceDirection(int face, float2 uv)
{
    float2 c = uv * 2.0f - 1.0f;     // [-1, 1]
    if (face == 0) return normalize(float3( 1.0f, -c.y, -c.x));   // +X
    if (face == 1) return normalize(float3(-1.0f, -c.y,  c.x));   // -X
    if (face == 2) return normalize(float3( c.x,  1.0f,  c.y));   // +Y
    if (face == 3) return normalize(float3( c.x, -1.0f, -c.y));   // -Y
    if (face == 4) return normalize(float3( c.x, -c.y,  1.0f));   // +Z
    return            normalize(float3(-c.x, -c.y, -1.0f));        // -Z
}

float3 ProceduralSky(float3 dir, float3 lightDir, float3 lightColor)
{
    float3 sky = (dir.y >= 0.0f)
        ? lerp(Horizon.rgb, Zenith.rgb, pow(saturate(dir.y), 0.5f))
        : lerp(Horizon.rgb, Ground.rgb, saturate(-dir.y * 3.0f));

    float3 sunDir = normalize(-lightDir);
    float  cs     = saturate(dot(dir, sunDir));
    float  glow   = pow(cs, 64.0f);
    float  disc   = smoothstep(0.9985f, 0.9998f, cs);
    sky += lightColor * (glow * 0.6f + disc * 4.0f);

    return sky;
}

float4 BakePS(VOut i) : SV_Target
{
    int face = (int)FaceIdx.x;
    float3 dir = FaceDirection(face, i.uv);
    float3 col = ProceduralSky(dir, LightDir.xyz, LightColor.xyz);
    return float4(col, 1.0f);
}
