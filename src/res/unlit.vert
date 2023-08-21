#version 150

uniform mat4 uModelViewMatrix, uProjectionMatrix;

in vec3 aVertex;
in vec4 aColor;

out vec4 vColor;

void main() {
    gl_Position = uProjectionMatrix * uModelViewMatrix * vec4(aVertex, 1);
    vColor = aColor;
}
