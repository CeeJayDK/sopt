// Repro for FX-COMPILER.md 1: a macro argument starting with '-' after a '-' in the macro: NEG(-1) is lexed as
// '--1'. Unpatched ReShade 6.8.0: error X3025 "l-value specifies const object". Patched: compiles, white screen.
#define NEG(x) -x

float4 PS(float4 p : SV_Position) : SV_Target { return NEG(-1) * 1.0; }

void VS(uint id : SV_VertexID, out float4 p : SV_Position)
{
	const float2 t = float2(id == 2 ? 2.0 : 0.0, id == 1 ? 2.0 : 0.0);
	p = float4(t * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

technique sopt_MacroMinus { pass { VertexShader = VS; PixelShader = PS; } }
