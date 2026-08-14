#version 450
layout(set = 3, binding = 0, std140) uniform FragParams { vec4 color; } fp;
layout(location = 0) out vec4 outColor;
void main() { outColor = fp.color; }
