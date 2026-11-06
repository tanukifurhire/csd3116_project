#version 330 core
// Cel shading: banded sun diffuse with soft band edges, hard specular
// highlight, rim light, ambient, distance fog, optional sun shadow map.
in vec3 vWorldPos;
in vec3 vNormal;
in vec3 vColor;
in vec2 vUV;
in vec4 vLightPos;

uniform vec4 uAlbedo;      // material colour * tint, alpha
uniform vec3 uEmission;
uniform sampler2D uTex;
uniform bool uHasTex;
uniform bool uGlossy;
uniform bool uDoubleSided;
uniform bool uAlphaTest;   // cutout decals
uniform bool uOpaque;      // opaque pass: alpha = motion-blur mask
uniform float uBlurMask;   // 1 = world object, 0 = camera-attached (no blur)

uniform vec3 uCamPos;
uniform vec3 uCamUp;       // camera up vector (world space)
uniform vec3 uSunDir;      // direction light travels
uniform vec3 uSunColor;
uniform vec3 uAmbient;
uniform vec3 uFogColor;
uniform vec2 uFogRange;    // start, end

uniform sampler2DShadow uShadowMap;
uniform bool uShadowOn;
uniform float uShadowTexel;

out vec4 fragColor;

float band(float x, float edge)
{
    float w = max(fwidth(x), 0.015);
    return smoothstep(edge - w, edge + w, x);
}

float sunShadow(vec3 n, vec3 l)
{
    if (!uShadowOn) return 1.0;
    vec3 p = vLightPos.xyz / vLightPos.w * 0.5 + 0.5;
    if (p.x <= 0.0 || p.x >= 1.0 || p.y <= 0.0 || p.y >= 1.0 || p.z >= 1.0) return 1.0;
    float bias = 0.0008 + 0.0025 * (1.0 - clamp(dot(n, l), 0.0, 1.0));
    // 4 bilinear-filtered compares (hardware PCF) ~ a soft 3x3 kernel
    float s = 0.0;
    s += texture(uShadowMap, vec3(p.xy + vec2(-0.6, -0.6) * uShadowTexel, p.z - bias));
    s += texture(uShadowMap, vec3(p.xy + vec2( 0.6, -0.6) * uShadowTexel, p.z - bias));
    s += texture(uShadowMap, vec3(p.xy + vec2(-0.6,  0.6) * uShadowTexel, p.z - bias));
    s += texture(uShadowMap, vec3(p.xy + vec2( 0.6,  0.6) * uShadowTexel, p.z - bias));
    s *= 0.25;
    // fade out towards the edge of the shadow box
    vec2 e = abs(p.xy - 0.5) * 2.0;
    float fade = smoothstep(0.8, 1.0, max(e.x, e.y));
    return mix(s, 1.0, fade);
}

void main()
{
    vec4 albedo = vec4(vColor, 1.0) * uAlbedo;
    if (uHasTex) albedo *= texture(uTex, vUV);
    if (uAlphaTest)
    {
        if (albedo.a < 0.5) discard;
        albedo.a = uAlbedo.a;
    }

    vec3 n = normalize(vNormal);
    vec3 v = normalize(uCamPos - vWorldPos);
    if (uDoubleSided && !gl_FrontFacing) n = -n;
    vec3 l = normalize(-uSunDir);

    float ndl = dot(n, l);
    float shadow = sunShadow(n, l);
    float lit = min(ndl, mix(-0.2, 1.0, shadow));   // shadow pushes into the dark band

    // 3 tones: shade / mid / full, with soft (anti-aliased) edges
    float t1 = band(lit, 0.02);
    float t2 = band(lit, 0.55);
    float diffuse = 0.28 + 0.44 * t1 + 0.28 * t2;

    // hemisphere-ish ambient so the shaded side isn't flat
    vec3 ambient = uAmbient * (0.85 + 0.25 * n.y);
    vec3 light = ambient + uSunColor * diffuse * 0.85;

    vec3 col = albedo.rgb * light;

    // hard specular blob (glossy materials only)
    if (uGlossy)
    {
        vec3 h = normalize(l + v);
        float s = pow(max(dot(n, h), 0.0), 64.0) * t1;
        col += uSunColor * band(s, 0.5) * 0.55;
    }

    // rim light on the sun-facing side of silhouettes
    float rim = 1.0 - max(dot(n, v), 0.0);
    // only on silhouettes seen side-on: big flat surfaces viewed at a grazing
    // angle (the ground, roofs) have normals along the camera's up vector
    float side = 1.0 - abs(dot(n, uCamUp));
    float rimMask = band(rim * side, 0.6) * (0.35 + 0.65 * t1);
    col += (uSunColor * 0.6 + albedo.rgb * 0.4) * rimMask * 0.22;

    col += uEmission;

    float dist = length(uCamPos - vWorldPos);
    float fog = smoothstep(uFogRange.x, uFogRange.y, dist);
    col = mix(col, uFogColor, fog);
    fragColor = vec4(col, uOpaque ? uBlurMask : albedo.a);
}
