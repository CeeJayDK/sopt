// ReShade FX compute shader for test_fx: thread IDs as inputs, storage writes, groupshared memory.
texture TexIn { Width = 1920; Height = 1080; Format = RGBA8; };
sampler SmpIn { Texture = TexIn; };
texture TexOut { Width = 1920; Height = 1080; Format = RGBA8; };
storage StOut { Texture = TexOut; };
texture TexF { Width = 1920; Height = 1080; Format = RGBA16F; };
storage StF { Texture = TexF; };

uniform float Strength < ui_min = 0.0; ui_max = 1.0; > = 0.5;

groupshared float tile[64];

void CSMain(uint3 id : SV_DispatchThreadID, uint3 tid : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
	float4 c = tex2Dfetch(SmpIn, id.xy);
	float2 uv = (float2(id.xy) + 0.5) / float2(1920.0, 1080.0);
	float g = float(tid.x) * 0.125 + float(tid.y) * 0.125;
	float l = dot(c.rgb, float3(0.2126, 0.7152, 0.0722));
	tile[gi] = l;
	barrier();
	float n = tile[(gi + 1) % 64];
	float v = 1.0 - (1.0 - l) * (1.0 - Strength);
	tex2Dstore(StOut, id.xy, float4(lerp(c.rgb, v.xxx, step(0.5, uv.x)), 1.0));
	tex2Dstore(StF, id.xy, float4(n * 2.0 + n * g, uv, 1.0));
}

technique CSTest
{
	pass { ComputeShader = CSMain<8, 8>; DispatchSizeX = 240; DispatchSizeY = 135; }
}
