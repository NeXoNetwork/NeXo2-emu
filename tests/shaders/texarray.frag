#version 460
// Array de texturas 2D (capa = color.z) y textureSize / textureQueryLevels
layout (location = 0) in vec3 inColor;
layout (location = 0) out vec4 outColor;
layout (binding = 0) uniform sampler2DArray tex;
void main() {
    if (inColor.z < 0.0) outColor = vec4(vec3(textureSize(tex, 0)), float(textureQueryLevels(tex))) / 255.0;
    else outColor = texture(tex, inColor);
}
