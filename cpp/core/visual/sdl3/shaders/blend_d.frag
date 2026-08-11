#version 450
layout(set = 2, binding = 0) uniform sampler2D srcTex;
layout(set = 2, binding = 1) uniform sampler2D dstTex;
layout(set = 2, binding = 2) uniform sampler2D opacityTables;
layout(location = 0) in vec2 fragSrcCoord;
layout(location = 1) in vec2 fragDstCoord;
layout(location = 0) out vec4 outColor;

uvec4 bytes(sampler2D tex, vec2 uv) {
    return uvec4(round(texture(tex, uv) * 255.0));
}
uint pack(uvec4 value) {
    return value.r | (value.g << 8) | (value.b << 16) | (value.a << 24);
}
vec4 unpack(uint value) {
    return vec4(value & 255u, (value >> 8) & 255u,
                (value >> 16) & 255u, value >> 24) / 255.0;
}

void main() {
    uvec4 s = bytes(srcTex, fragSrcCoord);
    uvec4 d = bytes(dstTex, fragDstCoord);
    if (s.a == 0u) { outColor = vec4(d) / 255.0; return; }
    if (s.a == 255u || d.a == 0u) {
        outColor = vec4(s) / 255.0;
        return;
    }
    uvec2 table = uvec2(round(
        texelFetch(opacityTables, ivec2(d.a, s.a), 0).rg * 255.0));
    uint sw = pack(s);
    uint dw = pack(d);
    uint rb = dw & 0x00ff00ffu;
    rb = (rb + (((sw & 0x00ff00ffu) - rb) * table.r >> 8)) &
         0x00ff00ffu;
    uint green = dw & 0x0000ff00u;
    uint sourceGreen = sw & 0x0000ff00u;
    uint result = rb +
        ((green + ((sourceGreen - green) * table.r >> 8)) & 0x0000ff00u) +
        (table.g << 24);
    outColor = unpack(result);
}
