#version 460
// Muestreador de sombras: compara color.z con la profundidad guardada
layout (location = 0) in vec3 inColor;
layout (location = 0) out vec4 outColor;
layout (binding = 1) uniform sampler2DShadow tex;
void main() { outColor = vec4(texture(tex, inColor)); }
