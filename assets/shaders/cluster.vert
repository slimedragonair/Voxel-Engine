#version 450

// LOD2 cluster vertex shader.
//
// Mirror of voxel_chunk.vert but for the half-resolution ClusterVertex
// format (12 bytes, 8-bit-per-axis position, no UV/corner field — corner is
// recovered from gl_VertexIndex). Outputs match voxel.frag's inputs exactly
// so the same fragment shader handles both LOD0/1 chunks and LOD2 clusters
// — this is what keeps the visual seam at the LOD boundary clean.
//
// Vertex format (from `include/voxel/render/meshing/ClusterMesh.hpp`):
//   uint8 posX, posY, posZ, faceIndex     → location 0 (R8G8B8A8_UINT, 4B)
//   uint32 materialId                      → location 1 (R32_UINT,        4B)
//   uint32 packedLight                     → location 2 (R32_UINT,        4B)
//
// Position is in cluster-local BLOCK units (range 0..128 inclusive).
// World position = cluster origin (xyz, from the scene-entry SSBO) + posXYZ.

layout(location = 0) in uvec4 inPosAndFace;   // .xyz = pos in blocks, .w = face index
layout(location = 1) in uint  inMaterialId;
layout(location = 2) in uint  inPackedLight;

layout(push_constant) uniform PushConstants {
    mat4 viewProjection;
    vec4 lightParams; // .xyz = sunDirection, .w = time
} pc;

// Mirrors `voxel_chunk.vert`'s chunkInstances binding so the same GPU cull
// pass (voxel_cull.comp) can populate the origins for clusters too — it
// just writes ClusterSceneEntry.origin → uvec4 origins[].
layout(set = 0, binding = 0, std430) readonly buffer ClusterInstances {
    vec4 origins[];
} clusterInstances;

layout(location = 0) out vec3 fragWorldPos;
layout(location = 1) flat out uint fragFaceIndex;
layout(location = 2) flat out uint fragMaterialTypeId;
layout(location = 3) flat out uint fragPackedLight;
layout(location = 4) out vec2 fragLocalUv;

void main() {
    // Position in cluster-local block units.
    vec3 localPos = vec3(float(inPosAndFace.x),
                         float(inPosAndFace.y),
                         float(inPosAndFace.z));
    uint face = inPosAndFace.w;

    // Corner index 0..3 within the quad. ClusterMesher emits exactly 4
    // vertices per quad in a fixed 0-1-2-3 winding; gl_VertexIndex post-
    // index-lookup gives us the original vertex buffer position, and the
    // low two bits are the corner.
    uint corner = uint(gl_VertexIndex) & 3u;

    // LOD-tier scale lives in the previously-unused .w of the per-
    // instance origin. LOD2 sets it to 1.0 (vertex positions already
    // in block units); LOD3 sets it to 4.0 (vertex positions span 0..128
    // representing the 0..512-block region, so the shader scales them
    // up). Bigger LOD tiers extend the same pattern.
    vec4 originScale = clusterInstances.origins[gl_InstanceIndex];
    vec3 clusterOrigin = originScale.xyz;
    float lodScale = originScale.w;
    vec3 worldPos = clusterOrigin + localPos * lodScale;
    uint materialTypeId = inMaterialId >> 16u;
    gl_Position = pc.viewProjection * vec4(worldPos, 1.0);

    fragWorldPos = worldPos;
    fragFaceIndex = face;
    // Same packing convention as voxel_chunk.vert: high 16 bits = block
    // type id. Materials are looked up in voxel.frag by that type id, so
    // LOD2 clusters share the material atlas with LOD0/1 chunks.
    fragMaterialTypeId = materialTypeId;
    fragPackedLight = inPackedLight;
    fragLocalUv = vec2(float(corner & 1u), float((corner >> 1u) & 1u));
}
