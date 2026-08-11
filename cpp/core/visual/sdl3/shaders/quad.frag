#version 450
layout(set = 2, binding = 0) uniform sampler2D tex0;
layout(location = 0) in vec2 fragSrcCoord;
layout(location = 0) out vec4 outColor;
void main() { outColor = texture(tex0, fragSrcCoord); }
