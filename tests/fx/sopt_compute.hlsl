// Plain HLSL compute shader for test_fx: [numthreads] entry, RW textures and buffers, buffers,
// groupshared memory, barriers and Interlocked*.
cbuffer Params : register(b0)
{
	float Strength;
	float2 InvSize;
};

struct Particle { float3 pos; float life; };

Texture2D<float4> Input : register(t0);
Texture2D<float> Depth : register(t1);
StructuredBuffer<float4> Weights : register(t2);
StructuredBuffer<Particle> Particles : register(t3);
Buffer<float2> Offsets : register(t4);
ByteAddressBuffer Raw : register(t5);
SamplerState Linear : register(s0);
RWTexture2D<float4> Output : register(u0);
RWTexture2D<float2> Motion : register(u1);
RWStructuredBuffer<float> Sums : register(u2);
RWStructuredBuffer<uint> Counter : register(u3);

groupshared float tile[64];
groupshared uint hits;

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 tid : SV_GroupThreadID, uint gi : SV_GroupIndex, uint3 gid : SV_GroupID)
{
	uint w, h;
	Output.GetDimensions(w, h);
	float4 c = Input[id.xy];
	float d = Depth.Load(int3(id.xy, 0));
	float2 uv = (float2(id.xy) + 0.5) * InvSize;
	float4 s = Input.SampleLevel(Linear, uv, 0);
	float4 wt = Weights[gi];
	float life = Particles[gi].life;
	float2 off = Offsets[gi];
	uint raw = Raw.Load(gi * 4);
	float l = dot(c.rgb, float3(0.2126, 0.7152, 0.0722));
	float4 e = Input[id.xy] * 2.0 - 1.0;
	float m = Weights[gi].x * 0.5 + Weights[gi].y * 0.5;
	tile[gi] = l;
	GroupMemoryBarrierWithGroupSync();
	float n = tile[(gi + 1) % 64];
	if (gi == 0) hits = 0;
	GroupMemoryBarrierWithGroupSync();
	uint before;
	InterlockedAdd(hits, 1, before);
	InterlockedAdd(Counter[0], 1);
	float v = 1.0 - (1.0 - l) * (1.0 - Strength);
	Output[id.xy] = float4(lerp(c.rgb, v.xxx, step(0.5, uv.x)), 1.0);
	Motion[id.xy] = off * 2.0 + off * d;
	float2 prev = Motion[id.xy];
	Sums[gi] = n * 2.0 + n * life + prev.x * wt.x + s.x;
	float g = float(tid.x) * 0.125 + float(tid.y) * 0.125 + float(gid.x);
	Output[id.xy] += g;
	float2 old = Motion[id.xy];
	Motion[id.xy] = off;
	float z = old.x * 0.5 + old.y * 0.5 * Strength;
}
