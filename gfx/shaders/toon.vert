#version 330 core
// Toon-shaded geometry (opaque + transparent passes).
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec3 aColor;
layout(location = 3) in vec2 aUV;

uniform mat4 uModel;
uniform mat3 uNormalMat;
uniform mat4 uViewProj;
uniform mat4 uLightVP;

out vec3 vWorldPos;
out vec3 vNormal;
out vec3 vColor;
out vec2 vUV;
out vec4 vLightPos;

void main()
{
    vec4 wp = uModel * vec4(aPos, 1.0);
    vWorldPos = wp.xyz;
    vNormal = uNormalMat * aNormal;
    vColor = aColor;
    vUV = aUV;
    vLightPos = uLightVP * wp;
    gl_Position = uViewProj * wp;
}
