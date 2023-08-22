#version 140

const float PATTERN_CONTRAST = 0.3;

in vec4 vColor;
in vec2 vTexCoord;

out vec4 oFragColor;

void main() {
    ivec2 i = ivec2(vTexCoord * 4.0) & 0xF;
    float pattern = float(i.x ^ i.y) / (16.0 / PATTERN_CONTRAST) + (1.0 - PATTERN_CONTRAST);
    oFragColor = vColor * vec4(vec3(pattern), 1);
}
