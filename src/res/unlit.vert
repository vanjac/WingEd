#version 110

uniform mat4 uModelViewMatrix, uProjectionMatrix;

attribute vec3 aVertex;
attribute vec4 aColor;

varying vec4 vColor;

void main() {
    gl_Position = uProjectionMatrix * uModelViewMatrix * vec4(aVertex, 1);
    vColor = aColor;
}
