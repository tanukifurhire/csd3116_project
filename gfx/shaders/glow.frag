#version 330 core
in vec2 vUV;
in vec4 vColor;
in vec2 vParams;
out vec4 fragColor;
void main()
{
    float r = length(vUV - 0.5) * 2.0;
    float a;
    if (vParams.x < 0.5)
    {
        // two-step toon glow: soft outer haze + crisp hot centre
        a = (1.0 - smoothstep(0.0, 1.0, r)) * 0.55 + (1.0 - smoothstep(0.30, 0.38, r)) * 0.45;
        a *= a;
    }
    else
    {
        float rr = vParams.y;
        float w = 0.12;
        a = 1.0 - smoothstep(w * 0.6, w, abs(r - rr));
    }
    if (a < 0.003) discard;
    // premultiplied: mostly additive, a little occlusion
    fragColor = vec4(vColor.rgb * a, a * vColor.a);
}
