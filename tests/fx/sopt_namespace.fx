// Test effect for fx_namespace: uniforms of the function's own namespace are written by their plain
// name (a header included inside different namespaces must not get one effect's namespace), uniforms
// of another namespace keep it.
namespace Other
{
	uniform float Far < ui_min = 1.0; ui_max = 2.0; > = 1.5;
}

namespace Ns
{
	uniform float Gain < ui_min = 0.0; ui_max = 2.0; > = 1.0;

	void VS(in uint id : SV_VertexID, out float4 position : SV_Position, out float2 texcoord : TEXCOORD)
	{
		texcoord.x = (id == 2) ? 2.0 : 0.0;
		texcoord.y = (id == 1) ? 2.0 : 0.0;
		position = float4(texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
	}

	float4 PS(float4 vpos : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
	{
		float v = texcoord.x * Gain + texcoord.y * Other::Far;
		return v;
	}

	technique NsTest { pass { VertexShader = VS; PixelShader = PS; } }
}
