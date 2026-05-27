#version 450

layout(location = 0) in uint inPackedPos;
layout(location = 1) in uint inPackedUv;
layout(location = 2) in uint inPackedLight;
layout(location = 3) in uint inPackedMaterial;

layout(push_constant) uniform PushConstants {
    mat4 viewProjection;
    vec4 chunkOrigin;
} pc;

layout(location = 0) out vec3 outColor;

vec3 materialColor(uint materialId) {
    if (materialId == 2u) {
        return vec3(0.34, 0.41, 0.32);
    }
    if (materialId == 3u) {
        return vec3(0.43, 0.28, 0.18);
    }
    if (materialId == 99u) {
        return vec3(1.0, 0.84, 0.18);
    }
    return vec3(0.46, 0.48, 0.50);
}

void main() {
    uint x = inPackedPos & 63u;
    uint y = (inPackedPos >> 6u) & 63u;
    uint z = (inPackedPos >> 12u) & 63u;
    uint face = (inPackedPos >> 18u) & 7u;

    vec3 worldPos = pc.chunkOrigin.xyz + vec3(float(x), float(y), float(z));
    gl_Position = pc.viewProjection * vec4(worldPos, 1.0);

    float shade = clamp(float(inPackedLight & 255u) / 255.0, 0.2, 1.0);

    outColor = materialColor(inPackedMaterial) * shade;
}
