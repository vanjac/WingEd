#version 110

attribute vec3 aVertex;
attribute vec4 aColor;

uniform mat4 uModelViewMatrix, uProjectionMatrix;

void main() {
    gl_Position = uProjectionMatrix * uModelViewMatrix * vec4(aVertex, 1);
    gl_FrontColor = aColor;
}
