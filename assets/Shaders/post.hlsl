// Phase 5 post-process. Single fullscreen pass. Reads SceneColor +
// SceneDepth, applies depth-edge outlines, exponential distance fog,
// and lift/gamma/gain color grading. Writes to backbuffer.

cbuffer CB : register(b0)
{
    float4x4 InvViewProj;
    float3   EyePos;        float NearZ;
    float4   ScreenParams;  // x=invW, y=invH, z=near, w=far
    float4   Outline;       // x=enabled, y=strength, z=threshold
    float4   OutlineColor;
    float4   Fog;           // x=enabled, y=density, z=start
    float4   FogColor;
    float4   Grade;         // x=enabled, y=exposure, z=saturation, w=invGamma
    float4   Lift;
    float4   Gain;
    float4   Tonemap;       // x=enabled (ACES)
};

Texture2D<float4> SceneColor : register(t0);
Texture2D<float>  SceneDepth : register(t1);
SamplerState      SamLinear  : register(s0);
SamplerState      SamPoint   : register(s1);

struct VOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

VOut PostVS(uint id : SV_VertexID)
{
    VOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv  = uv;
    o.pos = float4(uv * 2.0f - 1.0f, 1.0f, 1.0f);
    o.pos.y = -o.pos.y;
    return o;
}

float3 ReconstructWorld(float2 uv, float depth)
{
    float2 ndc = uv * 2.0f - 1.0f;
    ndc.y = -ndc.y;
    float4 wp = mul(float4(ndc, depth, 1.0f), InvViewProj);
    return wp.xyz / wp.w;
}

float DepthEdge(float2 uv)
{
    float dx = ScreenParams.x;
    float dy = ScreenParams.y;
    float c  = SceneDepth.Sample(SamPoint, uv).x;
    float l  = SceneDepth.Sample(SamPoint, uv + float2(-dx, 0)).x;
    float r  = SceneDepth.Sample(SamPoint, uv + float2( dx, 0)).x;
    float u  = SceneDepth.Sample(SamPoint, uv + float2(0, -dy)).x;
    float d  = SceneDepth.Sample(SamPoint, uv + float2(0,  dy)).x;
    float ul = SceneDepth.Sample(SamPoint, uv + float2(-dx, -dy)).x;
    float ur = SceneDepth.Sample(SamPoint, uv + float2( dx, -dy)).x;
    float dl = SceneDepth.Sample(SamPoint, uv + float2(-dx,  dy)).x;
    float dr = SceneDepth.Sample(SamPoint, uv + float2( dx,  dy)).x;

    // Sobel
    float gx = (ur + 2.0f * r + dr) - (ul + 2.0f * l + dl);
    float gy = (dl + 2.0f * d + dr) - (ul + 2.0f * u + ur);
    float g  = sqrt(gx * gx + gy * gy);

    // Scale threshold by depth so far edges are still detected.
    float scale = 1.0f / max(1.0f - c, 0.005f);
    float edge  = saturate((g - Outline.z) * scale * 600.0f);
    return edge;
}

// Narkowicz-approximated ACES filmic. Compresses HDR linear into [0,1]
// with a believable shoulder/toe. Apply BEFORE grading so saturation and
// lift/gain operate in display-referred space.
float3 ACESFilm(float3 x)
{
    float a = 2.51f;
    float b = 0.03f;
    float c = 2.43f;
    float d = 0.59f;
    float e = 0.14f;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

float3 ApplyTonemapAndGrading(float3 col)
{
    // Exposure first (still in HDR linear).
    if (Grade.x > 0.5f)
        col *= Grade.y;

    // ACES tonemap: HDR -> LDR. SceneColor is float16 so values can be
    // far above 1; without this the swapchain just clamps.
    if (Tonemap.x > 0.5f)
        col = ACESFilm(col);
    else
        col = saturate(col);

    if (Grade.x > 0.5f)
    {
        float l = dot(col, float3(0.299, 0.587, 0.114));
        col = lerp(float3(l,l,l), col, Grade.z);        // saturation
        col = (col + Lift.rgb) * Gain.rgb;              // lift / gain
        col = pow(max(col, 0.0f), float3(Grade.w, Grade.w, Grade.w));  // gamma
    }
    return col;
}

float3 ApplyFog(float3 col, float depth, float2 uv)
{
    if (Fog.x < 0.5f) return col;
    if (depth >= 0.9999f) return col;       // sky — already the fog tint at horizon

    float3 wpos = ReconstructWorld(uv, depth);
    float dist  = length(wpos - EyePos);
    float t     = max(dist - Fog.z, 0.0f) * Fog.y;
    float fog   = saturate(1.0f - exp(-t));
    return lerp(col, FogColor.rgb, fog);
}

float4 PostPS(VOut i) : SV_Target
{
    float3 col   = SceneColor.Sample(SamLinear, i.uv).rgb;
    float  depth = SceneDepth.Sample(SamPoint, i.uv).x;

    col = ApplyFog(col, depth, i.uv);

    if (Outline.x > 0.5f)
    {
        float e = DepthEdge(i.uv) * Outline.y;
        col = lerp(col, OutlineColor.rgb, e);
    }

    col = ApplyTonemapAndGrading(col);

    return float4(col, 1.0f);
}
