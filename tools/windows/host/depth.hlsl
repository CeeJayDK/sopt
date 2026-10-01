// sopt-host depth pass for DX11 (same scene as depth.vert); compiled to depth_dxbc.h with
// Microsoft's D3DCompile (vs_5_0, entry VS, O3), see tools/host/README.md.
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
// id comes from a vertex buffer (0, 1, 2, ...): SV_VertexID does not include a draw's
// start vertex in D3D11, and each row is one draw.
float4 VS(uint id : TEXCOORD0) : SV_Position {
  uint cell = id / 6, k = id % 6;  // two triangles per cell: corners 00 10 01, 10 11 01
  float2 corner = float2(k == 1 || k == 3 || k == 4 ? 1.0 : 0.0, k == 2 || k >= 4 ? 1.0 : 0.0);
  float2 uv = (float2(cell % 256, cell / 256) + corner) / float2(256.0, 144.0);
  const float n = 0.1, f = 1000.0;
  float depth = saturate(1.0 - f / (f - n) * (1.0 - n / sceneDistance(uv)));  // reversed Z
  return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), depth, 1.0);
}
