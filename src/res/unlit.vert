#version 110

uniform mat4 uModelViewMatrix, uProjectionMatrix;

void main() {
    gl_Position = uProjectionMatrix * uModelViewMatrix * gl_Vertex;
    gl_FrontColor = gl_Color;
}
