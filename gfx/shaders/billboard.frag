#version 330 core
in vec2 vUV;
in vec4 vColor;
in float vDist;
uniform sampler2D uTex;
uniform bool uHasTex;
uniform vec3 uFogColor;
uniform vec2 uFogRange;
out vec4 fragColor;
void main()
{
    float a;
    if (uHasTex)
        a = texture(uTex, vUV).a;
    else
    {
        float r = length(vUV - 0.5) * 2.0;
        a = 1.0 - smoothstep(0.55, 1.0, r);
    }
    // a couple of hard steps keep the smoke in the cel look
    a = smoothstep(0.05, 0.15, a) * 0.6 + smoothstep(0.45, 0.55, a) * 0.4;
    float fog = smoothstep(uFogRange.x, uFogRange.y, vDist);
    fragColor = vec4(mix(vColor.rgb, uFogColor, fog), a * vColor.a);
    if (fragColor.a < 0.004) discard;
}
