// Repro for FX-COMPILER.md 2: two pragmas in a row. Unpatched ReShade 6.8.0: error X3000 "syntax error: unexpected
// '_Pragma'" (reported on a wrong line). Patched: compiles, white screen.
#pragma reshade showfps
#pragma reshade showclock

float4 PS(float4 p : SV_Position) : SV_Target { return 1.0; }

void VS(uint id : SV_VertexID, out float4 p : SV_Position)
{
	const float2 t = float2(id == 2 ? 2.0 : 0.0, id == 1 ? 2.0 : 0.0);
	p = float4(t * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

technique sopt_PragmaTwice { pass { VertexShader = VS; PixelShader = PS; } }
