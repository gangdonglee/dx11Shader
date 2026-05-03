// Temporal antialiasing.
//   - Reprojects history into the current frame using SceneDepth +
//     PrevViewProj (camera-only motion; static scene assumption).
//   - Variance-clamps the reprojected sample to the current 3x3
//     neighborhood min/max to prevent ghosting on subpixel-shimmer.
//   - Blends current ↔ clamped history with TaaParams.y weight on
//     history.

cbuffer CB : register(b0)
{
    float4x4 PrevViewProj;
    float4x4 _Padding;
    float4   ScreenParams;  // x=invW, y=invH
    float4   TaaParams;     // x=enabled, y=historyBlend, z=clamp, w=firstFrame
    float4   EyePos;
};

Texture2D<float4> SceneColor : register(t0);
Texture2D<float4> HistoryTex : register(t1);
Texture2D<float>  SceneDepth : register(t2);
SamplerState      SamLinear  : register(s0);
SamplerState      SamPoint   : register(s1);

struct VOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

VOut TaaVS(uint id : SV_VertexID)
{
    VOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv  = uv;
    o.pos = float4(uv * 2.0f - 1.0f, 1.0f, 1.0f);
    o.pos.y = -o.pos.y;
    return o;
}

// Reconstruct world position from depth at given UV (must match the
// projection used for the SceneColor frame).  We don't have direct
// access to InvViewProj here — instead, we use PrevViewProj backwards
// is hard, so we take a different approach: reproject by treating the
// world position as known via depth + we need the *current* InvViewProj.
//
// Simpler trick: reproject in NDC space using PrevViewProj * inverse of
// current ViewProj — but we don't have that either. Easiest is to feed
// PrevClipFromCurrentClip, which is PrevViewProj * InvCurrentViewProj,
// from C++. To avoid one more cbuffer field, App composes it and writes
// directly into PrevViewProj (so the matrix here is really
// "prevClipFromCurrentClip"). The HLSL just multiplies clip-space
// vectors by it.
float2 ReprojectUV(float2 uv, float depth)
{
    float2 ndc = uv * 2.0f - 1.0f;
    ndc.y = -ndc.y;
    float4 curClip = float4(ndc, depth, 1.0f);
    float4 prev = mul(curClip, PrevViewProj);     // here = prevClipFromCurClip
    prev.xyz /= prev.w;
    float2 prevUV = prev.xy * 0.5f + 0.5f;
    prevUV.y = 1.0f - prevUV.y;
    return prevUV;
}

float4 TaaPS(VOut i) : SV_Target
{
    float3 cur = SceneColor.Sample(SamLinear, i.uv).rgb;

    // First frame: just emit current; history isn't valid yet.
    if (TaaParams.w > 0.5f)
        return float4(cur, 1.0f);

    float depth = SceneDepth.Sample(SamPoint, i.uv).x;

    // Sky pixels (depth ≈ 1) reproject very poorly; treat as no motion.
    float2 prevUV = (depth >= 0.9999f) ? i.uv : ReprojectUV(i.uv, depth);

    // If prev UV escapes the screen, no valid history.
    bool inside = (prevUV.x >= 0.0f && prevUV.x <= 1.0f &&
                   prevUV.y >= 0.0f && prevUV.y <= 1.0f);

    float3 hist = HistoryTex.SampleLevel(SamLinear, prevUV, 0).rgb;

    // Variance clamp: bound history to the current 3x3 neighborhood.
    if (TaaParams.z > 0.5f)
    {
        float3 nMin = cur, nMax = cur;
        float2 tx = float2(ScreenParams.x, ScreenParams.y);
        [unroll] for (int yy = -1; yy <= 1; ++yy)
        [unroll] for (int xx = -1; xx <= 1; ++xx)
        {
            float2 o = float2(xx, yy) * tx;
            float3 c = SceneColor.SampleLevel(SamLinear, i.uv + o, 0).rgb;
            nMin = min(nMin, c);
            nMax = max(nMax, c);
        }
        // Slight expansion so static high-frequency detail isn't over-smashed.
        float3 ext = (nMax - nMin) * 0.125f;
        nMin -= ext; nMax += ext;
        hist = clamp(hist, nMin, nMax);
    }

    float blend = inside ? TaaParams.y : 0.0f;
    float3 outc = lerp(cur, hist, blend);
    return float4(outc, 1.0f);
}
