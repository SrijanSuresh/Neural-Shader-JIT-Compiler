#version 330 core

in  vec2 vUV;
out vec4 FragColor;
uniform float uTime;

float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(
        mix(hash(i),             hash(i + vec2(1.0, 0.0)), f.x),
        mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), f.x),
        f.y);
}

float fbm(vec2 p) {
    float v = 0.0, a = 0.5;
    for (int i = 0; i < 6; i++) { v += a * noise(p); p *= 2.1; a *= 0.48; }
    return v;
}

void main() {
    vec2 uv = vUV * 2.0 - 1.0;
    float t  = uTime * 0.25;

    // Domain-warped fog — two layers of fbm for swirling motion
    vec2 q = vec2(fbm(uv + t),           fbm(uv + vec2(1.3, t)));
    vec2 r = vec2(fbm(uv + 2.0*q + vec2(1.7, 9.2) + 0.15*t),
                  fbm(uv + 2.0*q + vec2(8.3, 2.8) + 0.12*t));
    float f = fbm(uv + 2.8 * r);

    // Bloodborne/Elden Ring palette: near-black -> deep crimson -> dark violet
    vec3 col = mix(vec3(0.02, 0.005, 0.01),
                   vec3(0.35,  0.015, 0.04),
                   clamp(f * 2.0, 0.0, 1.0));
    col = mix(col,
              vec3(0.12, 0.04, 0.22),
              clamp(f * f * 4.0, 0.0, 1.0));
    col = mix(col,
              vec3(0.55, 0.3, 0.08),
              clamp((f - 0.8) * 5.0, 0.0, 1.0));

    // Pulsing blood-moon glow from top-right
    float moon = length(uv - vec2(0.6, -0.7));
    col += vec3(0.18, 0.02, 0.0) * exp(-moon * 2.0) * (0.7 + 0.3 * sin(uTime * 0.8));

    // Vignette
    float v = 1.0 - dot(uv * 0.65, uv * 0.65);
    col *= clamp(v, 0.0, 1.0);

    FragColor = vec4(col, 1.0);
}
