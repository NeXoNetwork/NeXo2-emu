#version 460
// Color uniforme del constbuf: prueba c[] en el shader de pixeles y la mezcla
layout (location = 0) out vec4 outColor;
layout (std140, binding = 0) uniform U { vec4 color; } u;
void main() { outColor = u.color; }
