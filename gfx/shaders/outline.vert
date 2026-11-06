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

out float vDist;

void main()
{
    vec4 wp = uModel * vec4(aPos, 1.0);
    vec3 nW = normalize(uNormalMat * aSmoothNormal);
    vec4 ve = uView * wp;
    float dist = length(ve.xyz);
    vDist = dist;
    // Keep the hull behind the surfaces it outlines: slide it away from the
    // camera along the view ray (same pixel, more depth) by 2% of the
    // distance. A constant depth bias is only millimetres at chase-camera
    // range, so where expanded back faces overlap front faces seen at a
    // grazing angle (roof, pillars, tree trunks) the hull won the depth test
    // on scattered pixels: ink speckles that crawled and flickered in motion.
    ve.xyz *= 1.02;
    vec4 clip = uProj * ve;
    vec2 dir = (uProj * vec4(mat3(uView) * nW, 0.0)).xy;
    float len = length(dir);
    dir = len > 1e-5 ? dir / len : vec2(0.0);
    // thinner lines far away so distant objects don't turn into ink blobs,
    // but never under ~1.2 px: sub-pixel lines break into dashes that shimmer
    float w = max(uWidthPx * clamp(1.4 - dist / 60.0, 0.35, 1.0), 1.2);
    clip.xy += dir * (w * 2.0 / uViewport) * clip.w;
    gl_Position = clip;
}
