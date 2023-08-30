#version 140

in vec4 vColor;
in vec2 vTexCoord;

uniform sampler2D baseTexture;

out vec4 oFragColor;

void main() {
    oFragColor = vColor * texture2D(baseTexture, vTexCoord);
}
