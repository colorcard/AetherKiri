#version 450
layout(set = 1, binding = 0, std140) uniform VertParams {
    vec2 dstOffset; vec2 dstSize; vec2 viewportSize;
    vec2 uvOffset; vec2 uvScale;
} vp;
layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec2 inTexCoord;
layout(location = 0) out vec2 fragSrcCoord;
layout(location = 1) out vec2 fragDstCoord;
void main() {
    fragSrcCoord = inTexCoord * vp.uvScale + vp.uvOffset;
    vec2 dstPos = inPosition * vp.dstSize + vp.dstOffset;
    // Destination-read shaders sample a region-sized scratch texture.
    fragDstCoord = inPosition;
    vec2 ndc = dstPos / vp.viewportSize * 2.0 - vec2(1.0);
    gl_Position = vec4(ndc.x, -ndc.y, 0.0, 1.0);
}
