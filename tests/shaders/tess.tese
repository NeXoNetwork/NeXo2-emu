#version 460
// Evaluacion de teselado: interpolacion lineal (el triangulo no cambia de forma)
layout (triangles, equal_spacing, ccw) in;
layout (location = 0) in vec3 inColor[];
layout (location = 0) out vec3 outColor;
void main() {
    vec3 t = gl_TessCoord;
    gl_Position = t.x * gl_in[0].gl_Position + t.y * gl_in[1].gl_Position + t.z * gl_in[2].gl_Position;
    outColor = t.x * inColor[0] + t.y * inColor[1] + t.z * inColor[2];
}
