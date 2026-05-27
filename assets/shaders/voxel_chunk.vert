#version 450

layout(location = 0) in uint inPackedPos;
layout(location = 1) in uint inPackedUv;
layout(location = 2) in uint inPackedLight;
layout(location = 3) in uint inPackedMaterial;

layout(push_constant) uniform PushConstants {
    mat4 viewProjection;
    vec4 lightParams; // .xyz = sunDirection, .w = time
} pc;

layout(set = 0, binding = 0, std430) readonly buffer ChunkInstances {
    vec4 origins[];
} chunkInstances;

layout(location = 0) out vec3 fragWorldPos;
layout(location = 1) flat out uint fragFaceIndex;
layout(location = 2) flat out uint fragMaterialTypeId;
layout(location = 3) flat out uint fragPackedLight;
layout(location = 4) out vec2 fragLocalUv;

void main() {
    uint x = inPackedPos & 63u;
    uint y = (inPackedPos >> 6u) & 63u;
    uint z = (inPackedPos >> 12u) & 63u;
    uint face = (inPackedPos >> 18u) & 7u;
    uint corner = (inPackedPos >> 21u) & 7u;
    uint materialTypeId = inPackedMaterial >> 16u;

    vec3 chunkOrigin = chunkInstances.origins[gl_InstanceIndex].xyz;
    vec3 worldPos = chunkOrigin + vec3(float(x), float(y), float(z));
    gl_Position = pc.viewProjection * vec4(worldPos, 1.0);

    fragWorldPos = worldPos;
    fragFaceIndex = face;
    fragMaterialTypeId = materialTypeId;
    fragPackedLight = inPackedLight;
    fragLocalUv = vec2(float(corner & 1u), float((corner >> 1u) & 1u));
}
