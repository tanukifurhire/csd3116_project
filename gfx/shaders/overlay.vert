#version 330 core
// 2D overlay (text, rects, HUD sprites, vector shapes), pixel coordinates
// with the origin top-left.
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec4 aColor;
layout(location = 2) in vec2 aUV;
uniform vec2 uViewport;
out vec4 vColor;
out vec2 vUV;
void main()
{
    vColor = aColor;
    vUV = aUV;
    gl_Position = vec4(aPos.x / uViewport.x * 2.0 - 1.0, 1.0 - aPos.y / uViewport.y * 2.0, 0.0, 1.0);
}
