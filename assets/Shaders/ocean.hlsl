#pragma pack_matrix(row_major)

// Ocean shader — separate track from water.hlsl. Targets RTS-distance
// open-water look:
//   - Five-wave Gerstner analytical normal (large swell + chop)
//   - Whitecap mask from normal slope (no vertex displacement needed)
//   - Sea foam streaks (anisotropic noise scroll)
//   - Sky gradient blended into reflection (zenith ↔ horizon)
// Plane mesh stays flat (no polygon edges). Reuses the global noise
// texture and reflection RT bound by Renderer.

cbuffer OceanCB : register(b0)
{
    float4x4 matVP;
    float4x4 matWorld;
    float4   eyePos;        // xyz
    float4   timeAndPad;    // x = time
    float4   waveParams;    // x = swell amp scale, y = scroll speed
    float4   shallowColor;  // rgb
    float4   deepColor;     // rgb
    float4   skyHorizon;    // rgb
    float4   skyZenith;     // rgb
    float4   whitecapColor; // rgb, a = strength
    float4   streakParams;  // x = strength, y = stretch, z = scroll, w = enabled(0/1)
    float4   reflParams;    // x = strength, y = fresnelPower, z = distortion, w = enabled
};

Texture2D    ReflectionTex : register(t1);
Texture2D    NoiseTex      : register(t2);
SamplerState DepthSamp     : register(s0);
SamplerState NoiseSamp     : register(s1);

struct VS_IN
{
    float3 Pos : POSITION;
    float2 UV  : TEXCOORD0;
};

struct VS_OUT
{
    float4 Pos      : SV_POSITION;
    float3 WorldPos : TEXCOORD0;
    float2 UV       : TEXCOORD1;
};

VS_OUT VS_Ocean(VS_IN input)
{
    VS_OUT output;
    float4 wp = mul(float4(input.Pos, 1.0), matWorld);
    output.WorldPos = wp.xyz;
    output.Pos      = mul(wp, matVP);
    output.UV       = input.UV;
    return output;
}

void AccumulateGerstner(float2 wxz, float t, float amp,
                        float2 dir, float wavelength, float amplitude,
                        float steepness, float speed,
                        inout float3 dpdx, inout float3 dpdz)
{
    float k   = 6.28318530 / wavelength;
    float phi = dot(dir, wxz) * k - speed * t;
    float a   = amplitude * amp;
    float ka  = a * k;
    float ca  = cos(phi) * ka;
    float sa  = sin(phi) * ka;
    float dx  = dir.x;
    float dz  = dir.y;

    dpdx.x += -steepness * sa * dx * dx;
    dpdx.y +=  ca * dx;
    dpdx.z += -steepness * sa * dx * dz;

    dpdz.x += -steepness * sa * dx * dz;
    dpdz.y +=  ca * dz;
    dpdz.z += -steepness * sa * dz * dz;
}

// Five large-scale swell packets — wavelengths much longer than the
// lake/water shader to read as open ocean at RTS distance.
float3 SampleOceanNormal(float2 wxz, float t, float amp)
{
    float3 dpdx = float3(1.0, 0.0, 0.0);
    float3 dpdz = float3(0.0, 0.0, 1.0);

    AccumulateGerstner(wxz, t, amp, float2( 1.00,  0.05), 80.0, 0.55, 0.55, 0.95, dpdx, dpdz);
    AccumulateGerstner(wxz, t, amp, float2( 0.71, -0.71), 45.0, 0.32, 0.55, 1.20, dpdx, dpdz);
    AccumulateGerstner(wxz, t, amp, float2(-0.40,  0.92), 25.0, 0.20, 0.55, 1.55, dpdx, dpdz);
    AccumulateGerstner(wxz, t, amp, float2( 0.30,  0.95), 12.0, 0.10, 0.50, 2.20, dpdx, dpdz);
    AccumulateGerstner(wxz, t, amp, float2(-0.85, -0.50),  6.0, 0.05, 0.45, 2.80, dpdx, dpdz);

    return normalize(cross(dpdz, dpdx));
}

