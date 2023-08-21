#version 110

const float AMBIENT_INTENSITY = 0.4;
const float DIFFUSE_INTENSITY = 0.6;
const vec3 LIGHT_DIR = vec3(0, 0, 1);

attribute vec3 aVertex;
attribute vec3 aNormal;
attribute vec4 aColor;

uniform mat4 uModelViewMatrix, uProjectionMatrix;
uniform mat3 uNormalMatrix;

void main (void)
{
    gl_Position = uProjectionMatrix * uModelViewMatrix * vec4(aVertex, 1);

    vec3 normal = uNormalMatrix * aNormal;
    float intensity = AMBIENT_INTENSITY + DIFFUSE_INTENSITY * max(0.0, dot(normal, LIGHT_DIR));
    gl_FrontColor = clamp(intensity * aColor, 0.0, 1.0);
}
