#version 330 core
// Inverted-hull ink outline: the mesh is drawn again with front faces culled
// and each vertex pushed out along its (smoothed) normal in screen space, so
// the line is a roughly constant number of pixels wide.
layout(location = 0) in vec3 aPos;
layout(location = 4) in vec3 aSmoothNormal;

uniform mat4 uModel;
uniform mat3 uNormalMat;
uniform mat4 uView;
uniform mat4 uProj;
uniform vec2 uViewport;     // framebuffer size in pixels
uniform float uWidthPx;     // outline width in pixels at close range
uniform vec3 uCamPos;

out float vDist;

void main()
{
    vec4 wp = uModel * vec4(aPos, 1.0);
    vec3 nW = normalize(uNormalMat * aSmoothNormal);
    vec4 clip = uProj * (uView * wp);
    vec2 dir = (uProj * vec4(mat3(uView) * nW, 0.0)).xy;
    float len = length(dir);
    dir = len > 1e-5 ? dir / len : vec2(0.0);
    float dist = length(uCamPos - wp.xyz);
    vDist = dist;
    // thinner lines far away so distant objects don't turn into ink blobs
    float w = uWidthPx * clamp(1.4 - dist / 60.0, 0.35, 1.0);
    clip.xy += dir * (w * 2.0 / uViewport) * clip.w;
    clip.z += 0.00002 * clip.w;   // keep the hull just behind coplanar surfaces
    gl_Position = clip;
}
