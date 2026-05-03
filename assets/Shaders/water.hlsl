// Phase 3 water shader.
//   WaterVS — sums up to 8 Gerstner waves to displace the grid quad and
//             emit world-space position, normal, and per-vertex foam term.
//   WaterPS — Fresnel-blended (deep tint <-> sky reflection) shading with
//             a tight specular highlight from the same directional light.
//
// Phase 4 hooks in here later: SSR replaces or augments the inline sky
// reflection, and a SceneColor SRV gets sampled for refraction.

cbuffer CB : register(b0)
{
    float4x4 World;
    float4x4 ViewProj;
    float4x4 InvViewProj;
    float3   EyePos;     float Time;
    float3   LightDir;   float Pad0;        // light points INTO the scene
    float3   LightColor; float SkyTint;
    float4   Shallow;
    float4   Deep;
    float4   WaveParams;   // x=amp, y=wavelength, z=speed, w=steepness
    float4   WindParams;   // x=cos(dir), y=sin(dir), z=numWaves, w=fresnelPow
    float4   ExtraParams;  // x=specPower, y=refractStrength, z=unused, w=hasSceneTex
    float4   ScreenParams; // x=invScreenW, y=invScreenH
    float4   SsrParams;    // x=enabled, y=steps, z=stepLen, w=thickness
    float4   ExtraParams2; // x=causticStrength, y=causticScale, z=sssStrength
    float4   Extinction;   // xyz = sigma RGB (m^-1), w = scatter strength
    float4   DetailParams; // x=enabled, y=strength, z=reflRoughness, w=skyMaxMip
    float4   DetailScales; // xyz = per-layer tile freq
    float4   FoamPlane;    // x=originX, y=originZ, z=size, w=invSize
};

Texture2D<float4> SceneColor   : register(t0);
Texture2D<float>  SceneDepth   : register(t1);
TextureCube       SkyCube      : register(t2);
Texture2D<float4> DetailNormal : register(t3);
Texture2D<float>  FoamMapTex   : register(t4);
SamplerState      SamLinear    : register(s0);
SamplerState      SamPoint     : register(s1);
SamplerState      SamLinearWrap: register(s2);

struct VSIn  { float3 pos : POSITION; float2 uv : TEXCOORD0; };
struct VSOut { float4 pos : SV_POSITION; float3 wpos : TEXCOORD0; float3 nor : TEXCOORD1; float3 jac : TEXCOORD2; };

static const float PI = 3.14159265f;

// 32-wave procedural spectrum. Parameters are computed inline from the
// wave index `k` so we can loop the count up to 32 without a giant
// static table. Distribution mimics a Phillips ocean spectrum:
//   - direction: cos²-lobe spread around wind axis with light chaos
//   - wavelength: log-decreasing from base*2 down to base*0.04
//   - amplitude:  ∝ wavelength (longer waves are taller)
//   - speed:      dispersion ∝ sqrt(g/wavelength)
//
// 32 waves ≈ "FFT-lite" — much richer than the previous 8 layers.

void GerstnerWave(float2 dir, float wavelength, float amp, float speed, float Q,
                  float2 worldXZ, float t,
                  inout float3 disp, inout float3 normalAccum,
                  inout float3 jacAccum)   // .x=∂dx/∂x, .y=∂dz/∂z, .z=∂dx/∂z (=∂dz/∂x)
{
    float w     = 2.0f * PI / max(wavelength, 1e-3f);
    float phi   = speed * w;
    float theta = w * dot(dir, worldXZ) - phi * t;
    float c     = cos(theta);
    float s     = sin(theta);
    float qa    = Q * amp;

    disp.x += qa * dir.x * c;
    disp.z += qa * dir.y * c;
    disp.y += amp * s;

    // Normal accumulators
    float wac = w * amp * c;
    float was = w * amp * s;
    normalAccum.x += -dir.x * wac;
    normalAccum.z += -dir.y * wac;
    normalAccum.y += Q * was;

    // Jacobian of horizontal displacement. ∂(qa*dir.x*cos(theta))/∂x
    //   = -qa*dir.x * sin(theta) * w*dir.x = -Q*A*w*sin*dir.x²
    // Same for Z and the cross term.
    jacAccum.x += -Q * dir.x * dir.x * was;
    jacAccum.y += -Q * dir.y * dir.y * was;
    jacAccum.z += -Q * dir.x * dir.y * was;
}

