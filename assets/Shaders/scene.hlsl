// Scene shader.
//   MeshVS / MeshPS — three lighting modes selected at runtime by Params.x:
//       0 = Lambert (Phase 1 default)
//       1 = PBR / GGX (realistic mode)
//       2 = Cel / Toon (BotW mode)
//   SkyVS  / SkyPS  — fullscreen procedural sky (gradient + sun).
//
// Phase 3+ leaves this file alone and adds water on top via separate passes.

cbuffer CB : register(b0)
{
    float4x4 World;
    float4x4 ViewProj;
    float4x4 InvViewProj;
    float4x4 LightViewProj;
    float3   EyePos;     float Time;
    float3   LightDir;   float Ambient;     // LightDir points INTO the scene
    float3   LightColor; float pad0;
    float4   BaseColor;
    float4   Params;     // x=mode, y=metallic, z=roughness, w=rim/cel intensity
    float4   ShadowParams; // x=enabled, y=depthBias, z=normalBias
};

Texture2D<float>      ShadowMap : register(t0);
SamplerComparisonState ShadowCmp : register(s0);

struct VSIn  { float3 pos : POSITION; float3 nor : NORMAL; float2 uv : TEXCOORD0; };
struct VSOut { float4 pos : SV_POSITION; float3 wpos : TEXCOORD0; float3 nor : TEXCOORD1; float2 uv : TEXCOORD2; };

VSOut MeshVS(VSIn i)
{
    VSOut o;
    float4 wp = mul(float4(i.pos, 1.0f), World);
    o.wpos = wp.xyz;
    o.pos  = mul(wp, ViewProj);
    o.nor  = mul((float3x3)World, i.nor);
    o.uv   = i.uv;
    return o;
}

// Depth-only entry for the shadow map pass. ViewProj here is the
// directional light's view-projection (App writes it before each
// shadow draw). No pixel shader is bound.
float4 ShadowVS(VSIn i) : SV_POSITION
{
    float4 wp = mul(float4(i.pos, 1.0f), World);
    return mul(wp, ViewProj);
}

// PCF 3x3 shadow sample. Returns 1 = lit, 0 = shadowed.
float SampleShadow(float3 worldPos, float3 N)
{
    if (ShadowParams.x < 0.5f) return 1.0f;
    // Normal-bias: slide the world position along the surface normal a bit
    // before projecting, to push it out of self-shadow acne.
    float3 biased = worldPos + N * ShadowParams.z;
    float4 ls = mul(float4(biased, 1.0f), LightViewProj);
    ls.xyz /= ls.w;
    float2 uv = ls.xy * 0.5f + 0.5f;
    uv.y = 1.0f - uv.y;
    if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f) return 1.0f;
    float refZ = ls.z - ShadowParams.y;

    // 3x3 PCF
    float sum = 0.0f;
    float texel = 1.0f / 2048.0f;   // matches default ShadowMap::Init size
    [unroll] for (int yy = -1; yy <= 1; ++yy)
    [unroll] for (int xx = -1; xx <= 1; ++xx)
    {
        float2 o = float2(xx, yy) * texel;
        sum += ShadowMap.SampleCmpLevelZero(ShadowCmp, uv + o, refZ);
    }
    return sum * (1.0f / 9.0f);
}

// ------------ helpers ----------------------------------------------------

static const float PI = 3.14159265f;

float3 FresnelSchlick(float cosT, float3 F0)
{
    return F0 + (1.0f - F0) * pow(saturate(1.0f - cosT), 5.0f);
}

float DistributionGGX(float3 N, float3 H, float roughness)
{
    float a  = roughness * roughness;
    float a2 = a * a;
    float NoH = saturate(dot(N, H));
    float d   = (NoH * NoH) * (a2 - 1.0f) + 1.0f;
    return a2 / (PI * d * d + 1e-5f);
}

float GeometrySchlickGGX(float NoV, float roughness)
{
    float r = roughness + 1.0f;
    float k = (r * r) * 0.125f;     // (r^2)/8 for direct lighting
    return NoV / (NoV * (1.0f - k) + k + 1e-5f);
}

float GeometrySmith(float3 N, float3 V, float3 L, float roughness)
{
    return GeometrySchlickGGX(saturate(dot(N, V)), roughness)
         * GeometrySchlickGGX(saturate(dot(N, L)), roughness);
}

// 4-step quantized N·L band, soft transitions for cel look.
float CelBand(float ndl)
{
    float t = saturate(ndl);
    if (t < 0.10f) return 0.10f;
    if (t < 0.45f) return 0.40f;
    if (t < 0.75f) return 0.70f;
    return 1.00f;
}

// ------------ pixel shaders ---------------------------------------------

float4 MeshPS(VSOut i) : SV_Target
{
    float3 N = normalize(i.nor);
    float3 V = normalize(EyePos - i.wpos);
    float3 L = normalize(-LightDir);
    float3 H = normalize(L + V);
    float  NdotL = saturate(dot(N, L));
    float  NdotV = saturate(dot(N, V));

    int   mode      = (int)Params.x;
    float metallic  = saturate(Params.y);
    float roughness = clamp(Params.z, 0.04f, 1.0f);
    float rimAmt    = Params.w;

    // Sun shadow (1 = lit, 0 = shadowed). Multiplies the direct lighting term
    // only; ambient/rim/cel-fill stay so geometry inside shadow keeps shape.
    float shadow = SampleShadow(i.wpos, N);

    float3 outCol = float3(0,0,0);

    if (mode == 0)
    {
        // ---------- Lambert ----------
        float3 diffuse = BaseColor.rgb * NdotL * LightColor * shadow;
        outCol = diffuse + BaseColor.rgb * Ambient;
    }
    else if (mode == 1)
    {
        // ---------- PBR / GGX ----------
        float3 F0 = lerp(float3(0.04, 0.04, 0.04), BaseColor.rgb, metallic);
        float  D  = DistributionGGX(N, H, roughness);
        float  G  = GeometrySmith(N, V, L, roughness);
        float3 F  = FresnelSchlick(saturate(dot(H, V)), F0);

        float3 spec = (D * G * F) / (4.0f * NdotV * NdotL + 1e-4f);
        float3 kS   = F;
        float3 kD   = (1.0f - kS) * (1.0f - metallic);

        float3 direct  = (kD * BaseColor.rgb / PI + spec) * LightColor * NdotL * shadow;
        float3 ambient = BaseColor.rgb * Ambient * (1.0f - metallic * 0.5f);
        outCol = direct + ambient;
    }
    else // mode == 2 — Cel / Toon
    {
        float band = CelBand(NdotL) * shadow;
        float3 lit  = BaseColor.rgb * lerp(float3(0.55,0.55,0.62), float3(1,1,1), band) * LightColor;

        // Soft specular highlight as a stepped band
        float spec = pow(saturate(dot(N, H)), lerp(64.0f, 8.0f, roughness));
        float specBand = step(0.6f, spec) * shadow;
        lit += specBand * LightColor * (1.0f - metallic * 0.6f) * 0.5f;

        // Rim light
        float rim = pow(1.0f - NdotV, 3.0f);
        lit += rim * rimAmt * LightColor * 0.6f;

        outCol = lit + BaseColor.rgb * Ambient * 0.6f;
    }

    return float4(outCol, BaseColor.a);
}

// Sky background is now drawn by Skybox (skybox.hlsl) using a baked
// cubemap. The previous in-line SkyVS/SkyPS lived here.
