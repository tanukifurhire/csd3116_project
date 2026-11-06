#version 330 core
// Stylised nitro flame: a unit teardrop shell (z 0..1 along the exhaust,
// radius ~1) scaled per layer, with flicker and lateral wobble.
layout(location = 0) in vec3 aPos;    // unit flame shell
layout(location = 1) in vec3 aNormal;
uniform mat4 uModel;       // exhaust frame * scale(radius, radius, length)
uniform mat4 uViewProj;
uniform vec3 uCamPos;
uniform float uTime;
uniform float uSeed;
out float vZ;
out float vFacing;
out vec3 vWorld;
void main()
{
    float z = aPos.z;
    float t = uTime + uSeed;
    // flicker: length pulses, tip wobbles sideways
    float flick = 0.86 + 0.09 * sin(t * 41.0) + 0.06 * sin(t * 67.0 + 1.3) + 0.04 * sin(t * 97.0 + 2.1);
    vec3 p = aPos;
    p.z *= flick;
    float wob = z * z * 0.35;
    p.x += wob * sin(t * 23.0 + z * 6.0);
    p.y += wob * cos(t * 19.0 + z * 5.0);
    vec4 w = uModel * vec4(p, 1.0);
    vec3 n = normalize(mat3(uModel) * aNormal);
    vWorld = w.xyz;
    vZ = z;
    vFacing = abs(dot(n, normalize(uCamPos - w.xyz)));
    gl_Position = uViewProj * w;
}
