#version 110

const float AMBIENT_INTENSITY = 0.4;
const float DIFFUSE_INTENSITY = 0.6;
const vec3 LIGHT_DIR = vec3(0, 0, 1);

void main (void)
{
    gl_Position = gl_ProjectionMatrix * gl_ModelViewMatrix * gl_Vertex;

    vec3 normal = gl_NormalMatrix * gl_Normal;
    float intensity = AMBIENT_INTENSITY + DIFFUSE_INTENSITY * max(0.0, dot(normal, LIGHT_DIR));
    gl_FrontColor = clamp(intensity * gl_Color, 0.0, 1.0);
}
