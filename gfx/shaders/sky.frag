#version 330 core
in vec3 vDir;
uniform samplerCube uSky;
uniform bool uHasSky;
uniform vec3 uFogColor;
uniform float uHorizonFog;   // 0..1 how strongly the horizon blends into fog
out vec4 fragColor;
void main()
{
    vec3 d = normalize(vDir);
    vec3 sky = uHasSky ? texture(uSky, d).rgb
                       : mix(uFogColor, vec3(0.35, 0.55, 0.85), smoothstep(0.0, 0.5, d.y));
    // blend the band around the horizon into the fog colour so distant fogged
    // geometry meets the sky without a seam
    float h = 1.0 - smoothstep(-0.02, 0.22, d.y);
    fragColor = vec4(mix(sky, uFogColor, h * uHorizonFog), 1.0);
}
