// --msaa: the test image drawn into the multisampled back buffer (a copy cannot change the
// sample count). Compiled to blit_dxbc.h with Microsoft's D3DCompile (vs_5_0 VS, ps_5_0 PS, O3).
Texture2D img : register(t0);

float4 VS(uint id : SV_VertexID) : SV_Position
{
	const float2 uv = float2(id == 1 ? 2.0 : 0.0, id == 2 ? 2.0 : 0.0);
	return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 PS(float4 pos : SV_Position) : SV_Target
{
	return img.Load(int3(pos.xy, 0));
}
