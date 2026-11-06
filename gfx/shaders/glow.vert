#version 330 core
// Camera-facing glow / ring sprites for nitro halos and ignition bursts.
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;   // rgb (premultiplied later), a = opacity
layout(location = 3) in vec2 aParams;  // x = kind (0 glow, 1 ring), y = ring radius
uniform mat4 uViewProj;
out vec2 vUV;
out vec4 vColor;
out vec2 vParams;
void main()
{
    vUV = aUV;
    vColor = aColor;
    vParams = aParams;
    gl_Position = uViewProj * vec4(aPos, 1.0);
}
