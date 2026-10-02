// Test effect for test_fx: same-variable chains as windows, across #if lines.
#ifndef TEST_REV
	#define TEST_REV 1
#endif
#ifndef TEST_LOG
	#define TEST_LOG 0
#endif

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
#if TEST_LOG
	d = d * d;
#endif
#if TEST_REV
	d = 1.0 - d;
#endif
	d = d * 2.0 + 1.0;
	float y = d * 3.0;
	d = d * d;
	float e = uv.y;
	e = e * 0.5;
	e = e + 0.25;
	e = e * e;
	return float4(d, y, e, 1.0);
}

technique TestChain { pass { VertexShader = TestVS; PixelShader = TestPS; } }
