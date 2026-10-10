#version 410 core

// sopt test: GLSL built-ins ReShade FX lacks (asinh, acosh, atanh) load and are regions.
in vec2 vUv;
out vec4 fragColor;

uniform sampler2D uScene;

void main()
{
    vec3 c = texture(uScene, vUv).rgb;
    float a = asinh(c.r * 4.0 - 2.0) * 0.5;
    float b = acosh(c.g * 3.0 + 1.0) * 0.5;
    float t = atanh(c.b * 1.8 - 0.9) * 0.5 + 0.5;
    fragColor = vec4(a, b, t, 1.0);
}
