#version 330 core
// Cube map skybox drawn last among opaques at depth = far plane.
layout(location = 0) in vec3 aPos;
uniform mat4 uViewRot;   // view matrix without translation
uniform mat4 uProj;
out vec3 vDir;
void main()
{
    vDir = aPos;
    vec4 p = uProj * uViewRot * vec4(aPos, 1.0);
    gl_Position = p.xyww;   // z/w = 1 -> far plane
}
