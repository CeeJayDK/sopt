// Header for test_fx: a helper whose statement is a region, found through the effect.
float3 SoptHelper(float3 c)
{
	float3 r = c * 0.5 + c * 0.5;
	return r;
}
