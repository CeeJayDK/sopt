// Plain HLSL (SM5 pixel shader) for test_fx: cbuffer, typed textures, sampler states,
// texture methods.
cbuffer Params : register(b0)
{
    float gExposure : packoffset(c0.x);
    float gContrast : packoffset(c0.y);
};
Texture2D<float4> gColor : register(t0);
Texture2D<float>  gDepth : register(t1);
SamplerState gPoint : register(s0);
SamplerState gLinear : register(s1);

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float3 c = gColor.Sample(gLinear, uv).rgb * 2.0 - 1.0;
    float d = gDepth.SampleLevel(gPoint, uv, 0) * 0.5 + 0.5;
    float3 n = gColor.Load(int3(pos.xy, 0)).rgb;
    float3 s = gColor.Sample(gLinear, uv).rgb;
    float3 w = pow(abs(s), 2.0) * gContrast;
    float3 r = n * 0.5 + n * 0.5;
    return float4(c * d * gExposure + w + r, 1.0);
}
