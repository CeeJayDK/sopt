// sopt_Derivatives.fx: which pixels of a 2 x 2 quad ddx / ddy read, per graphics API (topt's question P2, owner
// 2026-10-18). Run it with the Test Host on every API:
//     bin\run-test.ps1 -Effect sopt_Derivatives.fx -Frames 10
//
// Six bands, left to right: ddx, ddy, ddx_coarse, ddy_coarse, ddx_fine, ddy_fine. Each pixel takes the derivative of
// f = x * y (exact in float at these sizes): along x the true difference on its own row is y, along y on its own
// column x. The pixel shows what it got against that:
//   green   its own row / column (fine)
//   red     the row above / the column to the left (coarse: the quad's top row or left column)
//   yellow  the row below / the column to the right (coarse with the other row or column)
//   blue    anything else
// So a solid green band is fine, green and red (or yellow) stripes one pixel wide are coarse. ReShade writes
// ddx_fine as ddx below shader model 5 (Direct3D 9 / 10) and as dFdxFine / OpDPdxFine on OpenGL / Vulkan.

void SoptDerivVS(in uint id : SV_VertexID, out float4 pos : SV_Position)
{
	const float2 t = float2((id << 1) & 2, id & 2);
	pos = float4(t * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float3 SoptDerivMark(float d)
{
	return abs(d) < 0.25 ? float3(0.0, 0.8, 0.0) : abs(d + 1.0) < 0.25 ? float3(0.9, 0.0, 0.0)
	     : abs(d - 1.0) < 0.25 ? float3(1.0, 0.85, 0.0) : float3(0.0, 0.3, 1.0);
}

float4 SoptDerivPS(float4 pos : SV_Position) : SV_Target
{
	// Every derivative before any branch (derivatives need the whole quad).
	const float f = pos.x * pos.y;
	const float dx = ddx(f) - pos.y, dy = ddy(f) - pos.x;
	const float dxc = ddx_coarse(f) - pos.y, dyc = ddy_coarse(f) - pos.x;
	const float dxf = ddx_fine(f) - pos.y, dyf = ddy_fine(f) - pos.x;
	const float u = pos.x * BUFFER_RCP_WIDTH * 6.0;
	if (frac(u) < 0.01) return float4(0.0, 0.0, 0.0, 1.0);  // band borders
	const int band = int(u);
	const float d = band == 0 ? dx : band == 1 ? dy : band == 2 ? dxc : band == 3 ? dyc : band == 4 ? dxf : dyf;
	return float4(SoptDerivMark(d), 1.0);
}

technique sopt_Derivatives
{
	pass
	{
		VertexShader = SoptDerivVS;
		PixelShader = SoptDerivPS;
	}
}