VSOut WaterVS(VSIn i)
{
    VSOut o;

    float3 wp = mul(float4(i.pos, 1.0f), World).xyz;
    float2 windAxis = float2(WindParams.x, WindParams.y);
    float  baseLen   = WaveParams.y;
    float  baseAmp   = WaveParams.x;
    float  baseSpeed = WaveParams.z;
    float  Q         = WaveParams.w;
    int    n         = (int)clamp(WindParams.z, 1, 32);

    float3 disp = float3(0,0,0);
    float3 nAcc = float3(0,0,0);
    float3 jAcc = float3(0,0,0);
    float qShare  = Q / max((float)n, 1.0f);

    [loop]
    for (int k = 0; k < 32; ++k)
    {
        if (k >= n) break;
        float t = (float)k * (1.0f / 32.0f);

        float ang = (t - 0.5f) * 1.5f + sin(t * 12.73f) * 0.35f + sin(t * 27.13f) * 0.15f;
        float ca  = cos(ang), sa = sin(ang);
        float2 d  = float2(ca * windAxis.x - sa * windAxis.y,
                           sa * windAxis.x + ca * windAxis.y);

        float lenT    = pow(t, 1.4f);
        float wavelen = baseLen * lerp(2.0f, 0.04f, lenT);

        float chaos = 0.7f + 0.3f * sin(t * 41.7f);
        float amp   = baseAmp * (wavelen / baseLen) * 0.55f * chaos;
        float spd   = baseSpeed * sqrt(baseLen / max(wavelen, 0.01f));

        GerstnerWave(d, wavelen, amp, spd, qShare, wp.xz, Time, disp, nAcc, jAcc);
    }

    wp += disp;
    float3 N = normalize(float3(-nAcc.x, 1.0f - nAcc.y, -nAcc.z));

    o.pos  = mul(float4(wp, 1.0f), ViewProj);
    o.wpos = wp;
    o.nor  = N;
    o.jac  = jAcc;     // PS computes Jacobian determinant for foam mask

    return o;
}

// Reconstruct world-space position of whatever was rendered at `uv`
// using the depth value sceneZ from SceneDepth.
float3 ReconstructWorldPos(float2 uv, float sceneZ)
{
    float2 ndc = uv * 2.0f - 1.0f;
    ndc.y = -ndc.y;
    float4 farClip  = float4(ndc, sceneZ, 1.0f);
    float4 farWorld = mul(farClip, InvViewProj);
    return farWorld.xyz / farWorld.w;
}

// Cheap analytical caustic. Two crossed sin lattices summed in quadrature
// look like the light grid you get under wavy water in shallow pools.
float Caustic(float2 worldXZ, float scale, float t)
{
    float2 p = worldXZ * scale;
    float c1 = sin(p.x * 1.5f + t * 0.6f) * sin(p.y * 1.7f - t * 0.5f);
    float c2 = sin(p.x * 0.8f - t * 0.3f) * sin(p.y * 0.9f + t * 0.4f);
    float v  = saturate(c1 * c1 + c2 * c2 * 0.5f);
    return pow(v, 4.0f);
}

// Screen-space reflection. Marches a reflected world-space ray, projecting
// each step to screen and comparing against SceneDepth. Returns the SceneColor
// hit if found; falls back to the procedural sky otherwise.
float3 SSR(float3 wpos, float3 dirWS, float3 fallbackSky)
{
    if (SsrParams.x < 0.5f) return fallbackSky;

    int   steps     = (int)SsrParams.y;
    float stepLen   = SsrParams.z;
    float thickness = SsrParams.w;
    float3 hitCol   = fallbackSky;

    [loop]
    for (int s = 1; s <= 64; ++s)
    {
        if (s > steps) break;
        float3 p = wpos + dirWS * (stepLen * (float)s);
        float4 sp = mul(float4(p, 1.0f), ViewProj);
        if (sp.w <= 0.0f) break;
        sp.xyz /= sp.w;
        float2 uv = sp.xy * 0.5f + 0.5f;
        uv.y = 1.0f - uv.y;
        if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f) break;

        float sceneZ = SceneDepth.Sample(SamPoint, uv).x;
        if (sp.z >= sceneZ && sp.z < sceneZ + thickness * 0.01f)
        {
            // Hit. Edge fade so reflections don't pop at screen borders.
            float edge = saturate(1.0f - max(abs(uv.x * 2.0f - 1.0f), abs(uv.y * 2.0f - 1.0f)) * 1.4f);
            float3 col = SceneColor.Sample(SamLinear, uv).rgb;
            hitCol = lerp(fallbackSky, col, edge);
            break;
        }
    }
    return hitCol;
}

// Sky reflection now comes from a baked cubemap (Skybox::BakeCubemap).
// Sample SkyCube with the reflected view direction.

