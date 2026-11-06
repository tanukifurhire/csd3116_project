#version 330 core
in vec4 vColor;
in vec2 vUV;
uniform sampler2D uTex;
out vec4 fragColor;
void main() { fragColor = vColor * texture(uTex, vUV); }
