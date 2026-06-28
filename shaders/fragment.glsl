#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform float uTime;
void main() {
    FragColor = vec4(vUV, 0.5 + 0.5 * sin(uTime), 1.0);
}
