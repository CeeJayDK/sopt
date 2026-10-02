// Test effect for BUFFER_WIDTH / BUFFER_HEIGHT as compile-time inputs.
#define SCREEN_SIZE float2(BUFFER_WIDTH, BUFFER_HEIGHT)
#define PIXEL_SIZE float2(BUFFER_RCP_WIDTH, BUFFER_RCP_HEIGHT)

texture2D HalfTex { Width = BUFFER_WIDTH / 2; Height = BUFFER_HEIGHT / 2; Format = RGBA8; };
sampler2D HalfSamp { Texture = HalfTex; };
static const float2 kPixel = float2(BUFFER_RCP_WIDTH, BUFFER_RCP_HEIGHT); static const float kAspect = BUFFER_HEIGHT * BUFFER_RCP_WIDTH;

void VS(in uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD)
{
	uv = float2(id == 2 ? 2.0 : 0.0, id == 1 ? 2.0 : 0.0);
	pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 PS(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	float g = frac(dot(uv, SCREEN_SIZE * float2(0.0625, 0.2777778)) + 0.25);
	float2 o = uv * PIXEL_SIZE + uv * uv;
	float d = uv.x * (BUFFER_WIDTH / 3) + uv.y;
	float k = uv.x * kPixel.x + uv.y * kPixel.y;
	float a = uv.x * kAspect + uv.y * uv.y;
	return float4(g, o, d + k + a) + tex2D(HalfSamp, uv);
}

technique Buffer { pass { VertexShader = VS; PixelShader = PS; } }
