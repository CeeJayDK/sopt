#version 450
// Full-screen triangle for --batch pixel shaders without their own vertex shader: position and a texcoord
// at locations 0..3 (a pixel shader may read fewer components / locations).
layout(location = 0) out vec4 o0;
layout(location = 1) out vec4 o1;
layout(location = 2) out vec4 o2;
layout(location = 3) out vec4 o3;
void main() {
  vec2 t = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
  gl_Position = vec4(t * vec2(2.0, -2.0) + vec2(-1.0, 1.0), 0.0, 1.0);
  o0 = vec4(t, 0.0, 1.0);
  o1 = o0;
  o2 = o0;
  o3 = o0;
}
