// sopt_x2.fx: what ReShade hands the graphics driver for a multiply by 2 (owner, 2026-10-17).
// fxc (Direct3D 9 to 12) writes x * 2.0 as x + x, which costs NVIDIA its free x2 (the multiply's post-scale);
// ReShade's own SPIR-V (Vulkan) keeps the multiply. Run it with the Test Host:
//     bin\run-test.ps1 -Apis vulkan,dx11 -Effect sopt_x2.fx -Frames 10
// then read ReShade's cache (%TEMP%\ReShade): reshade-sopt_x2-<renderer>-...-SoptX2PS.cso is the code the driver
// gets (Vulkan: SPIR-V, look for OpFMul by 2.0; Direct3D: DXBC, its .asm next to it shows add r, r, r for the x2).
// The three outputs: x2, x4 (free on NVIDIA where it is a multiply) and x3 (never free, the control).

uniform float4 SoptX2U = float4(1.0, 2.0, 3.0, 4.0);

void SoptX2VS(in uint id : SV_VertexID, out float4 pos : SV_Position)
{
	const float2 t = float2((id << 1) & 2, id & 2);
	pos = float4(t * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 SoptX2PS(float4 pos : SV_Position) : SV_Target
{
	const float a = pos.x * 0.001 + SoptX2U.x, b = SoptX2U.y;
	return float4(max(a * b * 2.0, a), max(a * b * 4.0, a), max(a * b * 3.0, a), 1.0);
}

technique sopt_x2
{
	pass
	{
		VertexShader = SoptX2VS;
		PixelShader = SoptX2PS;
	}
}
