#version 330 core

in  vec2 vUV;
out vec4 FragColor;
uniform float uTime;

// Smooth Voronoi — mimics cellular membrane networks
vec2 hash2(vec2 p) {
    return fract(sin(vec2(dot(p,vec2(127.1,311.7)),
                          dot(p,vec2(269.5,183.3)))) * 43758.5453);
}

float voronoi(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    float d = 8.0;
    for(int y=-1;y<=1;y++)
        for(int x=-1;x<=1;x++){
            vec2 b = vec2(x,y);
            vec2 r = b + hash2(i+b) - f;
            d = min(d, dot(r,r));
        }
    return sqrt(d);
}

float hash(float n) { return fract(sin(n)*43758.5453); }

void main() {
    vec2 uv = vUV * 2.0 - 1.0;
    float t  = uTime * 0.18;

    // Primary cell layer — large membranes
    float cells    = voronoi(uv * 3.5 + t * 0.5);
    float membrane = 1.0 - smoothstep(0.04, 0.15, cells);

    // Secondary: organelles (smaller, faster drift)
    float org       = voronoi(uv * 10.0 + vec2(t*1.4, t*0.8));
    float org_mask  = smoothstep(0.25, 0.12, org);

    // Tertiary: fluid between cells
    float fluid = voronoi(uv * 1.8 - t * 0.3);

    // Nucleus SDF — pulsing central structure
    float nucleus_r = 0.13 + 0.03 * sin(uTime * 1.8);
    float nucleus   = smoothstep(0.01, -0.01, length(uv) - nucleus_r);
    float halo      = exp(-length(uv) * 5.0);

    // Fluorescence microscopy palette
    vec3 bg          = vec3(0.01, 0.03, 0.07);
    vec3 cytoplasm   = vec3(0.03, 0.18, 0.10);
    vec3 membrane_c  = vec3(0.08, 0.60, 0.85);   // cyan-blue walls
    vec3 organelle_c = vec3(0.92, 0.50, 0.08);   // amber organelles
    vec3 nucleus_c   = vec3(0.25, 0.02, 0.70);   // deep violet nucleus

    vec3 col = mix(bg, cytoplasm, smoothstep(0.85, 0.3, fluid));
    col = mix(col, membrane_c, membrane * 0.9);
    col = mix(col, organelle_c, org_mask * 0.85);

    // Nucleus core + outer glow
    col = mix(col, nucleus_c, nucleus);
    col += nucleus_c * 0.35 * halo;

    // Fluorescent pulse — cells "breathing"
    float pulse = 0.5 + 0.5 * sin(uTime * 2.2);
    col += membrane_c * 0.12 * membrane * pulse;
    col += organelle_c * 0.08 * org_mask * (1.0 - pulse);

    // Edge vignette
    col *= clamp(1.0 - dot(uv*0.55, uv*0.55), 0., 1.);

    FragColor = vec4(col, 1.0);
}
