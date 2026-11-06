#version 330 core
// Camera-facing quads (tyre smoke). One quad per particle, expanded on CPU.
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;
uniform mat4 uViewProj;
uniform vec3 uCamPos;
out vec2 vUV;
out vec4 vColor;
out float vDist;
void main()
{
    vUV = aUV;
    vColor = aColor;
    vDist = length(uCamPos - aPos);
    gl_Position = uViewProj * vec4(aPos, 1.0);
}
