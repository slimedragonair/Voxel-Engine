#pragma once

#include <cstdint>
#include <vector>

namespace voxel::data {
class BlockRegistry;
}

namespace voxel::render {

struct alignas(16) MaterialGpuData {
    float baseColor[4]{};
    float secondaryColor[4]{};
    float noiseParams[4]{};
    float textureParams[4]{};
};

static_assert(sizeof(MaterialGpuData) == 64, "MaterialGpuData must be 64 bytes for std430 alignment");

class MaterialTableBuilder {
public:
    static constexpr std::uint32_t kMaxMaterials = 256;

    [[nodiscard]] static std::vector<MaterialGpuData> build(const data::BlockRegistry& registry);
};

} // namespace voxel::render
