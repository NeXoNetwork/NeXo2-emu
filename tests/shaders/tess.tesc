#version 460
// Control de teselado: niveles fijos (exterior 4, 2, 3; interior 3) y puntos de paso
layout (vertices = 3) out;
layout (location = 0) in vec3 inColor[];
layout (location = 0) out vec3 outColor[];
void main() {
    if (gl_InvocationID == 0) {
        gl_TessLevelOuter[0] = 4.0;
        gl_TessLevelOuter[1] = 2.0;
        gl_TessLevelOuter[2] = 3.0;
        gl_TessLevelInner[0] = 3.0;
    }
    barrier();
    gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
    outColor[gl_InvocationID] = inColor[gl_InvocationID];
}
