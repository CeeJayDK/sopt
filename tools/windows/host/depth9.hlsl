// sopt-host depth pass for Direct3D 9 (the scene of depth.hlsl in float math: shader model 3 has no integers);
// compiled to depth9_dxbc.h with Microsoft's D3DCompile (vs_3_0 VS, ps_3_0 PS, O3).
float sphere(float z, float2 p, float2 c, float r, float d) {
  float2 q = (p - c) * float2(16.0 / 9.0, 1.0);
  float r2 = r * r - dot(q, q);
  return r2 > 0.0 ? min(z, d - sqrt(r2) * d) : z;
}
float sceneDistance(float2 p) {
  float z = 1000.0;
  if (p.y > 0.45) z = min(z, 2.0 / (p.y - 0.45));
  z = sphere(z, p, float2(0.25, 0.62), 0.09, 8.0);
  z = sphere(z, p, float2(0.62, 0.55), 0.05, 25.0);
  return sphere(z, p, float2(0.82, 0.74), 0.13, 4.0);
}
// id from a vertex buffer of floats 0, 1, 2, ... (exact up to 2^24).
float4 VS(float id : TEXCOORD0) : POSITION {
  float cell = floor(id / 6.0 + 1e-3), k = id - cell * 6.0;  // two triangles per cell: corners 00 10 01, 10 11 01
  float2 corner = float2(k == 1.0 || k == 3.0 || k == 4.0 ? 1.0 : 0.0, k == 2.0 || k >= 4.0 ? 1.0 : 0.0);
  float row = floor(cell / 256.0 + 1e-4);
  float2 uv = (float2(cell - row * 256.0, row) + corner) / float2(256.0, 144.0);
  const float n = 0.1, f = 1000.0;
  float depth = saturate(1.0 - f / (f - n) * (1.0 - n / sceneDistance(uv)));  // reversed Z
  return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), depth, 1.0);
}
// Direct3D 9 pairs a vs_3_0 with a ps_3_0 (no fixed-function pixel stage after it); colour writes are off.
float4 PS() : COLOR { return 0.0; }
