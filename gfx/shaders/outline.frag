#version 330 core
in float vDist;
uniform vec3 uInk;
uniform vec3 uFogColor;
uniform vec2 uFogRange;
uniform float uBlurMask;
out vec4 fragColor;
void main()
{
    float fog = smoothstep(uFogRange.x, uFogRange.y, vDist);
    fragColor = vec4(mix(uInk, uFogColor, fog), uBlurMask);
}
