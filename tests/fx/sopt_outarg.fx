uniform float Amount < ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; > = 0.5;
uniform bool Swap = false;
texture Back : COLOR;
sampler sBack { Texture = Back; };
void Fade(float d, out float2 lr, out float adj)
{
	lr = float2(d * 0.5, d * 0.25);
	adj = d;
}
float2 Div()
{
	float2 lr;
	float adj;
	Fade(Amount, lr, adj);
	return float2(-lr.x, lr.y);
}
float4 APS(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	float3 c = tex2D(sBack, uv).rgb;
	float d = Swap ? Div().y : Div().x;
	return float4(c * d * 2.0 + d, 1.0);
}
float4 BPS(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	float3 c = tex2D(sBack, uv).rgb;
	float2 lr;
	float adj;
	Fade(Amount, lr, adj);
	float k = lr.x * 2.0 + lr.y * adj;
	return float4(c * k, 1.0);
}
void PostProcessVS(in uint id : SV_VertexID, out float4 position : SV_Position, out float2 texcoord : TEXCOORD)
{
	texcoord.x = (id == 2) ? 2.0 : 0.0;
	texcoord.y = (id == 1) ? 2.0 : 0.0;
	position = float4(texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
technique T
{
	pass { VertexShader = PostProcessVS; PixelShader = APS; }
	pass { VertexShader = PostProcessVS; PixelShader = BPS; }
}
