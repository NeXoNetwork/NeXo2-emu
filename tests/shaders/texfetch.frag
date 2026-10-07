#version 460
// texelFetch: texel exacto (color.xy en texeles, nivel color.z) y con offset
layout (location = 0) in vec3 inColor;
layout (location = 0) out vec4 outColor;
layout (binding = 2) uniform sampler2D tex;
void main() {
    ivec2 p = ivec2(inColor.xy);
    if (inColor.z < 0.0) outColor = texelFetchOffset(tex, p, 0, ivec2(1, -1));
    else outColor = texelFetch(tex, p, int(inColor.z));
}
