// Test effect for test_fx: user-changeable preprocessor definitions as compile-time inputs.
#ifndef TEST_FAR
	#define TEST_FAR 1000.0
#endif
#ifndef TEST_MODE
	#define TEST_MODE 1
#endif
#define FIXED 3.0

texture DepthTex : DEPTH;
sampler Depth { Texture = DepthTex; };

void TestVS(in uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD)
{
	uv = float2(id == 2 ? 2.0 : 0.0, id == 1 ? 2.0 : 0.0);
	pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 TestPS(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	float d = tex2D(Depth, uv).x;
#if TEST_MODE
	d = d / (TEST_FAR - d * (TEST_FAR - 1.0));
#endif
	float e = d * FIXED + 1.0;
	return float4(d, e, 0.0, 1.0);
}

technique TestMacros { pass { VertexShader = TestVS; PixelShader = TestPS; } }
