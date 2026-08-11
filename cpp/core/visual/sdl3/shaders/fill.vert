#version 450
layout(set = 1, binding = 0, std140) uniform VertParams {
    vec2 dstOffset; vec2 dstSize; vec2 viewportSize;
    vec2 uvOffset; vec2 uvScale;
} vp;
layout(location = 0) in vec2 inPosition;
void main() {
    vec2 ndc = (inPosition * vp.dstSize + vp.dstOffset) /
               vp.viewportSize * 2.0 - vec2(1.0);
    gl_Position = vec4(ndc.x, -ndc.y, 0.0, 1.0);
}
