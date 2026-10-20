// Test effect for test_fx: a window must not move a declaration into an inner block (dh_uber_rt.fx).
// Line numbers are checked in test_fx.
float4 BlockVS(uint id : SV_VertexID) : SV_Position { return 0; }
float4 BlockPS(float4 p : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	float2 w = uv * 0.5;
	{
		float d = abs(uv.x - uv.y);
		w *= max(0.001, 1.0 - 100.0 * d * saturate(1.0 - uv.y * 2.0));
	}
	return float4(w, 0.0, 1.0);
}
technique TestBlock { pass { VertexShader = BlockVS; PixelShader = BlockPS; } }
