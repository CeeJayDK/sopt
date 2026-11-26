void IntVS(in uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) { uv = float2(0.0, 0.0); pos = float4(uv, 0.0, 1.0); }
#define KSCALE 2
uniform int fc < source = "framecount"; >; uniform int rnd < source = "random"; min = 1; max = 8; >; uniform int rnd2 < source = "random"; min = -1; max = 8; >;
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
		int m = i % 3;
		sum += m / 2;
		int sh = i >> 1;
		sum += sh % 3;
		int d = (i << 1) + 1;
		sum += i / d;
		int n = (i - 4) % 3;
		sum += n / 2;
		sum += fc % 8;
		sum += 100 / rnd;
		sum += rnd2 / 2;
	}
	return float(sum);
}

technique IntDiv { pass { VertexShader = IntVS; PixelShader = IntDivPS; } }
