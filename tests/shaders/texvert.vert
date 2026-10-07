#version 460
// Textura leida en el shader de vertices (sin derivadas: nivel 0)
layout (location = 0) in vec3 inPos;
layout (location = 1) in vec3 inColor;
layout (location = 0) out vec3 outColor;
layout (binding = 0) uniform sampler2D tex;
void main() {
    gl_Position = vec4(inPos, 1.0);
    outColor = texture(tex, inColor.xy).rgb;
}
