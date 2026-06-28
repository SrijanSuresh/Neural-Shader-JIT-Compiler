#version 330 core

in  vec2 vUV;
out vec4 FragColor;
uniform float uTime;

float sdBox(vec2 p, vec2 b) {
    vec2 d = abs(p) - b;
    return length(max(d,0.0)) + min(max(d.x,d.y),0.0);
}

vec2 rot(vec2 p, float a) {
    return vec2(p.x*cos(a)-p.y*sin(a), p.x*sin(a)+p.y*cos(a));
}

void main() {
    vec2 uv = vUV * 2.0 - 1.0;
    float t = uTime;

    // Rotating geometric layers — Persona 5 UI aesthetic
    vec2 r1 = rot(uv, t * 0.4);
    vec2 r2 = rot(uv, -t * 0.25);

    // Diamond tiling
    float d1 = abs(r1.x) + abs(r1.y);
    float pattern = step(0.5, fract(d1 * 2.5 - t * 0.3));

    // Concentric rings with spoke modulation
    float radius = length(uv);
    float theta  = atan(uv.y, uv.x);
    float ring   = step(0.5, fract((radius - t*0.15) * 4.0));
    float spoke  = step(0.5, fract((theta / 6.2832 + t*0.05) * 8.0));

    // SDF boxes — nested rotating frames
    float box1 = sdBox(r2, vec2(0.35, 0.35));
    float box2 = sdBox(r1 * 0.7, vec2(0.25, 0.25));
    float frame = clamp(smoothstep(0.02,-0.02,box1) - smoothstep(0.01,-0.01,box2), 0., 1.);

    // Persona palette: electric yellow, hot pink, cyan, dark base
    vec3 bg     = vec3(0.04, 0.02, 0.08);
    vec3 yellow = vec3(0.98, 0.85, 0.0);
    vec3 pink   = vec3(0.95, 0.12, 0.45);
    vec3 cyan   = vec3(0.05, 0.85, 0.95);
    vec3 white  = vec3(0.95, 0.95, 1.0);

    vec3 col = bg;
    col = mix(col, yellow * 0.55, pattern * 0.45);
    col = mix(col, pink,  ring * spoke * 0.65);
    col = mix(col, cyan,  frame * 0.85);
    col += white * smoothstep(0.01, 0.0, box1) * 0.7;

    // All-Out-Attack radial glow from center
    col += yellow * 0.5 * exp(-radius * 2.0) * (0.7 + 0.3*sin(t * 3.0));
    col += pink   * 0.35 * exp(-radius * 3.5) * (0.5 + 0.5*cos(t * 2.3));

    // Anime scan-line filter
    col *= 0.9 + 0.1 * sin(uv.y * 220.0 + t);

    // Vignette
    col *= 1.0 - 0.35 * dot(uv*0.8, uv*0.8);

    FragColor = vec4(clamp(col,0.,1.), 1.0);
}
