// Plain HLSL for test_fx: intrinsics ReShade FX lacks (fmod, isfinite) load and fmod is a region.
cbuffer Params : register(b0)
{
    float gScale;
    float gPeriod;
};
Texture2D<float4> gColor : register(t0);
SamplerState gLinear : register(s0);

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float4 c = gColor.Sample(gLinear, uv);
    float w = fmod(c.r * gScale + uv.x, gPeriod + 0.5);
    float k = isfinite(c.g) ? c.g : 0.0;
    return float4(w, k, c.b, 1.0);
}
