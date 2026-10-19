#version 150
// sopt test: GLSL as WebRender (Firefox) writes it: functions returning arrays, array constructors and array-typed
// locals, a prototype with the definition further down, precision qualifiers on parameters, gl_FragCoord in a helper,
// a last switch case without a break. The file loads, and lines 22, 30 (main, moved after pattern's definition) and
// 43 are regions.
in vec2 vUv;
out vec4 oFragColor;
uniform sampler2D sColor0;
uniform int uMode;

vec4 pattern(vec4 c);

vec4[2] pair(vec2 uv) {
    return vec4[2](texture(sColor0, uv), texture(sColor0, uv + 0.5));
}

vec2 pixel() {
    return gl_FragCoord.xy * 0.5;
}

float weight(highp float x, mediump float y) {
    return x * 0.25 + y * 0.75;
}

void main() {
    vec4[2] p = pair(vUv);
    int[2] modes = int[2](uMode, 1);
    vec4 c = pattern(p[0]) + p[1];
    float w = weight(c.x, c.y) + pixel().x;
    float k = c.z * 2.0 + c.z * 3.0;
    switch (modes[0]) {
        case 0:
            c.x = w;
            break;
        default:
            c.y = k;
    }
    oFragColor = c;
}

vec4 pattern(vec4 c) {
    vec4 r = c;
    r.w = c.x * 4.0 - c.x;
    return r;
}
