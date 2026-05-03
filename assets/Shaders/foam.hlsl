// Foam map update pass. Runs once per frame, world-XZ space.
//   Read: previous frame's foam at this UV.
//   Compute: current Jacobian-based foam by recomputing the same
//            Gerstner wave loop water.hlsl uses, sampling at worldXZ.
//   Write: max(current, previous * decay).
//
// Result is sampled by water.hlsl as a persistent foam mask, giving
// trails behind breaking waves and shoreline crests.

cbuffer CB : register(b0)
{
    float4 PlaneRect;     // x=originX, y=originZ, z=size, w=invSize
    float4 WaveParams;    // x=amp, y=len, z=speed, w=Q
    float4 WindParams;    // x=cos, y=sin, z=numWaves, w=time
    float4 UpdateParams;  // x=decay, y=firstFrame
};

Texture2D<float> FoamHistory : register(t0);
SamplerState     SamLinear   : register(s0);

static const float PI = 3.14159265f;

void GerstnerJac(float2 dir, float wavelength, float amp, float speed, float Q,
                 float2 worldXZ, float t,
                 inout float3 jac)
{
    float w     = 2.0f * PI / max(wavelength, 1e-3f);
    float phi   = speed * w;
    float theta = w * dot(dir, worldXZ) - phi * t;
    float was   = w * amp * sin(theta);
    jac.x += -Q * dir.x * dir.x * was;
    jac.y += -Q * dir.y * dir.y * was;
    jac.z += -Q * dir.x * dir.y * was;
}

float ComputeJacobianFoam(float2 worldXZ, float t)
{
    float2 windAxis = float2(WindParams.x, WindParams.y);
    float  baseLen  = WaveParams.y;
    float  baseAmp  = WaveParams.x;
    float  baseSpd  = WaveParams.z;
    float  Q        = WaveParams.w;
    int    n        = (int)clamp(WindParams.z, 1, 32);
    float  qShare   = Q / max((float)n, 1.0f);

    float3 jac = float3(0,0,0);

    [loop]
    for (int k = 0; k < 32; ++k)
    {
        if (k >= n) break;
        float u = (float)k * (1.0f / 32.0f);
        float ang = (u - 0.5f) * 1.5f + sin(u * 12.73f) * 0.35f + sin(u * 27.13f) * 0.15f;
        float ca = cos(ang), sa = sin(ang);
        float2 d = float2(ca * windAxis.x - sa * windAxis.y,
                          sa * windAxis.x + ca * windAxis.y);
        float lenT    = pow(u, 1.4f);
        float wavelen = baseLen * lerp(2.0f, 0.04f, lenT);
        float chaos   = 0.7f + 0.3f * sin(u * 41.7f);
        float amp     = baseAmp * (wavelen / baseLen) * 0.55f * chaos;
        float spd     = baseSpd * sqrt(baseLen / max(wavelen, 0.01f));
        GerstnerJac(d, wavelen, amp, spd, qShare, worldXZ, t, jac);
    }

    float J = (1.0f + jac.x) * (1.0f + jac.y) - jac.z * jac.z;
    return saturate((1.0f - J) * 1.4f);
}

struct VOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

VOut FoamVS(uint id : SV_VertexID)
{
    VOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv  = uv;
    o.pos = float4(uv * 2.0f - 1.0f, 0.0f, 1.0f);
    return o;
}

float4 FoamPS(VOut i) : SV_Target
{
    float2 worldXZ = PlaneRect.xy + i.uv * PlaneRect.z;
    float  cur     = ComputeJacobianFoam(worldXZ, WindParams.w);

    float prev = (UpdateParams.y > 0.5f)
        ? 0.0f
        : FoamHistory.Sample(SamLinear, i.uv).r;
    float decayed = prev * UpdateParams.x;

    float foam = max(cur, decayed);
    return float4(foam, 0, 0, 1);
}
