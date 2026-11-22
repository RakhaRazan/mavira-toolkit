#version 330 core

in vec2 vTexCoord;

uniform sampler2D uTexture;
uniform vec2 uTexelSize;
uniform vec2 uCursorUv;
uniform vec2 uQuadSizePx;
uniform float uLoupeRadiusPx;
uniform float uLoupeZoom;

out vec4 fragColor;

void main() {
    vec2 deltaPx = (vTexCoord - uCursorUv) * uQuadSizePx;
    float distPx = length(deltaPx);

    const float borderWidthPx = 2.0;
    if (distPx > uLoupeRadiusPx + borderWidthPx) {
        discard;
    }

    // Crisp anti-aliased lens border ring.
    if (distPx >= uLoupeRadiusPx - borderWidthPx) {
        float edgeAlpha = 1.0 - smoothstep(uLoupeRadiusPx, uLoupeRadiusPx + borderWidthPx, distPx);
        vec3 ringColor = mix(vec3(0.30, 0.55, 0.96), vec3(0.95, 0.97, 1.0), 0.35);
        fragColor = vec4(ringColor, edgeAlpha);
        return;
    }

    vec2 magUv = uCursorUv + (vTexCoord - uCursorUv) / max(uLoupeZoom, 1.0);
    if (magUv.x < 0.0 || magUv.x > 1.0 || magUv.y < 0.0 || magUv.y > 1.0) {
        fragColor = vec4(0.067, 0.075, 0.090, 1.0);
        return;
    }

    vec4 sampleCol = texture(uTexture, magUv);
    fragColor = sampleCol;
}
