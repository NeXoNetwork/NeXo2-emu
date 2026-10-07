#version 460
// Posicion por una matriz (uniform buffer) y color por un tinte
layout (location = 0) in vec3 inPos;
layout (location = 1) in vec3 inColor;
layout (location = 0) out vec3 outColor;
layout (std140, binding = 0) uniform Transform { mat4 mdlvMtx; vec4 tint; } u;
void main() {
    gl_Position = u.mdlvMtx * vec4(inPos, 1.0);
    outColor = inColor * u.tint.rgb;
}
