#version 330 core

in  vec2 vUV;
out vec4 FragColor;
uniform float uTime;

float hash(vec2 p)  { return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5453); }
float hash1(float n){ return fract(sin(n)*43758.5453); }

float noise(vec2 p) {
    vec2 i=floor(p), f=fract(p);
    f=f*f*(3.0-2.0*f);
    return mix(mix(hash(i),hash(i+vec2(1,0)),f.x),
               mix(hash(i+vec2(0,1)),hash(i+vec2(1,1)),f.x),f.y);
}

float fbm(vec2 p) {
    float v=0.,a=0.5;
    for(int i=0;i<7;i++){v+=a*noise(p);p*=2.05;a*=0.5;}
    return v;
}

void main() {
    vec2 uv = vUV * 2.0 - 1.0;
    float t  = uTime * 0.06;

    // Domain-warped nebula cloud
    vec2 q = vec2(fbm(uv*0.8 + t), fbm(uv*0.8 + vec2(5.2,1.3) + t));
    float f = fbm(uv + 2.0*q + t*0.5);

    // Nebula palette — deep space indigo → electric blue → gold stellar core
    vec3 col = mix(vec3(0.0, 0.005, 0.03),
                   vec3(0.04, 0.02, 0.25), clamp(f*2.0,0.,1.));
    col = mix(col, vec3(0.08, 0.12, 0.55),  clamp(f*f*3.5,0.,1.));
    col = mix(col, vec3(0.65, 0.42, 0.08),  clamp((f-0.65)*4.5,0.,1.));
    col = mix(col, vec3(1.0,  0.85, 0.4),   clamp((f-0.82)*6.0,0.,1.));

    // Star field — two density layers
    vec2 sg1 = floor(uv * 55.0);
    vec2 sf1 = fract(uv * 55.0) - 0.5;
    float s1 = hash(sg1);
    if(s1 > 0.955) {
        float tw = 0.6 + 0.4*sin(uTime*2.5 + s1*30.0);
        col += vec3(0.9,0.88,0.75) * (1.0-smoothstep(0.0,0.12,length(sf1))) * tw * 2.5;
    }
    vec2 sg2 = floor(uv * 120.0);
    vec2 sf2 = fract(uv * 120.0) - 0.5;
    float s2 = hash(sg2 + 999.0);
    if(s2 > 0.97) {
        col += vec3(0.7,0.8,1.0) * (1.0-smoothstep(0.0,0.08,length(sf2))) * 1.8;
    }

    // Bright stellar nursery cluster — upper right
    vec2 cluster_uv = uv - vec2(0.4, -0.3);
    float cluster = exp(-dot(cluster_uv,cluster_uv) * 3.5);
    col += vec3(0.85, 0.60, 0.15) * cluster * (0.5 + 0.5*sin(uTime*0.6));

    // Pulsar strobe — lower left
    vec2 pulsar_uv = uv - vec2(-0.55, 0.4);
    float pulsar_r = length(pulsar_uv);
    float beam = exp(-pulsar_r * 8.0) * (0.5 + 0.5*sin(uTime*4.0));
    col += vec3(0.3, 0.6, 1.0) * beam * 0.8;

    // Subtle vignette — space goes to black at edges
    col *= clamp(1.0 - dot(uv*0.45, uv*0.45), 0., 1.);

    FragColor = vec4(col, 1.0);
}
