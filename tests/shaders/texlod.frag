#version 460
// Nivel de mipmap explicito (color.z) y textureGrad
layout (location = 0) in vec3 inColor;
layout (location = 0) out vec4 outColor;
layout (binding = 0) uniform sampler2D tex;
void main() {
    if (inColor.z >= 0.0) outColor = textureLod(tex, inColor.xy, inColor.z);
    else outColor = textureGrad(tex, inColor.xy, vec2(-inColor.z, 0.0), vec2(0.0, -inColor.z));
}