float4 PS_Ocean(VS_OUT input) : SV_TARGET
{
    const float3 sunDir   = normalize(float3(-0.32, 1.0, -0.20));
    const float3 sunColor = float3(0.92, 0.92, 0.92);
    const float  ambient  = 0.30;
    const float  shine    = 110.0;
    const float  specGain = 1.10;

    float t = timeAndPad.x * waveParams.y;

    // Coarse swell normal.
    float3 n = SampleOceanNormal(input.WorldPos.xz, t, waveParams.x);

    // Fine bump from a high-frequency noise sample so close-up pixels
    // still get crisp ripples.
    float2 dxz   = input.WorldPos.xz;
    float2 nuv1  = dxz * 0.18 + t * float2( 0.05, -0.04);
    float2 nuv2  = dxz * 0.31 + t * float2(-0.04,  0.06);
    float h00    = NoiseTex.Sample(NoiseSamp, nuv1).r + NoiseTex.Sample(NoiseSamp, nuv2).r;
    float hX     = NoiseTex.Sample(NoiseSamp, nuv1 + float2(0.012, 0.0)).r + NoiseTex.Sample(NoiseSamp, nuv2 + float2(0.012, 0.0)).r;
    float hZ     = NoiseTex.Sample(NoiseSamp, nuv1 + float2(0.0, 0.012)).r + NoiseTex.Sample(NoiseSamp, nuv2 + float2(0.0, 0.012)).r;
    float bumpStrength = 0.5 * waveParams.x;
    float3 bumpN = normalize(float3((h00 - hX) * bumpStrength, 1.0, (h00 - hZ) * bumpStrength));
    n = normalize(n + bumpN * 0.30);

    // Slope-based base color: shallow tone where the surface is more
    // tilted (closer to crest), deep tone where it's flat (trough).
    float slope  = saturate(length(n.xz) * 1.6);
    float3 base  = lerp(deepColor.rgb, shallowColor.rgb, slope * 0.6);

    // Lighting.
    float3 viewDir = normalize(eyePos.xyz - input.WorldPos);
    float3 H       = normalize(sunDir + viewDir);
    float  NdotL   = saturate(dot(n, sunDir));
    float  NdotH   = saturate(dot(n, H));
    float  spec    = pow(NdotH, shine) * specGain;

    float3 lit      = base * (ambient + NdotL) * sunColor;
    float3 finalRgb = lit + sunColor * spec;

    // Sky gradient — sampled in the reflected view direction so the
    // surface picks up zenith blue for steep angles and horizon haze
    // when looking flat across the water.
    float3 reflDir = reflect(-viewDir, n);
    float  skyT    = saturate(reflDir.y);
    float3 sky     = lerp(skyHorizon.rgb, skyZenith.rgb, skyT);

    // Reflection RT (terrain) blended with the sky gradient using
    // Schlick Fresnel against the surface normal.
    float NdotV   = saturate(dot(n, viewDir));
    float fresnel = pow(1.0 - NdotV, max(reflParams.y, 0.5));
    if (reflParams.w > 0.5)
    {
        // Project worldPos to NDC for the reflection screen UV.
        float4 clip   = mul(float4(input.WorldPos, 1.0), matVP);
        float2 ndc    = clip.xy / clip.w;
        float2 reflUV = saturate(float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5)
                                + n.xz * reflParams.z);
        float3 reflTerrain = ReflectionTex.Sample(DepthSamp, reflUV).rgb;
        // If the reflection RT clear (sky-blue) bleeds in, we still get
        // sky color here; mix anyway for terrain edges to show up.
        sky = lerp(sky, reflTerrain, 0.7);
    }
    finalRgb = lerp(finalRgb, sky, saturate(fresnel * reflParams.x));

    // Whitecap — kicks in where the normal slope passes a threshold
    // (i.e. crests). Modulated by a slow noise so it's clustered.
    float2 wcUV   = dxz * 0.05 + t * float2(0.02, 0.015);
    float  wcMod  = NoiseTex.Sample(NoiseSamp, wcUV).r;
    float  wcMask = smoothstep(0.55, 0.80, slope) * wcMod;
    finalRgb = lerp(finalRgb, whitecapColor.rgb, wcMask * whitecapColor.a);

    // Sea foam streaks — anisotropic noise sample (stretched along x)
    // to read as long paint-like trails on the surface.
    if (streakParams.w > 0.5)
    {
        float2 stUV = float2(dxz.x * 0.04, dxz.y * 0.04 * max(streakParams.y, 1e-3))
                    + t * float2(streakParams.z, streakParams.z * 0.3);
        float st1 = NoiseTex.Sample(NoiseSamp, stUV).r;
        float st2 = NoiseTex.Sample(NoiseSamp, stUV * 1.7 + 0.5).r;
        float streak = saturate((st1 * st2 - 0.45) * 6.0);
        finalRgb = lerp(finalRgb, whitecapColor.rgb, streak * streakParams.x);
    }

    return float4(finalRgb, 1.0);
}
