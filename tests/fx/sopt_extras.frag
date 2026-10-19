#version 410 core

// sopt test: GLSL built-ins ReShade FX lacks (asinh, acosh, atanh) load and are regions; one-argument atan
// (FFmpeg's v360 crashed the parser, 0.8.0).
in vec2 vUv;
out vec4 fragColor;

uniform sampler2D uScene;

void main()
{
    vec3 c = texture(uScene, vUv).rgb;
    float a = asinh(c.r * 4.0 - 2.0) * 0.5;
    float b = acosh(c.g * 3.0 + 1.0) * 0.5;
    float t = atanh(c.b * 1.8 - 0.9) * 0.5 + 0.5;
    float u = atan(c.r * 2.0 - 1.0) * 0.6;
    fragColor = vec4(a, b, t, u);
}
