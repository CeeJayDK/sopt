#version 410 core

// sopt test: a GLSL fragment shader (sopt-fx --glsl / .frag).
in vec2 vUv;
out vec4 fragColor;

uniform sampler2D uScene;
uniform float uAmount;
uniform vec2 uTexel;

float luma(vec3 c)
{
    return dot(c, vec3(0.299, 0.587, 0.114));
}

void main()
{
    vec3 c = texture(uScene, vUv).rgb;
    vec3 b = texture(uScene, vUv + vec2(uTexel.x, 0.0)).rgb;
    float l = luma(c);
    vec3 sharp = c + (c - b) * uAmount * 2.0;
    float v = fract(vUv.x * 0.5) * 2.0 + 1.0 - fract(vUv.x * 0.5) * 2.0;
    vec3 m = mix(sharp, vec3(l), 0.25) * 2.0 * 0.5;
    fragColor = vec4(clamp(m, 0.0, 1.0) * v, 1.0);
}
