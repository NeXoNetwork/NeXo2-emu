#version 460
// Descarta los pixeles con poco rojo: prueba KIL
layout (location = 0) in vec3 inColor;
layout (location = 0) out vec4 outColor;
void main() {
    if (inColor.r < 0.5) discard;
    outColor = vec4(inColor, 1.0);
}
