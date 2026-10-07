#version 460
// Textura 2D con el sampler (filtro y repeticion del TSC). Coordenadas = color.xy
layout (location = 0) in vec3 inColor;
layout (location = 0) out vec4 outColor;
layout (binding = 0) uniform sampler2D tex;
void main() { outColor = texture(tex, inColor.xy); }