// Sample three scrolling normal-map layers, sum tangent-space dx/dy, then
// perturb the geometric normal. Layers tile at different world scales so
// no single grid pattern dominates.
float3 SampleDetailNormal(float2 worldXZ, float t)
{
    float2 d1 = DetailNormal.Sample(SamLinearWrap, worldXZ * DetailScales.x + t * float2( 0.04f,  0.02f)).xy * 2.0f - 1.0f;
    float2 d2 = DetailNormal.Sample(SamLinearWrap, worldXZ * DetailScales.y + t * float2(-0.05f,  0.03f)).xy * 2.0f - 1.0f;
    float2 d3 = DetailNormal.Sample(SamLinearWrap, worldXZ * DetailScales.z + t * float2( 0.02f, -0.04f)).xy * 2.0f - 1.0f;
    // Sum tangent-space dx/dy contributions; reconstruct full normal.
    float2 sumXY = d1 + d2 + d3;
    return normalize(float3(sumXY.x, 1.0f, sumXY.y));   // tangent space = (X right, Z up surface, Y world-up)
}

float4 WaterPS(VSOut i) : SV_Target
{
    float3 Ngeo = normalize(i.nor);

    // Pre-compute a quick foam estimate so we can boost the detail
    // normal strength on foam pixels — gives the foam its 3D rough look
    // (vs. flat painted white) and naturally scatters specular/reflection.
    float Jq = (1.0f + i.jac.x) * (1.0f + i.jac.y) - i.jac.z * i.jac.z;
    float quickJacFoam = saturate((1.0f - Jq) * 1.4f);
    float persistentFoamPre = 0.0f;
    {
        float2 fuv = (i.wpos.xz - FoamPlane.xy) * FoamPlane.w;
        if (FoamPlane.z > 0.0f &&
            fuv.x >= 0.0f && fuv.x <= 1.0f && fuv.y >= 0.0f && fuv.y <= 1.0f)
            persistentFoamPre = FoamMapTex.SampleLevel(SamLinear, fuv, 0).r;
    }
    float quickFoam = saturate(max(quickJacFoam, persistentFoamPre));

    float3 N = Ngeo;
    if (DetailParams.x > 0.5f)
    {
        float3 dTS = SampleDetailNormal(i.wpos.xz, Time);
        // Boost detail strength wherever foam is — foam = full-rough surface,
        // so its mesoscale normal should be much choppier than smooth water.
        float  s   = DetailParams.y * (1.0f + quickFoam * 3.0f);
        float3 perturbed = float3(Ngeo.x + dTS.x * s,
                                  Ngeo.y,
                                  Ngeo.z + dTS.z * s);
        N = normalize(perturbed);
    }
    float3 V = normalize(EyePos - i.wpos);
    float3 L = normalize(-LightDir);
    float3 H = normalize(L + V);
    float  NdotV = saturate(dot(N, V));
    float  NdotL = saturate(dot(N, L));

    float2 screenUV = i.pos.xy * float2(ScreenParams.x, ScreenParams.y);

    // ---- Refraction sample ----
    // Distort the scene-color sample by the surface normal projected to
    // screen XY. Strength scales with grazing angle.
    float  refrAmt   = ExtraParams.y * (0.5f + 0.5f * (1.0f - NdotV));
    float2 refractUV = saturate(screenUV + N.xz * refrAmt);
    float3 refrColor = (ExtraParams.w > 0.5f)
        ? SceneColor.Sample(SamLinear, refractUV).rgb
        : Shallow.rgb;
    float sceneZ = (ExtraParams.w > 0.5f)
        ? SceneDepth.Sample(SamPoint, refractUV).x
        : 1.0f;

    // ---- Beer-Lambert absorption ------------------------------------
    // Reconstruct the world-space position of whatever was rendered at
    // refractUV. Distance from water surface to that point along the
    // view ray is dWater; doubling approximates the round trip
    // (sun → floor → eye) so the floor color absorbs at the same rate
    // as the inscatter builds up.
    float3 floorWorld = ReconstructWorldPos(refractUV, sceneZ);
    float  dWater     = length(i.wpos - floorWorld);
    float3 sigma      = Extinction.xyz;
    float3 T          = exp(-sigma * dWater * 2.0f);   // RGB transmittance
    float  absorbed   = saturate(1.0f - max(T.r, max(T.g, T.b)));   // 0=clear, 1=fully absorbed

    // ---- Caustics on the seafloor (modulate refrColor before absorption) ----
    if (ExtraParams.w > 0.5f && ExtraParams2.x > 0.0f)
    {
        float caust = Caustic(floorWorld.xz, ExtraParams2.y, Time);
        // Caustics get the most light when transmittance is high.
        float caustMask = max(T.r, max(T.g, T.b)) * ExtraParams2.x;
        refrColor *= 1.0f + caust * caustMask;
    }

    // Beer-Lambert composite: refraction color filtered by transmittance,
    // plus scattered "water color" inscatter that grows with absorption.
    float3 scatterCol  = Deep.rgb * Extinction.w;
    float3 belowSurface = refrColor * T + scatterCol * (1.0f - T);

    // ---- Subsurface scatter (back-light through thin water) ----
    {
        float backLight = pow(saturate(dot(V, -L) + 0.4f), 3.0f);
        float thinness  = max(T.r, max(T.g, T.b));    // bright where water is thin
        float3 sss      = backLight * thinness * Shallow.rgb * LightColor * ExtraParams2.z;
        belowSurface += sss;
    }

    // ---- Foam (Jacobian crest + shore + persistent history map) ----
    float J            = (1.0f + i.jac.x) * (1.0f + i.jac.y) - i.jac.z * i.jac.z;
    float jacobianFoam = saturate((1.0f - J) * 1.4f);

    float shoreFoam = saturate(1.0f - absorbed * 6.0f);
    shoreFoam = pow(shoreFoam, 4.0f) * 0.7f;

    // Persistent foam from FoamMap (decayed history of past Jacobian
    // events at this world location). Mapped from worldXZ → UV using
    // the same plane rect FoamMap was updated against.
    float2 foamUV  = (i.wpos.xz - FoamPlane.xy) * FoamPlane.w;
    float persistentFoam = (FoamPlane.z > 0.0f && foamUV.x >= 0.0f && foamUV.x <= 1.0f
                            && foamUV.y >= 0.0f && foamUV.y <= 1.0f)
        ? FoamMapTex.SampleLevel(SamLinear, foamUV, 0).r
        : 0.0f;

    float foamMask = saturate(max(jacobianFoam, persistentFoam) + shoreFoam);

    // Organic noise pattern from the detail-normal map's alpha channel
    // (filled with fBm height). Two scrolling layers blended.
    float n1 = DetailNormal.Sample(SamLinearWrap, i.wpos.xz * 0.30f + Time * float2( 0.04f,  0.02f)).a;
    float n2 = DetailNormal.Sample(SamLinearWrap, i.wpos.xz * 1.10f + Time * float2(-0.06f,  0.03f)).a;
    float foamNoise = saturate(n1 * 0.6f + n2 * 0.4f);

    // Threshold the noise against the mask: low mask hides everything,
    // high mask reveals patches whose density grows with mask.
    float threshold = 1.0f - foamMask;
    float foam = smoothstep(threshold - 0.10f, threshold + 0.05f, foamNoise) * saturate(foamMask * 1.4f);

    // Center vs edge color — center = pure white, edge = light cyan
    // (subsurface tint of thin foam). Lerp by foam thickness proxy.
    float3 foamCenter = float3(1.00f, 1.00f, 1.00f);
    float3 foamEdge   = float3(0.82f, 0.92f, 1.00f);
    float3 foamColor  = lerp(foamEdge, foamCenter, smoothstep(0.3f, 0.85f, foam));

    belowSurface = lerp(belowSurface, foamColor, foam);

    // ---- Reflection: SSR with mipped cubemap sky as fallback ----
    float3 R       = reflect(-V, N);
    float  reflLod = saturate(DetailParams.z) * DetailParams.w;
    float3 skyCol  = SkyCube.SampleLevel(SamLinear, R, reflLod).rgb * SkyTint;
    float3 reflCol = (ExtraParams.w > 0.5f) ? SSR(i.wpos, R, skyCol) : skyCol;

    // ---- Fresnel + composite ----
    float F0      = 0.02f;
    float fresnel = F0 + (1.0f - F0) * pow(1.0f - NdotV, WindParams.w);
    // Foam = full-rough; clamp its mirror reflection contribution so
    // foam patches look matte instead of like polished plastic.
    fresnel *= (1.0f - foam * 0.6f);

    // Tight specular sun glint — suppressed on foam (foam = full-rough).
    float spec = pow(saturate(dot(N, H)), max(ExtraParams.x, 4.0f));
    spec *= (1.0f - foam * 0.85f);

    float3 col = lerp(belowSurface, reflCol, fresnel);
    col += spec * LightColor * (1.0f - fresnel * 0.6f);

    return float4(col, 1.0f);
}
