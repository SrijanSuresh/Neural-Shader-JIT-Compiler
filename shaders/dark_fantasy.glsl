#version 330 core

in  vec2 vUV;
out vec4 FragColor;
uniform float uTime;

float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float noise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i+vec2(1,0)), f.x),
               mix(hash(i+vec2(0,1)), hash(i+vec2(1,1)), f.x), f.y);
}

float fbm(vec2 p) {
    float v=0., a=0.5;
    for(int i=0;i<6;i++){v+=a*noise(p);p*=2.1;a*=0.48;}
    return v;
}

void main() {
    vec2 uv = vUV * 2.0 - 1.0;
    float t = uTime * 0.3;

    vec2 q = vec2(fbm(uv+t), fbm(uv+vec2(1.3,t)));
    vec2 r = vec2(fbm(uv+2.0*q+vec2(1.7,9.2)+0.15*t),
                  fbm(uv+2.0*q+vec2(8.3,2.8)+0.12*t));
    float f = fbm(uv + 2.5*r);

    vec3 col = mix(vec3(0.04,0.01,0.02), vec3(0.9,0.08,0.12), clamp(f*2.2,0.,1.));
    col = mix(col, vec3(0.35,0.08,0.65), clamp(f*f*3.5,0.,1.));
    col = mix(col, vec3(1.0,0.55,0.05),  clamp((f-0.7)*4.0,0.,1.));

    float moon = length(uv - vec2(0.55,-0.65));
    col += vec3(0.7,0.08,0.0) * exp(-moon*3.0) * (0.6+0.4*sin(uTime*0.7));
    col += vec3(1.0,0.5,0.1) * smoothstep(0.18,0.12,moon);

    float angle = atan(uv.y,uv.x);
    float rune = abs(sin(angle*6.0+uTime*0.4)) * exp(-length(uv)*1.8);
    col += vec3(0.1,0.0,0.4)*rune*0.6;

    col *= clamp(1.0-dot(uv*0.5,uv*0.5),0.,1.);
    FragColor = vec4(col,1.0);
}
