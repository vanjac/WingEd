#version 140

in vec4 vColor;

out vec4 oFragColor;

void main() {
    ivec2 iCoord = ivec2(gl_FragCoord) & 3;
    if (iCoord != ivec2(0, 0) && iCoord != ivec2(2, 2))
        discard;
    oFragColor = vColor;
}
