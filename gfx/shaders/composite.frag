#version 330 core
// Final screen pass: blend in the (optionally half-res) motion blur where
// things actually move, then anime speed lines, slipstream air ribbons and a
// nitro edge tint.
in vec2 vUV;
uniform sampler2D uScene;
uniform sampler2D uDepth;
uniform sampler2D uBlur;
uniform bool uHasBlur;
uniform mat4 uInvViewProj;
uniform mat4 uPrevViewProj;
uniform float uCamBlur;
uniform float uRadial;
uniform float uMaxBlur;
uniform vec2 uResolution;
uniform float uTime;
uniform float uLines;       // speed-line amount 0..1+
uniform float uNitro;       // 0..1
uniform float uSlip;        // 0..1
out vec4 fragColor;

float hash(float n) { return fract(sin(n * 12.9898) * 43758.5453); }

vec2 motionVector(vec2 uv)
{
    float d = texture(uDepth, uv).r;
    vec4 world = uInvViewProj * vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    world /= world.w;
    vec4 prev = uPrevViewProj * world;
    vec2 v = (uv - (prev.xy / prev.w * 0.5 + 0.5)) * uCamBlur + (uv - 0.5) * uRadial;
    float l = length(v);
    return l > uMaxBlur ? v * (uMaxBlur / l) : v;
}

void main()
{
    vec4 sharp = texture(uScene, vUV);
    vec3 col = sharp.rgb;
    if (uHasBlur)
    {
        vec2 v = motionVector(vUV) * sharp.a;
        float px = length(v * uResolution);
        col = mix(col, texture(uBlur, vUV).rgb, smoothstep(0.75, 3.0, px));
    }

    float aspect = uResolution.x / uResolution.y;
    vec2 p = (vUV - 0.5) * vec2(aspect, 1.0);
    float r = length(p) / (0.5 * length(vec2(aspect, 1.0)));   // 0 centre .. 1 corner
    float ang = atan(p.y, p.x) / 6.2831853 + 0.5;

    // --- speed lines: thin radial streaks from the edges, re-rolled ~24x/s
    if (uLines > 0.001)
    {
        float N = 140.0;
        float a = ang * N;
        float cell = floor(a);
        float frame = floor(uTime * 24.0);
        float h1 = hash(cell + frame * 17.0);
        float h2 = hash(cell * 3.1 + frame * 5.0 + 11.0);
        float density = clamp(uLines, 0.0, 1.0) * 0.32;
        float on = step(1.0 - density, h1);
        float inner = mix(0.98, 0.55, clamp(uLines, 0.0, 1.0)) + h2 * 0.2;
        float x = abs(fract(a) - 0.5);                         // distance from the line centre, in cells
        float width = mix(0.06, 0.22, h2) * (0.4 + 0.6 * r);    // thicker towards the edge
        float fw = fwidth(a);
        float line = 1.0 - smoothstep(width - fw, width + fw, x);
        float radial = smoothstep(inner, inner + 0.12, r);
        float lineA = on * line * radial * (0.45 + 0.3 * uNitro);
        vec3 lineCol = mix(vec3(1.0), vec3(0.75, 0.9, 1.0), uNitro);
        col = mix(col, lineCol, clamp(lineA, 0.0, 0.85));
    }

    // --- slipstream: thin wavy air ribbons streaming outwards at the sides
    if (uSlip > 0.001)
    {
        float phase = ang * 9.0 + 0.22 * sin(r * 7.0 - uTime * 3.5) + 0.07 * sin(r * 19.0 + uTime * 2.0);
        float d = abs(fract(phase) - 0.5);                     // 0 on the ribbon centre line (cycles)
        float fw = max(fwidth(phase), 1e-4);
        float ribbon = 1.0 - smoothstep(0.012, 0.012 + 1.5 * fw, d);
        float seg = floor(phase);
        float dash = smoothstep(0.1, 0.8, sin(r * 9.0 - uTime * 14.0 + seg * 2.3));
        float mask = smoothstep(0.4, 0.8, r);
        col = mix(col, vec3(0.85, 0.97, 1.0), ribbon * dash * mask * 0.6 * uSlip);
    }

    // --- nitro: cool edge tint + slight exposure kick
    if (uNitro > 0.001)
    {
        float vig = smoothstep(0.45, 1.05, r);
        col = mix(col, col * vec3(0.75, 0.9, 1.15) + vec3(0.03, 0.06, 0.12), vig * 0.6 * uNitro);
        col *= 1.0 + 0.06 * uNitro;
    }
    fragColor = vec4(col, 1.0);
}
