#pragma pack_matrix(row_major)

// Reflection-pass terrain shader (Phase 5). Forward-lit single RT
// output sampled by water.hlsl. PS discards geometry below the water
// plane so submerged terrain never pollutes the reflection.

cbuffer TerrainCB : register(b0)
{
    float4x4 matWVP;
    float4x4 matWorld;
    float4 eyePos;
    float4 fogColor;
    float4 fogParams;        // start, end, amount, unused
    float4 fowParams;        // x=gridOriginX, y=gridOriginZ, z=mapWidth, w=enable
    float4 reflectionParams; // x = waterY (PS discards if worldY < x)
};

Texture2D SourceTex : register(t0);
Texture2D FoWMap    : register(t1);
SamplerState Samp : register(s0);

struct VS_IN
{
    float3 Pos    : POSITION;
    float3 Normal : NORMAL;
    float2 UV     : TEXCOORD0;
};

struct VS_OUT
{
    float4 Pos      : SV_POSITION;
    float3 WorldPos : TEXCOORD0;
    float3 Normal   : TEXCOORD1;
    float2 UV       : TEXCOORD2;
};

VS_OUT VS_TerrainRefl(VS_IN input)
{
    VS_OUT output;
    output.Pos      = mul(float4(input.Pos, 1.0), matWVP);
    output.WorldPos = mul(float4(input.Pos, 1.0), matWorld).xyz;
    output.Normal   = normalize(mul(float4(input.Normal, 0.0), matWorld).xyz);
    output.UV       = input.UV;
    return output;
}

float4 PS_TerrainRefl(VS_OUT input) : SV_TARGET
{
    if (input.WorldPos.y < reflectionParams.x)
        discard;

    float3 base = SourceTex.Sample(Samp, input.UV).rgb;
    float3 n    = normalize(input.Normal);

    // Match Renderer::LightingCB so reflected terrain matches the
    // primary lit pass tonally.
    const float3 sunDir   = normalize(float3(-0.32, 1.0, -0.20));
    const float3 sunColor = float3(0.90, 0.90, 0.90);
    const float  ambient  = 0.36;
    float NdotL = saturate(dot(n, sunDir));
    float3 lit  = base * (ambient + NdotL) * sunColor;

    return float4(lit, 1.0);
}
