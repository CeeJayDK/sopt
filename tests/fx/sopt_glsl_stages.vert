#version 450
// Test shader for test_fx: a GLSL vertex shader as AnyPS5 writes them (2026-10-31): gl_PerVertex redeclared, a named
// uniform block, a FX keyword (linear) and an FX reserved word (packed) as names, bitCount, gl_VertexIndex.
out gl_PerVertex { vec4 gl_Position; };
layout(push_constant) uniform Parameters { float gain; vec2 offset; } parameters;
layout(location = 0) out vec3 color;
void main() {
    float linear = float(gl_VertexIndex) * 0.5;
    uint packed = uint(bitCount(uint(gl_VertexIndex)));
    vec2 p = vec2(linear, float(packed)) * parameters.gain + parameters.offset;
    p = p * p * 2.0 + p;
    gl_Position = vec4(p, 0.5, 1.0);
    color = vec3(p, linear);
}
