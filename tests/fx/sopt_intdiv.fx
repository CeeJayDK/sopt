void IntVS(in uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) { uv = float2(0.0, 0.0); pos = float4(uv, 0.0, 1.0); }
#define KSCALE 2
// int-to-uint rewrite: only divisions whose two sides are proven to qualify.
float4 IntDivPS(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	int sum = 0;
	for (int i = 0; i < 8; i++)
	{
		sum += i / 2;
		sum += i % 3;
		sum += (i + 1) / (i + 1);
		sum += (-i) / 2;
		sum += i * 3 / 2;
		sum += BUFFER_WIDTH / 2;
		sum += i / KSCALE;
		int2 a = int2(i, i);
		int s = i + 1;
		sum += (a / s).x;
	}
	return float(sum);
}

technique IntDiv { pass { VertexShader = IntVS; PixelShader = IntDivPS; } }
