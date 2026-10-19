// Plain HLSL for test_fx, as OBS Studio writes it: vectors with brace initializers ({ 0, 0, 0, 1 }), and a macro
// argument that starts with '-' after a '-' in the macro (NEG(-1) was lexed as '--1'). Loads; line 14 is a region.
uniform float3 gWeights = { 0.25, 0.5, 0.25 };
static const float4 gBias = { 0, 0, 0, 1 };
Texture2D<float4> gColor : register(t0);
SamplerState gLinear : register(s0);

#define NEG(x) -x

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float4 c = gColor.Sample(gLinear, uv);
    float n = NEG(-1) * c.r;
    float w = c.a * 3.0 - c.a + c.g * gWeights.x;
    return float4(w, n, 0.0, 1.0) + gBias;
}
