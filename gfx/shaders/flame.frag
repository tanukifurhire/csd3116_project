#version 330 core
// Cel-shaded flame layer. Output is premultiplied (blend ONE, ONE_MINUS_SRC_ALPHA):
// alpha = how much it hides what's behind, rgb is added.
in float vZ;
in float vFacing;
in vec3 vWorld;
uniform vec3 uColorBase;   // colour at the nozzle
uniform vec3 uColorTip;    // colour at the tip
uniform float uOpacity;    // 0 = purely additive, 1 = opaque
uniform float uGain;
uniform float uThreshold;  // toon edge of the heat band
uniform float uDiamonds;   // shock-diamond banding (core layer)
out vec4 fragColor;

float band(float x, float edge)
{
    float w = max(fwidth(x), 0.02);
    return smoothstep(edge - w, edge + w, x);
}

void main()
{
    // "heat": hottest at the nozzle and where we look through the most flame
    float heat = (1.0 - vZ) * (0.35 + 0.65 * sqrt(vFacing));
    float a = band(heat, uThreshold);
    if (a <= 0.0) discard;
    vec3 c = mix(uColorBase, uColorTip, smoothstep(0.45, 0.95, vZ));
    // second tone inside the band keeps the cel look
    c *= mix(0.85, 1.15, band(heat, uThreshold + 0.25));
    if (uDiamonds > 0.0)
        c *= 1.0 + uDiamonds * band(sin(vZ * 28.0) * 0.5 + 0.5, 0.7);
    fragColor = vec4(c * uGain * a, a * uOpacity);
}
