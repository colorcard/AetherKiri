#version 450
layout(set = 3, binding = 0, std140) uniform FragParams {
    uvec4 colorAndOpacity;
} fp;
layout(set = 2, binding = 0) uniform sampler2D dstTex;
layout(set = 2, binding = 1) uniform sampler2D opacityTables;
layout(location = 1) in vec2 fragDstCoord;
layout(location = 0) out vec4 outColor;

uint pack(uvec4 value) {
    return value.r | (value.g << 8) | (value.b << 16) | (value.a << 24);
}
vec4 unpack(uint value) {
    return vec4(value & 255u, (value >> 8) & 255u,
                (value >> 16) & 255u, value >> 24) / 255.0;
}

void main() {
    uvec4 d = uvec4(round(texture(dstTex, fragDstCoord) * 255.0));
    uint opacity = min(fp.colorAndOpacity.a, 255u);
    uvec2 table = uvec2(round(
        texelFetch(opacityTables, ivec2(d.a, opacity), 0).rg * 255.0));
    uint dw = pack(d);
    uint color = pack(uvec4(fp.colorAndOpacity.rgb, 0u));
    uint rb = dw & 0x00ff00ffu;
    rb = (rb + (((color & 0x00ff00ffu) - rb) * table.r >> 8)) &
         0x00ff00ffu;
    uint green = dw & 0x0000ff00u;
    uint sourceGreen = color & 0x0000ff00u;
    uint result = rb +
        ((green + ((sourceGreen - green) * table.r >> 8)) & 0x0000ff00u) +
        (table.g << 24);
    outColor = unpack(result);
}
