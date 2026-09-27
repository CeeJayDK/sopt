#version 450
// sopt-host depth pass: a 256 x 144 grid of quads covering the screen (no vertex buffer),
// one row per draw call (firstVertex picks the row): ReShade's generic depth ignores depth
// buffers with <= 3 vertices or <= 8 draw calls a frame. The scene depth is computed per
// vertex and written through z, like a game's z prepass (no fragment shader). Keep in sync
// with kDepthHlsl in sopt_host.cpp: ground plane up to a horizon, sky at the far plane,
// three spheres; reversed Z (1 = near, 0 = far: ReShade's default
// RESHADE_DEPTH_INPUT_IS_REVERSED = 1, like most current games), near 0.1, far 1000.
float sphere(float z, vec2 p, vec2 c, float r, float d) {
  vec2 q = (p - c) * vec2(16.0 / 9.0, 1.0);
  float r2 = r * r - dot(q, q);
  return r2 > 0.0 ? min(z, d - sqrt(r2) * d) : z;
}
float sceneDistance(vec2 p) {
  float z = 1000.0;                                           // sky
  if (p.y > 0.45) z = min(z, 2.0 / (p.y - 0.45));             // ground
  z = sphere(z, p, vec2(0.25, 0.62), 0.09, 8.0);
  z = sphere(z, p, vec2(0.62, 0.55), 0.05, 25.0);
  return sphere(z, p, vec2(0.82, 0.74), 0.13, 4.0);
}
void main() {
  uint cell = uint(gl_VertexIndex) / 6u, k = uint(gl_VertexIndex) % 6u;  // corners 00 10 01, 10 11 01
  vec2 corner = vec2(k == 1u || k == 3u || k == 4u ? 1.0 : 0.0, k == 2u || k >= 4u ? 1.0 : 0.0);
  vec2 uv = (vec2(cell % 256u, cell / 256u) + corner) / vec2(256.0, 144.0);
  const float n = 0.1, f = 1000.0;
  float depth = clamp(1.0 - f / (f - n) * (1.0 - n / sceneDistance(uv)), 0.0, 1.0);  // reversed Z
  gl_Position = vec4(uv * 2.0 - 1.0, depth, 1.0);
}
