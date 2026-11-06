#version 330 core
// Camera-motion blur (depth reprojection with last frame's view-projection)
// plus a radial blur towards the screen centre for speed/nitro. Scene alpha
// is a blur mask: 0 = camera-attached object (player car), never smeared and
// never smeared *into* the background.
in vec2 vUV;
uniform sampler2D uScene;   // rgb + blur mask in a
uniform sampler2D uDepth;
uniform mat4 uInvViewProj;
uniform mat4 uPrevViewProj;
uniform float uCamBlur;     // camera-motion strength
uniform float uRadial;      // radial blur amount (uv units at the screen edge)
uniform float uMaxBlur;     // clamp, uv units
uniform int uSamples;
out vec4 fragColor;

vec2 motionVector(vec2 uv)
{
    float d = texture(uDepth, uv).r;
    vec4 ndc = vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    vec4 world = uInvViewProj * ndc;
    world /= world.w;
    vec4 prev = uPrevViewProj * world;
    vec2 prevUV = prev.xy / prev.w * 0.5 + 0.5;
    vec2 v = (uv - prevUV) * uCamBlur + (uv - 0.5) * uRadial;
    float l = length(v);
    return l > uMaxBlur ? v * (uMaxBlur / l) : v;
}

void main()
{
    vec4 centre = texture(uScene, vUV);
    vec2 v = motionVector(vUV) * centre.a;
    vec3 acc = centre.rgb;
    float wsum = 1.0;
    int n = max(uSamples, 2);
    // interleaved-gradient-noise jitter turns few-sample ghosting into fine grain
    float jitter = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715)))) - 0.5;
    for (int i = 0; i < n; ++i)
    {
        float t = (float(i) + 0.5 + jitter) / float(n) - 0.5;
        vec4 s = texture(uScene, vUV + v * t);
        acc += s.rgb * s.a;   // masked pixels (player car) don't bleed
        wsum += s.a;
    }
    fragColor = vec4(acc / wsum, 1.0);
}
